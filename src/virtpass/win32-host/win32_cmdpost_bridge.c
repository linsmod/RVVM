/*
 * win32_cmdpost_bridge.c - Win32 implementation of the vp_cmdpost host
 * callbacks (the layer that jni_bridge.c implements on Android).
 *
 * Guest ABI contract (verified against vp_ndk_stub.c):
 *  - WINDOW_LOCK:  guest passes its own ANativeWindow_Buffer; the host fills
 *                  width/height/stride/format. "bits" stays NULL: the guest
 *                  allocates its own pixel buffer (pixbuf_ensure).
 *  - WINDOW_UNLOCK: a1 = guest pixel pointer; cmdpost translates it to a host
 *                  pointer, the host converts pixels to a DIB and blits them.
 *  - WINDOW_SET_BUF: (width, height, format) geometry change request.
 *  - WINDOW_GET_SIZE: callback fills long* out-params (LLP64 caveat: see README).
 *  - Lifecycle:     queued via cmdpost_queue_lifecycle_cmd(), consumed by the
 *                   guest through SYS_ANDROID_GAME_POLL_CMD.
 *  - Motion events: queued via cmdpost_queue_motion_event(), same polling path.
 *  - Sensors:       stub accelerometer/gyroscope/light pushed every 100 ms
 *                   while the guest has them enabled (handle table matches
 *                   g_sensors[] in vp_ndk_stub.c).
 */

#define WIN32_LEAN_AND_MEAN
// #define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>  /* _access(): launcher guest-existence check */

#include "win32_cmdpost_bridge.h" /* self-protypes for forward refs (launcher) */
#include "virtpass/vp_cmdpost.h"  /* single copy lives in src/virtpass */
#include "core/rvvm_user.h"       /* rvvm_user_linux() guest entry point */
#include "virtpass/vp_android.h"  /* guest ABI constants: APP_CMD_*, WINDOW_FORMAT_*, ASENSOR_TYPE_* */
#include "win32_gl_dispatch.h" /* on_egl_dispatch, on_gl_dispatch, g_gl_active */
#include "win32_gl_backend.h"  /* win32_gl_backend_load/ready/name/unload, w32gl_arg_f */
#include "win32_aaudio_wasapi.h" /* win32_aaudio_ops, win32_aaudio_shutdown */

#define WM_APP_GUEST_EXIT (WM_APP + 1)
#define WM_APP_RESIZE_TO_SURFACE (WM_APP + 2)
#define SENSOR_TIMER_ID   1
#define SENSOR_TIMER_MS   100

/* Launcher (Android-style picker) child control IDs. */
#define IDC_COMBO 101
#define IDC_RUN   102
#define IDC_STOP  103
#define IDC_EXIT  104

/* How long win32_host_shutdown() waits for the guest thread to unwind. Kept
 * below the ~5s budget Windows grants a CTRL_CLOSE_EVENT handler so a
 * console-close shutdown still completes before the OS hard-kills us. */
#define GUEST_EXIT_TIMEOUT_MS 4000

/* ------------------------------------------------------------------ */
/* Host state                                                          */
/* ------------------------------------------------------------------ */

static HWND            g_hwnd       = NULL;
static bool            g_cs_ready   = false;
static CRITICAL_SECTION g_surf_cs;

/* Present surface (BGRA DIB, top-down) */
static HBITMAP         g_dib        = NULL;
static uint8_t*        g_dib_bits   = NULL;
static HBITMAP         g_dib_back   = NULL;  /* back buffer: guest writes here */
static uint8_t*        g_dib_back_bits = NULL;
static BITMAPINFO      g_bmi;

/* Guest surface geometry (as reported to the guest) */
static int32_t g_surf_w   = 0;
static int32_t g_surf_h   = 0;
static int32_t g_surf_fmt = WINDOW_FORMAT_RGBA_8888;
static int32_t g_init_w   = 640;
static int32_t g_init_h   = 480;

/* Layer-2 OS window: pure viewport, its size is independent of the virtual
 * panel. WM_PAINT scales the panel uniformly to fit (contain, never cropped)
 * and centres it, filling the leftover area with black. */
static int32_t g_win_w    = 1024;
static int32_t g_win_h    = 768;

/* Layer-1 virtual display: host-owned panel parameters. The guest observes
 * them ONLY through the public NDK ABI (ANativeWindow_getWidth/Height and
 * AConfiguration_*); no virtpass-specific display ABI is exposed. */
static int32_t g_virt_w   = 1024;
static int32_t g_virt_h   = 768;
static int32_t g_virt_ppi = ACONFIGURATION_DENSITY_MEDIUM;

/* Stub sensor enable state (handle table: vp_ndk_stub.c g_sensors[]) */
static bool g_accel_on = false;  /* handle 0 */
static bool g_gyro_on  = false;  /* handle 2 */
static bool g_light_on = false;  /* handle 3 */

/* Guest thread */
static HANDLE g_guest_thread = NULL;
static int    g_guest_argc   = 0;
static char** g_guest_argv   = NULL;
/* Guest exit code. Written on the guest thread: host_guest_exit_cb() stores
 * the real code at sys_exit time, guest_thread_main() may overwrite it with
 * the rvvm_user_linux() error code if the guest never started. */
static int    g_guest_rc     = -1;

/* Visibility edge (Android foreground/background). true while the OS window is
 * minimized. PAUSE/STOP are emitted only on the visible -> minimized edge and
 * START/RESUME only on the reverse edge, so the lifecycle commands are never
 * spammed by the stream of WM_SIZE messages. */
static bool   g_minimized    = false;

/* ------------------------------------------------------------------ */
/* Launcher (Android-style picker) state                               */
/* ------------------------------------------------------------------ */

#define MAX_GUESTS      64
#define MAX_GUEST_NAME  96
#define LAUNCH_MARGIN   16
#define LAUNCH_CTRL_H   28
#define LAUNCH_CTRL_GAP  8

/* Known sample guests; used when the assets directory holds no .exe files
 * (e.g. the assets haven't been built yet). */
static const char* LAUNCHER_DEFAULT_GUESTS[] = {
    "test_render", "test_game_activity", "test_sensor_guest",
    "test_render_gles", "test_audio",
};

static char g_assets_dir[MAX_PATH] = ".";
static char g_guest_names[MAX_GUESTS][MAX_GUEST_NAME];
static int  g_guest_count   = 0;
static bool g_launcher      = false; /* launcher UI enabled (only when no
                                        guest is given on the command line) */
static bool g_launcher_idle = false; /* picker shown, no guest running */
static HWND g_combo     = NULL;      /* guest dropdown        */
static HWND g_btn_run   = NULL;      /* Run  button           */
static HWND g_btn_stop  = NULL;      /* Stop button           */
static HWND g_btn_exit  = NULL;      /* Exit button           */

/* Off-screen composition surface for WM_PAINT. The whole client area is
 * composed here (black letterbox + uniformly scaled panel + boundary frame)
 * and blitted to the window DC in a single pass. Painting the background and
 * then the panel straight onto the window DC in separate steps means the
 * cleared area is on screen while the (possibly stretching) panel blit is
 * still in flight - visible as a flash every frame. Recreated only when the
 * client size changes. */
static HDC     g_cmp_dc  = NULL;
static HBITMAP g_cmp_bm  = NULL;
static HGDIOBJ g_cmp_old = NULL;
static int32_t g_cmp_w   = 0;
static int32_t g_cmp_h   = 0;

