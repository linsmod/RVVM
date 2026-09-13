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
 * Generic dispatch switches, included from each host's dispatch TU
 * (win32-host/win32_gl_dispatch.c, android-host .../android_gl_host.c).
 * Do not include from anywhere else (defines static functions).
 * `a` is the const int64_t* args array, `ret` the int64_t* out.
 * Guest data pointers are translated by the vpgl_gptr*() helpers
 * defined in the including TU above this include, and an entry point
 * that was never resolved reports through vpgl_missing() from the
 * same TU: without it a NULL p_* silently swallowed the call.
 */

/* memcpy: the glMapBufferRange() staging below copies whole ranges
 * between the host mapping and the guest staging buffer. */
#include <string.h>

/* fn_id -> name, for the RVVM_GL_TRACE call log */
static const char* vpgl_egl_name(uint32_t fn_id)
{
    switch (fn_id) {
    case EGL_FN_CHOOSECONFIG: return "eglChooseConfig";
    case EGL_FN_CREATECONTEXT: return "eglCreateContext";
    case EGL_FN_CREATEPBUFFERSURFACE: return "eglCreatePbufferSurface";
    case EGL_FN_CREATEWINDOWSURFACE: return "eglCreateWindowSurface";
    case EGL_FN_DESTROYCONTEXT: return "eglDestroyContext";
    case EGL_FN_DESTROYSURFACE: return "eglDestroySurface";
    case EGL_FN_GETCONFIGATTRIB: return "eglGetConfigAttrib";
    case EGL_FN_GETCURRENTDISPLAY: return "eglGetCurrentDisplay";
    case EGL_FN_GETCURRENTSURFACE: return "eglGetCurrentSurface";
    case EGL_FN_GETDISPLAY: return "eglGetDisplay";
    case EGL_FN_GETERROR: return "eglGetError";
    case EGL_FN_INITIALIZE: return "eglInitialize";
    case EGL_FN_MAKECURRENT: return "eglMakeCurrent";
    case EGL_FN_QUERYCONTEXT: return "eglQueryContext";
    case EGL_FN_QUERYSTRING: return "eglQueryString";
    case EGL_FN_QUERYSURFACE: return "eglQuerySurface";
    case EGL_FN_SWAPBUFFERS: return "eglSwapBuffers";
    case EGL_FN_TERMINATE: return "eglTerminate";
    case EGL_FN_SWAPINTERVAL: return "eglSwapInterval";
    case EGL_FN_BINDAPI: return "eglBindAPI";
    case EGL_FN_GETCURRENTCONTEXT: return "eglGetCurrentContext";
    default: return NULL;
    }
}

