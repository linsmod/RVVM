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

#ifndef VPGL_HOST_TYPES_H
#define VPGL_HOST_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Host-side GL/EGL function types matching the real EGL/GLES
 * implementations (win32: SwiftShader / ANGLE from the Android SDK
 * emulator directory; android: the system libEGL/libGLESv2).
 * All types are vpgl_-prefixed so this header never collides with
 * real GL/EGL headers.
 */

#define vpgl_APIENTRY

/* ---- type definitions ---- */
typedef void          vpgl_void;
typedef char          vpgl_char;
typedef unsigned int  vpgl_GLenum;
typedef unsigned int  vpgl_GLuint;
typedef unsigned int  vpgl_GLbitfield;
typedef int           vpgl_GLint;
typedef int           vpgl_GLsizei;
typedef char          vpgl_GLchar;
typedef unsigned char vpgl_GLboolean;
typedef signed char   vpgl_GLbyte;
typedef unsigned char vpgl_GLubyte;
typedef short         vpgl_GLshort;
typedef unsigned short vpgl_GLushort;
typedef float         vpgl_GLfloat;
typedef float         vpgl_GLclampf;
typedef ptrdiff_t     vpgl_GLintptr;
typedef ptrdiff_t     vpgl_GLsizeiptr;
typedef void*         vpgl_EGLDisplay;
typedef void*         vpgl_EGLSurface;
typedef void*         vpgl_EGLContext;
typedef void*         vpgl_EGLConfig;
typedef int           vpgl_EGLint;
typedef unsigned int  vpgl_EGLBoolean;
typedef unsigned int  vpgl_EGLenum;
/* Native window handle behind EGLNativeWindowType. The win32 host
 * never constructs one (its window surface downgrades to a pbuffer),
 * but the android host passes its real ANativeWindow through here. */
typedef void*         vpgl_EGLNativeWindowType;

/* EGL attribute tokens the host dispatch inspects directly (surface
 * attributes are a guest array it walks itself). */
#define vpgl_EGL_NONE   0x3038
#define vpgl_EGL_WIDTH  0x3057
#define vpgl_EGL_HEIGHT 0x3056

/* ---- EGL function pointer types ---- */
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglChooseConfig)(vpgl_void* dpy, const vpgl_EGLint* attrib_list, vpgl_void* configs, vpgl_EGLint config_size, vpgl_EGLint* num_config);
typedef vpgl_void* (vpgl_APIENTRY *vpgl_PFN_eglCreateContext)(vpgl_void* dpy, vpgl_void* config, vpgl_void* share_context, const vpgl_EGLint* attrib_list);
typedef vpgl_void* (vpgl_APIENTRY *vpgl_PFN_eglCreatePbufferSurface)(vpgl_void* dpy, vpgl_void* config, const vpgl_EGLint* attrib_list);
typedef vpgl_void* (vpgl_APIENTRY *vpgl_PFN_eglCreateWindowSurface)(vpgl_void* dpy, vpgl_void* config, vpgl_void* win, const vpgl_EGLint* attrib_list);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglDestroyContext)(vpgl_void* dpy, vpgl_void* ctx);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglDestroySurface)(vpgl_void* dpy, vpgl_void* surface);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglGetConfigAttrib)(vpgl_void* dpy, vpgl_void* config, vpgl_EGLint attribute, vpgl_EGLint* value);
typedef vpgl_void* (vpgl_APIENTRY *vpgl_PFN_eglGetDisplay)(vpgl_void* display_id);
typedef vpgl_EGLint (vpgl_APIENTRY *vpgl_PFN_eglGetError)(void);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglInitialize)(vpgl_void* dpy, vpgl_EGLint* major, vpgl_EGLint* minor);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglMakeCurrent)(vpgl_void* dpy, vpgl_void* draw, vpgl_void* read, vpgl_void* ctx);
typedef const vpgl_char* (vpgl_APIENTRY *vpgl_PFN_eglQueryString)(vpgl_void* dpy, vpgl_EGLint name);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglQuerySurface)(vpgl_void* dpy, vpgl_void* surface, vpgl_EGLint attribute, vpgl_EGLint* value);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglSwapBuffers)(vpgl_void* dpy, vpgl_void* surface);
typedef vpgl_EGLBoolean (vpgl_APIENTRY *vpgl_PFN_eglTerminate)(vpgl_void* dpy);

