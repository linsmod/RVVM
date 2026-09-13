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

#ifndef VIRTPASS_GL
#define VIRTPASS_GL

#include <stdint.h>

/* ============================================================
 * GL types (khronos widths, riscv64 LP64 guest)
 * GLES2 core, plus the GLES3 additions (64-bit integers, GLsync)
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
typedef long             GLint64;     /* khronos_int64_t  */
typedef unsigned long    GLuint64;    /* khronos_uint64_t */
/* Fence/sync object. Never dereferenced: the guest only hands it back, so the
 * opaque pointer form is all the guest side needs. */
typedef void*            GLsync;

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
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_STATIC_DRAW         0x88E4
#define GL_STREAM_DRAW         0x88E0
#define GL_DYNAMIC_DRAW        0x88E8
#define GL_FRAMEBUFFER         0x8D40
#define GL_RENDERBUFFER        0x8D41
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0   0x8CE0
#define GL_DEPTH_ATTACHMENT    0x8D00
#define GL_DEPTH_COMPONENT16   0x81A5

#define GL_DEPTH_TEST          0x0B71
#define GL_BLEND               0x0BE2
#define GL_CULL_FACE           0x0B44
#define GL_SCISSOR_TEST        0x0C11
#define GL_TEXTURE_2D          0x0DE1
#define GL_TEXTURE0            0x84C0

#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_NEAREST             0x2600
#define GL_LINEAR              0x2601
#define GL_CLAMP_TO_EDGE       0x812F

#define GL_RGBA8               0x8058
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT            0x0BA2
#define GL_MAX_TEXTURE_SIZE    0x0D33
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2

#define GL_COLOR_CLEAR_VALUE   0x0C22
#define GL_ACTIVE_UNIFORMS     0x8B86
#define GL_ACTIVE_ATTRIBUTES   0x8B89
#define GL_ATTACHED_SHADERS    0x8B85
#define GL_INFO_LOG_LENGTH     0x8B84

/* ============================================================
 * GLES3 constants (subset used by guests + host smoke tests)
 * ============================================================ */
#define GL_MAJOR_VERSION       0x821B
#define GL_MINOR_VERSION       0x821C
#define GL_NUM_EXTENSIONS      0x821D

#define GL_MAX_3D_TEXTURE_SIZE 0x8073
#define GL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#define GL_MAX_ELEMENT_INDEX   0x8D6B
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF

#define GL_R8                  0x8229
#define GL_RGB8                0x8051
#define GL_RGBA32F             0x8814
#define GL_RGB32F              0x8815
#define GL_DEPTH_COMPONENT24   0x81A6
#define GL_DEPTH24_STENCIL8    0x88F0

#define GL_TEXTURE_3D          0x806F
#define GL_TEXTURE_WRAP_R      0x8072
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D

#define GL_UNIFORM_BUFFER      0x8A11
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#define GL_UNIFORM_BLOCK_DATA_SIZE 0x8A40
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_INVALID_INDEX       0xFFFFFFFF
#define GL_BUFFER_SIZE         0x8764
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_READ_BUFFER         0x0C02
#define GL_READ_FRAMEBUFFER    0x8CA8
#define GL_DRAW_FRAMEBUFFER    0x8CA9
#define GL_COLOR_ATTACHMENT1   0x8CE1
#define GL_FRAMEBUFFER_SRGB    0x8DB9

#define GL_ANY_SAMPLES_PASSED  0x8C2F
#define GL_SAMPLES_PASSED      0x8914
#define GL_QUERY_RESULT        0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867

/* glMapBufferRange() access bits. The guest needs them to ask for a mapping,
 * and the host staging code inspects the same bits to decide whether to seed
 * the staging buffer and whether to write it back. */
#define GL_MAP_READ_BIT             0x0001
#define GL_MAP_WRITE_BIT            0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT  0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT   0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT   0x0020

#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT    0x00000001
#define GL_ALREADY_SIGNALED    0x911A
#define GL_TIMEOUT_EXPIRED     0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED         0x911D
#define GL_TIMEOUT_IGNORED     0xFFFFFFFFFFFFFFFFull

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
#define EGL_BAD_ATTRIBUTE      0x3004
#define EGL_BAD_CONFIG         0x3005
#define EGL_BAD_CONTEXT        0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY        0x3008
#define EGL_BAD_MATCH          0x3009
#define EGL_BAD_NATIVE_PIXMAP  0x300A
#define EGL_BAD_NATIVE_WINDOW  0x300B
#define EGL_BAD_PARAMETER      0x300C
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
#define EGL_OPENGL_ES3_BIT_KHR 0x0040

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
/* ES 3.x contexts. EGL_CONTEXT_MAJOR_VERSION is the very same token as
 * EGL_CONTEXT_CLIENT_VERSION (both 0x3098) - it is what additionally selects
 * the API version together with the minor version below (3.1/3.2 contexts). */
