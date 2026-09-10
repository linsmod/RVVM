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
 *    raw uintptr values (identity-mapped guest memory).
 */

#ifndef WIN32_GL_BACKEND_H
#define WIN32_GL_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Host-side GL/EGL function types matching the real EGL/GLES DLLs
 * (SwiftShader / ANGLE from the Android SDK emulator directory).
 * Export names were verified undecorated -> plain cdecl, no
 * WINAPI. All types are w32gl_-prefixed so this header never
 * collides with real GL/EGL headers.
 */

#define w32gl_APIENTRY /* cdecl */

/* ---- type definitions ---- */
typedef void          w32gl_void;
typedef char          w32gl_char;
typedef unsigned int  w32gl_GLenum;
typedef unsigned int  w32gl_GLuint;
typedef unsigned int  w32gl_GLbitfield;
typedef int           w32gl_GLint;
typedef int           w32gl_GLsizei;
typedef char          w32gl_GLchar;
typedef unsigned char w32gl_GLboolean;
typedef signed char   w32gl_GLbyte;
typedef unsigned char w32gl_GLubyte;
typedef short         w32gl_GLshort;
typedef unsigned short w32gl_GLushort;
typedef float         w32gl_GLfloat;
typedef float         w32gl_GLclampf;
typedef ptrdiff_t     w32gl_GLintptr;
typedef ptrdiff_t     w32gl_GLsizeiptr;
typedef void*         w32gl_EGLDisplay;
typedef void*         w32gl_EGLSurface;
typedef void*         w32gl_EGLContext;
typedef void*         w32gl_EGLConfig;
typedef int           w32gl_EGLint;
typedef unsigned int  w32gl_EGLBoolean;
typedef unsigned int  w32gl_EGLenum;

/* ============================================================
 * Function IDs (single source of truth)
 * ============================================================ */

