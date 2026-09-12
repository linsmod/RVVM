/*
 * GENERATED FILE - produced by tools/gen_gl_abi.py - DO NOT EDIT BY HAND.
 *
 * Source of truth: NDK sysroot headers GLES2/gl2.h + EGL/egl.h (parsed).
 * Regenerate with:  python tools/gen_gl_abi.py
 *
 * Phase 3 ABI notes:
 *  - fn_id macros are the single source of truth shared by the guest stubs,
 *    src/virtpass/vp_cmdpost.c and the win32 host GL dispatch.
 *  - gl_call.args has 9 slots (glCompressedTexSubImage2D needs 9; the
 *    original Phase 3 plan said 6 - widened before first deployment, so this
 *    is an internal ABI change with zero consumers).
 *  - Floats travel bit-packed through the int64_t slots; pointers travel as
 *    guest virtual addresses. Guest memory is NOT mapped into the host, so
 *    the host dispatch translates every data pointer argument with
 *    rvvm_user_guest_ptr() and, for calls that hand back a host-owned string
 *    (glGetString/eglQueryString), copies it through the guest scratch
 *    buffer offered in args[GL_CALL_RETBUF_SLOT].
 */

/*
 * vp_gl_stub.c - Guest-side EGL/GLES stubs for statically-linked riscv64
 * ELFs. Every call is marshalled through one hypercall:
 *
 *   SYS_GL_CALL   -> gl_call* in a0 (GLES functions)
 *   SYS_EGL_CALL  -> gl_call* in a0 (EGL functions)
 *
 * The host (win32_gl_dispatch.c on Windows, jni_bridge on Android) executes
 * the real EGL/GLES call and writes the return value back into _c.ret.
 */
#include <string.h>
#include <stdint.h>
#include "virtpass/vp_gl.h"

/* Same ecall trampoline as vp_ndk_stub.c */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2,
                                   long a3, long a4, long a5)
{
    register long t0 __asm__("a7") = nr;
    register long t1 __asm__("a0") = a0;
    register long t2 __asm__("a1") = a1;
    register long t3 __asm__("a2") = a2;
    register long t4 __asm__("a3") = a3;
    register long t5 __asm__("a4") = a4;
    register long t6 __asm__("a5") = a5;

    __asm__ __volatile__(
        "ecall"
        : "+r"(t1)
        : "r"(t2), "r"(t3), "r"(t4), "r"(t5), "r"(t6), "r"(t0)
        : "memory"
    );
    return t1;
}

/* Float <-> int64 bit packing (GLES2 has no double params).
 * Only packing is needed guest-side: no GLES2 core function returns a float,
 * unpacking happens host-side via vpgl_arg_f(). */
static inline int64_t glstub_packf(float v)
{
    union { float f; uint32_t u; } cvt;
    cvt.f = v;
    return (int64_t)cvt.u;
}

#define GLSTUB_CALL(id, n)                        \
    gl_call _c;                                   \
    memset(&_c, 0, sizeof(_c));                   \
    _c.fn_id = (uint32_t)(id);                    \
    _c.nargs = (uint32_t)(n)

#define GLSTUB_DO(nr)                             \
    virtpass_syscall((nr), (long)(uintptr_t)&_c, 0, 0, 0, 0, 0)

/* Scratch buffer handed to the host by calls returning a host-owned string
 * (glGetString/eglQueryString). File scope, not a gl_call member: the host
 * answers with this guest address and the caller reads it after the call, so
 * it must outlive the stub's own stack frame. */
static char glstub_retbuf[GL_CALL_RETBUF_CAP];

/* glVertexAttribPointer/glDrawElements accept either a guest address (client
 * array) or a byte offset into the bound buffer object, and the host cannot
 * tell the two apart from the value alone. The stub therefore marks the
 * address form by setting the top bit, which no real offset or guest address
 * uses; the host clears it after translating. An offset passes through
 * unmarked and reaches the GL implementation unchanged. */
#define GLSTUB_OFFSET_PTR_TAG  ((int64_t)1 << 62)
#define GLSTUB_OFFSET_PTR(p)                                    \
    (((p) && (uintptr_t)(p) < GLSTUB_OFFSET_PTR_TAG)            \
         ? ((int64_t)(uintptr_t)(p) | GLSTUB_OFFSET_PTR_TAG)    \
         : (int64_t)(uintptr_t)(p))

typedef void (*glstub_proc_t)(void);

typedef struct { const char* name; glstub_proc_t proc; } glstub_proc_entry;

