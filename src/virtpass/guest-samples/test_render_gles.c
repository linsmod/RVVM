/*
 * test_render_gles.c - GL ES 2.0 pipeline test with pixel readback
 *
 * The smoke test only proved the entry points can be called. This one drives
 * the parts of GLES2 that actually move guest memory across the ABI, and
 * checks the result by reading pixels back rather than by looking for a crash:
 *
 *   1. EGL bring-up (getDisplay/initialize/chooseConfig/pbuffer/context)
 *   2. shader compile + link, with the info log read back
 *   3. a VBO triangle (glVertexAttribPointer with a *byte offset*)
 *   4. a client-array triangle (glVertexAttribPointer with a *guest address*)
 *   5. a texture uploaded from a guest-side pixel array
 *   6. an offscreen FBO render target
 *   7. glReadPixels with the result compared against expected colour
 *   8. glGetError swept after each stage
 *   9. glVertexAttribPointer + glDrawArrays through a real VBO
 *  10. eglGetProcAddress round-trip, then calling the resolved pointer
 *  11. an animated frame loop: N frames driven by the display vsync through
 *      AChoreographer, each frame shifting a vertex colour uniform, with a
 *      pixel sampled every few frames to prove the animation advances
 *
 * Stage 3 and stage 4 use the same entry point with the same argument slot
 * holding a VBO byte offset and a guest array address respectively, so a
 * regression in the offset/address disambiguation shows up as one of them
 * failing rather than as a crash.
 *
 * Every stage reports PASS/FAIL and the run exits non-zero if any fails, so
 * this works as a regression gate rather than a manual eyeball check.
 *
 * The last stage runs FRAME_TARGET frames as an animation rather than
 * rendering once and exiting: the frames are driven by the display vsync
 * through AChoreographer and the Looper, which is the path a real Android
 * frame loop takes.
 *
 * Structure deliberately kept to one file and plain C: no GL loader, no math
 * library. Vertices are in clip space so no matrix code is needed.
 */

#include <stdio.h>
#include <stdlib.h>  /* getenv: RVVM_GL_TEST_SIZE */
#include <string.h>  /* strcmp */
#include "virtpass/vp_gl.h"
#include "virtpass/vp_android.h"

/* Two run modes, selected at startup:
 *
 *  - default: WINDOW SURFACE, the path a real app takes. The guest binds
 *    eglCreateWindowSurface with a placeholder (NULL) native window - the
 *    host owns the real one - and eglSwapBuffers presents: through the DIB
 *    blit on win32 (which backs the window surface with a pbuffer) and
 *    through SurfaceFlinger on Android (which binds the real SurfaceView).
 *    The window dictates the surface size, so the content size is re-latched
 *    from eglQuerySurface after bring-up.
 *
 *  - RVVM_GL_TEST_OFFSCREEN=1: PBUFFER mode, the pixel-exact regression
 *    gate. The content size comes from RVVM_GL_TEST_SIZE (default 64x64),
 *    which is also how the oversized case is reached:
 *      RVVM_GL_TEST_SIZE=WxH   exact content size (may exceed the panel)
 *      RVVM_GL_TEST_SIZE=full  match the panel
 *    An oversized surface exercises the crop in the win32 presenter, which
 *    must clamp the copy to the panel instead of running past the DIB. */
static int32_t g_fb_w = 64;
static int32_t g_fb_h = 64;

/* Non-zero: pbuffer regression mode (RVVM_GL_TEST_OFFSCREEN=1). Zero: the
 * default window-surface mode. See the file header. */
static int g_offscreen;

#define FB_MAX 2048

static int g_failures;
static int g_stage;

/* Pick the content size from the environment, if it says anything usable.
 * Writes the requested size through w/h; leaves them alone otherwise. */
static void content_size_from_env(int32_t* w, int32_t* h)
{
    const char* s = getenv("RVVM_GL_TEST_SIZE");
    if (!s || !*s) return;

    if (strcmp(s, "full") == 0) {
        int32_t pw = ANativeWindow_getWidth(NULL);
        int32_t ph = ANativeWindow_getHeight(NULL);
        if (pw > 0 && ph > 0) { *w = pw; *h = ph; }
        return;
    }

    int vw = 0, vh = 0;
    if (sscanf(s, "%dx%d", &vw, &vh) == 2 && vw > 0 && vh > 0) {
        if (vw > FB_MAX) vw = FB_MAX;
        if (vh > FB_MAX) vh = FB_MAX;
        *w = vw;
        *h = vh;
    }
}