#define GL_FN_BASE 1
#define GL_FN_ACTIVETEXTURE 1
#define GL_FN_ATTACHSHADER 2
#define GL_FN_BINDATTRIBLOCATION 3
#define GL_FN_BINDBUFFER 4
#define GL_FN_BINDFRAMEBUFFER 5
#define GL_FN_BINDRENDERBUFFER 6
#define GL_FN_BINDTEXTURE 7
#define GL_FN_BLENDCOLOR 8
#define GL_FN_BLENDEQUATION 9
#define GL_FN_BLENDEQUATIONSEPARATE 10
#define GL_FN_BLENDFUNC 11
#define GL_FN_BLENDFUNCSEPARATE 12
#define GL_FN_BUFFERDATA 13
#define GL_FN_BUFFERSUBDATA 14
#define GL_FN_CHECKFRAMEBUFFERSTATUS 15
#define GL_FN_CLEAR 16
#define GL_FN_CLEARCOLOR 17
#define GL_FN_CLEARDEPTHF 18
#define GL_FN_CLEARSTENCIL 19
#define GL_FN_COLORMASK 20
#define GL_FN_COMPILESHADER 21
#define GL_FN_COMPRESSEDTEXIMAGE2D 22
#define GL_FN_COMPRESSEDTEXSUBIMAGE2D 23
#define GL_FN_COPYTEXIMAGE2D 24
#define GL_FN_COPYTEXSUBIMAGE2D 25
#define GL_FN_CREATEPROGRAM 26
#define GL_FN_CREATESHADER 27
#define GL_FN_CULLFACE 28
#define GL_FN_DELETEBUFFERS 29
#define GL_FN_DELETEFRAMEBUFFERS 30
#define GL_FN_DELETEPROGRAM 31
#define GL_FN_DELETERENDERBUFFERS 32
#define GL_FN_DELETESHADER 33
#define GL_FN_DELETETEXTURES 34
#define GL_FN_DEPTHFUNC 35
#define GL_FN_DEPTHMASK 36
#define GL_FN_DEPTHRANGEF 37
#define GL_FN_DETACHSHADER 38
#define GL_FN_DISABLE 39
#define GL_FN_DISABLEVERTEXATTRIBARRAY 40
#define GL_FN_DRAWARRAYS 41
#define GL_FN_DRAWELEMENTS 42
#define GL_FN_ENABLE 43
#define GL_FN_ENABLEVERTEXATTRIBARRAY 44
#define GL_FN_FINISH 45
#define GL_FN_FLUSH 46
#define GL_FN_FRAMEBUFFERRENDERBUFFER 47
#define GL_FN_FRAMEBUFFERTEXTURE2D 48
#define GL_FN_FRONTFACE 49
#define GL_FN_GENBUFFERS 50
#define GL_FN_GENERATEMIPMAP 51
#define GL_FN_GENFRAMEBUFFERS 52
#define GL_FN_GENRENDERBUFFERS 53
#define GL_FN_GENTEXTURES 54
#define GL_FN_GETACTIVEATTRIB 55
#define GL_FN_GETACTIVEUNIFORM 56
#define GL_FN_GETATTACHEDSHADERS 57
#define GL_FN_GETATTRIBLOCATION 58
#define GL_FN_GETBOOLEANV 59
#define GL_FN_GETBUFFERPARAMETERIV 60
#define GL_FN_GETERROR 61
#define GL_FN_GETFLOATV 62
#define GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV 63
#define GL_FN_GETINTEGERV 64
#define GL_FN_GETPROGRAMIV 65
#define GL_FN_GETPROGRAMINFOLOG 66
#define GL_FN_GETRENDERBUFFERPARAMETERIV 67
#define GL_FN_GETSHADERIV 68
#define GL_FN_GETSHADERINFOLOG 69
#define GL_FN_GETSHADERPRECISIONFORMAT 70
#define GL_FN_GETSHADERSOURCE 71
#define GL_FN_GETSTRING 72
#define GL_FN_GETTEXPARAMETERFV 73
#define GL_FN_GETTEXPARAMETERIV 74
#define GL_FN_GETUNIFORMFV 75
#define GL_FN_GETUNIFORMIV 76
#define GL_FN_GETUNIFORMLOCATION 77
#define GL_FN_GETVERTEXATTRIBFV 78
#define GL_FN_GETVERTEXATTRIBIV 79
#define GL_FN_GETVERTEXATTRIBPOINTERV 80
#define GL_FN_HINT 81
#define GL_FN_ISBUFFER 82
#define GL_FN_ISENABLED 83
#define GL_FN_ISFRAMEBUFFER 84
#define GL_FN_ISPROGRAM 85
#define GL_FN_ISRENDERBUFFER 86
#define GL_FN_ISSHADER 87
#define GL_FN_ISTEXTURE 88
#define GL_FN_LINEWIDTH 89
#define GL_FN_LINKPROGRAM 90
#define GL_FN_PIXELSTOREI 91
#define GL_FN_POLYGONOFFSET 92
#define GL_FN_READPIXELS 93
#define GL_FN_RELEASESHADERCOMPILER 94
#define GL_FN_RENDERBUFFERSTORAGE 95
#define GL_FN_SAMPLECOVERAGE 96
#define GL_FN_SCISSOR 97
#define GL_FN_SHADERBINARY 98
#define GL_FN_SHADERSOURCE 99
#define GL_FN_STENCILFUNC 100
#define GL_FN_STENCILFUNCSEPARATE 101
#define GL_FN_STENCILMASK 102
#define GL_FN_STENCILMASKSEPARATE 103
#define GL_FN_STENCILOP 104
#define GL_FN_STENCILOPSEPARATE 105
#define GL_FN_TEXIMAGE2D 106
#define GL_FN_TEXPARAMETERF 107
#define GL_FN_TEXPARAMETERFV 108
#define GL_FN_TEXPARAMETERI 109
#define GL_FN_TEXPARAMETERIV 110
#define GL_FN_TEXSUBIMAGE2D 111
#define GL_FN_UNIFORM1F 112
#define GL_FN_UNIFORM1FV 113
#define GL_FN_UNIFORM1I 114
#define GL_FN_UNIFORM1IV 115
#define GL_FN_UNIFORM2F 116
#define GL_FN_UNIFORM2FV 117
#define GL_FN_UNIFORM2I 118
#define GL_FN_UNIFORM2IV 119
#define GL_FN_UNIFORM3F 120
#define GL_FN_UNIFORM3FV 121
#define GL_FN_UNIFORM3I 122
#define GL_FN_UNIFORM3IV 123
#define GL_FN_UNIFORM4F 124
#define GL_FN_UNIFORM4FV 125
#define GL_FN_UNIFORM4I 126
#define GL_FN_UNIFORM4IV 127
#define GL_FN_UNIFORMMATRIX2FV 128
#define GL_FN_UNIFORMMATRIX3FV 129
#define GL_FN_UNIFORMMATRIX4FV 130
#define GL_FN_USEPROGRAM 131
#define GL_FN_VALIDATEPROGRAM 132
#define GL_FN_VERTEXATTRIB1F 133
#define GL_FN_VERTEXATTRIB1FV 134
#define GL_FN_VERTEXATTRIB2F 135
#define GL_FN_VERTEXATTRIB2FV 136
#define GL_FN_VERTEXATTRIB3F 137
#define GL_FN_VERTEXATTRIB3FV 138
#define GL_FN_VERTEXATTRIB4F 139
#define GL_FN_VERTEXATTRIB4FV 140
#define GL_FN_VERTEXATTRIBPOINTER 141
#define GL_FN_VIEWPORT 142

#define EGL_FN_BASE 0x100
#define EGL_FN_CHOOSECONFIG 0x100
#define EGL_FN_CREATECONTEXT 0x101
#define EGL_FN_CREATEPBUFFERSURFACE 0x102
#define EGL_FN_CREATEWINDOWSURFACE 0x103
#define EGL_FN_DESTROYCONTEXT 0x104
#define EGL_FN_DESTROYSURFACE 0x105
#define EGL_FN_GETCONFIGATTRIB 0x106
#define EGL_FN_GETDISPLAY 0x107
#define EGL_FN_GETERROR 0x108
#define EGL_FN_GETPROCADDRESS 0x109
#define EGL_FN_INITIALIZE 0x10A
#define EGL_FN_MAKECURRENT 0x10B
#define EGL_FN_QUERYSTRING 0x10C
#define EGL_FN_QUERYSURFACE 0x10D
#define EGL_FN_SWAPBUFFERS 0x10E
#define EGL_FN_TERMINATE 0x10F

#define GL_CALL_MAX_ARGS 9

/* Syscall numbers for marshalled GL/EGL calls (Phase 3) */
#define SYS_GL_CALL_BASE   0x10020
#define SYS_GL_CALL   (SYS_GL_CALL_BASE + 0)
#define SYS_EGL_CALL  (SYS_GL_CALL_BASE + 1)

