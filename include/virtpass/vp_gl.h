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

#ifndef VIRTPASS_GL
#define VIRTPASS_GL

#include <stdint.h>

/* ============================================================
 * GLES2 core types (khronos widths, riscv64 LP64 guest)
 * ============================================================ */
typedef void             GLvoid;
typedef unsigned int     GLenum;
typedef unsigned char    GLboolean;
typedef unsigned int     GLbitfield;
typedef signed char      GLbyte;
typedef short            GLshort;
typedef int              GLint;
typedef unsigned char    GLubyte;
typedef unsigned short   GLushort;
typedef unsigned int     GLuint;
typedef int              GLsizei;
typedef float            GLfloat;
typedef float            GLclampf;
typedef char             GLchar;
typedef long             GLintptr;    /* khronos_intptr_t */
typedef long             GLsizeiptr;  /* khronos_ssize_t  */

/* ============================================================
 * GLES2 constants (subset used by guests + host smoke tests)
 * ============================================================ */
#define GL_FALSE 0
#define GL_TRUE  1

#define GL_BYTE                0x1400
#define GL_UNSIGNED_BYTE       0x1401
#define GL_SHORT               0x1402
#define GL_UNSIGNED_SHORT      0x1403
#define GL_INT                 0x1404
#define GL_UNSIGNED_INT        0x1405
#define GL_FLOAT               0x1406

#define GL_NEVER               0x0200
#define GL_LESS                0x0201
#define GL_EQUAL               0x0202
#define GL_LEQUAL              0x0203
#define GL_GREATER             0x0204
#define GL_NOTEQUAL            0x0205
#define GL_GEQUAL              0x0206
#define GL_ALWAYS              0x0207

#define GL_POINTS              0x0000
#define GL_LINES               0x0001
#define GL_LINE_LOOP           0x0002
#define GL_LINE_STRIP          0x0003
#define GL_TRIANGLES           0x0004
#define GL_TRIANGLE_STRIP      0x0005
#define GL_TRIANGLE_FAN        0x0006

#define GL_ZERO                0
#define GL_ONE                 1

#define GL_NO_ERROR            0
#define GL_INVALID_ENUM        0x0500
#define GL_INVALID_VALUE       0x0501
#define GL_INVALID_OPERATION   0x0502
#define GL_OUT_OF_MEMORY       0x0505

#define GL_VENDOR              0x1F00
#define GL_RENDERER            0x1F01
#define GL_VERSION             0x1F02
#define GL_EXTENSIONS          0x1F03

#define GL_DEPTH_BUFFER_BIT    0x00000100
#define GL_STENCIL_BUFFER_BIT  0x00000400
#define GL_COLOR_BUFFER_BIT    0x00004000

#define GL_RGBA                0x1908
#define GL_RGB                 0x1907

#define GL_FRAGMENT_SHADER     0x8B30
#define GL_VERTEX_SHADER       0x8B31
#define GL_COMPILE_STATUS      0x8B81
#define GL_LINK_STATUS         0x8B82
#define GL_ARRAY_BUFFER        0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW         0x88E4
#define GL_STREAM_DRAW         0x88E0
#define GL_FRAMEBUFFER         0x8D40
#define GL_RENDERBUFFER        0x8D41

/* ============================================================
 * EGL core types (opaque handles travel as uintptr values)
 * ============================================================ */
typedef void*    EGLDisplay;
typedef void*    EGLSurface;
typedef void*    EGLContext;
typedef void*    EGLConfig;
typedef int32_t  EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_DONT_CARE        ((EGLint)-1)

#define EGL_DEFAULT_DISPLAY  ((EGLDisplay)0)
#define EGL_NO_DISPLAY       ((EGLDisplay)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)

#define EGL_SUCCESS            0x3000
#define EGL_NOT_INITIALIZED    0x3001
#define EGL_BAD_ACCESS         0x3002
#define EGL_BAD_ALLOC          0x3003
#define EGL_BAD_MATCH          0x3004
#define EGL_BAD_ATTRIBUTE      0x3005
#define EGL_BAD_CONFIG         0x3006
#define EGL_BAD_CONTEXT        0x3007
#define EGL_BAD_CURRENT_SURFACE 0x3008
#define EGL_BAD_DISPLAY        0x3009
#define EGL_BAD_MATCH2         0x300A /* reserved */
#define EGL_BAD_NATIVE_WINDOW  0x300B
#define EGL_BAD_SURFACE        0x300D
#define EGL_CONTEXT_LOST       0x300E