/* Offscreen regression mode? Anything other than unset/empty/"0" enables it. */
static int offscreen_from_env(void)
{
    const char* s = getenv("RVVM_GL_TEST_OFFSCREEN");
    return s && *s && strcmp(s, "0") != 0;
}

static void stage(const char* name)
{
    g_stage++;
    printf("\n[%d] %s\n", g_stage, name);
}

static void check(int ok, const char* what)
{
    printf("    %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_failures++;
}

/* Drain and report the GL error queue. A leftover error from an earlier stage
 * would otherwise be misattributed to the current one. */
static int gl_error_clear(const char* where)
{
    int err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("    GL error 0x%04x at %s\n", err, where);
        /* Drain the rest so the next stage starts clean. */
        while (glGetError() != GL_NO_ERROR) { }
        g_failures++;
        return 0;
    }
    return 1;
}

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &n, log);
        printf("    shader compile failed: %.*s\n", (int)n, log);
    }
    return ok ? sh : 0;
}

static GLuint link_program(const char* vs_src, const char* fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), &n, log);
        printf("    program link failed: %.*s\n", (int)n, log);
    }

    /* The shaders are owned by the program once linked. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    return ok ? prog : 0;
}

/* Read one pixel and compare against an expected RGBA value with a tolerance
 * (the host GL implementation may pick a slightly different rounding). */
static int expect_pixel(int x, int y, int r, int g, int b, int a, const char* what)
{
    unsigned char px[4] = { 0, 0, 0, 0 };
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

    int dr = (int)px[0] - r, dg = (int)px[1] - g;
    int db = (int)px[2] - b, da = (int)px[3] - a;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    if (da < 0) da = -da;

    int ok = dr <= 2 && dg <= 2 && db <= 2 && da <= 2;
    if (!ok) {
        printf("    pixel(%d,%d) got %d,%d,%d,%d want %d,%d,%d,%d\n",
               x, y, px[0], px[1], px[2], px[3], r, g, b, a);
    }
    check(ok, what);
    return ok;
}

/* Clip-space triangles: no matrices needed, the vertices are already in NDC.
 * NDC y is up, framebuffer row 0 is the bottom. */
static const float TRI_FULL[] = {        /* covers the whole viewport */
    -1.0f, -1.0f, 0.0f,
     3.0f, -1.0f, 0.0f,
    -1.0f,  3.0f, 0.0f,
};
static const float TRI_LEFT[] = {        /* left half only */
    -1.0f, -1.0f, 0.0f,
     0.0f, -1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
};

#define VS_SOLID                                                              \
    "attribute vec3 a_pos;\n"                                                 \
    "void main() { gl_Position = vec4(a_pos, 1.0); }\n"

#define FS_SOLID                                                              \
    "precision mediump float;\n"                                              \
    "uniform vec4 u_color;\n"                                                 \
    "void main() { gl_FragColor = u_color; }\n"

#define VS_TEX                                                                \
    "attribute vec3 a_pos;\n"                                                 \
    "attribute vec2 a_uv;\n"                                                  \
    "varying vec2 v_uv;\n"                                                    \
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 1.0); }\n"

#define FS_TEX                                                                \
    "precision mediump float;\n"                                              \
    "uniform sampler2D u_tex;\n"                                              \
    "varying vec2 v_uv;\n"                                                    \
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n"

static const float TRI_TEX[] = {  /* position + uv */
    -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
     3.0f, -1.0f, 0.0f,  2.0f, 0.0f,
    -1.0f,  3.0f, 0.0f,  0.0f, 2.0f,
};

/* ---------------- animated frame loop state ---------------- */

#define FRAME_TARGET 30

static EGLDisplay g_dpy;
static EGLSurface g_surf;
static GLint g_u_color;
static int g_frame;         /* frames actually rendered */
static int g_vsync_frames;  /* frames entered through the vsync callback */
static int g_probe_first = -1;
static int g_probe_last = -1;

/* One vsync period. Draws a ramp frame, reads a pixel back, and re-arms so the
 * next display frame calls us again - the standard Choreographer pattern. */