static const glstub_proc_entry glstub_procs[] = {
    { "glActiveTexture", (glstub_proc_t)&glActiveTexture },
    { "glAttachShader", (glstub_proc_t)&glAttachShader },
    { "glBindAttribLocation", (glstub_proc_t)&glBindAttribLocation },
    { "glBindBuffer", (glstub_proc_t)&glBindBuffer },
    { "glBindFramebuffer", (glstub_proc_t)&glBindFramebuffer },
    { "glBindRenderbuffer", (glstub_proc_t)&glBindRenderbuffer },
    { "glBindTexture", (glstub_proc_t)&glBindTexture },
    { "glBlendColor", (glstub_proc_t)&glBlendColor },
    { "glBlendEquation", (glstub_proc_t)&glBlendEquation },
    { "glBlendEquationSeparate", (glstub_proc_t)&glBlendEquationSeparate },
    { "glBlendFunc", (glstub_proc_t)&glBlendFunc },
    { "glBlendFuncSeparate", (glstub_proc_t)&glBlendFuncSeparate },
    { "glBufferData", (glstub_proc_t)&glBufferData },
    { "glBufferSubData", (glstub_proc_t)&glBufferSubData },
    { "glCheckFramebufferStatus", (glstub_proc_t)&glCheckFramebufferStatus },
    { "glClear", (glstub_proc_t)&glClear },
    { "glClearColor", (glstub_proc_t)&glClearColor },
    { "glClearDepthf", (glstub_proc_t)&glClearDepthf },
    { "glClearStencil", (glstub_proc_t)&glClearStencil },
    { "glColorMask", (glstub_proc_t)&glColorMask },
    { "glCompileShader", (glstub_proc_t)&glCompileShader },
    { "glCompressedTexImage2D", (glstub_proc_t)&glCompressedTexImage2D },
    { "glCompressedTexSubImage2D", (glstub_proc_t)&glCompressedTexSubImage2D },
    { "glCopyTexImage2D", (glstub_proc_t)&glCopyTexImage2D },
    { "glCopyTexSubImage2D", (glstub_proc_t)&glCopyTexSubImage2D },
    { "glCreateProgram", (glstub_proc_t)&glCreateProgram },
    { "glCreateShader", (glstub_proc_t)&glCreateShader },
    { "glCullFace", (glstub_proc_t)&glCullFace },
    { "glDeleteBuffers", (glstub_proc_t)&glDeleteBuffers },
    { "glDeleteFramebuffers", (glstub_proc_t)&glDeleteFramebuffers },
    { "glDeleteProgram", (glstub_proc_t)&glDeleteProgram },
    { "glDeleteRenderbuffers", (glstub_proc_t)&glDeleteRenderbuffers },
    { "glDeleteShader", (glstub_proc_t)&glDeleteShader },
    { "glDeleteTextures", (glstub_proc_t)&glDeleteTextures },
    { "glDepthFunc", (glstub_proc_t)&glDepthFunc },
    { "glDepthMask", (glstub_proc_t)&glDepthMask },
    { "glDepthRangef", (glstub_proc_t)&glDepthRangef },
    { "glDetachShader", (glstub_proc_t)&glDetachShader },
    { "glDisable", (glstub_proc_t)&glDisable },
    { "glDisableVertexAttribArray", (glstub_proc_t)&glDisableVertexAttribArray },
    { "glDrawArrays", (glstub_proc_t)&glDrawArrays },
    { "glDrawElements", (glstub_proc_t)&glDrawElements },
    { "glEnable", (glstub_proc_t)&glEnable },
    { "glEnableVertexAttribArray", (glstub_proc_t)&glEnableVertexAttribArray },
    { "glFinish", (glstub_proc_t)&glFinish },
    { "glFlush", (glstub_proc_t)&glFlush },
    { "glFramebufferRenderbuffer", (glstub_proc_t)&glFramebufferRenderbuffer },
    { "glFramebufferTexture2D", (glstub_proc_t)&glFramebufferTexture2D },
    { "glFrontFace", (glstub_proc_t)&glFrontFace },
    { "glGenBuffers", (glstub_proc_t)&glGenBuffers },
    { "glGenerateMipmap", (glstub_proc_t)&glGenerateMipmap },
    { "glGenFramebuffers", (glstub_proc_t)&glGenFramebuffers },
    { "glGenRenderbuffers", (glstub_proc_t)&glGenRenderbuffers },
    { "glGenTextures", (glstub_proc_t)&glGenTextures },
    { "glGetActiveAttrib", (glstub_proc_t)&glGetActiveAttrib },
    { "glGetActiveUniform", (glstub_proc_t)&glGetActiveUniform },
    { "glGetAttachedShaders", (glstub_proc_t)&glGetAttachedShaders },
    { "glGetAttribLocation", (glstub_proc_t)&glGetAttribLocation },
    { "glGetBooleanv", (glstub_proc_t)&glGetBooleanv },
    { "glGetBufferParameteriv", (glstub_proc_t)&glGetBufferParameteriv },
    { "glGetError", (glstub_proc_t)&glGetError },
    { "glGetFloatv", (glstub_proc_t)&glGetFloatv },
    { "glGetFramebufferAttachmentParameteriv", (glstub_proc_t)&glGetFramebufferAttachmentParameteriv },
    { "glGetIntegerv", (glstub_proc_t)&glGetIntegerv },
    { "glGetProgramiv", (glstub_proc_t)&glGetProgramiv },
    { "glGetProgramInfoLog", (glstub_proc_t)&glGetProgramInfoLog },
    { "glGetRenderbufferParameteriv", (glstub_proc_t)&glGetRenderbufferParameteriv },
    { "glGetShaderiv", (glstub_proc_t)&glGetShaderiv },
    { "glGetShaderInfoLog", (glstub_proc_t)&glGetShaderInfoLog },
    { "glGetShaderPrecisionFormat", (glstub_proc_t)&glGetShaderPrecisionFormat },
    { "glGetShaderSource", (glstub_proc_t)&glGetShaderSource },
    { "glGetString", (glstub_proc_t)&glGetString },
    { "glGetTexParameterfv", (glstub_proc_t)&glGetTexParameterfv },
    { "glGetTexParameteriv", (glstub_proc_t)&glGetTexParameteriv },
    { "glGetUniformfv", (glstub_proc_t)&glGetUniformfv },
    { "glGetUniformiv", (glstub_proc_t)&glGetUniformiv },
    { "glGetUniformLocation", (glstub_proc_t)&glGetUniformLocation },
    { "glGetVertexAttribfv", (glstub_proc_t)&glGetVertexAttribfv },
    { "glGetVertexAttribiv", (glstub_proc_t)&glGetVertexAttribiv },
    { "glGetVertexAttribPointerv", (glstub_proc_t)&glGetVertexAttribPointerv },
    { "glHint", (glstub_proc_t)&glHint },
    { "glIsBuffer", (glstub_proc_t)&glIsBuffer },
    { "glIsEnabled", (glstub_proc_t)&glIsEnabled },
    { "glIsFramebuffer", (glstub_proc_t)&glIsFramebuffer },
    { "glIsProgram", (glstub_proc_t)&glIsProgram },
    { "glIsRenderbuffer", (glstub_proc_t)&glIsRenderbuffer },
    { "glIsShader", (glstub_proc_t)&glIsShader },
    { "glIsTexture", (glstub_proc_t)&glIsTexture },
    { "glLineWidth", (glstub_proc_t)&glLineWidth },
    { "glLinkProgram", (glstub_proc_t)&glLinkProgram },
    { "glPixelStorei", (glstub_proc_t)&glPixelStorei },
    { "glPolygonOffset", (glstub_proc_t)&glPolygonOffset },
    { "glReadPixels", (glstub_proc_t)&glReadPixels },
    { "glReleaseShaderCompiler", (glstub_proc_t)&glReleaseShaderCompiler },
    { "glRenderbufferStorage", (glstub_proc_t)&glRenderbufferStorage },
    { "glSampleCoverage", (glstub_proc_t)&glSampleCoverage },
    { "glScissor", (glstub_proc_t)&glScissor },
    { "glShaderBinary", (glstub_proc_t)&glShaderBinary },
    { "glShaderSource", (glstub_proc_t)&glShaderSource },
    { "glStencilFunc", (glstub_proc_t)&glStencilFunc },
    { "glStencilFuncSeparate", (glstub_proc_t)&glStencilFuncSeparate },
    { "glStencilMask", (glstub_proc_t)&glStencilMask },
    { "glStencilMaskSeparate", (glstub_proc_t)&glStencilMaskSeparate },
    { "glStencilOp", (glstub_proc_t)&glStencilOp },
    { "glStencilOpSeparate", (glstub_proc_t)&glStencilOpSeparate },
    { "glTexImage2D", (glstub_proc_t)&glTexImage2D },
    { "glTexParameterf", (glstub_proc_t)&glTexParameterf },
    { "glTexParameterfv", (glstub_proc_t)&glTexParameterfv },
    { "glTexParameteri", (glstub_proc_t)&glTexParameteri },
    { "glTexParameteriv", (glstub_proc_t)&glTexParameteriv },
    { "glTexSubImage2D", (glstub_proc_t)&glTexSubImage2D },
    { "glUniform1f", (glstub_proc_t)&glUniform1f },
    { "glUniform1fv", (glstub_proc_t)&glUniform1fv },
    { "glUniform1i", (glstub_proc_t)&glUniform1i },
    { "glUniform1iv", (glstub_proc_t)&glUniform1iv },
    { "glUniform2f", (glstub_proc_t)&glUniform2f },
    { "glUniform2fv", (glstub_proc_t)&glUniform2fv },
    { "glUniform2i", (glstub_proc_t)&glUniform2i },
    { "glUniform2iv", (glstub_proc_t)&glUniform2iv },
    { "glUniform3f", (glstub_proc_t)&glUniform3f },
    { "glUniform3fv", (glstub_proc_t)&glUniform3fv },
    { "glUniform3i", (glstub_proc_t)&glUniform3i },
    { "glUniform3iv", (glstub_proc_t)&glUniform3iv },
    { "glUniform4f", (glstub_proc_t)&glUniform4f },
    { "glUniform4fv", (glstub_proc_t)&glUniform4fv },
    { "glUniform4i", (glstub_proc_t)&glUniform4i },
    { "glUniform4iv", (glstub_proc_t)&glUniform4iv },
    { "glUniformMatrix2fv", (glstub_proc_t)&glUniformMatrix2fv },
    { "glUniformMatrix3fv", (glstub_proc_t)&glUniformMatrix3fv },
    { "glUniformMatrix4fv", (glstub_proc_t)&glUniformMatrix4fv },
    { "glUseProgram", (glstub_proc_t)&glUseProgram },
    { "glValidateProgram", (glstub_proc_t)&glValidateProgram },
    { "glVertexAttrib1f", (glstub_proc_t)&glVertexAttrib1f },
    { "glVertexAttrib1fv", (glstub_proc_t)&glVertexAttrib1fv },
    { "glVertexAttrib2f", (glstub_proc_t)&glVertexAttrib2f },
    { "glVertexAttrib2fv", (glstub_proc_t)&glVertexAttrib2fv },
    { "glVertexAttrib3f", (glstub_proc_t)&glVertexAttrib3f },
    { "glVertexAttrib3fv", (glstub_proc_t)&glVertexAttrib3fv },
    { "glVertexAttrib4f", (glstub_proc_t)&glVertexAttrib4f },
    { "glVertexAttrib4fv", (glstub_proc_t)&glVertexAttrib4fv },
    { "glVertexAttribPointer", (glstub_proc_t)&glVertexAttribPointer },
    { "glViewport", (glstub_proc_t)&glViewport },
    { "eglChooseConfig", (glstub_proc_t)&eglChooseConfig },
    { "eglCreateContext", (glstub_proc_t)&eglCreateContext },
    { "eglCreatePbufferSurface", (glstub_proc_t)&eglCreatePbufferSurface },
    { "eglCreateWindowSurface", (glstub_proc_t)&eglCreateWindowSurface },
    { "eglDestroyContext", (glstub_proc_t)&eglDestroyContext },
    { "eglDestroySurface", (glstub_proc_t)&eglDestroySurface },
    { "eglGetConfigAttrib", (glstub_proc_t)&eglGetConfigAttrib },
    { "eglGetDisplay", (glstub_proc_t)&eglGetDisplay },
    { "eglGetError", (glstub_proc_t)&eglGetError },
    { "eglInitialize", (glstub_proc_t)&eglInitialize },
    { "eglMakeCurrent", (glstub_proc_t)&eglMakeCurrent },
    { "eglQueryString", (glstub_proc_t)&eglQueryString },
    { "eglQuerySurface", (glstub_proc_t)&eglQuerySurface },
    { "eglSwapBuffers", (glstub_proc_t)&eglSwapBuffers },
    { "eglTerminate", (glstub_proc_t)&eglTerminate },
};