#define EGL_BUFFER_SIZE      0x3020
#define EGL_ALPHA_SIZE       0x3021
#define EGL_BLUE_SIZE        0x3022
#define EGL_GREEN_SIZE       0x3023
#define EGL_RED_SIZE         0x3024
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_SAMPLES          0x3031
#define EGL_SAMPLE_BUFFERS   0x3032
#define EGL_SURFACE_TYPE     0x3033
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_NONE             0x3038

#define EGL_PBUFFER_BIT      0x0001
#define EGL_PIXMAP_BIT       0x0002
#define EGL_WINDOW_BIT       0x0004
#define EGL_OPENGL_ES_BIT    0x0001
#define EGL_OPENGL_ES2_BIT   0x0004
#define EGL_OPENGL_ES3_BIT   0x0040

#define EGL_WIDTH            0x3057
#define EGL_HEIGHT           0x3056
#define EGL_LARGEST_PBUFFER  0x3058

#define EGL_VENDOR           0x3053
#define EGL_VERSION          0x3054
#define EGL_EXTENSIONS       0x3055
#define EGL_CLIENT_APIS      0x308D

#define EGL_OPENGL_ES_API    0x30A0
#define EGL_OPENGL_API       0x30A2
#define EGL_OPENVG_API       0x30A1

#define EGL_DRAW             0x3059
#define EGL_READ             0x305A

#define EGL_CONTEXT_CLIENT_VERSION 0x3098

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
/* gl_call.args[] slot carrying the guest scratch buffer for the
 * calls returning a host-owned string (glGetString / eglQueryString).
 * The buffer must outlive the call: the stub owns it statically. */
#define GL_CALL_RETBUF_SLOT (GL_CALL_MAX_ARGS - 1)
#define GL_CALL_RETBUF_CAP  8192

/* The marshalled-call syscall numbers (SYS_GL_CALL / SYS_EGL_CALL) come
 * from the shared ABI header, together with every other hypercall. */
#include "virtpass/vp_syscall.h"
/* ============================================================
 * Marshalling struct (guest fills, host consumes)
 *
 * Guest allocates gl_call on its stack and passes its address in a0.
 * Pointers inside args[] are GUEST addresses: guest memory is no longer
 * mapped into the host, so the backend must run each data pointer through
 * rvvm_user_guest_ptr() before dereferencing it. Opaque host values
 * (EGLDisplay/EGLConfig/EGLSurface/EGLContext and the EGLNative* types) are
 * only passed back by the guest, never dereferenced, so they pass through.
 *
 * args[GL_CALL_RETBUF_SLOT] holds the address of a guest scratch buffer
 * (GL_CALL_RETBUF_CAP bytes) for calls that hand back a host-owned string
 * (glGetString/eglQueryString): the host copies the string there and answers
 * with that guest address. The stub keeps it in a static, not on its stack -
 * the pointer outlives the call. Only those two single-argument calls use the
 * slot; everywhere else it is just the last parameter (or unused).
 * ============================================================ */
typedef struct {
    uint32_t fn_id;                    /* GL_FN_* / EGL_FN_*            */
    uint32_t nargs;                    /* number of real args[] slots   */
    int64_t  ret;                      /* host writes the return value  */
    int64_t  args[GL_CALL_MAX_ARGS];   /* 32-bit ints are zero/sign
                                        * extended; floats bit-packed;
                                        * data pointers as guest VA;
                                        * handles as opaque host values;
                                        * last slot = string scratch     */
} gl_call;

/* ============================================================
 * Guest-facing API (same signatures as the NDK headers)
 * ============================================================ */

uint32_t eglChooseConfig(void* dpy, const int32_t* attrib_list, void* configs, int32_t config_size, int32_t* num_config);
void* eglCreateContext(void* dpy, void* config, void* share_context, const int32_t* attrib_list);
void* eglCreatePbufferSurface(void* dpy, void* config, const int32_t* attrib_list);
void* eglCreateWindowSurface(void* dpy, void* config, void* win, const int32_t* attrib_list);
uint32_t eglDestroyContext(void* dpy, void* ctx);
uint32_t eglDestroySurface(void* dpy, void* surface);
uint32_t eglGetConfigAttrib(void* dpy, void* config, int32_t attribute, int32_t* value);
void* eglGetDisplay(void* display_id);
int32_t eglGetError(void);
void* eglGetProcAddress(const char* procname);
uint32_t eglInitialize(void* dpy, int32_t* major, int32_t* minor);
uint32_t eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx);
const char* eglQueryString(void* dpy, int32_t name);
uint32_t eglQuerySurface(void* dpy, void* surface, int32_t attribute, int32_t* value);
uint32_t eglSwapBuffers(void* dpy, void* surface);
uint32_t eglTerminate(void* dpy);

