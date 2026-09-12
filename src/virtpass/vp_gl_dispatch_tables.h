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
 * Generic dispatch switches, included from each host's dispatch TU
 * (win32-host/win32_gl_dispatch.c, android-host .../android_gl_host.c).
 * Do not include from anywhere else (defines static functions).
 * `a` is the const int64_t* args array, `ret` the int64_t* out.
 * Guest data pointers are translated by the vpgl_gptr*() helpers
 * defined in the including TU above this include.
 */

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
    case EGL_FN_GETDISPLAY: return "eglGetDisplay";
    case EGL_FN_GETERROR: return "eglGetError";
    case EGL_FN_INITIALIZE: return "eglInitialize";
    case EGL_FN_MAKECURRENT: return "eglMakeCurrent";
    case EGL_FN_QUERYSTRING: return "eglQueryString";
    case EGL_FN_QUERYSURFACE: return "eglQuerySurface";
    case EGL_FN_SWAPBUFFERS: return "eglSwapBuffers";
    case EGL_FN_TERMINATE: return "eglTerminate";
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
    default: return NULL;
    }
}

static void vpgl_dispatch_egl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case EGL_FN_CHOOSECONFIG:
        if (p_eglChooseConfig) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglChooseConfig)p_eglChooseConfig)((vpgl_void*)(uintptr_t)a[0], (const vpgl_EGLint*)vpgl_gptr(a[1]), (vpgl_EGLConfig*)vpgl_gptr(a[2]), (vpgl_EGLint)a[3], (vpgl_EGLint*)vpgl_gptr(a[4])); }
        break;
    case EGL_FN_CREATECONTEXT:
        if (p_eglCreateContext) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreateContext)p_eglCreateContext)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (const vpgl_EGLint*)vpgl_gptr(a[3])); }
        break;
    case EGL_FN_CREATEPBUFFERSURFACE:
        if (p_eglCreatePbufferSurface) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreatePbufferSurface)p_eglCreatePbufferSurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (const vpgl_EGLint*)vpgl_gptr(a[2])); }
        break;
    case EGL_FN_CREATEWINDOWSURFACE:
        if (p_eglCreateWindowSurface) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglCreateWindowSurface)p_eglCreateWindowSurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (const vpgl_EGLint*)vpgl_gptr(a[3])); }
        break;
    case EGL_FN_DESTROYCONTEXT:
        if (p_eglDestroyContext) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglDestroyContext)p_eglDestroyContext)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_DESTROYSURFACE:
        if (p_eglDestroySurface) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglDestroySurface)p_eglDestroySurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_GETCONFIGATTRIB:
        if (p_eglGetConfigAttrib) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglGetConfigAttrib)p_eglGetConfigAttrib)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_EGLint)a[2], (vpgl_EGLint*)vpgl_gptr(a[3])); }
        break;
    case EGL_FN_GETDISPLAY:
        if (p_eglGetDisplay) { *ret = (int64_t)(intptr_t)((vpgl_PFN_eglGetDisplay)p_eglGetDisplay)((vpgl_void*)(uintptr_t)a[0]); }
        break;
    case EGL_FN_GETERROR:
        if (p_eglGetError) { *ret = (int64_t)(int32_t)((vpgl_PFN_eglGetError)p_eglGetError)(); }
        break;
    case EGL_FN_INITIALIZE:
        if (p_eglInitialize) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglInitialize)p_eglInitialize)((vpgl_void*)(uintptr_t)a[0], (vpgl_EGLint*)vpgl_gptr(a[1]), (vpgl_EGLint*)vpgl_gptr(a[2])); }
        break;
    case EGL_FN_MAKECURRENT:
        if (p_eglMakeCurrent) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglMakeCurrent)p_eglMakeCurrent)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_void*)(uintptr_t)a[2], (vpgl_void*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_QUERYSTRING:
        if (p_eglQueryString) { *ret = vpgl_string_out(a, (const char*)((vpgl_PFN_eglQueryString)p_eglQueryString)((vpgl_void*)(uintptr_t)a[0], (vpgl_EGLint)a[1])); }
        break;
    case EGL_FN_QUERYSURFACE:
        if (p_eglQuerySurface) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglQuerySurface)p_eglQuerySurface)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1], (vpgl_EGLint)a[2], (vpgl_EGLint*)vpgl_gptr(a[3])); }
        break;
    case EGL_FN_SWAPBUFFERS:
        if (p_eglSwapBuffers) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglSwapBuffers)p_eglSwapBuffers)((vpgl_void*)(uintptr_t)a[0], (vpgl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_TERMINATE:
        if (p_eglTerminate) { *ret = (int64_t)(uint32_t)((vpgl_PFN_eglTerminate)p_eglTerminate)((vpgl_void*)(uintptr_t)a[0]); }
        break;
    default:
        break;
    }
}