uint32_t eglChooseConfig(void* dpy, const int32_t* attrib_list, void* configs, int32_t config_size, int32_t* num_config)
{
    GLSTUB_CALL(EGL_FN_CHOOSECONFIG, 5);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)attrib_list;
    _c.args[2] = (int64_t)(uintptr_t)configs;
    _c.args[3] = (int64_t)(int32_t)config_size;
    _c.args[4] = (int64_t)(uintptr_t)num_config;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void* eglCreateContext(void* dpy, void* config, void* share_context, const int32_t* attrib_list)
{
    GLSTUB_CALL(EGL_FN_CREATECONTEXT, 4);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)config;
    _c.args[2] = (int64_t)(uintptr_t)share_context;
    _c.args[3] = (int64_t)(uintptr_t)attrib_list;
    GLSTUB_DO(SYS_EGL_CALL);
    return (void*)(uintptr_t)_c.ret;
}

void* eglCreatePbufferSurface(void* dpy, void* config, const int32_t* attrib_list)
{
    GLSTUB_CALL(EGL_FN_CREATEPBUFFERSURFACE, 3);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)config;
    _c.args[2] = (int64_t)(uintptr_t)attrib_list;
    GLSTUB_DO(SYS_EGL_CALL);
    return (void*)(uintptr_t)_c.ret;
}

void* eglCreateWindowSurface(void* dpy, void* config, void* win, const int32_t* attrib_list)
{
    GLSTUB_CALL(EGL_FN_CREATEWINDOWSURFACE, 4);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)config;
    _c.args[2] = (int64_t)(uintptr_t)win;
    _c.args[3] = (int64_t)(uintptr_t)attrib_list;
    GLSTUB_DO(SYS_EGL_CALL);
    return (void*)(uintptr_t)_c.ret;
}

uint32_t eglDestroyContext(void* dpy, void* ctx)
{
    GLSTUB_CALL(EGL_FN_DESTROYCONTEXT, 2);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)ctx;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t eglDestroySurface(void* dpy, void* surface)
{
    GLSTUB_CALL(EGL_FN_DESTROYSURFACE, 2);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)surface;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t eglGetConfigAttrib(void* dpy, void* config, int32_t attribute, int32_t* value)
{
    GLSTUB_CALL(EGL_FN_GETCONFIGATTRIB, 4);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)config;
    _c.args[2] = (int64_t)(int32_t)attribute;
    _c.args[3] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void* eglGetDisplay(void* display_id)
{
    GLSTUB_CALL(EGL_FN_GETDISPLAY, 1);
    _c.args[0] = (int64_t)(uintptr_t)display_id;
    GLSTUB_DO(SYS_EGL_CALL);
    return (void*)(uintptr_t)_c.ret;
}

int32_t eglGetError(void)
{
    GLSTUB_CALL(EGL_FN_GETERROR, 0);
    GLSTUB_DO(SYS_EGL_CALL);
    return (int32_t)(int32_t)_c.ret;
}

void* eglGetProcAddress(const char* procname)
{
    const char* name = (const char*)procname;
    if (name) {
        for (size_t i = 0; i < sizeof(glstub_procs) / sizeof(glstub_procs[0]); i++) {
            if (strcmp(name, glstub_procs[i].name) == 0) {
                return (void*)(uintptr_t)glstub_procs[i].proc;
            }
        }
    }
    return (void*)0;
}

uint32_t eglInitialize(void* dpy, int32_t* major, int32_t* minor)
{
    GLSTUB_CALL(EGL_FN_INITIALIZE, 3);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)major;
    _c.args[2] = (int64_t)(uintptr_t)minor;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx)
{
    GLSTUB_CALL(EGL_FN_MAKECURRENT, 4);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)draw;
    _c.args[2] = (int64_t)(uintptr_t)read;
    _c.args[3] = (int64_t)(uintptr_t)ctx;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

const char* eglQueryString(void* dpy, int32_t name)
{
    GLSTUB_CALL(EGL_FN_QUERYSTRING, 2);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(int32_t)name;
    _c.args[GL_CALL_RETBUF_SLOT] = (int64_t)(uintptr_t)glstub_retbuf;
    GLSTUB_DO(SYS_EGL_CALL);
    return (const char*)(uintptr_t)glstub_retbuf;
}

