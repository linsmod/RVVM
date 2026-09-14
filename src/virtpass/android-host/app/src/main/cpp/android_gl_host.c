/*
 * android_gl_host.c - system EGL/GLES backend for the Android host
 *
 * Registers the marshalled GL/EGL dispatch (SYS_EGL_CALL / SYS_GL_CALL) with
 * vp_cmdpost and services it against the SYSTEM EGL/GLES. Layout mirrors
 * win32_gl_dispatch.c on purpose: the generated vp_gl_dispatch_tables.h is
 * included by both, so the argument-translation rules exist exactly once.
 *
 * Pointer translation (identical rules to win32):
 *  - gl_call.args[] carries guest virtual addresses; every data pointer goes
 *    through rvvm_user_guest_ptr() before the system GL dereferences it.
 *  - Opaque handles (EGLDisplay/Config/Surface/Context, GLsync) are host
 *    values the guest only passes back - never translated.
 *  - glVertexAttribPointer/glVertexAttribIPointer/glDrawElements/
 *    glDrawElementsInstanced/glDrawRangeElements carry either a client-array
 *    address or a buffer byte offset; vpgl_ptr() picks the form from the
 *    bound buffer object (see win32_gl_dispatch.c).
 *  - glGetString/glGetStringi/eglQueryString copy the host-owned string into
 *    the guest scratch buffer at args[GL_CALL_RETBUF_SLOT].
 *
 * The one behavioural difference from win32: eglCreateWindowSurface binds the
 * real SurfaceView window and eglSwapBuffers presents natively - no pbuffer,
 * no DIB, no blit.
 */

#include "android_gl_host.h"

#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>  /* getenv: RVVM_GL_TRACE */
#include <string.h>

#include "core/rvvm_user.h"   /* rvvm_user_guest_ptr */
#include "virtpass/vp_cmdpost.h" /* cmdpost_set_gl_callbacks */
#include "virtpass/vp_gl.h"   /* fn_id macros + gl_call ABI */

#include "virtpass/vp_gl_host_types.h"   /* vpgl_ types + extern p_* */
#include "virtpass/vp_gl_host_entries.h" /* p_* storage (this is the one TU) */

/* on_egl_dispatch / on_gl_dispatch are defined below; cmdpost_set_gl_callbacks
 * in android_gl_host_init() takes their addresses. */
void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret);
void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret);

#define LOG_TAG "RVVM-GL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ============================================================
 * System backend loading
 * ============================================================ */

static void* g_h_egl  = NULL;
static void* g_h_gles = NULL;
static const char* g_gl_lib = NULL; /* which soname g_h_gles came from */
static bool  g_loaded = false;

