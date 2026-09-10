/*
 * win32_cmdpost_bridge.c - Win32 implementation of the vp_cmdpost host
 * callbacks (the layer that jni_bridge.c implements on Android).
 *
 * Guest ABI contract (verified against vp_ndk_stub.c):
 *  - WINDOW_LOCK:  guest passes its own ANativeWindow_Buffer; the host fills
 *                  width/height/stride/format. "bits" stays NULL: the guest
 *                  allocates its own pixel buffer (pixbuf_ensure).
 *  - WINDOW_UNLOCK: a1 = guest pixel pointer (identity-mapped guest memory);
 *                  the host converts pixels to a DIB and blits them.
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

#include "virtpass/vp_cmdpost.h"  /* single copy lives in src/virtpass */
#include "core/rvvm_user.h"       /* rvvm_user_linux() guest entry point */
#include "virtpass/vp_android.h"  /* guest ABI constants: APP_CMD_*, WINDOW_FORMAT_*, ASENSOR_TYPE_* */
#include "win32_gl_dispatch.h" /* on_egl_dispatch, on_gl_dispatch, g_gl_active */
#include "win32_gl_backend.h"  /* win32_gl_backend_load/ready/name/unload, w32gl_arg_f */

#define WM_APP_GUEST_EXIT (WM_APP + 1)
#define WM_APP_RESIZE_TO_SURFACE (WM_APP + 2)
#define SENSOR_TIMER_ID   1
#define SENSOR_TIMER_MS   100

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
static int    g_guest_rc     = -1;

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
                           int32_t src_fmt, bool rows_bottom_up)
{
    (void)src_fmt; /* conversion always uses the current surface format */
    EnterCriticalSection(&g_surf_cs);
    if (!g_dib_back_bits || g_surf_w != w || g_surf_h != h) {
        LeaveCriticalSection(&g_surf_cs);
        return;
    }
    for (int32_t y = 0; y < h; y++) {
        int32_t sy = rows_bottom_up ? (h - 1 - y) : y;
        convert_row(g_dib_back_bits + (size_t)y * (size_t)g_surf_w * 4,
                    rows + (size_t)sy * (size_t)g_surf_w * 4,
                    g_surf_w, g_surf_fmt);
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
    present_frame_impl(rows, w, h, src_fmt, rows_bottom_up);
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
    present_frame_impl(rb, w, h, WINDOW_FORMAT_RGBA_8888, true);
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
    if (guestPixels && g_dib_back_bits && g_surf_w > 0 && g_surf_h > 0) {
        int bpp = surf_bpp();
        const uint8_t* src = (const uint8_t*)guestPixels;
        uint8_t* dst = g_dib_back_bits;
        int32_t y;
        for (y = 0; y < g_surf_h; y++) {
            convert_row(dst + (size_t)y * (size_t)g_surf_w * 4,
                        src + (size_t)y * (size_t)g_surf_w * (size_t)bpp,
                        g_surf_w, g_surf_fmt);
        }
    }
    LeaveCriticalSection(&g_surf_cs);

    if (guestPixels && !logged_frame) {
        /* Positive confirmation that the whole LOCK -> render -> UNLOCK path
         * works, i.e. the guest did get its pixel buffer. */
        logged_frame = true;
        winhost_log("first frame posted: %dx%d fmt=%d from guest pixbuf %p", w, h, fmt, guestPixels);
    }

    present_frame(guestPixels ? (const uint8_t*)guestPixels : NULL,
                  guestPixels ? g_surf_w : 0,
                  guestPixels ? g_surf_h : 0,
                  g_surf_fmt, false);
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
        /* Android GameActivity startup order */
        queue_lifecycle(APP_CMD_START);
        queue_lifecycle(APP_CMD_INIT_WINDOW);
        queue_lifecycle(APP_CMD_RESUME);
        queue_lifecycle(APP_CMD_GAINED_FOCUS);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        EnterCriticalSection(&g_surf_cs);
        /* Letterbox background: the whole client area is black, the panel is
         * drawn on top inside the centred contain-fit rectangle. */
        FillRect(wdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_dib && g_surf_w > 0 && g_surf_h > 0) {
            /* The window is just a viewport onto the virtual panel: scale it
             * uniformly to fit and centre it. The panel is never cropped and
             * never distorted; window size and panel size are decoupled. */
            vp_view v;
            HDC mdc;
            HGDIOBJ old;
            viewport_fit(rc.right, rc.bottom, g_surf_w, g_surf_h, &v);
            mdc = CreateCompatibleDC(wdc);
            old = SelectObject(mdc, g_dib);
            if (v.dw == g_surf_w && v.dh == g_surf_h) {
                BitBlt(wdc, v.dx, v.dy, v.dw, v.dh, mdc, 0, 0, SRCCOPY);
            } else {
                SetStretchBltMode(wdc, HALFTONE);
                SetBrushOrgEx(wdc, 0, 0, NULL);
                StretchBlt(wdc, v.dx, v.dy, v.dw, v.dh,
                           mdc, 0, 0, g_surf_w, g_surf_h, SRCCOPY);
            }
            SelectObject(mdc, old);
            DeleteDC(mdc);

            /* Mark the virtual display region with a red frame so the
             * letterboxed panel boundary inside the OS window is visible. */
            {
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                HGDIOBJ old_pen = SelectObject(wdc, pen);
                HGDIOBJ old_brush = SelectObject(wdc, GetStockObject(NULL_BRUSH));
                Rectangle(wdc, v.dx, v.dy, v.dx + v.dw, v.dy + v.dh);
                SelectObject(wdc, old_brush);
                SelectObject(wdc, old_pen);
                DeleteObject(pen);
            }
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
        if (wParam != SIZE_MINIMIZED) {
            /* The window is just a viewport onto the fixed-size virtual panel,
             * so on resize we only need to repaint: the panel is re-scaled to
             * the new client area (contain fit). Invalidate the whole client
             * area so WM_PAINT redraws immediately instead of leaving the
             * freshly exposed region stale/unpainted. */
            if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
            queue_lifecycle(APP_CMD_WINDOW_RESIZED);
        }
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
        set_title(g_guest_rc == 0 ? "guest exited (0)" : "guest exited");
        winhost_log("guest exited with code %d", g_guest_rc);
        /* Guest is gone: no one will poll lifecycle cmds anymore, and the
         * host has no reason to keep running. Tear down the same way as
         * WM_CLOSE (Android teardown order) and quit the message loop. */
        queue_lifecycle(APP_CMD_PAUSE);
        queue_lifecycle(APP_CMD_STOP);
        queue_lifecycle(APP_CMD_DESTROY);
        DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        /* Android teardown order */
        queue_lifecycle(APP_CMD_PAUSE);
        queue_lifecycle(APP_CMD_STOP);
        queue_lifecycle(APP_CMD_DESTROY);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        queue_lifecycle(APP_CMD_TERM_WINDOW);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

/* ------------------------------------------------------------------ */
/* Guest thread                                                        */
/* ------------------------------------------------------------------ */

static DWORD WINAPI guest_thread_main(LPVOID arg)
{
    static char* envp[] = { NULL };
    static char env_prefix[] = "RVVM_USER_PREFIX="; /* putenv needs a persistent string */
    (void)arg;

    /* Same pattern as jni_bridge.c: assets resolve relative to CWD */
    putenv(env_prefix);

    g_guest_rc = rvvm_user_linux(g_guest_argc, g_guest_argv, envp);

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

bool win32_host_init(const char* title, int win_w, int win_h,
                     int virt_w, int virt_h, int virt_ppi)
{
    WNDCLASSA wc;
    RECT r;

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

    SetTimer(g_hwnd, SENSOR_TIMER_ID, SENSOR_TIMER_MS, NULL);

    cmdpost_init();
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

    if (g_guest_thread) {
        WaitForSingleObject(g_guest_thread, INFINITE);
        CloseHandle(g_guest_thread);
        g_guest_thread = NULL;
    }
    if (g_hwnd) {
        KillTimer(g_hwnd, SENSOR_TIMER_ID);
        g_hwnd = NULL;
    }
    cmdpost_cleanup();
    if (g_cs_ready) {
        DeleteCriticalSection(&g_surf_cs);
        g_cs_ready = false;
    }
}
