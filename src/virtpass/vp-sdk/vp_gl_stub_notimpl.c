/*
 * GENERATED FILE - produced by tools/gen_stub_notimpl.py - DO NOT EDIT BY HAND.
 *
 * "Not implemented" build variant of src/virtpass/vp_gl_stub.c.
 * Regenerate from the repository root:
 *     python tools/gen_stub_notimpl.py src/virtpass/vp_gl_stub.c --outdir src/virtpass/vp-sdk
 *
 * Every API function below keeps its original signature and reports itself on
 * stderr instead of issuing a hypercall to the host. Link a guest against this
 * file instead of the real stub to see which host APIs it actually asks for.
 */

/*
 * GENERATED FILE - produced by tools/gen_gl_abi.py - DO NOT EDIT BY HAND.
 *
 * Source of truth: NDK sysroot headers GLES2/gl2.h + GLES3/gl3.h + EGL/egl.h
 * (parsed; gl3.h is merged after gl2.h so the GLES2 ids never move).
 * Regenerate with:  python tools/gen_gl_abi.py
 *
 * ABI notes:
 *  - fn_id macros are the single source of truth shared by the guest stubs,
 *    src/virtpass/vp_cmdpost.c and both host GL dispatches.
 *  - gl_call.args has 12 slots. glTexSubImage3D (GLES3) needs 11 and
 *    glCompressedTexSubImage2D (GLES2) 9; the extra slot keeps
 *    GL_CALL_RETBUF_SLOT above every real parameter list. Guest and host are
 *    rebuilt together, so widening it is an internal ABI change only.
 *  - Floats travel bit-packed through the int64_t slots; pointers travel as
 *    guest virtual addresses. Guest memory is NOT mapped into the host, so
 *    the host dispatch translates every data pointer argument with
 *    rvvm_user_guest_ptr() and, for calls that hand back a host-owned string
 *    (glGetString/glGetStringi/eglQueryString), copies it through the guest
 *    scratch buffer offered in args[GL_CALL_RETBUF_SLOT].
 *  - Opaque host values (EGLDisplay/Config/Surface/Context, GLsync) are only
 *    passed back by the guest and never translated.
 *  - The overloaded pointer arguments (glVertexAttribPointer/IPointer,
 *    glDrawElements/Instanced, glDrawRangeElements) travel as their bare
 *    value; the host reads it as a byte offset when a buffer is bound to the
 *    matching target and as a guest address otherwise (vpgl_ptr()).
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
#include <stdio.h>       /* generator: stubs report via stderr */
#include <stddef.h>      /* generator: NULL / size_t */

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

/* ============================================================
 * Generated fallback reporter
 *
 * One line per distinct API is printed the first time it is reached: a guest
 * render loop calls these functions thousands of times per second, so
 * reporting every call would drown the host console in identical lines. The
 * dedup table compares the __func__ literals (stable per function); the race
 * between guest threads at worst repeats a line.
 * ============================================================ */
static void vp_stub_not_implemented(const char* api)
{
    static const char* reported[512];
    static unsigned     count = 0;
    unsigned            i;

    for (i = 0; i < count; i++) {
        if (reported[i] == api) {
            return;
        }
    }
    if (count < sizeof(reported) / sizeof(reported[0])) {
        reported[count++] = api;
    }
    fprintf(stderr, "[virtpass] not implemented: %s()\n", api);
}


/* Same ecall trampoline as vp_ndk_stub.c */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2,
                                   long a3, long a4, long a5)
{
    vp_stub_not_implemented(__func__);
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return 0;
}

/* Float <-> int64 bit packing (neither GLES2 nor GLES3 has double params).
 * Only packing is needed guest-side: no GL core function returns a float,
 * unpacking happens host-side via vpgl_arg_f(). */