#define EGL_CONTEXT_MAJOR_VERSION  0x3098
#define EGL_CONTEXT_MINOR_VERSION  0x30FB
#define EGL_CONTEXT_OPENGL_DEBUG   0x31B0

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
#define GL_FN_READBUFFER 143
#define GL_FN_DRAWRANGEELEMENTS 144
#define GL_FN_TEXIMAGE3D 145
#define GL_FN_TEXSUBIMAGE3D 146
#define GL_FN_COPYTEXSUBIMAGE3D 147
#define GL_FN_COMPRESSEDTEXIMAGE3D 148
#define GL_FN_COMPRESSEDTEXSUBIMAGE3D 149
#define GL_FN_GENQUERIES 150
#define GL_FN_DELETEQUERIES 151
#define GL_FN_ISQUERY 152
#define GL_FN_BEGINQUERY 153
#define GL_FN_ENDQUERY 154
#define GL_FN_GETQUERYIV 155
#define GL_FN_GETQUERYOBJECTUIV 156
#define GL_FN_UNMAPBUFFER 157
#define GL_FN_GETBUFFERPOINTERV 158
#define GL_FN_DRAWBUFFERS 159
#define GL_FN_UNIFORMMATRIX2X3FV 160
#define GL_FN_UNIFORMMATRIX3X2FV 161
#define GL_FN_UNIFORMMATRIX2X4FV 162
#define GL_FN_UNIFORMMATRIX4X2FV 163
#define GL_FN_UNIFORMMATRIX3X4FV 164
#define GL_FN_UNIFORMMATRIX4X3FV 165
#define GL_FN_BLITFRAMEBUFFER 166
#define GL_FN_RENDERBUFFERSTORAGEMULTISAMPLE 167
#define GL_FN_FRAMEBUFFERTEXTURELAYER 168
#define GL_FN_MAPBUFFERRANGE 169
#define GL_FN_FLUSHMAPPEDBUFFERRANGE 170
#define GL_FN_BINDVERTEXARRAY 171
#define GL_FN_DELETEVERTEXARRAYS 172
#define GL_FN_GENVERTEXARRAYS 173
#define GL_FN_ISVERTEXARRAY 174
#define GL_FN_GETINTEGERI_V 175
#define GL_FN_BEGINTRANSFORMFEEDBACK 176
#define GL_FN_ENDTRANSFORMFEEDBACK 177
#define GL_FN_BINDBUFFERRANGE 178
#define GL_FN_BINDBUFFERBASE 179
#define GL_FN_TRANSFORMFEEDBACKVARYINGS 180
#define GL_FN_GETTRANSFORMFEEDBACKVARYING 181
#define GL_FN_VERTEXATTRIBIPOINTER 182
#define GL_FN_GETVERTEXATTRIBIIV 183
#define GL_FN_GETVERTEXATTRIBIUIV 184
#define GL_FN_VERTEXATTRIBI4I 185
#define GL_FN_VERTEXATTRIBI4UI 186
#define GL_FN_VERTEXATTRIBI4IV 187
#define GL_FN_VERTEXATTRIBI4UIV 188
#define GL_FN_GETUNIFORMUIV 189
#define GL_FN_GETFRAGDATALOCATION 190
#define GL_FN_UNIFORM1UI 191
#define GL_FN_UNIFORM2UI 192
#define GL_FN_UNIFORM3UI 193
#define GL_FN_UNIFORM4UI 194
#define GL_FN_UNIFORM1UIV 195
#define GL_FN_UNIFORM2UIV 196
#define GL_FN_UNIFORM3UIV 197
#define GL_FN_UNIFORM4UIV 198
#define GL_FN_CLEARBUFFERIV 199
#define GL_FN_CLEARBUFFERUIV 200
#define GL_FN_CLEARBUFFERFV 201
#define GL_FN_CLEARBUFFERFI 202
#define GL_FN_GETSTRINGI 203
#define GL_FN_COPYBUFFERSUBDATA 204
#define GL_FN_GETUNIFORMINDICES 205
#define GL_FN_GETACTIVEUNIFORMSIV 206
#define GL_FN_GETUNIFORMBLOCKINDEX 207
#define GL_FN_GETACTIVEUNIFORMBLOCKIV 208
#define GL_FN_GETACTIVEUNIFORMBLOCKNAME 209
#define GL_FN_UNIFORMBLOCKBINDING 210
#define GL_FN_DRAWARRAYSINSTANCED 211
#define GL_FN_DRAWELEMENTSINSTANCED 212
#define GL_FN_FENCESYNC 213
#define GL_FN_ISSYNC 214
#define GL_FN_DELETESYNC 215
#define GL_FN_CLIENTWAITSYNC 216
#define GL_FN_WAITSYNC 217
#define GL_FN_GETINTEGER64V 218
#define GL_FN_GETSYNCIV 219
#define GL_FN_GETINTEGER64I_V 220
#define GL_FN_GETBUFFERPARAMETERI64V 221
#define GL_FN_GENSAMPLERS 222
#define GL_FN_DELETESAMPLERS 223
#define GL_FN_ISSAMPLER 224
#define GL_FN_BINDSAMPLER 225
#define GL_FN_SAMPLERPARAMETERI 226
#define GL_FN_SAMPLERPARAMETERIV 227
#define GL_FN_SAMPLERPARAMETERF 228
#define GL_FN_SAMPLERPARAMETERFV 229
#define GL_FN_GETSAMPLERPARAMETERIV 230
#define GL_FN_GETSAMPLERPARAMETERFV 231
#define GL_FN_VERTEXATTRIBDIVISOR 232
#define GL_FN_BINDTRANSFORMFEEDBACK 233
#define GL_FN_DELETETRANSFORMFEEDBACKS 234
#define GL_FN_GENTRANSFORMFEEDBACKS 235
#define GL_FN_ISTRANSFORMFEEDBACK 236
#define GL_FN_PAUSETRANSFORMFEEDBACK 237
#define GL_FN_RESUMETRANSFORMFEEDBACK 238
#define GL_FN_GETPROGRAMBINARY 239
#define GL_FN_PROGRAMBINARY 240
#define GL_FN_PROGRAMPARAMETERI 241
#define GL_FN_INVALIDATEFRAMEBUFFER 242
#define GL_FN_INVALIDATESUBFRAMEBUFFER 243
#define GL_FN_TEXSTORAGE2D 244
#define GL_FN_TEXSTORAGE3D 245
#define GL_FN_GETINTERNALFORMATIV 246

