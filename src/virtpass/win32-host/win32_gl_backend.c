#include "win32_gl_backend.h"
#include "virtpass/vp_gl_host_entries.h" /* p_* storage (generated, shared with android) */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE g_h_egl  = NULL;
static HMODULE g_h_gles = NULL;
static bool   g_loaded  = false;
static int    g_refcnt  = 0;


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

static inline float vpgl_arg_f(int64_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* ---------------------------------------------------------------------------
 * Backend DLL discovery
 *
 * libEGL.dll / libGLESv2.dll ship with the Android SDK emulator, one
 * directory per backend:
 *   <sdk>\emulator\lib64\gles_angle\{libEGL,libGLESv2,d3dcompiler_47}.dll
 *   <sdk>\emulator\lib64\gles_swiftshader\...
 * <sdk> comes from RVVM_GL_DLL_DIR (explicit, wins), then ANDROID_SDK_ROOT /
 * ANDROID_HOME, then the usual per-user install location.
 * --------------------------------------------------------------------------- */

static char g_dll_dir[MAX_PATH];
static char g_backend[32];

/* Load the pair from `dir`. Full paths keep a stray libEGL.dll next to the
 * executable from shadowing the backend, and SetDllDirectoryA keeps the
 * directory on the search path while ANGLE pulls in d3dcompiler_47.dll. */
static bool gl_load_pair(const char* dir)
{
    char egl_path[MAX_PATH], gles_path[MAX_PATH];
    const char* egl_name  = "libEGL.dll";
    const char* gles_name = "libGLESv2.dll";

    if (dir && *dir) {
        snprintf(egl_path, sizeof(egl_path), "%s\\%s", dir, egl_name);
        snprintf(gles_path, sizeof(gles_path), "%s\\%s", dir, gles_name);
        egl_name = egl_path;
        gles_name = gles_path;
        SetDllDirectoryA(dir);
    }

    g_h_egl  = LoadLibraryA(egl_name);
    g_h_gles = LoadLibraryA(gles_name);
    SetDllDirectoryA(NULL);

    if (g_h_egl && g_h_gles) return true;
    if (g_h_egl)  { FreeLibrary(g_h_egl);  g_h_egl = NULL; }
    if (g_h_gles) { FreeLibrary(g_h_gles); g_h_gles = NULL; }
    return false;
}

/* Try <root>\emulator\lib64\gles_<name>; skip roots that do not exist. */
static bool gl_try_backend_dir(const char* root, const char* name)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\emulator\\lib64\\gles_%s", root, name);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) return false;
    if (!gl_load_pair(dir)) {
        gl_log("GL backend probe failed: %s", dir);
        return false;
    }
    snprintf(g_dll_dir, sizeof(g_dll_dir), "%s", dir);
    snprintf(g_backend, sizeof(g_backend), "%s", name);
    gl_log("GL backend dir: %s", dir);
    return true;
}

/* Last resort: any gles_* sibling that actually loads (covers emulator
 * releases that add gles_angle9 / gles_angle11 / ...). */
static bool gl_scan_backend_dirs(const char* root)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\emulator\\lib64\\gles_*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    char dir[MAX_PATH];
    bool hit = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        snprintf(dir, sizeof(dir), "%s\\emulator\\lib64\\%s", root, fd.cFileName);
        if (!gl_load_pair(dir)) continue;
        snprintf(g_dll_dir, sizeof(g_dll_dir), "%s", dir);
        snprintf(g_backend, sizeof(g_backend), "%s", fd.cFileName + 5);
        gl_log("GL backend dir: %s", dir);
        hit = true;
        break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return hit;
}

static bool gl_try_sdk_root(const char* root, const char* prefer, const char* other)
{
    if (!root || !*root) return false;
    return gl_try_backend_dir(root, prefer) ||
           gl_try_backend_dir(root, other) ||
           gl_scan_backend_dirs(root);
}