static const char* vpgl_gl_name(uint32_t fn_id)
{
    switch (fn_id) {
    case GL_FN_ACTIVETEXTURE: return "glActiveTexture";
    case GL_FN_ATTACHSHADER: return "glAttachShader";
    case GL_FN_BINDATTRIBLOCATION: return "glBindAttribLocation";
    case GL_FN_BINDBUFFER: return "glBindBuffer";
    case GL_FN_BINDFRAMEBUFFER: return "glBindFramebuffer";
    case GL_FN_BINDRENDERBUFFER: return "glBindRenderbuffer";
    case GL_FN_BINDTEXTURE: return "glBindTexture";
    case GL_FN_BLENDCOLOR: return "glBlendColor";
    case GL_FN_BLENDEQUATION: return "glBlendEquation";
    case GL_FN_BLENDEQUATIONSEPARATE: return "glBlendEquationSeparate";
    case GL_FN_BLENDFUNC: return "glBlendFunc";
    case GL_FN_BLENDFUNCSEPARATE: return "glBlendFuncSeparate";
    case GL_FN_BUFFERDATA: return "glBufferData";
    case GL_FN_BUFFERSUBDATA: return "glBufferSubData";
    case GL_FN_CHECKFRAMEBUFFERSTATUS: return "glCheckFramebufferStatus";
    case GL_FN_CLEAR: return "glClear";
    case GL_FN_CLEARCOLOR: return "glClearColor";
    case GL_FN_CLEARDEPTHF: return "glClearDepthf";
    case GL_FN_CLEARSTENCIL: return "glClearStencil";
    case GL_FN_COLORMASK: return "glColorMask";
    case GL_FN_COMPILESHADER: return "glCompileShader";
    case GL_FN_COMPRESSEDTEXIMAGE2D: return "glCompressedTexImage2D";
    case GL_FN_COMPRESSEDTEXSUBIMAGE2D: return "glCompressedTexSubImage2D";
    case GL_FN_COPYTEXIMAGE2D: return "glCopyTexImage2D";
    case GL_FN_COPYTEXSUBIMAGE2D: return "glCopyTexSubImage2D";
    case GL_FN_CREATEPROGRAM: return "glCreateProgram";
    case GL_FN_CREATESHADER: return "glCreateShader";
    case GL_FN_CULLFACE: return "glCullFace";
    case GL_FN_DELETEBUFFERS: return "glDeleteBuffers";
    case GL_FN_DELETEFRAMEBUFFERS: return "glDeleteFramebuffers";
    case GL_FN_DELETEPROGRAM: return "glDeleteProgram";
    case GL_FN_DELETERENDERBUFFERS: return "glDeleteRenderbuffers";
    case GL_FN_DELETESHADER: return "glDeleteShader";
    case GL_FN_DELETETEXTURES: return "glDeleteTextures";
    case GL_FN_DEPTHFUNC: return "glDepthFunc";
    case GL_FN_DEPTHMASK: return "glDepthMask";
    case GL_FN_DEPTHRANGEF: return "glDepthRangef";
    case GL_FN_DETACHSHADER: return "glDetachShader";
    case GL_FN_DISABLE: return "glDisable";
    case GL_FN_DISABLEVERTEXATTRIBARRAY: return "glDisableVertexAttribArray";
    case GL_FN_DRAWARRAYS: return "glDrawArrays";
    case GL_FN_DRAWELEMENTS: return "glDrawElements";
    case GL_FN_ENABLE: return "glEnable";
    case GL_FN_ENABLEVERTEXATTRIBARRAY: return "glEnableVertexAttribArray";
    case GL_FN_FINISH: return "glFinish";
    case GL_FN_FLUSH: return "glFlush";
    case GL_FN_FRAMEBUFFERRENDERBUFFER: return "glFramebufferRenderbuffer";
    case GL_FN_FRAMEBUFFERTEXTURE2D: return "glFramebufferTexture2D";
    case GL_FN_FRONTFACE: return "glFrontFace";
    case GL_FN_GENBUFFERS: return "glGenBuffers";
    case GL_FN_GENERATEMIPMAP: return "glGenerateMipmap";
    case GL_FN_GENFRAMEBUFFERS: return "glGenFramebuffers";
    case GL_FN_GENRENDERBUFFERS: return "glGenRenderbuffers";
    case GL_FN_GENTEXTURES: return "glGenTextures";
    case GL_FN_GETACTIVEATTRIB: return "glGetActiveAttrib";
    case GL_FN_GETACTIVEUNIFORM: return "glGetActiveUniform";
    case GL_FN_GETATTACHEDSHADERS: return "glGetAttachedShaders";
    case GL_FN_GETATTRIBLOCATION: return "glGetAttribLocation";
    case GL_FN_GETBOOLEANV: return "glGetBooleanv";
    case GL_FN_GETBUFFERPARAMETERIV: return "glGetBufferParameteriv";
    case GL_FN_GETERROR: return "glGetError";
    case GL_FN_GETFLOATV: return "glGetFloatv";
    case GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV: return "glGetFramebufferAttachmentParameteriv";
    case GL_FN_GETINTEGERV: return "glGetIntegerv";
    case GL_FN_GETPROGRAMIV: return "glGetProgramiv";
    case GL_FN_GETPROGRAMINFOLOG: return "glGetProgramInfoLog";
    case GL_FN_GETRENDERBUFFERPARAMETERIV: return "glGetRenderbufferParameteriv";
    case GL_FN_GETSHADERIV: return "glGetShaderiv";
    case GL_FN_GETSHADERINFOLOG: return "glGetShaderInfoLog";
    case GL_FN_GETSHADERPRECISIONFORMAT: return "glGetShaderPrecisionFormat";
    case GL_FN_GETSHADERSOURCE: return "glGetShaderSource";
    case GL_FN_GETSTRING: return "glGetString";
    case GL_FN_GETTEXPARAMETERFV: return "glGetTexParameterfv";
    case GL_FN_GETTEXPARAMETERIV: return "glGetTexParameteriv";
    case GL_FN_GETUNIFORMFV: return "glGetUniformfv";
    case GL_FN_GETUNIFORMIV: return "glGetUniformiv";
    case GL_FN_GETUNIFORMLOCATION: return "glGetUniformLocation";
    case GL_FN_GETVERTEXATTRIBFV: return "glGetVertexAttribfv";
    case GL_FN_GETVERTEXATTRIBIV: return "glGetVertexAttribiv";
    case GL_FN_GETVERTEXATTRIBPOINTERV: return "glGetVertexAttribPointerv";
    case GL_FN_HINT: return "glHint";
    case GL_FN_ISBUFFER: return "glIsBuffer";
    case GL_FN_ISENABLED: return "glIsEnabled";
    case GL_FN_ISFRAMEBUFFER: return "glIsFramebuffer";
    case GL_FN_ISPROGRAM: return "glIsProgram";
    case GL_FN_ISRENDERBUFFER: return "glIsRenderbuffer";
    case GL_FN_ISSHADER: return "glIsShader";
    case GL_FN_ISTEXTURE: return "glIsTexture";
    case GL_FN_LINEWIDTH: return "glLineWidth";
    case GL_FN_LINKPROGRAM: return "glLinkProgram";
    case GL_FN_PIXELSTOREI: return "glPixelStorei";
    case GL_FN_POLYGONOFFSET: return "glPolygonOffset";
    case GL_FN_READPIXELS: return "glReadPixels";
    case GL_FN_RELEASESHADERCOMPILER: return "glReleaseShaderCompiler";
    case GL_FN_RENDERBUFFERSTORAGE: return "glRenderbufferStorage";
    case GL_FN_SAMPLECOVERAGE: return "glSampleCoverage";
    case GL_FN_SCISSOR: return "glScissor";
    case GL_FN_SHADERBINARY: return "glShaderBinary";
    case GL_FN_SHADERSOURCE: return "glShaderSource";
    case GL_FN_STENCILFUNC: return "glStencilFunc";
    case GL_FN_STENCILFUNCSEPARATE: return "glStencilFuncSeparate";
    case GL_FN_STENCILMASK: return "glStencilMask";
    case GL_FN_STENCILMASKSEPARATE: return "glStencilMaskSeparate";
    case GL_FN_STENCILOP: return "glStencilOp";
    case GL_FN_STENCILOPSEPARATE: return "glStencilOpSeparate";
    case GL_FN_TEXIMAGE2D: return "glTexImage2D";
    case GL_FN_TEXPARAMETERF: return "glTexParameterf";
    case GL_FN_TEXPARAMETERFV: return "glTexParameterfv";
    case GL_FN_TEXPARAMETERI: return "glTexParameteri";
    case GL_FN_TEXPARAMETERIV: return "glTexParameteriv";
    case GL_FN_TEXSUBIMAGE2D: return "glTexSubImage2D";
    case GL_FN_UNIFORM1F: return "glUniform1f";
    case GL_FN_UNIFORM1FV: return "glUniform1fv";
    case GL_FN_UNIFORM1I: return "glUniform1i";
    case GL_FN_UNIFORM1IV: return "glUniform1iv";
    case GL_FN_UNIFORM2F: return "glUniform2f";
    case GL_FN_UNIFORM2FV: return "glUniform2fv";
    case GL_FN_UNIFORM2I: return "glUniform2i";
    case GL_FN_UNIFORM2IV: return "glUniform2iv";
    case GL_FN_UNIFORM3F: return "glUniform3f";
    case GL_FN_UNIFORM3FV: return "glUniform3fv";
    case GL_FN_UNIFORM3I: return "glUniform3i";
    case GL_FN_UNIFORM3IV: return "glUniform3iv";
    case GL_FN_UNIFORM4F: return "glUniform4f";
    case GL_FN_UNIFORM4FV: return "glUniform4fv";
    case GL_FN_UNIFORM4I: return "glUniform4i";
    case GL_FN_UNIFORM4IV: return "glUniform4iv";
    case GL_FN_UNIFORMMATRIX2FV: return "glUniformMatrix2fv";
    case GL_FN_UNIFORMMATRIX3FV: return "glUniformMatrix3fv";
    case GL_FN_UNIFORMMATRIX4FV: return "glUniformMatrix4fv";
    case GL_FN_USEPROGRAM: return "glUseProgram";
    case GL_FN_VALIDATEPROGRAM: return "glValidateProgram";
    case GL_FN_VERTEXATTRIB1F: return "glVertexAttrib1f";
    case GL_FN_VERTEXATTRIB1FV: return "glVertexAttrib1fv";
    case GL_FN_VERTEXATTRIB2F: return "glVertexAttrib2f";
    case GL_FN_VERTEXATTRIB2FV: return "glVertexAttrib2fv";
    case GL_FN_VERTEXATTRIB3F: return "glVertexAttrib3f";
    case GL_FN_VERTEXATTRIB3FV: return "glVertexAttrib3fv";
    case GL_FN_VERTEXATTRIB4F: return "glVertexAttrib4f";
    case GL_FN_VERTEXATTRIB4FV: return "glVertexAttrib4fv";
    case GL_FN_VERTEXATTRIBPOINTER: return "glVertexAttribPointer";
    case GL_FN_VIEWPORT: return "glViewport";
    case GL_FN_READBUFFER: return "glReadBuffer";
    case GL_FN_DRAWRANGEELEMENTS: return "glDrawRangeElements";
    case GL_FN_TEXIMAGE3D: return "glTexImage3D";
    case GL_FN_TEXSUBIMAGE3D: return "glTexSubImage3D";
    case GL_FN_COPYTEXSUBIMAGE3D: return "glCopyTexSubImage3D";
    case GL_FN_COMPRESSEDTEXIMAGE3D: return "glCompressedTexImage3D";
    case GL_FN_COMPRESSEDTEXSUBIMAGE3D: return "glCompressedTexSubImage3D";
    case GL_FN_GENQUERIES: return "glGenQueries";
    case GL_FN_DELETEQUERIES: return "glDeleteQueries";
    case GL_FN_ISQUERY: return "glIsQuery";
    case GL_FN_BEGINQUERY: return "glBeginQuery";
    case GL_FN_ENDQUERY: return "glEndQuery";
    case GL_FN_GETQUERYIV: return "glGetQueryiv";
    case GL_FN_GETQUERYOBJECTUIV: return "glGetQueryObjectuiv";
    case GL_FN_UNMAPBUFFER: return "glUnmapBuffer";
    case GL_FN_GETBUFFERPOINTERV: return "glGetBufferPointerv";
    case GL_FN_DRAWBUFFERS: return "glDrawBuffers";
    case GL_FN_UNIFORMMATRIX2X3FV: return "glUniformMatrix2x3fv";
    case GL_FN_UNIFORMMATRIX3X2FV: return "glUniformMatrix3x2fv";
    case GL_FN_UNIFORMMATRIX2X4FV: return "glUniformMatrix2x4fv";
    case GL_FN_UNIFORMMATRIX4X2FV: return "glUniformMatrix4x2fv";
    case GL_FN_UNIFORMMATRIX3X4FV: return "glUniformMatrix3x4fv";
    case GL_FN_UNIFORMMATRIX4X3FV: return "glUniformMatrix4x3fv";
    case GL_FN_BLITFRAMEBUFFER: return "glBlitFramebuffer";
    case GL_FN_RENDERBUFFERSTORAGEMULTISAMPLE: return "glRenderbufferStorageMultisample";
    case GL_FN_FRAMEBUFFERTEXTURELAYER: return "glFramebufferTextureLayer";
    case GL_FN_MAPBUFFERRANGE: return "glMapBufferRange";
    case GL_FN_FLUSHMAPPEDBUFFERRANGE: return "glFlushMappedBufferRange";
    case GL_FN_BINDVERTEXARRAY: return "glBindVertexArray";
    case GL_FN_DELETEVERTEXARRAYS: return "glDeleteVertexArrays";
    case GL_FN_GENVERTEXARRAYS: return "glGenVertexArrays";
    case GL_FN_ISVERTEXARRAY: return "glIsVertexArray";
    case GL_FN_GETINTEGERI_V: return "glGetIntegeri_v";
    case GL_FN_BEGINTRANSFORMFEEDBACK: return "glBeginTransformFeedback";
    case GL_FN_ENDTRANSFORMFEEDBACK: return "glEndTransformFeedback";
    case GL_FN_BINDBUFFERRANGE: return "glBindBufferRange";
    case GL_FN_BINDBUFFERBASE: return "glBindBufferBase";
    case GL_FN_TRANSFORMFEEDBACKVARYINGS: return "glTransformFeedbackVaryings";
    case GL_FN_GETTRANSFORMFEEDBACKVARYING: return "glGetTransformFeedbackVarying";
    case GL_FN_VERTEXATTRIBIPOINTER: return "glVertexAttribIPointer";
    case GL_FN_GETVERTEXATTRIBIIV: return "glGetVertexAttribIiv";
    case GL_FN_GETVERTEXATTRIBIUIV: return "glGetVertexAttribIuiv";
    case GL_FN_VERTEXATTRIBI4I: return "glVertexAttribI4i";
    case GL_FN_VERTEXATTRIBI4UI: return "glVertexAttribI4ui";
    case GL_FN_VERTEXATTRIBI4IV: return "glVertexAttribI4iv";
    case GL_FN_VERTEXATTRIBI4UIV: return "glVertexAttribI4uiv";
    case GL_FN_GETUNIFORMUIV: return "glGetUniformuiv";
    case GL_FN_GETFRAGDATALOCATION: return "glGetFragDataLocation";
    case GL_FN_UNIFORM1UI: return "glUniform1ui";
    case GL_FN_UNIFORM2UI: return "glUniform2ui";
    case GL_FN_UNIFORM3UI: return "glUniform3ui";
    case GL_FN_UNIFORM4UI: return "glUniform4ui";
    case GL_FN_UNIFORM1UIV: return "glUniform1uiv";
    case GL_FN_UNIFORM2UIV: return "glUniform2uiv";
    case GL_FN_UNIFORM3UIV: return "glUniform3uiv";
    case GL_FN_UNIFORM4UIV: return "glUniform4uiv";
    case GL_FN_CLEARBUFFERIV: return "glClearBufferiv";
    case GL_FN_CLEARBUFFERUIV: return "glClearBufferuiv";
    case GL_FN_CLEARBUFFERFV: return "glClearBufferfv";
    case GL_FN_CLEARBUFFERFI: return "glClearBufferfi";
    case GL_FN_GETSTRINGI: return "glGetStringi";
    case GL_FN_COPYBUFFERSUBDATA: return "glCopyBufferSubData";
    case GL_FN_GETUNIFORMINDICES: return "glGetUniformIndices";
    case GL_FN_GETACTIVEUNIFORMSIV: return "glGetActiveUniformsiv";
    case GL_FN_GETUNIFORMBLOCKINDEX: return "glGetUniformBlockIndex";
    case GL_FN_GETACTIVEUNIFORMBLOCKIV: return "glGetActiveUniformBlockiv";
    case GL_FN_GETACTIVEUNIFORMBLOCKNAME: return "glGetActiveUniformBlockName";
    case GL_FN_UNIFORMBLOCKBINDING: return "glUniformBlockBinding";
    case GL_FN_DRAWARRAYSINSTANCED: return "glDrawArraysInstanced";
    case GL_FN_DRAWELEMENTSINSTANCED: return "glDrawElementsInstanced";
    case GL_FN_FENCESYNC: return "glFenceSync";
    case GL_FN_ISSYNC: return "glIsSync";
    case GL_FN_DELETESYNC: return "glDeleteSync";
    case GL_FN_CLIENTWAITSYNC: return "glClientWaitSync";
    case GL_FN_WAITSYNC: return "glWaitSync";
    case GL_FN_GETINTEGER64V: return "glGetInteger64v";
    case GL_FN_GETSYNCIV: return "glGetSynciv";
    case GL_FN_GETINTEGER64I_V: return "glGetInteger64i_v";
    case GL_FN_GETBUFFERPARAMETERI64V: return "glGetBufferParameteri64v";
    case GL_FN_GENSAMPLERS: return "glGenSamplers";
    case GL_FN_DELETESAMPLERS: return "glDeleteSamplers";
    case GL_FN_ISSAMPLER: return "glIsSampler";
    case GL_FN_BINDSAMPLER: return "glBindSampler";
    case GL_FN_SAMPLERPARAMETERI: return "glSamplerParameteri";
    case GL_FN_SAMPLERPARAMETERIV: return "glSamplerParameteriv";
    case GL_FN_SAMPLERPARAMETERF: return "glSamplerParameterf";
    case GL_FN_SAMPLERPARAMETERFV: return "glSamplerParameterfv";
    case GL_FN_GETSAMPLERPARAMETERIV: return "glGetSamplerParameteriv";
    case GL_FN_GETSAMPLERPARAMETERFV: return "glGetSamplerParameterfv";
    case GL_FN_VERTEXATTRIBDIVISOR: return "glVertexAttribDivisor";
    case GL_FN_BINDTRANSFORMFEEDBACK: return "glBindTransformFeedback";
    case GL_FN_DELETETRANSFORMFEEDBACKS: return "glDeleteTransformFeedbacks";
    case GL_FN_GENTRANSFORMFEEDBACKS: return "glGenTransformFeedbacks";
    case GL_FN_ISTRANSFORMFEEDBACK: return "glIsTransformFeedback";
    case GL_FN_PAUSETRANSFORMFEEDBACK: return "glPauseTransformFeedback";
    case GL_FN_RESUMETRANSFORMFEEDBACK: return "glResumeTransformFeedback";
    case GL_FN_GETPROGRAMBINARY: return "glGetProgramBinary";
    case GL_FN_PROGRAMBINARY: return "glProgramBinary";
    case GL_FN_PROGRAMPARAMETERI: return "glProgramParameteri";
    case GL_FN_INVALIDATEFRAMEBUFFER: return "glInvalidateFramebuffer";
    case GL_FN_INVALIDATESUBFRAMEBUFFER: return "glInvalidateSubFramebuffer";
    case GL_FN_TEXSTORAGE2D: return "glTexStorage2D";
    case GL_FN_TEXSTORAGE3D: return "glTexStorage3D";
    case GL_FN_GETINTERNALFORMATIV: return "glGetInternalformativ";
    default: return NULL;
    }
}