/* Caller holds g_surf_cs */
static void cmp_surface_release_locked(void)
{
    if (g_cmp_dc) {
        if (g_cmp_old) SelectObject(g_cmp_dc, g_cmp_old);
        DeleteDC(g_cmp_dc);
    }
    if (g_cmp_bm) DeleteObject(g_cmp_bm);
    g_cmp_dc  = NULL;
    g_cmp_bm  = NULL;
    g_cmp_old = NULL;
    g_cmp_w   = 0;
    g_cmp_h   = 0;
}

/* Caller holds g_surf_cs */
static HDC cmp_surface_get_locked(HDC ref, int32_t w, int32_t h)
{
    HDC     dc;
    HBITMAP bm;

    if (g_cmp_dc && g_cmp_w == w && g_cmp_h == h) return g_cmp_dc;

    cmp_surface_release_locked();
    dc = CreateCompatibleDC(ref);
    bm = dc ? CreateCompatibleBitmap(ref, w, h) : NULL;
    if (!dc || !bm) {
        if (bm) DeleteObject(bm);
        if (dc) DeleteDC(dc);
        return NULL;
    }
    g_cmp_old = SelectObject(dc, bm);
    g_cmp_dc  = dc;
    g_cmp_bm  = bm;
    g_cmp_w   = w;
    g_cmp_h   = h;
    return g_cmp_dc;
}

static void winhost_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[winhost %10llu ms] ", (unsigned long long)GetTickCount64());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Phase 4: Win32 vsync clock                                          */
/* ------------------------------------------------------------------ */
/*
 * The guest's AChoreographer needs one tick per display frame. Windows has no
 * public vblank event, but DwmFlush() is the documented "wait until the
 * compositor has presented the pending frame" call: it blocks for roughly one
 * refresh interval whenever the window has something to present.
 *
 * Both guest paths are driven from the same clock:
 *  - 方案 A (blocking WAIT): on_choreographer_wait() paces one frame and
 *    returns the monotonic frame time the stub compares against its due_ns.
 *  - 方案 B (fd wakeup): the clock thread runs at the refresh rate and calls
 *    vp_cmdpost_vsync_tick(), which writes the frame time into the pipe the
 *    guest's Looper is blocked in poll() on.
 *
 * DwmFlush is resolved dynamically so the host still runs (with plain Sleep
 * pacing) where dwmapi or composition is unavailable.
 */
typedef HRESULT (WINAPI* dwm_flush_fn)(void);

static dwm_flush_fn  g_dwm_flush    = NULL;
static HANDLE        g_vsync_thread = NULL;
static volatile LONG g_vsync_run    = 0;
static int64_t       g_vsync_period_ns = 16666667LL;  /* 60 Hz fallback */

/* Monotonic nanoseconds. rvvm-user serves the guest's clock_gettime() syscall
 * from clock_gettime() in this very process, so using the same call keeps the
 * frame times we hand the guest comparable with its own CLOCK_MONOTONIC. */
static int64_t host_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void vsync_detect_rate(void)
{
    HDC hdc = GetDC(NULL);
    int hz  = hdc ? GetDeviceCaps(hdc, VREFRESH) : 0;
    if (hdc) {
        ReleaseDC(NULL, hdc);
    }
    if (hz < 24 || hz > 480) {
        hz = 60;  /* 0/1 means "hardware default" */
    }
    g_vsync_period_ns = 1000000000LL / hz;
}

/* Wait for one display frame and return its monotonic timestamp. DwmFlush may
 * return immediately when nothing is pending, so the period grid provides the
 * lower bound that keeps the cadence at the refresh rate. */
static int64_t vsync_wait_frame(void)
{
    int64_t period = g_vsync_period_ns;
    int64_t before = host_now_ns();
    int64_t next   = ((before / period) + 1) * period;
    int64_t now;

    if (g_dwm_flush) {
        g_dwm_flush();
    }

    now = host_now_ns();
    if (now < next) {
        DWORD ms = (DWORD)((next - now) / 1000000);
        if (ms) {
            Sleep(ms);
        }
        now = host_now_ns();
        if (now < next) {
            now = next;
        }
    }
    return now;
}

static int64_t on_choreographer_wait(void)
{
    return vsync_wait_frame();
}

static DWORD WINAPI vsync_thread_main(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_vsync_run, 1, 1) == 1) {
        int64_t t = vsync_wait_frame();
        if (InterlockedCompareExchange(&g_vsync_run, 1, 1) != 1) {
            break;
        }
        vp_cmdpost_vsync_tick(t);
    }
    return 0;
}

static void vsync_clock_start(void)
{
    HMODULE m;

    if (g_vsync_thread) {
        return;
    }

    m = LoadLibraryA("dwmapi.dll");
    if (m) {
        g_dwm_flush = (dwm_flush_fn)(void*)GetProcAddress(m, "DwmFlush");
    }
    vsync_detect_rate();

    InterlockedExchange(&g_vsync_run, 1);
    g_vsync_thread = CreateThread(NULL, 0, vsync_thread_main, NULL, 0, NULL);
    if (!g_vsync_thread) {
        InterlockedExchange(&g_vsync_run, 0);
        winhost_log("vsync clock: CreateThread failed, guest paces itself");
        return;
    }

    winhost_log("vsync clock: %d Hz via %s",
                (int)(1000000000LL / g_vsync_period_ns),
                g_dwm_flush ? "DwmFlush" : "Sleep pacing");
}

static void vsync_clock_stop(void)
{
    if (!g_vsync_thread) {
        return;
    }

    /* Wake a guest parked in poll() and let it degrade: it would otherwise wait
     * for a frame that will never come. */
    vp_cmdpost_vsync_source_lost();

    InterlockedExchange(&g_vsync_run, 0);
    if (WaitForSingleObject(g_vsync_thread, 3000) == WAIT_OBJECT_0) {
        CloseHandle(g_vsync_thread);
    }
    else {
        /* DwmFlush cannot be interrupted: rather than risk the thread touching a
         * torn-down cmdpost, let it observe g_vsync_run and exit on its own. */
        winhost_log("vsync clock: thread did not stop in time");
    }
    g_vsync_thread = NULL;
}

/* ------------------------------------------------------------------ */
/* Surface plumbing                                                    */
/* ------------------------------------------------------------------ */

static int surf_bpp(void)
{
    return (g_surf_fmt == WINDOW_FORMAT_RGB_565) ? 2 : 4;
}