static inline int64_t glstub_packf(float v)
{
    vp_stub_not_implemented(__func__);
    (void)v;
    return 0;
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
    { "glReadBuffer", (glstub_proc_t)&glReadBuffer },
    { "glDrawRangeElements", (glstub_proc_t)&glDrawRangeElements },
    { "glTexImage3D", (glstub_proc_t)&glTexImage3D },
    { "glTexSubImage3D", (glstub_proc_t)&glTexSubImage3D },
    { "glCopyTexSubImage3D", (glstub_proc_t)&glCopyTexSubImage3D },
    { "glCompressedTexImage3D", (glstub_proc_t)&glCompressedTexImage3D },
    { "glCompressedTexSubImage3D", (glstub_proc_t)&glCompressedTexSubImage3D },
    { "glGenQueries", (glstub_proc_t)&glGenQueries },
    { "glDeleteQueries", (glstub_proc_t)&glDeleteQueries },
    { "glIsQuery", (glstub_proc_t)&glIsQuery },
    { "glBeginQuery", (glstub_proc_t)&glBeginQuery },
    { "glEndQuery", (glstub_proc_t)&glEndQuery },
    { "glGetQueryiv", (glstub_proc_t)&glGetQueryiv },
    { "glGetQueryObjectuiv", (glstub_proc_t)&glGetQueryObjectuiv },
    { "glUnmapBuffer", (glstub_proc_t)&glUnmapBuffer },
    { "glGetBufferPointerv", (glstub_proc_t)&glGetBufferPointerv },
    { "glDrawBuffers", (glstub_proc_t)&glDrawBuffers },
    { "glUniformMatrix2x3fv", (glstub_proc_t)&glUniformMatrix2x3fv },
    { "glUniformMatrix3x2fv", (glstub_proc_t)&glUniformMatrix3x2fv },
    { "glUniformMatrix2x4fv", (glstub_proc_t)&glUniformMatrix2x4fv },
    { "glUniformMatrix4x2fv", (glstub_proc_t)&glUniformMatrix4x2fv },
    { "glUniformMatrix3x4fv", (glstub_proc_t)&glUniformMatrix3x4fv },
    { "glUniformMatrix4x3fv", (glstub_proc_t)&glUniformMatrix4x3fv },
    { "glBlitFramebuffer", (glstub_proc_t)&glBlitFramebuffer },
    { "glRenderbufferStorageMultisample", (glstub_proc_t)&glRenderbufferStorageMultisample },
    { "glFramebufferTextureLayer", (glstub_proc_t)&glFramebufferTextureLayer },
    { "glFlushMappedBufferRange", (glstub_proc_t)&glFlushMappedBufferRange },
    { "glBindVertexArray", (glstub_proc_t)&glBindVertexArray },
    { "glDeleteVertexArrays", (glstub_proc_t)&glDeleteVertexArrays },
    { "glGenVertexArrays", (glstub_proc_t)&glGenVertexArrays },
    { "glIsVertexArray", (glstub_proc_t)&glIsVertexArray },
    { "glGetIntegeri_v", (glstub_proc_t)&glGetIntegeri_v },
    { "glBeginTransformFeedback", (glstub_proc_t)&glBeginTransformFeedback },
    { "glEndTransformFeedback", (glstub_proc_t)&glEndTransformFeedback },
    { "glBindBufferRange", (glstub_proc_t)&glBindBufferRange },
    { "glBindBufferBase", (glstub_proc_t)&glBindBufferBase },
    { "glTransformFeedbackVaryings", (glstub_proc_t)&glTransformFeedbackVaryings },
    { "glGetTransformFeedbackVarying", (glstub_proc_t)&glGetTransformFeedbackVarying },
    { "glVertexAttribIPointer", (glstub_proc_t)&glVertexAttribIPointer },
    { "glGetVertexAttribIiv", (glstub_proc_t)&glGetVertexAttribIiv },
    { "glGetVertexAttribIuiv", (glstub_proc_t)&glGetVertexAttribIuiv },
    { "glVertexAttribI4i", (glstub_proc_t)&glVertexAttribI4i },
    { "glVertexAttribI4ui", (glstub_proc_t)&glVertexAttribI4ui },
    { "glVertexAttribI4iv", (glstub_proc_t)&glVertexAttribI4iv },
    { "glVertexAttribI4uiv", (glstub_proc_t)&glVertexAttribI4uiv },
    { "glGetUniformuiv", (glstub_proc_t)&glGetUniformuiv },
    { "glGetFragDataLocation", (glstub_proc_t)&glGetFragDataLocation },
    { "glUniform1ui", (glstub_proc_t)&glUniform1ui },
    { "glUniform2ui", (glstub_proc_t)&glUniform2ui },
    { "glUniform3ui", (glstub_proc_t)&glUniform3ui },
    { "glUniform4ui", (glstub_proc_t)&glUniform4ui },
    { "glUniform1uiv", (glstub_proc_t)&glUniform1uiv },
    { "glUniform2uiv", (glstub_proc_t)&glUniform2uiv },
    { "glUniform3uiv", (glstub_proc_t)&glUniform3uiv },
    { "glUniform4uiv", (glstub_proc_t)&glUniform4uiv },
    { "glClearBufferiv", (glstub_proc_t)&glClearBufferiv },
    { "glClearBufferuiv", (glstub_proc_t)&glClearBufferuiv },
    { "glClearBufferfv", (glstub_proc_t)&glClearBufferfv },
    { "glClearBufferfi", (glstub_proc_t)&glClearBufferfi },
    { "glGetStringi", (glstub_proc_t)&glGetStringi },
    { "glCopyBufferSubData", (glstub_proc_t)&glCopyBufferSubData },
    { "glGetUniformIndices", (glstub_proc_t)&glGetUniformIndices },
    { "glGetActiveUniformsiv", (glstub_proc_t)&glGetActiveUniformsiv },
    { "glGetUniformBlockIndex", (glstub_proc_t)&glGetUniformBlockIndex },
    { "glGetActiveUniformBlockiv", (glstub_proc_t)&glGetActiveUniformBlockiv },
    { "glGetActiveUniformBlockName", (glstub_proc_t)&glGetActiveUniformBlockName },
    { "glUniformBlockBinding", (glstub_proc_t)&glUniformBlockBinding },
    { "glDrawArraysInstanced", (glstub_proc_t)&glDrawArraysInstanced },
    { "glDrawElementsInstanced", (glstub_proc_t)&glDrawElementsInstanced },
    { "glFenceSync", (glstub_proc_t)&glFenceSync },
    { "glIsSync", (glstub_proc_t)&glIsSync },
    { "glDeleteSync", (glstub_proc_t)&glDeleteSync },
    { "glClientWaitSync", (glstub_proc_t)&glClientWaitSync },
    { "glWaitSync", (glstub_proc_t)&glWaitSync },
    { "glGetInteger64v", (glstub_proc_t)&glGetInteger64v },
    { "glGetSynciv", (glstub_proc_t)&glGetSynciv },
    { "glGetInteger64i_v", (glstub_proc_t)&glGetInteger64i_v },
    { "glGetBufferParameteri64v", (glstub_proc_t)&glGetBufferParameteri64v },
    { "glGenSamplers", (glstub_proc_t)&glGenSamplers },
    { "glDeleteSamplers", (glstub_proc_t)&glDeleteSamplers },
    { "glIsSampler", (glstub_proc_t)&glIsSampler },
    { "glBindSampler", (glstub_proc_t)&glBindSampler },
    { "glSamplerParameteri", (glstub_proc_t)&glSamplerParameteri },
    { "glSamplerParameteriv", (glstub_proc_t)&glSamplerParameteriv },
    { "glSamplerParameterf", (glstub_proc_t)&glSamplerParameterf },
    { "glSamplerParameterfv", (glstub_proc_t)&glSamplerParameterfv },
    { "glGetSamplerParameteriv", (glstub_proc_t)&glGetSamplerParameteriv },
    { "glGetSamplerParameterfv", (glstub_proc_t)&glGetSamplerParameterfv },
    { "glVertexAttribDivisor", (glstub_proc_t)&glVertexAttribDivisor },
    { "glBindTransformFeedback", (glstub_proc_t)&glBindTransformFeedback },
    { "glDeleteTransformFeedbacks", (glstub_proc_t)&glDeleteTransformFeedbacks },
    { "glGenTransformFeedbacks", (glstub_proc_t)&glGenTransformFeedbacks },
    { "glIsTransformFeedback", (glstub_proc_t)&glIsTransformFeedback },
    { "glPauseTransformFeedback", (glstub_proc_t)&glPauseTransformFeedback },
    { "glResumeTransformFeedback", (glstub_proc_t)&glResumeTransformFeedback },
    { "glGetProgramBinary", (glstub_proc_t)&glGetProgramBinary },
    { "glProgramBinary", (glstub_proc_t)&glProgramBinary },
    { "glProgramParameteri", (glstub_proc_t)&glProgramParameteri },
    { "glInvalidateFramebuffer", (glstub_proc_t)&glInvalidateFramebuffer },
    { "glInvalidateSubFramebuffer", (glstub_proc_t)&glInvalidateSubFramebuffer },
    { "glTexStorage2D", (glstub_proc_t)&glTexStorage2D },
    { "glTexStorage3D", (glstub_proc_t)&glTexStorage3D },
    { "glGetInternalformativ", (glstub_proc_t)&glGetInternalformativ },
    { "eglChooseConfig", (glstub_proc_t)&eglChooseConfig },
    { "eglCreateContext", (glstub_proc_t)&eglCreateContext },
    { "eglCreatePbufferSurface", (glstub_proc_t)&eglCreatePbufferSurface },
    { "eglCreateWindowSurface", (glstub_proc_t)&eglCreateWindowSurface },
    { "eglDestroyContext", (glstub_proc_t)&eglDestroyContext },
    { "eglDestroySurface", (glstub_proc_t)&eglDestroySurface },
    { "eglGetConfigAttrib", (glstub_proc_t)&eglGetConfigAttrib },
    { "eglGetCurrentDisplay", (glstub_proc_t)&eglGetCurrentDisplay },
    { "eglGetCurrentSurface", (glstub_proc_t)&eglGetCurrentSurface },
    { "eglGetDisplay", (glstub_proc_t)&eglGetDisplay },
    { "eglGetError", (glstub_proc_t)&eglGetError },
    { "eglInitialize", (glstub_proc_t)&eglInitialize },
    { "eglMakeCurrent", (glstub_proc_t)&eglMakeCurrent },
    { "eglQueryContext", (glstub_proc_t)&eglQueryContext },
    { "eglQueryString", (glstub_proc_t)&eglQueryString },
    { "eglQuerySurface", (glstub_proc_t)&eglQuerySurface },
    { "eglSwapBuffers", (glstub_proc_t)&eglSwapBuffers },
    { "eglTerminate", (glstub_proc_t)&eglTerminate },
    { "eglSwapInterval", (glstub_proc_t)&eglSwapInterval },
    { "eglBindAPI", (glstub_proc_t)&eglBindAPI },
    { "eglGetCurrentContext", (glstub_proc_t)&eglGetCurrentContext },
};