uint32_t eglQuerySurface(void* dpy, void* surface, int32_t attribute, int32_t* value)
{
    GLSTUB_CALL(EGL_FN_QUERYSURFACE, 4);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)surface;
    _c.args[2] = (int64_t)(int32_t)attribute;
    _c.args[3] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t eglSwapBuffers(void* dpy, void* surface)
{
    GLSTUB_CALL(EGL_FN_SWAPBUFFERS, 2);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    _c.args[1] = (int64_t)(uintptr_t)surface;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t eglTerminate(void* dpy)
{
    GLSTUB_CALL(EGL_FN_TERMINATE, 1);
    _c.args[0] = (int64_t)(uintptr_t)dpy;
    GLSTUB_DO(SYS_EGL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void glActiveTexture(uint32_t texture)
{
    GLSTUB_CALL(GL_FN_ACTIVETEXTURE, 1);
    _c.args[0] = (int64_t)(uint32_t)texture;
    GLSTUB_DO(SYS_GL_CALL);
}

void glAttachShader(uint32_t program, uint32_t shader)
{
    GLSTUB_CALL(GL_FN_ATTACHSHADER, 2);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)shader;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBindAttribLocation(uint32_t program, uint32_t index, const char* name)
{
    GLSTUB_CALL(GL_FN_BINDATTRIBLOCATION, 3);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)index;
    _c.args[2] = (int64_t)(uintptr_t)name;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBindBuffer(uint32_t target, uint32_t buffer)
{
    GLSTUB_CALL(GL_FN_BINDBUFFER, 2);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)buffer;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBindFramebuffer(uint32_t target, uint32_t framebuffer)
{
    GLSTUB_CALL(GL_FN_BINDFRAMEBUFFER, 2);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)framebuffer;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBindRenderbuffer(uint32_t target, uint32_t renderbuffer)
{
    GLSTUB_CALL(GL_FN_BINDRENDERBUFFER, 2);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)renderbuffer;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBindTexture(uint32_t target, uint32_t texture)
{
    GLSTUB_CALL(GL_FN_BINDTEXTURE, 2);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)texture;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBlendColor(float red, float green, float blue, float alpha)
{
    GLSTUB_CALL(GL_FN_BLENDCOLOR, 4);
    _c.args[0] = glstub_packf(red);
    _c.args[1] = glstub_packf(green);
    _c.args[2] = glstub_packf(blue);
    _c.args[3] = glstub_packf(alpha);
    GLSTUB_DO(SYS_GL_CALL);
}

void glBlendEquation(uint32_t mode)
{
    GLSTUB_CALL(GL_FN_BLENDEQUATION, 1);
    _c.args[0] = (int64_t)(uint32_t)mode;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBlendEquationSeparate(uint32_t modeRGB, uint32_t modeAlpha)
{
    GLSTUB_CALL(GL_FN_BLENDEQUATIONSEPARATE, 2);
    _c.args[0] = (int64_t)(uint32_t)modeRGB;
    _c.args[1] = (int64_t)(uint32_t)modeAlpha;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBlendFunc(uint32_t sfactor, uint32_t dfactor)
{
    GLSTUB_CALL(GL_FN_BLENDFUNC, 2);
    _c.args[0] = (int64_t)(uint32_t)sfactor;
    _c.args[1] = (int64_t)(uint32_t)dfactor;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBlendFuncSeparate(uint32_t sfactorRGB, uint32_t dfactorRGB, uint32_t sfactorAlpha, uint32_t dfactorAlpha)
{
    GLSTUB_CALL(GL_FN_BLENDFUNCSEPARATE, 4);
    _c.args[0] = (int64_t)(uint32_t)sfactorRGB;
    _c.args[1] = (int64_t)(uint32_t)dfactorRGB;
    _c.args[2] = (int64_t)(uint32_t)sfactorAlpha;
    _c.args[3] = (int64_t)(uint32_t)dfactorAlpha;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBufferData(uint32_t target, long size, const void* data, uint32_t usage)
{
    GLSTUB_CALL(GL_FN_BUFFERDATA, 4);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)size;
    _c.args[2] = (int64_t)(uintptr_t)data;
    _c.args[3] = (int64_t)(uint32_t)usage;
    GLSTUB_DO(SYS_GL_CALL);
}

void glBufferSubData(uint32_t target, long offset, long size, const void* data)
{
    GLSTUB_CALL(GL_FN_BUFFERSUBDATA, 4);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)offset;
    _c.args[2] = (int64_t)size;
    _c.args[3] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

uint32_t glCheckFramebufferStatus(uint32_t target)
{
    GLSTUB_CALL(GL_FN_CHECKFRAMEBUFFERSTATUS, 1);
    _c.args[0] = (int64_t)(uint32_t)target;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void glClear(uint32_t mask)
{
    GLSTUB_CALL(GL_FN_CLEAR, 1);
    _c.args[0] = (int64_t)(uint32_t)mask;
    GLSTUB_DO(SYS_GL_CALL);
}

void glClearColor(float red, float green, float blue, float alpha)
{
    GLSTUB_CALL(GL_FN_CLEARCOLOR, 4);
    _c.args[0] = glstub_packf(red);
    _c.args[1] = glstub_packf(green);
    _c.args[2] = glstub_packf(blue);
    _c.args[3] = glstub_packf(alpha);
    GLSTUB_DO(SYS_GL_CALL);
}

void glClearDepthf(float d)
{
    GLSTUB_CALL(GL_FN_CLEARDEPTHF, 1);
    _c.args[0] = glstub_packf(d);
    GLSTUB_DO(SYS_GL_CALL);
}

void glClearStencil(int32_t s)
{
    GLSTUB_CALL(GL_FN_CLEARSTENCIL, 1);
    _c.args[0] = (int64_t)(int32_t)s;
    GLSTUB_DO(SYS_GL_CALL);
}

void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    GLSTUB_CALL(GL_FN_COLORMASK, 4);
    _c.args[0] = (int64_t)(uint32_t)red;
    _c.args[1] = (int64_t)(uint32_t)green;
    _c.args[2] = (int64_t)(uint32_t)blue;
    _c.args[3] = (int64_t)(uint32_t)alpha;
    GLSTUB_DO(SYS_GL_CALL);
}

void glCompileShader(uint32_t shader)
{
    GLSTUB_CALL(GL_FN_COMPILESHADER, 1);
    _c.args[0] = (int64_t)(uint32_t)shader;
    GLSTUB_DO(SYS_GL_CALL);
}

void glCompressedTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t border, int32_t imageSize, const void* data)
{
    GLSTUB_CALL(GL_FN_COMPRESSEDTEXIMAGE2D, 8);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(uint32_t)internalformat;
    _c.args[3] = (int64_t)(int32_t)width;
    _c.args[4] = (int64_t)(int32_t)height;
    _c.args[5] = (int64_t)(int32_t)border;
    _c.args[6] = (int64_t)(int32_t)imageSize;
    _c.args[7] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

void glCompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, int32_t imageSize, const void* data)
{
    GLSTUB_CALL(GL_FN_COMPRESSEDTEXSUBIMAGE2D, 9);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(int32_t)xoffset;
    _c.args[3] = (int64_t)(int32_t)yoffset;
    _c.args[4] = (int64_t)(int32_t)width;
    _c.args[5] = (int64_t)(int32_t)height;
    _c.args[6] = (int64_t)(uint32_t)format;
    _c.args[7] = (int64_t)(int32_t)imageSize;
    _c.args[8] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y, int32_t width, int32_t height, int32_t border)
{
    GLSTUB_CALL(GL_FN_COPYTEXIMAGE2D, 8);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(uint32_t)internalformat;
    _c.args[3] = (int64_t)(int32_t)x;
    _c.args[4] = (int64_t)(int32_t)y;
    _c.args[5] = (int64_t)(int32_t)width;
    _c.args[6] = (int64_t)(int32_t)height;
    _c.args[7] = (int64_t)(int32_t)border;
    GLSTUB_DO(SYS_GL_CALL);
}

void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t width, int32_t height)
{
    GLSTUB_CALL(GL_FN_COPYTEXSUBIMAGE2D, 8);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(int32_t)xoffset;
    _c.args[3] = (int64_t)(int32_t)yoffset;
    _c.args[4] = (int64_t)(int32_t)x;
    _c.args[5] = (int64_t)(int32_t)y;
    _c.args[6] = (int64_t)(int32_t)width;
    _c.args[7] = (int64_t)(int32_t)height;
    GLSTUB_DO(SYS_GL_CALL);
}

uint32_t glCreateProgram(void)
{
    GLSTUB_CALL(GL_FN_CREATEPROGRAM, 0);
    GLSTUB_DO(SYS_GL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

uint32_t glCreateShader(uint32_t type)
{
    GLSTUB_CALL(GL_FN_CREATESHADER, 1);
    _c.args[0] = (int64_t)(uint32_t)type;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void glCullFace(uint32_t mode)
{
    GLSTUB_CALL(GL_FN_CULLFACE, 1);
    _c.args[0] = (int64_t)(uint32_t)mode;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteBuffers(int32_t n, const uint32_t* buffers)
{
    GLSTUB_CALL(GL_FN_DELETEBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)buffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteFramebuffers(int32_t n, const uint32_t* framebuffers)
{
    GLSTUB_CALL(GL_FN_DELETEFRAMEBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)framebuffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteProgram(uint32_t program)
{
    GLSTUB_CALL(GL_FN_DELETEPROGRAM, 1);
    _c.args[0] = (int64_t)(uint32_t)program;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteRenderbuffers(int32_t n, const uint32_t* renderbuffers)
{
    GLSTUB_CALL(GL_FN_DELETERENDERBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)renderbuffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteShader(uint32_t shader)
{
    GLSTUB_CALL(GL_FN_DELETESHADER, 1);
    _c.args[0] = (int64_t)(uint32_t)shader;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDeleteTextures(int32_t n, const uint32_t* textures)
{
    GLSTUB_CALL(GL_FN_DELETETEXTURES, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)textures;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDepthFunc(uint32_t func)
{
    GLSTUB_CALL(GL_FN_DEPTHFUNC, 1);
    _c.args[0] = (int64_t)(uint32_t)func;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDepthMask(uint8_t flag)
{
    GLSTUB_CALL(GL_FN_DEPTHMASK, 1);
    _c.args[0] = (int64_t)(uint32_t)flag;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDepthRangef(float n, float f)
{
    GLSTUB_CALL(GL_FN_DEPTHRANGEF, 2);
    _c.args[0] = glstub_packf(n);
    _c.args[1] = glstub_packf(f);
    GLSTUB_DO(SYS_GL_CALL);
}

void glDetachShader(uint32_t program, uint32_t shader)
{
    GLSTUB_CALL(GL_FN_DETACHSHADER, 2);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)shader;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDisable(uint32_t cap)
{
    GLSTUB_CALL(GL_FN_DISABLE, 1);
    _c.args[0] = (int64_t)(uint32_t)cap;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDisableVertexAttribArray(uint32_t index)
{
    GLSTUB_CALL(GL_FN_DISABLEVERTEXATTRIBARRAY, 1);
    _c.args[0] = (int64_t)(uint32_t)index;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDrawArrays(uint32_t mode, int32_t first, int32_t count)
{
    GLSTUB_CALL(GL_FN_DRAWARRAYS, 3);
    _c.args[0] = (int64_t)(uint32_t)mode;
    _c.args[1] = (int64_t)(int32_t)first;
    _c.args[2] = (int64_t)(int32_t)count;
    GLSTUB_DO(SYS_GL_CALL);
}

void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* indices)
{
    GLSTUB_CALL(GL_FN_DRAWELEMENTS, 4);
    _c.args[0] = (int64_t)(uint32_t)mode;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uint32_t)type;
    _c.args[3] = GLSTUB_OFFSET_PTR(indices);
    GLSTUB_DO(SYS_GL_CALL);
}

void glEnable(uint32_t cap)
{
    GLSTUB_CALL(GL_FN_ENABLE, 1);
    _c.args[0] = (int64_t)(uint32_t)cap;
    GLSTUB_DO(SYS_GL_CALL);
}

void glEnableVertexAttribArray(uint32_t index)
{
    GLSTUB_CALL(GL_FN_ENABLEVERTEXATTRIBARRAY, 1);
    _c.args[0] = (int64_t)(uint32_t)index;
    GLSTUB_DO(SYS_GL_CALL);
}

void glFinish(void)
{
    GLSTUB_CALL(GL_FN_FINISH, 0);
    GLSTUB_DO(SYS_GL_CALL);
}

void glFlush(void)
{
    GLSTUB_CALL(GL_FN_FLUSH, 0);
    GLSTUB_DO(SYS_GL_CALL);
}

void glFramebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbuffertarget, uint32_t renderbuffer)
{
    GLSTUB_CALL(GL_FN_FRAMEBUFFERRENDERBUFFER, 4);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)attachment;
    _c.args[2] = (int64_t)(uint32_t)renderbuffertarget;
    _c.args[3] = (int64_t)(uint32_t)renderbuffer;
    GLSTUB_DO(SYS_GL_CALL);
}

void glFramebufferTexture2D(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level)
{
    GLSTUB_CALL(GL_FN_FRAMEBUFFERTEXTURE2D, 5);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)attachment;
    _c.args[2] = (int64_t)(uint32_t)textarget;
    _c.args[3] = (int64_t)(uint32_t)texture;
    _c.args[4] = (int64_t)(int32_t)level;
    GLSTUB_DO(SYS_GL_CALL);
}

void glFrontFace(uint32_t mode)
{
    GLSTUB_CALL(GL_FN_FRONTFACE, 1);
    _c.args[0] = (int64_t)(uint32_t)mode;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGenBuffers(int32_t n, uint32_t* buffers)
{
    GLSTUB_CALL(GL_FN_GENBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)buffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGenerateMipmap(uint32_t target)
{
    GLSTUB_CALL(GL_FN_GENERATEMIPMAP, 1);
    _c.args[0] = (int64_t)(uint32_t)target;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGenFramebuffers(int32_t n, uint32_t* framebuffers)
{
    GLSTUB_CALL(GL_FN_GENFRAMEBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)framebuffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGenRenderbuffers(int32_t n, uint32_t* renderbuffers)
{
    GLSTUB_CALL(GL_FN_GENRENDERBUFFERS, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)renderbuffers;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGenTextures(int32_t n, uint32_t* textures)
{
    GLSTUB_CALL(GL_FN_GENTEXTURES, 2);
    _c.args[0] = (int64_t)(int32_t)n;
    _c.args[1] = (int64_t)(uintptr_t)textures;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetActiveAttrib(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name)
{
    GLSTUB_CALL(GL_FN_GETACTIVEATTRIB, 7);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)index;
    _c.args[2] = (int64_t)(int32_t)bufSize;
    _c.args[3] = (int64_t)(uintptr_t)length;
    _c.args[4] = (int64_t)(uintptr_t)size;
    _c.args[5] = (int64_t)(uintptr_t)type;
    _c.args[6] = (int64_t)(uintptr_t)name;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetActiveUniform(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name)
{
    GLSTUB_CALL(GL_FN_GETACTIVEUNIFORM, 7);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)index;
    _c.args[2] = (int64_t)(int32_t)bufSize;
    _c.args[3] = (int64_t)(uintptr_t)length;
    _c.args[4] = (int64_t)(uintptr_t)size;
    _c.args[5] = (int64_t)(uintptr_t)type;
    _c.args[6] = (int64_t)(uintptr_t)name;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetAttachedShaders(uint32_t program, int32_t maxCount, int32_t* count, uint32_t* shaders)
{
    GLSTUB_CALL(GL_FN_GETATTACHEDSHADERS, 4);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(int32_t)maxCount;
    _c.args[2] = (int64_t)(uintptr_t)count;
    _c.args[3] = (int64_t)(uintptr_t)shaders;
    GLSTUB_DO(SYS_GL_CALL);
}

int32_t glGetAttribLocation(uint32_t program, const char* name)
{
    GLSTUB_CALL(GL_FN_GETATTRIBLOCATION, 2);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uintptr_t)name;
    GLSTUB_DO(SYS_GL_CALL);
    return (int32_t)(int32_t)_c.ret;
}

void glGetBooleanv(uint32_t pname, uint8_t* data)
{
    GLSTUB_CALL(GL_FN_GETBOOLEANV, 2);
    _c.args[0] = (int64_t)(uint32_t)pname;
    _c.args[1] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetBufferParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETBUFFERPARAMETERIV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

uint32_t glGetError(void)
{
    GLSTUB_CALL(GL_FN_GETERROR, 0);
    GLSTUB_DO(SYS_GL_CALL);
    return (uint32_t)(uint32_t)_c.ret;
}

void glGetFloatv(uint32_t pname, float* data)
{
    GLSTUB_CALL(GL_FN_GETFLOATV, 2);
    _c.args[0] = (int64_t)(uint32_t)pname;
    _c.args[1] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetFramebufferAttachmentParameteriv(uint32_t target, uint32_t attachment, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV, 4);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)attachment;
    _c.args[2] = (int64_t)(uint32_t)pname;
    _c.args[3] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetIntegerv(uint32_t pname, int32_t* data)
{
    GLSTUB_CALL(GL_FN_GETINTEGERV, 2);
    _c.args[0] = (int64_t)(uint32_t)pname;
    _c.args[1] = (int64_t)(uintptr_t)data;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetProgramiv(uint32_t program, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETPROGRAMIV, 3);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetProgramInfoLog(uint32_t program, int32_t bufSize, int32_t* length, char* infoLog)
{
    GLSTUB_CALL(GL_FN_GETPROGRAMINFOLOG, 4);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(int32_t)bufSize;
    _c.args[2] = (int64_t)(uintptr_t)length;
    _c.args[3] = (int64_t)(uintptr_t)infoLog;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetRenderbufferParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETRENDERBUFFERPARAMETERIV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetShaderiv(uint32_t shader, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETSHADERIV, 3);
    _c.args[0] = (int64_t)(uint32_t)shader;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetShaderInfoLog(uint32_t shader, int32_t bufSize, int32_t* length, char* infoLog)
{
    GLSTUB_CALL(GL_FN_GETSHADERINFOLOG, 4);
    _c.args[0] = (int64_t)(uint32_t)shader;
    _c.args[1] = (int64_t)(int32_t)bufSize;
    _c.args[2] = (int64_t)(uintptr_t)length;
    _c.args[3] = (int64_t)(uintptr_t)infoLog;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetShaderPrecisionFormat(uint32_t shadertype, uint32_t precisiontype, int32_t* range, int32_t* precision)
{
    GLSTUB_CALL(GL_FN_GETSHADERPRECISIONFORMAT, 4);
    _c.args[0] = (int64_t)(uint32_t)shadertype;
    _c.args[1] = (int64_t)(uint32_t)precisiontype;
    _c.args[2] = (int64_t)(uintptr_t)range;
    _c.args[3] = (int64_t)(uintptr_t)precision;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetShaderSource(uint32_t shader, int32_t bufSize, int32_t* length, char* source)
{
    GLSTUB_CALL(GL_FN_GETSHADERSOURCE, 4);
    _c.args[0] = (int64_t)(uint32_t)shader;
    _c.args[1] = (int64_t)(int32_t)bufSize;
    _c.args[2] = (int64_t)(uintptr_t)length;
    _c.args[3] = (int64_t)(uintptr_t)source;
    GLSTUB_DO(SYS_GL_CALL);
}

const uint8_t* glGetString(uint32_t name)
{
    GLSTUB_CALL(GL_FN_GETSTRING, 1);
    _c.args[0] = (int64_t)(uint32_t)name;
    _c.args[GL_CALL_RETBUF_SLOT] = (int64_t)(uintptr_t)glstub_retbuf;
    GLSTUB_DO(SYS_GL_CALL);
    return (const uint8_t*)(uintptr_t)glstub_retbuf;
}

void glGetTexParameterfv(uint32_t target, uint32_t pname, float* params)
{
    GLSTUB_CALL(GL_FN_GETTEXPARAMETERFV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETTEXPARAMETERIV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetUniformfv(uint32_t program, int32_t location, float* params)
{
    GLSTUB_CALL(GL_FN_GETUNIFORMFV, 3);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(int32_t)location;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetUniformiv(uint32_t program, int32_t location, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETUNIFORMIV, 3);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(int32_t)location;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

int32_t glGetUniformLocation(uint32_t program, const char* name)
{
    GLSTUB_CALL(GL_FN_GETUNIFORMLOCATION, 2);
    _c.args[0] = (int64_t)(uint32_t)program;
    _c.args[1] = (int64_t)(uintptr_t)name;
    GLSTUB_DO(SYS_GL_CALL);
    return (int32_t)(int32_t)_c.ret;
}

void glGetVertexAttribfv(uint32_t index, uint32_t pname, float* params)
{
    GLSTUB_CALL(GL_FN_GETVERTEXATTRIBFV, 3);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetVertexAttribiv(uint32_t index, uint32_t pname, int32_t* params)
{
    GLSTUB_CALL(GL_FN_GETVERTEXATTRIBIV, 3);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glGetVertexAttribPointerv(uint32_t index, uint32_t pname, void** pointer)
{
    GLSTUB_CALL(GL_FN_GETVERTEXATTRIBPOINTERV, 3);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)pointer;
    GLSTUB_DO(SYS_GL_CALL);
}

void glHint(uint32_t target, uint32_t mode)
{
    GLSTUB_CALL(GL_FN_HINT, 2);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)mode;
    GLSTUB_DO(SYS_GL_CALL);
}

uint8_t glIsBuffer(uint32_t buffer)
{
    GLSTUB_CALL(GL_FN_ISBUFFER, 1);
    _c.args[0] = (int64_t)(uint32_t)buffer;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsEnabled(uint32_t cap)
{
    GLSTUB_CALL(GL_FN_ISENABLED, 1);
    _c.args[0] = (int64_t)(uint32_t)cap;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsFramebuffer(uint32_t framebuffer)
{
    GLSTUB_CALL(GL_FN_ISFRAMEBUFFER, 1);
    _c.args[0] = (int64_t)(uint32_t)framebuffer;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsProgram(uint32_t program)
{
    GLSTUB_CALL(GL_FN_ISPROGRAM, 1);
    _c.args[0] = (int64_t)(uint32_t)program;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsRenderbuffer(uint32_t renderbuffer)
{
    GLSTUB_CALL(GL_FN_ISRENDERBUFFER, 1);
    _c.args[0] = (int64_t)(uint32_t)renderbuffer;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsShader(uint32_t shader)
{
    GLSTUB_CALL(GL_FN_ISSHADER, 1);
    _c.args[0] = (int64_t)(uint32_t)shader;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

uint8_t glIsTexture(uint32_t texture)
{
    GLSTUB_CALL(GL_FN_ISTEXTURE, 1);
    _c.args[0] = (int64_t)(uint32_t)texture;
    GLSTUB_DO(SYS_GL_CALL);
    return (uint8_t)(uint32_t)_c.ret;
}

void glLineWidth(float width)
{
    GLSTUB_CALL(GL_FN_LINEWIDTH, 1);
    _c.args[0] = glstub_packf(width);
    GLSTUB_DO(SYS_GL_CALL);
}

void glLinkProgram(uint32_t program)
{
    GLSTUB_CALL(GL_FN_LINKPROGRAM, 1);
    _c.args[0] = (int64_t)(uint32_t)program;
    GLSTUB_DO(SYS_GL_CALL);
}

void glPixelStorei(uint32_t pname, int32_t param)
{
    GLSTUB_CALL(GL_FN_PIXELSTOREI, 2);
    _c.args[0] = (int64_t)(uint32_t)pname;
    _c.args[1] = (int64_t)(int32_t)param;
    GLSTUB_DO(SYS_GL_CALL);
}

void glPolygonOffset(float factor, float units)
{
    GLSTUB_CALL(GL_FN_POLYGONOFFSET, 2);
    _c.args[0] = glstub_packf(factor);
    _c.args[1] = glstub_packf(units);
    GLSTUB_DO(SYS_GL_CALL);
}

void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void* pixels)
{
    GLSTUB_CALL(GL_FN_READPIXELS, 7);
    _c.args[0] = (int64_t)(int32_t)x;
    _c.args[1] = (int64_t)(int32_t)y;
    _c.args[2] = (int64_t)(int32_t)width;
    _c.args[3] = (int64_t)(int32_t)height;
    _c.args[4] = (int64_t)(uint32_t)format;
    _c.args[5] = (int64_t)(uint32_t)type;
    _c.args[6] = (int64_t)(uintptr_t)pixels;
    GLSTUB_DO(SYS_GL_CALL);
}

void glReleaseShaderCompiler(void)
{
    GLSTUB_CALL(GL_FN_RELEASESHADERCOMPILER, 0);
    GLSTUB_DO(SYS_GL_CALL);
}

void glRenderbufferStorage(uint32_t target, uint32_t internalformat, int32_t width, int32_t height)
{
    GLSTUB_CALL(GL_FN_RENDERBUFFERSTORAGE, 4);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)internalformat;
    _c.args[2] = (int64_t)(int32_t)width;
    _c.args[3] = (int64_t)(int32_t)height;
    GLSTUB_DO(SYS_GL_CALL);
}

void glSampleCoverage(float value, uint8_t invert)
{
    GLSTUB_CALL(GL_FN_SAMPLECOVERAGE, 2);
    _c.args[0] = glstub_packf(value);
    _c.args[1] = (int64_t)(uint32_t)invert;
    GLSTUB_DO(SYS_GL_CALL);
}

void glScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GLSTUB_CALL(GL_FN_SCISSOR, 4);
    _c.args[0] = (int64_t)(int32_t)x;
    _c.args[1] = (int64_t)(int32_t)y;
    _c.args[2] = (int64_t)(int32_t)width;
    _c.args[3] = (int64_t)(int32_t)height;
    GLSTUB_DO(SYS_GL_CALL);
}

void glShaderBinary(int32_t count, const uint32_t* shaders, uint32_t binaryformat, const void* binary, int32_t length)
{
    GLSTUB_CALL(GL_FN_SHADERBINARY, 5);
    _c.args[0] = (int64_t)(int32_t)count;
    _c.args[1] = (int64_t)(uintptr_t)shaders;
    _c.args[2] = (int64_t)(uint32_t)binaryformat;
    _c.args[3] = (int64_t)(uintptr_t)binary;
    _c.args[4] = (int64_t)(int32_t)length;
    GLSTUB_DO(SYS_GL_CALL);
}

void glShaderSource(uint32_t shader, int32_t count, const char** string, const int32_t* length)
{
    GLSTUB_CALL(GL_FN_SHADERSOURCE, 4);
    _c.args[0] = (int64_t)(uint32_t)shader;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)string;
    _c.args[3] = (int64_t)(uintptr_t)length;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilFunc(uint32_t func, int32_t ref, uint32_t mask)
{
    GLSTUB_CALL(GL_FN_STENCILFUNC, 3);
    _c.args[0] = (int64_t)(uint32_t)func;
    _c.args[1] = (int64_t)(int32_t)ref;
    _c.args[2] = (int64_t)(uint32_t)mask;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilFuncSeparate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask)
{
    GLSTUB_CALL(GL_FN_STENCILFUNCSEPARATE, 4);
    _c.args[0] = (int64_t)(uint32_t)face;
    _c.args[1] = (int64_t)(uint32_t)func;
    _c.args[2] = (int64_t)(int32_t)ref;
    _c.args[3] = (int64_t)(uint32_t)mask;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilMask(uint32_t mask)
{
    GLSTUB_CALL(GL_FN_STENCILMASK, 1);
    _c.args[0] = (int64_t)(uint32_t)mask;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilMaskSeparate(uint32_t face, uint32_t mask)
{
    GLSTUB_CALL(GL_FN_STENCILMASKSEPARATE, 2);
    _c.args[0] = (int64_t)(uint32_t)face;
    _c.args[1] = (int64_t)(uint32_t)mask;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    GLSTUB_CALL(GL_FN_STENCILOP, 3);
    _c.args[0] = (int64_t)(uint32_t)fail;
    _c.args[1] = (int64_t)(uint32_t)zfail;
    _c.args[2] = (int64_t)(uint32_t)zpass;
    GLSTUB_DO(SYS_GL_CALL);
}

void glStencilOpSeparate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass)
{
    GLSTUB_CALL(GL_FN_STENCILOPSEPARATE, 4);
    _c.args[0] = (int64_t)(uint32_t)face;
    _c.args[1] = (int64_t)(uint32_t)sfail;
    _c.args[2] = (int64_t)(uint32_t)dpfail;
    _c.args[3] = (int64_t)(uint32_t)dppass;
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void* pixels)
{
    GLSTUB_CALL(GL_FN_TEXIMAGE2D, 9);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(int32_t)internalformat;
    _c.args[3] = (int64_t)(int32_t)width;
    _c.args[4] = (int64_t)(int32_t)height;
    _c.args[5] = (int64_t)(int32_t)border;
    _c.args[6] = (int64_t)(uint32_t)format;
    _c.args[7] = (int64_t)(uint32_t)type;
    _c.args[8] = (int64_t)(uintptr_t)pixels;
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexParameterf(uint32_t target, uint32_t pname, float param)
{
    GLSTUB_CALL(GL_FN_TEXPARAMETERF, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = glstub_packf(param);
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexParameterfv(uint32_t target, uint32_t pname, const float* params)
{
    GLSTUB_CALL(GL_FN_TEXPARAMETERFV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexParameteri(uint32_t target, uint32_t pname, int32_t param)
{
    GLSTUB_CALL(GL_FN_TEXPARAMETERI, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(int32_t)param;
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexParameteriv(uint32_t target, uint32_t pname, const int32_t* params)
{
    GLSTUB_CALL(GL_FN_TEXPARAMETERIV, 3);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(uint32_t)pname;
    _c.args[2] = (int64_t)(uintptr_t)params;
    GLSTUB_DO(SYS_GL_CALL);
}

void glTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels)
{
    GLSTUB_CALL(GL_FN_TEXSUBIMAGE2D, 9);
    _c.args[0] = (int64_t)(uint32_t)target;
    _c.args[1] = (int64_t)(int32_t)level;
    _c.args[2] = (int64_t)(int32_t)xoffset;
    _c.args[3] = (int64_t)(int32_t)yoffset;
    _c.args[4] = (int64_t)(int32_t)width;
    _c.args[5] = (int64_t)(int32_t)height;
    _c.args[6] = (int64_t)(uint32_t)format;
    _c.args[7] = (int64_t)(uint32_t)type;
    _c.args[8] = (int64_t)(uintptr_t)pixels;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform1f(int32_t location, float v0)
{
    GLSTUB_CALL(GL_FN_UNIFORM1F, 2);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = glstub_packf(v0);
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform1fv(int32_t location, int32_t count, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM1FV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform1i(int32_t location, int32_t v0)
{
    GLSTUB_CALL(GL_FN_UNIFORM1I, 2);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)v0;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform1iv(int32_t location, int32_t count, const int32_t* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM1IV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform2f(int32_t location, float v0, float v1)
{
    GLSTUB_CALL(GL_FN_UNIFORM2F, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = glstub_packf(v0);
    _c.args[2] = glstub_packf(v1);
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform2fv(int32_t location, int32_t count, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM2FV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform2i(int32_t location, int32_t v0, int32_t v1)
{
    GLSTUB_CALL(GL_FN_UNIFORM2I, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)v0;
    _c.args[2] = (int64_t)(int32_t)v1;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform2iv(int32_t location, int32_t count, const int32_t* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM2IV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform3f(int32_t location, float v0, float v1, float v2)
{
    GLSTUB_CALL(GL_FN_UNIFORM3F, 4);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = glstub_packf(v0);
    _c.args[2] = glstub_packf(v1);
    _c.args[3] = glstub_packf(v2);
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform3fv(int32_t location, int32_t count, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM3FV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform3i(int32_t location, int32_t v0, int32_t v1, int32_t v2)
{
    GLSTUB_CALL(GL_FN_UNIFORM3I, 4);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)v0;
    _c.args[2] = (int64_t)(int32_t)v1;
    _c.args[3] = (int64_t)(int32_t)v2;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform3iv(int32_t location, int32_t count, const int32_t* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM3IV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform4f(int32_t location, float v0, float v1, float v2, float v3)
{
    GLSTUB_CALL(GL_FN_UNIFORM4F, 5);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = glstub_packf(v0);
    _c.args[2] = glstub_packf(v1);
    _c.args[3] = glstub_packf(v2);
    _c.args[4] = glstub_packf(v3);
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform4fv(int32_t location, int32_t count, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM4FV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform4i(int32_t location, int32_t v0, int32_t v1, int32_t v2, int32_t v3)
{
    GLSTUB_CALL(GL_FN_UNIFORM4I, 5);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)v0;
    _c.args[2] = (int64_t)(int32_t)v1;
    _c.args[3] = (int64_t)(int32_t)v2;
    _c.args[4] = (int64_t)(int32_t)v3;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniform4iv(int32_t location, int32_t count, const int32_t* value)
{
    GLSTUB_CALL(GL_FN_UNIFORM4IV, 3);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniformMatrix2fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORMMATRIX2FV, 4);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uint32_t)transpose;
    _c.args[3] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniformMatrix3fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORMMATRIX3FV, 4);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uint32_t)transpose;
    _c.args[3] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUniformMatrix4fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    GLSTUB_CALL(GL_FN_UNIFORMMATRIX4FV, 4);
    _c.args[0] = (int64_t)(int32_t)location;
    _c.args[1] = (int64_t)(int32_t)count;
    _c.args[2] = (int64_t)(uint32_t)transpose;
    _c.args[3] = (int64_t)(uintptr_t)value;
    GLSTUB_DO(SYS_GL_CALL);
}

void glUseProgram(uint32_t program)
{
    GLSTUB_CALL(GL_FN_USEPROGRAM, 1);
    _c.args[0] = (int64_t)(uint32_t)program;
    GLSTUB_DO(SYS_GL_CALL);
}

void glValidateProgram(uint32_t program)
{
    GLSTUB_CALL(GL_FN_VALIDATEPROGRAM, 1);
    _c.args[0] = (int64_t)(uint32_t)program;
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib1f(uint32_t index, float x)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB1F, 2);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = glstub_packf(x);
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib1fv(uint32_t index, const float* v)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB1FV, 2);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uintptr_t)v;
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib2f(uint32_t index, float x, float y)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB2F, 3);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = glstub_packf(x);
    _c.args[2] = glstub_packf(y);
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib2fv(uint32_t index, const float* v)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB2FV, 2);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uintptr_t)v;
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib3f(uint32_t index, float x, float y, float z)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB3F, 4);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = glstub_packf(x);
    _c.args[2] = glstub_packf(y);
    _c.args[3] = glstub_packf(z);
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib3fv(uint32_t index, const float* v)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB3FV, 2);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uintptr_t)v;
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib4f(uint32_t index, float x, float y, float z, float w)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB4F, 5);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = glstub_packf(x);
    _c.args[2] = glstub_packf(y);
    _c.args[3] = glstub_packf(z);
    _c.args[4] = glstub_packf(w);
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttrib4fv(uint32_t index, const float* v)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIB4FV, 2);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(uintptr_t)v;
    GLSTUB_DO(SYS_GL_CALL);
}

void glVertexAttribPointer(uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride, const void* pointer)
{
    GLSTUB_CALL(GL_FN_VERTEXATTRIBPOINTER, 6);
    _c.args[0] = (int64_t)(uint32_t)index;
    _c.args[1] = (int64_t)(int32_t)size;
    _c.args[2] = (int64_t)(uint32_t)type;
    _c.args[3] = (int64_t)(uint32_t)normalized;
    _c.args[4] = (int64_t)(int32_t)stride;
    _c.args[5] = GLSTUB_OFFSET_PTR(pointer);
    GLSTUB_DO(SYS_GL_CALL);
}

void glViewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GLSTUB_CALL(GL_FN_VIEWPORT, 4);
    _c.args[0] = (int64_t)(int32_t)x;
    _c.args[1] = (int64_t)(int32_t)y;
    _c.args[2] = (int64_t)(int32_t)width;
    _c.args[3] = (int64_t)(int32_t)height;
    GLSTUB_DO(SYS_GL_CALL);
}
