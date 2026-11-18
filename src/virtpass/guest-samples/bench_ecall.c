/*
 * bench_ecall.c - ecall 开销微基准（MSG2 论断的实测）
 *
 * 背景：virtpass_browser 设计讨论中，"vp_gl_dispatch 逐调用转发，每帧几千个
 * ecall 会比软件光栅还慢"这一论断没有实测支撑。本程序把它拆成可测数字：
 *
 *   A0  纯 guest 计算循环（无 ecall）              —— 循环本底，用于相减
 *   A1  getpid() 循环                              —— 裸 ecall/trap 往返地板
 *       （host 侧 handler 只读一个字段，rvvm_user.c case 172）
 *   A2  glGetError() 循环                          —— 全 GL 通路：GLSTUB 封送
 *       （gl_call memset + ecall + cmdpost_dispatch + 分发表 + 真 GL 调用）
 *       A2 - A1 = "GL 转发税"
 *   B   帧外推：每帧 X 次 glGetError + 1 次 glTexSubImage2D 全帧上传 +
 *       glFinish，X ∈ {0,100,500,1000,5000}。拟合 ms/frame ≈ a + X*b：
 *       a = 全帧上传固定成本（指针翻译 + host 复制进纹理），
 *       b = 每个调用的实际边际成本。X=5000 就是"每帧几千个 ecall"场景。
 *   C   软件对照：guest 侧 memset 整帧（软件光栅的带宽地板，真实光栅只贵不便宜）
 *
 * 判读（60fps 预算 16.7ms）：
 *   b ~ 几百 ns  → 5000 调用/帧 ≈ 2-3ms，"比软件光栅还慢"不成立
 *   b ~ 数 us    → 3000 调用 = 10ms+，论断成立，GL 只做合成是对的
 *
 * 运行：
 *   build_virtpass.ps1            # guest-samples/ 下新 .c 自动构建打包
 *   rvvm_winhost.exe --app bench_ecall
 *   RVVM_BENCH_SIZE=1920x1080     # 可选，默认两档 1280x800 / 1920x1080 都跑
 *   RVVM_NOJIT=1                  # 可选，解释器路径的下界（未来开页表那条）
 *
 * 注意：不要开 RVVM_TRACE —— 每个 syscall 一行 trace，会把数据全毁。
 * 计时用 CLOCK_MONOTONIC（本身也是 ecall，但只在计时区间边界各一次）。
 * 全部取多轮最小值（Windows 定时器抖动明显）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "virtpass/vp_glext.h" /* GL_BGRA_EXT 等 1504 个扩展 token */

#define FB_MAX 2048

/* ---------------- globals（循环体经函数指针驱动，参数走全局） ---------------- */

static int g_iters;              /* A 系列每次计时区的迭代数 */
static int g_x_calls;            /* B 系列每帧注入的"纯转发调用"数 */
static int g_fw, g_fh;           /* 帧尺寸 */
static uint32_t* g_frame;        /* guest 侧帧 buffer */
static uint32_t g_up_format;     /* 实测可用的上传格式：GL_BGRA_EXT 或 GL_RGBA */

/* ---------------- 计时 ---------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------- 计时区循环体 ---------------- */

static void loop_a0_busy(void)
{
    /* 纯 guest 计算：guest 以 -O0 构建（GUEST_CFLAGS），与生产 guest 同口径。
     * 编译器不会把 volatile 累加提出循环。 */
    volatile uint32_t acc = 0;
    for (int i = 0; i < g_iters; i++) acc += (uint32_t)i;
}

static void loop_getpid(void)
{
    for (int i = 0; i < g_iters; i++) (void)getpid();
}

static void loop_glgeterror(void)
{
    for (int i = 0; i < g_iters; i++) glGetError();
}

