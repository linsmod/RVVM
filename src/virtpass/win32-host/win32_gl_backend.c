#include "win32_gl_backend.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE g_h_egl  = NULL;
static HMODULE g_h_gles = NULL;
static bool   g_loaded  = false;
static int    g_refcnt  = 0;

/* p_* entry points — defined here (header declares them extern). */
w32gl_PFN_eglChooseConfig p_eglChooseConfig;
w32gl_PFN_eglCreateContext p_eglCreateContext;
w32gl_PFN_eglCreatePbufferSurface p_eglCreatePbufferSurface;
w32gl_PFN_eglCreateWindowSurface p_eglCreateWindowSurface;
w32gl_PFN_eglDestroyContext p_eglDestroyContext;
w32gl_PFN_eglDestroySurface p_eglDestroySurface;
w32gl_PFN_eglGetConfigAttrib p_eglGetConfigAttrib;
w32gl_PFN_eglGetDisplay p_eglGetDisplay;
w32gl_PFN_eglGetError p_eglGetError;
w32gl_PFN_eglGetProcAddress p_eglGetProcAddress;
w32gl_PFN_eglInitialize p_eglInitialize;
w32gl_PFN_eglMakeCurrent p_eglMakeCurrent;
w32gl_PFN_eglQueryString p_eglQueryString;
w32gl_PFN_eglQuerySurface p_eglQuerySurface;
w32gl_PFN_eglSwapBuffers p_eglSwapBuffers;
w32gl_PFN_eglTerminate p_eglTerminate;

