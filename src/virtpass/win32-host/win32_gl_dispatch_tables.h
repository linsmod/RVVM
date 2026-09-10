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

/*
 * Generic dispatch switches, included from win32_gl_dispatch.c.
 * Do not include from anywhere else (defines static functions).
 * `a` is the const int64_t* args array, `ret` the int64_t* out.
 */

static void w32gl_dispatch_egl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case EGL_FN_CHOOSECONFIG:
        if (p_eglChooseConfig) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglChooseConfig)p_eglChooseConfig)((w32gl_void*)(uintptr_t)a[0], (const w32gl_EGLint*)(uintptr_t)a[1], (w32gl_void*)(uintptr_t)a[2], (w32gl_EGLint)a[3], (w32gl_EGLint*)(uintptr_t)a[4]); }
        break;
    case EGL_FN_CREATECONTEXT:
        if (p_eglCreateContext) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglCreateContext)p_eglCreateContext)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (w32gl_void*)(uintptr_t)a[2], (const w32gl_EGLint*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_CREATEPBUFFERSURFACE:
        if (p_eglCreatePbufferSurface) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglCreatePbufferSurface)p_eglCreatePbufferSurface)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (const w32gl_EGLint*)(uintptr_t)a[2]); }
        break;
    case EGL_FN_CREATEWINDOWSURFACE:
        if (p_eglCreateWindowSurface) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglCreateWindowSurface)p_eglCreateWindowSurface)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (w32gl_void*)(uintptr_t)a[2], (const w32gl_EGLint*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_DESTROYCONTEXT:
        if (p_eglDestroyContext) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglDestroyContext)p_eglDestroyContext)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_DESTROYSURFACE:
        if (p_eglDestroySurface) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglDestroySurface)p_eglDestroySurface)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_GETCONFIGATTRIB:
        if (p_eglGetConfigAttrib) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglGetConfigAttrib)p_eglGetConfigAttrib)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (w32gl_EGLint)a[2], (w32gl_EGLint*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_GETDISPLAY:
        if (p_eglGetDisplay) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglGetDisplay)p_eglGetDisplay)((w32gl_void*)(uintptr_t)a[0]); }
        break;
    case EGL_FN_GETERROR:
        if (p_eglGetError) { *ret = (int64_t)(int32_t)((w32gl_PFN_eglGetError)p_eglGetError)(); }
        break;
    case EGL_FN_GETPROCADDRESS:
        if (p_eglGetProcAddress) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglGetProcAddress)p_eglGetProcAddress)((const w32gl_char*)(uintptr_t)a[0]); }
        break;
    case EGL_FN_INITIALIZE:
        if (p_eglInitialize) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglInitialize)p_eglInitialize)((w32gl_void*)(uintptr_t)a[0], (w32gl_EGLint*)(uintptr_t)a[1], (w32gl_EGLint*)(uintptr_t)a[2]); }
        break;
    case EGL_FN_MAKECURRENT:
        if (p_eglMakeCurrent) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglMakeCurrent)p_eglMakeCurrent)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (w32gl_void*)(uintptr_t)a[2], (w32gl_void*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_QUERYSTRING:
        if (p_eglQueryString) { *ret = (int64_t)(intptr_t)((w32gl_PFN_eglQueryString)p_eglQueryString)((w32gl_void*)(uintptr_t)a[0], (w32gl_EGLint)a[1]); }
        break;
    case EGL_FN_QUERYSURFACE:
        if (p_eglQuerySurface) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglQuerySurface)p_eglQuerySurface)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1], (w32gl_EGLint)a[2], (w32gl_EGLint*)(uintptr_t)a[3]); }
        break;
    case EGL_FN_SWAPBUFFERS:
        if (p_eglSwapBuffers) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglSwapBuffers)p_eglSwapBuffers)((w32gl_void*)(uintptr_t)a[0], (w32gl_void*)(uintptr_t)a[1]); }
        break;
    case EGL_FN_TERMINATE:
        if (p_eglTerminate) { *ret = (int64_t)(uint32_t)((w32gl_PFN_eglTerminate)p_eglTerminate)((w32gl_void*)(uintptr_t)a[0]); }
        break;
    default:
        break;
    }
}