bool android_gl_host_init(void)
{
    if (g_loaded) {
        /* Libraries stay loaded for the process lifetime; only the cmdpost
         * callbacks need reinstalling (cmdpost_cleanup NULLs them on guest
         * exit). */
        cmdpost_set_gl_callbacks(on_egl_dispatch, on_gl_dispatch);
        return true;
    }

    /* The GLES3 entry points live in libGLESv3.so; on devices that only ship
     * the v2 soname the same library is reachable through it, so try v3 first
     * and fall back rather than losing the ES3 surface. */
    g_h_egl  = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    g_h_gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
    if (g_h_gles) {
        g_gl_lib = "libGLESv3.so";
    } else {
        g_h_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        g_gl_lib = "libGLESv2.so";
    }
    if (!g_h_egl || !g_h_gles) {
        LOGE("dlopen system GL failed: egl=%p gles=%p (%s)",
             g_h_egl, g_h_gles, dlerror());
        if (g_h_egl)  { dlclose(g_h_egl);  g_h_egl = NULL; }
        if (g_h_gles) { dlclose(g_h_gles); g_h_gles = NULL; }
        return false;
    }

    #define LOAD_EGL(name)                                                     \
        do {                                                                   \
            p_egl##name = (vpgl_PFN_egl##name)dlsym(g_h_egl, "egl" #name);     \
            if (!p_egl##name) {                                                \
                LOGE("dlsym egl%s failed", #name);                             \
                goto fail;                                                     \
            }                                                                  \
        } while (0)

    #define LOAD_GL(name)                                                      \
        do {                                                                   \
            p_gl##name = (vpgl_PFN_gl##name)dlsym(g_h_gles, "gl" #name);       \
            if (!p_gl##name) {                                                 \
                LOGE("dlsym gl%s failed", #name);                              \
                goto fail;                                                     \
            }                                                                  \
        } while (0)

    /* The names come from the generated vp_gl_host_entries.h, so the loader
     * cannot fall behind the ABI. eglGetProcAddress is answered in the guest
     * stub and is not in the list. */
    VPGL_EGL_ENTRY_LIST(LOAD_EGL)
    VPGL_GL_ENTRY_LIST(LOAD_GL)

    #undef LOAD_EGL
    #undef LOAD_GL

    g_loaded = true;
    LOGI("system GL backend loaded (libEGL.so + %s)", g_gl_lib);
    cmdpost_set_gl_callbacks(on_egl_dispatch, on_gl_dispatch);
    return true;

fail:
    LOGE("system GL backend incomplete; guests fall back to CPU rendering");
    dlclose(g_h_egl);  g_h_egl = NULL;
    dlclose(g_h_gles); g_h_gles = NULL;
    return false;
}

/* ============================================================
 * Pointer translation - same rules as win32_gl_dispatch.c
 * ============================================================ */

static float vpgl_arg_f(int64_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* Guest address -> host pointer. NULL stays NULL. */
static void* vpgl_gptr(int64_t v)
{
    return v ? rvvm_user_guest_ptr((uint64_t)v) : NULL;
}

/* A marshalled call whose entry point was never resolved. The loader below
 * fails hard on that, so reaching this means a p_* the loader list does not
 * know about; without the report the call would be an invisible no-op. Capped,
 * because a render loop can hit the same call thousands of times per frame. */
static void vpgl_missing(const char* name)
{
    static int reported;
    if (reported >= 32) return;
    reported++;
    LOGE("%s not resolved by the host GL backend (call dropped)", name);
}

/* glVertexAttribPointer/IPointer and glDrawElements/Instanced/glDrawRangeElements
 * take either a client-array address or a buffer byte offset; GL decides by the
 * bound buffer object, so resolve it from live GL state (see win32 for the full
 * rationale). */
enum { VPGL_PTR_ARRAY, VPGL_PTR_ELEMENT };

static void* vpgl_ptr(int64_t v, int kind)
{
    vpgl_GLint bound = 0;
    const vpgl_GLenum target = (kind == VPGL_PTR_ARRAY)
                                   ? (vpgl_GLenum)GL_ARRAY_BUFFER_BINDING
                                   : (vpgl_GLenum)GL_ELEMENT_ARRAY_BUFFER_BINDING;
    if (p_glGetIntegerv) p_glGetIntegerv(target, &bound);
    if (bound) {
        return (void*)(uintptr_t)v;
    }
    return vpgl_gptr(v);
}

/* glShaderSource's array of `count` guest string pointers. */
#define VPGL_MAX_SHADER_SRCS 64
static const vpgl_GLchar* vpgl_shader_srcs[VPGL_MAX_SHADER_SRCS];

static const vpgl_GLchar** vpgl_translate_shader_srcs(const int64_t* a, int count)
{
    if (!a[2]) return NULL;
    const int64_t* srcs = (const int64_t*)rvvm_user_guest_ptr((uint64_t)a[2]);
    if (!srcs) return NULL;
    if (count < 0) count = 0;
    if (count > VPGL_MAX_SHADER_SRCS) count = VPGL_MAX_SHADER_SRCS;
    for (int i = 0; i < count; i++) {
        vpgl_shader_srcs[i] = (const vpgl_GLchar*)vpgl_gptr(srcs[i]);
    }
    return vpgl_shader_srcs;
}

/* glGetString/eglQueryString: land the host-owned string in the guest's
 * scratch buffer and answer with that guest address. */
static int64_t vpgl_string_out(const int64_t* a, const char* str)
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

/* ============================================================
 * Dispatch
 * ============================================================ */

/* Name tables + generic switches, shared with the win32 host. Defines static
 * functions: include exactly once, after the helpers above. */
#include "virtpass/vp_gl_dispatch_tables.h"

static bool g_gl_active = false;
static struct ANativeWindow* g_window = NULL; /* set via android_gl_set_native_window */

void android_gl_set_native_window(struct ANativeWindow* window)
{
    g_window = window;
    LOGI("GL window %s", window ? "attached" : "detached");
}

/* Call trace, logcat flavour of the win32 RVVM_GL_TRACE helper. */
static bool gl_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("RVVM_GL_TRACE");
        enabled = (v && strcmp(v, "off") != 0) ? 1 : 0;
    }
    return enabled == 1;
}

static void gl_trace(const char* kind, uint32_t fn_id, const int64_t* a, int64_t ret)
{
    if (!gl_trace_enabled()) return;
    const char* name = (kind[0] == 'g') ? vpgl_gl_name(fn_id) : vpgl_egl_name(fn_id);
    LOGI("[gl] %s %-28s ret=%lld", kind, name ? name : "?", (long long)ret);
}

void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    *ret = 0;
    switch (fn_id) {
    case EGL_FN_GETDISPLAY:
        *ret = (int64_t)(intptr_t)p_eglGetDisplay((vpgl_void*)0);
        break;
    case EGL_FN_CREATEWINDOWSURFACE: {
        if (!g_gl_active) { g_gl_active = true; LOGI("GL mode active"); }
        /* Bind the REAL SurfaceView window: the Android host owns one, so
         * unlike the win32 host there is no pbuffer downgrade here and
         * eglSwapBuffers presents through SurfaceFlinger directly. The
         * attribute list is a guest pointer like every data argument. */
        if (!g_window) {
            /* Surface not up yet. Fail the call (the guest sees
             * EGL_NO_SURFACE, exactly like a real EGL would) rather than
             * silently diverting to a pbuffer nothing would ever show. */
            LOGW("eglCreateWindowSurface with no window attached");
            *ret = 0;
            break;
        }
        /* The EGL surface sizes its buffers from the window's geometry, and
         * the window is the floating card's viewport, not the panel the guest
         * renders. Push the pinned panel size first, or the guest's
         * glViewport(0, 0, panelW, panelH) would map onto the bottom-left
         * corner of a viewport-sized surface. */
        {
            extern void jni_apply_surface_geometry(struct ANativeWindow* w);
            jni_apply_surface_geometry(g_window);
        }
        *ret = (int64_t)(intptr_t)p_eglCreateWindowSurface(
            (vpgl_EGLDisplay)(uintptr_t)args[0],
            (vpgl_EGLConfig)(uintptr_t)args[1],
            (vpgl_EGLNativeWindowType)(void*)g_window,
            (const vpgl_EGLint*)vpgl_gptr(args[3]));
        break;
    }
    case EGL_FN_SWAPBUFFERS: {
        /* Real present: the system compositor takes it from here. */
        *ret = (int64_t)p_eglSwapBuffers(
            (vpgl_EGLDisplay)(uintptr_t)args[0],
            (vpgl_EGLSurface)(uintptr_t)args[1]);
        /* The GL present path: a successful swap means a frame is on the
         * surface, the same cue the CPU unlock path gives the UI. */
        if (*ret == 1) {
            extern void jni_guest_first_frame(void);
            jni_guest_first_frame();
        }
        break;
    }
    default:
        vpgl_dispatch_egl_generic(fn_id, args, ret);
        break;
    }
    gl_trace("egl", fn_id, args, *ret);
}

void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    *ret = 0;
    vpgl_dispatch_gl_generic(fn_id, args, ret);
    gl_trace("gl", fn_id, args, *ret);
}