/* ============================================================
 * Marshalling struct (guest fills, host consumes)
 *
 * Guest allocates gl_call on its stack and passes its address in a0.
 * Pointers inside args[] are guest addresses - identity-mapped into
 * host memory by the emulator, so the host uses them directly.
 * ============================================================ */
typedef struct {
    uint32_t fn_id;                    /* GL_FN_* / EGL_FN_*            */
    uint32_t nargs;                    /* number of valid args[] slots  */
    int64_t  ret;                      /* host writes the return value  */
    int64_t  args[GL_CALL_MAX_ARGS];   /* 32-bit ints are zero/sign
                                        * extended; floats bit-packed;
                                        * pointers as uintptr           */
} gl_call;

/* ---- EGL function pointer types ---- */
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglChooseConfig)(w32gl_void* dpy, const w32gl_EGLint* attrib_list, w32gl_void* configs, w32gl_EGLint config_size, w32gl_EGLint* num_config);
typedef w32gl_void* (w32gl_APIENTRY *w32gl_PFN_eglCreateContext)(w32gl_void* dpy, w32gl_void* config, w32gl_void* share_context, const w32gl_EGLint* attrib_list);
typedef w32gl_void* (w32gl_APIENTRY *w32gl_PFN_eglCreatePbufferSurface)(w32gl_void* dpy, w32gl_void* config, const w32gl_EGLint* attrib_list);
typedef w32gl_void* (w32gl_APIENTRY *w32gl_PFN_eglCreateWindowSurface)(w32gl_void* dpy, w32gl_void* config, w32gl_void* win, const w32gl_EGLint* attrib_list);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglDestroyContext)(w32gl_void* dpy, w32gl_void* ctx);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglDestroySurface)(w32gl_void* dpy, w32gl_void* surface);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglGetConfigAttrib)(w32gl_void* dpy, w32gl_void* config, w32gl_EGLint attribute, w32gl_EGLint* value);
typedef w32gl_void* (w32gl_APIENTRY *w32gl_PFN_eglGetDisplay)(w32gl_void* display_id);
typedef w32gl_EGLint (w32gl_APIENTRY *w32gl_PFN_eglGetError)(void);
typedef w32gl_void* (w32gl_APIENTRY *w32gl_PFN_eglGetProcAddress)(const w32gl_char* procname);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglInitialize)(w32gl_void* dpy, w32gl_EGLint* major, w32gl_EGLint* minor);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglMakeCurrent)(w32gl_void* dpy, w32gl_void* draw, w32gl_void* read, w32gl_void* ctx);
typedef const w32gl_char* (w32gl_APIENTRY *w32gl_PFN_eglQueryString)(w32gl_void* dpy, w32gl_EGLint name);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglQuerySurface)(w32gl_void* dpy, w32gl_void* surface, w32gl_EGLint attribute, w32gl_EGLint* value);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglSwapBuffers)(w32gl_void* dpy, w32gl_void* surface);
typedef w32gl_EGLBoolean (w32gl_APIENTRY *w32gl_PFN_eglTerminate)(w32gl_void* dpy);