bool win32_gl_backend_load(void)
{
    if (g_loaded) { g_refcnt++; return true; }

    const char* backend = getenv("RVVM_GL_BACKEND");
    if (backend && strcmp(backend, "off") == 0) {
        gl_log("GL backend disabled (RVVM_GL_BACKEND=off)");
        return false;
    }

    /* RVVM_GL_BACKEND names the preferred DLL set; the other one is the
     * fallback (ANGLE needs D3D, SwiftShader is pure CPU). */
    const char* prefer = (backend && strcmp(backend, "swiftshader") == 0)
                             ? "swiftshader" : "angle";
    const char* other  = strcmp(prefer, "angle") == 0 ? "swiftshader" : "angle";

    /* 1. Explicit directory wins. */
    const char* dir = getenv("RVVM_GL_DLL_DIR");
    if (dir && *dir) {
        gl_log("GL backend dir: %s", dir);
        if (gl_load_pair(dir)) {
            snprintf(g_dll_dir, sizeof(g_dll_dir), "%s", dir);
            snprintf(g_backend, sizeof(g_backend), "%s", prefer);
        } else {
            gl_log("GL backend load failed (%s): egl=%p gles=%p",
                   prefer, (void*)g_h_egl, (void*)g_h_gles);
        }
    }

    /* 2. Android SDK emulator tree (libEGL.dll + libGLESv2.dll per backend). */
    if (!g_h_egl || !g_h_gles) {
        char local_sdk[MAX_PATH], profile_sdk[MAX_PATH];
        const char* roots[4];
        int nroots = 0;
        const char* env;

        if ((env = getenv("ANDROID_SDK_ROOT")) && *env) roots[nroots++] = env;
        if ((env = getenv("ANDROID_HOME")) && *env) roots[nroots++] = env;
        if ((env = getenv("LOCALAPPDATA")) && *env) {
            snprintf(local_sdk, sizeof(local_sdk), "%s\\Android\\Sdk", env);
            roots[nroots++] = local_sdk;
        }
        if ((env = getenv("USERPROFILE")) && *env) {
            snprintf(profile_sdk, sizeof(profile_sdk),
                     "%s\\AppData\\Local\\Android\\Sdk", env);
            roots[nroots++] = profile_sdk;
        }

        for (int i = 0; i < nroots; i++) {
            if (gl_try_sdk_root(roots[i], prefer, other)) break;
        }
    }

    if (!g_h_egl || !g_h_gles) {
        gl_log("GL backend load failed (%s): egl=%p gles=%p",
               prefer, (void*)g_h_egl, (void*)g_h_gles);
        if (g_h_egl)  { FreeLibrary(g_h_egl);  g_h_egl = NULL; }
        if (g_h_gles) { FreeLibrary(g_h_gles); g_h_gles = NULL; }
        return false;
    }

    /* GetProcAddress returns a generic FARPROC: cast through void* so the
     * function-pointer-to-function-pointer conversion is out of -Wcast-function-type
     * (same idiom as posix_shim.c / win32_cmdpost_bridge.c). */
    #define LOAD(name) p_egl##name = (vpgl_PFN_egl##name)(void*)GetProcAddress(g_h_egl, "egl" #name)
    LOAD(ChooseConfig);
    LOAD(CreateContext);
    LOAD(CreatePbufferSurface);
    LOAD(CreateWindowSurface);
    LOAD(DestroyContext);
    LOAD(DestroySurface);
    LOAD(GetConfigAttrib);
    LOAD(GetDisplay);
    LOAD(GetError);
    /* eglGetProcAddress is not loaded: the guest stub answers it from its own
     * entry points, so no host function address ever reaches the guest. */
    LOAD(Initialize);
    LOAD(MakeCurrent);
    LOAD(QueryString);
    LOAD(QuerySurface);
    LOAD(SwapBuffers);
    LOAD(Terminate);
    #undef LOAD

    #define LOAD(name) p_gl##name = (vpgl_PFN_gl##name)(void*)GetProcAddress(g_h_gles, "gl" #name)
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
    gl_log("GL backend loaded: %s (%s)", g_backend,
           g_dll_dir[0] ? g_dll_dir : "(default)");
    return true;
}

bool win32_gl_backend_ready(void)
{
    return g_loaded && g_refcnt > 0;
}

const char* win32_gl_backend_name(void)
{
    const char* backend = getenv("RVVM_GL_BACKEND");
    if (backend && strcmp(backend, "off") == 0) return "off";
    return g_loaded ? g_backend : "";
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