/* Caller holds g_surf_cs */
static void surf_recreate_locked(int32_t w, int32_t h)
{
    void* bits = NULL;

    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = NULL;
        g_dib_bits = NULL;
    }
    if (g_dib_back) {
        DeleteObject(g_dib_back);
        g_dib_back = NULL;
        g_dib_back_bits = NULL;
    }
    if (w <= 0 || h <= 0) return;

    ZeroMemory(&g_bmi, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth       = w;
    g_bmi.bmiHeader.biHeight      = -h; /* top-down rows */
    g_bmi.bmiHeader.biPlanes      = 1;
    g_bmi.bmiHeader.biBitCount    = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    {
        HDC hdc = GetDC(NULL);
        g_dib = CreateDIBSection(hdc, &g_bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_dib_bits = (uint8_t*)bits;
        bits = NULL;
        /* Back buffer: the guest renders into this one; present swaps it in */
        g_dib_back = CreateDIBSection(hdc, &g_bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_dib_back_bits = (uint8_t*)bits;
        ReleaseDC(NULL, hdc);
    }
    if (!g_dib_bits || !g_dib_back_bits) winhost_log("CreateDIBSection(%dx%d) failed", (int)w, (int)h);
}

/* Present is done exclusively on the UI thread in WM_PAINT; the guest
 * thread only converts frames into the back buffer and flips. */

/* Guest RGBA/RGBX/565 row -> DIB BGRA row */
static void convert_row(uint8_t* dst, const uint8_t* src, int32_t w, int32_t fmt)
{
    int32_t i;
    if (fmt == WINDOW_FORMAT_RGB_565) {
        for (i = 0; i < w; i++) {
            uint16_t p = (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
            uint8_t r = (uint8_t)((((p >> 11) & 0x1F) * 255) / 31);
            uint8_t g = (uint8_t)((((p >> 5) & 0x3F) * 255) / 63);
            uint8_t b = (uint8_t)(((p & 0x1F) * 255) / 31);
            dst[i * 4 + 0] = b;
            dst[i * 4 + 1] = g;
            dst[i * 4 + 2] = r;
            dst[i * 4 + 3] = 0xFF;
        }
    } else {
        /* WINDOW_FORMAT_RGBA_8888 / RGBX_8888: bytes are R,G,B,A */
        for (i = 0; i < w; i++) {
            dst[i * 4 + 0] = src[i * 4 + 2];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 0];
            dst[i * 4 + 3] = 0xFF;
        }
    }
}

/* ------------------------------------------------------------------ */
/* vp_cmdpost callbacks (invoked on the guest thread via dispatch)     */
/* ------------------------------------------------------------------ */

static void present_frame_impl(const uint8_t* rows, int32_t w, int32_t h,
                           int32_t src_fmt, int32_t src_bpp,
                           bool rows_bottom_up)
{
    (void)src_fmt; /* conversion always uses the current surface format */
    EnterCriticalSection(&g_surf_cs);
    if (src_bpp <= 0) src_bpp = surf_bpp(); /* 0: source matches the surface */
    if (!rows || !g_dib_back_bits || g_surf_w != w || g_surf_h != h) {
        LeaveCriticalSection(&g_surf_cs);
        return;
    }
    {
        /* The source pitch belongs to the caller's buffer. Hard-coding four
         * bytes per pixel (as this used to) silently mis-strides RGB_565
         * frames; the GL path always hands over 4-byte RGBA and says so. */
        size_t src_pitch = (size_t)w * (size_t)src_bpp;
        size_t dst_pitch = (size_t)w * 4u;
        for (int32_t y = 0; y < h; y++) {
            int32_t sy = rows_bottom_up ? (h - 1 - y) : y;
            convert_row(g_dib_back_bits + (size_t)y * dst_pitch,
                        rows + (size_t)sy * src_pitch,
                        w, g_surf_fmt);
        }
    }
    { HBITMAP tb = g_dib; uint8_t* tp = g_dib_bits;
      g_dib = g_dib_back; g_dib_bits = g_dib_back_bits;
      g_dib_back = tb; g_dib_back_bits = tp; }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    LeaveCriticalSection(&g_surf_cs);
}

void present_frame(const uint8_t* rows, int32_t w, int32_t h,
                    int32_t src_fmt, bool rows_bottom_up)
{
    present_frame_impl(rows, w, h, src_fmt, 0, rows_bottom_up);
}

void present_gl_frame(void)
{
    if (!g_gl_active || !g_dib_back_bits) return;
    static uint8_t* rb = NULL; static int32_t rb_w = 0, rb_h = 0;
    EnterCriticalSection(&g_surf_cs);
    if (rb_w != g_surf_w || rb_h != g_surf_h) {
        free(rb);
        rb_w = g_surf_w; rb_h = g_surf_h;
        if (rb_w > 0 && rb_h > 0) {
            rb = (uint8_t*)malloc((size_t)rb_w * (size_t)rb_h * 4);
        }
    }
    int32_t w = g_surf_w, h = g_surf_h;
    LeaveCriticalSection(&g_surf_cs);

    if (!rb || w <= 0 || h <= 0) return;
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    present_frame_impl(rb, w, h, WINDOW_FORMAT_RGBA_8888, 4, true);
}

static int32_t on_window_lock(void* window, void* outBuffer, void* dirtyBounds)
{
    cmdpost_ANativeWindow_Buffer* buf = (cmdpost_ANativeWindow_Buffer*)outBuffer;
    static bool logged_first = false;
    (void)window;
    (void)dirtyBounds;
    if (!buf) {
        winhost_log("WINDOW_LOCK with NULL outBuffer -> rejected");
        return -1;
    }

    EnterCriticalSection(&g_surf_cs);
    if (g_surf_w <= 0) {
        /* First lock before any SET_BUF: default to the window size */
        g_surf_w = g_init_w;
        g_surf_h = g_init_h;
    }
    if (!g_dib_bits) surf_recreate_locked(g_surf_w, g_surf_h);
    buf->width  = g_surf_w;
    buf->height = g_surf_h;
    buf->stride = g_surf_w;    /* pixels, matches guest expectation */
    buf->format = g_surf_fmt;
    buf->bits   = NULL;        /* guest allocates its own buffer (pixbuf_ensure) */
    if (!logged_first) {
        logged_first = true;
        winhost_log("WINDOW_LOCK ok: %dx%d stride=%d fmt=%d "
                    "(guest renders into its own pixbuf; bits stays NULL)",
                    buf->width, buf->height, buf->stride, buf->format);
    }
    LeaveCriticalSection(&g_surf_cs);
    return 0;
}

static int32_t on_window_unlock(void* window, void* guestPixels)
{
    static bool logged_frame = false;
    int32_t w = 0, h = 0, fmt = 0;
    (void)window;
    if (!guestPixels) {
        /* UNLOCK with a null pixel pointer is the fingerprint of the guest
         * failing to allocate its own pixbuf after a *successful* LOCK
         * (pixbuf_ensure in vp_ndk_stub.c). The window itself is fine; say so
         * instead of silently posting nothing. */
        winhost_log("WINDOW_UNLOCK with NULL pixels: guest pixbuf unavailable "
                    "(lock succeeded, no frame posted)");
    }
    EnterCriticalSection(&g_surf_cs);
    w   = g_surf_w;
    h   = g_surf_h;
    fmt = g_surf_fmt;
    LeaveCriticalSection(&g_surf_cs);

    if (guestPixels && !logged_frame) {
        /* Positive confirmation that the whole LOCK -> render -> UNLOCK path
         * works, i.e. the guest did get its pixel buffer. */
        logged_frame = true;
        winhost_log("first frame posted: %dx%d fmt=%d from guest pixbuf %p", w, h, fmt, guestPixels);
    }

    /* Conversion, buffer swap and invalidate all live in present_frame(). The
     * old code converted the frame to BGRA here *and* again inside
     * present_frame(), i.e. every single frame was converted twice. */
    present_frame(guestPixels ? (const uint8_t*)guestPixels : NULL,
                  guestPixels ? w : 0,
                  guestPixels ? h : 0,
                  fmt, false);
    return 0;
}

static void on_window_size(int64_t* width, int64_t* height)
{
    if (!width || !height) return;
    EnterCriticalSection(&g_surf_cs);
    if (g_surf_w > 0 && g_surf_h > 0) {
        *width  = (int64_t)g_surf_w;
        *height = (int64_t)g_surf_h;
    } else {
        *width  = (int64_t)g_init_w;
        *height = (int64_t)g_init_h;
    }
    LeaveCriticalSection(&g_surf_cs);
}

static int32_t on_window_set_buf(int32_t width, int32_t height, int32_t format)
{
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return -1;
    if (format != WINDOW_FORMAT_RGBA_8888 &&
        format != WINDOW_FORMAT_RGBX_8888 &&
        format != WINDOW_FORMAT_RGB_565) return -1;

    EnterCriticalSection(&g_surf_cs);
    g_surf_w   = width;
    g_surf_h   = height;
    g_surf_fmt = format;
    surf_recreate_locked(width, height);
    LeaveCriticalSection(&g_surf_cs);
    return 0;
}

/* Quantise an arbitrary physical PPI to the nearest public NDK density
 * bucket. The exact PPI (virt_ppi) stays host-private: the public NDK only
 * exposes these quantised buckets to the guest. */
static int32_t density_bucket_for_ppi(int32_t ppi)
{
    static const int32_t buckets[] = {
        ACONFIGURATION_DENSITY_LOW,     /* 120 */
        ACONFIGURATION_DENSITY_MEDIUM,  /* 160 */
        ACONFIGURATION_DENSITY_TV,      /* 213 */
        ACONFIGURATION_DENSITY_HIGH,    /* 240 */
        ACONFIGURATION_DENSITY_XHIGH,   /* 320 */
        ACONFIGURATION_DENSITY_XXHIGH,  /* 480 */
        ACONFIGURATION_DENSITY_XXXHIGH, /* 640 */
    };
    size_t i, best = 0;
    int32_t best_d = -1;

    if (ppi <= 0) return ACONFIGURATION_DENSITY_MEDIUM;
    for (i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
        int32_t d = ppi - buckets[i];
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return buckets[best];
}

static int32_t on_config_get(int32_t field, int32_t* outValue)
{
    int32_t w, h, ppi, width_dp, height_dp, long_dp, short_dp;

    if (!outValue) return -1;

    EnterCriticalSection(&g_surf_cs);
    w   = (g_surf_w > 0) ? g_surf_w : g_virt_w;
    h   = (g_surf_h > 0) ? g_surf_h : g_virt_h;
    ppi = (g_virt_ppi > 0) ? g_virt_ppi : ACONFIGURATION_DENSITY_MEDIUM;
    LeaveCriticalSection(&g_surf_cs);

    width_dp  = w * 160 / ppi;
    height_dp = h * 160 / ppi;
    long_dp   = (width_dp >= height_dp) ? width_dp : height_dp;
    short_dp  = (width_dp >= height_dp) ? height_dp : width_dp;

    switch (field) {
    case VP_ACONFIG_QUERY_ORIENTATION:
        *outValue = (w >= h) ? ACONFIGURATION_ORIENTATION_LAND
                             : ACONFIGURATION_ORIENTATION_PORT;
        return 0;
    case VP_ACONFIG_QUERY_DENSITY:
        *outValue = density_bucket_for_ppi(ppi);
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_SIZE:
        *outValue = (short_dp < 320) ? ACONFIGURATION_SCREENSIZE_SMALL
                  : (short_dp < 480) ? ACONFIGURATION_SCREENSIZE_NORMAL
                  : (short_dp < 720) ? ACONFIGURATION_SCREENSIZE_LARGE
                                     : ACONFIGURATION_SCREENSIZE_XLARGE;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_LONG:
        *outValue = (long_dp * 5 >= short_dp * 8) ? ACONFIGURATION_SCREENLONG_YES
                                                  : ACONFIGURATION_SCREENLONG_NO;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_ROUND:
        *outValue = ACONFIGURATION_SCREENROUND_NO;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_WIDTH_DP:
        *outValue = width_dp;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP:
        *outValue = height_dp;
        return 0;
    default:
        return -1;
    }
}

static void on_sensor_init(void)
{
    winhost_log("sensor system initialized (stub sensors)");
}

/* Handle values match g_sensors[] in vp_ndk_stub.c */
static void on_sensor_enable(int handle, bool enable)
{
    switch (handle) {
    case 0: g_accel_on = enable; break; /* ASENSOR_TYPE_ACCELEROMETER */
    case 2: g_gyro_on  = enable; break; /* ASENSOR_TYPE_GYROSCOPE     */
    case 3: g_light_on = enable; break; /* ASENSOR_TYPE_LIGHT         */
    default: break;                     /* others: accepted, stubbed no-op */
    }
    winhost_log("sensor enable: handle=%d enable=%d", handle, enable ? 1 : 0);
}

static void on_sensor_data(sensor_event_t* event)
{
    (void)event; /* host does not consume sensor data */
}

static void on_game_lifecycle(int32_t cmd)
{
    /* Mirrors jni_bridge.c: log only; real path is the queued commands */
    winhost_log("guest lifecycle callback: cmd=%d", cmd);
}

static void on_game_input(void* motionEvent)
{
    (void)motionEvent;
    winhost_log("guest input callback");
}

/* ------------------------------------------------------------------ */
/* Stub sensor feeding                                                 */
/* ------------------------------------------------------------------ */

static void push_stub_sensor(int32_t type, float x, float y, float z)
{
    sensor_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.version   = 1;
    ev.type      = type;
    ev.timestamp = (int64_t)GetTickCount64() * 1000000LL;
    ev.vector.x  = x;
    ev.vector.y  = y;
    ev.vector.z  = z;
    cmdpost_push_sensor_event(&ev);
}

static void feed_stub_sensors(void)
{
    if (g_accel_on) push_stub_sensor(ASENSOR_TYPE_ACCELEROMETER, 0.0f, 0.0f, 9.81f);
    if (g_gyro_on)  push_stub_sensor(ASENSOR_TYPE_GYROSCOPE, 0.0f, 0.0f, 0.0f);
    if (g_light_on) push_stub_sensor(ASENSOR_TYPE_LIGHT, 0.0f, 0.0f, 0.0f);
}

/* ------------------------------------------------------------------ */
/* Viewport mapping (layer 2 -> layer 1)                               */
/* ------------------------------------------------------------------ */

/* Contain-fit: scale the virtual panel uniformly to the largest size that
 * fits inside the window client area, then centre it. The panel is never
 * cropped and never distorted; the leftover area is painted black. */
typedef struct { int dx, dy, dw, dh; } vp_view;

static void viewport_fit(int cw, int ch, int sw, int sh, vp_view* v)
{
    double s, sx, sy;

    v->dx = v->dy = v->dw = v->dh = 0;
    if (cw <= 0 || ch <= 0 || sw <= 0 || sh <= 0) return;

    sx = (double)cw / (double)sw;
    sy = (double)ch / (double)sh;
    s  = (sx < sy) ? sx : sy;
    v->dw = (int)((double)sw * s + 0.5);
    v->dh = (int)((double)sh * s + 0.5);
    if (v->dw < 1) v->dw = 1;
    if (v->dh < 1) v->dh = 1;
    v->dx = (cw - v->dw) / 2;
    v->dy = (ch - v->dh) / 2;
}

/* ------------------------------------------------------------------ */
/* Input translation                                                   */
/* ------------------------------------------------------------------ */

static void queue_mouse_motion(int action, LPARAM lp)
{
    RECT rc;
    int cw, ch, sw, sh;
    float x, y;
    vp_view v;
    cmdpost_GameActivityMotionEvent ev;

    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return;
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    EnterCriticalSection(&g_surf_cs);
    sw = (g_surf_w > 0) ? g_surf_w : g_init_w;
    sh = (g_surf_h > 0) ? g_surf_h : g_init_h;
    LeaveCriticalSection(&g_surf_cs);

    /* Undo the letterbox transform: window pixel -> panel pixel */
    viewport_fit(cw, ch, sw, sh, &v);
    if (v.dw <= 0 || v.dh <= 0) return;
    x = ((float)(short)LOWORD(lp) - (float)v.dx) * (float)sw / (float)v.dw;
    y = ((float)(short)HIWORD(lp) - (float)v.dy) * (float)sh / (float)v.dh;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > (float)(sw - 1)) x = (float)(sw - 1);
    if (y > (float)(sh - 1)) y = (float)(sh - 1);

    memset(&ev, 0, sizeof(ev));
    ev.eventTime        = (int64_t)GetTickCount64() * 1000000LL;
    ev.deviceId         = 0;
    ev.source           = AINPUT_SOURCE_TOUCHSCREEN;
    ev.action           = action;
    ev.pointerCount     = 1;
    ev.pointers[0].x        = x;
    ev.pointers[0].y        = y;
    ev.pointers[0].rawX     = x;
    ev.pointers[0].rawY     = y;
    ev.pointers[0].pressure = 1.0f;
    ev.pointers[0].size     = 1.0f;
    ev.pointers[0].id       = 0;
    ev.pointers[0].toolType = 1; /* AMOTION_EVENT_TOOL_TYPE_FINGER */
    cmdpost_queue_motion_event(&ev);
}

static void log_key(const char* what, UINT vk, LPARAM lp)
{
    winhost_log("key %s: VK=0x%02X scan=%u repeat=%u",
                what, (unsigned)vk,
                (unsigned)((lp >> 16) & 0xFF),
                (unsigned)(lp & 0xFFFF));
}

/* ------------------------------------------------------------------ */
/* Lifecycle mapping                                                   */
/* ------------------------------------------------------------------ */

static void queue_lifecycle(int32_t cmd)
{
    cmdpost_queue_lifecycle_cmd(cmd);
    winhost_log("lifecycle -> cmd=%d", cmd);
}

static void set_title(const char* suffix)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "RVVM WinHost - %s", suffix);
    if (g_hwnd) SetWindowTextA(g_hwnd, buf);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

/* Launcher helpers are defined further down (Public API section) but win_proc
 * calls them from WM_COMMAND / WM_APP_GUEST_EXIT. Forward declarations. */
static void launcher_ui_idle(void);
static void launcher_launch(void);
static void launcher_stop(void);

static LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    /*
     * Architecture note: the Win32 window is ONLY a presentation surface
     * (screen emulation). The Android lifecycle emulation is a separate
     * state machine below: it sends the startup sequence once and the
     * teardown sequence on real exit. Host-side focus/activation noise
     * (WM_ACTIVATE/WM_SETFOCUS/WM_KILLFOCUS) intentionally does NOT map
     * to PAUSE/RESUME - treating cosmetic focus changes as an Android
     * app going background makes the guest tear down and rebuild its
     * surface, which shows up as the window visibly flashing.
     */
    case WM_CREATE:
        /* Android GameActivity startup order. Skipped while the launcher is
         * idle - there is no guest to consume them yet; the launcher queues
         * them when it boots a guest. */
        if (!g_launcher) {
            queue_lifecycle(APP_CMD_START);
            queue_lifecycle(APP_CMD_INIT_WINDOW);
            queue_lifecycle(APP_CMD_RESUME);
            queue_lifecycle(APP_CMD_GAINED_FOCUS);
        }
        return 0;

    case WM_COMMAND:
        /* Launcher child controls (dropdown / buttons) notify their parent
         * window via WM_COMMAND. Ignored when not in launcher mode. */
        if (!g_launcher) return DefWindowProcA(hwnd, msg, wParam, lParam);
        switch (LOWORD(wParam)) {
        case IDC_RUN:  launcher_launch();       return 0;
        case IDC_STOP: launcher_stop();         return 0;
        case IDC_EXIT: PostMessageA(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HDC cdc;
        GetClientRect(hwnd, &rc);

        /* Launcher idle view: no guest is running, so instead of the guest
         * panel paint an opaque background behind the picker controls. Child
         * controls (combo + buttons) are separate windows drawn on top. */
        if (g_launcher_idle) {
            RECT t = rc;
            HBRUSH bg = CreateSolidBrush(RGB(16, 16, 24));
            HPEN   br = CreatePen(PS_SOLID, 0, RGB(90, 90, 110));
            HGDIOBJ ob, op;
            t.top += LAUNCH_MARGIN;
            t.bottom = t.top + 24;
            FillRect(wdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(wdc, TRANSPARENT);
            SetTextColor(wdc, RGB(240, 240, 240));
            DrawTextA(wdc, "RVVM WinHost - pick a guest, then Run", -1,
                      &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            op = SelectObject(wdc, br);
            ob = SelectObject(wdc, (HGDIOBJ)GetStockObject(NULL_BRUSH));
            Rectangle(wdc, 0, 0, rc.right, rc.bottom);
            SelectObject(wdc, ob);
            SelectObject(wdc, op);
            DeleteObject(br);
            EndPaint(hwnd, &ps);
            return 0;
        }

        EnterCriticalSection(&g_surf_cs);
        /* Compose the whole client area off-screen and blit it in one pass.
         * Filling the background and then drawing the panel directly onto the
         * window DC exposes the cleared area while the (stretching) panel blit
         * is still in flight, which shows up as a flash on every frame. */
        cdc = (rc.right > 0 && rc.bottom > 0)
            ? cmp_surface_get_locked(wdc, rc.right, rc.bottom) : NULL;
        if (cdc) {
            /* Letterbox background: the whole client area is black, the panel
             * goes on top inside the centred contain-fit rectangle. */
            FillRect(cdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (g_dib && g_surf_w > 0 && g_surf_h > 0) {
                /* The window is just a viewport onto the virtual panel: scale
                 * it uniformly to fit and centre it. The panel is never
                 * cropped and never distorted; window size and panel size are
                 * decoupled. */
                vp_view v;
                HDC mdc;
                HGDIOBJ old;
                viewport_fit(rc.right, rc.bottom, g_surf_w, g_surf_h, &v);
                mdc = CreateCompatibleDC(cdc);
                old = SelectObject(mdc, g_dib);
                if (v.dw == g_surf_w && v.dh == g_surf_h) {
                    BitBlt(cdc, v.dx, v.dy, v.dw, v.dh, mdc, 0, 0, SRCCOPY);
                } else {
                    SetStretchBltMode(cdc, HALFTONE);
                    SetBrushOrgEx(cdc, 0, 0, NULL);
                    StretchBlt(cdc, v.dx, v.dy, v.dw, v.dh,
                               mdc, 0, 0, g_surf_w, g_surf_h, SRCCOPY);
                }
                SelectObject(mdc, old);
                DeleteDC(mdc);

                /* Mark the virtual display region with a red frame so the
                 * letterboxed panel boundary inside the OS window is visible. */
                {
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                    HGDIOBJ old_pen = SelectObject(cdc, pen);
                    HGDIOBJ old_brush = SelectObject(cdc, GetStockObject(NULL_BRUSH));
                    Rectangle(cdc, v.dx, v.dy, v.dx + v.dw, v.dy + v.dh);
                    SelectObject(cdc, old_brush);
                    SelectObject(cdc, old_pen);
                    DeleteObject(pen);
                }
            }
            BitBlt(wdc, 0, 0, rc.right, rc.bottom, cdc, 0, 0, SRCCOPY);
        }
        LeaveCriticalSection(&g_surf_cs);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* avoid flicker, WM_PAINT covers the client area */

    case WM_ACTIVATE:
        /* Deliberately NOT mapped to PAUSE/RESUME: see lifecycle note above */
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        /* Deliberately NOT mapped to GAINED/LOST_FOCUS: see note above */
        return 0;

    case WM_SIZE:
        /* Visibility edge: the OS window is the activity's foreground/background
         * signal (WM_ACTIVATE is deliberately ignored, see the note above).
         * Minimizing is an Android "gone to background", NOT a destroy: the
         * activity stays alive (STOP), so we pause it and stop the vsync source
         * instead of letting a hidden window keep rendering at full speed. */
        if (wParam == SIZE_MINIMIZED) {
            if (!g_minimized) {
                g_minimized = true;
                queue_lifecycle(APP_CMD_PAUSE);
                queue_lifecycle(APP_CMD_STOP);
                vsync_clock_stop();
            }
            return 0;
        }

        if (g_minimized) {
            /* Foreground again: re-arm the vsync source before resuming so the
             * clock is running when the guest comes back. The guest re-probes
             * the fd-wakeup path on its next frame (cmdpost_set_choreographer_
             * callback clears the source-lost flag the stop above raised). */
            g_minimized = false;
            cmdpost_set_choreographer_callback(on_choreographer_wait);
            vsync_clock_start();
            queue_lifecycle(APP_CMD_START);
            queue_lifecycle(APP_CMD_RESUME);
        } else {
            queue_lifecycle(APP_CMD_WINDOW_RESIZED);
        }

        /* The window is just a viewport onto the fixed-size virtual panel, so on
         * resize we only need to repaint: the panel is re-scaled to the new
         * client area (contain fit). Invalidate the whole client area so WM_PAINT
         * redraws immediately instead of leaving the freshly exposed region
         * stale/unpainted. */
        if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetCapture(hwnd);
        queue_mouse_motion(AMOTION_EVENT_ACTION_DOWN, lParam);
        return 0;

    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON) {
            queue_mouse_motion(AMOTION_EVENT_ACTION_MOVE, lParam);
        }
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        queue_mouse_motion(AMOTION_EVENT_ACTION_UP, lParam);
        return 0;

    case WM_KEYDOWN:
        log_key("down", (UINT)wParam, lParam);
        return 0;

    case WM_KEYUP:
        log_key("up", (UINT)wParam, lParam);
        return 0;

    case WM_TIMER:
        if (wParam == SENSOR_TIMER_ID) feed_stub_sensors();
        return 0;

    case WM_APP_GUEST_EXIT:
        /* Release the finished guest thread handle: launcher_launch() guards
         * on g_guest_thread, so keeping it would silently block the next Run
         * in the picker. The thread has posted its final message and only
         * unwinds from here, which does not need our handle. */
        if (g_guest_thread) {
            CloseHandle(g_guest_thread);
            g_guest_thread = NULL;
        }
        {
            char title[48];
            snprintf(title, sizeof(title), "guest exited (%d)", g_guest_rc);
            set_title(title);
        }
        winhost_log("guest exited with code %d", g_guest_rc);
        /* Guest is gone: no one will poll lifecycle cmds anymore. Tear down
         * the same way as WM_CLOSE (Android teardown order). */
        queue_lifecycle(APP_CMD_PAUSE);
        queue_lifecycle(APP_CMD_STOP);
        queue_lifecycle(APP_CMD_DESTROY);
        if (g_launcher) {
            /* Launcher mode: do NOT close the window. Reset the guest-owned
             * surface and re-show the picker so another guest can be booted
             * in the same window. */
            launcher_ui_idle();
            return 0;
        }
        /* Non-launcher mode: the host has no reason to keep running - quit
         * the message loop like WM_CLOSE. */
        DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        /* Real finish: DESTROY is the only command that ends the activity.
         * PAUSE/STOP are now visibility-driven (WM_SIZE above), so we only add
         * them here when the activity is still foreground, to keep the Android
         * teardown order onPause -> onStop -> onDestroy. When the window was
         * already minimized those two were sent on the way down and DESTROY
         * alone is correct. */
        if (!g_minimized) {
            queue_lifecycle(APP_CMD_PAUSE);
            queue_lifecycle(APP_CMD_STOP);
        }
        queue_lifecycle(APP_CMD_DESTROY);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        queue_lifecycle(APP_CMD_TERM_WINDOW);
        /* The window is gone: drop the composition surface with it. Guarded on
         * g_cs_ready because teardown order is not guaranteed if init failed. */
        if (g_cs_ready) {
            EnterCriticalSection(&g_surf_cs);
            cmp_surface_release_locked();
            LeaveCriticalSection(&g_surf_cs);
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

/* Console control handler. Closing the console window (or Ctrl+C / Ctrl+Break)
 * must follow the exact same graceful path as closing the OS window. Without a
 * handler, Windows' default is to terminate the process outright (CSRSS ->
 * TerminateProcess): the guest never sees PAUSE/STOP/DESTROY, the vsync clock
 * is never stopped and the guest thread is killed mid-flight.
 *
 * This callback runs on a dedicated console thread with a ~5s budget before the
 * OS hard-kills the process, so it only *posts* WM_CLOSE and lets the UI thread
 * run the ordered teardown (lifecycle -> DestroyWindow -> PostQuitMessage). */
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_hwnd) {
            PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
            return TRUE; /* handled: do not fall back to TerminateProcess */
        }
        /* Window already gone (message loop exited): nothing left to drain. */
        return FALSE;
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Guest thread                                                        */
/* ------------------------------------------------------------------ */

/* rvvm exit hook (rvvm_user_set_exit_callback). Fires on the guest vCPU
 * thread the instant the guest calls sys_exit / sys_exit_group, while
 * rvvm_user_linux() is still unwinding the other vCPUs - so this must not
 * touch cmdpost, the surface or the window; the ordered teardown runs later
 * in WM_APP_GUEST_EXIT.
 *
 * Registering it is load-bearing: with no callback registered, rvvm_user.c
 * falls back to _Exit(exit_code) inside the syscall path, which terminates
 * the whole WinHost process from the guest thread - guest_thread_main would
 * never post WM_APP_GUEST_EXIT and the launcher could never return to the
 * picker (and Stop would take the window down with the guest). */
static void host_guest_exit_cb(int exit_code)
{
    /* rvvm_user_linux() itself always returns 0 on a guest-driven exit, so
     * this callback is the only source of the real exit code. */
    g_guest_rc = exit_code;
    winhost_log("guest exit callback: code %d", exit_code);
}

static DWORD WINAPI guest_thread_main(LPVOID arg)
{
    static char* envp[] = { NULL };
    static char env_prefix[] = "RVVM_USER_PREFIX="; /* putenv needs a persistent string */
    (void)arg;

    /* Same pattern as jni_bridge.c: assets resolve relative to CWD */
    putenv(env_prefix);

    {
        int rc = rvvm_user_linux(g_guest_argc, g_guest_argv, envp);
        /* 0 = guest-driven exit: host_guest_exit_cb() already recorded the
         * real exit code. Nonzero = the guest never started (ELF load
         * failure), the callback never fired, so propagate the error. */
        if (rc != 0) {
            g_guest_rc = rc;
        }
    }

    if (g_guest_argv) {
        int i;
        for (i = 0; i < g_guest_argc; i++) free(g_guest_argv[i]);
        free(g_guest_argv);
        g_guest_argv = NULL;
        g_guest_argc = 0;
    }

    PostMessage(g_hwnd, WM_APP_GUEST_EXIT, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void launcher_scan_guests(void)
{
    char pattern[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    g_guest_count = 0;

    snprintf(pattern, sizeof(pattern), "%s\\*.exe", g_assets_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char* ext = strrchr(fd.cFileName, '.');
            if (ext) *ext = '\0';               /* strip ".exe" */
            if (fd.cFileName[0] != '\0' && n < MAX_GUESTS) {
                snprintf(g_guest_names[n], MAX_GUEST_NAME, "%s", fd.cFileName);
                n++;
            }
        } while (FindNextFileA(h, &fd) && n < MAX_GUESTS);
        FindClose(h);
    }

    /* Missing/empty assets dir: fall back to the known sample names so the
     * picker is usable even before the guest assets have been built. */
    if (n == 0) {
        size_t n_def = sizeof(LAUNCHER_DEFAULT_GUESTS) / sizeof(LAUNCHER_DEFAULT_GUESTS[0]);
        for (n = 0; n < (int)n_def && n < MAX_GUESTS; n++) {
            snprintf(g_guest_names[n], MAX_GUEST_NAME, "%s", LAUNCHER_DEFAULT_GUESTS[n]);
        }
    }
    g_guest_count = n;
    winhost_log("launcher: %d guest(s) in %s", g_guest_count, g_assets_dir);
}

/* Create the dropdown + Run/Stop/Exit buttons as children of the host window. */
static void launcher_ui_create(void)
{
    int i;
    HMODULE hinst = GetModuleHandleA(NULL);

    if (g_combo || !g_hwnd) return;
    if (g_guest_count == 0) launcher_scan_guests();

    g_combo = CreateWindowA("COMBOBOX", "",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                            LAUNCH_MARGIN, LAUNCH_MARGIN, 260, 22,
                            g_hwnd, (HMENU)(INT_PTR)IDC_COMBO, hinst, NULL);
    if (g_combo) {
        for (i = 0; i < g_guest_count; i++) {
            SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)(LPCTSTR)g_guest_names[i]);
        }
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
    }

    g_btn_run  = CreateWindowA("BUTTON", "Run",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN, LAUNCH_MARGIN + 22 + LAUNCH_CTRL_GAP, 80, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_RUN, hinst, NULL);
    g_btn_stop = CreateWindowA("BUTTON", "Stop",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + 88,   LAUNCH_MARGIN + 22 + LAUNCH_CTRL_GAP, 80, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_STOP, hinst, NULL);
    g_btn_exit = CreateWindowA("BUTTON", "Exit",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + 176,  LAUNCH_MARGIN + 22 + LAUNCH_CTRL_GAP, 80, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_EXIT, hinst, NULL);
}

/* Enter the guest-running view: hide the picker controls and arm the frame
 * clock (it is stopped while the launcher is idle). */
static void launcher_ui_running(void)
{
    g_launcher_idle = false;
    if (g_combo)    ShowWindow(g_combo, SW_HIDE);
    if (g_btn_run)  ShowWindow(g_btn_run, SW_HIDE);
    if (g_btn_stop) ShowWindow(g_btn_stop, SW_HIDE);
    if (g_btn_exit) ShowWindow(g_btn_exit, SW_HIDE);
    vsync_clock_start();
}

/* Back to the picker: re-show the controls, reset the surface so the next
 * guest starts on a clean panel, and stop the frame clock while idle. */
static void launcher_ui_idle(void)
{
    if (g_combo)    { SendMessageA(g_combo, CB_SETCURSEL, 0, 0); ShowWindow(g_combo, SW_SHOW); }
    if (g_btn_run)  ShowWindow(g_btn_run, SW_SHOW);
    if (g_btn_stop) ShowWindow(g_btn_stop, SW_SHOW);
    if (g_btn_exit) ShowWindow(g_btn_exit, SW_SHOW);

    EnterCriticalSection(&g_surf_cs);
    surf_recreate_locked(g_init_w, g_init_h);
    g_surf_w = g_init_w;
    g_surf_h = g_init_h;
    LeaveCriticalSection(&g_surf_cs);

    vsync_clock_stop();

    g_launcher_idle = true;
    if (g_hwnd) { InvalidateRect(g_hwnd, NULL, TRUE); set_title("launcher"); }
}

bool win32_host_set_launcher(const char* assets_dir)
{
    if (assets_dir && *assets_dir) {
        snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", assets_dir);
    }
    g_launcher = true;
    launcher_scan_guests();
    launcher_ui_create();
    launcher_ui_idle();
    return true;
}

/* Boot the selected guest into the existing window. Runs on the UI thread
 * (button click handler). */
static void launcher_launch(void)
{
    char path[MAX_PATH + 96];
    char* argv[1];
    int sel;

    if (g_guest_thread || g_guest_count <= 0 || !g_combo) return;
    sel = (int)SendMessageA(g_combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_guest_count) return;

    snprintf(path, sizeof(path), "%s\\%s.exe", g_assets_dir, g_guest_names[sel]);
    if (_access(path, 0) != 0) {
        winhost_log("launcher: guest not found: %s", path);
        return;
    }

    /* Android GameActivity startup sequence. The WM_CREATE path is skipped
     * while the launcher is idle, so it is queued here - before the guest
     * thread starts, so the guest finds it on its first poll. */
    queue_lifecycle(APP_CMD_START);
    queue_lifecycle(APP_CMD_INIT_WINDOW);
    queue_lifecycle(APP_CMD_RESUME);
    queue_lifecycle(APP_CMD_GAINED_FOCUS);

    argv[0] = path;
    if (win32_host_start_guest(1, argv)) {
        winhost_log("launcher: starting %s", g_guest_names[sel]);
        launcher_ui_running();
    }
}

/* Stop the running guest. The guest thread cannot be force-killed safely, so
 * this is a cooperative stop: queue the Android teardown and let the guest
 * exit on its own (WM_APP_GUEST_EXIT then returns us to the picker). */
static void launcher_stop(void)
{
    if (!g_guest_thread) return;
    queue_lifecycle(APP_CMD_PAUSE);
    queue_lifecycle(APP_CMD_STOP);
    queue_lifecycle(APP_CMD_DESTROY);
    winhost_log("Stop requested: teardown lifecycle queued");
}

/* (Re)register every host-side cmdpost callback. Done once at init and again
 * before each launch: rvvm_user.c's guest exit path calls cmdpost_cleanup(),
 * which NULLs all callbacks and drops the AAudio backend - a relaunched guest
 * would probe a dead proxy (AAUDIO_QUERY -> 0 caps) and exit(1) before ever
 * reaching main(). Runs on the UI thread before the guest thread exists, so
 * there is no race with in-flight guest dispatches. */
static void win32_cmdpost_register_callbacks(void)
{
    cmdpost_set_sensor_callbacks(on_sensor_init, on_sensor_enable, on_sensor_data);
    cmdpost_set_window_callbacks(on_window_lock, on_window_unlock);
    cmdpost_set_window_size_callback(on_window_size);
    cmdpost_set_window_set_buf_callback(on_window_set_buf);
    cmdpost_set_config_callback(on_config_get);
    cmdpost_set_game_callbacks(on_game_lifecycle, on_game_input);
    /* Phase 3: GL/EGL dispatch callbacks */
    cmdpost_set_gl_callbacks(on_egl_dispatch, on_gl_dispatch);
    /* Phase 4: vsync source. Registering the blocker makes the guest advertise
     * the AChoreographer caps and, once the clock thread is up, it drives the
     * fd-wakeup path the guest's Looper polls. */
    cmdpost_set_choreographer_callback(on_choreographer_wait);
    /* Phase 5: AAudio backend (WASAPI). query() reports 0 caps when no audio
     * device exists, so every AAudio call on the guest fails gracefully. */
    cmdpost_set_audio_callbacks(win32_aaudio_ops());
}

bool win32_host_init(const char* title, int win_w, int win_h,
                     int virt_w, int virt_h, int virt_ppi, bool launcher)
{
    WNDCLASSA wc;
    RECT r;

    /* Set the launcher flag before the window is created: WM_CREATE fires
     * synchronously inside CreateWindowExA and must not queue the Android
     * startup lifecycle when there is no guest to consume it yet. */
    g_launcher = launcher;

    /* Layer 2: the OS window is only a viewport, sized independently of the
     * virtual panel. */
    g_win_w = (win_w > 0) ? win_w : 1024;
    g_win_h = (win_h > 0) ? win_h : 768;

    /* Layer 1: the host-owned virtual display the guest renders into. When no
     * explicit panel geometry is requested it follows the window size, so the
     * panel fills the viewport without borders. */
    g_virt_w   = (virt_w > 0) ? virt_w : g_win_w;
    g_virt_h   = (virt_h > 0) ? virt_h : g_win_h;
    g_virt_ppi = (virt_ppi > 0) ? virt_ppi : ACONFIGURATION_DENSITY_MEDIUM;

    /* The virtual panel is the surface geometry handed to the guest. */
    g_init_w = g_virt_w;
    g_init_h = g_virt_h;

    winhost_log("window: %dx%d px | virtual display: %dx%d px @ %d ppi (bucket %d)",
                g_win_w, g_win_h, g_virt_w, g_virt_h, g_virt_ppi,
                density_bucket_for_ppi(g_virt_ppi));

    SetProcessDPIAware();
    InitializeCriticalSection(&g_surf_cs);
    g_cs_ready = true;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = win_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RVVMWinHost";
    if (!RegisterClassA(&wc)) {
        winhost_log("RegisterClassA failed");
        return false;
    }

    r.left = 0; r.top = 0; r.right = g_win_w; r.bottom = g_win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        winhost_log("CreateWindowExA failed");
        return false;
    }

    /* Route console close / Ctrl+C through the same graceful WM_CLOSE path as
     * the window close button (see console_ctrl_handler). */
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        winhost_log("SetConsoleCtrlHandler failed; console close will not be graceful");
    }

    SetTimer(g_hwnd, SENSOR_TIMER_ID, SENSOR_TIMER_MS, NULL);

    cmdpost_init();
    win32_cmdpost_register_callbacks();
    /* Guest exit: record the real exit code and route sys_exit through the
     * graceful unwind instead of rvvm_user.c's _Exit() fallback (see
     * host_guest_exit_cb). Same role as jni_bridge.c's on_guest_exit. */
    rvvm_user_set_exit_callback(host_guest_exit_cb);
    vsync_clock_start();
    /* Try to load the GL backend (angle/swiftshader) */
    if (win32_gl_backend_load()) {
        winhost_log("GL backend: %s ready", win32_gl_backend_name());
    } else {
        winhost_log("GL backend: unavailable, fallback to CPU path");
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

bool win32_host_start_guest(int argc, char** argv)
{
    int i;

    if (g_guest_thread) return false;

    /* The previous guest's exit ran cmdpost_cleanup(), which NULLed every
     * host-side callback (audio backend included). Restore them before the
     * guest thread starts, or the guest probes a dead proxy and exits(1)
     * at its first AAUDIO_QUERY. */
    win32_cmdpost_register_callbacks();

    g_guest_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    if (!g_guest_argv) return false;
    for (i = 0; i < argc; i++) {
        g_guest_argv[i] = _strdup(argv[i]);
        if (!g_guest_argv[i]) {
            while (--i >= 0) free(g_guest_argv[i]);
            free(g_guest_argv);
            g_guest_argv = NULL;
            return false;
        }
    }
    g_guest_argc = argc;

    g_guest_thread = CreateThread(NULL, 0, guest_thread_main, NULL, 0, NULL);
    if (!g_guest_thread) {
        winhost_log("CreateThread failed");
        for (i = 0; i < argc; i++) free(g_guest_argv[i]);
        free(g_guest_argv);
        g_guest_argv = NULL;
        g_guest_argc = 0;
        return false;
    }

    set_title("guest running");
    winhost_log("guest launched: %s", argv[0]);
    return true;
}

int win32_host_message_loop(void)
{
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return g_guest_rc;
}

int win32_host_guest_exit_code(void)
{
    return g_guest_rc;
}

void win32_host_shutdown(void)
{
    /* Stop the frame clock first: it tells a guest blocked in poll() that the
     * source is gone, so the guest degrades instead of waiting forever. */
    vsync_clock_stop();

    /* Stop the audio backend before the guest thread: the WASAPI pump threads
     * must be joined before cmdpost_cleanup() tears down the stream table. */
    win32_aaudio_shutdown();

    bool guest_stopped = true;
    if (g_guest_thread) {
        /* Bounded wait: a wedged guest must not make a graceful exit hang
         * forever (CTRL_CLOSE_EVENT only grants ~5s before the OS kills us). */
        if (WaitForSingleObject(g_guest_thread, GUEST_EXIT_TIMEOUT_MS) == WAIT_TIMEOUT) {
            guest_stopped = false;
            winhost_log("guest thread did not exit within %d ms; skipping post-guest cleanup",
                        GUEST_EXIT_TIMEOUT_MS);
        }
        CloseHandle(g_guest_thread);
        g_guest_thread = NULL;
    }
    if (g_hwnd) {
        KillTimer(g_hwnd, SENSOR_TIMER_ID);
        g_hwnd = NULL;
    }
    if (!guest_stopped) {
        /* The guest thread is still live and may be touching cmdpost / the
         * surface right now. Tearing those down underneath it would be a
         * use-after-free, so leave them for process exit (ExitProcess reclaims
         * every thread and handle) rather than racing a still-running guest. */
        return;
    }
    cmdpost_cleanup();
    if (g_cs_ready) {
        DeleteCriticalSection(&g_surf_cs);
        g_cs_ready = false;
    }
}