uint32_t eglChooseConfig(void* dpy, const int32_t* attrib_list, void* configs, int32_t config_size, int32_t* num_config)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)attrib_list;
    (void)configs;
    (void)config_size;
    (void)num_config;
    return 0;
}

void* eglCreateContext(void* dpy, void* config, void* share_context, const int32_t* attrib_list)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)attrib_list;
    return NULL;
}

void* eglCreatePbufferSurface(void* dpy, void* config, const int32_t* attrib_list)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)config;
    (void)attrib_list;
    return NULL;
}

void* eglCreateWindowSurface(void* dpy, void* config, void* win, const int32_t* attrib_list)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)config;
    (void)win;
    (void)attrib_list;
    return NULL;
}

uint32_t eglDestroyContext(void* dpy, void* ctx)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)ctx;
    return 0;
}

uint32_t eglDestroySurface(void* dpy, void* surface)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)surface;
    return 0;
}

uint32_t eglGetConfigAttrib(void* dpy, void* config, int32_t attribute, int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)config;
    (void)attribute;
    (void)value;
    return 0;
}

void* eglGetCurrentDisplay(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

void* eglGetCurrentSurface(int32_t readdraw)
{
    vp_stub_not_implemented(__func__);
    (void)readdraw;
    return NULL;
}

void* eglGetDisplay(void* display_id)
{
    vp_stub_not_implemented(__func__);
    (void)display_id;
    return NULL;
}

int32_t eglGetError(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

void* eglGetProcAddress(const char* procname)
{
    vp_stub_not_implemented(__func__);
    (void)procname;
    return NULL;
}

uint32_t eglInitialize(void* dpy, int32_t* major, int32_t* minor)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)major;
    (void)minor;
    return 0;
}

uint32_t eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)draw;
    (void)read;
    (void)ctx;
    return 0;
}

uint32_t eglQueryContext(void* dpy, void* ctx, int32_t attribute, int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)ctx;
    (void)attribute;
    (void)value;
    return 0;
}

const char* eglQueryString(void* dpy, int32_t name)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)name;
    return NULL;
}

uint32_t eglQuerySurface(void* dpy, void* surface, int32_t attribute, int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)surface;
    (void)attribute;
    (void)value;
    return 0;
}

uint32_t eglSwapBuffers(void* dpy, void* surface)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)surface;
    return 0;
}

