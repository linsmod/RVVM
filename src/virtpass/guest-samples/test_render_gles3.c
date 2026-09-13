/*
 * test_render_gles3.c - GL ES 3.0 pipeline test with pixel readback
 *
 * test_render_gles.c covers the GLES2 ABI. This one drives what GLES3 adds,
 * choosing the calls that stress the ABI rather than the ones that are easy to
 * call, and checking the result through the pixels:
 *
 *   1. EGL bring-up with an ES3 context (eglBindAPI, EGL_OPENGL_ES3_BIT,
 *      EGL_CONTEXT_CLIENT_VERSION=3), confirmed through GL_VERSION,
 *      GL_MAJOR_VERSION/GL_MINOR_VERSION and eglQueryContext
 *   2. the extension list through GL_NUM_EXTENSIONS + glGetStringi, the GLES3
 *      replacement for glGetString(GL_EXTENSIONS)
 *   3. a #version 300 es program with an *integer* vertex attribute. Both
 *      halves of the overloaded pointer argument are covered: the *offset*
 *      form by glVertexAttribPointer/glVertexAttribIPointer into a VBO (the
 *      integer cell sits 12 bytes in), the *address* form by glDrawElements
 *      reading a client-side index array - GLES3 forbids client-side vertex
 *      arrays, so indices are the one place the address form is still legal
 *   4. instancing (glVertexAttribDivisor + glDrawArraysInstanced, then
 *      glDrawElementsInstanced with a VBO index offset)
 *   5. a uniform block (glGetUniformBlockIndex/glUniformBlockBinding/
 *      glBindBufferBase) actually moving geometry
 *   6. glTexStorage2D + glTexSubImage2D from a guest array
 *   7. a 3D texture through glTexImage3D (10 args) and glTexSubImage3D (11
 *      args, the widest call in the ABI) sampled per layer
 *   8. multi-target rendering: two colour attachments, glDrawBuffers,
 *      glReadBuffer(GL_COLOR_ATTACHMENT1) and a glBlitFramebuffer present
 *   9. an occlusion query (glGenQueries/glBeginQuery/glEndQuery/
 *      glGetQueryObjectuiv)
 *  10. a fence sync: GLsync round-trip through glIsSync/glClientWaitSync
 *  11. 64-bit queries (glGetInteger64v, glGetBufferParameteri64v)
 *  12. a frame loop presenting through eglSwapBuffers
 *
 * Stages 3, 4, 5 and 7 are the ones with no GLES2 equivalent in the ABI: the
 * integer attribute, the instanced index offset, the uniform block and the
 * 11-argument 3D upload all fail visibly if a marshalling slot is wrong, while
 * the same rendering without them is pixel-identical. That is why the probes
 * check the *difference* a correct value makes (a shifted triangle, a black
 * corner) instead of just "something was drawn".
 *
 * Every stage reports PASS/FAIL and the run exits non-zero if any fails, so
 * this works as a regression gate rather than a manual eyeball check.
 *
 * Structure deliberately kept to one file and plain C: no GL loader, no math
 * library. Vertices are in clip space so no matrix code is needed.
 */

#include <stdio.h>
#include <stdlib.h>  /* getenv: RVVM_GL_TEST_SIZE */
#include <string.h>  /* strcmp */
#include <stdint.h>  /* int32_t, uint16_t */
#include "virtpass/vp_gl.h"
#include "virtpass/vp_android.h"

/* Two run modes, selected at startup, mirroring test_render_gles.c:
 *
 *  - default: WINDOW SURFACE, the path a real app takes. The guest binds
 *    eglCreateWindowSurface with a placeholder (NULL) native window - the host
 *    owns the real one - and eglSwapBuffers presents. The window dictates the
 *    surface size, so the content size is re-latched from eglQuerySurface
 *    after bring-up.
 *
 *  - RVVM_GL_TEST_OFFSCREEN=1: PBUFFER mode, the pixel-exact regression gate.
 *    The content size comes from RVVM_GL_TEST_SIZE (default 64x64):
 *      RVVM_GL_TEST_SIZE=WxH   exact content size (may exceed the panel)
 *      RVVM_GL_TEST_SIZE=full  match the panel
 *
 * A window surface is single-buffered from the guest's point of view, so every
 * pixel probe below reads back *before* eglSwapBuffers. */
static int32_t g_fb_w = 64;
static int32_t g_fb_h = 64;
static int g_offscreen;

#define FB_MAX 2048

static int g_failures;
static int g_stage;

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
    if (!ok) {
        printf("FIRST FAILURE: %s\n", what);
        g_failures++;
    }
}

/* Drain and report the GL error queue. A leftover error from an earlier stage
 * would otherwise be misattributed to the current one. */