static void loop_frame(void)
{
    /* "每帧几千个 ecall"的场景模拟：X 次纯转发调用 + 1 次全帧上传 + 同步。
     * glGetError 作为调用代理：所有 marshalled 调用的成本 ≈ A2（同一条
     * 封送+ecall+分发链），选它是因为无副作用、无 GL 错误队列堆积。 */
    for (int i = 0; i < g_x_calls; i++) glGetError();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_fw, g_fh,
                    g_up_format, GL_UNSIGNED_BYTE, g_frame);
    glFinish(); /* 强制同步，让上传成本落进本次计时区间 */
}

static void loop_fill(void)
{
    /* 软件对照：整帧填充。memset 是任何软件光栅的带宽地板。 */
    memset(g_frame, 0xA5, (size_t)g_fw * (size_t)g_fh * 4);
}

/* ---------------- 基准驱动 ---------------- */

/* 多轮取最小值，返回 best ns/iter */
static double bench_best(const char* name, int iters, void (*fn)(void), int rounds)
{
    double best = -1;
    g_iters = iters;
    fn(); /* 预热：稳住 JIT 热路径与缓存 */
    for (int r = 0; r < rounds; r++) {
        uint64_t t0 = now_ns();
        fn();
        uint64_t dt = now_ns() - t0;
        double per = (double)dt / (double)iters;
        if (best < 0 || per < best) best = per;
    }
    printf("  %-16s %8d iters x%d   best %10.1f ns/iter\n",
           name, iters, rounds, best);
    return best;
}

/* B 系列：frames 帧取每帧平均，多轮取最小，返回 best ns/frame */
static double bench_frames(int x, int frames, int rounds)
{
    double best = -1;
    g_x_calls = x;
    for (int i = 0; i < 10; i++) loop_frame(); /* 预热 */
    for (int r = 0; r < rounds; r++) {
        uint64_t t0 = now_ns();
        for (int f = 0; f < frames; f++) loop_frame();
        uint64_t dt = now_ns() - t0;
        double per = (double)dt / (double)frames;
        if (best < 0 || per < best) best = per;
    }
    printf("    X=%-5d %10.3f ms/frame\n", x, best / 1e6);
    return best;
}

/* ---------------- 上传格式与纹理 ---------------- */

/* 先探 BGRA_EXT 是否被 host GL 接受（无效格式会在 eglGetError 留下
 * GL_INVALID_ENUM）；不行就回退 RGBA。探测用 8x8 小纹理，不影响正式测量。 */
static int probe_upload_format(void)
{
    const int W = 8, H = 8;
    static const uint32_t px[64] = { 0 }; /* 全零，格式探测不看内容 */
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    while (glGetError() != GL_NO_ERROR) { } /* 排干历史错误 */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, W, H, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    if (glGetError() == GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        printf("  upload format: GL_BGRA_EXT\n");
        return GL_BGRA_EXT;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError() == GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        printf("  upload format: GL_RGBA (BGRA_EXT rejected by host GL)\n");
        return GL_RGBA;
    }
    glDeleteTextures(1, &tex);
    printf("  upload format: NEITHER accepted, B/C phases unreliable\n");
    return 0;
}

/* 正式测量纹理：分配 level 0（TexImage2D 带 NULL，尺寸固定，只测 SubImage） */
static int make_frame_texture(int w, int h)
{
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, (int32_t)g_up_format, w, h, 0,
                 g_up_format, GL_UNSIGNED_BYTE, NULL);
    return glGetError() == GL_NO_ERROR ? (int)tex : 0;
}

/* ---------------- 帧测试套件（一个尺寸一档） ---------------- */