/* ---- GLES2 function pointer types ---- */
typedef void (vpgl_APIENTRY *vpgl_PFN_glActiveTexture)(vpgl_GLenum texture);
typedef void (vpgl_APIENTRY *vpgl_PFN_glAttachShader)(vpgl_GLuint program, vpgl_GLuint shader);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBindAttribLocation)(vpgl_GLuint program, vpgl_GLuint index, const vpgl_GLchar* name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBindBuffer)(vpgl_GLenum target, vpgl_GLuint buffer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBindFramebuffer)(vpgl_GLenum target, vpgl_GLuint framebuffer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBindRenderbuffer)(vpgl_GLenum target, vpgl_GLuint renderbuffer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBindTexture)(vpgl_GLenum target, vpgl_GLuint texture);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBlendColor)(vpgl_GLfloat red, vpgl_GLfloat green, vpgl_GLfloat blue, vpgl_GLfloat alpha);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBlendEquation)(vpgl_GLenum mode);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBlendEquationSeparate)(vpgl_GLenum modeRGB, vpgl_GLenum modeAlpha);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBlendFunc)(vpgl_GLenum sfactor, vpgl_GLenum dfactor);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBlendFuncSeparate)(vpgl_GLenum sfactorRGB, vpgl_GLenum dfactorRGB, vpgl_GLenum sfactorAlpha, vpgl_GLenum dfactorAlpha);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBufferData)(vpgl_GLenum target, vpgl_GLsizeiptr size, const vpgl_void* data, vpgl_GLenum usage);
typedef void (vpgl_APIENTRY *vpgl_PFN_glBufferSubData)(vpgl_GLenum target, vpgl_GLintptr offset, vpgl_GLsizeiptr size, const vpgl_void* data);
typedef vpgl_GLenum (vpgl_APIENTRY *vpgl_PFN_glCheckFramebufferStatus)(vpgl_GLenum target);
typedef void (vpgl_APIENTRY *vpgl_PFN_glClear)(vpgl_GLbitfield mask);
typedef void (vpgl_APIENTRY *vpgl_PFN_glClearColor)(vpgl_GLfloat red, vpgl_GLfloat green, vpgl_GLfloat blue, vpgl_GLfloat alpha);
typedef void (vpgl_APIENTRY *vpgl_PFN_glClearDepthf)(vpgl_GLfloat d);
typedef void (vpgl_APIENTRY *vpgl_PFN_glClearStencil)(vpgl_GLint s);
typedef void (vpgl_APIENTRY *vpgl_PFN_glColorMask)(vpgl_GLboolean red, vpgl_GLboolean green, vpgl_GLboolean blue, vpgl_GLboolean alpha);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCompileShader)(vpgl_GLuint shader);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCompressedTexImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLenum internalformat, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLint border, vpgl_GLsizei imageSize, const vpgl_void* data);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCompressedTexSubImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLint xoffset, vpgl_GLint yoffset, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLenum format, vpgl_GLsizei imageSize, const vpgl_void* data);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCopyTexImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLenum internalformat, vpgl_GLint x, vpgl_GLint y, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLint border);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCopyTexSubImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLint xoffset, vpgl_GLint yoffset, vpgl_GLint x, vpgl_GLint y, vpgl_GLsizei width, vpgl_GLsizei height);
typedef vpgl_GLuint (vpgl_APIENTRY *vpgl_PFN_glCreateProgram)(void);
typedef vpgl_GLuint (vpgl_APIENTRY *vpgl_PFN_glCreateShader)(vpgl_GLenum type);
typedef void (vpgl_APIENTRY *vpgl_PFN_glCullFace)(vpgl_GLenum mode);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteBuffers)(vpgl_GLsizei n, const vpgl_GLuint* buffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteFramebuffers)(vpgl_GLsizei n, const vpgl_GLuint* framebuffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteProgram)(vpgl_GLuint program);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteRenderbuffers)(vpgl_GLsizei n, const vpgl_GLuint* renderbuffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteShader)(vpgl_GLuint shader);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDeleteTextures)(vpgl_GLsizei n, const vpgl_GLuint* textures);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDepthFunc)(vpgl_GLenum func);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDepthMask)(vpgl_GLboolean flag);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDepthRangef)(vpgl_GLfloat n, vpgl_GLfloat f);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDetachShader)(vpgl_GLuint program, vpgl_GLuint shader);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDisable)(vpgl_GLenum cap);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDisableVertexAttribArray)(vpgl_GLuint index);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDrawArrays)(vpgl_GLenum mode, vpgl_GLint first, vpgl_GLsizei count);
typedef void (vpgl_APIENTRY *vpgl_PFN_glDrawElements)(vpgl_GLenum mode, vpgl_GLsizei count, vpgl_GLenum type, const vpgl_void* indices);
typedef void (vpgl_APIENTRY *vpgl_PFN_glEnable)(vpgl_GLenum cap);
typedef void (vpgl_APIENTRY *vpgl_PFN_glEnableVertexAttribArray)(vpgl_GLuint index);
typedef void (vpgl_APIENTRY *vpgl_PFN_glFinish)(void);
typedef void (vpgl_APIENTRY *vpgl_PFN_glFlush)(void);
typedef void (vpgl_APIENTRY *vpgl_PFN_glFramebufferRenderbuffer)(vpgl_GLenum target, vpgl_GLenum attachment, vpgl_GLenum renderbuffertarget, vpgl_GLuint renderbuffer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glFramebufferTexture2D)(vpgl_GLenum target, vpgl_GLenum attachment, vpgl_GLenum textarget, vpgl_GLuint texture, vpgl_GLint level);
typedef void (vpgl_APIENTRY *vpgl_PFN_glFrontFace)(vpgl_GLenum mode);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGenBuffers)(vpgl_GLsizei n, vpgl_GLuint* buffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGenerateMipmap)(vpgl_GLenum target);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGenFramebuffers)(vpgl_GLsizei n, vpgl_GLuint* framebuffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGenRenderbuffers)(vpgl_GLsizei n, vpgl_GLuint* renderbuffers);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGenTextures)(vpgl_GLsizei n, vpgl_GLuint* textures);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetActiveAttrib)(vpgl_GLuint program, vpgl_GLuint index, vpgl_GLsizei bufSize, vpgl_GLsizei* length, vpgl_GLint* size, vpgl_GLenum* type, vpgl_GLchar* name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetActiveUniform)(vpgl_GLuint program, vpgl_GLuint index, vpgl_GLsizei bufSize, vpgl_GLsizei* length, vpgl_GLint* size, vpgl_GLenum* type, vpgl_GLchar* name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetAttachedShaders)(vpgl_GLuint program, vpgl_GLsizei maxCount, vpgl_GLsizei* count, vpgl_GLuint* shaders);
typedef vpgl_GLint (vpgl_APIENTRY *vpgl_PFN_glGetAttribLocation)(vpgl_GLuint program, const vpgl_GLchar* name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetBooleanv)(vpgl_GLenum pname, vpgl_GLboolean* data);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetBufferParameteriv)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLint* params);
typedef vpgl_GLenum (vpgl_APIENTRY *vpgl_PFN_glGetError)(void);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetFloatv)(vpgl_GLenum pname, vpgl_GLfloat* data);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetFramebufferAttachmentParameteriv)(vpgl_GLenum target, vpgl_GLenum attachment, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetIntegerv)(vpgl_GLenum pname, vpgl_GLint* data);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetProgramiv)(vpgl_GLuint program, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetProgramInfoLog)(vpgl_GLuint program, vpgl_GLsizei bufSize, vpgl_GLsizei* length, vpgl_GLchar* infoLog);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetRenderbufferParameteriv)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetShaderiv)(vpgl_GLuint shader, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetShaderInfoLog)(vpgl_GLuint shader, vpgl_GLsizei bufSize, vpgl_GLsizei* length, vpgl_GLchar* infoLog);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetShaderPrecisionFormat)(vpgl_GLenum shadertype, vpgl_GLenum precisiontype, vpgl_GLint* range, vpgl_GLint* precision);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetShaderSource)(vpgl_GLuint shader, vpgl_GLsizei bufSize, vpgl_GLsizei* length, vpgl_GLchar* source);
typedef const vpgl_GLubyte* (vpgl_APIENTRY *vpgl_PFN_glGetString)(vpgl_GLenum name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetTexParameterfv)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLfloat* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetTexParameteriv)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetUniformfv)(vpgl_GLuint program, vpgl_GLint location, vpgl_GLfloat* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetUniformiv)(vpgl_GLuint program, vpgl_GLint location, vpgl_GLint* params);
typedef vpgl_GLint (vpgl_APIENTRY *vpgl_PFN_glGetUniformLocation)(vpgl_GLuint program, const vpgl_GLchar* name);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetVertexAttribfv)(vpgl_GLuint index, vpgl_GLenum pname, vpgl_GLfloat* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetVertexAttribiv)(vpgl_GLuint index, vpgl_GLenum pname, vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glGetVertexAttribPointerv)(vpgl_GLuint index, vpgl_GLenum pname, vpgl_void** pointer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glHint)(vpgl_GLenum target, vpgl_GLenum mode);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsBuffer)(vpgl_GLuint buffer);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsEnabled)(vpgl_GLenum cap);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsFramebuffer)(vpgl_GLuint framebuffer);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsProgram)(vpgl_GLuint program);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsRenderbuffer)(vpgl_GLuint renderbuffer);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsShader)(vpgl_GLuint shader);
typedef vpgl_GLboolean (vpgl_APIENTRY *vpgl_PFN_glIsTexture)(vpgl_GLuint texture);
typedef void (vpgl_APIENTRY *vpgl_PFN_glLineWidth)(vpgl_GLfloat width);
typedef void (vpgl_APIENTRY *vpgl_PFN_glLinkProgram)(vpgl_GLuint program);
typedef void (vpgl_APIENTRY *vpgl_PFN_glPixelStorei)(vpgl_GLenum pname, vpgl_GLint param);
typedef void (vpgl_APIENTRY *vpgl_PFN_glPolygonOffset)(vpgl_GLfloat factor, vpgl_GLfloat units);
typedef void (vpgl_APIENTRY *vpgl_PFN_glReadPixels)(vpgl_GLint x, vpgl_GLint y, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLenum format, vpgl_GLenum type, vpgl_void* pixels);
typedef void (vpgl_APIENTRY *vpgl_PFN_glReleaseShaderCompiler)(void);
typedef void (vpgl_APIENTRY *vpgl_PFN_glRenderbufferStorage)(vpgl_GLenum target, vpgl_GLenum internalformat, vpgl_GLsizei width, vpgl_GLsizei height);
typedef void (vpgl_APIENTRY *vpgl_PFN_glSampleCoverage)(vpgl_GLfloat value, vpgl_GLboolean invert);
typedef void (vpgl_APIENTRY *vpgl_PFN_glScissor)(vpgl_GLint x, vpgl_GLint y, vpgl_GLsizei width, vpgl_GLsizei height);
typedef void (vpgl_APIENTRY *vpgl_PFN_glShaderBinary)(vpgl_GLsizei count, const vpgl_GLuint* shaders, vpgl_GLenum binaryformat, const vpgl_void* binary, vpgl_GLsizei length);
typedef void (vpgl_APIENTRY *vpgl_PFN_glShaderSource)(vpgl_GLuint shader, vpgl_GLsizei count, const vpgl_GLchar** string, const vpgl_GLint* length);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilFunc)(vpgl_GLenum func, vpgl_GLint ref, vpgl_GLuint mask);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilFuncSeparate)(vpgl_GLenum face, vpgl_GLenum func, vpgl_GLint ref, vpgl_GLuint mask);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilMask)(vpgl_GLuint mask);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilMaskSeparate)(vpgl_GLenum face, vpgl_GLuint mask);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilOp)(vpgl_GLenum fail, vpgl_GLenum zfail, vpgl_GLenum zpass);
typedef void (vpgl_APIENTRY *vpgl_PFN_glStencilOpSeparate)(vpgl_GLenum face, vpgl_GLenum sfail, vpgl_GLenum dpfail, vpgl_GLenum dppass);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLint internalformat, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLint border, vpgl_GLenum format, vpgl_GLenum type, const vpgl_void* pixels);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexParameterf)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLfloat param);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexParameterfv)(vpgl_GLenum target, vpgl_GLenum pname, const vpgl_GLfloat* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexParameteri)(vpgl_GLenum target, vpgl_GLenum pname, vpgl_GLint param);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexParameteriv)(vpgl_GLenum target, vpgl_GLenum pname, const vpgl_GLint* params);
typedef void (vpgl_APIENTRY *vpgl_PFN_glTexSubImage2D)(vpgl_GLenum target, vpgl_GLint level, vpgl_GLint xoffset, vpgl_GLint yoffset, vpgl_GLsizei width, vpgl_GLsizei height, vpgl_GLenum format, vpgl_GLenum type, const vpgl_void* pixels);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform1f)(vpgl_GLint location, vpgl_GLfloat v0);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform1fv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform1i)(vpgl_GLint location, vpgl_GLint v0);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform1iv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLint* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform2f)(vpgl_GLint location, vpgl_GLfloat v0, vpgl_GLfloat v1);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform2fv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform2i)(vpgl_GLint location, vpgl_GLint v0, vpgl_GLint v1);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform2iv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLint* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform3f)(vpgl_GLint location, vpgl_GLfloat v0, vpgl_GLfloat v1, vpgl_GLfloat v2);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform3fv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform3i)(vpgl_GLint location, vpgl_GLint v0, vpgl_GLint v1, vpgl_GLint v2);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform3iv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLint* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform4f)(vpgl_GLint location, vpgl_GLfloat v0, vpgl_GLfloat v1, vpgl_GLfloat v2, vpgl_GLfloat v3);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform4fv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform4i)(vpgl_GLint location, vpgl_GLint v0, vpgl_GLint v1, vpgl_GLint v2, vpgl_GLint v3);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniform4iv)(vpgl_GLint location, vpgl_GLsizei count, const vpgl_GLint* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniformMatrix2fv)(vpgl_GLint location, vpgl_GLsizei count, vpgl_GLboolean transpose, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniformMatrix3fv)(vpgl_GLint location, vpgl_GLsizei count, vpgl_GLboolean transpose, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUniformMatrix4fv)(vpgl_GLint location, vpgl_GLsizei count, vpgl_GLboolean transpose, const vpgl_GLfloat* value);
typedef void (vpgl_APIENTRY *vpgl_PFN_glUseProgram)(vpgl_GLuint program);
typedef void (vpgl_APIENTRY *vpgl_PFN_glValidateProgram)(vpgl_GLuint program);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib1f)(vpgl_GLuint index, vpgl_GLfloat x);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib1fv)(vpgl_GLuint index, const vpgl_GLfloat* v);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib2f)(vpgl_GLuint index, vpgl_GLfloat x, vpgl_GLfloat y);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib2fv)(vpgl_GLuint index, const vpgl_GLfloat* v);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib3f)(vpgl_GLuint index, vpgl_GLfloat x, vpgl_GLfloat y, vpgl_GLfloat z);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib3fv)(vpgl_GLuint index, const vpgl_GLfloat* v);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib4f)(vpgl_GLuint index, vpgl_GLfloat x, vpgl_GLfloat y, vpgl_GLfloat z, vpgl_GLfloat w);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttrib4fv)(vpgl_GLuint index, const vpgl_GLfloat* v);
typedef void (vpgl_APIENTRY *vpgl_PFN_glVertexAttribPointer)(vpgl_GLuint index, vpgl_GLint size, vpgl_GLenum type, vpgl_GLboolean normalized, vpgl_GLsizei stride, const vpgl_void* pointer);
typedef void (vpgl_APIENTRY *vpgl_PFN_glViewport)(vpgl_GLint x, vpgl_GLint y, vpgl_GLsizei width, vpgl_GLsizei height);