static int gl_error_clear(const char* where)
{
    int err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("    GL error 0x%04x at %s\n", err, where);
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

/* Everything a stage might need to talk to a program. -1 for the names the
 * program does not declare, so the attribute setup can skip them. */
typedef struct {
    GLuint prog;
    GLint  l_pos;
    GLint  l_cell;
    GLint  l_off;
    GLint  u_color;
    GLint  u_cell_scale;
    GLint  u_tex2d;
    GLint  u_tex3d;
    GLint  u_slice;
} Prog;

static void prog_make(Prog* p, const char* vs_src, const char* fs_src)
{
    memset(p, 0, sizeof(*p));
    p->prog = 0;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        /* Counted as a failure here: the stages bail out via `goto done`, so a
         * shader that fails to build would otherwise let the run report success
         * with the remaining stages silently skipped. */
        check(0, "stage shaders compiled");
        return;
    }

    p->prog = glCreateProgram();
    glAttachShader(p->prog, vs);
    glAttachShader(p->prog, fs);
    glLinkProgram(p->prog);

    GLint ok = 0;
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetProgramInfoLog(p->prog, (GLsizei)sizeof(log), &n, log);
        printf("    program link failed: %.*s\n", (int)n, log);
        glDeleteProgram(p->prog);
        p->prog = 0;
    }

    /* The shaders are owned by the program once linked. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!p->prog) {
        check(0, "stage program linked");
        return;
    }

    p->l_pos        = glGetAttribLocation(p->prog, "a_pos");
    p->l_cell       = glGetAttribLocation(p->prog, "a_cell");
    p->l_off        = glGetAttribLocation(p->prog, "a_off");
    p->u_color      = glGetUniformLocation(p->prog, "u_color");
    p->u_cell_scale = glGetUniformLocation(p->prog, "u_cell_scale");
    p->u_tex2d      = glGetUniformLocation(p->prog, "u_tex2d");
    p->u_tex3d      = glGetUniformLocation(p->prog, "u_tex3d");
    p->u_slice      = glGetUniformLocation(p->prog, "u_slice");
}

static void prog_free(Prog* p)
{
    if (p->prog) glDeleteProgram(p->prog);
    p->prog = 0;
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

/* ---------------- geometry ---------------- */

/* One vertex: a float position followed by an integer cell index. The two
 * attribute pointers walk the same 20-byte stride, which is what makes the
 * GLES3 integer attribute testable against the GLES2 float one. */
typedef struct {
    float   pos[3];
    int32_t cell[2];
} Vert;

/* Clip-space triangles: no matrices needed, the vertices are already in NDC.
 * NDC y is up, framebuffer row 0 is the bottom. */
static const Vert TRI_FULL[] = {        /* covers the whole viewport */
    { { -1.0f, -1.0f, 0.0f }, { 0, 0 } },
    { {  3.0f, -1.0f, 0.0f }, { 0, 0 } },
    { { -1.0f,  3.0f, 0.0f }, { 0, 0 } },
};
static const Vert TRI_LEFT[] = {        /* left half only */
    { { -1.0f, -1.0f, 0.0f }, { 0, 0 } },
    { {  0.0f, -1.0f, 0.0f }, { 0, 0 } },
    { { -1.0f,  1.0f, 0.0f }, { 0, 0 } },
};

/* TRI_LEFT with a cell index of (1,1): a correctly marshalled integer
 * attribute shifts the triangle into the upper right, so the bottom left
 * corner stays black. Read as float bits instead - the failure mode a wrong
 * classification would produce - the shift disappears and the lower left
 * stays painted.
 *
 * TRI_LEFT shifted by (1,1) is (0,0) (1,0) (0,2): it covers the upper right
 * quadrant of the viewport, and pixel (1,1) is far outside it. */
static const Vert TRI_L_SHIFT[] = {
    { { -1.0f, -1.0f, 0.0f }, { 1, 1 } },
    { {  0.0f, -1.0f, 0.0f }, { 1, 1 } },
    { { -1.0f,  1.0f, 0.0f }, { 1, 1 } },
};

/* Client-side index array. GLES3 removed client-side *vertex* arrays (a
 * glVertexAttribPointer/glVertexAttribIPointer with no buffer bound is an
 * INVALID_OPERATION), so a GLES3 sample cannot use the address form of those
 * two. glDrawElements is different: with no element buffer bound, `indices` is
 * still a guest address, which is exactly the overloaded pointer argument in
 * its address form. */
static const uint16_t TRI_L_INDEX[] = { 0, 1, 2 };

/* Per-instance offsets: (0,0) keeps TRI_LEFT on the left half, (1,0) moves the
 * second copy onto the right half. The probes then require BOTH halves to be
 * painted, which only happens if the divisor actually delivered two instances. */