/* ---- GLES2 function pointer types ---- */
typedef void (w32gl_APIENTRY *w32gl_PFN_glActiveTexture)(w32gl_GLenum texture);
typedef void (w32gl_APIENTRY *w32gl_PFN_glAttachShader)(w32gl_GLuint program, w32gl_GLuint shader);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBindAttribLocation)(w32gl_GLuint program, w32gl_GLuint index, const w32gl_GLchar* name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBindBuffer)(w32gl_GLenum target, w32gl_GLuint buffer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBindFramebuffer)(w32gl_GLenum target, w32gl_GLuint framebuffer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBindRenderbuffer)(w32gl_GLenum target, w32gl_GLuint renderbuffer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBindTexture)(w32gl_GLenum target, w32gl_GLuint texture);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBlendColor)(w32gl_GLfloat red, w32gl_GLfloat green, w32gl_GLfloat blue, w32gl_GLfloat alpha);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBlendEquation)(w32gl_GLenum mode);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBlendEquationSeparate)(w32gl_GLenum modeRGB, w32gl_GLenum modeAlpha);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBlendFunc)(w32gl_GLenum sfactor, w32gl_GLenum dfactor);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBlendFuncSeparate)(w32gl_GLenum sfactorRGB, w32gl_GLenum dfactorRGB, w32gl_GLenum sfactorAlpha, w32gl_GLenum dfactorAlpha);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBufferData)(w32gl_GLenum target, w32gl_GLsizeiptr size, const w32gl_void* data, w32gl_GLenum usage);
typedef void (w32gl_APIENTRY *w32gl_PFN_glBufferSubData)(w32gl_GLenum target, w32gl_GLintptr offset, w32gl_GLsizeiptr size, const w32gl_void* data);
typedef w32gl_GLenum (w32gl_APIENTRY *w32gl_PFN_glCheckFramebufferStatus)(w32gl_GLenum target);
typedef void (w32gl_APIENTRY *w32gl_PFN_glClear)(w32gl_GLbitfield mask);
typedef void (w32gl_APIENTRY *w32gl_PFN_glClearColor)(w32gl_GLfloat red, w32gl_GLfloat green, w32gl_GLfloat blue, w32gl_GLfloat alpha);
typedef void (w32gl_APIENTRY *w32gl_PFN_glClearDepthf)(w32gl_GLfloat d);
typedef void (w32gl_APIENTRY *w32gl_PFN_glClearStencil)(w32gl_GLint s);
typedef void (w32gl_APIENTRY *w32gl_PFN_glColorMask)(w32gl_GLboolean red, w32gl_GLboolean green, w32gl_GLboolean blue, w32gl_GLboolean alpha);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCompileShader)(w32gl_GLuint shader);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCompressedTexImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLenum internalformat, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLint border, w32gl_GLsizei imageSize, const w32gl_void* data);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCompressedTexSubImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLint xoffset, w32gl_GLint yoffset, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLenum format, w32gl_GLsizei imageSize, const w32gl_void* data);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCopyTexImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLenum internalformat, w32gl_GLint x, w32gl_GLint y, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLint border);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCopyTexSubImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLint xoffset, w32gl_GLint yoffset, w32gl_GLint x, w32gl_GLint y, w32gl_GLsizei width, w32gl_GLsizei height);
typedef w32gl_GLuint (w32gl_APIENTRY *w32gl_PFN_glCreateProgram)(void);
typedef w32gl_GLuint (w32gl_APIENTRY *w32gl_PFN_glCreateShader)(w32gl_GLenum type);
typedef void (w32gl_APIENTRY *w32gl_PFN_glCullFace)(w32gl_GLenum mode);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteBuffers)(w32gl_GLsizei n, const w32gl_GLuint* buffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteFramebuffers)(w32gl_GLsizei n, const w32gl_GLuint* framebuffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteProgram)(w32gl_GLuint program);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteRenderbuffers)(w32gl_GLsizei n, const w32gl_GLuint* renderbuffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteShader)(w32gl_GLuint shader);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDeleteTextures)(w32gl_GLsizei n, const w32gl_GLuint* textures);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDepthFunc)(w32gl_GLenum func);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDepthMask)(w32gl_GLboolean flag);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDepthRangef)(w32gl_GLfloat n, w32gl_GLfloat f);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDetachShader)(w32gl_GLuint program, w32gl_GLuint shader);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDisable)(w32gl_GLenum cap);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDisableVertexAttribArray)(w32gl_GLuint index);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDrawArrays)(w32gl_GLenum mode, w32gl_GLint first, w32gl_GLsizei count);
typedef void (w32gl_APIENTRY *w32gl_PFN_glDrawElements)(w32gl_GLenum mode, w32gl_GLsizei count, w32gl_GLenum type, const w32gl_void* indices);
typedef void (w32gl_APIENTRY *w32gl_PFN_glEnable)(w32gl_GLenum cap);
typedef void (w32gl_APIENTRY *w32gl_PFN_glEnableVertexAttribArray)(w32gl_GLuint index);
typedef void (w32gl_APIENTRY *w32gl_PFN_glFinish)(void);
typedef void (w32gl_APIENTRY *w32gl_PFN_glFlush)(void);
typedef void (w32gl_APIENTRY *w32gl_PFN_glFramebufferRenderbuffer)(w32gl_GLenum target, w32gl_GLenum attachment, w32gl_GLenum renderbuffertarget, w32gl_GLuint renderbuffer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glFramebufferTexture2D)(w32gl_GLenum target, w32gl_GLenum attachment, w32gl_GLenum textarget, w32gl_GLuint texture, w32gl_GLint level);
typedef void (w32gl_APIENTRY *w32gl_PFN_glFrontFace)(w32gl_GLenum mode);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGenBuffers)(w32gl_GLsizei n, w32gl_GLuint* buffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGenerateMipmap)(w32gl_GLenum target);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGenFramebuffers)(w32gl_GLsizei n, w32gl_GLuint* framebuffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGenRenderbuffers)(w32gl_GLsizei n, w32gl_GLuint* renderbuffers);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGenTextures)(w32gl_GLsizei n, w32gl_GLuint* textures);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetActiveAttrib)(w32gl_GLuint program, w32gl_GLuint index, w32gl_GLsizei bufSize, w32gl_GLsizei* length, w32gl_GLint* size, w32gl_GLenum* type, w32gl_GLchar* name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetActiveUniform)(w32gl_GLuint program, w32gl_GLuint index, w32gl_GLsizei bufSize, w32gl_GLsizei* length, w32gl_GLint* size, w32gl_GLenum* type, w32gl_GLchar* name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetAttachedShaders)(w32gl_GLuint program, w32gl_GLsizei maxCount, w32gl_GLsizei* count, w32gl_GLuint* shaders);
typedef w32gl_GLint (w32gl_APIENTRY *w32gl_PFN_glGetAttribLocation)(w32gl_GLuint program, const w32gl_GLchar* name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetBooleanv)(w32gl_GLenum pname, w32gl_GLboolean* data);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetBufferParameteriv)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLint* params);
typedef w32gl_GLenum (w32gl_APIENTRY *w32gl_PFN_glGetError)(void);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetFloatv)(w32gl_GLenum pname, w32gl_GLfloat* data);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetFramebufferAttachmentParameteriv)(w32gl_GLenum target, w32gl_GLenum attachment, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetIntegerv)(w32gl_GLenum pname, w32gl_GLint* data);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetProgramiv)(w32gl_GLuint program, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetProgramInfoLog)(w32gl_GLuint program, w32gl_GLsizei bufSize, w32gl_GLsizei* length, w32gl_GLchar* infoLog);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetRenderbufferParameteriv)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetShaderiv)(w32gl_GLuint shader, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetShaderInfoLog)(w32gl_GLuint shader, w32gl_GLsizei bufSize, w32gl_GLsizei* length, w32gl_GLchar* infoLog);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetShaderPrecisionFormat)(w32gl_GLenum shadertype, w32gl_GLenum precisiontype, w32gl_GLint* range, w32gl_GLint* precision);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetShaderSource)(w32gl_GLuint shader, w32gl_GLsizei bufSize, w32gl_GLsizei* length, w32gl_GLchar* source);
typedef const w32gl_GLubyte* (w32gl_APIENTRY *w32gl_PFN_glGetString)(w32gl_GLenum name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetTexParameterfv)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLfloat* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetTexParameteriv)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetUniformfv)(w32gl_GLuint program, w32gl_GLint location, w32gl_GLfloat* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetUniformiv)(w32gl_GLuint program, w32gl_GLint location, w32gl_GLint* params);
typedef w32gl_GLint (w32gl_APIENTRY *w32gl_PFN_glGetUniformLocation)(w32gl_GLuint program, const w32gl_GLchar* name);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetVertexAttribfv)(w32gl_GLuint index, w32gl_GLenum pname, w32gl_GLfloat* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetVertexAttribiv)(w32gl_GLuint index, w32gl_GLenum pname, w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glGetVertexAttribPointerv)(w32gl_GLuint index, w32gl_GLenum pname, w32gl_void** pointer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glHint)(w32gl_GLenum target, w32gl_GLenum mode);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsBuffer)(w32gl_GLuint buffer);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsEnabled)(w32gl_GLenum cap);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsFramebuffer)(w32gl_GLuint framebuffer);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsProgram)(w32gl_GLuint program);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsRenderbuffer)(w32gl_GLuint renderbuffer);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsShader)(w32gl_GLuint shader);
typedef w32gl_GLboolean (w32gl_APIENTRY *w32gl_PFN_glIsTexture)(w32gl_GLuint texture);
typedef void (w32gl_APIENTRY *w32gl_PFN_glLineWidth)(w32gl_GLfloat width);
typedef void (w32gl_APIENTRY *w32gl_PFN_glLinkProgram)(w32gl_GLuint program);
typedef void (w32gl_APIENTRY *w32gl_PFN_glPixelStorei)(w32gl_GLenum pname, w32gl_GLint param);
typedef void (w32gl_APIENTRY *w32gl_PFN_glPolygonOffset)(w32gl_GLfloat factor, w32gl_GLfloat units);
typedef void (w32gl_APIENTRY *w32gl_PFN_glReadPixels)(w32gl_GLint x, w32gl_GLint y, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLenum format, w32gl_GLenum type, w32gl_void* pixels);
typedef void (w32gl_APIENTRY *w32gl_PFN_glReleaseShaderCompiler)(void);
typedef void (w32gl_APIENTRY *w32gl_PFN_glRenderbufferStorage)(w32gl_GLenum target, w32gl_GLenum internalformat, w32gl_GLsizei width, w32gl_GLsizei height);
typedef void (w32gl_APIENTRY *w32gl_PFN_glSampleCoverage)(w32gl_GLfloat value, w32gl_GLboolean invert);
typedef void (w32gl_APIENTRY *w32gl_PFN_glScissor)(w32gl_GLint x, w32gl_GLint y, w32gl_GLsizei width, w32gl_GLsizei height);
typedef void (w32gl_APIENTRY *w32gl_PFN_glShaderBinary)(w32gl_GLsizei count, const w32gl_GLuint* shaders, w32gl_GLenum binaryformat, const w32gl_void* binary, w32gl_GLsizei length);
typedef void (w32gl_APIENTRY *w32gl_PFN_glShaderSource)(w32gl_GLuint shader, w32gl_GLsizei count, const w32gl_GLchar** string, const w32gl_GLint* length);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilFunc)(w32gl_GLenum func, w32gl_GLint ref, w32gl_GLuint mask);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilFuncSeparate)(w32gl_GLenum face, w32gl_GLenum func, w32gl_GLint ref, w32gl_GLuint mask);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilMask)(w32gl_GLuint mask);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilMaskSeparate)(w32gl_GLenum face, w32gl_GLuint mask);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilOp)(w32gl_GLenum fail, w32gl_GLenum zfail, w32gl_GLenum zpass);
typedef void (w32gl_APIENTRY *w32gl_PFN_glStencilOpSeparate)(w32gl_GLenum face, w32gl_GLenum sfail, w32gl_GLenum dpfail, w32gl_GLenum dppass);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLint internalformat, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLint border, w32gl_GLenum format, w32gl_GLenum type, const w32gl_void* pixels);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexParameterf)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLfloat param);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexParameterfv)(w32gl_GLenum target, w32gl_GLenum pname, const w32gl_GLfloat* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexParameteri)(w32gl_GLenum target, w32gl_GLenum pname, w32gl_GLint param);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexParameteriv)(w32gl_GLenum target, w32gl_GLenum pname, const w32gl_GLint* params);
typedef void (w32gl_APIENTRY *w32gl_PFN_glTexSubImage2D)(w32gl_GLenum target, w32gl_GLint level, w32gl_GLint xoffset, w32gl_GLint yoffset, w32gl_GLsizei width, w32gl_GLsizei height, w32gl_GLenum format, w32gl_GLenum type, const w32gl_void* pixels);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform1f)(w32gl_GLint location, w32gl_GLfloat v0);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform1fv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform1i)(w32gl_GLint location, w32gl_GLint v0);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform1iv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLint* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform2f)(w32gl_GLint location, w32gl_GLfloat v0, w32gl_GLfloat v1);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform2fv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform2i)(w32gl_GLint location, w32gl_GLint v0, w32gl_GLint v1);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform2iv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLint* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform3f)(w32gl_GLint location, w32gl_GLfloat v0, w32gl_GLfloat v1, w32gl_GLfloat v2);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform3fv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform3i)(w32gl_GLint location, w32gl_GLint v0, w32gl_GLint v1, w32gl_GLint v2);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform3iv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLint* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform4f)(w32gl_GLint location, w32gl_GLfloat v0, w32gl_GLfloat v1, w32gl_GLfloat v2, w32gl_GLfloat v3);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform4fv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform4i)(w32gl_GLint location, w32gl_GLint v0, w32gl_GLint v1, w32gl_GLint v2, w32gl_GLint v3);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniform4iv)(w32gl_GLint location, w32gl_GLsizei count, const w32gl_GLint* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniformMatrix2fv)(w32gl_GLint location, w32gl_GLsizei count, w32gl_GLboolean transpose, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniformMatrix3fv)(w32gl_GLint location, w32gl_GLsizei count, w32gl_GLboolean transpose, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUniformMatrix4fv)(w32gl_GLint location, w32gl_GLsizei count, w32gl_GLboolean transpose, const w32gl_GLfloat* value);
typedef void (w32gl_APIENTRY *w32gl_PFN_glUseProgram)(w32gl_GLuint program);
typedef void (w32gl_APIENTRY *w32gl_PFN_glValidateProgram)(w32gl_GLuint program);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib1f)(w32gl_GLuint index, w32gl_GLfloat x);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib1fv)(w32gl_GLuint index, const w32gl_GLfloat* v);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib2f)(w32gl_GLuint index, w32gl_GLfloat x, w32gl_GLfloat y);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib2fv)(w32gl_GLuint index, const w32gl_GLfloat* v);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib3f)(w32gl_GLuint index, w32gl_GLfloat x, w32gl_GLfloat y, w32gl_GLfloat z);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib3fv)(w32gl_GLuint index, const w32gl_GLfloat* v);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib4f)(w32gl_GLuint index, w32gl_GLfloat x, w32gl_GLfloat y, w32gl_GLfloat z, w32gl_GLfloat w);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttrib4fv)(w32gl_GLuint index, const w32gl_GLfloat* v);
typedef void (w32gl_APIENTRY *w32gl_PFN_glVertexAttribPointer)(w32gl_GLuint index, w32gl_GLint size, w32gl_GLenum type, w32gl_GLboolean normalized, w32gl_GLsizei stride, const w32gl_void* pointer);
typedef void (w32gl_APIENTRY *w32gl_PFN_glViewport)(w32gl_GLint x, w32gl_GLint y, w32gl_GLsizei width, w32gl_GLsizei height);

