#include "win32_gl_backend.h"

#include <stdio.h>
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
 * scratch buffer offered in args[GL_CALL_RETBUF_SLOT] and answer with that
 * guest address instead. */
static int64_t w32gl_string_out(const int64_t* a, const char* str)
{
    if (!str) return 0;
    char* dst = (char*)rvvm_user_guest_ptr((uint64_t)a[GL_CALL_RETBUF_SLOT]);
    if (!dst) return 0;
    size_t len = strlen(str);
    if (len > GL_CALL_RETBUF_CAP - 1) len = GL_CALL_RETBUF_CAP - 1;
    memcpy(dst, str, len);
    dst[len] = '\0';
    return a[GL_CALL_RETBUF_SLOT];
}

#include "win32_gl_dispatch_tables.h"
#include "win32_cmdpost_bridge.h" /* present_gl_frame */

bool g_gl_active = false;

/* ============================================================
 * EGL dispatch: 3 special cases before the generic table
 * ============================================================ */

void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
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
}

/* ============================================================
 * GLES dispatch: all fn_id 1-142 handled by generic table.
 * Keep hook points for future specialization.
 * ============================================================ */

void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    switch (fn_id) {
    default:
        w32gl_dispatch_gl_generic(fn_id, args, ret);
        break;
    }
}