void glActiveTexture(uint32_t texture);
void glAttachShader(uint32_t program, uint32_t shader);
void glBindAttribLocation(uint32_t program, uint32_t index, const char* name);
void glBindBuffer(uint32_t target, uint32_t buffer);
void glBindFramebuffer(uint32_t target, uint32_t framebuffer);
void glBindRenderbuffer(uint32_t target, uint32_t renderbuffer);
void glBindTexture(uint32_t target, uint32_t texture);
void glBlendColor(float red, float green, float blue, float alpha);
void glBlendEquation(uint32_t mode);
void glBlendEquationSeparate(uint32_t modeRGB, uint32_t modeAlpha);
void glBlendFunc(uint32_t sfactor, uint32_t dfactor);
void glBlendFuncSeparate(uint32_t sfactorRGB, uint32_t dfactorRGB, uint32_t sfactorAlpha, uint32_t dfactorAlpha);
void glBufferData(uint32_t target, long size, const void* data, uint32_t usage);
void glBufferSubData(uint32_t target, long offset, long size, const void* data);
uint32_t glCheckFramebufferStatus(uint32_t target);
void glClear(uint32_t mask);
void glClearColor(float red, float green, float blue, float alpha);
void glClearDepthf(float d);
void glClearStencil(int32_t s);
void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
void glCompileShader(uint32_t shader);
void glCompressedTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t border, int32_t imageSize, const void* data);
void glCompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, int32_t imageSize, const void* data);
void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y, int32_t width, int32_t height, int32_t border);
void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t width, int32_t height);
uint32_t glCreateProgram(void);
uint32_t glCreateShader(uint32_t type);
void glCullFace(uint32_t mode);
void glDeleteBuffers(int32_t n, const uint32_t* buffers);
void glDeleteFramebuffers(int32_t n, const uint32_t* framebuffers);
void glDeleteProgram(uint32_t program);
void glDeleteRenderbuffers(int32_t n, const uint32_t* renderbuffers);
void glDeleteShader(uint32_t shader);
void glDeleteTextures(int32_t n, const uint32_t* textures);
void glDepthFunc(uint32_t func);
void glDepthMask(uint8_t flag);
void glDepthRangef(float n, float f);
void glDetachShader(uint32_t program, uint32_t shader);
void glDisable(uint32_t cap);
void glDisableVertexAttribArray(uint32_t index);
void glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* indices);
void glEnable(uint32_t cap);
void glEnableVertexAttribArray(uint32_t index);
void glFinish(void);
void glFlush(void);
void glFramebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbuffertarget, uint32_t renderbuffer);
void glFramebufferTexture2D(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level);
void glFrontFace(uint32_t mode);
void glGenBuffers(int32_t n, uint32_t* buffers);
void glGenerateMipmap(uint32_t target);
void glGenFramebuffers(int32_t n, uint32_t* framebuffers);
void glGenRenderbuffers(int32_t n, uint32_t* renderbuffers);
void glGenTextures(int32_t n, uint32_t* textures);
void glGetActiveAttrib(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glGetActiveUniform(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glGetAttachedShaders(uint32_t program, int32_t maxCount, int32_t* count, uint32_t* shaders);
int32_t glGetAttribLocation(uint32_t program, const char* name);
void glGetBooleanv(uint32_t pname, uint8_t* data);
void glGetBufferParameteriv(uint32_t target, uint32_t pname, int32_t* params);
uint32_t glGetError(void);
void glGetFloatv(uint32_t pname, float* data);
void glGetFramebufferAttachmentParameteriv(uint32_t target, uint32_t attachment, uint32_t pname, int32_t* params);
void glGetIntegerv(uint32_t pname, int32_t* data);
void glGetProgramiv(uint32_t program, uint32_t pname, int32_t* params);
void glGetProgramInfoLog(uint32_t program, int32_t bufSize, int32_t* length, char* infoLog);
void glGetRenderbufferParameteriv(uint32_t target, uint32_t pname, int32_t* params);
void glGetShaderiv(uint32_t shader, uint32_t pname, int32_t* params);
void glGetShaderInfoLog(uint32_t shader, int32_t bufSize, int32_t* length, char* infoLog);
void glGetShaderPrecisionFormat(uint32_t shadertype, uint32_t precisiontype, int32_t* range, int32_t* precision);
void glGetShaderSource(uint32_t shader, int32_t bufSize, int32_t* length, char* source);
const uint8_t* glGetString(uint32_t name);
void glGetTexParameterfv(uint32_t target, uint32_t pname, float* params);
void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* params);
void glGetUniformfv(uint32_t program, int32_t location, float* params);
void glGetUniformiv(uint32_t program, int32_t location, int32_t* params);
int32_t glGetUniformLocation(uint32_t program, const char* name);
void glGetVertexAttribfv(uint32_t index, uint32_t pname, float* params);
void glGetVertexAttribiv(uint32_t index, uint32_t pname, int32_t* params);
void glGetVertexAttribPointerv(uint32_t index, uint32_t pname, void** pointer);
void glHint(uint32_t target, uint32_t mode);
uint8_t glIsBuffer(uint32_t buffer);
uint8_t glIsEnabled(uint32_t cap);
uint8_t glIsFramebuffer(uint32_t framebuffer);
uint8_t glIsProgram(uint32_t program);
uint8_t glIsRenderbuffer(uint32_t renderbuffer);
uint8_t glIsShader(uint32_t shader);
uint8_t glIsTexture(uint32_t texture);
void glLineWidth(float width);
void glLinkProgram(uint32_t program);
void glPixelStorei(uint32_t pname, int32_t param);
void glPolygonOffset(float factor, float units);
void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void* pixels);
void glReleaseShaderCompiler(void);
void glRenderbufferStorage(uint32_t target, uint32_t internalformat, int32_t width, int32_t height);
void glSampleCoverage(float value, uint8_t invert);
void glScissor(int32_t x, int32_t y, int32_t width, int32_t height);
void glShaderBinary(int32_t count, const uint32_t* shaders, uint32_t binaryformat, const void* binary, int32_t length);
void glShaderSource(uint32_t shader, int32_t count, const char** string, const int32_t* length);
void glStencilFunc(uint32_t func, int32_t ref, uint32_t mask);
void glStencilFuncSeparate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask);
void glStencilMask(uint32_t mask);
void glStencilMaskSeparate(uint32_t face, uint32_t mask);
void glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass);
void glStencilOpSeparate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void* pixels);
void glTexParameterf(uint32_t target, uint32_t pname, float param);
void glTexParameterfv(uint32_t target, uint32_t pname, const float* params);
void glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
void glTexParameteriv(uint32_t target, uint32_t pname, const int32_t* params);
void glTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels);
void glUniform1f(int32_t location, float v0);
void glUniform1fv(int32_t location, int32_t count, const float* value);
void glUniform1i(int32_t location, int32_t v0);
void glUniform1iv(int32_t location, int32_t count, const int32_t* value);
void glUniform2f(int32_t location, float v0, float v1);
void glUniform2fv(int32_t location, int32_t count, const float* value);
void glUniform2i(int32_t location, int32_t v0, int32_t v1);
void glUniform2iv(int32_t location, int32_t count, const int32_t* value);
void glUniform3f(int32_t location, float v0, float v1, float v2);
void glUniform3fv(int32_t location, int32_t count, const float* value);
void glUniform3i(int32_t location, int32_t v0, int32_t v1, int32_t v2);
void glUniform3iv(int32_t location, int32_t count, const int32_t* value);
void glUniform4f(int32_t location, float v0, float v1, float v2, float v3);
void glUniform4fv(int32_t location, int32_t count, const float* value);
void glUniform4i(int32_t location, int32_t v0, int32_t v1, int32_t v2, int32_t v3);
void glUniform4iv(int32_t location, int32_t count, const int32_t* value);
void glUniformMatrix2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUseProgram(uint32_t program);
void glValidateProgram(uint32_t program);
void glVertexAttrib1f(uint32_t index, float x);
void glVertexAttrib1fv(uint32_t index, const float* v);
void glVertexAttrib2f(uint32_t index, float x, float y);
void glVertexAttrib2fv(uint32_t index, const float* v);
void glVertexAttrib3f(uint32_t index, float x, float y, float z);
void glVertexAttrib3fv(uint32_t index, const float* v);
void glVertexAttrib4f(uint32_t index, float x, float y, float z, float w);
void glVertexAttrib4fv(uint32_t index, const float* v);
void glVertexAttribPointer(uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride, const void* pointer);
void glViewport(int32_t x, int32_t y, int32_t width, int32_t height);

#endif /* VIRTPASS_GL */