#define EGL_FN_BASE 0x100
#define EGL_FN_CHOOSECONFIG 0x100
#define EGL_FN_CREATECONTEXT 0x101
#define EGL_FN_CREATEPBUFFERSURFACE 0x102
#define EGL_FN_CREATEWINDOWSURFACE 0x103
#define EGL_FN_DESTROYCONTEXT 0x104
#define EGL_FN_DESTROYSURFACE 0x105
#define EGL_FN_GETCONFIGATTRIB 0x106
#define EGL_FN_GETCURRENTDISPLAY 0x107
#define EGL_FN_GETCURRENTSURFACE 0x108
#define EGL_FN_GETDISPLAY 0x109
#define EGL_FN_GETERROR 0x10A
#define EGL_FN_INITIALIZE 0x10B
#define EGL_FN_MAKECURRENT 0x10C
#define EGL_FN_QUERYCONTEXT 0x10D
#define EGL_FN_QUERYSTRING 0x10E
#define EGL_FN_QUERYSURFACE 0x10F
#define EGL_FN_SWAPBUFFERS 0x110
#define EGL_FN_TERMINATE 0x111
#define EGL_FN_SWAPINTERVAL 0x112
#define EGL_FN_BINDAPI 0x113
#define EGL_FN_GETCURRENTCONTEXT 0x114