uint32_t eglTerminate(void* dpy)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    return 0;
}

uint32_t eglSwapInterval(void* dpy, int32_t interval)
{
    vp_stub_not_implemented(__func__);
    (void)dpy;
    (void)interval;
    return 0;
}

uint32_t eglBindAPI(uint32_t api)
{
    vp_stub_not_implemented(__func__);
    (void)api;
    return 0;
}

void* eglGetCurrentContext(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

void glActiveTexture(uint32_t texture)
{
    vp_stub_not_implemented(__func__);
    (void)texture;
}

void glAttachShader(uint32_t program, uint32_t shader)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)shader;
}

void glBindAttribLocation(uint32_t program, uint32_t index, const char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)index;
    (void)name;
}

void glBindBuffer(uint32_t target, uint32_t buffer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)buffer;
}

void glBindFramebuffer(uint32_t target, uint32_t framebuffer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)framebuffer;
}

void glBindRenderbuffer(uint32_t target, uint32_t renderbuffer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)renderbuffer;
}

void glBindTexture(uint32_t target, uint32_t texture)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)texture;
}

void glBlendColor(float red, float green, float blue, float alpha)
{
    vp_stub_not_implemented(__func__);
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

void glBlendEquation(uint32_t mode)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
}

void glBlendEquationSeparate(uint32_t modeRGB, uint32_t modeAlpha)
{
    vp_stub_not_implemented(__func__);
    (void)modeRGB;
    (void)modeAlpha;
}

void glBlendFunc(uint32_t sfactor, uint32_t dfactor)
{
    vp_stub_not_implemented(__func__);
    (void)sfactor;
    (void)dfactor;
}

void glBlendFuncSeparate(uint32_t sfactorRGB, uint32_t dfactorRGB, uint32_t sfactorAlpha, uint32_t dfactorAlpha)
{
    vp_stub_not_implemented(__func__);
    (void)sfactorRGB;
    (void)dfactorRGB;
    (void)sfactorAlpha;
    (void)dfactorAlpha;
}

void glBufferData(uint32_t target, long size, const void* data, uint32_t usage)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)size;
    (void)data;
    (void)usage;
}

void glBufferSubData(uint32_t target, long offset, long size, const void* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)offset;
    (void)size;
    (void)data;
}

uint32_t glCheckFramebufferStatus(uint32_t target)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    return 0;
}

void glClear(uint32_t mask)
{
    vp_stub_not_implemented(__func__);
    (void)mask;
}

void glClearColor(float red, float green, float blue, float alpha)
{
    vp_stub_not_implemented(__func__);
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

void glClearDepthf(float d)
{
    vp_stub_not_implemented(__func__);
    (void)d;
}

void glClearStencil(int32_t s)
{
    vp_stub_not_implemented(__func__);
    (void)s;
}

void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    vp_stub_not_implemented(__func__);
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

void glCompileShader(uint32_t shader)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
}

void glCompressedTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t border, int32_t imageSize, const void* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)border;
    (void)imageSize;
    (void)data;
}

void glCompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, int32_t imageSize, const void* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)width;
    (void)height;
    (void)format;
    (void)imageSize;
    (void)data;
}

void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y, int32_t width, int32_t height, int32_t border)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)internalformat;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)border;
}

void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

uint32_t glCreateProgram(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

uint32_t glCreateShader(uint32_t type)
{
    vp_stub_not_implemented(__func__);
    (void)type;
    return 0;
}

void glCullFace(uint32_t mode)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
}

void glDeleteBuffers(int32_t n, const uint32_t* buffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)buffers;
}

void glDeleteFramebuffers(int32_t n, const uint32_t* framebuffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)framebuffers;
}

void glDeleteProgram(uint32_t program)
{
    vp_stub_not_implemented(__func__);
    (void)program;
}

void glDeleteRenderbuffers(int32_t n, const uint32_t* renderbuffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)renderbuffers;
}

void glDeleteShader(uint32_t shader)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
}

void glDeleteTextures(int32_t n, const uint32_t* textures)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)textures;
}

void glDepthFunc(uint32_t func)
{
    vp_stub_not_implemented(__func__);
    (void)func;
}

void glDepthMask(uint8_t flag)
{
    vp_stub_not_implemented(__func__);
    (void)flag;
}

void glDepthRangef(float n, float f)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)f;
}

void glDetachShader(uint32_t program, uint32_t shader)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)shader;
}

void glDisable(uint32_t cap)
{
    vp_stub_not_implemented(__func__);
    (void)cap;
}

void glDisableVertexAttribArray(uint32_t index)
{
    vp_stub_not_implemented(__func__);
    (void)index;
}

void glDrawArrays(uint32_t mode, int32_t first, int32_t count)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
    (void)first;
    (void)count;
}

void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* indices)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
}

void glEnable(uint32_t cap)
{
    vp_stub_not_implemented(__func__);
    (void)cap;
}

void glEnableVertexAttribArray(uint32_t index)
{
    vp_stub_not_implemented(__func__);
    (void)index;
}

void glFinish(void)
{
    vp_stub_not_implemented(__func__);
}

void glFlush(void)
{
    vp_stub_not_implemented(__func__);
}

void glFramebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbuffertarget, uint32_t renderbuffer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)attachment;
    (void)renderbuffertarget;
    (void)renderbuffer;
}

void glFramebufferTexture2D(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)attachment;
    (void)textarget;
    (void)texture;
    (void)level;
}

void glFrontFace(uint32_t mode)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
}

void glGenBuffers(int32_t n, uint32_t* buffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)buffers;
}

void glGenerateMipmap(uint32_t target)
{
    vp_stub_not_implemented(__func__);
    (void)target;
}

void glGenFramebuffers(int32_t n, uint32_t* framebuffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)framebuffers;
}