static void vpgl_dispatch_egl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case EGL_FN_CHOOSECONFIG:
        if (p_eglChooseConfig) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglChooseConfig)p_eglChooseConfig)((vpgl_void*)(uintptr_t)a[0], (const vpgl_EGLint*)vpgl_gptr(a[1]), (vpgl_EGLConfig*)vpgl_gptr(a[2]), (vpgl_EGLint)a[3], (vpgl_EGLint*)vpgl_gptr(a[4])); }
        else vpgl_missing("eglChooseConfig");
        break;
    case EGL_FN_CREATECONTEXT:
        if (p_eglCreateContext) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreateContext)p_eglCreateContext)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (const vpgl_EGLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("eglCreateContext");
        break;
    case EGL_FN_CREATEPBUFFERSURFACE:
        if (p_eglCreatePbufferSurface) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreatePbufferSurface)p_eglCreatePbufferSurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (const vpgl_EGLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("eglCreatePbufferSurface");
        break;
    case EGL_FN_CREATEWINDOWSURFACE:
        if (p_eglCreateWindowSurface) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreateWindowSurface)p_eglCreateWindowSurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (const vpgl_EGLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("eglCreateWindowSurface");
        break;
    case EGL_FN_DESTROYCONTEXT:
        if (p_eglDestroyContext) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglDestroyContext)p_eglDestroyContext)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        else vpgl_missing("eglDestroyContext");
        break;
    case EGL_FN_DESTROYSURFACE:
        if (p_eglDestroySurface) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglDestroySurface)p_eglDestroySurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        else vpgl_missing("eglDestroySurface");
        break;
    case EGL_FN_GETCONFIGATTRIB:
        if (p_eglGetConfigAttrib) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglGetConfigAttrib)p_eglGetConfigAttrib)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_EGLint)a[2], (vpgl_EGLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("eglGetConfigAttrib");
        break;
    case EGL_FN_GETCURRENTDISPLAY:
        if (p_eglGetCurrentDisplay) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglGetCurrentDisplay)p_eglGetCurrentDisplay)(); }
        else vpgl_missing("eglGetCurrentDisplay");
        break;
    case EGL_FN_GETCURRENTSURFACE:
        if (p_eglGetCurrentSurface) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglGetCurrentSurface)p_eglGetCurrentSurface)((vpgl_EGLint)a[0]); }
        else vpgl_missing("eglGetCurrentSurface");
        break;
    case EGL_FN_GETDISPLAY:
        if (p_eglGetDisplay) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglGetDisplay)p_eglGetDisplay)((vpgl_void*)(uintptr_t)a[0]); }
        else vpgl_missing("eglGetDisplay");
        break;
    case EGL_FN_GETERROR:
        if (p_eglGetError) { *ret = (int64_t)(int32_t)((vpgl_PFN_eglGetError)p_eglGetError)(); }
        else vpgl_missing("eglGetError");
        break;
    case EGL_FN_INITIALIZE:
        if (p_eglInitialize) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglInitialize)p_eglInitialize)((vpgl_void*)(uintptr_t)a[0], (vpgl_EGLint*)vpgl_gptr(a[1]), (vpgl_EGLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("eglInitialize");
        break;
    case EGL_FN_MAKECURRENT:
        if (p_eglMakeCurrent) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglMakeCurrent)p_eglMakeCurrent)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (vpgl_void*)(uintptr_t)a[3]); }
        else vpgl_missing("eglMakeCurrent");
        break;
    case EGL_FN_QUERYCONTEXT:
        if (p_eglQueryContext) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglQueryContext)p_eglQueryContext)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_EGLint)a[2], (vpgl_EGLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("eglQueryContext");
        break;
    case EGL_FN_QUERYSTRING:
        if (p_eglQueryString) { *ret = vpgl_string_out(a, (const char*)((vpgl_PFN_eglQueryString)p_eglQueryString)((vpgl_void*)(uintptr_t)a[0], (vpgl_EGLint)a[1])); }
        else vpgl_missing("eglQueryString");
        break;
    case EGL_FN_QUERYSURFACE:
        if (p_eglQuerySurface) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglQuerySurface)p_eglQuerySurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_EGLint)a[2], (vpgl_EGLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("eglQuerySurface");
        break;
    case EGL_FN_SWAPBUFFERS:
        if (p_eglSwapBuffers) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglSwapBuffers)p_eglSwapBuffers)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        else vpgl_missing("eglSwapBuffers");
        break;
    case EGL_FN_TERMINATE:
        if (p_eglTerminate) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglTerminate)p_eglTerminate)((vpgl_void*)(uintptr_t)a[0]); }
        else vpgl_missing("eglTerminate");
        break;
    case EGL_FN_SWAPINTERVAL:
        if (p_eglSwapInterval) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglSwapInterval)p_eglSwapInterval)((vpgl_void*)(uintptr_t)a[0], (vpgl_EGLint)a[1]); }
        else vpgl_missing("eglSwapInterval");
        break;
    case EGL_FN_BINDAPI:
        if (p_eglBindAPI) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglBindAPI)p_eglBindAPI)((vpgl_EGLenum)a[0]); }
        else vpgl_missing("eglBindAPI");
        break;
    case EGL_FN_GETCURRENTCONTEXT:
        if (p_eglGetCurrentContext) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglGetCurrentContext)p_eglGetCurrentContext)(); }
        else vpgl_missing("eglGetCurrentContext");
        break;
    default:
        break;
    }
}


/* ============================================================
 * glMapBufferRange() staging (see tools/gen_gl_abi.py MAP_RANGE_FN)
 *
 * The address glMapBufferRange() returns points into host memory, which the
 * guest cannot dereference, so it is never handed over. Instead the range is
 * mirrored into the guest staging buffer the stub passed in args[MAP_PTR_SLOT]:
 * reads are served through a host-only GL_MAP_READ_BIT mapping, writes are
 * pushed back with glBufferSubData(). Nothing is ever really mapped, so
 * glGetBufferPointerv() answers NULL - which is what a driver answers for a
 * buffer it does not have mapped.
 * ============================================================ */
typedef struct {
    uint32_t target;   /* buffer target; 0 marks a free slot */
    int64_t  offset;
    int64_t  length;
    uint32_t access;
    uint64_t guest;    /* guest address of the staging buffer */
} vpgl_map_slot;

#define VPGL_MAP_SLOTS 16
static vpgl_map_slot vpgl_maps[VPGL_MAP_SLOTS];

static vpgl_map_slot* vpgl_map_find(uint32_t target)
{
    int i;
    for (i = 0; i < VPGL_MAP_SLOTS; i++) {
        if (vpgl_maps[i].target == target) {
            return &vpgl_maps[i];
        }
    }
    return NULL;
}

static vpgl_map_slot* vpgl_map_add(uint32_t target)
{
    int i;
    vpgl_map_slot* s = vpgl_map_find(target);
    if (s) {
        return s;
    }
    for (i = 0; i < VPGL_MAP_SLOTS; i++) {
        if (!vpgl_maps[i].target) {
            return &vpgl_maps[i];
        }
    }
    return NULL;
}

/* Copy [off, off+len) of the staging buffer back into the buffer object. */
static void vpgl_map_flush(vpgl_map_slot* s, int64_t off, int64_t len)
{
    void* guest;
    if (!s || !p_glBufferSubData) {
        return;
    }
    if (off < 0) {
        off = 0;
    }
    if (off > s->length) {
        off = s->length;
    }
    if (len < 0 || len > s->length - off) {
        len = s->length - off;
    }
    if (len <= 0) {
        return;
    }
    guest = vpgl_gptr((int64_t)s->guest);
    if (!guest) {
        return;
    }
    p_glBufferSubData((vpgl_GLenum)s->target, (vpgl_GLintptr)(s->offset + off),
                      (vpgl_GLsizeiptr)len,
                      (const void*)((char*)guest + off));
}

static void vpgl_dispatch_map_buffer_range(const int64_t* a, int64_t* ret)
{
    vpgl_GLenum     target = (vpgl_GLenum)a[0];
    vpgl_GLintptr   offset = (vpgl_GLintptr)a[1];
    vpgl_GLsizeiptr length = (vpgl_GLsizeiptr)a[2];
    vpgl_GLbitfield access = (vpgl_GLbitfield)a[3];
    void*           guest  = vpgl_gptr(a[4]);
    vpgl_map_slot*  s;

    *ret = 0;
    if (!p_glMapBufferRange) {
        vpgl_missing("glMapBufferRange");
        return;
    }
    if (!guest || length <= 0) {
        return;
    }
    /* Seed the staging buffer with the current contents when the guest asked
     * to read them. An INVALIDATE_* mapping declares them undefined, so the
     * copy is skipped there. The mapping used for it is host-only and closed
     * again right away. */
    if ((access & vpgl_GL_MAP_READ_BIT) &&
        !(access & (vpgl_GL_MAP_INVALIDATE_RANGE_BIT |
                    vpgl_GL_MAP_INVALIDATE_BUFFER_BIT))) {
        const void* src = p_glMapBufferRange(target, offset, length,
                                             vpgl_GL_MAP_READ_BIT);
        if (src) {
            memcpy(guest, src, (size_t)length);
            if (p_glUnmapBuffer) {
                p_glUnmapBuffer(target);
            }
        }
    }
    s = vpgl_map_add((uint32_t)target);
    if (!s) {
        return;
    }
    s->target = (uint32_t)target;
    s->offset = (int64_t)offset;
    s->length = (int64_t)length;
    s->access = (uint32_t)access;
    s->guest  = (uint64_t)a[4];
    /* Answer with the guest address: the only pointer the caller can use. */
    *ret = a[4];
}

static void vpgl_dispatch_unmap_buffer(const int64_t* a, int64_t* ret)
{
    vpgl_map_slot* s = vpgl_map_find((uint32_t)a[0]);

    *ret = 0;
    if (s) {
        if (s->access & vpgl_GL_MAP_WRITE_BIT) {
            vpgl_map_flush(s, 0, s->length);
        }
        s->target = 0;
        *ret = 1; /* GL_TRUE: the range was mapped, through the staging buffer */
        return;
    }
    /* Nothing of ours is mapped on this target: let the driver answer, which
     * is GL_FALSE for a buffer it never mapped. */
    if (p_glUnmapBuffer) {
        *ret = (int64_t)(uint32_t)((vpgl_GLboolean)p_glUnmapBuffer((vpgl_GLenum)a[0]));
    } else {
        vpgl_missing("glUnmapBuffer");
    }
}

static void vpgl_dispatch_flush_mapped_range(const int64_t* a, int64_t* ret)
{
    vpgl_map_slot* s = vpgl_map_find((uint32_t)a[0]);

    if (s) {
        if (s->access & vpgl_GL_MAP_WRITE_BIT) {
            vpgl_map_flush(s, a[1], a[2]);
        }
    } else if (p_glFlushMappedBufferRange) {
        p_glFlushMappedBufferRange((vpgl_GLenum)a[0], (vpgl_GLintptr)a[1],
                                   (vpgl_GLsizeiptr)a[2]);
    }
    *ret = 0; /* void */
}