#define GL_CALL_MAX_ARGS 12
/* gl_call.args[] slot carrying the guest scratch buffer for the
 * calls returning a host-owned string (glGetString/glGetStringi/
 * eglQueryString). GL_CALL_MAX_ARGS is one wider than the widest real
 * call (glTexSubImage3D, 11), so this slot can never collide with a
 * parameter. The buffer must outlive the call: the stub owns it
 * statically. */
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
 * (glGetString/glGetStringi/eglQueryString): the host copies the string there
 * and answers with that guest address. The stub keeps it in a static, not on
 * its stack - the pointer outlives the call. Only the string-returning calls
 * use the slot (glGetStringi passes its index in args[1]); everywhere else it
 * is unused, and nothing can reach it as a parameter because it sits one slot
 * above the widest call.
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
void* eglGetCurrentDisplay(void);
void* eglGetCurrentSurface(int32_t readdraw);
void* eglGetDisplay(void* display_id);
int32_t eglGetError(void);
void* eglGetProcAddress(const char* procname);
uint32_t eglInitialize(void* dpy, int32_t* major, int32_t* minor);
uint32_t eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx);
uint32_t eglQueryContext(void* dpy, void* ctx, int32_t attribute, int32_t* value);
const char* eglQueryString(void* dpy, int32_t name);
uint32_t eglQuerySurface(void* dpy, void* surface, int32_t attribute, int32_t* value);
uint32_t eglSwapBuffers(void* dpy, void* surface);
uint32_t eglTerminate(void* dpy);
uint32_t eglSwapInterval(void* dpy, int32_t interval);
uint32_t eglBindAPI(uint32_t api);
void* eglGetCurrentContext(void);

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
void glReadBuffer(uint32_t src);
void glDrawRangeElements(uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, const void* indices);
void glTexImage3D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, uint32_t format, uint32_t type, const void* pixels);
void glTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, uint32_t type, const void* pixels);
void glCopyTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t x, int32_t y, int32_t width, int32_t height);
void glCompressedTexImage3D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, int32_t imageSize, const void* data);
void glCompressedTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, int32_t imageSize, const void* data);
void glGenQueries(int32_t n, uint32_t* ids);
void glDeleteQueries(int32_t n, const uint32_t* ids);
uint8_t glIsQuery(uint32_t id);
void glBeginQuery(uint32_t target, uint32_t id);
void glEndQuery(uint32_t target);
void glGetQueryiv(uint32_t target, uint32_t pname, int32_t* params);
void glGetQueryObjectuiv(uint32_t id, uint32_t pname, uint32_t* params);
uint8_t glUnmapBuffer(uint32_t target);
void glGetBufferPointerv(uint32_t target, uint32_t pname, void** params);
void glDrawBuffers(int32_t n, const uint32_t* bufs);
void glUniformMatrix2x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix2x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glBlitFramebuffer(int32_t srcX0, int32_t srcY0, int32_t srcX1, int32_t srcY1, int32_t dstX0, int32_t dstY0, int32_t dstX1, int32_t dstY1, uint32_t mask, uint32_t filter);
void glRenderbufferStorageMultisample(uint32_t target, int32_t samples, uint32_t internalformat, int32_t width, int32_t height);
void glFramebufferTextureLayer(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level, int32_t layer);
void* glMapBufferRange(uint32_t target, long offset, long length, uint32_t access);
void glFlushMappedBufferRange(uint32_t target, long offset, long length);
void glBindVertexArray(uint32_t array);
void glDeleteVertexArrays(int32_t n, const uint32_t* arrays);
void glGenVertexArrays(int32_t n, uint32_t* arrays);
uint8_t glIsVertexArray(uint32_t array);
void glGetIntegeri_v(uint32_t target, uint32_t index, int32_t* data);
void glBeginTransformFeedback(uint32_t primitiveMode);
void glEndTransformFeedback(void);
void glBindBufferRange(uint32_t target, uint32_t index, uint32_t buffer, long offset, long size);
void glBindBufferBase(uint32_t target, uint32_t index, uint32_t buffer);
void glTransformFeedbackVaryings(uint32_t program, int32_t count, const char** varyings, uint32_t bufferMode);
void glGetTransformFeedbackVarying(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glVertexAttribIPointer(uint32_t index, int32_t size, uint32_t type, int32_t stride, const void* pointer);
void glGetVertexAttribIiv(uint32_t index, uint32_t pname, int32_t* params);
void glGetVertexAttribIuiv(uint32_t index, uint32_t pname, uint32_t* params);
void glVertexAttribI4i(uint32_t index, int32_t x, int32_t y, int32_t z, int32_t w);
void glVertexAttribI4ui(uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w);
void glVertexAttribI4iv(uint32_t index, const int32_t* v);
void glVertexAttribI4uiv(uint32_t index, const uint32_t* v);
void glGetUniformuiv(uint32_t program, int32_t location, uint32_t* params);
int32_t glGetFragDataLocation(uint32_t program, const char* name);
void glUniform1ui(int32_t location, uint32_t v0);
void glUniform2ui(int32_t location, uint32_t v0, uint32_t v1);
void glUniform3ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2);
void glUniform4ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3);
void glUniform1uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform2uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform3uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform4uiv(int32_t location, int32_t count, const uint32_t* value);
void glClearBufferiv(uint32_t buffer, int32_t drawbuffer, const int32_t* value);
void glClearBufferuiv(uint32_t buffer, int32_t drawbuffer, const uint32_t* value);
void glClearBufferfv(uint32_t buffer, int32_t drawbuffer, const float* value);
void glClearBufferfi(uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil);
const uint8_t* glGetStringi(uint32_t name, uint32_t index);
void glCopyBufferSubData(uint32_t readTarget, uint32_t writeTarget, long readOffset, long writeOffset, long size);
void glGetUniformIndices(uint32_t program, int32_t uniformCount, const char** uniformNames, uint32_t* uniformIndices);
void glGetActiveUniformsiv(uint32_t program, int32_t uniformCount, const uint32_t* uniformIndices, uint32_t pname, int32_t* params);
uint32_t glGetUniformBlockIndex(uint32_t program, const char* uniformBlockName);
void glGetActiveUniformBlockiv(uint32_t program, uint32_t uniformBlockIndex, uint32_t pname, int32_t* params);
void glGetActiveUniformBlockName(uint32_t program, uint32_t uniformBlockIndex, int32_t bufSize, int32_t* length, char* uniformBlockName);
void glUniformBlockBinding(uint32_t program, uint32_t uniformBlockIndex, uint32_t uniformBlockBinding);
void glDrawArraysInstanced(uint32_t mode, int32_t first, int32_t count, int32_t instancecount);
void glDrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type, const void* indices, int32_t instancecount);
void* glFenceSync(uint32_t condition, uint32_t flags);
uint8_t glIsSync(void* sync);
void glDeleteSync(void* sync);
uint32_t glClientWaitSync(void* sync, uint32_t flags, uint64_t timeout);
void glWaitSync(void* sync, uint32_t flags, uint64_t timeout);
void glGetInteger64v(uint32_t pname, int64_t* data);
void glGetSynciv(void* sync, uint32_t pname, int32_t bufSize, int32_t* length, int32_t* values);
void glGetInteger64i_v(uint32_t target, uint32_t index, int64_t* data);
void glGetBufferParameteri64v(uint32_t target, uint32_t pname, int64_t* params);
void glGenSamplers(int32_t count, uint32_t* samplers);
void glDeleteSamplers(int32_t count, const uint32_t* samplers);
uint8_t glIsSampler(uint32_t sampler);
void glBindSampler(uint32_t unit, uint32_t sampler);
void glSamplerParameteri(uint32_t sampler, uint32_t pname, int32_t param);
void glSamplerParameteriv(uint32_t sampler, uint32_t pname, const int32_t* param);
void glSamplerParameterf(uint32_t sampler, uint32_t pname, float param);
void glSamplerParameterfv(uint32_t sampler, uint32_t pname, const float* param);
void glGetSamplerParameteriv(uint32_t sampler, uint32_t pname, int32_t* params);
void glGetSamplerParameterfv(uint32_t sampler, uint32_t pname, float* params);
void glVertexAttribDivisor(uint32_t index, uint32_t divisor);
void glBindTransformFeedback(uint32_t target, uint32_t id);
void glDeleteTransformFeedbacks(int32_t n, const uint32_t* ids);
void glGenTransformFeedbacks(int32_t n, uint32_t* ids);
uint8_t glIsTransformFeedback(uint32_t id);
void glPauseTransformFeedback(void);
void glResumeTransformFeedback(void);
void glGetProgramBinary(uint32_t program, int32_t bufSize, int32_t* length, uint32_t* binaryFormat, void* binary);
void glProgramBinary(uint32_t program, uint32_t binaryFormat, const void* binary, int32_t length);
void glProgramParameteri(uint32_t program, uint32_t pname, int32_t value);
void glInvalidateFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments);
void glInvalidateSubFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments, int32_t x, int32_t y, int32_t width, int32_t height);
void glTexStorage2D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height);
void glTexStorage3D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height, int32_t depth);
void glGetInternalformativ(uint32_t target, uint32_t internalformat, uint32_t pname, int32_t bufSize, int32_t* params);

#endif /* VIRTPASS_GL */
