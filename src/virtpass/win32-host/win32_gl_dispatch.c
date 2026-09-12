#include "win32_gl_backend.h"
#include "win32_gl_dispatch.h" /* on_*_dispatch, g_gl_inflight */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rvvm_user.h" /* rvvm_user_guest_ptr */

static float w32gl_arg_f(int64_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* ============================================================
 * Guest <-> host pointer translation
 *
 * gl_call.args[] carries guest virtual addresses (see the gl_call ABI notes).
 * The guest runs in its own private memory (rvvm_user machine), so every
 * data pointer must be translated before the host GL implementation touches
 * it. Opaque handles (EGLDisplay/... and the EGLNative* types) are host
 * values the guest only passes back - they are never translated.
 * ============================================================ */

/* Guest address -> host pointer. NULL stays NULL (the guest's NULL page is
 * unmapped, and GL treats NULL as "no data"). */
static void* w32gl_gptr(int64_t v)
{
    return v ? rvvm_user_guest_ptr((uint64_t)v) : NULL;
}

/* glVertexAttribPointer/glDrawElements overload their pointer argument: it is
 * a guest address for a client-side array, but a byte offset into the bound
 * buffer object for VBO rendering. Offsets are not mappable guest addresses,
 * so fall back to the raw value in that case. */
static void* w32gl_gptr_or_off(int64_t v)
{
    void* p = w32gl_gptr(v);
    return p ? p : (void*)(uintptr_t)v;
}

/* glShaderSource passes `count` guest string pointers. Build the array of
 * host pointers the real implementation expects. Bounded scratch: GLES2
 * shaders never need more chunks than this, and overflowing would only
 * truncate the source, not corrupt memory. */
#define W32GL_MAX_SHADER_SRCS 64
static const w32gl_GLchar* w32gl_shader_srcs[W32GL_MAX_SHADER_SRCS];

static const w32gl_GLchar** w32gl_translate_shader_srcs(const int64_t* a, int count)
{
    if (!a[2]) return NULL;
    const int64_t* srcs = (const int64_t*)rvvm_user_guest_ptr((uint64_t)a[2]);
    if (!srcs) return NULL;
    if (count < 0) count = 0;
    if (count > W32GL_MAX_SHADER_SRCS) count = W32GL_MAX_SHADER_SRCS;
    for (int i = 0; i < count; i++) {
        w32gl_shader_srcs[i] = (const w32gl_GLchar*)w32gl_gptr(srcs[i]);
    }
    return w32gl_shader_srcs;
}

/* Pointer-returning calls (glGetString/eglQueryString) hand out a string
 * owned by the GL DLL, which the guest cannot read. Copy it into the guest
 * scratch buffer the stub offered in args[GL_CALL_RETBUF_SLOT] and answer
 * with that guest address instead. */
static int64_t w32gl_string_out(const int64_t* a, const char* str)
{
    if (!str) return 0;
    uint64_t ga = (uint64_t)a[GL_CALL_RETBUF_SLOT];
    /* Refuse to write unless the whole landing zone is mapped, so a bogus
     * slot value cannot run off the end of guest RAM. */
    if (!ga || !rvvm_user_guest_ptr(ga) ||
        !rvvm_user_guest_ptr(ga + GL_CALL_RETBUF_CAP - 1)) {
        fprintf(stderr, "[gl] retbuf slot %llx unmapped; dropping string\n",
                (unsigned long long)ga);
        return 0;
    }
    char* dst = (char*)rvvm_user_guest_ptr(ga);
    size_t len = strlen(str);
    if (len > GL_CALL_RETBUF_CAP - 1) len = GL_CALL_RETBUF_CAP - 1;
    memcpy(dst, str, len);
    dst[len] = '\0';
    return (int64_t)ga;
}

#include "win32_gl_dispatch_tables.h"
#include "win32_cmdpost_bridge.h" /* present_gl_frame */

bool g_gl_active = false;

/* Trace every marshalled call. The guest smashes its stack as soon as ANGLE
 * touches it, so the last line printed before the fault is the call that
 * introduced the bad pointer. */
static bool gl_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("RVVM_GL_TRACE");
        enabled = (v && strcmp(v, "off") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

/* Print the argument list before dispatching. A call that faults inside the
 * GL implementation never reaches the post-call trace, so this is the only
 * record of what it was handed. */
static void gl_trace_args(const char* kind, uint32_t fn_id, const int64_t* a)
{
    if (!gl_trace_enabled() || !a) return;
    const char* name = (kind[0] == 'g') ? w32gl_gl_name(fn_id) : w32gl_egl_name(fn_id);
    printf("[gl] %s %-28s args=[", kind, name ? name : "?");
    for (int i = 0; i < GL_CALL_MAX_ARGS; i++) printf(" %llx", (unsigned long long)a[i]);
    printf(" ]\n");
    fflush(stdout);
}

static void gl_trace(const char* kind, uint32_t fn_id, const int64_t* a, int64_t ret)
{
    g_gl_inflight = NULL;
    if (!gl_trace_enabled()) return;
    const char* name = (kind[0] == 'g') ? w32gl_gl_name(fn_id) : w32gl_egl_name(fn_id);
    printf("[gl] %s %-28s ret=%lld", kind, name ? name : "?", (long long)ret);
    if (a) {
        printf(" [");
        for (int i = 0; i < GL_CALL_MAX_ARGS; i++) printf(" %llx", (unsigned long long)a[i]);
        printf(" ]");
    }
    printf("\n");
    fflush(stdout);
}

/* ============================================================
 * EGL dispatch: 3 special cases before the generic table
 * ============================================================ */

void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    *ret = 0;
    g_gl_inflight = w32gl_egl_name(fn_id);
    gl_trace_args("egl", fn_id, args);
    if (gl_trace_enabled()) fflush(stdout);
    switch (fn_id) {
    case EGL_FN_GETDISPLAY:
        *ret = (int64_t)(intptr_t)p_eglGetDisplay((w32gl_void*)0);
        break;
    case EGL_FN_CREATEWINDOWSURFACE: {
        if (!g_gl_active) { g_gl_active = true; printf("[winhost] GL mode active\n"); }
        /* The win32 host has no native window to render into: back the
         * guest's window surface with an offscreen pbuffer instead. The
         * native win handle (args[2]) is dropped, the attribute list is
         * args[3] and is a guest pointer like every other data argument. */
        *ret = (int64_t)(intptr_t)p_eglCreatePbufferSurface(
            (w32gl_EGLDisplay)(uintptr_t)args[0],
            (w32gl_EGLConfig)(uintptr_t)args[1],
            (const w32gl_EGLint*)w32gl_gptr(args[3]));
        break;
    }
    case EGL_FN_SWAPBUFFERS:
        present_gl_frame();
        *ret = 1;
        break;
    default:
        w32gl_dispatch_egl_generic(fn_id, args, ret);
        break;
    }
    gl_trace("egl", fn_id, args, *ret);
}

/* ============================================================
 * GLES dispatch: all fn_id 1-142 handled by generic table.
 * Keep hook points for future specialization.
 * ============================================================ */

void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    *ret = 0;
    g_gl_inflight = w32gl_gl_name(fn_id);
    gl_trace_args("gl", fn_id, args);
    switch (fn_id) {
    default:
        w32gl_dispatch_gl_generic(fn_id, args, ret);
        break;
    }
    gl_trace("gl", fn_id, args, *ret);
}