static void run_frame_suite(int w, int h)
{
    g_fw = w;
    g_fh = h;
    g_frame = malloc((size_t)w * (size_t)h * 4);
    if (!g_frame) {
        printf("  OOM for %dx%d, skipped\n", w, h);
        return;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        /* 假 BGRA 像素，避免 memset 型 DRAM 压缩/零页优化失真 */
        g_frame[i] = 0xFF30548C;
    }

    int tex = make_frame_texture(w, h);
    if (!tex) {
        printf("  texture alloc failed for %dx%d, skipped\n", w, h);
        free(g_frame);
        g_frame = NULL;
        return;
    }

    printf("\n[Frame suite %dx%d (%.1f MB), upload 0x%04x]\n",
           w, h, (double)w * h * 4 / 1048576.0, g_up_format);

    double ms0 = bench_frames(0, 60, 3) / 1e6;       /* a：纯上传+finish */
    double ms1k = bench_frames(1000, 60, 3) / 1e6;
    double ms5k = bench_frames(5000, 60, 3) / 1e6;

    double b_ns = (ms5k - ms0) * 1e6 / 5000.0;        /* 每调用边际成本 */
    printf("  fit: a=%.3f ms (upload+finish)  b=%.0f ns/call\n", ms0, b_ns);

    /* 软件对照：整帧 memset（软件光栅带宽地板，零 ecall） */
    double fill_ns = bench_best("C memset frame", 100, loop_fill, 5);
    printf("  software fill floor: %.3f ms/frame (%.1f GB/s, host damage "
           "blit 另计，同地址空间 memcpy)\n",
           fill_ns / 1e6, (double)w * h * 4 / fill_ns);

    /* 判读 */
    printf("  extrapolate: 3000 calls/frame -> %.2f ms ecall tax vs "
           "16.7ms budget\n", 3000.0 * b_ns / 1e6);
    (void)ms1k;
    glDeleteTextures(1, (uint32_t*)&tex);
    free(g_frame);
    g_frame = NULL;
}

/* ---------------- main ---------------- */

int main(void)
{
    int w = 1280, h = 800;
    const char* s = getenv("RVVM_BENCH_SIZE");
    if (s && *s) {
        int vw = 0, vh = 0;
        if (sscanf(s, "%dx%d", &vw, &vh) == 2 && vw > 0 && vh > 0 &&
            vw <= FB_MAX && vh <= FB_MAX) {
            w = vw;
            h = vh;
        }
    }

    printf("=== bench_ecall: ecall overhead microbenchmark ===\n");
    printf("frame sizes: 1280x800 + 1920x1080 (env RVVM_BENCH_SIZE=%dx%d "
           "overrides to single)\n\n", w, h);

    /* EGL bring-up：pbuffer 64x64 即可 —— B/C 阶段不往默认帧缓冲画任何东西，
     * 上传目标是纹理，避免大 pbuffer 撞 host 的 pbuffer 上限。 */
    const EGLint pbuffer_attribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint choose_attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE
    };

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0, dummy = 0;
    EGLConfig config = NULL;
    if (!dpy || !eglInitialize(dpy, &major, &minor) ||
        !eglChooseConfig(dpy, choose_attribs, &config, 1, &dummy) || !config) {
        printf("EGL bring-up FAILED\n");
        return 1;
    }
    EGLSurface surf = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT,
                                      context_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("EGL context FAILED\n");
        return 1;
    }
    printf("GL_VERSION : %s\n", (const char*)glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n\n", (const char*)glGetString(GL_RENDERER));

    /* A 系列：尺寸无关，先跑 */
    printf("[A series] per-call cost\n");
    bench_best("A0 busy loop", 100000, loop_a0_busy, 5);
    double a1 = bench_best("A1 getpid", 100000, loop_getpid, 5);
    double a2 = bench_best("A2 glGetError", 100000, loop_glgeterror, 5);
    printf("  -> bare ecall floor %10.1f ns\n", a1);
    printf("  -> GL forwarding tax (A2-A1) %10.1f ns\n", a2 - a1);

    /* 上传格式探测，然后 B/C 系列 */
    g_up_format = probe_upload_format();
    if (g_up_format) {
        run_frame_suite(w, h);
        if (getenv("RVVM_BENCH_SIZE") == NULL) {
            run_frame_suite(1920, 1080);
        }
    }

    printf("\ndone. 判读：b 为几百 ns => '每帧几千 ecall 比软件光栅慢'不成立；\n"
           "b 为数 us => 论断成立。RVVM_NOJIT=1 重跑取解释器路径下界。\n");
    return 0;
}