/* ---- resolved entry points (NULL when missing) ---- */
extern vpgl_PFN_eglChooseConfig p_eglChooseConfig;
extern vpgl_PFN_eglCreateContext p_eglCreateContext;
extern vpgl_PFN_eglCreatePbufferSurface p_eglCreatePbufferSurface;
extern vpgl_PFN_eglCreateWindowSurface p_eglCreateWindowSurface;
extern vpgl_PFN_eglDestroyContext p_eglDestroyContext;
extern vpgl_PFN_eglDestroySurface p_eglDestroySurface;
extern vpgl_PFN_eglGetConfigAttrib p_eglGetConfigAttrib;
extern vpgl_PFN_eglGetDisplay p_eglGetDisplay;
extern vpgl_PFN_eglGetError p_eglGetError;
extern vpgl_PFN_eglInitialize p_eglInitialize;
extern vpgl_PFN_eglMakeCurrent p_eglMakeCurrent;
extern vpgl_PFN_eglQueryString p_eglQueryString;
extern vpgl_PFN_eglQuerySurface p_eglQuerySurface;
extern vpgl_PFN_eglSwapBuffers p_eglSwapBuffers;
extern vpgl_PFN_eglTerminate p_eglTerminate;

extern vpgl_PFN_glActiveTexture p_glActiveTexture;
extern vpgl_PFN_glAttachShader p_glAttachShader;
extern vpgl_PFN_glBindAttribLocation p_glBindAttribLocation;
extern vpgl_PFN_glBindBuffer p_glBindBuffer;
extern vpgl_PFN_glBindFramebuffer p_glBindFramebuffer;
extern vpgl_PFN_glBindRenderbuffer p_glBindRenderbuffer;
extern vpgl_PFN_glBindTexture p_glBindTexture;
extern vpgl_PFN_glBlendColor p_glBlendColor;
extern vpgl_PFN_glBlendEquation p_glBlendEquation;
extern vpgl_PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
extern vpgl_PFN_glBlendFunc p_glBlendFunc;
extern vpgl_PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
extern vpgl_PFN_glBufferData p_glBufferData;
extern vpgl_PFN_glBufferSubData p_glBufferSubData;
extern vpgl_PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
extern vpgl_PFN_glClear p_glClear;
extern vpgl_PFN_glClearColor p_glClearColor;
extern vpgl_PFN_glClearDepthf p_glClearDepthf;
extern vpgl_PFN_glClearStencil p_glClearStencil;
extern vpgl_PFN_glColorMask p_glColorMask;
extern vpgl_PFN_glCompileShader p_glCompileShader;
extern vpgl_PFN_glCompressedTexImage2D p_glCompressedTexImage2D;
extern vpgl_PFN_glCompressedTexSubImage2D p_glCompressedTexSubImage2D;
extern vpgl_PFN_glCopyTexImage2D p_glCopyTexImage2D;
extern vpgl_PFN_glCopyTexSubImage2D p_glCopyTexSubImage2D;
extern vpgl_PFN_glCreateProgram p_glCreateProgram;
extern vpgl_PFN_glCreateShader p_glCreateShader;
extern vpgl_PFN_glCullFace p_glCullFace;
extern vpgl_PFN_glDeleteBuffers p_glDeleteBuffers;
extern vpgl_PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
extern vpgl_PFN_glDeleteProgram p_glDeleteProgram;
extern vpgl_PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
extern vpgl_PFN_glDeleteShader p_glDeleteShader;
extern vpgl_PFN_glDeleteTextures p_glDeleteTextures;
extern vpgl_PFN_glDepthFunc p_glDepthFunc;
extern vpgl_PFN_glDepthMask p_glDepthMask;
extern vpgl_PFN_glDepthRangef p_glDepthRangef;
extern vpgl_PFN_glDetachShader p_glDetachShader;
extern vpgl_PFN_glDisable p_glDisable;
extern vpgl_PFN_glDisableVertexAttribArray p_glDisableVertexAttribArray;
extern vpgl_PFN_glDrawArrays p_glDrawArrays;
extern vpgl_PFN_glDrawElements p_glDrawElements;
extern vpgl_PFN_glEnable p_glEnable;
extern vpgl_PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
extern vpgl_PFN_glFinish p_glFinish;
extern vpgl_PFN_glFlush p_glFlush;
extern vpgl_PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;
extern vpgl_PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
extern vpgl_PFN_glFrontFace p_glFrontFace;
extern vpgl_PFN_glGenBuffers p_glGenBuffers;
extern vpgl_PFN_glGenerateMipmap p_glGenerateMipmap;
extern vpgl_PFN_glGenFramebuffers p_glGenFramebuffers;
extern vpgl_PFN_glGenRenderbuffers p_glGenRenderbuffers;
extern vpgl_PFN_glGenTextures p_glGenTextures;
extern vpgl_PFN_glGetActiveAttrib p_glGetActiveAttrib;
extern vpgl_PFN_glGetActiveUniform p_glGetActiveUniform;
extern vpgl_PFN_glGetAttachedShaders p_glGetAttachedShaders;
extern vpgl_PFN_glGetAttribLocation p_glGetAttribLocation;
extern vpgl_PFN_glGetBooleanv p_glGetBooleanv;
extern vpgl_PFN_glGetBufferParameteriv p_glGetBufferParameteriv;
extern vpgl_PFN_glGetError p_glGetError;
extern vpgl_PFN_glGetFloatv p_glGetFloatv;
extern vpgl_PFN_glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv;
extern vpgl_PFN_glGetIntegerv p_glGetIntegerv;
extern vpgl_PFN_glGetProgramiv p_glGetProgramiv;
extern vpgl_PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
extern vpgl_PFN_glGetRenderbufferParameteriv p_glGetRenderbufferParameteriv;
extern vpgl_PFN_glGetShaderiv p_glGetShaderiv;
extern vpgl_PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
extern vpgl_PFN_glGetShaderPrecisionFormat p_glGetShaderPrecisionFormat;
extern vpgl_PFN_glGetShaderSource p_glGetShaderSource;
extern vpgl_PFN_glGetString p_glGetString;
extern vpgl_PFN_glGetTexParameterfv p_glGetTexParameterfv;
extern vpgl_PFN_glGetTexParameteriv p_glGetTexParameteriv;
extern vpgl_PFN_glGetUniformfv p_glGetUniformfv;
extern vpgl_PFN_glGetUniformiv p_glGetUniformiv;
extern vpgl_PFN_glGetUniformLocation p_glGetUniformLocation;
extern vpgl_PFN_glGetVertexAttribfv p_glGetVertexAttribfv;
extern vpgl_PFN_glGetVertexAttribiv p_glGetVertexAttribiv;
extern vpgl_PFN_glGetVertexAttribPointerv p_glGetVertexAttribPointerv;
extern vpgl_PFN_glHint p_glHint;
extern vpgl_PFN_glIsBuffer p_glIsBuffer;
extern vpgl_PFN_glIsEnabled p_glIsEnabled;
extern vpgl_PFN_glIsFramebuffer p_glIsFramebuffer;
extern vpgl_PFN_glIsProgram p_glIsProgram;
extern vpgl_PFN_glIsRenderbuffer p_glIsRenderbuffer;
extern vpgl_PFN_glIsShader p_glIsShader;
extern vpgl_PFN_glIsTexture p_glIsTexture;
extern vpgl_PFN_glLineWidth p_glLineWidth;
extern vpgl_PFN_glLinkProgram p_glLinkProgram;
extern vpgl_PFN_glPixelStorei p_glPixelStorei;
extern vpgl_PFN_glPolygonOffset p_glPolygonOffset;
extern vpgl_PFN_glReadPixels p_glReadPixels;
extern vpgl_PFN_glReleaseShaderCompiler p_glReleaseShaderCompiler;
extern vpgl_PFN_glRenderbufferStorage p_glRenderbufferStorage;
extern vpgl_PFN_glSampleCoverage p_glSampleCoverage;
extern vpgl_PFN_glScissor p_glScissor;
extern vpgl_PFN_glShaderBinary p_glShaderBinary;
extern vpgl_PFN_glShaderSource p_glShaderSource;
extern vpgl_PFN_glStencilFunc p_glStencilFunc;
extern vpgl_PFN_glStencilFuncSeparate p_glStencilFuncSeparate;
extern vpgl_PFN_glStencilMask p_glStencilMask;
extern vpgl_PFN_glStencilMaskSeparate p_glStencilMaskSeparate;
extern vpgl_PFN_glStencilOp p_glStencilOp;
extern vpgl_PFN_glStencilOpSeparate p_glStencilOpSeparate;
extern vpgl_PFN_glTexImage2D p_glTexImage2D;
extern vpgl_PFN_glTexParameterf p_glTexParameterf;
extern vpgl_PFN_glTexParameterfv p_glTexParameterfv;
extern vpgl_PFN_glTexParameteri p_glTexParameteri;
extern vpgl_PFN_glTexParameteriv p_glTexParameteriv;
extern vpgl_PFN_glTexSubImage2D p_glTexSubImage2D;
extern vpgl_PFN_glUniform1f p_glUniform1f;
extern vpgl_PFN_glUniform1fv p_glUniform1fv;
extern vpgl_PFN_glUniform1i p_glUniform1i;
extern vpgl_PFN_glUniform1iv p_glUniform1iv;
extern vpgl_PFN_glUniform2f p_glUniform2f;
extern vpgl_PFN_glUniform2fv p_glUniform2fv;
extern vpgl_PFN_glUniform2i p_glUniform2i;
extern vpgl_PFN_glUniform2iv p_glUniform2iv;
extern vpgl_PFN_glUniform3f p_glUniform3f;
extern vpgl_PFN_glUniform3fv p_glUniform3fv;
extern vpgl_PFN_glUniform3i p_glUniform3i;
extern vpgl_PFN_glUniform3iv p_glUniform3iv;
extern vpgl_PFN_glUniform4f p_glUniform4f;
extern vpgl_PFN_glUniform4fv p_glUniform4fv;
extern vpgl_PFN_glUniform4i p_glUniform4i;
extern vpgl_PFN_glUniform4iv p_glUniform4iv;
extern vpgl_PFN_glUniformMatrix2fv p_glUniformMatrix2fv;
extern vpgl_PFN_glUniformMatrix3fv p_glUniformMatrix3fv;
extern vpgl_PFN_glUniformMatrix4fv p_glUniformMatrix4fv;
extern vpgl_PFN_glUseProgram p_glUseProgram;
extern vpgl_PFN_glValidateProgram p_glValidateProgram;
extern vpgl_PFN_glVertexAttrib1f p_glVertexAttrib1f;
extern vpgl_PFN_glVertexAttrib1fv p_glVertexAttrib1fv;
extern vpgl_PFN_glVertexAttrib2f p_glVertexAttrib2f;
extern vpgl_PFN_glVertexAttrib2fv p_glVertexAttrib2fv;
extern vpgl_PFN_glVertexAttrib3f p_glVertexAttrib3f;
extern vpgl_PFN_glVertexAttrib3fv p_glVertexAttrib3fv;
extern vpgl_PFN_glVertexAttrib4f p_glVertexAttrib4f;
extern vpgl_PFN_glVertexAttrib4fv p_glVertexAttrib4fv;
extern vpgl_PFN_glVertexAttribPointer p_glVertexAttribPointer;
extern vpgl_PFN_glViewport p_glViewport;

/* The fn_id macros and the gl_call struct live in virtpass/vp_gl.h:
 * hosts include that header, the same one the guest and
 * vp_cmdpost.c compile, rather than a second copy of the ABI. */
#endif /* VPGL_HOST_TYPES_H */