/* ---- resolved entry points (NULL when missing) ---- */
extern w32gl_PFN_eglChooseConfig p_eglChooseConfig;
extern w32gl_PFN_eglCreateContext p_eglCreateContext;
extern w32gl_PFN_eglCreatePbufferSurface p_eglCreatePbufferSurface;
extern w32gl_PFN_eglCreateWindowSurface p_eglCreateWindowSurface;
extern w32gl_PFN_eglDestroyContext p_eglDestroyContext;
extern w32gl_PFN_eglDestroySurface p_eglDestroySurface;
extern w32gl_PFN_eglGetConfigAttrib p_eglGetConfigAttrib;
extern w32gl_PFN_eglGetDisplay p_eglGetDisplay;
extern w32gl_PFN_eglGetError p_eglGetError;
extern w32gl_PFN_eglGetProcAddress p_eglGetProcAddress;
extern w32gl_PFN_eglInitialize p_eglInitialize;
extern w32gl_PFN_eglMakeCurrent p_eglMakeCurrent;
extern w32gl_PFN_eglQueryString p_eglQueryString;
extern w32gl_PFN_eglQuerySurface p_eglQuerySurface;
extern w32gl_PFN_eglSwapBuffers p_eglSwapBuffers;
extern w32gl_PFN_eglTerminate p_eglTerminate;