static const float INST_OFF[] = { 0.0f, 0.0f,  1.0f, 0.0f };
static const uint16_t INST_IDX[] = { 0, 1, 2 };

static GLuint make_vert_vbo(const Vert* v, int n)
{
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * (int)sizeof(Vert)), v,
                 GL_STATIC_DRAW);
    return b;
}

/* ---------------- shaders (GLSL ES 3.00) ---------------- */

#define VS_MAIN                                                               \
    "#version 300 es\n"                                                       \
    "in vec3 a_pos;\n"                                                        \
    /* GLES3: integer vertex attribute, fed by glVertexAttribIPointer */      \
    "in ivec2 a_cell;\n"                                                      \
    "in vec2 a_off;\n"   /* per-instance offset, divisor 1 */                 \
    "uniform int u_cell_scale;\n"                                             \
    "out vec2 v_uv;\n"                                                        \
    "void main() {\n"                                                         \
    "    vec2 cell = vec2(a_cell) * float(u_cell_scale);\n"                   \
    "    v_uv = a_pos.xy * 0.5 + 0.5;\n"                                      \
    "    gl_Position = vec4(a_pos.xy + cell + a_off, 0.0, 1.0);\n"            \
    "}\n"

#define FS_SOLID                                                              \
    "#version 300 es\n"                                                       \
    "precision mediump float;\n"                                              \
    "uniform vec4 u_color;\n"                                                 \
    "out vec4 o_color;\n"                                                     \
    "void main() { o_color = u_color; }\n"

#define FS_TEX2D                                                              \
    "#version 300 es\n"                                                       \
    "precision mediump float;\n"                                              \
    "uniform sampler2D u_tex2d;\n"                                            \
    "in vec2 v_uv;\n"                                                         \
    "out vec4 o_color;\n"                                                     \
    "void main() { o_color = texture(u_tex2d, v_uv); }\n"

/* The fragment language predeclares default precisions for float, int,
 * sampler2D and samplerCube only - sampler3D has none, so leaving the qualifier
 * off is a compile error ("No precision specified (sampler)"), not a warning. */
#define FS_TEX3D                                                              \
    "#version 300 es\n"                                                       \
    "precision mediump float;\n"                                              \
    "uniform mediump sampler3D u_tex3d;\n"                                    \
    "uniform float u_slice;\n"                                                \
    "in vec2 v_uv;\n"                                                         \
    "out vec4 o_color;\n"                                                     \
    "void main() { o_color = texture(u_tex3d, vec3(v_uv, u_slice)); }\n"

#define VS_UBO                                                                \
    "#version 300 es\n"                                                       \
    "in vec3 a_pos;\n"                                                        \
    "layout(std140) uniform Scene {\n"                                        \
    "    vec4 u_off;\n"                                                       \
    "};\n"                                                                    \
    "void main() { gl_Position = vec4(a_pos + u_off.xyz, 1.0); }\n"

#define FS_MRT                                                                \
    "#version 300 es\n"                                                       \
    "precision mediump float;\n"                                              \
    "layout(location = 0) out vec4 o_a;\n"                                    \
    "layout(location = 1) out vec4 o_b;\n"                                    \
    "void main() {\n"                                                         \
    "    o_a = vec4(1.0, 0.0, 0.0, 1.0);\n"  /* red  -> GL_COLOR_ATTACHMENT0 */ \
    "    o_b = vec4(0.0, 0.0, 1.0, 1.0);\n"  /* blue -> GL_COLOR_ATTACHMENT1 */ \
    "}\n"