w32gl_PFN_glActiveTexture p_glActiveTexture;
w32gl_PFN_glAttachShader p_glAttachShader;
w32gl_PFN_glBindAttribLocation p_glBindAttribLocation;
w32gl_PFN_glBindBuffer p_glBindBuffer;
w32gl_PFN_glBindFramebuffer p_glBindFramebuffer;
w32gl_PFN_glBindRenderbuffer p_glBindRenderbuffer;
w32gl_PFN_glBindTexture p_glBindTexture;
w32gl_PFN_glBlendColor p_glBlendColor;
w32gl_PFN_glBlendEquation p_glBlendEquation;
w32gl_PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
w32gl_PFN_glBlendFunc p_glBlendFunc;
w32gl_PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
w32gl_PFN_glBufferData p_glBufferData;
w32gl_PFN_glBufferSubData p_glBufferSubData;
w32gl_PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
w32gl_PFN_glClear p_glClear;
w32gl_PFN_glClearColor p_glClearColor;
w32gl_PFN_glClearDepthf p_glClearDepthf;
w32gl_PFN_glClearStencil p_glClearStencil;
w32gl_PFN_glColorMask p_glColorMask;
w32gl_PFN_glCompileShader p_glCompileShader;
w32gl_PFN_glCompressedTexImage2D p_glCompressedTexImage2D;
w32gl_PFN_glCompressedTexSubImage2D p_glCompressedTexSubImage2D;
w32gl_PFN_glCopyTexImage2D p_glCopyTexImage2D;
w32gl_PFN_glCopyTexSubImage2D p_glCopyTexSubImage2D;
w32gl_PFN_glCreateProgram p_glCreateProgram;
w32gl_PFN_glCreateShader p_glCreateShader;
w32gl_PFN_glCullFace p_glCullFace;
w32gl_PFN_glDeleteBuffers p_glDeleteBuffers;
w32gl_PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
w32gl_PFN_glDeleteProgram p_glDeleteProgram;
w32gl_PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
w32gl_PFN_glDeleteShader p_glDeleteShader;
w32gl_PFN_glDeleteTextures p_glDeleteTextures;
w32gl_PFN_glDepthFunc p_glDepthFunc;
w32gl_PFN_glDepthMask p_glDepthMask;
w32gl_PFN_glDepthRangef p_glDepthRangef;
w32gl_PFN_glDetachShader p_glDetachShader;
w32gl_PFN_glDisable p_glDisable;
w32gl_PFN_glDisableVertexAttribArray p_glDisableVertexAttribArray;
w32gl_PFN_glDrawArrays p_glDrawArrays;
w32gl_PFN_glDrawElements p_glDrawElements;
w32gl_PFN_glEnable p_glEnable;
w32gl_PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
w32gl_PFN_glFinish p_glFinish;
w32gl_PFN_glFlush p_glFlush;
w32gl_PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;
w32gl_PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
w32gl_PFN_glFrontFace p_glFrontFace;
w32gl_PFN_glGenBuffers p_glGenBuffers;
w32gl_PFN_glGenerateMipmap p_glGenerateMipmap;
w32gl_PFN_glGenFramebuffers p_glGenFramebuffers;
w32gl_PFN_glGenRenderbuffers p_glGenRenderbuffers;
w32gl_PFN_glGenTextures p_glGenTextures;
w32gl_PFN_glGetActiveAttrib p_glGetActiveAttrib;
w32gl_PFN_glGetActiveUniform p_glGetActiveUniform;
w32gl_PFN_glGetAttachedShaders p_glGetAttachedShaders;
w32gl_PFN_glGetAttribLocation p_glGetAttribLocation;
w32gl_PFN_glGetBooleanv p_glGetBooleanv;
w32gl_PFN_glGetBufferParameteriv p_glGetBufferParameteriv;
w32gl_PFN_glGetError p_glGetError;
w32gl_PFN_glGetFloatv p_glGetFloatv;
w32gl_PFN_glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv;
w32gl_PFN_glGetIntegerv p_glGetIntegerv;
w32gl_PFN_glGetProgramiv p_glGetProgramiv;
w32gl_PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
w32gl_PFN_glGetRenderbufferParameteriv p_glGetRenderbufferParameteriv;
w32gl_PFN_glGetShaderiv p_glGetShaderiv;
w32gl_PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
w32gl_PFN_glGetShaderPrecisionFormat p_glGetShaderPrecisionFormat;
w32gl_PFN_glGetShaderSource p_glGetShaderSource;
w32gl_PFN_glGetString p_glGetString;
w32gl_PFN_glGetTexParameterfv p_glGetTexParameterfv;
w32gl_PFN_glGetTexParameteriv p_glGetTexParameteriv;
w32gl_PFN_glGetUniformfv p_glGetUniformfv;
w32gl_PFN_glGetUniformiv p_glGetUniformiv;
w32gl_PFN_glGetUniformLocation p_glGetUniformLocation;
w32gl_PFN_glGetVertexAttribfv p_glGetVertexAttribfv;
w32gl_PFN_glGetVertexAttribiv p_glGetVertexAttribiv;
w32gl_PFN_glGetVertexAttribPointerv p_glGetVertexAttribPointerv;
w32gl_PFN_glHint p_glHint;
w32gl_PFN_glIsBuffer p_glIsBuffer;
w32gl_PFN_glIsEnabled p_glIsEnabled;
w32gl_PFN_glIsFramebuffer p_glIsFramebuffer;
w32gl_PFN_glIsProgram p_glIsProgram;
w32gl_PFN_glIsRenderbuffer p_glIsRenderbuffer;
w32gl_PFN_glIsShader p_glIsShader;
w32gl_PFN_glIsTexture p_glIsTexture;
w32gl_PFN_glLineWidth p_glLineWidth;
w32gl_PFN_glLinkProgram p_glLinkProgram;
w32gl_PFN_glPixelStorei p_glPixelStorei;
w32gl_PFN_glPolygonOffset p_glPolygonOffset;
w32gl_PFN_glReadPixels p_glReadPixels;
w32gl_PFN_glReleaseShaderCompiler p_glReleaseShaderCompiler;
w32gl_PFN_glRenderbufferStorage p_glRenderbufferStorage;
w32gl_PFN_glSampleCoverage p_glSampleCoverage;
w32gl_PFN_glScissor p_glScissor;
w32gl_PFN_glShaderBinary p_glShaderBinary;
w32gl_PFN_glShaderSource p_glShaderSource;
w32gl_PFN_glStencilFunc p_glStencilFunc;
w32gl_PFN_glStencilFuncSeparate p_glStencilFuncSeparate;
w32gl_PFN_glStencilMask p_glStencilMask;
w32gl_PFN_glStencilMaskSeparate p_glStencilMaskSeparate;
w32gl_PFN_glStencilOp p_glStencilOp;
w32gl_PFN_glStencilOpSeparate p_glStencilOpSeparate;
w32gl_PFN_glTexImage2D p_glTexImage2D;
w32gl_PFN_glTexParameterf p_glTexParameterf;
w32gl_PFN_glTexParameterfv p_glTexParameterfv;
w32gl_PFN_glTexParameteri p_glTexParameteri;
w32gl_PFN_glTexParameteriv p_glTexParameteriv;
w32gl_PFN_glTexSubImage2D p_glTexSubImage2D;
w32gl_PFN_glUniform1f p_glUniform1f;
w32gl_PFN_glUniform1fv p_glUniform1fv;
w32gl_PFN_glUniform1i p_glUniform1i;
w32gl_PFN_glUniform1iv p_glUniform1iv;
w32gl_PFN_glUniform2f p_glUniform2f;
w32gl_PFN_glUniform2fv p_glUniform2fv;
w32gl_PFN_glUniform2i p_glUniform2i;
w32gl_PFN_glUniform2iv p_glUniform2iv;
w32gl_PFN_glUniform3f p_glUniform3f;
w32gl_PFN_glUniform3fv p_glUniform3fv;
w32gl_PFN_glUniform3i p_glUniform3i;
w32gl_PFN_glUniform3iv p_glUniform3iv;
w32gl_PFN_glUniform4f p_glUniform4f;
w32gl_PFN_glUniform4fv p_glUniform4fv;
w32gl_PFN_glUniform4i p_glUniform4i;
w32gl_PFN_glUniform4iv p_glUniform4iv;
w32gl_PFN_glUniformMatrix2fv p_glUniformMatrix2fv;
w32gl_PFN_glUniformMatrix3fv p_glUniformMatrix3fv;
w32gl_PFN_glUniformMatrix4fv p_glUniformMatrix4fv;
w32gl_PFN_glUseProgram p_glUseProgram;
w32gl_PFN_glValidateProgram p_glValidateProgram;
w32gl_PFN_glVertexAttrib1f p_glVertexAttrib1f;
w32gl_PFN_glVertexAttrib1fv p_glVertexAttrib1fv;
w32gl_PFN_glVertexAttrib2f p_glVertexAttrib2f;
w32gl_PFN_glVertexAttrib2fv p_glVertexAttrib2fv;
w32gl_PFN_glVertexAttrib3f p_glVertexAttrib3f;
w32gl_PFN_glVertexAttrib3fv p_glVertexAttrib3fv;
w32gl_PFN_glVertexAttrib4f p_glVertexAttrib4f;
w32gl_PFN_glVertexAttrib4fv p_glVertexAttrib4fv;
w32gl_PFN_glVertexAttribPointer p_glVertexAttribPointer;
w32gl_PFN_glViewport p_glViewport;