extern w32gl_PFN_glActiveTexture p_glActiveTexture;
extern w32gl_PFN_glAttachShader p_glAttachShader;
extern w32gl_PFN_glBindAttribLocation p_glBindAttribLocation;
extern w32gl_PFN_glBindBuffer p_glBindBuffer;
extern w32gl_PFN_glBindFramebuffer p_glBindFramebuffer;
extern w32gl_PFN_glBindRenderbuffer p_glBindRenderbuffer;
extern w32gl_PFN_glBindTexture p_glBindTexture;
extern w32gl_PFN_glBlendColor p_glBlendColor;
extern w32gl_PFN_glBlendEquation p_glBlendEquation;
extern w32gl_PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
extern w32gl_PFN_glBlendFunc p_glBlendFunc;
extern w32gl_PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
extern w32gl_PFN_glBufferData p_glBufferData;
extern w32gl_PFN_glBufferSubData p_glBufferSubData;
extern w32gl_PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
extern w32gl_PFN_glClear p_glClear;
extern w32gl_PFN_glClearColor p_glClearColor;
extern w32gl_PFN_glClearDepthf p_glClearDepthf;
extern w32gl_PFN_glClearStencil p_glClearStencil;
extern w32gl_PFN_glColorMask p_glColorMask;
extern w32gl_PFN_glCompileShader p_glCompileShader;
extern w32gl_PFN_glCompressedTexImage2D p_glCompressedTexImage2D;
extern w32gl_PFN_glCompressedTexSubImage2D p_glCompressedTexSubImage2D;
extern w32gl_PFN_glCopyTexImage2D p_glCopyTexImage2D;
extern w32gl_PFN_glCopyTexSubImage2D p_glCopyTexSubImage2D;
extern w32gl_PFN_glCreateProgram p_glCreateProgram;
extern w32gl_PFN_glCreateShader p_glCreateShader;
extern w32gl_PFN_glCullFace p_glCullFace;
extern w32gl_PFN_glDeleteBuffers p_glDeleteBuffers;
extern w32gl_PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
extern w32gl_PFN_glDeleteProgram p_glDeleteProgram;
extern w32gl_PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
extern w32gl_PFN_glDeleteShader p_glDeleteShader;
extern w32gl_PFN_glDeleteTextures p_glDeleteTextures;
extern w32gl_PFN_glDepthFunc p_glDepthFunc;
extern w32gl_PFN_glDepthMask p_glDepthMask;
extern w32gl_PFN_glDepthRangef p_glDepthRangef;
extern w32gl_PFN_glDetachShader p_glDetachShader;
extern w32gl_PFN_glDisable p_glDisable;
extern w32gl_PFN_glDisableVertexAttribArray p_glDisableVertexAttribArray;
extern w32gl_PFN_glDrawArrays p_glDrawArrays;
extern w32gl_PFN_glDrawElements p_glDrawElements;
extern w32gl_PFN_glEnable p_glEnable;
extern w32gl_PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
extern w32gl_PFN_glFinish p_glFinish;
extern w32gl_PFN_glFlush p_glFlush;
extern w32gl_PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;
extern w32gl_PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
extern w32gl_PFN_glFrontFace p_glFrontFace;
extern w32gl_PFN_glGenBuffers p_glGenBuffers;
extern w32gl_PFN_glGenerateMipmap p_glGenerateMipmap;
extern w32gl_PFN_glGenFramebuffers p_glGenFramebuffers;
extern w32gl_PFN_glGenRenderbuffers p_glGenRenderbuffers;
extern w32gl_PFN_glGenTextures p_glGenTextures;
extern w32gl_PFN_glGetActiveAttrib p_glGetActiveAttrib;
extern w32gl_PFN_glGetActiveUniform p_glGetActiveUniform;
extern w32gl_PFN_glGetAttachedShaders p_glGetAttachedShaders;
extern w32gl_PFN_glGetAttribLocation p_glGetAttribLocation;
extern w32gl_PFN_glGetBooleanv p_glGetBooleanv;
extern w32gl_PFN_glGetBufferParameteriv p_glGetBufferParameteriv;
extern w32gl_PFN_glGetError p_glGetError;
extern w32gl_PFN_glGetFloatv p_glGetFloatv;
extern w32gl_PFN_glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv;
extern w32gl_PFN_glGetIntegerv p_glGetIntegerv;
extern w32gl_PFN_glGetProgramiv p_glGetProgramiv;
extern w32gl_PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
extern w32gl_PFN_glGetRenderbufferParameteriv p_glGetRenderbufferParameteriv;
extern w32gl_PFN_glGetShaderiv p_glGetShaderiv;
extern w32gl_PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
extern w32gl_PFN_glGetShaderPrecisionFormat p_glGetShaderPrecisionFormat;
extern w32gl_PFN_glGetShaderSource p_glGetShaderSource;
extern w32gl_PFN_glGetString p_glGetString;
extern w32gl_PFN_glGetTexParameterfv p_glGetTexParameterfv;
extern w32gl_PFN_glGetTexParameteriv p_glGetTexParameteriv;
extern w32gl_PFN_glGetUniformfv p_glGetUniformfv;
extern w32gl_PFN_glGetUniformiv p_glGetUniformiv;
extern w32gl_PFN_glGetUniformLocation p_glGetUniformLocation;
extern w32gl_PFN_glGetVertexAttribfv p_glGetVertexAttribfv;
extern w32gl_PFN_glGetVertexAttribiv p_glGetVertexAttribiv;
extern w32gl_PFN_glGetVertexAttribPointerv p_glGetVertexAttribPointerv;
extern w32gl_PFN_glHint p_glHint;
extern w32gl_PFN_glIsBuffer p_glIsBuffer;
extern w32gl_PFN_glIsEnabled p_glIsEnabled;
extern w32gl_PFN_glIsFramebuffer p_glIsFramebuffer;
extern w32gl_PFN_glIsProgram p_glIsProgram;
extern w32gl_PFN_glIsRenderbuffer p_glIsRenderbuffer;
extern w32gl_PFN_glIsShader p_glIsShader;
extern w32gl_PFN_glIsTexture p_glIsTexture;
extern w32gl_PFN_glLineWidth p_glLineWidth;
extern w32gl_PFN_glLinkProgram p_glLinkProgram;
extern w32gl_PFN_glPixelStorei p_glPixelStorei;
extern w32gl_PFN_glPolygonOffset p_glPolygonOffset;
extern w32gl_PFN_glReadPixels p_glReadPixels;
extern w32gl_PFN_glReleaseShaderCompiler p_glReleaseShaderCompiler;
extern w32gl_PFN_glRenderbufferStorage p_glRenderbufferStorage;
extern w32gl_PFN_glSampleCoverage p_glSampleCoverage;
extern w32gl_PFN_glScissor p_glScissor;
extern w32gl_PFN_glShaderBinary p_glShaderBinary;
extern w32gl_PFN_glShaderSource p_glShaderSource;
extern w32gl_PFN_glStencilFunc p_glStencilFunc;
extern w32gl_PFN_glStencilFuncSeparate p_glStencilFuncSeparate;
extern w32gl_PFN_glStencilMask p_glStencilMask;
extern w32gl_PFN_glStencilMaskSeparate p_glStencilMaskSeparate;
extern w32gl_PFN_glStencilOp p_glStencilOp;
extern w32gl_PFN_glStencilOpSeparate p_glStencilOpSeparate;
extern w32gl_PFN_glTexImage2D p_glTexImage2D;
extern w32gl_PFN_glTexParameterf p_glTexParameterf;
extern w32gl_PFN_glTexParameterfv p_glTexParameterfv;
extern w32gl_PFN_glTexParameteri p_glTexParameteri;
extern w32gl_PFN_glTexParameteriv p_glTexParameteriv;
extern w32gl_PFN_glTexSubImage2D p_glTexSubImage2D;
extern w32gl_PFN_glUniform1f p_glUniform1f;
extern w32gl_PFN_glUniform1fv p_glUniform1fv;
extern w32gl_PFN_glUniform1i p_glUniform1i;
extern w32gl_PFN_glUniform1iv p_glUniform1iv;
extern w32gl_PFN_glUniform2f p_glUniform2f;
extern w32gl_PFN_glUniform2fv p_glUniform2fv;
extern w32gl_PFN_glUniform2i p_glUniform2i;
extern w32gl_PFN_glUniform2iv p_glUniform2iv;
extern w32gl_PFN_glUniform3f p_glUniform3f;
extern w32gl_PFN_glUniform3fv p_glUniform3fv;
extern w32gl_PFN_glUniform3i p_glUniform3i;
extern w32gl_PFN_glUniform3iv p_glUniform3iv;
extern w32gl_PFN_glUniform4f p_glUniform4f;
extern w32gl_PFN_glUniform4fv p_glUniform4fv;
extern w32gl_PFN_glUniform4i p_glUniform4i;
extern w32gl_PFN_glUniform4iv p_glUniform4iv;
extern w32gl_PFN_glUniformMatrix2fv p_glUniformMatrix2fv;
extern w32gl_PFN_glUniformMatrix3fv p_glUniformMatrix3fv;
extern w32gl_PFN_glUniformMatrix4fv p_glUniformMatrix4fv;
extern w32gl_PFN_glUseProgram p_glUseProgram;
extern w32gl_PFN_glValidateProgram p_glValidateProgram;
extern w32gl_PFN_glVertexAttrib1f p_glVertexAttrib1f;
extern w32gl_PFN_glVertexAttrib1fv p_glVertexAttrib1fv;
extern w32gl_PFN_glVertexAttrib2f p_glVertexAttrib2f;
extern w32gl_PFN_glVertexAttrib2fv p_glVertexAttrib2fv;
extern w32gl_PFN_glVertexAttrib3f p_glVertexAttrib3f;
extern w32gl_PFN_glVertexAttrib3fv p_glVertexAttrib3fv;
extern w32gl_PFN_glVertexAttrib4f p_glVertexAttrib4f;
extern w32gl_PFN_glVertexAttrib4fv p_glVertexAttrib4fv;
extern w32gl_PFN_glVertexAttribPointer p_glVertexAttribPointer;
extern w32gl_PFN_glViewport p_glViewport;

/* ---- backend lifecycle -------------------------------------------- */

/*
 * Load libEGL.dll + libGLESv2.dll and resolve all entry points.
 * Idempotent and lazily cached: first call does the work, later calls are
 * no-ops returning the cached result. Selection:
 *
 *   RVVM_GL_BACKEND   = angle | swiftshader | off   (default: angle,
 *                     falling back to swiftshader if angle fails)
 *   RVVM_GL_DLL_DIR   = explicit directory containing libEGL.dll and
 *                     libGLESv2.dll (skips backend lookup)
 *
 * When no backend can be loaded every GL/EGL dispatch returns 0, which
 * makes guests fall back to the CPU rendering path (WINDOW_LOCK/UNLOCK).
 */
bool win32_gl_backend_load(void);

/* True after a successful win32_gl_backend_load(). */
bool win32_gl_backend_ready(void);

/* Backend name for logs ("angle", "swiftshader", "" when unloaded). */
const char* win32_gl_backend_name(void);

/* Unload the backend (called from win32_host_shutdown). */
void win32_gl_backend_unload(void);

#endif /* WIN32_GL_BACKEND_H */