void glGenRenderbuffers(int32_t n, uint32_t* renderbuffers)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)renderbuffers;
}

void glGenTextures(int32_t n, uint32_t* textures)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)textures;
}

void glGetActiveAttrib(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)index;
    (void)bufSize;
    (void)length;
    (void)size;
    (void)type;
    (void)name;
}

void glGetActiveUniform(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)index;
    (void)bufSize;
    (void)length;
    (void)size;
    (void)type;
    (void)name;
}

void glGetAttachedShaders(uint32_t program, int32_t maxCount, int32_t* count, uint32_t* shaders)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)maxCount;
    (void)count;
    (void)shaders;
}

int32_t glGetAttribLocation(uint32_t program, const char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)name;
    return 0;
}

void glGetBooleanv(uint32_t pname, uint8_t* data)
{
    vp_stub_not_implemented(__func__);
    (void)pname;
    (void)data;
}

void glGetBufferParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

uint32_t glGetError(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

void glGetFloatv(uint32_t pname, float* data)
{
    vp_stub_not_implemented(__func__);
    (void)pname;
    (void)data;
}

void glGetFramebufferAttachmentParameteriv(uint32_t target, uint32_t attachment, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)attachment;
    (void)pname;
    (void)params;
}

void glGetIntegerv(uint32_t pname, int32_t* data)
{
    vp_stub_not_implemented(__func__);
    (void)pname;
    (void)data;
}

void glGetProgramiv(uint32_t program, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)pname;
    (void)params;
}

void glGetProgramInfoLog(uint32_t program, int32_t bufSize, int32_t* length, char* infoLog)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)bufSize;
    (void)length;
    (void)infoLog;
}

void glGetRenderbufferParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glGetShaderiv(uint32_t shader, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
    (void)pname;
    (void)params;
}

void glGetShaderInfoLog(uint32_t shader, int32_t bufSize, int32_t* length, char* infoLog)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
    (void)bufSize;
    (void)length;
    (void)infoLog;
}

void glGetShaderPrecisionFormat(uint32_t shadertype, uint32_t precisiontype, int32_t* range, int32_t* precision)
{
    vp_stub_not_implemented(__func__);
    (void)shadertype;
    (void)precisiontype;
    (void)range;
    (void)precision;
}

void glGetShaderSource(uint32_t shader, int32_t bufSize, int32_t* length, char* source)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
    (void)bufSize;
    (void)length;
    (void)source;
}

const uint8_t* glGetString(uint32_t name)
{
    vp_stub_not_implemented(__func__);
    (void)name;
    return NULL;
}

void glGetTexParameterfv(uint32_t target, uint32_t pname, float* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glGetUniformfv(uint32_t program, int32_t location, float* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)location;
    (void)params;
}

void glGetUniformiv(uint32_t program, int32_t location, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)location;
    (void)params;
}

int32_t glGetUniformLocation(uint32_t program, const char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)name;
    return 0;
}

void glGetVertexAttribfv(uint32_t index, uint32_t pname, float* params)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)pname;
    (void)params;
}

void glGetVertexAttribiv(uint32_t index, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)pname;
    (void)params;
}

void glGetVertexAttribPointerv(uint32_t index, uint32_t pname, void** pointer)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)pname;
    (void)pointer;
}

void glHint(uint32_t target, uint32_t mode)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)mode;
}

uint8_t glIsBuffer(uint32_t buffer)
{
    vp_stub_not_implemented(__func__);
    (void)buffer;
    return 0;
}

uint8_t glIsEnabled(uint32_t cap)
{
    vp_stub_not_implemented(__func__);
    (void)cap;
    return 0;
}

uint8_t glIsFramebuffer(uint32_t framebuffer)
{
    vp_stub_not_implemented(__func__);
    (void)framebuffer;
    return 0;
}

uint8_t glIsProgram(uint32_t program)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    return 0;
}

uint8_t glIsRenderbuffer(uint32_t renderbuffer)
{
    vp_stub_not_implemented(__func__);
    (void)renderbuffer;
    return 0;
}

uint8_t glIsShader(uint32_t shader)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
    return 0;
}

uint8_t glIsTexture(uint32_t texture)
{
    vp_stub_not_implemented(__func__);
    (void)texture;
    return 0;
}

void glLineWidth(float width)
{
    vp_stub_not_implemented(__func__);
    (void)width;
}

void glLinkProgram(uint32_t program)
{
    vp_stub_not_implemented(__func__);
    (void)program;
}

void glPixelStorei(uint32_t pname, int32_t param)
{
    vp_stub_not_implemented(__func__);
    (void)pname;
    (void)param;
}

void glPolygonOffset(float factor, float units)
{
    vp_stub_not_implemented(__func__);
    (void)factor;
    (void)units;
}

void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void* pixels)
{
    vp_stub_not_implemented(__func__);
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)pixels;
}

void glReleaseShaderCompiler(void)
{
    vp_stub_not_implemented(__func__);
}

void glRenderbufferStorage(uint32_t target, uint32_t internalformat, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)internalformat;
    (void)width;
    (void)height;
}

void glSampleCoverage(float value, uint8_t invert)
{
    vp_stub_not_implemented(__func__);
    (void)value;
    (void)invert;
}

void glScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void glShaderBinary(int32_t count, const uint32_t* shaders, uint32_t binaryformat, const void* binary, int32_t length)
{
    vp_stub_not_implemented(__func__);
    (void)count;
    (void)shaders;
    (void)binaryformat;
    (void)binary;
    (void)length;
}

void glShaderSource(uint32_t shader, int32_t count, const char** string, const int32_t* length)
{
    vp_stub_not_implemented(__func__);
    (void)shader;
    (void)count;
    (void)string;
    (void)length;
}

