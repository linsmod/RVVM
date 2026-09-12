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
 *  - Opaque handles (EGLDisplay/Config/Surface/Context) are host values the
 *    guest only passes back - never translated.
 *  - glVertexAttribPointer/glDrawElements carry the guest's
 *    GLSTUB_OFFSET_PTR_TAG for the client-array form; strip and translate.
 *  - glGetString/eglQueryString copy the host-owned string into the guest
 *    scratch buffer at args[GL_CALL_RETBUF_SLOT].
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

    g_h_egl  = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    g_h_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
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

    /* Keep in sync with the whitelist in tools/gen_gl_abi.py. */
    LOAD_EGL(GetError);
    LOAD_EGL(GetDisplay);
    LOAD_EGL(Initialize);
    LOAD_EGL(Terminate);
    LOAD_EGL(ChooseConfig);
    LOAD_EGL(GetConfigAttrib);
    LOAD_EGL(CreateWindowSurface);
    LOAD_EGL(CreatePbufferSurface);
    LOAD_EGL(DestroySurface);
    LOAD_EGL(CreateContext);
    LOAD_EGL(DestroyContext);
    LOAD_EGL(MakeCurrent);
    LOAD_EGL(SwapBuffers);
    LOAD_EGL(QuerySurface);
    LOAD_EGL(QueryString);
    /* eglGetProcAddress is answered in the guest stub; never loaded here. */

    LOAD_GL(ActiveTexture);
    LOAD_GL(AttachShader);
    LOAD_GL(BindAttribLocation);
    LOAD_GL(BindBuffer);
    LOAD_GL(BindFramebuffer);
    LOAD_GL(BindRenderbuffer);
    LOAD_GL(BindTexture);
    LOAD_GL(BlendColor);
    LOAD_GL(BlendEquation);
    LOAD_GL(BlendEquationSeparate);
    LOAD_GL(BlendFunc);
    LOAD_GL(BlendFuncSeparate);
    LOAD_GL(BufferData);
    LOAD_GL(BufferSubData);
    LOAD_GL(CheckFramebufferStatus);
    LOAD_GL(Clear);
    LOAD_GL(ClearColor);
    LOAD_GL(ClearDepthf);
    LOAD_GL(ClearStencil);
    LOAD_GL(ColorMask);
    LOAD_GL(CompileShader);
    LOAD_GL(CompressedTexImage2D);
    LOAD_GL(CompressedTexSubImage2D);
    LOAD_GL(CopyTexImage2D);
    LOAD_GL(CopyTexSubImage2D);
    LOAD_GL(CreateProgram);
    LOAD_GL(CreateShader);
    LOAD_GL(CullFace);
    LOAD_GL(DeleteBuffers);
    LOAD_GL(DeleteFramebuffers);
    LOAD_GL(DeleteProgram);
    LOAD_GL(DeleteRenderbuffers);
    LOAD_GL(DeleteShader);
    LOAD_GL(DeleteTextures);
    LOAD_GL(DepthFunc);
    LOAD_GL(DepthMask);
    LOAD_GL(DepthRangef);
    LOAD_GL(DetachShader);
    LOAD_GL(Disable);
    LOAD_GL(DisableVertexAttribArray);
    LOAD_GL(DrawArrays);
    LOAD_GL(DrawElements);
    LOAD_GL(Enable);
    LOAD_GL(EnableVertexAttribArray);
    LOAD_GL(Finish);
    LOAD_GL(Flush);
    LOAD_GL(FramebufferRenderbuffer);
    LOAD_GL(FramebufferTexture2D);
    LOAD_GL(FrontFace);
    LOAD_GL(GenBuffers);
    LOAD_GL(GenerateMipmap);
    LOAD_GL(GenFramebuffers);
    LOAD_GL(GenRenderbuffers);
    LOAD_GL(GenTextures);
    LOAD_GL(GetActiveAttrib);
    LOAD_GL(GetActiveUniform);
    LOAD_GL(GetAttachedShaders);
    LOAD_GL(GetAttribLocation);
    LOAD_GL(GetBooleanv);
    LOAD_GL(GetBufferParameteriv);
    LOAD_GL(GetError);
    LOAD_GL(GetFloatv);
    LOAD_GL(GetFramebufferAttachmentParameteriv);
    LOAD_GL(GetIntegerv);
    LOAD_GL(GetProgramiv);
    LOAD_GL(GetProgramInfoLog);
    LOAD_GL(GetRenderbufferParameteriv);
    LOAD_GL(GetShaderiv);
    LOAD_GL(GetShaderInfoLog);
    LOAD_GL(GetShaderPrecisionFormat);
    LOAD_GL(GetShaderSource);
    LOAD_GL(GetString);
    LOAD_GL(GetTexParameterfv);
    LOAD_GL(GetTexParameteriv);
    LOAD_GL(GetUniformfv);
    LOAD_GL(GetUniformiv);
    LOAD_GL(GetUniformLocation);
    LOAD_GL(GetVertexAttribfv);
    LOAD_GL(GetVertexAttribiv);
    LOAD_GL(GetVertexAttribPointerv);
    LOAD_GL(Hint);
    LOAD_GL(IsBuffer);
    LOAD_GL(IsEnabled);
    LOAD_GL(IsFramebuffer);
    LOAD_GL(IsProgram);
    LOAD_GL(IsRenderbuffer);
    LOAD_GL(IsShader);
    LOAD_GL(IsTexture);
    LOAD_GL(LineWidth);
    LOAD_GL(LinkProgram);
    LOAD_GL(PixelStorei);
    LOAD_GL(PolygonOffset);
    LOAD_GL(ReadPixels);
    LOAD_GL(ReleaseShaderCompiler);
    LOAD_GL(RenderbufferStorage);
    LOAD_GL(SampleCoverage);
    LOAD_GL(Scissor);
    LOAD_GL(ShaderBinary);
    LOAD_GL(ShaderSource);
    LOAD_GL(StencilFunc);
    LOAD_GL(StencilFuncSeparate);
    LOAD_GL(StencilMask);
    LOAD_GL(StencilMaskSeparate);
    LOAD_GL(StencilOp);
    LOAD_GL(StencilOpSeparate);
    LOAD_GL(TexImage2D);
    LOAD_GL(TexParameterf);
    LOAD_GL(TexParameterfv);
    LOAD_GL(TexParameteri);
    LOAD_GL(TexParameteriv);
    LOAD_GL(TexSubImage2D);
    LOAD_GL(Uniform1f);
    LOAD_GL(Uniform1fv);
    LOAD_GL(Uniform1i);
    LOAD_GL(Uniform1iv);
    LOAD_GL(Uniform2f);
    LOAD_GL(Uniform2fv);
    LOAD_GL(Uniform2i);
    LOAD_GL(Uniform2iv);
    LOAD_GL(Uniform3f);
    LOAD_GL(Uniform3fv);
    LOAD_GL(Uniform3i);
    LOAD_GL(Uniform3iv);
    LOAD_GL(Uniform4f);
    LOAD_GL(Uniform4fv);
    LOAD_GL(Uniform4i);
    LOAD_GL(Uniform4iv);
    LOAD_GL(UniformMatrix2fv);
    LOAD_GL(UniformMatrix3fv);
    LOAD_GL(UniformMatrix4fv);
    LOAD_GL(UseProgram);
    LOAD_GL(ValidateProgram);
    LOAD_GL(VertexAttrib1f);
    LOAD_GL(VertexAttrib1fv);
    LOAD_GL(VertexAttrib2f);
    LOAD_GL(VertexAttrib2fv);
    LOAD_GL(VertexAttrib3f);
    LOAD_GL(VertexAttrib3fv);
    LOAD_GL(VertexAttrib4f);
    LOAD_GL(VertexAttrib4fv);
    LOAD_GL(VertexAttribPointer);
    LOAD_GL(Viewport);

    #undef LOAD_EGL
    #undef LOAD_GL

    g_loaded = true;
    LOGI("system GL backend loaded (libEGL.so + libGLESv2.so)");
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

/* The guest stub tags the client-array form of the overloaded
 * glVertexAttribPointer/glDrawElements pointer; bare values are VBO offsets. */
#define VPGL_OFFSET_PTR_TAG ((int64_t)1 << 62)

static void* vpgl_gptr_or_off(int64_t v)
{
    if (v & VPGL_OFFSET_PTR_TAG) {
        return vpgl_gptr(v & ~VPGL_OFFSET_PTR_TAG);
    }
    return (void*)(uintptr_t)v;
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
