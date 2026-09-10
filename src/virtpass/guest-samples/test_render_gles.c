/*
 * test_render_gles.c - GL ES smoke test: clear screen to red
 *
 * Builds a minimal GL pipeline: eglGetDisplay -> init -> pbuffer ->
 * glClearColor(1,0,0,1) -> glClear -> eglSwapBuffers.
 * Host expects a red frame (9.1 smoke test).
 *
 * Build:
 *   riscv64-linux-android35-clang -static -o test_render_gles.exe \
 *       test_render_gles.c -L../vp_ndk_stub -landroid_stubs -lgles_stubs
 * Run:
 *   rvvm_user test_render_gles.exe
 */

#include <stdio.h>
#include "virtpass/vp_gl.h"
#include "virtpass/vp_android.h"

int main(void)
{
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    EGLint major, minor, num_config;
    EGLConfig config;
    const EGLint attrib_list[] = {
        EGL_WIDTH,  640,
        EGL_HEIGHT, 480,
        EGL_NONE
    };
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    const EGLint choose_attribs[] = {
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    int has_gl = 1;

    printf("=== GL ES Red Frame Smoke Test ===\n");

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        printf("eglGetDisplay failed: %s\n", eglGetError() == EGL_SUCCESS ? "EGL_SUCCESS" : "unknown");
        has_gl = 0;
    }

    if (has_gl && !eglInitialize(dpy, &major, &minor)) {
        printf("eglInitialize failed: %d\n", eglGetError());
        has_gl = 0;
    }

    if (has_gl && !eglChooseConfig(dpy, choose_attribs, &config, 1, &num_config)) {
        printf("eglChooseConfig failed: %d\n", eglGetError());
        has_gl = 0;
    }

    if (has_gl && num_config == 0) {
        printf("eglChooseConfig: no configs\n");
        has_gl = 0;
    }

    surf = eglCreatePbufferSurface(dpy, config, attrib_list);
    if (surf == EGL_NO_SURFACE) {
        printf("eglCreatePbufferSurface failed: %d\n", eglGetError());
        has_gl = 0;
    }

    if (has_gl) {
        ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
        if (ctx == EGL_NO_CONTEXT) {
            printf("eglCreateContext failed: %d\n", eglGetError());
            has_gl = 0;
        }
    }

    if (has_gl && !eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("eglMakeCurrent failed: %d\n", eglGetError());
        has_gl = 0;
    }

    if (has_gl) {
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(dpy, surf);
        printf("GL red frame rendered!\n");
    }

    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);

    printf("GL smoke test %s\n", has_gl ? "PASSED" : "SKIPPED (GL unavailable)");
    return has_gl ? 0 : 1;
}