void glStencilFunc(uint32_t func, int32_t ref, uint32_t mask)
{
    vp_stub_not_implemented(__func__);
    (void)func;
    (void)ref;
    (void)mask;
}

void glStencilFuncSeparate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask)
{
    vp_stub_not_implemented(__func__);
    (void)face;
    (void)func;
    (void)ref;
    (void)mask;
}

void glStencilMask(uint32_t mask)
{
    vp_stub_not_implemented(__func__);
    (void)mask;
}

void glStencilMaskSeparate(uint32_t face, uint32_t mask)
{
    vp_stub_not_implemented(__func__);
    (void)face;
    (void)mask;
}

void glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    vp_stub_not_implemented(__func__);
    (void)fail;
    (void)zfail;
    (void)zpass;
}

void glStencilOpSeparate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass)
{
    vp_stub_not_implemented(__func__);
    (void)face;
    (void)sfail;
    (void)dpfail;
    (void)dppass;
}

void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void* pixels)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)border;
    (void)format;
    (void)type;
    (void)pixels;
}

void glTexParameterf(uint32_t target, uint32_t pname, float param)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)param;
}

void glTexParameterfv(uint32_t target, uint32_t pname, const float* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glTexParameteri(uint32_t target, uint32_t pname, int32_t param)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)param;
}

void glTexParameteriv(uint32_t target, uint32_t pname, const int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)pixels;
}

void glUniform1f(int32_t location, float v0)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
}

void glUniform1fv(int32_t location, int32_t count, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform1i(int32_t location, int32_t v0)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
}

void glUniform1iv(int32_t location, int32_t count, const int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform2f(int32_t location, float v0, float v1)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
}

void glUniform2fv(int32_t location, int32_t count, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform2i(int32_t location, int32_t v0, int32_t v1)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
}

void glUniform2iv(int32_t location, int32_t count, const int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform3f(int32_t location, float v0, float v1, float v2)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
}

void glUniform3fv(int32_t location, int32_t count, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform3i(int32_t location, int32_t v0, int32_t v1, int32_t v2)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
}

void glUniform3iv(int32_t location, int32_t count, const int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform4f(int32_t location, float v0, float v1, float v2, float v3)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
    (void)v3;
}

void glUniform4fv(int32_t location, int32_t count, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform4i(int32_t location, int32_t v0, int32_t v1, int32_t v2, int32_t v3)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
    (void)v3;
}

void glUniform4iv(int32_t location, int32_t count, const int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniformMatrix2fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix3fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix4fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUseProgram(uint32_t program)
{
    vp_stub_not_implemented(__func__);
    (void)program;
}

void glValidateProgram(uint32_t program)
{
    vp_stub_not_implemented(__func__);
    (void)program;
}

void glVertexAttrib1f(uint32_t index, float x)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
}

void glVertexAttrib1fv(uint32_t index, const float* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glVertexAttrib2f(uint32_t index, float x, float y)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
    (void)y;
}

void glVertexAttrib2fv(uint32_t index, const float* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glVertexAttrib3f(uint32_t index, float x, float y, float z)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
    (void)y;
    (void)z;
}

void glVertexAttrib3fv(uint32_t index, const float* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glVertexAttrib4f(uint32_t index, float x, float y, float z, float w)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
    (void)y;
    (void)z;
    (void)w;
}

void glVertexAttrib4fv(uint32_t index, const float* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glVertexAttribPointer(uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride, const void* pointer)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)size;
    (void)type;
    (void)normalized;
    (void)stride;
    (void)pointer;
}

void glViewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void glReadBuffer(uint32_t src)
{
    vp_stub_not_implemented(__func__);
    (void)src;
}

void glDrawRangeElements(uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, const void* indices)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
    (void)start;
    (void)end;
    (void)count;
    (void)type;
    (void)indices;
}

void glTexImage3D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, uint32_t format, uint32_t type, const void* pixels)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)depth;
    (void)border;
    (void)format;
    (void)type;
    (void)pixels;
}

void glTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, uint32_t type, const void* pixels)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)zoffset;
    (void)width;
    (void)height;
    (void)depth;
    (void)format;
    (void)type;
    (void)pixels;
}

void glCopyTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t x, int32_t y, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)zoffset;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void glCompressedTexImage3D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, int32_t imageSize, const void* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)depth;
    (void)border;
    (void)imageSize;
    (void)data;
}

void glCompressedTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, int32_t imageSize, const void* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)level;
    (void)xoffset;
    (void)yoffset;
    (void)zoffset;
    (void)width;
    (void)height;
    (void)depth;
    (void)format;
    (void)imageSize;
    (void)data;
}

void glGenQueries(int32_t n, uint32_t* ids)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)ids;
}

void glDeleteQueries(int32_t n, const uint32_t* ids)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)ids;
}

uint8_t glIsQuery(uint32_t id)
{
    vp_stub_not_implemented(__func__);
    (void)id;
    return 0;
}

void glBeginQuery(uint32_t target, uint32_t id)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)id;
}

void glEndQuery(uint32_t target)
{
    vp_stub_not_implemented(__func__);
    (void)target;
}

void glGetQueryiv(uint32_t target, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glGetQueryObjectuiv(uint32_t id, uint32_t pname, uint32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)id;
    (void)pname;
    (void)params;
}

uint8_t glUnmapBuffer(uint32_t target)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    return 0;
}

void glGetBufferPointerv(uint32_t target, uint32_t pname, void** params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glDrawBuffers(int32_t n, const uint32_t* bufs)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)bufs;
}

void glUniformMatrix2x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix3x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix2x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix4x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix3x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glUniformMatrix4x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)transpose;
    (void)value;
}