static void vpgl_dispatch_gl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case GL_FN_MAPBUFFERRANGE:
        vpgl_dispatch_map_buffer_range(a, ret);
        break;
    case GL_FN_UNMAPBUFFER:
        vpgl_dispatch_unmap_buffer(a, ret);
        break;
    case GL_FN_FLUSHMAPPEDBUFFERRANGE:
        vpgl_dispatch_flush_mapped_range(a, ret);
        break;
    case GL_FN_ACTIVETEXTURE:
        if (p_glActiveTexture) { ((vpgl_PFN_glActiveTexture)p_glActiveTexture)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glActiveTexture");
        break;
    case GL_FN_ATTACHSHADER:
        if (p_glAttachShader) { ((vpgl_PFN_glAttachShader)p_glAttachShader)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glAttachShader");
        break;
    case GL_FN_BINDATTRIBLOCATION:
        if (p_glBindAttribLocation) { ((vpgl_PFN_glBindAttribLocation)p_glBindAttribLocation)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (const vpgl_GLchar*)vpgl_gptr(a[2])); }
        else vpgl_missing("glBindAttribLocation");
        break;
    case GL_FN_BINDBUFFER:
        if (p_glBindBuffer) { ((vpgl_PFN_glBindBuffer)p_glBindBuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindBuffer");
        break;
    case GL_FN_BINDFRAMEBUFFER:
        if (p_glBindFramebuffer) { ((vpgl_PFN_glBindFramebuffer)p_glBindFramebuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindFramebuffer");
        break;
    case GL_FN_BINDRENDERBUFFER:
        if (p_glBindRenderbuffer) { ((vpgl_PFN_glBindRenderbuffer)p_glBindRenderbuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindRenderbuffer");
        break;
    case GL_FN_BINDTEXTURE:
        if (p_glBindTexture) { ((vpgl_PFN_glBindTexture)p_glBindTexture)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindTexture");
        break;
    case GL_FN_BLENDCOLOR:
        if (p_glBlendColor) { ((vpgl_PFN_glBlendColor)p_glBlendColor)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        else vpgl_missing("glBlendColor");
        break;
    case GL_FN_BLENDEQUATION:
        if (p_glBlendEquation) { ((vpgl_PFN_glBlendEquation)p_glBlendEquation)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glBlendEquation");
        break;
    case GL_FN_BLENDEQUATIONSEPARATE:
        if (p_glBlendEquationSeparate) { ((vpgl_PFN_glBlendEquationSeparate)p_glBlendEquationSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        else vpgl_missing("glBlendEquationSeparate");
        break;
    case GL_FN_BLENDFUNC:
        if (p_glBlendFunc) { ((vpgl_PFN_glBlendFunc)p_glBlendFunc)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        else vpgl_missing("glBlendFunc");
        break;
    case GL_FN_BLENDFUNCSEPARATE:
        if (p_glBlendFuncSeparate) { ((vpgl_PFN_glBlendFuncSeparate)p_glBlendFuncSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLenum)a[3]); }
        else vpgl_missing("glBlendFuncSeparate");
        break;
    case GL_FN_BUFFERDATA:
        if (p_glBufferData) { ((vpgl_PFN_glBufferData)p_glBufferData)((vpgl_GLenum)a[0], (vpgl_GLsizeiptr)a[1], (const vpgl_void*)vpgl_gptr(a[2]), (vpgl_GLenum)a[3]); }
        else vpgl_missing("glBufferData");
        break;
    case GL_FN_BUFFERSUBDATA:
        if (p_glBufferSubData) { ((vpgl_PFN_glBufferSubData)p_glBufferSubData)((vpgl_GLenum)a[0], (vpgl_GLintptr)a[1], (vpgl_GLsizeiptr)a[2], (const vpgl_void*)vpgl_gptr(a[3])); }
        else vpgl_missing("glBufferSubData");
        break;
    case GL_FN_CHECKFRAMEBUFFERSTATUS:
        if (p_glCheckFramebufferStatus) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCheckFramebufferStatus)p_glCheckFramebufferStatus)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glCheckFramebufferStatus");
        break;
    case GL_FN_CLEAR:
        if (p_glClear) { ((vpgl_PFN_glClear)p_glClear)((vpgl_GLbitfield)a[0]); }
        else vpgl_missing("glClear");
        break;
    case GL_FN_CLEARCOLOR:
        if (p_glClearColor) { ((vpgl_PFN_glClearColor)p_glClearColor)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        else vpgl_missing("glClearColor");
        break;
    case GL_FN_CLEARDEPTHF:
        if (p_glClearDepthf) { ((vpgl_PFN_glClearDepthf)p_glClearDepthf)(vpgl_arg_f(a[0])); }
        else vpgl_missing("glClearDepthf");
        break;
    case GL_FN_CLEARSTENCIL:
        if (p_glClearStencil) { ((vpgl_PFN_glClearStencil)p_glClearStencil)((vpgl_GLint)a[0]); }
        else vpgl_missing("glClearStencil");
        break;
    case GL_FN_COLORMASK:
        if (p_glColorMask) { ((vpgl_PFN_glColorMask)p_glColorMask)((vpgl_GLboolean)a[0], (vpgl_GLboolean)a[1], (vpgl_GLboolean)a[2], (vpgl_GLboolean)a[3]); }
        else vpgl_missing("glColorMask");
        break;
    case GL_FN_COMPILESHADER:
        if (p_glCompileShader) { ((vpgl_PFN_glCompileShader)p_glCompileShader)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glCompileShader");
        break;
    case GL_FN_COMPRESSEDTEXIMAGE2D:
        if (p_glCompressedTexImage2D) { ((vpgl_PFN_glCompressedTexImage2D)p_glCompressedTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLint)a[5], (vpgl_GLsizei)a[6], (const vpgl_void*)vpgl_gptr(a[7])); }
        else vpgl_missing("glCompressedTexImage2D");
        break;
    case GL_FN_COMPRESSEDTEXSUBIMAGE2D:
        if (p_glCompressedTexSubImage2D) { ((vpgl_PFN_glCompressedTexSubImage2D)p_glCompressedTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLenum)a[6], (vpgl_GLsizei)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        else vpgl_missing("glCompressedTexSubImage2D");
        break;
    case GL_FN_COPYTEXIMAGE2D:
        if (p_glCopyTexImage2D) { ((vpgl_PFN_glCopyTexImage2D)p_glCopyTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLsizei)a[5], (vpgl_GLsizei)a[6], (vpgl_GLint)a[7]); }
        else vpgl_missing("glCopyTexImage2D");
        break;
    case GL_FN_COPYTEXSUBIMAGE2D:
        if (p_glCopyTexSubImage2D) { ((vpgl_PFN_glCopyTexSubImage2D)p_glCopyTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLint)a[5], (vpgl_GLsizei)a[6], (vpgl_GLsizei)a[7]); }
        else vpgl_missing("glCopyTexSubImage2D");
        break;
    case GL_FN_CREATEPROGRAM:
        if (p_glCreateProgram) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCreateProgram)p_glCreateProgram)(); }
        else vpgl_missing("glCreateProgram");
        break;
    case GL_FN_CREATESHADER:
        if (p_glCreateShader) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCreateShader)p_glCreateShader)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glCreateShader");
        break;
    case GL_FN_CULLFACE:
        if (p_glCullFace) { ((vpgl_PFN_glCullFace)p_glCullFace)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glCullFace");
        break;
    case GL_FN_DELETEBUFFERS:
        if (p_glDeleteBuffers) { ((vpgl_PFN_glDeleteBuffers)p_glDeleteBuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteBuffers");
        break;
    case GL_FN_DELETEFRAMEBUFFERS:
        if (p_glDeleteFramebuffers) { ((vpgl_PFN_glDeleteFramebuffers)p_glDeleteFramebuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteFramebuffers");
        break;
    case GL_FN_DELETEPROGRAM:
        if (p_glDeleteProgram) { ((vpgl_PFN_glDeleteProgram)p_glDeleteProgram)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glDeleteProgram");
        break;
    case GL_FN_DELETERENDERBUFFERS:
        if (p_glDeleteRenderbuffers) { ((vpgl_PFN_glDeleteRenderbuffers)p_glDeleteRenderbuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteRenderbuffers");
        break;
    case GL_FN_DELETESHADER:
        if (p_glDeleteShader) { ((vpgl_PFN_glDeleteShader)p_glDeleteShader)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glDeleteShader");
        break;
    case GL_FN_DELETETEXTURES:
        if (p_glDeleteTextures) { ((vpgl_PFN_glDeleteTextures)p_glDeleteTextures)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteTextures");
        break;
    case GL_FN_DEPTHFUNC:
        if (p_glDepthFunc) { ((vpgl_PFN_glDepthFunc)p_glDepthFunc)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glDepthFunc");
        break;
    case GL_FN_DEPTHMASK:
        if (p_glDepthMask) { ((vpgl_PFN_glDepthMask)p_glDepthMask)((vpgl_GLboolean)a[0]); }
        else vpgl_missing("glDepthMask");
        break;
    case GL_FN_DEPTHRANGEF:
        if (p_glDepthRangef) { ((vpgl_PFN_glDepthRangef)p_glDepthRangef)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1])); }
        else vpgl_missing("glDepthRangef");
        break;
    case GL_FN_DETACHSHADER:
        if (p_glDetachShader) { ((vpgl_PFN_glDetachShader)p_glDetachShader)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glDetachShader");
        break;
    case GL_FN_DISABLE:
        if (p_glDisable) { ((vpgl_PFN_glDisable)p_glDisable)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glDisable");
        break;
    case GL_FN_DISABLEVERTEXATTRIBARRAY:
        if (p_glDisableVertexAttribArray) { ((vpgl_PFN_glDisableVertexAttribArray)p_glDisableVertexAttribArray)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glDisableVertexAttribArray");
        break;
    case GL_FN_DRAWARRAYS:
        if (p_glDrawArrays) { ((vpgl_PFN_glDrawArrays)p_glDrawArrays)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2]); }
        else vpgl_missing("glDrawArrays");
        break;
    case GL_FN_DRAWELEMENTS:
        if (p_glDrawElements) { ((vpgl_PFN_glDrawElements)p_glDrawElements)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (const vpgl_void*)vpgl_ptr(a[3], VPGL_PTR_ELEMENT)); }
        else vpgl_missing("glDrawElements");
        break;
    case GL_FN_ENABLE:
        if (p_glEnable) { ((vpgl_PFN_glEnable)p_glEnable)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glEnable");
        break;
    case GL_FN_ENABLEVERTEXATTRIBARRAY:
        if (p_glEnableVertexAttribArray) { ((vpgl_PFN_glEnableVertexAttribArray)p_glEnableVertexAttribArray)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glEnableVertexAttribArray");
        break;
    case GL_FN_FINISH:
        if (p_glFinish) { ((vpgl_PFN_glFinish)p_glFinish)(); }
        else vpgl_missing("glFinish");
        break;
    case GL_FN_FLUSH:
        if (p_glFlush) { ((vpgl_PFN_glFlush)p_glFlush)(); }
        else vpgl_missing("glFlush");
        break;
    case GL_FN_FRAMEBUFFERRENDERBUFFER:
        if (p_glFramebufferRenderbuffer) { ((vpgl_PFN_glFramebufferRenderbuffer)p_glFramebufferRenderbuffer)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLuint)a[3]); }
        else vpgl_missing("glFramebufferRenderbuffer");
        break;
    case GL_FN_FRAMEBUFFERTEXTURE2D:
        if (p_glFramebufferTexture2D) { ((vpgl_PFN_glFramebufferTexture2D)p_glFramebufferTexture2D)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLuint)a[3], (vpgl_GLint)a[4]); }
        else vpgl_missing("glFramebufferTexture2D");
        break;
    case GL_FN_FRONTFACE:
        if (p_glFrontFace) { ((vpgl_PFN_glFrontFace)p_glFrontFace)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glFrontFace");
        break;
    case GL_FN_GENBUFFERS:
        if (p_glGenBuffers) { ((vpgl_PFN_glGenBuffers)p_glGenBuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenBuffers");
        break;
    case GL_FN_GENERATEMIPMAP:
        if (p_glGenerateMipmap) { ((vpgl_PFN_glGenerateMipmap)p_glGenerateMipmap)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glGenerateMipmap");
        break;
    case GL_FN_GENFRAMEBUFFERS:
        if (p_glGenFramebuffers) { ((vpgl_PFN_glGenFramebuffers)p_glGenFramebuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenFramebuffers");
        break;
    case GL_FN_GENRENDERBUFFERS:
        if (p_glGenRenderbuffers) { ((vpgl_PFN_glGenRenderbuffers)p_glGenRenderbuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenRenderbuffers");
        break;
    case GL_FN_GENTEXTURES:
        if (p_glGenTextures) { ((vpgl_PFN_glGenTextures)p_glGenTextures)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenTextures");
        break;
    case GL_FN_GETACTIVEATTRIB:
        if (p_glGetActiveAttrib) { ((vpgl_PFN_glGetActiveAttrib)p_glGetActiveAttrib)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLint*)vpgl_gptr(a[4]), (vpgl_GLenum*)vpgl_gptr(a[5]), (vpgl_GLchar*)vpgl_gptr(a[6])); }
        else vpgl_missing("glGetActiveAttrib");
        break;
    case GL_FN_GETACTIVEUNIFORM:
        if (p_glGetActiveUniform) { ((vpgl_PFN_glGetActiveUniform)p_glGetActiveUniform)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLint*)vpgl_gptr(a[4]), (vpgl_GLenum*)vpgl_gptr(a[5]), (vpgl_GLchar*)vpgl_gptr(a[6])); }
        else vpgl_missing("glGetActiveUniform");
        break;
    case GL_FN_GETATTACHEDSHADERS:
        if (p_glGetAttachedShaders) { ((vpgl_PFN_glGetAttachedShaders)p_glGetAttachedShaders)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLuint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetAttachedShaders");
        break;
    case GL_FN_GETATTRIBLOCATION:
        if (p_glGetAttribLocation) { *ret = (int64_t)(int32_t)((vpgl_PFN_glGetAttribLocation)p_glGetAttribLocation)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetAttribLocation");
        break;
    case GL_FN_GETBOOLEANV:
        if (p_glGetBooleanv) { ((vpgl_PFN_glGetBooleanv)p_glGetBooleanv)((vpgl_GLenum)a[0], (vpgl_GLboolean*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetBooleanv");
        break;
    case GL_FN_GETBUFFERPARAMETERIV:
        if (p_glGetBufferParameteriv) { ((vpgl_PFN_glGetBufferParameteriv)p_glGetBufferParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetBufferParameteriv");
        break;
    case GL_FN_GETERROR:
        if (p_glGetError) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glGetError)p_glGetError)(); }
        else vpgl_missing("glGetError");
        break;
    case GL_FN_GETFLOATV:
        if (p_glGetFloatv) { ((vpgl_PFN_glGetFloatv)p_glGetFloatv)((vpgl_GLenum)a[0], (vpgl_GLfloat*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetFloatv");
        break;
    case GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV:
        if (p_glGetFramebufferAttachmentParameteriv) { ((vpgl_PFN_glGetFramebufferAttachmentParameteriv)p_glGetFramebufferAttachmentParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetFramebufferAttachmentParameteriv");
        break;
    case GL_FN_GETINTEGERV:
        if (p_glGetIntegerv) { ((vpgl_PFN_glGetIntegerv)p_glGetIntegerv)((vpgl_GLenum)a[0], (vpgl_GLint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetIntegerv");
        break;
    case GL_FN_GETPROGRAMIV:
        if (p_glGetProgramiv) { ((vpgl_PFN_glGetProgramiv)p_glGetProgramiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetProgramiv");
        break;
    case GL_FN_GETPROGRAMINFOLOG:
        if (p_glGetProgramInfoLog) { ((vpgl_PFN_glGetProgramInfoLog)p_glGetProgramInfoLog)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetProgramInfoLog");
        break;
    case GL_FN_GETRENDERBUFFERPARAMETERIV:
        if (p_glGetRenderbufferParameteriv) { ((vpgl_PFN_glGetRenderbufferParameteriv)p_glGetRenderbufferParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetRenderbufferParameteriv");
        break;
    case GL_FN_GETSHADERIV:
        if (p_glGetShaderiv) { ((vpgl_PFN_glGetShaderiv)p_glGetShaderiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetShaderiv");
        break;
    case GL_FN_GETSHADERINFOLOG:
        if (p_glGetShaderInfoLog) { ((vpgl_PFN_glGetShaderInfoLog)p_glGetShaderInfoLog)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetShaderInfoLog");
        break;
    case GL_FN_GETSHADERPRECISIONFORMAT:
        if (p_glGetShaderPrecisionFormat) { ((vpgl_PFN_glGetShaderPrecisionFormat)p_glGetShaderPrecisionFormat)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2]), (vpgl_GLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetShaderPrecisionFormat");
        break;
    case GL_FN_GETSHADERSOURCE:
        if (p_glGetShaderSource) { ((vpgl_PFN_glGetShaderSource)p_glGetShaderSource)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetShaderSource");
        break;
    case GL_FN_GETSTRING:
        if (p_glGetString) { *ret = vpgl_string_out(a, (const char*)((vpgl_PFN_glGetString)p_glGetString)((vpgl_GLenum)a[0])); }
        else vpgl_missing("glGetString");
        break;
    case GL_FN_GETTEXPARAMETERFV:
        if (p_glGetTexParameterfv) { ((vpgl_PFN_glGetTexParameterfv)p_glGetTexParameterfv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetTexParameterfv");
        break;
    case GL_FN_GETTEXPARAMETERIV:
        if (p_glGetTexParameteriv) { ((vpgl_PFN_glGetTexParameteriv)p_glGetTexParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetTexParameteriv");
        break;
    case GL_FN_GETUNIFORMFV:
        if (p_glGetUniformfv) { ((vpgl_PFN_glGetUniformfv)p_glGetUniformfv)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetUniformfv");
        break;
    case GL_FN_GETUNIFORMIV:
        if (p_glGetUniformiv) { ((vpgl_PFN_glGetUniformiv)p_glGetUniformiv)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetUniformiv");
        break;
    case GL_FN_GETUNIFORMLOCATION:
        if (p_glGetUniformLocation) { *ret = (int64_t)(int32_t)((vpgl_PFN_glGetUniformLocation)p_glGetUniformLocation)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetUniformLocation");
        break;
    case GL_FN_GETVERTEXATTRIBFV:
        if (p_glGetVertexAttribfv) { ((vpgl_PFN_glGetVertexAttribfv)p_glGetVertexAttribfv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetVertexAttribfv");
        break;
    case GL_FN_GETVERTEXATTRIBIV:
        if (p_glGetVertexAttribiv) { ((vpgl_PFN_glGetVertexAttribiv)p_glGetVertexAttribiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetVertexAttribiv");
        break;
    case GL_FN_GETVERTEXATTRIBPOINTERV:
        if (p_glGetVertexAttribPointerv) { ((vpgl_PFN_glGetVertexAttribPointerv)p_glGetVertexAttribPointerv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_void**)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetVertexAttribPointerv");
        break;
    case GL_FN_HINT:
        if (p_glHint) { ((vpgl_PFN_glHint)p_glHint)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        else vpgl_missing("glHint");
        break;
    case GL_FN_ISBUFFER:
        if (p_glIsBuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsBuffer)p_glIsBuffer)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsBuffer");
        break;
    case GL_FN_ISENABLED:
        if (p_glIsEnabled) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsEnabled)p_glIsEnabled)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glIsEnabled");
        break;
    case GL_FN_ISFRAMEBUFFER:
        if (p_glIsFramebuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsFramebuffer)p_glIsFramebuffer)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsFramebuffer");
        break;
    case GL_FN_ISPROGRAM:
        if (p_glIsProgram) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsProgram)p_glIsProgram)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsProgram");
        break;
    case GL_FN_ISRENDERBUFFER:
        if (p_glIsRenderbuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsRenderbuffer)p_glIsRenderbuffer)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsRenderbuffer");
        break;
    case GL_FN_ISSHADER:
        if (p_glIsShader) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsShader)p_glIsShader)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsShader");
        break;
    case GL_FN_ISTEXTURE:
        if (p_glIsTexture) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsTexture)p_glIsTexture)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsTexture");
        break;
    case GL_FN_LINEWIDTH:
        if (p_glLineWidth) { ((vpgl_PFN_glLineWidth)p_glLineWidth)(vpgl_arg_f(a[0])); }
        else vpgl_missing("glLineWidth");
        break;
    case GL_FN_LINKPROGRAM:
        if (p_glLinkProgram) { ((vpgl_PFN_glLinkProgram)p_glLinkProgram)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glLinkProgram");
        break;
    case GL_FN_PIXELSTOREI:
        if (p_glPixelStorei) { ((vpgl_PFN_glPixelStorei)p_glPixelStorei)((vpgl_GLenum)a[0], (vpgl_GLint)a[1]); }
        else vpgl_missing("glPixelStorei");
        break;
    case GL_FN_POLYGONOFFSET:
        if (p_glPolygonOffset) { ((vpgl_PFN_glPolygonOffset)p_glPolygonOffset)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1])); }
        else vpgl_missing("glPolygonOffset");
        break;
    case GL_FN_READPIXELS:
        if (p_glReadPixels) { ((vpgl_PFN_glReadPixels)p_glReadPixels)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3], (vpgl_GLenum)a[4], (vpgl_GLenum)a[5], (vpgl_void*)vpgl_gptr(a[6])); }
        else vpgl_missing("glReadPixels");
        break;
    case GL_FN_RELEASESHADERCOMPILER:
        if (p_glReleaseShaderCompiler) { ((vpgl_PFN_glReleaseShaderCompiler)p_glReleaseShaderCompiler)(); }
        else vpgl_missing("glReleaseShaderCompiler");
        break;
    case GL_FN_RENDERBUFFERSTORAGE:
        if (p_glRenderbufferStorage) { ((vpgl_PFN_glRenderbufferStorage)p_glRenderbufferStorage)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        else vpgl_missing("glRenderbufferStorage");
        break;
    case GL_FN_SAMPLECOVERAGE:
        if (p_glSampleCoverage) { ((vpgl_PFN_glSampleCoverage)p_glSampleCoverage)(vpgl_arg_f(a[0]), (vpgl_GLboolean)a[1]); }
        else vpgl_missing("glSampleCoverage");
        break;
    case GL_FN_SCISSOR:
        if (p_glScissor) { ((vpgl_PFN_glScissor)p_glScissor)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        else vpgl_missing("glScissor");
        break;
    case GL_FN_SHADERBINARY:
        if (p_glShaderBinary) { ((vpgl_PFN_glShaderBinary)p_glShaderBinary)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1]), (vpgl_GLenum)a[2], (const vpgl_void*)vpgl_gptr(a[3]), (vpgl_GLsizei)a[4]); }
        else vpgl_missing("glShaderBinary");
        break;
    case GL_FN_SHADERSOURCE:
        if (p_glShaderSource) { ((vpgl_PFN_glShaderSource)p_glShaderSource)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], vpgl_translate_shader_srcs(a, (int)a[1]), (const vpgl_GLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glShaderSource");
        break;
    case GL_FN_STENCILFUNC:
        if (p_glStencilFunc) { ((vpgl_PFN_glStencilFunc)p_glStencilFunc)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLuint)a[2]); }
        else vpgl_missing("glStencilFunc");
        break;
    case GL_FN_STENCILFUNCSEPARATE:
        if (p_glStencilFuncSeparate) { ((vpgl_PFN_glStencilFuncSeparate)p_glStencilFuncSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2], (vpgl_GLuint)a[3]); }
        else vpgl_missing("glStencilFuncSeparate");
        break;
    case GL_FN_STENCILMASK:
        if (p_glStencilMask) { ((vpgl_PFN_glStencilMask)p_glStencilMask)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glStencilMask");
        break;
    case GL_FN_STENCILMASKSEPARATE:
        if (p_glStencilMaskSeparate) { ((vpgl_PFN_glStencilMaskSeparate)p_glStencilMaskSeparate)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glStencilMaskSeparate");
        break;
    case GL_FN_STENCILOP:
        if (p_glStencilOp) { ((vpgl_PFN_glStencilOp)p_glStencilOp)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2]); }
        else vpgl_missing("glStencilOp");
        break;
    case GL_FN_STENCILOPSEPARATE:
        if (p_glStencilOpSeparate) { ((vpgl_PFN_glStencilOpSeparate)p_glStencilOpSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLenum)a[3]); }
        else vpgl_missing("glStencilOpSeparate");
        break;
    case GL_FN_TEXIMAGE2D:
        if (p_glTexImage2D) { ((vpgl_PFN_glTexImage2D)p_glTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLint)a[5], (vpgl_GLenum)a[6], (vpgl_GLenum)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        else vpgl_missing("glTexImage2D");
        break;
    case GL_FN_TEXPARAMETERF:
        if (p_glTexParameterf) { ((vpgl_PFN_glTexParameterf)p_glTexParameterf)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], vpgl_arg_f(a[2])); }
        else vpgl_missing("glTexParameterf");
        break;
    case GL_FN_TEXPARAMETERFV:
        if (p_glTexParameterfv) { ((vpgl_PFN_glTexParameterfv)p_glTexParameterfv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glTexParameterfv");
        break;
    case GL_FN_TEXPARAMETERI:
        if (p_glTexParameteri) { ((vpgl_PFN_glTexParameteri)p_glTexParameteri)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2]); }
        else vpgl_missing("glTexParameteri");
        break;
    case GL_FN_TEXPARAMETERIV:
        if (p_glTexParameteriv) { ((vpgl_PFN_glTexParameteriv)p_glTexParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glTexParameteriv");
        break;
    case GL_FN_TEXSUBIMAGE2D:
        if (p_glTexSubImage2D) { ((vpgl_PFN_glTexSubImage2D)p_glTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLenum)a[6], (vpgl_GLenum)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        else vpgl_missing("glTexSubImage2D");
        break;
    case GL_FN_UNIFORM1F:
        if (p_glUniform1f) { ((vpgl_PFN_glUniform1f)p_glUniform1f)((vpgl_GLint)a[0], vpgl_arg_f(a[1])); }
        else vpgl_missing("glUniform1f");
        break;
    case GL_FN_UNIFORM1FV:
        if (p_glUniform1fv) { ((vpgl_PFN_glUniform1fv)p_glUniform1fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform1fv");
        break;
    case GL_FN_UNIFORM1I:
        if (p_glUniform1i) { ((vpgl_PFN_glUniform1i)p_glUniform1i)((vpgl_GLint)a[0], (vpgl_GLint)a[1]); }
        else vpgl_missing("glUniform1i");
        break;
    case GL_FN_UNIFORM1IV:
        if (p_glUniform1iv) { ((vpgl_PFN_glUniform1iv)p_glUniform1iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform1iv");
        break;
    case GL_FN_UNIFORM2F:
        if (p_glUniform2f) { ((vpgl_PFN_glUniform2f)p_glUniform2f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2])); }
        else vpgl_missing("glUniform2f");
        break;
    case GL_FN_UNIFORM2FV:
        if (p_glUniform2fv) { ((vpgl_PFN_glUniform2fv)p_glUniform2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform2fv");
        break;
    case GL_FN_UNIFORM2I:
        if (p_glUniform2i) { ((vpgl_PFN_glUniform2i)p_glUniform2i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2]); }
        else vpgl_missing("glUniform2i");
        break;
    case GL_FN_UNIFORM2IV:
        if (p_glUniform2iv) { ((vpgl_PFN_glUniform2iv)p_glUniform2iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform2iv");
        break;
    case GL_FN_UNIFORM3F:
        if (p_glUniform3f) { ((vpgl_PFN_glUniform3f)p_glUniform3f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        else vpgl_missing("glUniform3f");
        break;
    case GL_FN_UNIFORM3FV:
        if (p_glUniform3fv) { ((vpgl_PFN_glUniform3fv)p_glUniform3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform3fv");
        break;
    case GL_FN_UNIFORM3I:
        if (p_glUniform3i) { ((vpgl_PFN_glUniform3i)p_glUniform3i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3]); }
        else vpgl_missing("glUniform3i");
        break;
    case GL_FN_UNIFORM3IV:
        if (p_glUniform3iv) { ((vpgl_PFN_glUniform3iv)p_glUniform3iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform3iv");
        break;
    case GL_FN_UNIFORM4F:
        if (p_glUniform4f) { ((vpgl_PFN_glUniform4f)p_glUniform4f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3]), vpgl_arg_f(a[4])); }
        else vpgl_missing("glUniform4f");
        break;
    case GL_FN_UNIFORM4FV:
        if (p_glUniform4fv) { ((vpgl_PFN_glUniform4fv)p_glUniform4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform4fv");
        break;
    case GL_FN_UNIFORM4I:
        if (p_glUniform4i) { ((vpgl_PFN_glUniform4i)p_glUniform4i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4]); }
        else vpgl_missing("glUniform4i");
        break;
    case GL_FN_UNIFORM4IV:
        if (p_glUniform4iv) { ((vpgl_PFN_glUniform4iv)p_glUniform4iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform4iv");
        break;
    case GL_FN_UNIFORMMATRIX2FV:
        if (p_glUniformMatrix2fv) { ((vpgl_PFN_glUniformMatrix2fv)p_glUniformMatrix2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix2fv");
        break;
    case GL_FN_UNIFORMMATRIX3FV:
        if (p_glUniformMatrix3fv) { ((vpgl_PFN_glUniformMatrix3fv)p_glUniformMatrix3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix3fv");
        break;
    case GL_FN_UNIFORMMATRIX4FV:
        if (p_glUniformMatrix4fv) { ((vpgl_PFN_glUniformMatrix4fv)p_glUniformMatrix4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix4fv");
        break;
    case GL_FN_USEPROGRAM:
        if (p_glUseProgram) { ((vpgl_PFN_glUseProgram)p_glUseProgram)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glUseProgram");
        break;
    case GL_FN_VALIDATEPROGRAM:
        if (p_glValidateProgram) { ((vpgl_PFN_glValidateProgram)p_glValidateProgram)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glValidateProgram");
        break;
    case GL_FN_VERTEXATTRIB1F:
        if (p_glVertexAttrib1f) { ((vpgl_PFN_glVertexAttrib1f)p_glVertexAttrib1f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1])); }
        else vpgl_missing("glVertexAttrib1f");
        break;
    case GL_FN_VERTEXATTRIB1FV:
        if (p_glVertexAttrib1fv) { ((vpgl_PFN_glVertexAttrib1fv)p_glVertexAttrib1fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttrib1fv");
        break;
    case GL_FN_VERTEXATTRIB2F:
        if (p_glVertexAttrib2f) { ((vpgl_PFN_glVertexAttrib2f)p_glVertexAttrib2f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2])); }
        else vpgl_missing("glVertexAttrib2f");
        break;
    case GL_FN_VERTEXATTRIB2FV:
        if (p_glVertexAttrib2fv) { ((vpgl_PFN_glVertexAttrib2fv)p_glVertexAttrib2fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttrib2fv");
        break;
    case GL_FN_VERTEXATTRIB3F:
        if (p_glVertexAttrib3f) { ((vpgl_PFN_glVertexAttrib3f)p_glVertexAttrib3f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        else vpgl_missing("glVertexAttrib3f");
        break;
    case GL_FN_VERTEXATTRIB3FV:
        if (p_glVertexAttrib3fv) { ((vpgl_PFN_glVertexAttrib3fv)p_glVertexAttrib3fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttrib3fv");
        break;
    case GL_FN_VERTEXATTRIB4F:
        if (p_glVertexAttrib4f) { ((vpgl_PFN_glVertexAttrib4f)p_glVertexAttrib4f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3]), vpgl_arg_f(a[4])); }
        else vpgl_missing("glVertexAttrib4f");
        break;
    case GL_FN_VERTEXATTRIB4FV:
        if (p_glVertexAttrib4fv) { ((vpgl_PFN_glVertexAttrib4fv)p_glVertexAttrib4fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttrib4fv");
        break;
    case GL_FN_VERTEXATTRIBPOINTER:
        if (p_glVertexAttribPointer) { ((vpgl_PFN_glVertexAttribPointer)p_glVertexAttribPointer)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLboolean)a[3], (vpgl_GLsizei)a[4], (const vpgl_void*)vpgl_ptr(a[5], VPGL_PTR_ARRAY)); }
        else vpgl_missing("glVertexAttribPointer");
        break;
    case GL_FN_VIEWPORT:
        if (p_glViewport) { ((vpgl_PFN_glViewport)p_glViewport)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        else vpgl_missing("glViewport");
        break;
    case GL_FN_READBUFFER:
        if (p_glReadBuffer) { ((vpgl_PFN_glReadBuffer)p_glReadBuffer)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glReadBuffer");
        break;
    case GL_FN_DRAWRANGEELEMENTS:
        if (p_glDrawRangeElements) { ((vpgl_PFN_glDrawRangeElements)p_glDrawRangeElements)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2], (vpgl_GLsizei)a[3], (vpgl_GLenum)a[4], (const vpgl_void*)vpgl_ptr(a[5], VPGL_PTR_ELEMENT)); }
        else vpgl_missing("glDrawRangeElements");
        break;
    case GL_FN_TEXIMAGE3D:
        if (p_glTexImage3D) { ((vpgl_PFN_glTexImage3D)p_glTexImage3D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLint)a[6], (vpgl_GLenum)a[7], (vpgl_GLenum)a[8], (const vpgl_void*)vpgl_gptr(a[9])); }
        else vpgl_missing("glTexImage3D");
        break;
    case GL_FN_TEXSUBIMAGE3D:
        if (p_glTexSubImage3D) { ((vpgl_PFN_glTexSubImage3D)p_glTexSubImage3D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLsizei)a[5], (vpgl_GLsizei)a[6], (vpgl_GLsizei)a[7], (vpgl_GLenum)a[8], (vpgl_GLenum)a[9], (const vpgl_void*)vpgl_gptr(a[10])); }
        else vpgl_missing("glTexSubImage3D");
        break;
    case GL_FN_COPYTEXSUBIMAGE3D:
        if (p_glCopyTexSubImage3D) { ((vpgl_PFN_glCopyTexSubImage3D)p_glCopyTexSubImage3D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLint)a[5], (vpgl_GLint)a[6], (vpgl_GLsizei)a[7], (vpgl_GLsizei)a[8]); }
        else vpgl_missing("glCopyTexSubImage3D");
        break;
    case GL_FN_COMPRESSEDTEXIMAGE3D:
        if (p_glCompressedTexImage3D) { ((vpgl_PFN_glCompressedTexImage3D)p_glCompressedTexImage3D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLint)a[6], (vpgl_GLsizei)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        else vpgl_missing("glCompressedTexImage3D");
        break;
    case GL_FN_COMPRESSEDTEXSUBIMAGE3D:
        if (p_glCompressedTexSubImage3D) { ((vpgl_PFN_glCompressedTexSubImage3D)p_glCompressedTexSubImage3D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLsizei)a[5], (vpgl_GLsizei)a[6], (vpgl_GLsizei)a[7], (vpgl_GLenum)a[8], (vpgl_GLsizei)a[9], (const vpgl_void*)vpgl_gptr(a[10])); }
        else vpgl_missing("glCompressedTexSubImage3D");
        break;
    case GL_FN_GENQUERIES:
        if (p_glGenQueries) { ((vpgl_PFN_glGenQueries)p_glGenQueries)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenQueries");
        break;
    case GL_FN_DELETEQUERIES:
        if (p_glDeleteQueries) { ((vpgl_PFN_glDeleteQueries)p_glDeleteQueries)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteQueries");
        break;
    case GL_FN_ISQUERY:
        if (p_glIsQuery) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsQuery)p_glIsQuery)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsQuery");
        break;
    case GL_FN_BEGINQUERY:
        if (p_glBeginQuery) { ((vpgl_PFN_glBeginQuery)p_glBeginQuery)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBeginQuery");
        break;
    case GL_FN_ENDQUERY:
        if (p_glEndQuery) { ((vpgl_PFN_glEndQuery)p_glEndQuery)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glEndQuery");
        break;
    case GL_FN_GETQUERYIV:
        if (p_glGetQueryiv) { ((vpgl_PFN_glGetQueryiv)p_glGetQueryiv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetQueryiv");
        break;
    case GL_FN_GETQUERYOBJECTUIV:
        if (p_glGetQueryObjectuiv) { ((vpgl_PFN_glGetQueryObjectuiv)p_glGetQueryObjectuiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetQueryObjectuiv");
        break;
    case GL_FN_GETBUFFERPOINTERV:
        if (p_glGetBufferPointerv) { ((vpgl_PFN_glGetBufferPointerv)p_glGetBufferPointerv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_void**)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetBufferPointerv");
        break;
    case GL_FN_DRAWBUFFERS:
        if (p_glDrawBuffers) { ((vpgl_PFN_glDrawBuffers)p_glDrawBuffers)((vpgl_GLsizei)a[0], (const vpgl_GLenum*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDrawBuffers");
        break;
    case GL_FN_UNIFORMMATRIX2X3FV:
        if (p_glUniformMatrix2x3fv) { ((vpgl_PFN_glUniformMatrix2x3fv)p_glUniformMatrix2x3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix2x3fv");
        break;
    case GL_FN_UNIFORMMATRIX3X2FV:
        if (p_glUniformMatrix3x2fv) { ((vpgl_PFN_glUniformMatrix3x2fv)p_glUniformMatrix3x2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix3x2fv");
        break;
    case GL_FN_UNIFORMMATRIX2X4FV:
        if (p_glUniformMatrix2x4fv) { ((vpgl_PFN_glUniformMatrix2x4fv)p_glUniformMatrix2x4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix2x4fv");
        break;
    case GL_FN_UNIFORMMATRIX4X2FV:
        if (p_glUniformMatrix4x2fv) { ((vpgl_PFN_glUniformMatrix4x2fv)p_glUniformMatrix4x2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix4x2fv");
        break;
    case GL_FN_UNIFORMMATRIX3X4FV:
        if (p_glUniformMatrix3x4fv) { ((vpgl_PFN_glUniformMatrix3x4fv)p_glUniformMatrix3x4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix3x4fv");
        break;
    case GL_FN_UNIFORMMATRIX4X3FV:
        if (p_glUniformMatrix4x3fv) { ((vpgl_PFN_glUniformMatrix4x3fv)p_glUniformMatrix4x3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        else vpgl_missing("glUniformMatrix4x3fv");
        break;
    case GL_FN_BLITFRAMEBUFFER:
        if (p_glBlitFramebuffer) { ((vpgl_PFN_glBlitFramebuffer)p_glBlitFramebuffer)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLint)a[5], (vpgl_GLint)a[6], (vpgl_GLint)a[7], (vpgl_GLbitfield)a[8], (vpgl_GLenum)a[9]); }
        else vpgl_missing("glBlitFramebuffer");
        break;
    case GL_FN_RENDERBUFFERSTORAGEMULTISAMPLE:
        if (p_glRenderbufferStorageMultisample) { ((vpgl_PFN_glRenderbufferStorageMultisample)p_glRenderbufferStorageMultisample)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4]); }
        else vpgl_missing("glRenderbufferStorageMultisample");
        break;
    case GL_FN_FRAMEBUFFERTEXTURELAYER:
        if (p_glFramebufferTextureLayer) { ((vpgl_PFN_glFramebufferTextureLayer)p_glFramebufferTextureLayer)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLuint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4]); }
        else vpgl_missing("glFramebufferTextureLayer");
        break;
    case GL_FN_BINDVERTEXARRAY:
        if (p_glBindVertexArray) { ((vpgl_PFN_glBindVertexArray)p_glBindVertexArray)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glBindVertexArray");
        break;
    case GL_FN_DELETEVERTEXARRAYS:
        if (p_glDeleteVertexArrays) { ((vpgl_PFN_glDeleteVertexArrays)p_glDeleteVertexArrays)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteVertexArrays");
        break;
    case GL_FN_GENVERTEXARRAYS:
        if (p_glGenVertexArrays) { ((vpgl_PFN_glGenVertexArrays)p_glGenVertexArrays)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenVertexArrays");
        break;
    case GL_FN_ISVERTEXARRAY:
        if (p_glIsVertexArray) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsVertexArray)p_glIsVertexArray)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsVertexArray");
        break;
    case GL_FN_GETINTEGERI_V:
        if (p_glGetIntegeri_v) { ((vpgl_PFN_glGetIntegeri_v)p_glGetIntegeri_v)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetIntegeri_v");
        break;
    case GL_FN_BEGINTRANSFORMFEEDBACK:
        if (p_glBeginTransformFeedback) { ((vpgl_PFN_glBeginTransformFeedback)p_glBeginTransformFeedback)((vpgl_GLenum)a[0]); }
        else vpgl_missing("glBeginTransformFeedback");
        break;
    case GL_FN_ENDTRANSFORMFEEDBACK:
        if (p_glEndTransformFeedback) { ((vpgl_PFN_glEndTransformFeedback)p_glEndTransformFeedback)(); }
        else vpgl_missing("glEndTransformFeedback");
        break;
    case GL_FN_BINDBUFFERRANGE:
        if (p_glBindBufferRange) { ((vpgl_PFN_glBindBufferRange)p_glBindBufferRange)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2], (vpgl_GLintptr)a[3], (vpgl_GLsizeiptr)a[4]); }
        else vpgl_missing("glBindBufferRange");
        break;
    case GL_FN_BINDBUFFERBASE:
        if (p_glBindBufferBase) { ((vpgl_PFN_glBindBufferBase)p_glBindBufferBase)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2]); }
        else vpgl_missing("glBindBufferBase");
        break;
    case GL_FN_TRANSFORMFEEDBACKVARYINGS:
        if (p_glTransformFeedbackVaryings) { ((vpgl_PFN_glTransformFeedbackVaryings)p_glTransformFeedbackVaryings)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLchar**)vpgl_gptr(a[2]), (vpgl_GLenum)a[3]); }
        else vpgl_missing("glTransformFeedbackVaryings");
        break;
    case GL_FN_GETTRANSFORMFEEDBACKVARYING:
        if (p_glGetTransformFeedbackVarying) { ((vpgl_PFN_glGetTransformFeedbackVarying)p_glGetTransformFeedbackVarying)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLsizei*)vpgl_gptr(a[4]), (vpgl_GLenum*)vpgl_gptr(a[5]), (vpgl_GLchar*)vpgl_gptr(a[6])); }
        else vpgl_missing("glGetTransformFeedbackVarying");
        break;
    case GL_FN_VERTEXATTRIBIPOINTER:
        if (p_glVertexAttribIPointer) { ((vpgl_PFN_glVertexAttribIPointer)p_glVertexAttribIPointer)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (const vpgl_void*)vpgl_ptr(a[4], VPGL_PTR_ARRAY)); }
        else vpgl_missing("glVertexAttribIPointer");
        break;
    case GL_FN_GETVERTEXATTRIBIIV:
        if (p_glGetVertexAttribIiv) { ((vpgl_PFN_glGetVertexAttribIiv)p_glGetVertexAttribIiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetVertexAttribIiv");
        break;
    case GL_FN_GETVERTEXATTRIBIUIV:
        if (p_glGetVertexAttribIuiv) { ((vpgl_PFN_glGetVertexAttribIuiv)p_glGetVertexAttribIuiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetVertexAttribIuiv");
        break;
    case GL_FN_VERTEXATTRIBI4I:
        if (p_glVertexAttribI4i) { ((vpgl_PFN_glVertexAttribI4i)p_glVertexAttribI4i)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4]); }
        else vpgl_missing("glVertexAttribI4i");
        break;
    case GL_FN_VERTEXATTRIBI4UI:
        if (p_glVertexAttribI4ui) { ((vpgl_PFN_glVertexAttribI4ui)p_glVertexAttribI4ui)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2], (vpgl_GLuint)a[3], (vpgl_GLuint)a[4]); }
        else vpgl_missing("glVertexAttribI4ui");
        break;
    case GL_FN_VERTEXATTRIBI4IV:
        if (p_glVertexAttribI4iv) { ((vpgl_PFN_glVertexAttribI4iv)p_glVertexAttribI4iv)((vpgl_GLuint)a[0], (const vpgl_GLint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttribI4iv");
        break;
    case GL_FN_VERTEXATTRIBI4UIV:
        if (p_glVertexAttribI4uiv) { ((vpgl_PFN_glVertexAttribI4uiv)p_glVertexAttribI4uiv)((vpgl_GLuint)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glVertexAttribI4uiv");
        break;
    case GL_FN_GETUNIFORMUIV:
        if (p_glGetUniformuiv) { ((vpgl_PFN_glGetUniformuiv)p_glGetUniformuiv)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetUniformuiv");
        break;
    case GL_FN_GETFRAGDATALOCATION:
        if (p_glGetFragDataLocation) { *ret = (int64_t)(int32_t)((vpgl_PFN_glGetFragDataLocation)p_glGetFragDataLocation)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetFragDataLocation");
        break;
    case GL_FN_UNIFORM1UI:
        if (p_glUniform1ui) { ((vpgl_PFN_glUniform1ui)p_glUniform1ui)((vpgl_GLint)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glUniform1ui");
        break;
    case GL_FN_UNIFORM2UI:
        if (p_glUniform2ui) { ((vpgl_PFN_glUniform2ui)p_glUniform2ui)((vpgl_GLint)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2]); }
        else vpgl_missing("glUniform2ui");
        break;
    case GL_FN_UNIFORM3UI:
        if (p_glUniform3ui) { ((vpgl_PFN_glUniform3ui)p_glUniform3ui)((vpgl_GLint)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2], (vpgl_GLuint)a[3]); }
        else vpgl_missing("glUniform3ui");
        break;
    case GL_FN_UNIFORM4UI:
        if (p_glUniform4ui) { ((vpgl_PFN_glUniform4ui)p_glUniform4ui)((vpgl_GLint)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2], (vpgl_GLuint)a[3], (vpgl_GLuint)a[4]); }
        else vpgl_missing("glUniform4ui");
        break;
    case GL_FN_UNIFORM1UIV:
        if (p_glUniform1uiv) { ((vpgl_PFN_glUniform1uiv)p_glUniform1uiv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform1uiv");
        break;
    case GL_FN_UNIFORM2UIV:
        if (p_glUniform2uiv) { ((vpgl_PFN_glUniform2uiv)p_glUniform2uiv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform2uiv");
        break;
    case GL_FN_UNIFORM3UIV:
        if (p_glUniform3uiv) { ((vpgl_PFN_glUniform3uiv)p_glUniform3uiv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform3uiv");
        break;
    case GL_FN_UNIFORM4UIV:
        if (p_glUniform4uiv) { ((vpgl_PFN_glUniform4uiv)p_glUniform4uiv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glUniform4uiv");
        break;
    case GL_FN_CLEARBUFFERIV:
        if (p_glClearBufferiv) { ((vpgl_PFN_glClearBufferiv)p_glClearBufferiv)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glClearBufferiv");
        break;
    case GL_FN_CLEARBUFFERUIV:
        if (p_glClearBufferuiv) { ((vpgl_PFN_glClearBufferuiv)p_glClearBufferuiv)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glClearBufferuiv");
        break;
    case GL_FN_CLEARBUFFERFV:
        if (p_glClearBufferfv) { ((vpgl_PFN_glClearBufferfv)p_glClearBufferfv)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glClearBufferfv");
        break;
    case GL_FN_CLEARBUFFERFI:
        if (p_glClearBufferfi) { ((vpgl_PFN_glClearBufferfi)p_glClearBufferfi)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], vpgl_arg_f(a[2]), (vpgl_GLint)a[3]); }
        else vpgl_missing("glClearBufferfi");
        break;
    case GL_FN_GETSTRINGI:
        if (p_glGetStringi) { *ret = vpgl_string_out(a, (const char*)((vpgl_PFN_glGetStringi)p_glGetStringi)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1])); }
        else vpgl_missing("glGetStringi");
        break;
    case GL_FN_COPYBUFFERSUBDATA:
        if (p_glCopyBufferSubData) { ((vpgl_PFN_glCopyBufferSubData)p_glCopyBufferSubData)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLintptr)a[2], (vpgl_GLintptr)a[3], (vpgl_GLsizeiptr)a[4]); }
        else vpgl_missing("glCopyBufferSubData");
        break;
    case GL_FN_GETUNIFORMINDICES:
        if (p_glGetUniformIndices) { ((vpgl_PFN_glGetUniformIndices)p_glGetUniformIndices)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLchar**)vpgl_gptr(a[2]), (vpgl_GLuint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetUniformIndices");
        break;
    case GL_FN_GETACTIVEUNIFORMSIV:
        if (p_glGetActiveUniformsiv) { ((vpgl_PFN_glGetActiveUniformsiv)p_glGetActiveUniformsiv)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLuint*)vpgl_gptr(a[2]), (vpgl_GLenum)a[3], (vpgl_GLint*)vpgl_gptr(a[4])); }
        else vpgl_missing("glGetActiveUniformsiv");
        break;
    case GL_FN_GETUNIFORMBLOCKINDEX:
        if (p_glGetUniformBlockIndex) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glGetUniformBlockIndex)p_glGetUniformBlockIndex)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetUniformBlockIndex");
        break;
    case GL_FN_GETACTIVEUNIFORMBLOCKIV:
        if (p_glGetActiveUniformBlockiv) { ((vpgl_PFN_glGetActiveUniformBlockiv)p_glGetActiveUniformBlockiv)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLenum)a[2], (vpgl_GLint*)vpgl_gptr(a[3])); }
        else vpgl_missing("glGetActiveUniformBlockiv");
        break;
    case GL_FN_GETACTIVEUNIFORMBLOCKNAME:
        if (p_glGetActiveUniformBlockName) { ((vpgl_PFN_glGetActiveUniformBlockName)p_glGetActiveUniformBlockName)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLchar*)vpgl_gptr(a[4])); }
        else vpgl_missing("glGetActiveUniformBlockName");
        break;
    case GL_FN_UNIFORMBLOCKBINDING:
        if (p_glUniformBlockBinding) { ((vpgl_PFN_glUniformBlockBinding)p_glUniformBlockBinding)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLuint)a[2]); }
        else vpgl_missing("glUniformBlockBinding");
        break;
    case GL_FN_DRAWARRAYSINSTANCED:
        if (p_glDrawArraysInstanced) { ((vpgl_PFN_glDrawArraysInstanced)p_glDrawArraysInstanced)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        else vpgl_missing("glDrawArraysInstanced");
        break;
    case GL_FN_DRAWELEMENTSINSTANCED:
        if (p_glDrawElementsInstanced) { ((vpgl_PFN_glDrawElementsInstanced)p_glDrawElementsInstanced)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (const vpgl_void*)vpgl_ptr(a[3], VPGL_PTR_ELEMENT), (vpgl_GLsizei)a[4]); }
        else vpgl_missing("glDrawElementsInstanced");
        break;
    case GL_FN_FENCESYNC:
        if (p_glFenceSync) { *ret = (int64_t)(intptr_t)((vpgl_PFN_glFenceSync)p_glFenceSync)((vpgl_GLenum)a[0], (vpgl_GLbitfield)a[1]); }
        else vpgl_missing("glFenceSync");
        break;
    case GL_FN_ISSYNC:
        if (p_glIsSync) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsSync)p_glIsSync)((vpgl_void*)(uintptr_t)a[0]); }
        else vpgl_missing("glIsSync");
        break;
    case GL_FN_DELETESYNC:
        if (p_glDeleteSync) { ((vpgl_PFN_glDeleteSync)p_glDeleteSync)((vpgl_void*)(uintptr_t)a[0]); }
        else vpgl_missing("glDeleteSync");
        break;
    case GL_FN_CLIENTWAITSYNC:
        if (p_glClientWaitSync) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glClientWaitSync)p_glClientWaitSync)((vpgl_void*)(uintptr_t)a[0], (vpgl_GLbitfield)a[1], (vpgl_GLuint64)a[2]); }
        else vpgl_missing("glClientWaitSync");
        break;
    case GL_FN_WAITSYNC:
        if (p_glWaitSync) { ((vpgl_PFN_glWaitSync)p_glWaitSync)((vpgl_void*)(uintptr_t)a[0], (vpgl_GLbitfield)a[1], (vpgl_GLuint64)a[2]); }
        else vpgl_missing("glWaitSync");
        break;
    case GL_FN_GETINTEGER64V:
        if (p_glGetInteger64v) { ((vpgl_PFN_glGetInteger64v)p_glGetInteger64v)((vpgl_GLenum)a[0], (vpgl_GLint64*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGetInteger64v");
        break;
    case GL_FN_GETSYNCIV:
        if (p_glGetSynciv) { ((vpgl_PFN_glGetSynciv)p_glGetSynciv)((vpgl_void*)(uintptr_t)a[0], (vpgl_GLenum)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLint*)vpgl_gptr(a[4])); }
        else vpgl_missing("glGetSynciv");
        break;
    case GL_FN_GETINTEGER64I_V:
        if (p_glGetInteger64i_v) { ((vpgl_PFN_glGetInteger64i_v)p_glGetInteger64i_v)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1], (vpgl_GLint64*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetInteger64i_v");
        break;
    case GL_FN_GETBUFFERPARAMETERI64V:
        if (p_glGetBufferParameteri64v) { ((vpgl_PFN_glGetBufferParameteri64v)p_glGetBufferParameteri64v)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint64*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetBufferParameteri64v");
        break;
    case GL_FN_GENSAMPLERS:
        if (p_glGenSamplers) { ((vpgl_PFN_glGenSamplers)p_glGenSamplers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenSamplers");
        break;
    case GL_FN_DELETESAMPLERS:
        if (p_glDeleteSamplers) { ((vpgl_PFN_glDeleteSamplers)p_glDeleteSamplers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteSamplers");
        break;
    case GL_FN_ISSAMPLER:
        if (p_glIsSampler) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsSampler)p_glIsSampler)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsSampler");
        break;
    case GL_FN_BINDSAMPLER:
        if (p_glBindSampler) { ((vpgl_PFN_glBindSampler)p_glBindSampler)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindSampler");
        break;
    case GL_FN_SAMPLERPARAMETERI:
        if (p_glSamplerParameteri) { ((vpgl_PFN_glSamplerParameteri)p_glSamplerParameteri)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2]); }
        else vpgl_missing("glSamplerParameteri");
        break;
    case GL_FN_SAMPLERPARAMETERIV:
        if (p_glSamplerParameteriv) { ((vpgl_PFN_glSamplerParameteriv)p_glSamplerParameteriv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glSamplerParameteriv");
        break;
    case GL_FN_SAMPLERPARAMETERF:
        if (p_glSamplerParameterf) { ((vpgl_PFN_glSamplerParameterf)p_glSamplerParameterf)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], vpgl_arg_f(a[2])); }
        else vpgl_missing("glSamplerParameterf");
        break;
    case GL_FN_SAMPLERPARAMETERFV:
        if (p_glSamplerParameterfv) { ((vpgl_PFN_glSamplerParameterfv)p_glSamplerParameterfv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glSamplerParameterfv");
        break;
    case GL_FN_GETSAMPLERPARAMETERIV:
        if (p_glGetSamplerParameteriv) { ((vpgl_PFN_glGetSamplerParameteriv)p_glGetSamplerParameteriv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetSamplerParameteriv");
        break;
    case GL_FN_GETSAMPLERPARAMETERFV:
        if (p_glGetSamplerParameterfv) { ((vpgl_PFN_glGetSamplerParameterfv)p_glGetSamplerParameterfv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        else vpgl_missing("glGetSamplerParameterfv");
        break;
    case GL_FN_VERTEXATTRIBDIVISOR:
        if (p_glVertexAttribDivisor) { ((vpgl_PFN_glVertexAttribDivisor)p_glVertexAttribDivisor)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glVertexAttribDivisor");
        break;
    case GL_FN_BINDTRANSFORMFEEDBACK:
        if (p_glBindTransformFeedback) { ((vpgl_PFN_glBindTransformFeedback)p_glBindTransformFeedback)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        else vpgl_missing("glBindTransformFeedback");
        break;
    case GL_FN_DELETETRANSFORMFEEDBACKS:
        if (p_glDeleteTransformFeedbacks) { ((vpgl_PFN_glDeleteTransformFeedbacks)p_glDeleteTransformFeedbacks)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glDeleteTransformFeedbacks");
        break;
    case GL_FN_GENTRANSFORMFEEDBACKS:
        if (p_glGenTransformFeedbacks) { ((vpgl_PFN_glGenTransformFeedbacks)p_glGenTransformFeedbacks)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        else vpgl_missing("glGenTransformFeedbacks");
        break;
    case GL_FN_ISTRANSFORMFEEDBACK:
        if (p_glIsTransformFeedback) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsTransformFeedback)p_glIsTransformFeedback)((vpgl_GLuint)a[0]); }
        else vpgl_missing("glIsTransformFeedback");
        break;
    case GL_FN_PAUSETRANSFORMFEEDBACK:
        if (p_glPauseTransformFeedback) { ((vpgl_PFN_glPauseTransformFeedback)p_glPauseTransformFeedback)(); }
        else vpgl_missing("glPauseTransformFeedback");
        break;
    case GL_FN_RESUMETRANSFORMFEEDBACK:
        if (p_glResumeTransformFeedback) { ((vpgl_PFN_glResumeTransformFeedback)p_glResumeTransformFeedback)(); }
        else vpgl_missing("glResumeTransformFeedback");
        break;
    case GL_FN_GETPROGRAMBINARY:
        if (p_glGetProgramBinary) { ((vpgl_PFN_glGetProgramBinary)p_glGetProgramBinary)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLenum*)vpgl_gptr(a[3]), (vpgl_void*)vpgl_gptr(a[4])); }
        else vpgl_missing("glGetProgramBinary");
        break;
    case GL_FN_PROGRAMBINARY:
        if (p_glProgramBinary) { ((vpgl_PFN_glProgramBinary)p_glProgramBinary)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (const vpgl_void*)vpgl_gptr(a[2]), (vpgl_GLsizei)a[3]); }
        else vpgl_missing("glProgramBinary");
        break;
    case GL_FN_PROGRAMPARAMETERI:
        if (p_glProgramParameteri) { ((vpgl_PFN_glProgramParameteri)p_glProgramParameteri)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2]); }
        else vpgl_missing("glProgramParameteri");
        break;
    case GL_FN_INVALIDATEFRAMEBUFFER:
        if (p_glInvalidateFramebuffer) { ((vpgl_PFN_glInvalidateFramebuffer)p_glInvalidateFramebuffer)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLenum*)vpgl_gptr(a[2])); }
        else vpgl_missing("glInvalidateFramebuffer");
        break;
    case GL_FN_INVALIDATESUBFRAMEBUFFER:
        if (p_glInvalidateSubFramebuffer) { ((vpgl_PFN_glInvalidateSubFramebuffer)p_glInvalidateSubFramebuffer)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLenum*)vpgl_gptr(a[2]), (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLsizei)a[5], (vpgl_GLsizei)a[6]); }
        else vpgl_missing("glInvalidateSubFramebuffer");
        break;
    case GL_FN_TEXSTORAGE2D:
        if (p_glTexStorage2D) { ((vpgl_PFN_glTexStorage2D)p_glTexStorage2D)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4]); }
        else vpgl_missing("glTexStorage2D");
        break;
    case GL_FN_TEXSTORAGE3D:
        if (p_glTexStorage3D) { ((vpgl_PFN_glTexStorage3D)p_glTexStorage3D)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5]); }
        else vpgl_missing("glTexStorage3D");
        break;
    case GL_FN_GETINTERNALFORMATIV:
        if (p_glGetInternalformativ) { ((vpgl_PFN_glGetInternalformativ)p_glGetInternalformativ)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLint*)vpgl_gptr(a[4])); }
        else vpgl_missing("glGetInternalformativ");
        break;
    default:
        break;
    }
}