static void vpgl_dispatch_gl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case GL_FN_ACTIVETEXTURE:
        if (p_glActiveTexture) { ((vpgl_PFN_glActiveTexture)p_glActiveTexture)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_ATTACHSHADER:
        if (p_glAttachShader) { ((vpgl_PFN_glAttachShader)p_glAttachShader)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_BINDATTRIBLOCATION:
        if (p_glBindAttribLocation) { ((vpgl_PFN_glBindAttribLocation)p_glBindAttribLocation)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (const vpgl_GLchar*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_BINDBUFFER:
        if (p_glBindBuffer) { ((vpgl_PFN_glBindBuffer)p_glBindBuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_BINDFRAMEBUFFER:
        if (p_glBindFramebuffer) { ((vpgl_PFN_glBindFramebuffer)p_glBindFramebuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_BINDRENDERBUFFER:
        if (p_glBindRenderbuffer) { ((vpgl_PFN_glBindRenderbuffer)p_glBindRenderbuffer)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_BINDTEXTURE:
        if (p_glBindTexture) { ((vpgl_PFN_glBindTexture)p_glBindTexture)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_BLENDCOLOR:
        if (p_glBlendColor) { ((vpgl_PFN_glBlendColor)p_glBlendColor)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        break;
    case GL_FN_BLENDEQUATION:
        if (p_glBlendEquation) { ((vpgl_PFN_glBlendEquation)p_glBlendEquation)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_BLENDEQUATIONSEPARATE:
        if (p_glBlendEquationSeparate) { ((vpgl_PFN_glBlendEquationSeparate)p_glBlendEquationSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        break;
    case GL_FN_BLENDFUNC:
        if (p_glBlendFunc) { ((vpgl_PFN_glBlendFunc)p_glBlendFunc)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        break;
    case GL_FN_BLENDFUNCSEPARATE:
        if (p_glBlendFuncSeparate) { ((vpgl_PFN_glBlendFuncSeparate)p_glBlendFuncSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLenum)a[3]); }
        break;
    case GL_FN_BUFFERDATA:
        if (p_glBufferData) { ((vpgl_PFN_glBufferData)p_glBufferData)((vpgl_GLenum)a[0], (vpgl_GLsizeiptr)a[1], (const vpgl_void*)vpgl_gptr(a[2]), (vpgl_GLenum)a[3]); }
        break;
    case GL_FN_BUFFERSUBDATA:
        if (p_glBufferSubData) { ((vpgl_PFN_glBufferSubData)p_glBufferSubData)((vpgl_GLenum)a[0], (vpgl_GLintptr)a[1], (vpgl_GLsizeiptr)a[2], (const vpgl_void*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_CHECKFRAMEBUFFERSTATUS:
        if (p_glCheckFramebufferStatus) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCheckFramebufferStatus)p_glCheckFramebufferStatus)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_CLEAR:
        if (p_glClear) { ((vpgl_PFN_glClear)p_glClear)((vpgl_GLbitfield)a[0]); }
        break;
    case GL_FN_CLEARCOLOR:
        if (p_glClearColor) { ((vpgl_PFN_glClearColor)p_glClearColor)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        break;
    case GL_FN_CLEARDEPTHF:
        if (p_glClearDepthf) { ((vpgl_PFN_glClearDepthf)p_glClearDepthf)(vpgl_arg_f(a[0])); }
        break;
    case GL_FN_CLEARSTENCIL:
        if (p_glClearStencil) { ((vpgl_PFN_glClearStencil)p_glClearStencil)((vpgl_GLint)a[0]); }
        break;
    case GL_FN_COLORMASK:
        if (p_glColorMask) { ((vpgl_PFN_glColorMask)p_glColorMask)((vpgl_GLboolean)a[0], (vpgl_GLboolean)a[1], (vpgl_GLboolean)a[2], (vpgl_GLboolean)a[3]); }
        break;
    case GL_FN_COMPILESHADER:
        if (p_glCompileShader) { ((vpgl_PFN_glCompileShader)p_glCompileShader)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_COMPRESSEDTEXIMAGE2D:
        if (p_glCompressedTexImage2D) { ((vpgl_PFN_glCompressedTexImage2D)p_glCompressedTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLint)a[5], (vpgl_GLsizei)a[6], (const vpgl_void*)vpgl_gptr(a[7])); }
        break;
    case GL_FN_COMPRESSEDTEXSUBIMAGE2D:
        if (p_glCompressedTexSubImage2D) { ((vpgl_PFN_glCompressedTexSubImage2D)p_glCompressedTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLenum)a[6], (vpgl_GLsizei)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        break;
    case GL_FN_COPYTEXIMAGE2D:
        if (p_glCopyTexImage2D) { ((vpgl_PFN_glCopyTexImage2D)p_glCopyTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLsizei)a[5], (vpgl_GLsizei)a[6], (vpgl_GLint)a[7]); }
        break;
    case GL_FN_COPYTEXSUBIMAGE2D:
        if (p_glCopyTexSubImage2D) { ((vpgl_PFN_glCopyTexSubImage2D)p_glCopyTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4], (vpgl_GLint)a[5], (vpgl_GLsizei)a[6], (vpgl_GLsizei)a[7]); }
        break;
    case GL_FN_CREATEPROGRAM:
        if (p_glCreateProgram) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCreateProgram)p_glCreateProgram)(); }
        break;
    case GL_FN_CREATESHADER:
        if (p_glCreateShader) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glCreateShader)p_glCreateShader)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_CULLFACE:
        if (p_glCullFace) { ((vpgl_PFN_glCullFace)p_glCullFace)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_DELETEBUFFERS:
        if (p_glDeleteBuffers) { ((vpgl_PFN_glDeleteBuffers)p_glDeleteBuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_DELETEFRAMEBUFFERS:
        if (p_glDeleteFramebuffers) { ((vpgl_PFN_glDeleteFramebuffers)p_glDeleteFramebuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_DELETEPROGRAM:
        if (p_glDeleteProgram) { ((vpgl_PFN_glDeleteProgram)p_glDeleteProgram)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_DELETERENDERBUFFERS:
        if (p_glDeleteRenderbuffers) { ((vpgl_PFN_glDeleteRenderbuffers)p_glDeleteRenderbuffers)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_DELETESHADER:
        if (p_glDeleteShader) { ((vpgl_PFN_glDeleteShader)p_glDeleteShader)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_DELETETEXTURES:
        if (p_glDeleteTextures) { ((vpgl_PFN_glDeleteTextures)p_glDeleteTextures)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_DEPTHFUNC:
        if (p_glDepthFunc) { ((vpgl_PFN_glDepthFunc)p_glDepthFunc)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_DEPTHMASK:
        if (p_glDepthMask) { ((vpgl_PFN_glDepthMask)p_glDepthMask)((vpgl_GLboolean)a[0]); }
        break;
    case GL_FN_DEPTHRANGEF:
        if (p_glDepthRangef) { ((vpgl_PFN_glDepthRangef)p_glDepthRangef)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1])); }
        break;
    case GL_FN_DETACHSHADER:
        if (p_glDetachShader) { ((vpgl_PFN_glDetachShader)p_glDetachShader)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_DISABLE:
        if (p_glDisable) { ((vpgl_PFN_glDisable)p_glDisable)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_DISABLEVERTEXATTRIBARRAY:
        if (p_glDisableVertexAttribArray) { ((vpgl_PFN_glDisableVertexAttribArray)p_glDisableVertexAttribArray)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_DRAWARRAYS:
        if (p_glDrawArrays) { ((vpgl_PFN_glDrawArrays)p_glDrawArrays)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2]); }
        break;
    case GL_FN_DRAWELEMENTS:
        if (p_glDrawElements) { ((vpgl_PFN_glDrawElements)p_glDrawElements)((vpgl_GLenum)a[0], (vpgl_GLsizei)a[1], (vpgl_GLenum)a[2], (const vpgl_void*)vpgl_gptr_or_off(a[3])); }
        break;
    case GL_FN_ENABLE:
        if (p_glEnable) { ((vpgl_PFN_glEnable)p_glEnable)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_ENABLEVERTEXATTRIBARRAY:
        if (p_glEnableVertexAttribArray) { ((vpgl_PFN_glEnableVertexAttribArray)p_glEnableVertexAttribArray)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_FINISH:
        if (p_glFinish) { ((vpgl_PFN_glFinish)p_glFinish)(); }
        break;
    case GL_FN_FLUSH:
        if (p_glFlush) { ((vpgl_PFN_glFlush)p_glFlush)(); }
        break;
    case GL_FN_FRAMEBUFFERRENDERBUFFER:
        if (p_glFramebufferRenderbuffer) { ((vpgl_PFN_glFramebufferRenderbuffer)p_glFramebufferRenderbuffer)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLuint)a[3]); }
        break;
    case GL_FN_FRAMEBUFFERTEXTURE2D:
        if (p_glFramebufferTexture2D) { ((vpgl_PFN_glFramebufferTexture2D)p_glFramebufferTexture2D)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLuint)a[3], (vpgl_GLint)a[4]); }
        break;
    case GL_FN_FRONTFACE:
        if (p_glFrontFace) { ((vpgl_PFN_glFrontFace)p_glFrontFace)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_GENBUFFERS:
        if (p_glGenBuffers) { ((vpgl_PFN_glGenBuffers)p_glGenBuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GENERATEMIPMAP:
        if (p_glGenerateMipmap) { ((vpgl_PFN_glGenerateMipmap)p_glGenerateMipmap)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_GENFRAMEBUFFERS:
        if (p_glGenFramebuffers) { ((vpgl_PFN_glGenFramebuffers)p_glGenFramebuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GENRENDERBUFFERS:
        if (p_glGenRenderbuffers) { ((vpgl_PFN_glGenRenderbuffers)p_glGenRenderbuffers)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GENTEXTURES:
        if (p_glGenTextures) { ((vpgl_PFN_glGenTextures)p_glGenTextures)((vpgl_GLsizei)a[0], (vpgl_GLuint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETACTIVEATTRIB:
        if (p_glGetActiveAttrib) { ((vpgl_PFN_glGetActiveAttrib)p_glGetActiveAttrib)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLint*)vpgl_gptr(a[4]), (vpgl_GLenum*)vpgl_gptr(a[5]), (vpgl_GLchar*)vpgl_gptr(a[6])); }
        break;
    case GL_FN_GETACTIVEUNIFORM:
        if (p_glGetActiveUniform) { ((vpgl_PFN_glGetActiveUniform)p_glGetActiveUniform)((vpgl_GLuint)a[0], (vpgl_GLuint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei*)vpgl_gptr(a[3]), (vpgl_GLint*)vpgl_gptr(a[4]), (vpgl_GLenum*)vpgl_gptr(a[5]), (vpgl_GLchar*)vpgl_gptr(a[6])); }
        break;
    case GL_FN_GETATTACHEDSHADERS:
        if (p_glGetAttachedShaders) { ((vpgl_PFN_glGetAttachedShaders)p_glGetAttachedShaders)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLuint*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETATTRIBLOCATION:
        if (p_glGetAttribLocation) { *ret = (int64_t)(int32_t)((vpgl_PFN_glGetAttribLocation)p_glGetAttribLocation)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETBOOLEANV:
        if (p_glGetBooleanv) { ((vpgl_PFN_glGetBooleanv)p_glGetBooleanv)((vpgl_GLenum)a[0], (vpgl_GLboolean*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETBUFFERPARAMETERIV:
        if (p_glGetBufferParameteriv) { ((vpgl_PFN_glGetBufferParameteriv)p_glGetBufferParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETERROR:
        if (p_glGetError) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glGetError)p_glGetError)(); }
        break;
    case GL_FN_GETFLOATV:
        if (p_glGetFloatv) { ((vpgl_PFN_glGetFloatv)p_glGetFloatv)((vpgl_GLenum)a[0], (vpgl_GLfloat*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV:
        if (p_glGetFramebufferAttachmentParameteriv) { ((vpgl_PFN_glGetFramebufferAttachmentParameteriv)p_glGetFramebufferAttachmentParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLint*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETINTEGERV:
        if (p_glGetIntegerv) { ((vpgl_PFN_glGetIntegerv)p_glGetIntegerv)((vpgl_GLenum)a[0], (vpgl_GLint*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETPROGRAMIV:
        if (p_glGetProgramiv) { ((vpgl_PFN_glGetProgramiv)p_glGetProgramiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETPROGRAMINFOLOG:
        if (p_glGetProgramInfoLog) { ((vpgl_PFN_glGetProgramInfoLog)p_glGetProgramInfoLog)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETRENDERBUFFERPARAMETERIV:
        if (p_glGetRenderbufferParameteriv) { ((vpgl_PFN_glGetRenderbufferParameteriv)p_glGetRenderbufferParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETSHADERIV:
        if (p_glGetShaderiv) { ((vpgl_PFN_glGetShaderiv)p_glGetShaderiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETSHADERINFOLOG:
        if (p_glGetShaderInfoLog) { ((vpgl_PFN_glGetShaderInfoLog)p_glGetShaderInfoLog)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETSHADERPRECISIONFORMAT:
        if (p_glGetShaderPrecisionFormat) { ((vpgl_PFN_glGetShaderPrecisionFormat)p_glGetShaderPrecisionFormat)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2]), (vpgl_GLint*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETSHADERSOURCE:
        if (p_glGetShaderSource) { ((vpgl_PFN_glGetShaderSource)p_glGetShaderSource)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLsizei*)vpgl_gptr(a[2]), (vpgl_GLchar*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_GETSTRING:
        if (p_glGetString) { *ret = vpgl_string_out(a, (const char*)((vpgl_PFN_glGetString)p_glGetString)((vpgl_GLenum)a[0])); }
        break;
    case GL_FN_GETTEXPARAMETERFV:
        if (p_glGetTexParameterfv) { ((vpgl_PFN_glGetTexParameterfv)p_glGetTexParameterfv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETTEXPARAMETERIV:
        if (p_glGetTexParameteriv) { ((vpgl_PFN_glGetTexParameteriv)p_glGetTexParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETUNIFORMFV:
        if (p_glGetUniformfv) { ((vpgl_PFN_glGetUniformfv)p_glGetUniformfv)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETUNIFORMIV:
        if (p_glGetUniformiv) { ((vpgl_PFN_glGetUniformiv)p_glGetUniformiv)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETUNIFORMLOCATION:
        if (p_glGetUniformLocation) { *ret = (int64_t)(int32_t)((vpgl_PFN_glGetUniformLocation)p_glGetUniformLocation)((vpgl_GLuint)a[0], (const vpgl_GLchar*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_GETVERTEXATTRIBFV:
        if (p_glGetVertexAttribfv) { ((vpgl_PFN_glGetVertexAttribfv)p_glGetVertexAttribfv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETVERTEXATTRIBIV:
        if (p_glGetVertexAttribiv) { ((vpgl_PFN_glGetVertexAttribiv)p_glGetVertexAttribiv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_GETVERTEXATTRIBPOINTERV:
        if (p_glGetVertexAttribPointerv) { ((vpgl_PFN_glGetVertexAttribPointerv)p_glGetVertexAttribPointerv)((vpgl_GLuint)a[0], (vpgl_GLenum)a[1], (vpgl_void**)vpgl_gptr(a[2])); }
        break;
    case GL_FN_HINT:
        if (p_glHint) { ((vpgl_PFN_glHint)p_glHint)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1]); }
        break;
    case GL_FN_ISBUFFER:
        if (p_glIsBuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsBuffer)p_glIsBuffer)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_ISENABLED:
        if (p_glIsEnabled) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsEnabled)p_glIsEnabled)((vpgl_GLenum)a[0]); }
        break;
    case GL_FN_ISFRAMEBUFFER:
        if (p_glIsFramebuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsFramebuffer)p_glIsFramebuffer)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_ISPROGRAM:
        if (p_glIsProgram) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsProgram)p_glIsProgram)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_ISRENDERBUFFER:
        if (p_glIsRenderbuffer) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsRenderbuffer)p_glIsRenderbuffer)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_ISSHADER:
        if (p_glIsShader) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsShader)p_glIsShader)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_ISTEXTURE:
        if (p_glIsTexture) { *ret = (int64_t)(uint32_t)((vpgl_PFN_glIsTexture)p_glIsTexture)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_LINEWIDTH:
        if (p_glLineWidth) { ((vpgl_PFN_glLineWidth)p_glLineWidth)(vpgl_arg_f(a[0])); }
        break;
    case GL_FN_LINKPROGRAM:
        if (p_glLinkProgram) { ((vpgl_PFN_glLinkProgram)p_glLinkProgram)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_PIXELSTOREI:
        if (p_glPixelStorei) { ((vpgl_PFN_glPixelStorei)p_glPixelStorei)((vpgl_GLenum)a[0], (vpgl_GLint)a[1]); }
        break;
    case GL_FN_POLYGONOFFSET:
        if (p_glPolygonOffset) { ((vpgl_PFN_glPolygonOffset)p_glPolygonOffset)(vpgl_arg_f(a[0]), vpgl_arg_f(a[1])); }
        break;
    case GL_FN_READPIXELS:
        if (p_glReadPixels) { ((vpgl_PFN_glReadPixels)p_glReadPixels)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3], (vpgl_GLenum)a[4], (vpgl_GLenum)a[5], (vpgl_void*)vpgl_gptr(a[6])); }
        break;
    case GL_FN_RELEASESHADERCOMPILER:
        if (p_glReleaseShaderCompiler) { ((vpgl_PFN_glReleaseShaderCompiler)p_glReleaseShaderCompiler)(); }
        break;
    case GL_FN_RENDERBUFFERSTORAGE:
        if (p_glRenderbufferStorage) { ((vpgl_PFN_glRenderbufferStorage)p_glRenderbufferStorage)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        break;
    case GL_FN_SAMPLECOVERAGE:
        if (p_glSampleCoverage) { ((vpgl_PFN_glSampleCoverage)p_glSampleCoverage)(vpgl_arg_f(a[0]), (vpgl_GLboolean)a[1]); }
        break;
    case GL_FN_SCISSOR:
        if (p_glScissor) { ((vpgl_PFN_glScissor)p_glScissor)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        break;
    case GL_FN_SHADERBINARY:
        if (p_glShaderBinary) { ((vpgl_PFN_glShaderBinary)p_glShaderBinary)((vpgl_GLsizei)a[0], (const vpgl_GLuint*)vpgl_gptr(a[1]), (vpgl_GLenum)a[2], (const vpgl_void*)vpgl_gptr(a[3]), (vpgl_GLsizei)a[4]); }
        break;
    case GL_FN_SHADERSOURCE:
        if (p_glShaderSource) { ((vpgl_PFN_glShaderSource)p_glShaderSource)((vpgl_GLuint)a[0], (vpgl_GLsizei)a[1], vpgl_translate_shader_srcs(a, (int)a[1]), (const vpgl_GLint*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_STENCILFUNC:
        if (p_glStencilFunc) { ((vpgl_PFN_glStencilFunc)p_glStencilFunc)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLuint)a[2]); }
        break;
    case GL_FN_STENCILFUNCSEPARATE:
        if (p_glStencilFuncSeparate) { ((vpgl_PFN_glStencilFuncSeparate)p_glStencilFuncSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2], (vpgl_GLuint)a[3]); }
        break;
    case GL_FN_STENCILMASK:
        if (p_glStencilMask) { ((vpgl_PFN_glStencilMask)p_glStencilMask)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_STENCILMASKSEPARATE:
        if (p_glStencilMaskSeparate) { ((vpgl_PFN_glStencilMaskSeparate)p_glStencilMaskSeparate)((vpgl_GLenum)a[0], (vpgl_GLuint)a[1]); }
        break;
    case GL_FN_STENCILOP:
        if (p_glStencilOp) { ((vpgl_PFN_glStencilOp)p_glStencilOp)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2]); }
        break;
    case GL_FN_STENCILOPSEPARATE:
        if (p_glStencilOpSeparate) { ((vpgl_PFN_glStencilOpSeparate)p_glStencilOpSeparate)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLenum)a[2], (vpgl_GLenum)a[3]); }
        break;
    case GL_FN_TEXIMAGE2D:
        if (p_glTexImage2D) { ((vpgl_PFN_glTexImage2D)p_glTexImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLsizei)a[3], (vpgl_GLsizei)a[4], (vpgl_GLint)a[5], (vpgl_GLenum)a[6], (vpgl_GLenum)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        break;
    case GL_FN_TEXPARAMETERF:
        if (p_glTexParameterf) { ((vpgl_PFN_glTexParameterf)p_glTexParameterf)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], vpgl_arg_f(a[2])); }
        break;
    case GL_FN_TEXPARAMETERFV:
        if (p_glTexParameterfv) { ((vpgl_PFN_glTexParameterfv)p_glTexParameterfv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_TEXPARAMETERI:
        if (p_glTexParameteri) { ((vpgl_PFN_glTexParameteri)p_glTexParameteri)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (vpgl_GLint)a[2]); }
        break;
    case GL_FN_TEXPARAMETERIV:
        if (p_glTexParameteriv) { ((vpgl_PFN_glTexParameteriv)p_glTexParameteriv)((vpgl_GLenum)a[0], (vpgl_GLenum)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_TEXSUBIMAGE2D:
        if (p_glTexSubImage2D) { ((vpgl_PFN_glTexSubImage2D)p_glTexSubImage2D)((vpgl_GLenum)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLsizei)a[4], (vpgl_GLsizei)a[5], (vpgl_GLenum)a[6], (vpgl_GLenum)a[7], (const vpgl_void*)vpgl_gptr(a[8])); }
        break;
    case GL_FN_UNIFORM1F:
        if (p_glUniform1f) { ((vpgl_PFN_glUniform1f)p_glUniform1f)((vpgl_GLint)a[0], vpgl_arg_f(a[1])); }
        break;
    case GL_FN_UNIFORM1FV:
        if (p_glUniform1fv) { ((vpgl_PFN_glUniform1fv)p_glUniform1fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM1I:
        if (p_glUniform1i) { ((vpgl_PFN_glUniform1i)p_glUniform1i)((vpgl_GLint)a[0], (vpgl_GLint)a[1]); }
        break;
    case GL_FN_UNIFORM1IV:
        if (p_glUniform1iv) { ((vpgl_PFN_glUniform1iv)p_glUniform1iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM2F:
        if (p_glUniform2f) { ((vpgl_PFN_glUniform2f)p_glUniform2f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2])); }
        break;
    case GL_FN_UNIFORM2FV:
        if (p_glUniform2fv) { ((vpgl_PFN_glUniform2fv)p_glUniform2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM2I:
        if (p_glUniform2i) { ((vpgl_PFN_glUniform2i)p_glUniform2i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2]); }
        break;
    case GL_FN_UNIFORM2IV:
        if (p_glUniform2iv) { ((vpgl_PFN_glUniform2iv)p_glUniform2iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM3F:
        if (p_glUniform3f) { ((vpgl_PFN_glUniform3f)p_glUniform3f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        break;
    case GL_FN_UNIFORM3FV:
        if (p_glUniform3fv) { ((vpgl_PFN_glUniform3fv)p_glUniform3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM3I:
        if (p_glUniform3i) { ((vpgl_PFN_glUniform3i)p_glUniform3i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3]); }
        break;
    case GL_FN_UNIFORM3IV:
        if (p_glUniform3iv) { ((vpgl_PFN_glUniform3iv)p_glUniform3iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM4F:
        if (p_glUniform4f) { ((vpgl_PFN_glUniform4f)p_glUniform4f)((vpgl_GLint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3]), vpgl_arg_f(a[4])); }
        break;
    case GL_FN_UNIFORM4FV:
        if (p_glUniform4fv) { ((vpgl_PFN_glUniform4fv)p_glUniform4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLfloat*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORM4I:
        if (p_glUniform4i) { ((vpgl_PFN_glUniform4i)p_glUniform4i)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLint)a[2], (vpgl_GLint)a[3], (vpgl_GLint)a[4]); }
        break;
    case GL_FN_UNIFORM4IV:
        if (p_glUniform4iv) { ((vpgl_PFN_glUniform4iv)p_glUniform4iv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (const vpgl_GLint*)vpgl_gptr(a[2])); }
        break;
    case GL_FN_UNIFORMMATRIX2FV:
        if (p_glUniformMatrix2fv) { ((vpgl_PFN_glUniformMatrix2fv)p_glUniformMatrix2fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_UNIFORMMATRIX3FV:
        if (p_glUniformMatrix3fv) { ((vpgl_PFN_glUniformMatrix3fv)p_glUniformMatrix3fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_UNIFORMMATRIX4FV:
        if (p_glUniformMatrix4fv) { ((vpgl_PFN_glUniformMatrix4fv)p_glUniformMatrix4fv)((vpgl_GLint)a[0], (vpgl_GLsizei)a[1], (vpgl_GLboolean)a[2], (const vpgl_GLfloat*)vpgl_gptr(a[3])); }
        break;
    case GL_FN_USEPROGRAM:
        if (p_glUseProgram) { ((vpgl_PFN_glUseProgram)p_glUseProgram)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_VALIDATEPROGRAM:
        if (p_glValidateProgram) { ((vpgl_PFN_glValidateProgram)p_glValidateProgram)((vpgl_GLuint)a[0]); }
        break;
    case GL_FN_VERTEXATTRIB1F:
        if (p_glVertexAttrib1f) { ((vpgl_PFN_glVertexAttrib1f)p_glVertexAttrib1f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1])); }
        break;
    case GL_FN_VERTEXATTRIB1FV:
        if (p_glVertexAttrib1fv) { ((vpgl_PFN_glVertexAttrib1fv)p_glVertexAttrib1fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_VERTEXATTRIB2F:
        if (p_glVertexAttrib2f) { ((vpgl_PFN_glVertexAttrib2f)p_glVertexAttrib2f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2])); }
        break;
    case GL_FN_VERTEXATTRIB2FV:
        if (p_glVertexAttrib2fv) { ((vpgl_PFN_glVertexAttrib2fv)p_glVertexAttrib2fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_VERTEXATTRIB3F:
        if (p_glVertexAttrib3f) { ((vpgl_PFN_glVertexAttrib3f)p_glVertexAttrib3f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3])); }
        break;
    case GL_FN_VERTEXATTRIB3FV:
        if (p_glVertexAttrib3fv) { ((vpgl_PFN_glVertexAttrib3fv)p_glVertexAttrib3fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_VERTEXATTRIB4F:
        if (p_glVertexAttrib4f) { ((vpgl_PFN_glVertexAttrib4f)p_glVertexAttrib4f)((vpgl_GLuint)a[0], vpgl_arg_f(a[1]), vpgl_arg_f(a[2]), vpgl_arg_f(a[3]), vpgl_arg_f(a[4])); }
        break;
    case GL_FN_VERTEXATTRIB4FV:
        if (p_glVertexAttrib4fv) { ((vpgl_PFN_glVertexAttrib4fv)p_glVertexAttrib4fv)((vpgl_GLuint)a[0], (const vpgl_GLfloat*)vpgl_gptr(a[1])); }
        break;
    case GL_FN_VERTEXATTRIBPOINTER:
        if (p_glVertexAttribPointer) { ((vpgl_PFN_glVertexAttribPointer)p_glVertexAttribPointer)((vpgl_GLuint)a[0], (vpgl_GLint)a[1], (vpgl_GLenum)a[2], (vpgl_GLboolean)a[3], (vpgl_GLsizei)a[4], (const vpgl_void*)vpgl_gptr_or_off(a[5])); }
        break;
    case GL_FN_VIEWPORT:
        if (p_glViewport) { ((vpgl_PFN_glViewport)p_glViewport)((vpgl_GLint)a[0], (vpgl_GLint)a[1], (vpgl_GLsizei)a[2], (vpgl_GLsizei)a[3]); }
        break;
    default:
        break;
    }
}