void glBlitFramebuffer(int32_t srcX0, int32_t srcY0, int32_t srcX1, int32_t srcY1, int32_t dstX0, int32_t dstY0, int32_t dstX1, int32_t dstY1, uint32_t mask, uint32_t filter)
{
    vp_stub_not_implemented(__func__);
    (void)srcX0;
    (void)srcY0;
    (void)srcX1;
    (void)srcY1;
    (void)dstX0;
    (void)dstY0;
    (void)dstX1;
    (void)dstY1;
    (void)mask;
    (void)filter;
}

void glRenderbufferStorageMultisample(uint32_t target, int32_t samples, uint32_t internalformat, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)samples;
    (void)internalformat;
    (void)width;
    (void)height;
}

void glFramebufferTextureLayer(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level, int32_t layer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)attachment;
    (void)texture;
    (void)level;
    (void)layer;
}

void glFlushMappedBufferRange(uint32_t target, long offset, long length)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)offset;
    (void)length;
}

void glBindVertexArray(uint32_t array)
{
    vp_stub_not_implemented(__func__);
    (void)array;
}

void glDeleteVertexArrays(int32_t n, const uint32_t* arrays)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)arrays;
}

void glGenVertexArrays(int32_t n, uint32_t* arrays)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)arrays;
}

uint8_t glIsVertexArray(uint32_t array)
{
    vp_stub_not_implemented(__func__);
    (void)array;
    return 0;
}

void glGetIntegeri_v(uint32_t target, uint32_t index, int32_t* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)index;
    (void)data;
}

void glBeginTransformFeedback(uint32_t primitiveMode)
{
    vp_stub_not_implemented(__func__);
    (void)primitiveMode;
}

void glEndTransformFeedback(void)
{
    vp_stub_not_implemented(__func__);
}

void glBindBufferRange(uint32_t target, uint32_t index, uint32_t buffer, long offset, long size)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)index;
    (void)buffer;
    (void)offset;
    (void)size;
}

void glBindBufferBase(uint32_t target, uint32_t index, uint32_t buffer)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)index;
    (void)buffer;
}

void glTransformFeedbackVaryings(uint32_t program, int32_t count, const char** varyings, uint32_t bufferMode)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)count;
    (void)varyings;
    (void)bufferMode;
}

void glGetTransformFeedbackVarying(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)index;
    (void)bufSize;
    (void)length;
    (void)size;
    (void)type;
    (void)name;
}

void glVertexAttribIPointer(uint32_t index, int32_t size, uint32_t type, int32_t stride, const void* pointer)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)size;
    (void)type;
    (void)stride;
    (void)pointer;
}

void glGetVertexAttribIiv(uint32_t index, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)pname;
    (void)params;
}

void glGetVertexAttribIuiv(uint32_t index, uint32_t pname, uint32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)pname;
    (void)params;
}

void glVertexAttribI4i(uint32_t index, int32_t x, int32_t y, int32_t z, int32_t w)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
    (void)y;
    (void)z;
    (void)w;
}

void glVertexAttribI4ui(uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)x;
    (void)y;
    (void)z;
    (void)w;
}

void glVertexAttribI4iv(uint32_t index, const int32_t* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glVertexAttribI4uiv(uint32_t index, const uint32_t* v)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)v;
}

void glGetUniformuiv(uint32_t program, int32_t location, uint32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)location;
    (void)params;
}

int32_t glGetFragDataLocation(uint32_t program, const char* name)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)name;
    return 0;
}

void glUniform1ui(int32_t location, uint32_t v0)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
}

void glUniform2ui(int32_t location, uint32_t v0, uint32_t v1)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
}

void glUniform3ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
}

void glUniform4ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)v0;
    (void)v1;
    (void)v2;
    (void)v3;
}

void glUniform1uiv(int32_t location, int32_t count, const uint32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform2uiv(int32_t location, int32_t count, const uint32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform3uiv(int32_t location, int32_t count, const uint32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glUniform4uiv(int32_t location, int32_t count, const uint32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)location;
    (void)count;
    (void)value;
}

void glClearBufferiv(uint32_t buffer, int32_t drawbuffer, const int32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)buffer;
    (void)drawbuffer;
    (void)value;
}

void glClearBufferuiv(uint32_t buffer, int32_t drawbuffer, const uint32_t* value)
{
    vp_stub_not_implemented(__func__);
    (void)buffer;
    (void)drawbuffer;
    (void)value;
}

void glClearBufferfv(uint32_t buffer, int32_t drawbuffer, const float* value)
{
    vp_stub_not_implemented(__func__);
    (void)buffer;
    (void)drawbuffer;
    (void)value;
}

void glClearBufferfi(uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil)
{
    vp_stub_not_implemented(__func__);
    (void)buffer;
    (void)drawbuffer;
    (void)depth;
    (void)stencil;
}

const uint8_t* glGetStringi(uint32_t name, uint32_t index)
{
    vp_stub_not_implemented(__func__);
    (void)name;
    (void)index;
    return NULL;
}

void glCopyBufferSubData(uint32_t readTarget, uint32_t writeTarget, long readOffset, long writeOffset, long size)
{
    vp_stub_not_implemented(__func__);
    (void)readTarget;
    (void)writeTarget;
    (void)readOffset;
    (void)writeOffset;
    (void)size;
}

void glGetUniformIndices(uint32_t program, int32_t uniformCount, const char** uniformNames, uint32_t* uniformIndices)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformCount;
    (void)uniformNames;
    (void)uniformIndices;
}

void glGetActiveUniformsiv(uint32_t program, int32_t uniformCount, const uint32_t* uniformIndices, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformCount;
    (void)uniformIndices;
    (void)pname;
    (void)params;
}

uint32_t glGetUniformBlockIndex(uint32_t program, const char* uniformBlockName)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformBlockName;
    return 0;
}