static void w32gl_dispatch_gl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)
{
    switch (fn_id) {
    case GL_FN_ACTIVETEXTURE:
        if (p_glActiveTexture) { ((w32gl_PFN_glActiveTexture)p_glActiveTexture)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_ATTACHSHADER:
        if (p_glAttachShader) { ((w32gl_PFN_glAttachShader)p_glAttachShader)((w32gl_GLuint)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_BINDATTRIBLOCATION:
        if (p_glBindAttribLocation) { ((w32gl_PFN_glBindAttribLocation)p_glBindAttribLocation)((w32gl_GLuint)a[0], (w32gl_GLuint)a[1], (const w32gl_GLchar*)(uintptr_t)a[2]); }
        break;
    case GL_FN_BINDBUFFER:
        if (p_glBindBuffer) { ((w32gl_PFN_glBindBuffer)p_glBindBuffer)((w32gl_GLenum)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_BINDFRAMEBUFFER:
        if (p_glBindFramebuffer) { ((w32gl_PFN_glBindFramebuffer)p_glBindFramebuffer)((w32gl_GLenum)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_BINDRENDERBUFFER:
        if (p_glBindRenderbuffer) { ((w32gl_PFN_glBindRenderbuffer)p_glBindRenderbuffer)((w32gl_GLenum)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_BINDTEXTURE:
        if (p_glBindTexture) { ((w32gl_PFN_glBindTexture)p_glBindTexture)((w32gl_GLenum)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_BLENDCOLOR:
        if (p_glBlendColor) { ((w32gl_PFN_glBlendColor)p_glBlendColor)(w32gl_arg_f(a[0]), w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3])); }
        break;
    case GL_FN_BLENDEQUATION:
        if (p_glBlendEquation) { ((w32gl_PFN_glBlendEquation)p_glBlendEquation)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_BLENDEQUATIONSEPARATE:
        if (p_glBlendEquationSeparate) { ((w32gl_PFN_glBlendEquationSeparate)p_glBlendEquationSeparate)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1]); }
        break;
    case GL_FN_BLENDFUNC:
        if (p_glBlendFunc) { ((w32gl_PFN_glBlendFunc)p_glBlendFunc)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1]); }
        break;
    case GL_FN_BLENDFUNCSEPARATE:
        if (p_glBlendFuncSeparate) { ((w32gl_PFN_glBlendFuncSeparate)p_glBlendFuncSeparate)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2], (w32gl_GLenum)a[3]); }
        break;
    case GL_FN_BUFFERDATA:
        if (p_glBufferData) { ((w32gl_PFN_glBufferData)p_glBufferData)((w32gl_GLenum)a[0], (w32gl_GLsizeiptr)a[1], (const w32gl_void*)(uintptr_t)a[2], (w32gl_GLenum)a[3]); }
        break;
    case GL_FN_BUFFERSUBDATA:
        if (p_glBufferSubData) { ((w32gl_PFN_glBufferSubData)p_glBufferSubData)((w32gl_GLenum)a[0], (w32gl_GLintptr)a[1], (w32gl_GLsizeiptr)a[2], (const w32gl_void*)(uintptr_t)a[3]); }
        break;
    case GL_FN_CHECKFRAMEBUFFERSTATUS:
        if (p_glCheckFramebufferStatus) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glCheckFramebufferStatus)p_glCheckFramebufferStatus)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_CLEAR:
        if (p_glClear) { ((w32gl_PFN_glClear)p_glClear)((w32gl_GLbitfield)a[0]); }
        break;
    case GL_FN_CLEARCOLOR:
        if (p_glClearColor) { ((w32gl_PFN_glClearColor)p_glClearColor)(w32gl_arg_f(a[0]), w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3])); }
        break;
    case GL_FN_CLEARDEPTHF:
        if (p_glClearDepthf) { ((w32gl_PFN_glClearDepthf)p_glClearDepthf)(w32gl_arg_f(a[0])); }
        break;
    case GL_FN_CLEARSTENCIL:
        if (p_glClearStencil) { ((w32gl_PFN_glClearStencil)p_glClearStencil)((w32gl_GLint)a[0]); }
        break;
    case GL_FN_COLORMASK:
        if (p_glColorMask) { ((w32gl_PFN_glColorMask)p_glColorMask)((w32gl_GLboolean)a[0], (w32gl_GLboolean)a[1], (w32gl_GLboolean)a[2], (w32gl_GLboolean)a[3]); }
        break;
    case GL_FN_COMPILESHADER:
        if (p_glCompileShader) { ((w32gl_PFN_glCompileShader)p_glCompileShader)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_COMPRESSEDTEXIMAGE2D:
        if (p_glCompressedTexImage2D) { ((w32gl_PFN_glCompressedTexImage2D)p_glCompressedTexImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLenum)a[2], (w32gl_GLsizei)a[3], (w32gl_GLsizei)a[4], (w32gl_GLint)a[5], (w32gl_GLsizei)a[6], (const w32gl_void*)(uintptr_t)a[7]); }
        break;
    case GL_FN_COMPRESSEDTEXSUBIMAGE2D:
        if (p_glCompressedTexSubImage2D) { ((w32gl_PFN_glCompressedTexSubImage2D)p_glCompressedTexSubImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLint)a[3], (w32gl_GLsizei)a[4], (w32gl_GLsizei)a[5], (w32gl_GLenum)a[6], (w32gl_GLsizei)a[7], (const w32gl_void*)(uintptr_t)a[8]); }
        break;
    case GL_FN_COPYTEXIMAGE2D:
        if (p_glCopyTexImage2D) { ((w32gl_PFN_glCopyTexImage2D)p_glCopyTexImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLenum)a[2], (w32gl_GLint)a[3], (w32gl_GLint)a[4], (w32gl_GLsizei)a[5], (w32gl_GLsizei)a[6], (w32gl_GLint)a[7]); }
        break;
    case GL_FN_COPYTEXSUBIMAGE2D:
        if (p_glCopyTexSubImage2D) { ((w32gl_PFN_glCopyTexSubImage2D)p_glCopyTexSubImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLint)a[3], (w32gl_GLint)a[4], (w32gl_GLint)a[5], (w32gl_GLsizei)a[6], (w32gl_GLsizei)a[7]); }
        break;
    case GL_FN_CREATEPROGRAM:
        if (p_glCreateProgram) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glCreateProgram)p_glCreateProgram)(); }
        break;
    case GL_FN_CREATESHADER:
        if (p_glCreateShader) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glCreateShader)p_glCreateShader)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_CULLFACE:
        if (p_glCullFace) { ((w32gl_PFN_glCullFace)p_glCullFace)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_DELETEBUFFERS:
        if (p_glDeleteBuffers) { ((w32gl_PFN_glDeleteBuffers)p_glDeleteBuffers)((w32gl_GLsizei)a[0], (const w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_DELETEFRAMEBUFFERS:
        if (p_glDeleteFramebuffers) { ((w32gl_PFN_glDeleteFramebuffers)p_glDeleteFramebuffers)((w32gl_GLsizei)a[0], (const w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_DELETEPROGRAM:
        if (p_glDeleteProgram) { ((w32gl_PFN_glDeleteProgram)p_glDeleteProgram)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_DELETERENDERBUFFERS:
        if (p_glDeleteRenderbuffers) { ((w32gl_PFN_glDeleteRenderbuffers)p_glDeleteRenderbuffers)((w32gl_GLsizei)a[0], (const w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_DELETESHADER:
        if (p_glDeleteShader) { ((w32gl_PFN_glDeleteShader)p_glDeleteShader)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_DELETETEXTURES:
        if (p_glDeleteTextures) { ((w32gl_PFN_glDeleteTextures)p_glDeleteTextures)((w32gl_GLsizei)a[0], (const w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_DEPTHFUNC:
        if (p_glDepthFunc) { ((w32gl_PFN_glDepthFunc)p_glDepthFunc)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_DEPTHMASK:
        if (p_glDepthMask) { ((w32gl_PFN_glDepthMask)p_glDepthMask)((w32gl_GLboolean)a[0]); }
        break;
    case GL_FN_DEPTHRANGEF:
        if (p_glDepthRangef) { ((w32gl_PFN_glDepthRangef)p_glDepthRangef)(w32gl_arg_f(a[0]), w32gl_arg_f(a[1])); }
        break;
    case GL_FN_DETACHSHADER:
        if (p_glDetachShader) { ((w32gl_PFN_glDetachShader)p_glDetachShader)((w32gl_GLuint)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_DISABLE:
        if (p_glDisable) { ((w32gl_PFN_glDisable)p_glDisable)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_DISABLEVERTEXATTRIBARRAY:
        if (p_glDisableVertexAttribArray) { ((w32gl_PFN_glDisableVertexAttribArray)p_glDisableVertexAttribArray)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_DRAWARRAYS:
        if (p_glDrawArrays) { ((w32gl_PFN_glDrawArrays)p_glDrawArrays)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLsizei)a[2]); }
        break;
    case GL_FN_DRAWELEMENTS:
        if (p_glDrawElements) { ((w32gl_PFN_glDrawElements)p_glDrawElements)((w32gl_GLenum)a[0], (w32gl_GLsizei)a[1], (w32gl_GLenum)a[2], (const w32gl_void*)(uintptr_t)a[3]); }
        break;
    case GL_FN_ENABLE:
        if (p_glEnable) { ((w32gl_PFN_glEnable)p_glEnable)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_ENABLEVERTEXATTRIBARRAY:
        if (p_glEnableVertexAttribArray) { ((w32gl_PFN_glEnableVertexAttribArray)p_glEnableVertexAttribArray)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_FINISH:
        if (p_glFinish) { ((w32gl_PFN_glFinish)p_glFinish)(); }
        break;
    case GL_FN_FLUSH:
        if (p_glFlush) { ((w32gl_PFN_glFlush)p_glFlush)(); }
        break;
    case GL_FN_FRAMEBUFFERRENDERBUFFER:
        if (p_glFramebufferRenderbuffer) { ((w32gl_PFN_glFramebufferRenderbuffer)p_glFramebufferRenderbuffer)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2], (w32gl_GLuint)a[3]); }
        break;
    case GL_FN_FRAMEBUFFERTEXTURE2D:
        if (p_glFramebufferTexture2D) { ((w32gl_PFN_glFramebufferTexture2D)p_glFramebufferTexture2D)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2], (w32gl_GLuint)a[3], (w32gl_GLint)a[4]); }
        break;
    case GL_FN_FRONTFACE:
        if (p_glFrontFace) { ((w32gl_PFN_glFrontFace)p_glFrontFace)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_GENBUFFERS:
        if (p_glGenBuffers) { ((w32gl_PFN_glGenBuffers)p_glGenBuffers)((w32gl_GLsizei)a[0], (w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GENERATEMIPMAP:
        if (p_glGenerateMipmap) { ((w32gl_PFN_glGenerateMipmap)p_glGenerateMipmap)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_GENFRAMEBUFFERS:
        if (p_glGenFramebuffers) { ((w32gl_PFN_glGenFramebuffers)p_glGenFramebuffers)((w32gl_GLsizei)a[0], (w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GENRENDERBUFFERS:
        if (p_glGenRenderbuffers) { ((w32gl_PFN_glGenRenderbuffers)p_glGenRenderbuffers)((w32gl_GLsizei)a[0], (w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GENTEXTURES:
        if (p_glGenTextures) { ((w32gl_PFN_glGenTextures)p_glGenTextures)((w32gl_GLsizei)a[0], (w32gl_GLuint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETACTIVEATTRIB:
        if (p_glGetActiveAttrib) { ((w32gl_PFN_glGetActiveAttrib)p_glGetActiveAttrib)((w32gl_GLuint)a[0], (w32gl_GLuint)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei*)(uintptr_t)a[3], (w32gl_GLint*)(uintptr_t)a[4], (w32gl_GLenum*)(uintptr_t)a[5], (w32gl_GLchar*)(uintptr_t)a[6]); }
        break;
    case GL_FN_GETACTIVEUNIFORM:
        if (p_glGetActiveUniform) { ((w32gl_PFN_glGetActiveUniform)p_glGetActiveUniform)((w32gl_GLuint)a[0], (w32gl_GLuint)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei*)(uintptr_t)a[3], (w32gl_GLint*)(uintptr_t)a[4], (w32gl_GLenum*)(uintptr_t)a[5], (w32gl_GLchar*)(uintptr_t)a[6]); }
        break;
    case GL_FN_GETATTACHEDSHADERS:
        if (p_glGetAttachedShaders) { ((w32gl_PFN_glGetAttachedShaders)p_glGetAttachedShaders)((w32gl_GLuint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLsizei*)(uintptr_t)a[2], (w32gl_GLuint*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETATTRIBLOCATION:
        if (p_glGetAttribLocation) { *ret = (int64_t)(int32_t)((w32gl_PFN_glGetAttribLocation)p_glGetAttribLocation)((w32gl_GLuint)a[0], (const w32gl_GLchar*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETBOOLEANV:
        if (p_glGetBooleanv) { ((w32gl_PFN_glGetBooleanv)p_glGetBooleanv)((w32gl_GLenum)a[0], (w32gl_GLboolean*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETBUFFERPARAMETERIV:
        if (p_glGetBufferParameteriv) { ((w32gl_PFN_glGetBufferParameteriv)p_glGetBufferParameteriv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETERROR:
        if (p_glGetError) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glGetError)p_glGetError)(); }
        break;
    case GL_FN_GETFLOATV:
        if (p_glGetFloatv) { ((w32gl_PFN_glGetFloatv)p_glGetFloatv)((w32gl_GLenum)a[0], (w32gl_GLfloat*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV:
        if (p_glGetFramebufferAttachmentParameteriv) { ((w32gl_PFN_glGetFramebufferAttachmentParameteriv)p_glGetFramebufferAttachmentParameteriv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2], (w32gl_GLint*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETINTEGERV:
        if (p_glGetIntegerv) { ((w32gl_PFN_glGetIntegerv)p_glGetIntegerv)((w32gl_GLenum)a[0], (w32gl_GLint*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETPROGRAMIV:
        if (p_glGetProgramiv) { ((w32gl_PFN_glGetProgramiv)p_glGetProgramiv)((w32gl_GLuint)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETPROGRAMINFOLOG:
        if (p_glGetProgramInfoLog) { ((w32gl_PFN_glGetProgramInfoLog)p_glGetProgramInfoLog)((w32gl_GLuint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLsizei*)(uintptr_t)a[2], (w32gl_GLchar*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETRENDERBUFFERPARAMETERIV:
        if (p_glGetRenderbufferParameteriv) { ((w32gl_PFN_glGetRenderbufferParameteriv)p_glGetRenderbufferParameteriv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETSHADERIV:
        if (p_glGetShaderiv) { ((w32gl_PFN_glGetShaderiv)p_glGetShaderiv)((w32gl_GLuint)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETSHADERINFOLOG:
        if (p_glGetShaderInfoLog) { ((w32gl_PFN_glGetShaderInfoLog)p_glGetShaderInfoLog)((w32gl_GLuint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLsizei*)(uintptr_t)a[2], (w32gl_GLchar*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETSHADERPRECISIONFORMAT:
        if (p_glGetShaderPrecisionFormat) { ((w32gl_PFN_glGetShaderPrecisionFormat)p_glGetShaderPrecisionFormat)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2], (w32gl_GLint*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETSHADERSOURCE:
        if (p_glGetShaderSource) { ((w32gl_PFN_glGetShaderSource)p_glGetShaderSource)((w32gl_GLuint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLsizei*)(uintptr_t)a[2], (w32gl_GLchar*)(uintptr_t)a[3]); }
        break;
    case GL_FN_GETSTRING:
        if (p_glGetString) { *ret = (int64_t)(intptr_t)((w32gl_PFN_glGetString)p_glGetString)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_GETTEXPARAMETERFV:
        if (p_glGetTexParameterfv) { ((w32gl_PFN_glGetTexParameterfv)p_glGetTexParameterfv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETTEXPARAMETERIV:
        if (p_glGetTexParameteriv) { ((w32gl_PFN_glGetTexParameteriv)p_glGetTexParameteriv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETUNIFORMFV:
        if (p_glGetUniformfv) { ((w32gl_PFN_glGetUniformfv)p_glGetUniformfv)((w32gl_GLuint)a[0], (w32gl_GLint)a[1], (w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETUNIFORMIV:
        if (p_glGetUniformiv) { ((w32gl_PFN_glGetUniformiv)p_glGetUniformiv)((w32gl_GLuint)a[0], (w32gl_GLint)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETUNIFORMLOCATION:
        if (p_glGetUniformLocation) { *ret = (int64_t)(int32_t)((w32gl_PFN_glGetUniformLocation)p_glGetUniformLocation)((w32gl_GLuint)a[0], (const w32gl_GLchar*)(uintptr_t)a[1]); }
        break;
    case GL_FN_GETVERTEXATTRIBFV:
        if (p_glGetVertexAttribfv) { ((w32gl_PFN_glGetVertexAttribfv)p_glGetVertexAttribfv)((w32gl_GLuint)a[0], (w32gl_GLenum)a[1], (w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETVERTEXATTRIBIV:
        if (p_glGetVertexAttribiv) { ((w32gl_PFN_glGetVertexAttribiv)p_glGetVertexAttribiv)((w32gl_GLuint)a[0], (w32gl_GLenum)a[1], (w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_GETVERTEXATTRIBPOINTERV:
        if (p_glGetVertexAttribPointerv) { ((w32gl_PFN_glGetVertexAttribPointerv)p_glGetVertexAttribPointerv)((w32gl_GLuint)a[0], (w32gl_GLenum)a[1], (w32gl_void**)(uintptr_t)a[2]); }
        break;
    case GL_FN_HINT:
        if (p_glHint) { ((w32gl_PFN_glHint)p_glHint)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1]); }
        break;
    case GL_FN_ISBUFFER:
        if (p_glIsBuffer) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsBuffer)p_glIsBuffer)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_ISENABLED:
        if (p_glIsEnabled) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsEnabled)p_glIsEnabled)((w32gl_GLenum)a[0]); }
        break;
    case GL_FN_ISFRAMEBUFFER:
        if (p_glIsFramebuffer) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsFramebuffer)p_glIsFramebuffer)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_ISPROGRAM:
        if (p_glIsProgram) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsProgram)p_glIsProgram)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_ISRENDERBUFFER:
        if (p_glIsRenderbuffer) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsRenderbuffer)p_glIsRenderbuffer)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_ISSHADER:
        if (p_glIsShader) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsShader)p_glIsShader)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_ISTEXTURE:
        if (p_glIsTexture) { *ret = (int64_t)(uint32_t)((w32gl_PFN_glIsTexture)p_glIsTexture)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_LINEWIDTH:
        if (p_glLineWidth) { ((w32gl_PFN_glLineWidth)p_glLineWidth)(w32gl_arg_f(a[0])); }
        break;
    case GL_FN_LINKPROGRAM:
        if (p_glLinkProgram) { ((w32gl_PFN_glLinkProgram)p_glLinkProgram)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_PIXELSTOREI:
        if (p_glPixelStorei) { ((w32gl_PFN_glPixelStorei)p_glPixelStorei)((w32gl_GLenum)a[0], (w32gl_GLint)a[1]); }
        break;
    case GL_FN_POLYGONOFFSET:
        if (p_glPolygonOffset) { ((w32gl_PFN_glPolygonOffset)p_glPolygonOffset)(w32gl_arg_f(a[0]), w32gl_arg_f(a[1])); }
        break;
    case GL_FN_READPIXELS:
        if (p_glReadPixels) { ((w32gl_PFN_glReadPixels)p_glReadPixels)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei)a[3], (w32gl_GLenum)a[4], (w32gl_GLenum)a[5], (w32gl_void*)(uintptr_t)a[6]); }
        break;
    case GL_FN_RELEASESHADERCOMPILER:
        if (p_glReleaseShaderCompiler) { ((w32gl_PFN_glReleaseShaderCompiler)p_glReleaseShaderCompiler)(); }
        break;
    case GL_FN_RENDERBUFFERSTORAGE:
        if (p_glRenderbufferStorage) { ((w32gl_PFN_glRenderbufferStorage)p_glRenderbufferStorage)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei)a[3]); }
        break;
    case GL_FN_SAMPLECOVERAGE:
        if (p_glSampleCoverage) { ((w32gl_PFN_glSampleCoverage)p_glSampleCoverage)(w32gl_arg_f(a[0]), (w32gl_GLboolean)a[1]); }
        break;
    case GL_FN_SCISSOR:
        if (p_glScissor) { ((w32gl_PFN_glScissor)p_glScissor)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei)a[3]); }
        break;
    case GL_FN_SHADERBINARY:
        if (p_glShaderBinary) { ((w32gl_PFN_glShaderBinary)p_glShaderBinary)((w32gl_GLsizei)a[0], (const w32gl_GLuint*)(uintptr_t)a[1], (w32gl_GLenum)a[2], (const w32gl_void*)(uintptr_t)a[3], (w32gl_GLsizei)a[4]); }
        break;
    case GL_FN_SHADERSOURCE:
        if (p_glShaderSource) { ((w32gl_PFN_glShaderSource)p_glShaderSource)((w32gl_GLuint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLchar**)(uintptr_t)a[2], (const w32gl_GLint*)(uintptr_t)a[3]); }
        break;
    case GL_FN_STENCILFUNC:
        if (p_glStencilFunc) { ((w32gl_PFN_glStencilFunc)p_glStencilFunc)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLuint)a[2]); }
        break;
    case GL_FN_STENCILFUNCSEPARATE:
        if (p_glStencilFuncSeparate) { ((w32gl_PFN_glStencilFuncSeparate)p_glStencilFuncSeparate)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint)a[2], (w32gl_GLuint)a[3]); }
        break;
    case GL_FN_STENCILMASK:
        if (p_glStencilMask) { ((w32gl_PFN_glStencilMask)p_glStencilMask)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_STENCILMASKSEPARATE:
        if (p_glStencilMaskSeparate) { ((w32gl_PFN_glStencilMaskSeparate)p_glStencilMaskSeparate)((w32gl_GLenum)a[0], (w32gl_GLuint)a[1]); }
        break;
    case GL_FN_STENCILOP:
        if (p_glStencilOp) { ((w32gl_PFN_glStencilOp)p_glStencilOp)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2]); }
        break;
    case GL_FN_STENCILOPSEPARATE:
        if (p_glStencilOpSeparate) { ((w32gl_PFN_glStencilOpSeparate)p_glStencilOpSeparate)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLenum)a[2], (w32gl_GLenum)a[3]); }
        break;
    case GL_FN_TEXIMAGE2D:
        if (p_glTexImage2D) { ((w32gl_PFN_glTexImage2D)p_glTexImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLsizei)a[3], (w32gl_GLsizei)a[4], (w32gl_GLint)a[5], (w32gl_GLenum)a[6], (w32gl_GLenum)a[7], (const w32gl_void*)(uintptr_t)a[8]); }
        break;
    case GL_FN_TEXPARAMETERF:
        if (p_glTexParameterf) { ((w32gl_PFN_glTexParameterf)p_glTexParameterf)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], w32gl_arg_f(a[2])); }
        break;
    case GL_FN_TEXPARAMETERFV:
        if (p_glTexParameterfv) { ((w32gl_PFN_glTexParameterfv)p_glTexParameterfv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (const w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_TEXPARAMETERI:
        if (p_glTexParameteri) { ((w32gl_PFN_glTexParameteri)p_glTexParameteri)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (w32gl_GLint)a[2]); }
        break;
    case GL_FN_TEXPARAMETERIV:
        if (p_glTexParameteriv) { ((w32gl_PFN_glTexParameteriv)p_glTexParameteriv)((w32gl_GLenum)a[0], (w32gl_GLenum)a[1], (const w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_TEXSUBIMAGE2D:
        if (p_glTexSubImage2D) { ((w32gl_PFN_glTexSubImage2D)p_glTexSubImage2D)((w32gl_GLenum)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLint)a[3], (w32gl_GLsizei)a[4], (w32gl_GLsizei)a[5], (w32gl_GLenum)a[6], (w32gl_GLenum)a[7], (const w32gl_void*)(uintptr_t)a[8]); }
        break;
    case GL_FN_UNIFORM1F:
        if (p_glUniform1f) { ((w32gl_PFN_glUniform1f)p_glUniform1f)((w32gl_GLint)a[0], w32gl_arg_f(a[1])); }
        break;
    case GL_FN_UNIFORM1FV:
        if (p_glUniform1fv) { ((w32gl_PFN_glUniform1fv)p_glUniform1fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM1I:
        if (p_glUniform1i) { ((w32gl_PFN_glUniform1i)p_glUniform1i)((w32gl_GLint)a[0], (w32gl_GLint)a[1]); }
        break;
    case GL_FN_UNIFORM1IV:
        if (p_glUniform1iv) { ((w32gl_PFN_glUniform1iv)p_glUniform1iv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM2F:
        if (p_glUniform2f) { ((w32gl_PFN_glUniform2f)p_glUniform2f)((w32gl_GLint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2])); }
        break;
    case GL_FN_UNIFORM2FV:
        if (p_glUniform2fv) { ((w32gl_PFN_glUniform2fv)p_glUniform2fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM2I:
        if (p_glUniform2i) { ((w32gl_PFN_glUniform2i)p_glUniform2i)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2]); }
        break;
    case GL_FN_UNIFORM2IV:
        if (p_glUniform2iv) { ((w32gl_PFN_glUniform2iv)p_glUniform2iv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM3F:
        if (p_glUniform3f) { ((w32gl_PFN_glUniform3f)p_glUniform3f)((w32gl_GLint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3])); }
        break;
    case GL_FN_UNIFORM3FV:
        if (p_glUniform3fv) { ((w32gl_PFN_glUniform3fv)p_glUniform3fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM3I:
        if (p_glUniform3i) { ((w32gl_PFN_glUniform3i)p_glUniform3i)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLint)a[3]); }
        break;
    case GL_FN_UNIFORM3IV:
        if (p_glUniform3iv) { ((w32gl_PFN_glUniform3iv)p_glUniform3iv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM4F:
        if (p_glUniform4f) { ((w32gl_PFN_glUniform4f)p_glUniform4f)((w32gl_GLint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3]), w32gl_arg_f(a[4])); }
        break;
    case GL_FN_UNIFORM4FV:
        if (p_glUniform4fv) { ((w32gl_PFN_glUniform4fv)p_glUniform4fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLfloat*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORM4I:
        if (p_glUniform4i) { ((w32gl_PFN_glUniform4i)p_glUniform4i)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLint)a[2], (w32gl_GLint)a[3], (w32gl_GLint)a[4]); }
        break;
    case GL_FN_UNIFORM4IV:
        if (p_glUniform4iv) { ((w32gl_PFN_glUniform4iv)p_glUniform4iv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (const w32gl_GLint*)(uintptr_t)a[2]); }
        break;
    case GL_FN_UNIFORMMATRIX2FV:
        if (p_glUniformMatrix2fv) { ((w32gl_PFN_glUniformMatrix2fv)p_glUniformMatrix2fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLboolean)a[2], (const w32gl_GLfloat*)(uintptr_t)a[3]); }
        break;
    case GL_FN_UNIFORMMATRIX3FV:
        if (p_glUniformMatrix3fv) { ((w32gl_PFN_glUniformMatrix3fv)p_glUniformMatrix3fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLboolean)a[2], (const w32gl_GLfloat*)(uintptr_t)a[3]); }
        break;
    case GL_FN_UNIFORMMATRIX4FV:
        if (p_glUniformMatrix4fv) { ((w32gl_PFN_glUniformMatrix4fv)p_glUniformMatrix4fv)((w32gl_GLint)a[0], (w32gl_GLsizei)a[1], (w32gl_GLboolean)a[2], (const w32gl_GLfloat*)(uintptr_t)a[3]); }
        break;
    case GL_FN_USEPROGRAM:
        if (p_glUseProgram) { ((w32gl_PFN_glUseProgram)p_glUseProgram)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_VALIDATEPROGRAM:
        if (p_glValidateProgram) { ((w32gl_PFN_glValidateProgram)p_glValidateProgram)((w32gl_GLuint)a[0]); }
        break;
    case GL_FN_VERTEXATTRIB1F:
        if (p_glVertexAttrib1f) { ((w32gl_PFN_glVertexAttrib1f)p_glVertexAttrib1f)((w32gl_GLuint)a[0], w32gl_arg_f(a[1])); }
        break;
    case GL_FN_VERTEXATTRIB1FV:
        if (p_glVertexAttrib1fv) { ((w32gl_PFN_glVertexAttrib1fv)p_glVertexAttrib1fv)((w32gl_GLuint)a[0], (const w32gl_GLfloat*)(uintptr_t)a[1]); }
        break;
    case GL_FN_VERTEXATTRIB2F:
        if (p_glVertexAttrib2f) { ((w32gl_PFN_glVertexAttrib2f)p_glVertexAttrib2f)((w32gl_GLuint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2])); }
        break;
    case GL_FN_VERTEXATTRIB2FV:
        if (p_glVertexAttrib2fv) { ((w32gl_PFN_glVertexAttrib2fv)p_glVertexAttrib2fv)((w32gl_GLuint)a[0], (const w32gl_GLfloat*)(uintptr_t)a[1]); }
        break;
    case GL_FN_VERTEXATTRIB3F:
        if (p_glVertexAttrib3f) { ((w32gl_PFN_glVertexAttrib3f)p_glVertexAttrib3f)((w32gl_GLuint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3])); }
        break;
    case GL_FN_VERTEXATTRIB3FV:
        if (p_glVertexAttrib3fv) { ((w32gl_PFN_glVertexAttrib3fv)p_glVertexAttrib3fv)((w32gl_GLuint)a[0], (const w32gl_GLfloat*)(uintptr_t)a[1]); }
        break;
    case GL_FN_VERTEXATTRIB4F:
        if (p_glVertexAttrib4f) { ((w32gl_PFN_glVertexAttrib4f)p_glVertexAttrib4f)((w32gl_GLuint)a[0], w32gl_arg_f(a[1]), w32gl_arg_f(a[2]), w32gl_arg_f(a[3]), w32gl_arg_f(a[4])); }
        break;
    case GL_FN_VERTEXATTRIB4FV:
        if (p_glVertexAttrib4fv) { ((w32gl_PFN_glVertexAttrib4fv)p_glVertexAttrib4fv)((w32gl_GLuint)a[0], (const w32gl_GLfloat*)(uintptr_t)a[1]); }
        break;
    case GL_FN_VERTEXATTRIBPOINTER:
        if (p_glVertexAttribPointer) { ((w32gl_PFN_glVertexAttribPointer)p_glVertexAttribPointer)((w32gl_GLuint)a[0], (w32gl_GLint)a[1], (w32gl_GLenum)a[2], (w32gl_GLboolean)a[3], (w32gl_GLsizei)a[4], (const w32gl_void*)(uintptr_t)a[5]); }
        break;
    case GL_FN_VIEWPORT:
        if (p_glViewport) { ((w32gl_PFN_glViewport)p_glViewport)((w32gl_GLint)a[0], (w32gl_GLint)a[1], (w32gl_GLsizei)a[2], (w32gl_GLsizei)a[3]); }
        break;
    default:
        break;
    }
}