static const char* gl_backend_dir(void)
{
    const char* dir = getenv("RVVM_GL_DLL_DIR");
    if (dir && *dir) return dir;
    return NULL;
}

static void gl_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[winhost %10llu ms] ", (unsigned long long)GetTickCount64());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static inline float w32gl_arg_f(int64_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

bool win32_gl_backend_load(void)
{
    if (g_loaded) { g_refcnt++; return true; }

    const char* dir = gl_backend_dir();
    gl_log("GL backend dir: %s", dir ? dir : "(guess)");

    const char* backend = getenv("RVVM_GL_BACKEND");
    const char* gl_type = backend && strcmp(backend, "off") == 0 ? "off" :
                          backend && strcmp(backend, "swiftshader") == 0 ? "swiftshader" : "angle";

    if (strcmp(gl_type, "off") == 0) {
        gl_log("GL backend disabled (RVVM_GL_BACKEND=off)");
        return false;
    }

    if (dir) {
        SetDllDirectoryA(dir);
    }

    g_h_egl  = LoadLibraryA("libEGL.dll");
    g_h_gles = LoadLibraryA("libGLESv2.dll");
    SetDllDirectoryA(NULL);

    if (!g_h_egl || !g_h_gles) {
        gl_log("GL backend load failed (%s): egl=%p gles=%p",
               gl_type, (void*)g_h_egl, (void*)g_h_gles);
        if (g_h_egl) { FreeLibrary(g_h_egl); g_h_egl = NULL; }
        if (g_h_gles) { FreeLibrary(g_h_gles); g_h_gles = NULL; }
        const char* sdk = getenv("ANDROID_SDK_ROOT");
        if (!sdk) sdk = getenv("ANDROID_HOME");
        if (sdk) {
            const char* fallback = strcmp(gl_type, "angle") == 0 ? "swiftshader" : "angle";
            char fpath[512];
            snprintf(fpath, sizeof(fpath), "%s\\emulator\\lib64\\gles_%s", sdk, fallback);
            gl_log("Trying fallback backend: %s", fpath);
            SetDllDirectoryA(fpath);
            g_h_egl  = LoadLibraryA("libEGL.dll");
            g_h_gles = LoadLibraryA("libGLESv2.dll");
            SetDllDirectoryA(NULL);
            if (g_h_egl && g_h_gles) goto loaded;
            if (g_h_egl) { FreeLibrary(g_h_egl); g_h_egl = NULL; }
            if (g_h_gles) { FreeLibrary(g_h_gles); g_h_gles = NULL; }
        }
        return false;
    }

loaded:
    #define LOAD(name) p_egl##name = (w32gl_PFN_egl##name)GetProcAddress(g_h_egl, "egl" #name)
    LOAD(ChooseConfig);
    LOAD(CreateContext);
    LOAD(CreatePbufferSurface);
    LOAD(CreateWindowSurface);
    LOAD(DestroyContext);
    LOAD(DestroySurface);
    LOAD(GetConfigAttrib);
    LOAD(GetDisplay);
    LOAD(GetError);
    LOAD(GetProcAddress);
    LOAD(Initialize);
    LOAD(MakeCurrent);
    LOAD(QueryString);
    LOAD(QuerySurface);
    LOAD(SwapBuffers);
    LOAD(Terminate);
    #undef LOAD

    #define LOAD(name) p_gl##name = (w32gl_PFN_gl##name)GetProcAddress(g_h_gles, "gl" #name)
    LOAD(ActiveTexture);
    LOAD(AttachShader);
    LOAD(BindAttribLocation);
    LOAD(BindBuffer);
    LOAD(BindFramebuffer);
    LOAD(BindRenderbuffer);
    LOAD(BindTexture);
    LOAD(BlendColor);
    LOAD(BlendEquation);
    LOAD(BlendEquationSeparate);
    LOAD(BlendFunc);
    LOAD(BlendFuncSeparate);
    LOAD(BufferData);
    LOAD(BufferSubData);
    LOAD(CheckFramebufferStatus);
    LOAD(Clear);
    LOAD(ClearColor);
    LOAD(ClearDepthf);
    LOAD(ClearStencil);
    LOAD(ColorMask);
    LOAD(CompileShader);
    LOAD(CompressedTexImage2D);
    LOAD(CompressedTexSubImage2D);
    LOAD(CopyTexImage2D);
    LOAD(CopyTexSubImage2D);
    LOAD(CreateProgram);
    LOAD(CreateShader);
    LOAD(CullFace);
    LOAD(DeleteBuffers);
    LOAD(DeleteFramebuffers);
    LOAD(DeleteProgram);
    LOAD(DeleteRenderbuffers);
    LOAD(DeleteShader);
    LOAD(DeleteTextures);
    LOAD(DepthFunc);
    LOAD(DepthMask);
    LOAD(DepthRangef);
    LOAD(DetachShader);
    LOAD(Disable);
    LOAD(DisableVertexAttribArray);
    LOAD(DrawArrays);
    LOAD(DrawElements);
    LOAD(Enable);
    LOAD(EnableVertexAttribArray);
    LOAD(Finish);
    LOAD(Flush);
    LOAD(FramebufferRenderbuffer);
    LOAD(FramebufferTexture2D);
    LOAD(FrontFace);
    LOAD(GenBuffers);
    LOAD(GenerateMipmap);
    LOAD(GenFramebuffers);
    LOAD(GenRenderbuffers);
    LOAD(GenTextures);
    LOAD(GetActiveAttrib);
    LOAD(GetActiveUniform);
    LOAD(GetAttachedShaders);
    LOAD(GetAttribLocation);
    LOAD(GetBooleanv);
    LOAD(GetBufferParameteriv);
    LOAD(GetError);
    LOAD(GetFloatv);
    LOAD(GetFramebufferAttachmentParameteriv);
    LOAD(GetIntegerv);
    LOAD(GetProgramiv);
    LOAD(GetProgramInfoLog);
    LOAD(GetRenderbufferParameteriv);
    LOAD(GetShaderiv);
    LOAD(GetShaderInfoLog);
    LOAD(GetShaderPrecisionFormat);
    LOAD(GetShaderSource);
    LOAD(GetString);
    LOAD(GetTexParameterfv);
    LOAD(GetTexParameteriv);
    LOAD(GetUniformfv);
    LOAD(GetUniformiv);
    LOAD(GetUniformLocation);
    LOAD(GetVertexAttribfv);
    LOAD(GetVertexAttribiv);
    LOAD(GetVertexAttribPointerv);
    LOAD(Hint);
    LOAD(IsBuffer);
    LOAD(IsEnabled);
    LOAD(IsFramebuffer);
    LOAD(IsProgram);
    LOAD(IsRenderbuffer);
    LOAD(IsShader);
    LOAD(IsTexture);
    LOAD(LineWidth);
    LOAD(LinkProgram);
    LOAD(PixelStorei);
    LOAD(PolygonOffset);
    LOAD(ReadPixels);
    LOAD(ReleaseShaderCompiler);
    LOAD(RenderbufferStorage);
    LOAD(SampleCoverage);
    LOAD(Scissor);
    LOAD(ShaderBinary);
    LOAD(ShaderSource);
    LOAD(StencilFunc);
    LOAD(StencilFuncSeparate);
    LOAD(StencilMask);
    LOAD(StencilMaskSeparate);
    LOAD(StencilOp);
    LOAD(StencilOpSeparate);
    LOAD(TexImage2D);
    LOAD(TexParameterf);
    LOAD(TexParameterfv);
    LOAD(TexParameteri);
    LOAD(TexParameteriv);
    LOAD(TexSubImage2D);
    LOAD(Uniform1f);
    LOAD(Uniform1fv);
    LOAD(Uniform1i);
    LOAD(Uniform1iv);
    LOAD(Uniform2f);
    LOAD(Uniform2fv);
    LOAD(Uniform2i);
    LOAD(Uniform2iv);
    LOAD(Uniform3f);
    LOAD(Uniform3fv);
    LOAD(Uniform3i);
    LOAD(Uniform3iv);
    LOAD(Uniform4f);
    LOAD(Uniform4fv);
    LOAD(Uniform4i);
    LOAD(Uniform4iv);
    LOAD(UniformMatrix2fv);
    LOAD(UniformMatrix3fv);
    LOAD(UniformMatrix4fv);
    LOAD(UseProgram);
    LOAD(ValidateProgram);
    LOAD(VertexAttrib1f);
    LOAD(VertexAttrib1fv);
    LOAD(VertexAttrib2f);
    LOAD(VertexAttrib2fv);
    LOAD(VertexAttrib3f);
    LOAD(VertexAttrib3fv);
    LOAD(VertexAttrib4f);
    LOAD(VertexAttrib4fv);
    LOAD(VertexAttribPointer);
    LOAD(Viewport);
    #undef LOAD

    if (!p_eglGetDisplay || !p_eglCreatePbufferSurface) {
        gl_log("GL backend: missing critical symbols");
        win32_gl_backend_unload();
        return false;
    }

    g_loaded = true;
    g_refcnt = 1;
    gl_log("GL backend loaded: %s (%s)",
           backend && strcmp(backend, "swiftshader") == 0 ? "swiftshader" : "angle",
           dir ? dir : "(default)");
    return true;
}

bool win32_gl_backend_ready(void)
{
    return g_loaded && g_refcnt > 0;
}

const char* win32_gl_backend_name(void)
{
    const char* backend = getenv("RVVM_GL_BACKEND");
    if (backend && strcmp(backend, "swiftshader") == 0) return "swiftshader";
    if (backend && strcmp(backend, "off") == 0) return "off";
    return g_loaded ? "angle" : "";
}

void win32_gl_backend_unload(void)
{
    if (!g_loaded) return;
    g_refcnt--;
    if (g_refcnt > 0) return;
    if (g_h_gles) { FreeLibrary(g_h_gles); g_h_gles = NULL; }
    if (g_h_egl)  { FreeLibrary(g_h_egl);  g_h_egl = NULL; }
    g_loaded = false;
    g_refcnt = 0;
    gl_log("GL backend unloaded");
}