void glGetActiveUniformBlockiv(uint32_t program, uint32_t uniformBlockIndex, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformBlockIndex;
    (void)pname;
    (void)params;
}

void glGetActiveUniformBlockName(uint32_t program, uint32_t uniformBlockIndex, int32_t bufSize, int32_t* length, char* uniformBlockName)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformBlockIndex;
    (void)bufSize;
    (void)length;
    (void)uniformBlockName;
}

void glUniformBlockBinding(uint32_t program, uint32_t uniformBlockIndex, uint32_t uniformBlockBinding)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)uniformBlockIndex;
    (void)uniformBlockBinding;
}

void glDrawArraysInstanced(uint32_t mode, int32_t first, int32_t count, int32_t instancecount)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
    (void)first;
    (void)count;
    (void)instancecount;
}

void glDrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type, const void* indices, int32_t instancecount)
{
    vp_stub_not_implemented(__func__);
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
    (void)instancecount;
}

void* glFenceSync(uint32_t condition, uint32_t flags)
{
    vp_stub_not_implemented(__func__);
    (void)condition;
    (void)flags;
    return NULL;
}

uint8_t glIsSync(void* sync)
{
    vp_stub_not_implemented(__func__);
    (void)sync;
    return 0;
}

void glDeleteSync(void* sync)
{
    vp_stub_not_implemented(__func__);
    (void)sync;
}

uint32_t glClientWaitSync(void* sync, uint32_t flags, uint64_t timeout)
{
    vp_stub_not_implemented(__func__);
    (void)sync;
    (void)flags;
    (void)timeout;
    return 0;
}

void glWaitSync(void* sync, uint32_t flags, uint64_t timeout)
{
    vp_stub_not_implemented(__func__);
    (void)sync;
    (void)flags;
    (void)timeout;
}

void glGetInteger64v(uint32_t pname, int64_t* data)
{
    vp_stub_not_implemented(__func__);
    (void)pname;
    (void)data;
}

void glGetSynciv(void* sync, uint32_t pname, int32_t bufSize, int32_t* length, int32_t* values)
{
    vp_stub_not_implemented(__func__);
    (void)sync;
    (void)pname;
    (void)bufSize;
    (void)length;
    (void)values;
}

void glGetInteger64i_v(uint32_t target, uint32_t index, int64_t* data)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)index;
    (void)data;
}

void glGetBufferParameteri64v(uint32_t target, uint32_t pname, int64_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)pname;
    (void)params;
}

void glGenSamplers(int32_t count, uint32_t* samplers)
{
    vp_stub_not_implemented(__func__);
    (void)count;
    (void)samplers;
}

void glDeleteSamplers(int32_t count, const uint32_t* samplers)
{
    vp_stub_not_implemented(__func__);
    (void)count;
    (void)samplers;
}

uint8_t glIsSampler(uint32_t sampler)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    return 0;
}

void glBindSampler(uint32_t unit, uint32_t sampler)
{
    vp_stub_not_implemented(__func__);
    (void)unit;
    (void)sampler;
}

void glSamplerParameteri(uint32_t sampler, uint32_t pname, int32_t param)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)param;
}

void glSamplerParameteriv(uint32_t sampler, uint32_t pname, const int32_t* param)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)param;
}

void glSamplerParameterf(uint32_t sampler, uint32_t pname, float param)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)param;
}

void glSamplerParameterfv(uint32_t sampler, uint32_t pname, const float* param)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)param;
}

void glGetSamplerParameteriv(uint32_t sampler, uint32_t pname, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)params;
}

void glGetSamplerParameterfv(uint32_t sampler, uint32_t pname, float* params)
{
    vp_stub_not_implemented(__func__);
    (void)sampler;
    (void)pname;
    (void)params;
}

void glVertexAttribDivisor(uint32_t index, uint32_t divisor)
{
    vp_stub_not_implemented(__func__);
    (void)index;
    (void)divisor;
}

void glBindTransformFeedback(uint32_t target, uint32_t id)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)id;
}

void glDeleteTransformFeedbacks(int32_t n, const uint32_t* ids)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)ids;
}

void glGenTransformFeedbacks(int32_t n, uint32_t* ids)
{
    vp_stub_not_implemented(__func__);
    (void)n;
    (void)ids;
}

uint8_t glIsTransformFeedback(uint32_t id)
{
    vp_stub_not_implemented(__func__);
    (void)id;
    return 0;
}

void glPauseTransformFeedback(void)
{
    vp_stub_not_implemented(__func__);
}

void glResumeTransformFeedback(void)
{
    vp_stub_not_implemented(__func__);
}

void glGetProgramBinary(uint32_t program, int32_t bufSize, int32_t* length, uint32_t* binaryFormat, void* binary)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)bufSize;
    (void)length;
    (void)binaryFormat;
    (void)binary;
}

void glProgramBinary(uint32_t program, uint32_t binaryFormat, const void* binary, int32_t length)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)binaryFormat;
    (void)binary;
    (void)length;
}

void glProgramParameteri(uint32_t program, uint32_t pname, int32_t value)
{
    vp_stub_not_implemented(__func__);
    (void)program;
    (void)pname;
    (void)value;
}

void glInvalidateFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)numAttachments;
    (void)attachments;
}

void glInvalidateSubFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments, int32_t x, int32_t y, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)numAttachments;
    (void)attachments;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void glTexStorage2D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)levels;
    (void)internalformat;
    (void)width;
    (void)height;
}

void glTexStorage3D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height, int32_t depth)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)levels;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)depth;
}

void glGetInternalformativ(uint32_t target, uint32_t internalformat, uint32_t pname, int32_t bufSize, int32_t* params)
{
    vp_stub_not_implemented(__func__);
    (void)target;
    (void)internalformat;
    (void)pname;
    (void)bufSize;
    (void)params;
}