int main(void)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLint major = 0, minor = 0, num_config = 0;
    EGLConfig config = NULL;
    GLuint vao = 0, vbo_full = 0, vbo_left = 0;

    g_offscreen = offscreen_from_env();
    if (g_offscreen) content_size_from_env(&g_fb_w, &g_fb_h);
    printf("=== GL ES 3.0 Pipeline Test ===\n");
    printf("mode: %s\n", g_offscreen ? "offscreen pbuffer (regression)"
                                     : "window surface (native present)");
    printf("content size: %dx%d\n", (int)g_fb_w, (int)g_fb_h);

    const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  g_fb_w,
        EGL_HEIGHT, g_fb_h,
        EGL_NONE
    };
    /* ES 3.0 context: EGL_CONTEXT_CLIENT_VERSION 3 (the token is the same as
     * EGL_CONTEXT_MAJOR_VERSION). */
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    /* EGL_OPENGL_ES3_BIT is what tells eglChooseConfig the context version we
     * are going to ask for. */
    const EGLint choose_attribs[] = {
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    /* ---------------- 1. EGL bring-up (ES3) ---------------- */
    stage("EGL bring-up and ES3 context");
    {
        /* An ES3 app is expected to select the client API first. */
        check(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI(EGL_OPENGL_ES_API)");

        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        check(dpy != EGL_NO_DISPLAY, "eglGetDisplay");

        check(eglInitialize(dpy, &major, &minor), "eglInitialize");
        printf("    EGL %d.%d\n", major, minor);

        check(eglChooseConfig(dpy, choose_attribs, &config, 1, &num_config)
              && num_config == 1, "eglChooseConfig(EGL_OPENGL_ES3_BIT)");

        if (g_offscreen) {
            surf = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
            check(surf != EGL_NO_SURFACE, "eglCreatePbufferSurface");
        } else {
            /* Real-app path: the stub window handle is a placeholder - the
             * host owns the native window - so pass NULL. */
            surf = eglCreateWindowSurface(dpy, config, NULL, NULL);
            check(surf != EGL_NO_SURFACE, "eglCreateWindowSurface");

            /* The window dictates the surface size; re-latch it so every pixel
             * probe below lands inside the real surface. */
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
        check(ctx != EGL_NO_CONTEXT, "eglCreateContext(client version 3)");

        check(eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent");

        /* Read the version back three different ways: a string, the ES3
         * integer queries, and the context attribute the app asked for. */
        const char* ver = (const char*)glGetString(GL_VERSION);
        check(ver != NULL, "glGetString(GL_VERSION)");
        if (ver) {
            printf("    GL_VERSION : %s\n", ver);
            check(strncmp(ver, "OpenGL ES 3", 11) == 0,
                  "GL_VERSION reports OpenGL ES 3.x");
        }
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        printf("    GL_RENDERER: %s\n", renderer ? renderer : "(null)");
        printf("    GLSL       : %s\n", glsl ? glsl : "(null)");

        GLint vmaj = 0, vmin = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &vmaj);
        glGetIntegerv(GL_MINOR_VERSION, &vmin);
        check(vmaj >= 3, "glGetIntegerv(GL_MAJOR_VERSION) >= 3");
        printf("    GL %d.%d\n", vmaj, vmin);

        EGLint cver = 0;
        check(eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, &cver)
              && cver == 3, "eglQueryContext(EGL_CONTEXT_CLIENT_VERSION) == 3");

        if (g_failures) goto done;
    }
    gl_error_clear("EGL bring-up");

    /* ---------------- 2. extension list (glGetStringi) ---------------- */
    stage("Extension list via GL_NUM_EXTENSIONS + glGetStringi");
    {
        GLint next = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &next);
        check(next > 0, "GL_NUM_EXTENSIONS > 0");
        printf("    %d extensions\n", next);

        /* ES3 removed glGetString(GL_EXTENSIONS); implementations answer NULL.
         * Informational only - the spec is explicit but a permissive driver
         * still returning the list should not fail the gate. */
        const char* legacy = (const char*)glGetString(GL_EXTENSIONS);
        printf("    glGetString(GL_EXTENSIONS) -> %s\n",
               legacy ? "non-NULL (tolerated)" : "NULL (ES3 correct)");

        /* Every index must produce a string. This is the only place the ABI
         * hands out a host string per index rather than per call, so a broken
         * retbuf slot shows up as NULL/empty entries here. */
        int bad = 0;
        for (GLint i = 0; i < next; i++) {
            const char* name = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (!name || !*name) { bad++; continue; }
            if (i < 3) printf("    [%d] %s\n", i, name);
        }
        check(bad == 0, "every glGetStringi(GL_EXTENSIONS, i) returns a name");
    }
    gl_error_clear("extension stage");

    /* ---------------- 3. VAO + integer vertex attribute ---------------- */
    stage("VAO + integer attribute (VBO offset, then client-index address)");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_SOLID);
        check(p.prog != 0, "#version 300 es program linked");
        if (!p.prog) goto done;

        glGenVertexArrays(1, &vao);
        check(vao != 0, "glGenVertexArrays");
        glBindVertexArray(vao);
        GLint binding = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &binding);
        check((GLuint)binding == vao, "GL_VERTEX_ARRAY_BINDING follows the bind");

        vbo_full = make_vert_vbo(TRI_FULL, 3);
        vbo_left = make_vert_vbo(TRI_LEFT, 3);
        check(vbo_full != 0 && vbo_left != 0, "glGenBuffers + glBufferData");

        glUseProgram(p.prog);
        check(p.l_pos >= 0 && p.l_cell >= 0,
              "a_pos and a_cell are active attributes");
        check(p.u_cell_scale >= 0, "u_cell_scale location resolved");

        /* (a) VBO form: the position and the integer cell live in the same
         * buffer, 20 bytes apart, so glVertexAttribIPointer gets a *byte
         * offset* - the offset half of the overloaded pointer argument. */
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);
        glEnableVertexAttribArray((GLuint)p.l_cell);
        glVertexAttribIPointer((GLuint)p.l_cell, 2, GL_INT,
                               (GLsizei)sizeof(Vert),
                               (void*)(3 * sizeof(float)));
        if (p.l_off >= 0) {
            glDisableVertexAttribArray((GLuint)p.l_off);
            glVertexAttribDivisor((GLuint)p.l_off, 0);
        }

        glUniform1i(p.u_cell_scale, 1);
        glUniform4f(p.u_color, 1.0f, 0.0f, 0.0f, 1.0f);   /* red */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        check(glGetError() == GL_NO_ERROR, "no GL error after the VBO draw");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 0, 255, "centre is red");
        expect_pixel(1, 1, 255, 0, 0, 255,
                     "cell (0,0) does not shift the full triangle");

        /* (b) address half of the overloaded pointer argument. Client *vertex*
         * arrays are not legal in an ES 3.0 context, so the attributes stay in
         * VBO-offset form and it is glDrawElements that receives a guest
         * address - a client-side index array, drawn with no element buffer
         * bound. The buffer holds TRI_LEFT with cell (1,1). */
        GLuint vbo_shift = make_vert_vbo(TRI_L_SHIFT, 3);
        check(vbo_shift != 0, "glGenBuffers + glBufferData (cell (1,1))");
        glBindBuffer(GL_ARRAY_BUFFER, vbo_shift);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);
        glVertexAttribIPointer((GLuint)p.l_cell, 2, GL_INT,
                               (GLsizei)sizeof(Vert),
                               (void*)(3 * sizeof(float)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, TRI_L_INDEX);
        check(glGetError() == GL_NO_ERROR,
              "no GL error after the client-index draw");
        expect_pixel(1, 1, 0, 0, 0, 255,
                     "cell (1,1) shifted the triangle: corner is black");
        expect_pixel(g_fb_w * 3 / 4, g_fb_h * 3 / 4, 255, 0, 0, 255,
                     "shifted triangle covers the upper right");
        glDeleteBuffers(1, &vbo_shift);

        prog_free(&p);
    }
    gl_error_clear("integer attribute stage");

    /* ---------------- 4. instancing ---------------- */
    stage("Instancing (glVertexAttribDivisor, draw*, instanced)");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_SOLID);
        if (!p.prog) goto done;

        check(p.l_off >= 0, "a_off is an active attribute");
        if (p.l_off < 0) { prog_free(&p); goto done; }

        GLuint vbo_inst = 0;
        glGenBuffers(1, &vbo_inst);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(INST_OFF), INST_OFF,
                     GL_STATIC_DRAW);

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        glUniform4f(p.u_color, 0.0f, 1.0f, 0.0f, 1.0f);   /* green */

        /* a_pos from the left-half triangle, a_off per instance. */
        glBindBuffer(GL_ARRAY_BUFFER, vbo_left);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);
        glDisableVertexAttribArray((GLuint)p.l_cell);
        /* a_cell is an *integer* attribute (ivec2), so park it with the integer
         * setter. Feeding an integer attribute through glVertexAttrib2f leaves
         * its generic value ill-defined for the draws that follow. */
        glVertexAttribI4i((GLuint)p.l_cell, 0, 0, 0, 0);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
        glEnableVertexAttribArray((GLuint)p.l_off);
        glVertexAttribPointer((GLuint)p.l_off, 2, GL_FLOAT, GL_FALSE, 0,
                              (void*)0);
        glVertexAttribDivisor((GLuint)p.l_off, 1);

        /* (a) glDrawArraysInstanced: two copies, one per instance offset. */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
        check(glGetError() == GL_NO_ERROR, "no GL error after glDrawArraysInstanced");
        expect_pixel(g_fb_w / 4, g_fb_h / 4, 0, 255, 0, 255,
                     "instance 0 painted the left quarter");
        expect_pixel(g_fb_w * 3 / 4, g_fb_h / 4, 0, 255, 0, 255,
                     "instance 1 painted the right quarter");

        /* (b) glDrawElementsInstanced with the indices at byte offset 0 of a
         * bound element buffer: the instanced variant of the offset form. */
        GLuint ibo = 0;
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(INST_IDX),
                     INST_IDX, GL_STATIC_DRAW);

        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElementsInstanced(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (void*)0, 2);
        check(glGetError() == GL_NO_ERROR, "no GL error after glDrawElementsInstanced");
        expect_pixel(g_fb_w / 4, g_fb_h / 4, 0, 255, 0, 255,
                     "indexed instance 0 painted the left quarter");
        expect_pixel(g_fb_w * 3 / 4, g_fb_h / 4, 0, 255, 0, 255,
                     "indexed instance 1 painted the right quarter");

        glVertexAttribDivisor((GLuint)p.l_off, 0);
        /* vbo_inst is about to go. Leaving a_off enabled would leave an enabled
         * array with no buffer once the name is deleted, and ES 3.0 rejects a
         * draw that has a bufferless (client-side) enabled array - which is
         * exactly the VS_MAIN draws in the stages that follow. */
        glDisableVertexAttribArray((GLuint)p.l_off);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &ibo);
        glDeleteBuffers(1, &vbo_inst);
        prog_free(&p);
    }
    gl_error_clear("instancing stage");

    /* ---------------- 5. uniform block ---------------- */
    stage("Uniform block (glBindBufferBase / glUniformBlockBinding)");
    {
        Prog p;
        prog_make(&p, VS_UBO, FS_SOLID);
        check(p.prog != 0, "program with a uniform block linked");
        if (!p.prog) goto done;

        GLuint idx = glGetUniformBlockIndex(p.prog, "Scene");
        check(idx != GL_INVALID_INDEX, "glGetUniformBlockIndex(Scene)");
        printf("    block index: %u\n", idx);

        /* vec4 u_off = (1,0,0,0): shifts the full-screen triangle right, so the
         * bottom-left corner must go black. Without the block reaching the
         * shader the triangle would still cover everything. */
        static const float UBO_DATA[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        GLuint ubo = 0;
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)sizeof(UBO_DATA), UBO_DATA,
                     GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glUniformBlockBinding(p.prog, idx, 0);

        GLint block_size = 0;
        glGetActiveUniformBlockiv(p.prog, idx, GL_UNIFORM_BLOCK_DATA_SIZE,
                                  &block_size);
        check(block_size >= (GLint)sizeof(UBO_DATA), "block data size >= 16");
        printf("    block size: %d bytes\n", block_size);

        glUseProgram(p.prog);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);

        glUniform4f(p.u_color, 1.0f, 1.0f, 0.0f, 1.0f);   /* yellow */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        check(glGetError() == GL_NO_ERROR, "no GL error after the UBO draw");
        expect_pixel(1, 1, 0, 0, 0, 255, "u_off moved the triangle: corner is black");
        expect_pixel(g_fb_w * 3 / 4, g_fb_h * 3 / 4, 255, 255, 0, 255,
                     "upper right is yellow");

        glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
        glDeleteBuffers(1, &ubo);
        prog_free(&p);
    }
    gl_error_clear("uniform block stage");

    /* ---------------- 6. 2D texture: glTexStorage2D ---------------- */
    stage("glTexStorage2D + glTexSubImage2D from a guest array");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_TEX2D);
        if (!p.prog) goto done;

        enum { TEX_W = 8, TEX_H = 8 };
        static unsigned char tex[TEX_W * TEX_H * 4];
        for (int i = 0; i < TEX_W * TEX_H; i++) {
            tex[i * 4 + 0] = 0;
            tex[i * 4 + 1] = 0;
            tex[i * 4 + 2] = 255;   /* blue */
            tex[i * 4 + 3] = 255;
        }

        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        /* Immutable storage first, then the pixels: the GLES3 idiom. */
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TEX_W, TEX_H);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* `tex` is a guest array: glTexSubImage2D must translate the pointer. */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, tex);
        check(glGetError() == GL_NO_ERROR, "no GL error during the texture upload");

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t);
        glUniform1i(p.u_tex2d, 0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        check(glGetError() == GL_NO_ERROR, "no GL error after the textured draw");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 0, 0, 255, 255, "sampled pixel is blue");

        glDeleteTextures(1, &t);
        prog_free(&p);
    }
    gl_error_clear("2D texture stage");

    /* ---------------- 7. 3D texture: glTexImage3D + glTexSubImage3D ------- */
    stage("3D texture (glTexImage3D 10 args, glTexSubImage3D 11 args)");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_TEX3D);
        if (!p.prog) goto done;

        enum { T3_W = 4, T3_H = 4, T3_D = 2 };
        /* Sub-image for layer 1: opaque blue. The alpha byte must be written
         * too - leaving it zero makes the sample read back (0,0,255,0) and the
         * opaque-blue check below fails on every driver. */
        static unsigned char layer_blue[T3_W * T3_H * 4];
        for (int i = 0; i < T3_W * T3_H; i++) {
            layer_blue[i * 4 + 0] = 0;   layer_blue[i * 4 + 1] = 0;
            layer_blue[i * 4 + 2] = 255; layer_blue[i * 4 + 3] = 255;
        }
        /* Allocate both layers red, then overwrite layer 1 with blue through
         * glTexSubImage3D - the widest call the ABI has, and the reason
         * gl_call has room for 11 arguments. */
        static unsigned char all_red[T3_W * T3_H * T3_D * 4];
        for (int i = 0; i < T3_W * T3_H * T3_D; i++) {
            all_red[i * 4 + 0] = 255; all_red[i * 4 + 1] = 0;
            all_red[i * 4 + 2] = 0;   all_red[i * 4 + 3] = 255;
        }

        GLuint t3 = 0;
        glGenTextures(1, &t3);
        glBindTexture(GL_TEXTURE_3D, t3);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, T3_W, T3_H, T3_D, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, all_red);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        check(glGetError() == GL_NO_ERROR, "glTexImage3D accepted 10 arguments");

        /* zoffset = 1: only layer 1 is replaced. */
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 1, T3_W, T3_H, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, layer_blue);
        check(glGetError() == GL_NO_ERROR, "glTexSubImage3D accepted 11 arguments");

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, t3);
        glUniform1i(p.u_tex3d, 0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        /* Layer 0 spans w in [0,0.5), layer 1 in [0.5,1). */
        glUniform1f(p.u_slice, 0.25f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        check(glGetError() == GL_NO_ERROR, "no GL error sampling layer 0");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 0, 255,
                     "layer 0 is still red");

        glUniform1f(p.u_slice, 0.75f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 0, 0, 255, 255,
                     "layer 1 took the glTexSubImage3D pixels (blue)");

        glDeleteTextures(1, &t3);
        prog_free(&p);
    }
    gl_error_clear("3D texture stage");

    /* ---------------- 8. multi-target rendering ---------------- */
    stage("Multi-target FBO + glReadBuffer + glBlitFramebuffer");
    {
        GLuint fbo = 0, mrt_tex[2] = { 0, 0 };
        glGenFramebuffers(1, &fbo);
        glGenTextures(2, mrt_tex);
        check(fbo != 0 && mrt_tex[0] != 0 && mrt_tex[1] != 0,
              "glGenFramebuffers / glGenTextures");

        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, mrt_tex[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, g_fb_w, g_fb_h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, mrt_tex[0], 0);
        /* GL_COLOR_ATTACHMENT1 is GLES3-only: two colour targets on one FBO. */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D, mrt_tex[1], 0);
        const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, bufs);

        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "MRT framebuffer is complete");

        Prog p;
        prog_make(&p, VS_MAIN, FS_MRT);
        if (!p.prog) { glBindFramebuffer(GL_FRAMEBUFFER, 0); goto done; }

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        if (p.l_off >= 0) glDisableVertexAttribArray((GLuint)p.l_off);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        check(glGetError() == GL_NO_ERROR, "no GL error after the MRT draw");

        /* Both outputs must have landed in their own attachment. */
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 0, 255,
                     "attachment 0 received o_a (red)");
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 0, 0, 255, 255,
                     "attachment 1 received o_b (blue)");
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        /* Blit attachment 0 (red) into the default framebuffer: exercises the
         * 10-argument glBlitFramebuffer and the READ/DRAW framebuffer split. */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, g_fb_w, g_fb_h, 0, 0, g_fb_w, g_fb_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        check(glGetError() == GL_NO_ERROR, "no GL error after glBlitFramebuffer");
        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 0, 255,
                     "blit copied attachment 0 onto the default framebuffer");

        prog_free(&p);
        glDeleteTextures(2, mrt_tex);
        glDeleteFramebuffers(1, &fbo);
    }
    gl_error_clear("MRT stage");

    /* ---------------- 9. occlusion query ---------------- */
    stage("Occlusion query (glBeginQuery/glEndQuery/glGetQueryObjectuiv)");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_SOLID);
        if (!p.prog) goto done;

        GLuint q = 0;
        glGenQueries(1, &q);
        check(q != 0, "glGenQueries");
        /* Negative control: zero is never the name of a query object. */
        check(glIsQuery(0) == GL_FALSE, "glIsQuery(0)");

        /* glIsQuery on a *fresh* name is deliberately not asserted: the spec
         * calls that GL_TRUE, but ANGLE/D3D11 only materialises the query
         * object at glBeginQuery and answers GL_FALSE until then. Report what
         * the driver said; the portable assertions follow below. */
        printf("    glIsQuery after glGenQueries: %s (driver-dependent)\n",
               glIsQuery(q) == GL_TRUE ? "GL_TRUE" : "GL_FALSE (lazy object)");

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        glUniform4f(p.u_color, 1.0f, 0.0f, 1.0f, 1.0f);   /* magenta */
        if (p.l_off >= 0) glDisableVertexAttribArray((GLuint)p.l_off);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, q);
        /* From glBeginQuery on, the name really is a query object on every
         * driver - this is the positive form the stage asserts. */
        check(glIsQuery(q) == GL_TRUE, "glIsQuery while the query is active");
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_ANY_SAMPLES_PASSED);

        /* Availability straight after glEndQuery is NOT guaranteed: the result
         * is only required to be there once the query's commands have
         * completed. A desktop driver that has already retired the tiny draw
         * answers GL_TRUE, but a phone's tiled/deferred GPU has not even
         * submitted it yet and answers GL_FALSE - legal, and not a failure.
         * Report that value, then assert the portable form: glFinish()
         * completes every command, so the result must be available after it. */
        GLuint avail = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        printf("    available straight after glEndQuery: %s\n",
               avail == GL_TRUE ? "yes" : "no (deferred, tolerated)");

        glFinish();
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        check(avail == GL_TRUE, "GL_QUERY_RESULT_AVAILABLE after glFinish");

        GLuint samples = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &samples);
        check(samples == GL_TRUE, "GL_ANY_SAMPLES_PASSED == GL_TRUE");
        printf("    query result: %u\n", samples);

        expect_pixel(g_fb_w / 2, g_fb_h / 2, 255, 0, 255, 255, "frame is magenta");
        glDeleteQueries(1, &q);
        check(glIsQuery(q) == GL_FALSE, "glIsQuery after glDeleteQueries");
        prog_free(&p);
    }
    gl_error_clear("query stage");

    /* ---------------- 10. fence sync (GLsync handle) ---------------- */
    stage("Fence sync (glFenceSync / glClientWaitSync / GLuint64 timeout)");
    {
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        check(sync != NULL, "glFenceSync returned a handle");
        check(glIsSync(sync) == GL_TRUE, "glIsSync");

        /* The handle travels to the host and back, and the timeout is a GLuint64
         * argument - the second GLES3-only parameter width in the ABI. */
        GLenum r = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                                    1000000000ull);
        check(r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED,
              "glClientWaitSync signalled within 1s");
        printf("    glClientWaitSync -> 0x%04x\n", r);

        glDeleteSync(sync);
        check(glIsSync(sync) == GL_FALSE, "glDeleteSync dropped the handle");
    }
    gl_error_clear("fence stage");

    /* ---------------- 11. 64-bit queries ---------------- */
    stage("64-bit queries (glGetInteger64v / glGetBufferParameteri64v)");
    {
        GLint64 max_elem = 0;
        glGetInteger64v(GL_MAX_ELEMENT_INDEX, &max_elem);
        check(max_elem > 0, "glGetInteger64v(GL_MAX_ELEMENT_INDEX) > 0");
        printf("    max element index: %lld\n", (long long)max_elem);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        GLint64 bytes = -1;
        glGetBufferParameteri64v(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bytes);
        check(bytes == (GLint64)sizeof(TRI_FULL),
              "glGetBufferParameteri64v == the buffer size");
        printf("    buffer size: %lld\n", (long long)bytes);
    }
    gl_error_clear("64-bit stage");

    /* ---------------- 12. present loop ---------------- */
    stage("Frame loop through eglSwapBuffers");
    {
        Prog p;
        prog_make(&p, VS_MAIN, FS_SOLID);
        if (!p.prog) goto done;

        glUseProgram(p.prog);
        glUniform1i(p.u_cell_scale, 0);
        if (p.l_off >= 0) glDisableVertexAttribArray((GLuint)p.l_off);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
        glEnableVertexAttribArray((GLuint)p.l_pos);
        glVertexAttribPointer((GLuint)p.l_pos, 3, GL_FLOAT, GL_FALSE,
                              (GLsizei)sizeof(Vert), (void*)0);

        const int frames = 3;
        int first_g = -1, last_g = -1;
        for (int f = 0; f < frames; f++) {
            float g = (float)f / (float)(frames - 1);
            glUniform4f(p.u_color, 0.0f, g, 0.0f, 1.0f);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            /* Read back before the swap: on a window surface what comes back
             * after eglSwapBuffers is the compositor's business, a pbuffer just
             * keeps its content. A pre-swap readback always sees the frame just
             * drawn, on both surfaces and both hosts. */
            unsigned char px[4] = { 0, 0, 0, 0 };
            glReadPixels(g_fb_w / 2, g_fb_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (f == 0) first_g = px[1];
            last_g = px[1];
            eglSwapBuffers(dpy, surf);
        }
        check(last_g > first_g, "the ramp advanced across the presented frames");
        printf("    green channel: first frame %d, last frame %d\n",
               first_g, last_g);
        check(glGetError() == GL_NO_ERROR, "no GL error after presenting");
        prog_free(&p);
    }
    gl_error_clear("present stage");

done:
    if (vao)      glDeleteVertexArrays(1, &vao);
    if (vbo_full) glDeleteBuffers(1, &vbo_full);
    if (vbo_left) glDeleteBuffers(1, &vbo_left);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);

    printf("\n=== %d stage(s), %d failure(s) ===\n", g_stage, g_failures);
    printf("GLES3 pipeline test %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