static void on_frame(long frameTimeNanos, void* data)
{
    (void)frameTimeNanos;
    (void)data;
    g_vsync_frames++;

    if (g_frame >= FRAME_TARGET) return;

    /* Green rises with the frame index; the red channel stays at zero so a
     * stale or non-animated frame is distinguishable from a live one. */
    float g = (float)g_frame / (float)(FRAME_TARGET - 1);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(g_u_color, 0.0f, g, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Sample every frame into the ends, so the first and last rendered colours
     * can be compared afterwards. */
    unsigned char px[4] = { 0, 0, 0, 0 };
    glReadPixels(g_fb_w / 2, g_fb_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (g_frame == 0) g_probe_first = px[1];
    g_probe_last = px[1];

    eglSwapBuffers(g_dpy, g_surf);
    g_frame++;

    if (g_frame < FRAME_TARGET) {
        AChoreographer_postFrameCallback(AChoreographer_getInstance(),
                                         on_frame, NULL);
    }
}

int main(void)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLint major = 0, minor = 0, num_config = 0;
    EGLConfig config = NULL;

    g_offscreen = offscreen_from_env();
    /* Must run before the attribute arrays below: they are const-initialised
     * from g_fb_w/g_fb_h, so a late parse would send the default 64x64 to the
     * host while the pixel probes assume the requested size. Only the
     * offscreen mode honours RVVM_GL_TEST_SIZE: a window dictates its own
     * size, and the probes re-latch it after bring-up. */
    if (g_offscreen) content_size_from_env(&g_fb_w, &g_fb_h);
    printf("=== GL ES 2.0 Pipeline Test ===\n");
    printf("mode: %s\n", g_offscreen ? "offscreen pbuffer (regression)"
                                     : "window surface (native present)");
    printf("content size: %dx%d\n", (int)g_fb_w, (int)g_fb_h);

    const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  g_fb_w,
        EGL_HEIGHT, g_fb_h,
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

    /* ---------------- 1. EGL bring-up ---------------- */
    stage("EGL bring-up");

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(dpy != EGL_NO_DISPLAY, "eglGetDisplay");

    check(eglInitialize(dpy, &major, &minor), "eglInitialize");
    printf("    EGL %d.%d\n", major, minor);

    check(eglChooseConfig(dpy, choose_attribs, &config, 1, &num_config)
          && num_config == 1, "eglChooseConfig");

    if (g_offscreen) {
        surf = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
        check(surf != EGL_NO_SURFACE, "eglCreatePbufferSurface");
    } else {
        /* Real-app path: bind the window surface. The stub window handle is
         * a placeholder - the host owns the native window - so pass NULL.
         * What backs it is host policy: a panel-sized pbuffer + DIB blit on
         * win32, the real SurfaceView on Android. */
        surf = eglCreateWindowSurface(dpy, config, NULL, NULL);
        check(surf != EGL_NO_SURFACE, "eglCreateWindowSurface");

        /* The window dictates the surface size; re-latch the content size so
         * every pixel probe below lands inside the real surface. */
        EGLint wsw = 0, wsh = 0;
        if (eglQuerySurface(dpy, surf, EGL_WIDTH, &wsw) &&
            eglQuerySurface(dpy, surf, EGL_HEIGHT, &wsh) &&
            wsw > 0 && wsh > 0) {
            g_fb_w = (int32_t)wsw;
            g_fb_h = (int32_t)wsh;
        }
        printf("    window surface: %dx%d\n", (int)g_fb_w, (int)g_fb_h);
    }

    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
    check(ctx != EGL_NO_CONTEXT, "eglCreateContext");

    check(eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent");
    check(glGetString(GL_VERSION) != NULL, "glGetString(GL_VERSION)");
    printf("    GL_VERSION : %s\n", (const char*)glGetString(GL_VERSION));
    printf("    GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));

    if (g_failures) goto done;
    gl_error_clear("EGL bring-up");

    /* ---------------- 2. shader compile + link ---------------- */
    stage("Shader compile and link");
    {
        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        check(prog != 0, "solid program compiled and linked");
        if (prog) {
            GLint n = 0;
            glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
            printf("    active uniforms: %d\n", n);
            check(n >= 1, "u_color is an active uniform");
            glDeleteProgram(prog);
        } else {
            goto done;
        }
    }
    gl_error_clear("shader stage");

    /* ---------------- 3. VBO triangle (offset form) ---------------- */
    stage("VBO triangle - glVertexAttribPointer with a byte offset");
    {
        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        if (!prog) goto done;

        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        check(vbo != 0, "glGenBuffers");
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(TRI_FULL), TRI_FULL, GL_STATIC_DRAW);

        GLint loc = glGetAttribLocation(prog, "a_pos");
        check(loc >= 0, "glGetAttribLocation(a_pos)");
        glEnableVertexAttribArray((GLuint)loc);
        /* A VBO is bound and the offset is 0: this is the offset form of the
         * overloaded pointer argument, not a guest address. */
        glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

        GLint u = glGetUniformLocation(prog, "u_color");
        check(u >= 0, "glGetUniformLocation(u_color)");
        glUseProgram(prog);
        glUniform4f(u, 1.0f, 0.0f, 0.0f, 1.0f);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        check(glGetError() == GL_NO_ERROR, "no GL error after VBO draw");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 0, 255, "centre pixel is red");
        expect_pixel(1, 1, 255, 0, 0, 255, "corner pixel is red (full triangle)");

        glDisableVertexAttribArray((GLuint)loc);
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(prog);
    }
    gl_error_clear("VBO stage");

    /* ---------------- 4. client-array triangle (address form) ---------------- */
    stage("Client array triangle - glVertexAttribPointer with a guest address");
    {
        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        if (!prog) goto done;

        /* No VBO bound: the same parameter now carries a guest address. This
         * is the path the offset tagging exists to disambiguate. */
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        GLint loc = glGetAttribLocation(prog, "a_pos");
        glEnableVertexAttribArray((GLuint)loc);
        glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE, 0,
                              (void*)TRI_LEFT);

        GLint u = glGetUniformLocation(prog, "u_color");
        glUseProgram(prog);
        glUniform4f(u, 0.0f, 1.0f, 0.0f, 1.0f);   /* green */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        check(glGetError() == GL_NO_ERROR, "no GL error after client-array draw");

        /* TRI_LEFT is the lower-left triangle (-1,-1) (0,-1) (-1,1): its
         * hypotenuse runs from pixel 32 at the bottom edge to pixel 0 at the
         * top. Probe at the bottom (well inside) and off the right edge. */
        expect_pixel(g_fb_w / 4, 2, 0, 255, 0, 255, "inside the client-array triangle");
        expect_pixel(g_fb_w * 3 / 4, g_fb_h / 2, 0, 0, 0, 255, "outside is still black");

        glDisableVertexAttribArray((GLuint)loc);
        glDeleteProgram(prog);
    }
    gl_error_clear("client array stage");

    /* ---------------- 5. texture upload ---------------- */
    stage("Texture upload from a guest pixel array");
    {
        GLuint prog = link_program(VS_TEX, FS_TEX);
        if (!prog) goto done;

        enum { TEX_W = 8, TEX_H = 8 };
        static unsigned char tex[TEX_W * TEX_H * 4];
        for (int y = 0; y < TEX_H; y++) {
            for (int x = 0; x < TEX_W; x++) {
                unsigned char* p = &tex[(y * TEX_W + x) * 4];
                /* Solid blue-ish, so the sampled colour is unambiguous. */
                p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255;
            }
        }

        GLuint texid = 0;
        glGenTextures(1, &texid);
        check(texid != 0, "glGenTextures");
        glBindTexture(GL_TEXTURE_2D, texid);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* `tex` is a guest array: glTexImage2D must translate the pointer. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, tex);

        GLint loc = glGetAttribLocation(prog, "a_pos");
        GLint uvl = glGetAttribLocation(prog, "a_uv");
        glEnableVertexAttribArray((GLuint)loc);
        glEnableVertexAttribArray((GLuint)uvl);
        glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float), (void*)TRI_TEX);
        glVertexAttribPointer((GLuint)uvl, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float),
                              (void*)(TRI_TEX + 3));

        GLint u = glGetUniformLocation(prog, "u_tex");
        glUseProgram(prog);
        glUniform1i(u, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texid);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        check(glGetError() == GL_NO_ERROR, "no GL error after textured draw");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 0, 0, 255, 255, "sampled pixel is blue");

        glDisableVertexAttribArray((GLuint)loc);
        glDisableVertexAttribArray((GLuint)uvl);
        glDeleteTextures(1, &texid);
        glDeleteProgram(prog);
    }
    gl_error_clear("texture stage");

    /* ---------------- 6. offscreen FBO ---------------- */
    stage("Offscreen framebuffer render target");
    {
        GLuint fbo = 0, rb = 0;
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &rb);
        check(fbo != 0 && rb != 0, "glGenFramebuffers / glGenRenderbuffers");

        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_fb_w, g_fb_h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, rb);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        check(status == GL_FRAMEBUFFER_COMPLETE,
              "glCheckFramebufferStatus == GL_FRAMEBUFFER_COMPLETE");

        /* Render into the FBO and read it back: proves the readback follows
         * the bound framebuffer, not the window surface. */
        glClearColor(0.0f, 1.0f, 1.0f, 1.0f);   /* cyan */
        glClear(GL_COLOR_BUFFER_BIT);
        expect_pixel(0, 0, 0, 255, 255, 255, "FBO is cyan");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        check(glGetError() == GL_NO_ERROR, "no GL error after FBO stage");
        glDeleteRenderbuffers(1, &rb);
        glDeleteFramebuffers(1, &fbo);
    }
    gl_error_clear("FBO stage");

    /* ---------------- 7. state queries ---------------- */
    stage("State queries (glGetIntegerv / glGetBooleanv / glGetFloatv)");
    {
        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);
        check(vp[2] == g_fb_w && vp[3] == g_fb_h, "GL_VIEWPORT matches the surface");
        printf("    viewport: %d,%d %dx%d\n", vp[0], vp[1], vp[2], vp[3]);

        GLint max_tex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        check(max_tex >= 64, "GL_MAX_TEXTURE_SIZE is sane");
        printf("    max texture size: %d\n", max_tex);

        GLboolean depth = 0xFF;
        glGetBooleanv(GL_DEPTH_TEST, &depth);
        check(depth == GL_FALSE || depth == GL_TRUE, "glGetBooleanv(GL_DEPTH_TEST)");

        GLfloat clear_col[4] = { -1, -1, -1, -1 };
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
        check(clear_col[0] >= 0.0f && clear_col[0] <= 1.0f,
              "glGetFloatv(GL_COLOR_CLEAR_VALUE)");
    }
    gl_error_clear("query stage");

    /* ---------------- 8. depth test + vbo redraw ---------------- */
    stage("Depth test and a second VBO draw");
    {
        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        if (!prog) goto done;

        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(TRI_FULL), TRI_FULL, GL_STATIC_DRAW);

        GLint loc = glGetAttribLocation(prog, "a_pos");
        glEnableVertexAttribArray((GLuint)loc);
        glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);   /* no depth buffer on a pbuffer: keep it simple */

        GLint u = glGetUniformLocation(prog, "u_color");
        glUseProgram(prog);
        glUniform4f(u, 1.0f, 1.0f, 0.0f, 1.0f);   /* yellow */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        check(glGetError() == GL_NO_ERROR, "no GL error after depth-tested draw");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 255, 0, 255, "frame is yellow");

        glDisable(GL_DEPTH_TEST);
        glDisableVertexAttribArray((GLuint)loc);
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(prog);
    }
    gl_error_clear("depth stage");

    /* ---------------- 9. eglGetProcAddress ---------------- */
    stage("eglGetProcAddress round-trip");
    {
        typedef void (*PFNGLCLEARCOLOR)(float, float, float, float);
        PFNGLCLEARCOLOR p_clear =
            (PFNGLCLEARCOLOR)eglGetProcAddress("glClearColor");
        void* p_unknown = eglGetProcAddress("glNotAnEntryPoint");

        check(p_clear != NULL, "eglGetProcAddress(glClearColor) is callable");
        check(p_unknown == NULL, "unknown name returns NULL");
        printf("    glClearColor stub at %p\n", (void*)p_clear);

        if (p_clear) {
            /* Calling through the resolved pointer must behave exactly like
             * the direct call: repaint and confirm the colour changed. */
            p_clear(1.0f, 0.0f, 1.0f, 1.0f);   /* magenta */
            glClear(GL_COLOR_BUFFER_BIT);
            expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 255, 255,
                         "colour set through the resolved pointer");
        }
    }
    gl_error_clear("procaddress stage");

    /* ---------------- 10. surface query ---------------- */
    stage("Surface query");
    {
        EGLint sw = 0, sh = 0;
        check(eglQuerySurface(dpy, surf, EGL_WIDTH, &sw)
              && eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh),
              "eglQuerySurface");
        check(sw == g_fb_w && sh == g_fb_h, "surface size matches the request");
        printf("    surface: %dx%d\n", sw, sh);
    }
    gl_error_clear("surface query stage");

    /* ---------------- 11. animated frame loop ---------------- */
    stage("Animated frame loop (vsync-driven)");
    {
        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        if (!prog) goto done;

        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(TRI_FULL), TRI_FULL, GL_STATIC_DRAW);

        GLint loc = glGetAttribLocation(prog, "a_pos");
        glEnableVertexAttribArray((GLuint)loc);
        glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

        g_u_color = glGetUniformLocation(prog, "u_color");
        glUseProgram(prog);
        check(g_u_color >= 0, "u_color location resolved");

        g_dpy = dpy;
        g_surf = surf;

        /* Each frame renders a solid colour whose red ramp follows the frame
         * index, then hands the frame to the compositor. Re-arming from the
         * callback is the standard Choreographer pattern: the loop below only
         * pumps the Looper. */
        AChoreographer_getInstance();
        AChoreographer_postFrameCallback(AChoreographer_getInstance(),
                                         on_frame, NULL);

        while (g_frame < FRAME_TARGET) {
            /* pollOnce, not pollAll: the frame callback reports
             * ALOOPER_POLL_CALLBACK and pollAll would keep draining without
             * ever letting the loop re-check its exit condition. */
            ALooper_pollOnce(-1, NULL, NULL, NULL);
        }

        printf("    rendered %d frames, %d vsync callbacks\n",
               g_frame, g_vsync_frames);
        check(g_frame >= FRAME_TARGET, "requested frame count reached");
        check(g_vsync_frames == g_frame,
              "every frame came from a vsync callback");

        /* The ramp must have actually moved: frame 0 is dim, the last frame is
         * bright. Sampling both ends proves the loop animates rather than
         * repainting one colour. */
        check(g_probe_first != g_probe_last, "colour changed across the run");
        printf("    green channel: first frame %d, last frame %d\n",
               g_probe_first, g_probe_last);

        glDisableVertexAttribArray((GLuint)loc);
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(prog);
    }
    gl_error_clear("frame loop stage");

    /* ---------------- 12. oversized content (host-side crop) ---------------- */
    stage("Content vs panel (crop is host-side)");
    {
        /* The guest cannot observe the host panel: ANativeWindow_getWidth
         * reports the surface geometry the guest itself set, which is correct
         * Android behaviour. Whether the content overflows the panel is only
         * visible in the host log line
         *   "panel WxH <- content wxh at (0,0) [WxH used]"
         * where the "used" figures are the cropped copy.
         *
         * What the guest CAN assert is that its own framebuffer stays intact
         * no matter the size: cropping happens when the frame is composited
         * into the panel, never inside the GL surface. Render at the far
         * corner of the content and read it back. */
        printf("    content %dx%d; watch the host log for the crop figures\n",
               (int)g_fb_w, (int)g_fb_h);

        GLuint prog = link_program(VS_SOLID, FS_SOLID);
        if (prog) {
            GLint u = glGetUniformLocation(prog, "u_color");
            GLint l = glGetAttribLocation(prog, "a_pos");
            GLuint vbo = 0;
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(TRI_FULL), TRI_FULL,
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray((GLuint)l);
            glVertexAttribPointer((GLuint)l, 3, GL_FLOAT, GL_FALSE, 0,
                                  (void*)0);
            glUseProgram(prog);
            glUniform4f(u, 1.0f, 0.0f, 0.0f, 1.0f);
            glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            /* Read back BEFORE the swap. On a real window surface the back
             * buffer after eglSwapBuffers is implementation-defined - the
             * compositor's buffer queue decides which buffer comes back next
             * (a triple-buffered phone reads a two-frames-old frame), while a
             * pbuffer just keeps its content. A pre-swap readback always sees
             * the frame just drawn, on both surfaces and both hosts. */
            expect_pixel(g_fb_w - 1, g_fb_h - 1, 255, 0, 0, 255,
                         "content intact at its far corner");
            eglSwapBuffers(dpy, surf);
            check(glGetError() == GL_NO_ERROR, "no GL error after present");

            glDisableVertexAttribArray((GLuint)l);
            glDeleteBuffers(1, &vbo);
            glDeleteProgram(prog);
        }
    }
    gl_error_clear("oversize stage");

done:
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);

    printf("\n=== %d stage(s), %d failure(s) ===\n", g_stage, g_failures);
    printf("GL pipeline test %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
