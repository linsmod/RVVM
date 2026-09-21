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
 *  - Sensors:       virtual accelerometer/gyroscope/light supplied by
 *                   win32_sensor_stub.c and injected from WM_TIMER while the
 *                   guest has them enabled. The subsystem (vp_sensor.c) owns
 *                   the wire identity and the per-queue fan-out.
 */

#define WIN32_LEAN_AND_MEAN
// #define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h> /* -ENOENT/-EINVAL from the asset mount ops */
#include <time.h>
#include <io.h>       /* _access(), _setmode() */
#include <fcntl.h>    /* open() */
#include <sys/stat.h> /* stat() for the asset mount's size op */
#include <dirent.h>   /* opendir()/readdir() for its directory ops */

#include "win32_cmdpost_bridge.h" /* self-protypes for forward refs (launcher) */
#include "virtpass/vp_cmdpost.h"  /* single copy lives in src/virtpass */
#include "core/rvvm_user.h"       /* rvvm_user_linux() guest entry point */
#include "virtpass/vp_android.h"  /* guest ABI constants: APP_CMD_*, WINDOW_FORMAT_*, ASENSOR_TYPE_* */
#include "virtpass/vp_session.h"  /* display geometry + console lines, shared with the Android host */
#include "win32_gl_dispatch.h" /* on_egl_dispatch, on_gl_dispatch, g_gl_active */
#include "win32_gl_backend.h"  /* win32_gl_backend_load/ready/name/unload, w32gl_arg_f */
#include "win32_aaudio_wasapi.h" /* win32_aaudio_ops, win32_aaudio_shutdown */
#include "win32_sensor_stub.h"   /* win32_sensor_stub_ops, win32_sensor_stub_tick */
#include <vterm.h>               /* guest virtual TTY parser (libvterm) */

#define WM_APP_GUEST_EXIT (WM_APP + 1)
#define WM_APP_RESIZE_TO_SURFACE (WM_APP + 2)
/* Base tick for the virtual sensors. Each sensor emits at the rate the guest
 * asked for through ASensorEventQueue_setEventRate(); this is only the
 * granularity of the host-side scheduler. */
#define SENSOR_TIMER_ID   1
#define SENSOR_TIMER_MS   10
/* Stop watchdog: armed together with the cooperative teardown, fires when the
 * guest is still alive at the end of the grace period. */
#define STOP_TIMER_ID     2

/* Launcher (Android-style picker) child control IDs. */
#define IDC_COMBO   101
#define IDC_RUN     102
#define IDC_STOP    103
#define IDC_EXIT    104
#define IDC_SUSPEND 105
#define IDC_KILL    106

/* How long win32_host_shutdown() waits for the guest thread to unwind. Kept
 * below the ~5s budget Windows grants a CTRL_CLOSE_EVENT handler so a
 * console-close shutdown still completes before the OS hard-kills us. */
#define GUEST_EXIT_TIMEOUT_MS 4000

/* Grace period the cooperative Android teardown gets before the host stops the
 * guest itself. A guest that polls its lifecycle commands exits in a frame or
 * two; anything still alive after this is not going to exit on its own. */
#define STOP_GRACE_MS 1500

/* Exit code reported for a host-forced stop (128 + SIGKILL): the Stop watchdog
 * expiring, or the Kill button. It is not a guest exit code: the guest was taken
 * down by the host, not by its own sys_exit. */
#define STOP_FORCED_EXIT_CODE 137

/* ------------------------------------------------------------------ */
/* Host state                                                          */
/* ------------------------------------------------------------------ */

static HWND            g_hwnd       = NULL;

/* This host's cmdpost instance for the run in progress: made by whoever queues
 * that run's startup sequence (win32_host_init for the first run, before the
 * window exists because WM_CREATE is where the sequence is queued;
 * launcher_launch_sel for every later one), bound to the run's machine with
 * rvvm_user_set_host_ctx() so the guest's syscalls reach it, and freed by that
 * run's guest thread as it hands control back. NULL between runs. Every
 * cmdpost_* call in this file writes into it - that state used to be file-scope
 * globals in vp_cmdpost.c, one set for the whole process. Declared here because
 * the vsync clock (defined above the registration function) calls into it. */
static vp_cmdpost_t*   g_cmdpost    = NULL;

/* Guest virtual TTY (libvterm) rendering state. g_tty is the host-owned console
 * session (rvvm_tty_open: it owns the VTerm, the lock that serializes access to
 * it, the scrollback and the view state), created on first launch and attached
 * to each run; g_tty_vt is the VTerm behind it, kept only for the diagnostic
 * dump below; g_tty_dirty gates repainting the retained layer.
 *
 * The renderer takes the cells from rvvm_tty_snapshot() - a locked copy - and
 * never reads the VTerm directly, so the UI thread and the guest thread (which
 * parses its output into the same VTerm) cannot touch it at once. */
#define TTY_ROWS 24
#define TTY_COLS 80
static rvvm_tty_t* g_tty   = NULL;
static VTerm*   g_tty_vt    = NULL;
static bool     g_tty_dirty = false;
/* True once the guest has written anything to fd 1/2 this launch. The
 * composition buffer is rebuilt every frame, so the TTY layer must be
 * re-blitted every frame too - a one-shot paint would be covered by the
 * next panel blit. Also keeps the last screen visible after guest exit:
 * the VTerm survives, dirty stays false and the layer content freezes. */
static bool     g_tty_seen  = false;
/* Retained TTY layer: the cell grid is rendered here only on dirty, every
 * frame just blits the bitmap onto the composition surface. */
/* Snapshot buffer for the cells. The guest thread parses its output into the
 * same session this renderer reads, so the cells are copied out under the
 * session lock first (rvvm_tty_snapshot) and everything below - the GDI calls
 * included - runs on that copy with no lock held. Grown on demand. */
static rvvm_tty_cell_t* g_tty_cells     = NULL;
static size_t           g_tty_cells_cap = 0;
static HDC      g_tty_layer_dc   = NULL;
static HBITMAP  g_tty_layer_bmp  = NULL;
static HBITMAP  g_tty_layer_def  = NULL; /* DC's stock 1x1 bitmap, to restore */
static bool     g_tty_layer_ok   = false;
static int      g_tty_layer_w    = 0;
static int      g_tty_layer_h    = 0;

/* TTY cell rendering: fonts [cjk][bold], created once (tty_init_fonts). */
static HFONT tty_fonts[2][2];
static bool  tty_fonts_ready = false;

/* Helpers (defined near host_tty_cb) */
static void     tty_init_fonts(void);
static COLORREF tty_argb(uint32_t argb);
static bool     tty_cell_bg_argb(uint32_t argb, COLORREF* out);
static bool     tty_is_cjk(uint32_t cp);
static int      tty_utf16(uint32_t cp, wchar_t* out);
static void     tty_fill_rect_bg(HDC cdc, int x0, int y0, int x1, int y1, COLORREF col);
static void     tty_paint(HDC cdc);
static void     tty_layer_free(void);
static bool            g_cs_ready   = false;
static CRITICAL_SECTION g_surf_cs;

/* Present surface (BGRA DIB, top-down) */
static HBITMAP         g_dib        = NULL;
static uint8_t*        g_dib_bits   = NULL;
static HBITMAP         g_dib_back   = NULL;  /* back buffer: guest writes here */
static uint8_t*        g_dib_back_bits = NULL;
static BITMAPINFO      g_bmi;

/* Guest surface geometry (as reported to the guest).
 *
 * Three nested layers, each owned by a different party:
 *   L3 OS window      g_win_w/h           - pure viewport; scales the panel
 *   L2 virtual panel  g_session.panel_*   - host-owned display; the composition
 *   L1 content        g_session.gfx_*     - the surface the guest draws into
 *
 * L2 -> L1 is 1:1: the guest's surface is placed at the panel origin and the
 * rest of the panel stays background. L3 -> L2 is the only scaling step, so
 * the guest controls its own resolution without the host resampling twice.
 *
 * L2 and L1 live in the shared session object (virtpass/vp_session.h) rather
 * than in globals here: they are the same state the Android host keeps while
 * driving its SurfaceView, and keeping one implementation of the panel/surface
 * policy is the whole point of that object. The panel is pinned once in
 * win32_host_init() and the guest surface latches from it on first use; no
 * resize path touches either, which is the guarantee the Android host has to
 * spell out as a latch because its window is draggable. */
static vp_session_t g_session;

/* One-shot confirmations that the guest's render path works (first successful
 * WINDOW_LOCK, first posted frame). Per-guest, not per-process: the launcher
 * boots several guests one after another and every one of them has to be
 * observable, not just the first. Reset in win32_host_start_guest(). */
static bool g_logged_lock  = false;
static bool g_logged_frame = false;

/* Layer-2 OS window: pure viewport, its size is independent of the virtual
 * panel. WM_PAINT scales the panel uniformly to fit (contain, never cropped)
 * and centres it, filling the leftover area with black. */
static int32_t g_win_w    = 1024;
static int32_t g_win_h    = 768;

/* Layer-1 virtual display: host-owned panel parameters, held in the session
 * above. The guest observes them ONLY through the public NDK ABI
 * (ANativeWindow_getWidth/Height and AConfiguration_*); no virtpass-specific
 * display ABI is exposed. Pinned once in win32_host_init() and never moved. */

/* Guest thread */
static HANDLE g_guest_thread = NULL;
/* Userland machine for the booted guest. Owned (and freed) by the guest thread
 * once rvvm_user_linux_ex() runs, so the UI thread must only touch it through
 * the guards below and never while no guest is booted. */
static rvvm_machine_t* g_guest_machine = NULL;
static int    g_guest_argc   = 0;
static char** g_guest_argv   = NULL;
/* Guest exit code. Written on the guest thread: host_guest_exit_cb() stores
 * the real code at sys_exit time, guest_thread_main() may overwrite it with
 * the rvvm_user_linux_ex() error code if the guest never started. */
static int    g_guest_rc     = -1;

/* Host-side suspend/resume guards. The machine handle only exists while a guest
 * is booted, so every host control call goes through these instead of testing
 * g_guest_machine at each call site. */
static bool guest_suspended(void)
{
    return g_guest_machine && rvvm_user_is_suspended(g_guest_machine);
}

static void guest_resume(void)
{
    if (g_guest_machine) rvvm_user_resume(g_guest_machine);
}

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

/*
 * Combo box geometry. The height passed to CreateWindowA() for a
 * CBS_DROPDOWNLIST combo is the TOTAL height: the closed static box plus the
 * dropped list area. Windows shrinks the window to the static box when the
 * list is closed and expands it again on drop.
 *
 * Passing just the static height (22) leaves a zero-height list area, so
 * CB_SHOWDROPDOWN / a click on the arrow really does drop the list - the
 * ComboLBox window exists and CB_GETDROPPEDSTATE reports TRUE - but the list
 * has no pixels to show, which looks exactly like "the dropdown does not
 * open". The list area must be part of the created height.
 */
#define LAUNCH_COMBO_EDIT_H  22   /* closed static box (system picks ~this) */
#define LAUNCH_COMBO_LIST_H  180  /* dropped list area                     */
#define LAUNCH_COMBO_H       (LAUNCH_COMBO_EDIT_H + LAUNCH_COMBO_LIST_H)

/*
 * Vertical layout of the picker, top to bottom: title / combo / buttons. The
 * controls stay visible while a guest runs (the parent has WS_CLIPCHILDREN,
 * so the guest panel never paints over them), so the rows must not overlap.
 * The buttons sit below the combo's static box, NOT below the dropped list:
 * the list is a popup and floats over them while it is open.
 */
#define LAUNCH_TITLE_H   24
#define LAUNCH_COMBO_Y   (LAUNCH_MARGIN + LAUNCH_TITLE_H + LAUNCH_CTRL_GAP)
#define LAUNCH_BTN_Y     (LAUNCH_COMBO_Y + LAUNCH_COMBO_EDIT_H + LAUNCH_CTRL_GAP)

/*
 * Button row of the picker: Run / Stop / Kill / Suspend / Exit, left to right.
 * Run, Stop and Kill end up the same width and pitch; Suspend is wider because
 * its "Suspend" / "Resume" label is, and Exit follows it.
 */
#define LAUNCH_BTN_W     80
#define LAUNCH_BTN_STEP  (LAUNCH_BTN_W + LAUNCH_CTRL_GAP)
#define LAUNCH_SUSP_W    88

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
static HWND g_btn_susp  = NULL;      /* Suspend/Resume toggle */
static HWND g_btn_kill  = NULL;      /* Kill (stop without waiting) */

/* Guest switch requested while another guest is still running: the old one is
 * torn down cooperatively and this pick is booted from WM_APP_GUEST_EXIT. */
static bool g_switch_pending = false;
static int  g_switch_sel     = -1;

/* True while the Stop watchdog (STOP_TIMER_ID) is armed, i.e. a cooperative
 * teardown is outstanding. Cleared when the guest exits or the watchdog fires. */
static bool g_stop_watchdog  = false;

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
        vp_cmdpost_vsync_tick(g_cmdpost, t);
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
    vp_cmdpost_vsync_source_lost(g_cmdpost);

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
    return (g_session.gfx_fmt == WINDOW_FORMAT_RGB_565) ? 2 : 4;
}

/* Caller holds g_surf_cs.
 *
 * The DIB is the L2 composition surface: it is panel-sized, and a guest
 * surface smaller than the panel occupies its top-left corner with the rest
 * left as background. Sizing it to the panel (rather than to the guest
 * surface) is what keeps L2 and L1 distinct; sizing it to the surface would
 * make the panel collapse onto the content. */
static void surf_recreate_locked(int32_t panel_w, int32_t panel_h)
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
    if (panel_w <= 0 || panel_h <= 0) return;

    ZeroMemory(&g_bmi, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth       = panel_w;
    g_bmi.bmiHeader.biHeight      = -panel_h; /* top-down rows */
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
    if (!g_dib_bits || !g_dib_back_bits) {
        winhost_log("CreateDIBSection(%dx%d) failed", (int)panel_w, (int)panel_h);
        return;
    }

    /* A fresh panel starts blank; the guest surface only covers its corner,
     * so anything left showing would otherwise be stale pixels. */
    memset(g_dib_bits, 0, (size_t)panel_w * (size_t)panel_h * 4u);
    memset(g_dib_back_bits, 0, (size_t)panel_w * (size_t)panel_h * 4u);
}

/* Layer-2 -> layer-1 placement. The guest surface sits at the panel origin.
 *
 * EGL window coordinates put the origin at the bottom-left, and glReadPixels
 * (0,0,w,h) returns that corner. The DIB is top-down, so writing those rows in
 * order lands the surface at the top-left of the panel - the correct rendering
 * of "an unscaled surface pinned to the origin", not an arbitrary choice. */
static void content_rect_locked(int32_t* x, int32_t* y, int32_t* w, int32_t* h)
{
    *x = 0;
    *y = 0;
    *w = (g_session.gfx_w > 0) ? g_session.gfx_w : 0;
    *h = (g_session.gfx_h > 0) ? g_session.gfx_h : 0;
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
    if (!rows || !g_dib_back_bits ||
        g_session.panel_w <= 0 || g_session.panel_h <= 0) {
        LeaveCriticalSection(&g_surf_cs);
        return;
    }

    /* The frame is the guest's L1 surface; the DIB is the L2 panel. Copy 1:1
     * into the content rect (panel origin), cropping anything that would fall
     * outside the panel rather than writing past the DIB. */
    int32_t cx, cy, cw, ch;
    content_rect_locked(&cx, &cy, &cw, &ch);
    if (cw > w) cw = w;
    if (ch > h) ch = h;
    if (cx + cw > g_session.panel_w) cw = g_session.panel_w - cx;
    if (cy + ch > g_session.panel_h) ch = g_session.panel_h - cy;

    if (cw > 0 && ch > 0) {
        /* The source pitch belongs to the caller's buffer. Hard-coding four
         * bytes per pixel (as this used to) silently mis-strides RGB_565
         * frames; the GL path always hands over 4-byte RGBA and says so. */
        size_t src_pitch = (size_t)w * (size_t)src_bpp;
        size_t dst_pitch = (size_t)g_session.panel_w * 4u;
        for (int32_t y = 0; y < ch; y++) {
            int32_t sy = rows_bottom_up ? (h - 1 - y) : y;
            convert_row(g_dib_back_bits + (size_t)(cy + y) * dst_pitch
                            + (size_t)cx * 4u,
                        rows + (size_t)sy * src_pitch,
                        cw, g_session.gfx_fmt);
        }
    }
    { HBITMAP tb = g_dib; uint8_t* tp = g_dib_bits;
      g_dib = g_dib_back; g_dib_bits = g_dib_back_bits;
      g_dib_back = tb; g_dib_back_bits = tp; }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);

    /* Log once per distinct geometry: without the first frame as a baseline,
     * a frame that never reaches the window is indistinguishable from one
     * that was sized outside the panel. */
    static int32_t lg_w = -1, lg_h = -1, lg_pw = -1, lg_ph = -1;
    if (w != lg_w || h != lg_h ||
        g_session.panel_w != lg_pw || g_session.panel_h != lg_ph) {
        lg_w = w; lg_h = h;
        lg_pw = g_session.panel_w; lg_ph = g_session.panel_h;
        winhost_log("panel %dx%d <- content %dx%d at (%d,%d) [%dx%d used]",
                    (int)g_session.panel_w, (int)g_session.panel_h, (int)w, (int)h,
                    (int)cx, (int)cy, (int)cw, (int)ch);
    }
    LeaveCriticalSection(&g_surf_cs);
}

void present_frame(const uint8_t* rows, int32_t w, int32_t h,
                    int32_t src_fmt, bool rows_bottom_up)
{
    present_frame_impl(rows, w, h, src_fmt, 0, rows_bottom_up);
}

void present_gl_set_surface_size(int32_t cw, int32_t ch)
{
    if (cw <= 0 || ch <= 0) return;
    EnterCriticalSection(&g_surf_cs);
    /* Layer 1 only. The panel (L2) is host-owned and unaffected: a surface
     * smaller than the panel simply occupies less of it. */
    g_session.gfx_w = cw;
    g_session.gfx_h = ch;
    if (!g_dib_bits) {
        int32_t pw, ph;
        vp_session_panel_size(&g_session, &pw, &ph);
        surf_recreate_locked(pw, ph);
    }
    LeaveCriticalSection(&g_surf_cs);
}

void present_gl_panel_size(int32_t* w, int32_t* h)
{
    EnterCriticalSection(&g_surf_cs);
    vp_session_panel_size(&g_session, w, h);
    LeaveCriticalSection(&g_surf_cs);
}

void present_gl_frame(void)
{
    if (!g_gl_active) return;

    /* The GL path never calls ANativeWindow_lock, so on a pure-EGL guest the
     * panel DIB may not exist yet. Build it here rather than dropping the
     * frame: a guest that only uses EGL should still reach the screen. */
    EnterCriticalSection(&g_surf_cs);
    if (!g_dib_bits || !g_dib_back_bits) {
        int32_t pw, ph;
        vp_session_panel_size(&g_session, &pw, &ph);
        surf_recreate_locked(pw, ph);
    }
    /* No SET_BUF was ever seen: adopt the panel as the guest surface so the
     * readback has a defined size. Idempotent - it latches on the first call
     * and does nothing after. */
    vp_session_guest_geometry(&g_session, NULL, NULL, NULL);
    LeaveCriticalSection(&g_surf_cs);

    static uint8_t* rb = NULL; static int32_t rb_w = 0, rb_h = 0;
    EnterCriticalSection(&g_surf_cs);
    if (rb_w != g_session.gfx_w || rb_h != g_session.gfx_h) {
        free(rb);
        rb_w = g_session.gfx_w; rb_h = g_session.gfx_h;
        if (rb_w > 0 && rb_h > 0) {
            rb = (uint8_t*)malloc((size_t)rb_w * (size_t)rb_h * 4);
        }
    }
    int32_t w = g_session.gfx_w, h = g_session.gfx_h;
    LeaveCriticalSection(&g_surf_cs);

    if (!rb || w <= 0 || h <= 0) return;
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    present_frame_impl(rb, w, h, WINDOW_FORMAT_RGBA_8888, 4, true);
}

static int32_t on_window_lock(vp_cmdpost_t* inst, void* window, void* outBuffer, void* dirtyBounds)
{
    cmdpost_ANativeWindow_Buffer* buf = (cmdpost_ANativeWindow_Buffer*)outBuffer;
    (void)inst;   /* single-run host: the instance carries no extra state */
    (void)window;
    (void)dirtyBounds;
    if (!buf) {
        winhost_log("WINDOW_LOCK with NULL outBuffer -> rejected");
        return -1;
    }

    EnterCriticalSection(&g_surf_cs);
    /* First lock before any SET_BUF: default to the panel geometry. Latched
     * once, like the win32 host always did inline and the Android host does
     * through the same call. */
    vp_session_guest_geometry(&g_session, NULL, NULL, NULL);
    /* The composition surface is panel-sized (L2); the guest surface (L1) only
     * occupies its corner - see surf_recreate_locked(). */
    if (!g_dib_bits) {
        int32_t pw, ph;
        vp_session_panel_size(&g_session, &pw, &ph);
        surf_recreate_locked(pw, ph);
    }
    buf->width  = g_session.gfx_w;
    buf->height = g_session.gfx_h;
    buf->stride = g_session.gfx_w;  /* pixels, matches guest expectation */
    buf->format = g_session.gfx_fmt;
    buf->bits   = NULL;        /* guest allocates its own buffer (pixbuf_ensure) */
    if (!g_logged_lock) {
        g_logged_lock = true;
        winhost_log("WINDOW_LOCK ok: %dx%d stride=%d fmt=%d "
                    "(guest renders into its own pixbuf; bits stays NULL)",
                    buf->width, buf->height, buf->stride, buf->format);
    }
    LeaveCriticalSection(&g_surf_cs);
    return 0;
}

static int32_t on_window_unlock(vp_cmdpost_t* inst, void* window, void* guestPixels)
{
    int32_t w = 0, h = 0, fmt = 0;
    (void)inst;   /* single-run host: the instance carries no extra state */
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
    w   = g_session.gfx_w;
    h   = g_session.gfx_h;
    fmt = g_session.gfx_fmt;
    LeaveCriticalSection(&g_surf_cs);

    if (guestPixels && !g_logged_frame) {
        /* Positive confirmation that the whole LOCK -> render -> UNLOCK path
         * works, i.e. the guest did get its pixel buffer. */
        g_logged_frame = true;
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

static void on_window_size(vp_cmdpost_t* inst, int64_t* width, int64_t* height)
{
    int32_t w = 0, h = 0;

    (void)inst;   /* single-run host: the instance carries no extra state */
    if (!width || !height) return;
    EnterCriticalSection(&g_surf_cs);
    /* The guest's window is its own surface (L1), never the panel: being asked
     * for the size is what latches it from the panel the first time. */
    vp_session_guest_geometry(&g_session, &w, &h, NULL);
    LeaveCriticalSection(&g_surf_cs);

    *width  = (int64_t)w;
    *height = (int64_t)h;
}

static int32_t on_window_set_buf(vp_cmdpost_t* inst, int32_t width, int32_t height, int32_t format)
{
    /* The guest ABI validation stays here: these are the NDK's formats, and a
     * size or format outside them is a guest error it should hear about. */
    (void)inst;   /* single-run host: the instance carries no extra state */
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return -1;
    if (format != WINDOW_FORMAT_RGBA_8888 &&
        format != WINDOW_FORMAT_RGBX_8888 &&
        format != WINDOW_FORMAT_RGB_565) return -1;

    EnterCriticalSection(&g_surf_cs);
    /* Layer 1 only: the guest picked a content size. The composition surface
     * stays panel-sized, so the new content simply occupies a (possibly
     * different) corner of the existing panel - rebuilding the DIB here is
     * what used to collapse L2 onto L1. Create it if this is the first thing
     * the guest did, before any lock or GL present. */
    vp_session_set_guest_geometry(&g_session, width, height, format);
    if (!g_dib_bits) {
        int32_t pw, ph;
        vp_session_panel_size(&g_session, &pw, &ph);
        surf_recreate_locked(pw, ph);
    }
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

static int32_t on_config_get(vp_cmdpost_t* inst, int32_t field, int32_t* outValue)
{
    (void)inst;   /* single-run host: the instance carries no extra state */
    int32_t w, h, ppi, width_dp, height_dp, long_dp, short_dp;

    if (!outValue) return -1;

    EnterCriticalSection(&g_surf_cs);
    /* The configuration describes the guest's own surface, falling back to the
     * panel before a SET_BUF/LOCK has picked one. */
    w   = (g_session.gfx_w > 0) ? g_session.gfx_w : g_session.panel_w;
    h   = (g_session.gfx_h > 0) ? g_session.gfx_h : g_session.panel_h;
    ppi = (g_session.panel_ppi > 0) ? g_session.panel_ppi
                                    : ACONFIGURATION_DENSITY_MEDIUM;
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
    /* Input is delivered in layer-1 (content) coordinates: invert the same
     * layer-3 -> layer-2 transform WM_PAINT applies, then the 1:1 placement
     * of the content rect inside the panel carries the point to the guest. */
    vp_session_panel_size(&g_session, &sw, &sh);
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
    cmdpost_queue_motion_event(g_cmdpost, &ev);
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
    cmdpost_queue_lifecycle_cmd(g_cmdpost, cmd);
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

/* TEMP DEBUG: input/notifications trace. Remove after diagnosis. */
static bool g_dbg_dropped_prev = false;
static int  g_dbg_paint_count  = 0;

static const char* dbg_cls(HWND h)
{
    static char buf[64];
    if (!h) return "(null)";
    buf[0] = '\0';
    GetClassNameA(h, buf, sizeof(buf));
    return buf;
}

static void dbg_trace(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PARENTNOTIFY:
        winhost_log("DBG PARENTNOTIFY ev=%u child=%s id=%d",
                    (unsigned)LOWORD(wParam), dbg_cls((HWND)lParam),
                    (int)GetDlgCtrlID((HWND)lParam));
        break;
    case WM_COMMAND:
        winhost_log("DBG COMMAND id=%d notify=%u src=%s fromCombo=%d",
                    (int)LOWORD(wParam), (unsigned)HIWORD(wParam),
                    dbg_cls((HWND)lParam),
                    (int)(g_combo != NULL && (HWND)lParam == g_combo));
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: {
        POINT pt;
        HWND  hit;
        pt.x = (short)LOWORD(lParam);
        pt.y = (short)HIWORD(lParam);
        ClientToScreen(hwnd, &pt);
        hit = WindowFromPoint(pt);
        winhost_log("DBG %s client=%d,%d hit=%s isCombo=%d isMain=%d",
                    msg == WM_LBUTTONDOWN ? "LBTNDOWN" :
                    (msg == WM_LBUTTONDBLCLK ? "LBTNDBLCLK" : "LBTNUP"),
                    (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam),
                    dbg_cls(hit), (int)(hit == g_combo), (int)(hit == g_hwnd));
        break;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        winhost_log("DBG %s focus=%s",
                    msg == WM_SETFOCUS ? "SETFOCUS" : "KILLFOCUS",
                    dbg_cls(GetFocus()));
        break;
    case WM_ACTIVATE:
        winhost_log("DBG ACTIVATE state=%u minimized=%u",
                    (unsigned)LOWORD(wParam), (unsigned)HIWORD(wParam));
        break;
    case WM_CAPTURECHANGED:
        winhost_log("DBG CAPTURECHANGED newCapture=%s", dbg_cls((HWND)lParam));
        break;
    case WM_MOUSEACTIVATE:
        winhost_log("DBG MOUSEACTIVATE topLevel=%s hitCode=%u",
                    dbg_cls((HWND)wParam), (unsigned)LOWORD(lParam));
        break;
    case WM_TIMER:
        if (wParam == SENSOR_TIMER_ID && g_combo) {
            bool d = SendMessageA(g_combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
            if (d != g_dbg_dropped_prev) {
                g_dbg_dropped_prev = d;
                winhost_log("DBG combo dropped=%d comboLBox=0x%p",
                            (int)d, (void*)FindWindowA("ComboLBox", NULL));
            }
        }
        break;
    case WM_PAINT:
        if (g_launcher_idle && (++g_dbg_paint_count % 30) == 0) {
            winhost_log("DBG paint#%d (idle view repainted)", g_dbg_paint_count);
        }
        break;
    default:
        break;
    }
}

/* Launcher helpers are defined further down (Public API section) but win_proc
 * calls them from WM_COMMAND / WM_APP_GUEST_EXIT. Forward declarations. */
static void launcher_ui_idle(void);
static void launcher_launch(void);
static void launcher_launch_sel(int sel);
static void launcher_stop(void);
static void launcher_kill(void);
static void launcher_toggle_suspend(void);

static LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    dbg_trace(hwnd, msg, wParam, lParam);
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
        case IDC_KILL: launcher_kill();         return 0;
        case IDC_SUSPEND: launcher_toggle_suspend(); return 0;
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
            t.top = LAUNCH_MARGIN;
            t.bottom = t.top + LAUNCH_TITLE_H;
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
            /* After a guest exits, keep its frozen last screen visible over
             * the picker background. Child controls (combo + buttons) are
             * separate windows and always draw above this. */
            if (g_tty_vt && g_tty_seen) {
                tty_paint(wdc);
            }
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
            /* The panel is immutable once win32_host_init() has pinned it, so
             * this read needs no lock - as it never did. */
            int32_t pw = 0, ph = 0;
            vp_session_panel_size(&g_session, &pw, &ph);

            /* Letterbox background: the whole client area is black, the panel
             * goes on top inside the centred contain-fit rectangle. */
            FillRect(cdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (g_dib && pw > 0 && ph > 0) {
                /* The window is just a viewport onto the virtual panel: scale
                 * it uniformly to fit and centre it. The panel is never
                 * cropped and never distorted; window size and panel size are
                 * decoupled. The DIB is panel-sized, so this is the only
                 * scaling step in the chain (the guest surface inside the
                 * panel is composited 1:1). */
                vp_view v;
                HDC mdc;
                HGDIOBJ old;
                viewport_fit(rc.right, rc.bottom, pw, ph, &v);
                mdc = CreateCompatibleDC(cdc);
                old = SelectObject(mdc, g_dib);
                if (v.dw == pw && v.dh == ph) {
                    BitBlt(cdc, v.dx, v.dy, v.dw, v.dh, mdc, 0, 0, SRCCOPY);
                } else {
                    SetStretchBltMode(cdc, HALFTONE);
                    SetBrushOrgEx(cdc, 0, 0, NULL);
                    StretchBlt(cdc, v.dx, v.dy, v.dw, v.dh,
                               mdc, 0, 0, pw, ph, SRCCOPY);
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
            /* Guest virtual TTY overlay: blit the retained layer over the
             * panel every frame (the composition buffer is rebuilt from
             * scratch each paint, so a one-shot draw would be covered). */
            if (g_tty_vt && g_tty_seen) {
                tty_paint(cdc);
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
             * callback clears the source-lost flag the stop above raised).
             * A host-suspended guest is parked and polls nothing, so the clock
             * stays off; launcher_toggle_suspend() re-arms it on resume. */
            g_minimized = false;
            cmdpost_set_choreographer_callback(g_cmdpost, on_choreographer_wait);
            if (!guest_suspended()) vsync_clock_start();
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
        if (wParam == SENSOR_TIMER_ID) {
            win32_sensor_stub_tick();
        } else if (wParam == STOP_TIMER_ID) {
            /* The guest sat through the whole cooperative teardown without
             * exiting: it never polls its lifecycle commands (e.g.
             * guest-samples/test_render.c renders in a loop and ignores
             * APP_CMD_DESTROY) or it is wedged in its own loop. Stop it from
             * the host side instead of leaving the Stop button dead.
             *
             * rvvm_user_stop() is not TerminateThread(): it pauses every guest
             * vCPU and marks every guest thread finished, so the guest unwinds
             * through its normal exit path (cmdpost_end_run, machine free) and
             * WM_APP_GUEST_EXIT follows exactly as on a real guest exit. */
            KillTimer(hwnd, STOP_TIMER_ID);
            g_stop_watchdog = false;
            if (g_guest_machine) {
                winhost_log("Stop: guest ignored the teardown, forcing rvvm_user_stop()");
                rvvm_user_stop(g_guest_machine, STOP_FORCED_EXIT_CODE);
            }
        }
        return 0;

    case WM_APP_GUEST_EXIT:
        /* The guest exited on its own (or was forced down above): the pending
         * watchdog has nothing left to do. */
        if (g_stop_watchdog) {
            KillTimer(hwnd, STOP_TIMER_ID);
            g_stop_watchdog = false;
        }
        /* Release the finished guest thread handle: launcher_launch() guards
         * on g_guest_thread, so keeping it would silently block the next Run
         * in the picker. The thread has posted its final message and only
         * unwinds from here, which does not need our handle. */
        if (g_guest_thread) {
            CloseHandle(g_guest_thread);
            g_guest_thread = NULL;
        }
        /* rvvm_user_linux_ex() freed the machine; the VTerm is host-owned and
         * survives the guest, so keep g_tty_vt and the frozen last screen
         * (g_tty_seen stays set, dirty stays clear => no re-render). */
        g_guest_machine = NULL;
        if (g_tty_vt && g_tty_seen) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        {
            char title[48];
            snprintf(title, sizeof(title), "guest exited (%d)", g_guest_rc);
            set_title(title);
        }
        winhost_log("guest exited with code %d", g_guest_rc);
        /* Optional dump of the frozen screen matrix to stdout. Guest output
         * already reaches stdout verbatim through the host fd path; this shows
         * the parsed screen instead (escapes resolved, in-place updates
         * applied), which is what makes it useful from a batch run. */
        if (g_tty_vt && getenv("RVVM_TTY_DUMP")) {
            VTermScreen* scr = vterm_obtain_screen(g_tty_vt);
            VTermRect    rect = { 0, TTY_ROWS, 0, TTY_COLS };
            char         buf[TTY_ROWS * TTY_COLS + 1];
            size_t       n;
            /* The session lock, as for every other read of the VTerm. */
            rvvm_tty_lock(g_tty);
            n = vterm_screen_get_text(scr, buf, sizeof(buf) - 1, rect);
            rvvm_tty_unlock(g_tty);
            buf[n] = 0;
            printf("---- guest tty ----\n%s\n---- end guest tty ----\n", buf);
            fflush(stdout);
        }
        /* Guest is gone: nobody polls lifecycle cmds / input anymore, and the
         * guest thread already ran cmdpost_end_run(). Queuing the Android
         * teardown here (as WM_CLOSE does) would only leave PAUSE/STOP/DESTROY
         * sitting in the queue for the *next* guest booted in this process,
         * which then destroys itself on its first poll - the classic "second
         * Run does not work". Drop whatever is still pending instead; a new
         * launch queues a fresh startup sequence. */
        cmdpost_clear_lifecycle_cmds(g_cmdpost);
        cmdpost_clear_motion_events(g_cmdpost);
        if (g_launcher) {
            /* Launcher mode: do NOT close the window. Reset the guest-owned
             * surface and re-show the picker so another guest can be booted
             * in the same window. */
            launcher_ui_idle();
            if (g_switch_pending) {
                g_switch_pending = false;
                launcher_launch_sel(g_switch_sel);
                g_switch_sel = -1;
            }
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
        /* Host-owned console session: close it only when no guest can still be
         * writing into it; otherwise rvvm_user's ctx holds a live pointer until
         * the machine unwinds, and the process is going away anyway. */
        if (!g_guest_machine && g_tty) {
            rvvm_tty_close(g_tty);
            g_tty       = NULL;
            g_tty_vt    = NULL;
            g_tty_dirty = false;
            g_tty_seen  = false;
            tty_layer_free();
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
 * rvvm_user_linux_ex() is still unwinding the other vCPUs - so this must not
 * touch cmdpost, the surface or the window; the ordered teardown runs later
 * in WM_APP_GUEST_EXIT. It also fires on the thread that called
 * rvvm_user_stop() (host-initiated stop); the machine comes as an argument,
 * so both paths are identified the same way.
 *
 * Registering it is load-bearing: with no callback registered, rvvm_user.c
 * falls back to _Exit(exit_code) inside the syscall path, which terminates
 * the whole WinHost process from the guest thread - guest_thread_main would
 * never post WM_APP_GUEST_EXIT and the launcher could never return to the
 * picker (and Stop would take the window down with the guest). */
static void host_guest_exit_cb(rvvm_machine_t* machine, int exit_code)
{
    (void)machine; /* one guest at a time on this host: g_guest_machine */
    /* rvvm_user_linux_ex() itself always returns 0 on a guest-driven exit, so
     * this callback is the only source of the real exit code. */
    g_guest_rc = exit_code;
    winhost_log("guest exit callback: code %d", exit_code);
}

/* TTY callback: rvvm_user fires this on every guest fd 1/2 write after feeding
 * the bytes to the session. It only flags a repaint; the snapshot + draw happen
 * in WM_PAINT (throttled to vsync), so a burst of guest output collapses into
 * one frame instead of one redraw per write. The session's own serial is the
 * same signal for a poller; this host renders from the event instead. */
static void host_tty_cb(void* userdata, int fd, void* tty)
{
    (void)userdata; (void)fd; (void)tty;
    g_tty_dirty = true;
    g_tty_seen  = true; /* guest produced TTY output this launch */
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* --- TTY cell rendering helpers (color + Unicode) ---
 *
 * The cells come from the session snapshot (rvvm_tty_cell_t): a codepoint, ARGB
 * colors and flags, with colors already resolved and double-width gaps zeroed.
 * The renderer walks that grid and merges runs of same-styled cells into one
 * TextOutW call; run origins are placed by column (col * cw) instead of relying
 * on the GDI text advance, so double-width cells and CJK fallback fonts cannot
 * cause drift.
 */

/* Fonts: [cjk][bold], created once (array lives next to the TTY state).
 * CJK-capable face is used for CJK runs - GDI has no font linking in
 * TextOutW, so Lucida Console alone would render boxes for CJK codepoints. */
static void tty_init_fonts(void)
{
    if (tty_fonts_ready) {
        return;
    }
    for (int cjk = 0; cjk < 2; cjk++) {
        for (int bold = 0; bold < 2; bold++) {
            LOGFONTW lf;
            memset(&lf, 0, sizeof(lf));
            lf.lfHeight         = 16;
            lf.lfWeight         = bold ? FW_BOLD : FW_NORMAL;
            lf.lfCharSet        = cjk ? DEFAULT_CHARSET : ANSI_CHARSET;
            if (cjk) {
                memcpy(lf.lfFaceName, L"Microsoft YaHei", sizeof(L"Microsoft YaHei"));
            } else {
                memcpy(lf.lfFaceName, L"Lucida Console", sizeof(L"Lucida Console"));
            }
            tty_fonts[cjk][bold] = CreateFontIndirectW(&lf);
        }
    }
    tty_fonts_ready = true;
}

/* A cell color out of the session snapshot (already ARGB) as a COLORREF. */
static COLORREF tty_argb(uint32_t argb)
{
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

/* Background: black means "no background" - the default one - so the black
 * backdrop stays visible, which is the same test the Android renderer makes on
 * the same snapshot. */
static bool tty_cell_bg_argb(uint32_t argb, COLORREF* out)
{
    if ((argb & 0x00FFFFFF) == 0) {
        return false;
    }
    *out = tty_argb(argb);
    return true;
}

static bool tty_is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||   /* CJK radicals..Yi, kana, hangul jamo */
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   /* hangul syllables */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compat ideographs */
           (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms */
           (cp >= 0x20000 && cp <= 0x3FFFD);   /* CJK ext B.. */
}

/* UCS-4 -> UTF-16, returns the number of wchar_t units written (0 to skip) */
static int tty_utf16(uint32_t cp, wchar_t* out)
{
    if (cp == 0 || cp == (uint32_t)-1) {
        return 0;
    }
    if (cp < 0x20 || cp == 0x7F) {
        out[0] = L'.'; /* unprintable control */
        return 1;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp < 0xE000) {
            return 0; /* lone surrogate */
        }
        out[0] = (wchar_t)cp;
        return 1;
    }
    if (cp > 0x10FFFF) {
        return 0;
    }
    cp -= 0x10000;
    out[0] = (wchar_t)(0xD800 + (cp >> 10));
    out[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
    return 2;
}

static void tty_fill_rect_bg(HDC cdc, int x0, int y0, int x1, int y1, COLORREF col)
{
    HBRUSH br = CreateSolidBrush(col);
    RECT rc = { x0, y0, x1, y1 };
    FillRect(cdc, &rc, br);
    DeleteObject(br);
}

/* Release the retained TTY layer bitmap and the snapshot buffer (window
 * teardown, no guest running). */
static void tty_layer_free(void)
{
    if (g_tty_layer_dc) {
        if (g_tty_layer_bmp) {
            /* Deselect before delete: DeleteObject fails on a bitmap that is
             * still selected into a DC. */
            SelectObject(g_tty_layer_dc, g_tty_layer_def);
            DeleteObject(g_tty_layer_bmp);
            g_tty_layer_bmp = NULL;
        }
        DeleteDC(g_tty_layer_dc);
        g_tty_layer_dc = NULL;
    }
    free(g_tty_cells);
    g_tty_cells     = NULL;
    g_tty_cells_cap = 0;
    g_tty_layer_ok = false;
}

/* Render the TTY cell grid into the retained layer bitmap. Called only when
 * g_tty_dirty; every frame afterwards just blits the layer (tty_paint).
 *
 * The cells come from the session snapshot, taken under the session lock: the
 * guest thread parses its output into that same VTerm, so the copy is what
 * makes the GDI work below safe to do without holding the lock (and holding it
 * across rendering is exactly what its contract forbids). It also leaves this
 * renderer with no libvterm knowledge at all - colors arrive resolved, wide
 * glyph gaps already zeroed. */
static void tty_layer_render(void)
{
    tty_init_fonts();

    int rows = 0, cols = 0;
    rvvm_tty_lock(g_tty);
    rvvm_tty_get_size(g_tty, &rows, &cols);
    rvvm_tty_unlock(g_tty);
    if (rows < 1 || cols < 1) {
        g_tty_dirty = false;
        return;
    }

    size_t need = (size_t)rows * cols;
    if (need > g_tty_cells_cap) {
        rvvm_tty_cell_t* grown = realloc(g_tty_cells, need * sizeof(*grown));
        if (!grown) {
            g_tty_dirty = false;
            return;
        }
        g_tty_cells     = grown;
        g_tty_cells_cap = need;
    }

    rvvm_tty_view_t view;
    if (rvvm_tty_snapshot(g_tty, g_tty_cells, (int)g_tty_cells_cap, &view) <= 0) {
        g_tty_dirty = false;
        return;
    }

    /* Measure the monospace cell size, then (re)create the layer at exactly
     * the grid's pixel extent. */
    HDC ref = GetDC(NULL);
    SelectObject(ref, tty_fonts[0][0]);
    SIZE ch;
    GetTextExtentPoint32A(ref, "M", 1, &ch);
    int cw = ch.cx, chh = ch.cy;
    int w  = cw * view.cols, h = chh * view.rows;
    g_tty_layer_w = w;
    g_tty_layer_h = h;

    if (!g_tty_layer_dc) {
        g_tty_layer_dc  = CreateCompatibleDC(NULL);
        g_tty_layer_def = (HBITMAP)GetCurrentObject(g_tty_layer_dc, OBJ_BITMAP);
    }
    /* Recreate the bitmap each render: the cell size can change if the font
     * does, and this runs at most once per output burst. */
    if (g_tty_layer_bmp) {
        SelectObject(g_tty_layer_dc, g_tty_layer_def);
        DeleteObject(g_tty_layer_bmp);
        g_tty_layer_bmp = NULL;
    }
    g_tty_layer_bmp = CreateCompatibleBitmap(ref, w, h);
    ReleaseDC(NULL, ref);
    if (!g_tty_layer_dc || !g_tty_layer_bmp) {
        g_tty_layer_ok = false;
        return;
    }
    SelectObject(g_tty_layer_dc, g_tty_layer_bmp);
    HDC cdc = g_tty_layer_dc;
    g_tty_layer_ok = true;

    /* Opaque backdrop (the default background color). */
    RECT back = { 0, 0, w, h };
    FillRect(cdc, &back, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(cdc, TRANSPARENT);

    /* Walk the snapshot and paint same-styled runs of cells. Every run is drawn
     * at col * cw, so text advance never accumulates error across double-width
     * or fallback-font cells. */
    wchar_t wbuf[2 * TTY_COLS];
    const int grid_cols = view.cols;
    for (int r = 0; r < view.rows; r++) {
        const rvvm_tty_cell_t* row = g_tty_cells + (size_t)r * grid_cols;
        int c = 0;
        while (c < grid_cols) {
            const rvvm_tty_cell_t* cell = row + c;
            /* Blank cell, or the gap cell a double-width glyph already covers
             * (the packer zeroes those): no glyph, but a non-default background
             * (SGR set before erase, reverse video) still needs painting. */
            COLORREF bg = 0;
            bool has_bg = tty_cell_bg_argb(cell->bg, &bg);
            if (!cell->cp) {
                if (has_bg) {
                    tty_fill_rect_bg(cdc, c * cw, r * chh, (c + 1) * cw, (r + 1) * chh, bg);
                }
                c++;
                continue;
            }

            COLORREF fg     = tty_argb(cell->fg);
            bool     bold   = (cell->flags & RVT_TTY_BOLD) != 0;
            bool     cjk    = tty_is_cjk(cell->cp);
            bool     dwidth = (cell->flags & RVT_TTY_WIDE) != 0;
            int      run_x  = c;
            int      run_w  = dwidth ? 2 : 1;
            int      n      = 0;

            n += tty_utf16(cell->cp, wbuf + n);

            /* Width-1 runs extend while style and width match; double-width
             * chars always stand alone. */
            if (!dwidth) {
                while (run_x + run_w < grid_cols) {
                    const rvvm_tty_cell_t* nx = row + run_x + run_w;
                    if (!nx->cp || (nx->flags & RVT_TTY_WIDE)) {
                        break;
                    }
                    COLORREF nbg;
                    bool nx_has_bg = tty_cell_bg_argb(nx->bg, &nbg);
                    if (tty_is_cjk(nx->cp) != cjk ||
                        ((nx->flags ^ cell->flags) & RVT_TTY_BOLD) ||
                        nx->fg != cell->fg ||
                        nx_has_bg != has_bg || (nx_has_bg && nx->bg != cell->bg)) {
                        break;
                    }
                    n += tty_utf16(nx->cp, wbuf + n);
                    run_w++;
                }
            }

            int x0 = run_x * cw, x1 = (run_x + run_w) * cw;
            if (has_bg) {
                tty_fill_rect_bg(cdc, x0, r * chh, x1, (r + 1) * chh, bg);
            }
            SelectObject(cdc, tty_fonts[cjk][bold]);
            SetTextColor(cdc, fg);
            TextOutW(cdc, x0, r * chh, wbuf, n);
            c = run_x + run_w;
        }
    }
    g_tty_dirty = false;
}

/* Blit the retained TTY layer onto @cdc (composition DC while running, window
 * DC in the launcher idle view). Renders the layer first when dirty. The
 * caller gates on g_tty_vt + g_tty_seen. Thread-safety: the cell grid is read
 * under the session lock inside tty_layer_render(), which copies it out before
 * any drawing happens, so the UI thread and the guest thread no longer touch
 * the VTerm at the same time. */
static void tty_paint(HDC cdc)
{
    if (g_tty_dirty) {
        tty_layer_render();
    }
    if (g_tty_layer_ok && g_tty_layer_dc) {
        /* The layer bitmap stays selected into g_tty_layer_dc from
         * tty_layer_render; a plain blit needs no per-frame selection. */
        BitBlt(cdc, 0, 0, g_tty_layer_w, g_tty_layer_h,
               g_tty_layer_dc, 0, 0, SRCCOPY);
    }
}


/* Guest environment.
 *
 * The guest gets a deliberately narrow view of the host: its own prefix plus
 * the RVVM_GL_* bring-up switches (test sizes, GL backend tracing) that are
 * meant to be settable from the shell that launched the WinHost. The whole
 * host environ is not forwarded - the guest would inherit unrelated variables
 * and the set would differ per machine.
 *
 * The entries must outlive the guest thread, hence file scope and strdup'd
 * copies collected in guest_env_build()/guest_env_free(). */
#define GUEST_ENV_PREFIX "RVVM_USER_PREFIX="
static char** g_guest_envp = NULL;

/* Host variables handed to the guest, by prefix. */
static const char* guest_env_forward_prefixes[] = {
    "RVVM_GL_",
};

static bool guest_env_forwarded(const char* name)
{
    for (size_t i = 0;
         i < sizeof(guest_env_forward_prefixes) / sizeof(guest_env_forward_prefixes[0]);
         i++) {
        size_t n = strlen(guest_env_forward_prefixes[i]);
        if (strncmp(name, guest_env_forward_prefixes[i], n) == 0) return true;
    }
    return false;
}

static void guest_env_free(void)
{
    if (!g_guest_envp) return;
    for (char** e = g_guest_envp; *e; e++) free(*e);
    free(g_guest_envp);
    g_guest_envp = NULL;
}

/* Copy the forwarded host variables into a NULL-terminated array of
 * "NAME=value" strings, and expose the same prefix to our own getenv. */
static bool guest_env_build(void)
{
    size_t cap = 8, n = 0;
    char** env = (char**)calloc(cap, sizeof(char*));
    if (!env) return false;

    /* Hand the guest the same prefix the host resolved (empty = host paths pass
     * through). The host side reads it through rvvm_user_set_prefix() rather
     * than putenv(): MinGW's putenv("NAME=") removes the variable instead of
     * setting it empty, which rvvm_user.c reads as "use the build-time
     * default". */
    {
        static char env_prefix[1024];
        const char* host_prefix = getenv("RVVM_USER_PREFIX");
        snprintf(env_prefix, sizeof(env_prefix), GUEST_ENV_PREFIX "%s",
                 (host_prefix && host_prefix[0]) ? host_prefix : "");
        env[n] = _strdup(env_prefix);
        if (!env[n]) { free(env); return false; }
        n++;
    }

    {
        /* environ is the live host block; _wenviron is the wide variant, so
         * read the narrow one and match on name. */
        extern char** environ;
        for (char** e = environ; e && *e; e++) {
            const char* eq = strchr(*e, '=');
            if (!eq || eq == *e) continue;
            size_t name_len = (size_t)(eq - *e);
            char name[128];
            if (name_len >= sizeof(name)) continue;
            memcpy(name, *e, name_len);
            name[name_len] = '\0';
            if (!guest_env_forwarded(name)) continue;

            if (n + 2 > cap) {
                size_t ncap = cap * 2;
                char** grown = (char**)realloc(env, ncap * sizeof(char*));
                if (!grown) { guest_env_free(); free(env); return false; }
                env = grown;
                cap = ncap;
            }
            env[n] = _strdup(*e);
            if (!env[n]) { guest_env_free(); return false; }
            n++;
        }
    }

    env[n] = NULL;
    g_guest_envp = env;
    return true;
}

static DWORD WINAPI guest_thread_main(LPVOID arg)
{
    rvvm_machine_t* machine = g_guest_machine;
    /* The cmdpost instance this run was started with. Taken now - the UI thread
     * wrote it before CreateThread, which is a happens-before edge - and
     * released at the end of this function. */
    vp_cmdpost_t* cmdpost = g_cmdpost;
    (void)arg;

    guest_env_build();

    {
        int rc = rvvm_user_linux_ex(machine, g_guest_argc, g_guest_argv,
                                    g_guest_envp);
        /* 0 = guest-driven exit: host_guest_exit_cb() already recorded the
         * real exit code. Nonzero = the guest never started (ELF load
         * failure), the callback never fired, so propagate the error. */
        if (rc != 0) {
            g_guest_rc = rc;
        }
    }

    /* rvvm_user_linux_ex() owns and has just freed the machine: drop our
     * handle before the UI thread could touch it again. */
    g_guest_machine = NULL;

    if (g_guest_argv) {
        int i;
        for (i = 0; i < g_guest_argc; i++) free(g_guest_argv[i]);
        free(g_guest_argv);
        g_guest_argv = NULL;
        g_guest_argc = 0;
    }
    guest_env_free();

    /* This run is over, so its cmdpost instance - and the sensor state hanging
     * off it - goes now. The UI thread reads the pointer again only after the
     * PostMessage below: that is what makes this handoff ordered instead of a
     * race with the next run's create (a guest thread freeing the instance the
     * *next* run had already made is the failure this ordering rules out).
     * Compare before clearing so a shutdown path that already dropped it cannot
     * make this clear its successor. */
    if (g_cmdpost == cmdpost) {
        g_cmdpost = NULL;
    }
    cmdpost_destroy(cmdpost);

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

/* Put the guest-only controls back into their idle state: the Suspend toggle
 * shows "Suspend" again and, like Kill, is enabled only while a guest is
 * actually running. Called on boot and on guest exit so a suspend or a hot Kill
 * left over from the previous guest can never leak into the next one. */
static void launcher_run_controls_ui_reset(void)
{
    bool running = g_guest_thread != NULL;
    if (g_btn_susp) {
        SetWindowTextA(g_btn_susp, "Suspend");
        EnableWindow(g_btn_susp, running);
    }
    if (g_btn_kill) EnableWindow(g_btn_kill, running);
}

/* Create the dropdown + Run/Stop/Kill/Suspend/Exit buttons as children of the
 * host window. */
static void launcher_ui_create(void)
{
    int i;
    HMODULE hinst = GetModuleHandleA(NULL);

    if (g_combo || !g_hwnd) return;
    if (g_guest_count == 0) launcher_scan_guests();

    /* Total height (static box + dropped list), see LAUNCH_COMBO_H. */
    g_combo = CreateWindowA("COMBOBOX", "",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                            CBS_DROPDOWNLIST,
                            LAUNCH_MARGIN, LAUNCH_COMBO_Y, 260, LAUNCH_COMBO_H,
                            g_hwnd, (HMENU)(INT_PTR)IDC_COMBO, hinst, NULL);
    if (g_combo) {
        for (i = 0; i < g_guest_count; i++) {
            SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)(LPCTSTR)g_guest_names[i]);
        }
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
    }

    g_btn_run  = CreateWindowA("BUTTON", "Run",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + LAUNCH_BTN_STEP * 0, LAUNCH_BTN_Y,
                               LAUNCH_BTN_W, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_RUN, hinst, NULL);
    g_btn_stop = CreateWindowA("BUTTON", "Stop",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + LAUNCH_BTN_STEP * 1, LAUNCH_BTN_Y,
                               LAUNCH_BTN_W, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_STOP, hinst, NULL);
    /* Kill: next to Stop because both take the guest down - Stop cooperatively
     * (with a grace period), Kill immediately. Disabled while no guest is
     * running, like Suspend; see launcher_run_controls_ui_reset(). */
    g_btn_kill = CreateWindowA("BUTTON", "Kill",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + LAUNCH_BTN_STEP * 2, LAUNCH_BTN_Y,
                               LAUNCH_BTN_W, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_KILL, hinst, NULL);
    /* Suspend/Resume toggle. Disabled while no guest is running: there is
     * nothing to suspend, and it is re-enabled by launcher_ui_running(). */
    g_btn_susp = CreateWindowA("BUTTON", "Suspend",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + LAUNCH_BTN_STEP * 3, LAUNCH_BTN_Y,
                               LAUNCH_SUSP_W, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_SUSPEND, hinst, NULL);
    g_btn_exit = CreateWindowA("BUTTON", "Exit",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               LAUNCH_MARGIN + LAUNCH_BTN_STEP * 3 +
                                   LAUNCH_SUSP_W + LAUNCH_CTRL_GAP, LAUNCH_BTN_Y,
                               LAUNCH_BTN_W, LAUNCH_CTRL_H,
                               g_hwnd, (HMENU)(INT_PTR)IDC_EXIT, hinst, NULL);
    if (g_btn_susp) EnableWindow(g_btn_susp, FALSE);
    if (g_btn_kill) EnableWindow(g_btn_kill, FALSE);
}

/* Enter the guest-running view. The picker controls stay visible: the parent
 * has WS_CLIPCHILDREN, so the guest panel is composited under them and the
 * combo/buttons keep working (Run switches guests, Stop tears the current one
 * down). Only the frame clock is armed here - it is stopped while idle. */
static void launcher_ui_running(void)
{
    g_launcher_idle = false;
    launcher_run_controls_ui_reset();
    vsync_clock_start();
}

/* Back to the picker: keep the controls up, reset the surface so the next
 * guest starts on a clean panel, and stop the frame clock while idle. */
static void launcher_ui_idle(void)
{
    /* Preserve the current selection: launcher_ui_running() no longer hides
     * the combo, so re-selecting entry 0 here would fight the user's pick. */
    if (g_combo && SendMessageA(g_combo, CB_GETCURSEL, 0, 0) < 0) {
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
    }

    EnterCriticalSection(&g_surf_cs);
    /* Blank the panel (L2) and put the content size (L1) back to the panel's,
     * so the next guest starts on a clean surface. The geometry the previous
     * guest negotiated (SET_BUF) must not leak into it: reset_surface() drops
     * it - format included - and the re-latch right after restores the host
     * default, which is what this used to spell out by hand. */
    {
        int32_t pw, ph;
        vp_session_panel_size(&g_session, &pw, &ph);
        surf_recreate_locked(pw, ph);
    }
    vp_session_reset_surface(&g_session);
    vp_session_guest_geometry(&g_session, NULL, NULL, NULL);
    LeaveCriticalSection(&g_surf_cs);

    vsync_clock_stop();

    launcher_run_controls_ui_reset();

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

/* Boot the given guest into the existing window. Runs on the UI thread
 * (button click handler). */
static void launcher_launch_sel(int sel)
{
    char path[MAX_PATH + 96];
    char* argv[1];

    if (g_guest_count <= 0 || sel < 0 || sel >= g_guest_count) return;

    snprintf(path, sizeof(path), "%s\\%s.exe", g_assets_dir, g_guest_names[sel]);
    if (_access(path, 0) != 0) {
        winhost_log("launcher: guest not found: %s", path);
        return;
    }

    /* This run's cmdpost instance, made here so the startup sequence queued
     * just below lands in it (win32_host_init does the same for the first run,
     * for the same reason). The previous run freed its own as it handed control
     * back, so this is normally NULL. */
    if (!g_cmdpost) {
        g_cmdpost = cmdpost_create();
    }

    /* Boot on a clean slate: anything still queued belongs to the previous
     * guest, or to the idle window (e.g. PAUSE/STOP from a minimize). A stale
     * DESTROY reaching the new guest makes it exit before it ever renders. */
    cmdpost_clear_lifecycle_cmds(g_cmdpost);
    cmdpost_clear_motion_events(g_cmdpost);

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

/* Run button. Now that the picker stays on screen it is reachable while a
 * guest is running, so it switches guests instead of doing nothing: the current
 * one is asked to tear down (see launcher_stop(), which escalates for guests
 * that ignore the lifecycle commands) and the new pick is booted from
 * WM_APP_GUEST_EXIT once it is gone. */
static void launcher_launch(void)
{
    int sel;

    if (g_guest_count <= 0 || !g_combo) return;
    sel = (int)SendMessageA(g_combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_guest_count) return;

    if (g_guest_thread) {
        g_switch_sel     = sel;
        g_switch_pending = true;
        winhost_log("launcher: switching to %s once %s exits",
                    g_guest_names[sel], "the running guest");
        launcher_stop();
        return;
    }
    launcher_launch_sel(sel);
}

/* Stop the running guest. Cooperative first: queue the Android teardown and
 * let the guest exit on its own (WM_APP_GUEST_EXIT then returns us to the
 * picker). A guest must poll its lifecycle commands for that to work, so a
 * watchdog is armed as well - see the WM_TIMER handler for what happens when
 * the grace period expires with the guest still alive. */
static void launcher_stop(void)
{
    if (!g_guest_thread) return;
    /* A suspended guest cannot poll the teardown, so the cooperative stop would
     * just sit out the grace period and end in a forced rvvm_user_stop(). Wake
     * it first: it then exits the normal way. */
    if (guest_suspended()) {
        guest_resume();
        launcher_run_controls_ui_reset();
        if (!g_minimized) vsync_clock_start();
        winhost_log("Stop: resuming the suspended guest first");
    }
    queue_lifecycle(APP_CMD_PAUSE);
    queue_lifecycle(APP_CMD_STOP);
    queue_lifecycle(APP_CMD_DESTROY);
    if (!g_stop_watchdog) {
        SetTimer(g_hwnd, STOP_TIMER_ID, STOP_GRACE_MS, NULL);
        g_stop_watchdog = true;
    }
    winhost_log("Stop requested: teardown lifecycle queued (%d ms grace)", STOP_GRACE_MS);
}

/* Kill button. Takes the running guest down *now*, skipping the cooperative
 * teardown grace period Stop waits out: same termination path as the Stop
 * watchdog, i.e. rvvm_user_stop() pauses every guest vCPU and marks every guest
 * thread finished, so the guest still unwinds through its own exit path and
 * WM_APP_GUEST_EXIT brings the picker back. (This is deliberately not
 * TerminateThread() on a thread that is inside guest interpreter code.)
 *
 * Any cooperative teardown a previous Stop queued is superseded: its pending
 * watchdog is disarmed here so it cannot fire against the *next* guest that the
 * picker boots. A parked (suspended) guest needs no explicit resume -
 * rvvm_user_stop() wakes the parked vCPUs as part of the exit path itself. */
static void launcher_kill(void)
{
    if (!g_guest_thread || !g_guest_machine) return;

    if (g_stop_watchdog) {
        KillTimer(g_hwnd, STOP_TIMER_ID);
        g_stop_watchdog = false;
    }

    winhost_log("Kill: forcing rvvm_user_stop() now (no grace period)");
    rvvm_user_stop(g_guest_machine, STOP_FORCED_EXIT_CODE);
}

/* Suspend/Resume toggle. Suspending the vCPUs also parks the frame clock: a
 * parked guest polls neither frames nor lifecycle commands, so a running clock
 * would only pile up vsync ticks for it to burn through on resume. */
static void launcher_toggle_suspend(void)
{
    if (!g_guest_thread) return;

    if (guest_suspended()) {
        guest_resume();
        SetWindowTextA(g_btn_susp, "Suspend");
        if (!g_minimized) vsync_clock_start();
        winhost_log("Suspend: guest resumed");
    } else {
        bool parked = rvvm_user_suspend(g_guest_machine);
        SetWindowTextA(g_btn_susp, "Resume");
        vsync_clock_stop();
        winhost_log("Suspend: vCPUs %s",
                    parked ? "parked" : "parking (one is in a blocking syscall)");
    }
}

/* ============================================================
 * Bundled assets (the /assets mount)
 *
 * The WinHost has no APK: its asset tree is a real directory - the same one the
 * launcher lists guests from (--assets / RVVM_ASSETS) - so every op here is a
 * plain file call. The descriptors the mount hands out are ordinary seekable
 * files, a guest can lseek() an asset like any other file, and nothing is
 * buffered and nothing needs a thread.
 * ============================================================ */

/* Resolve an asset name under g_assets_dir, refusing anything that climbs out of
 * it: the tree belongs to the guest and must not turn into a way to read the
 * host's disk. Returns false when @name is not a plain relative path. */
static bool asset_host_path(char* out, size_t outsz, const char* name)
{
    if (!name || !*name || name[0] == '/' || name[0] == '\\' || strstr(name, "..")) {
        return false;
    }
    /* APK asset names use '/', Windows accepts it as a separator too, so a nested
     * name like "shaders/basic.glsl" resolves as-is. */
    snprintf(out, outsz, "%s\\%s", g_assets_dir, name);
    return true;
}

static int win32_asset_open_fd(void* user, const char* name)
{
    char path[MAX_PATH + 128];
    int fd;

    (void)user;

    if (!asset_host_path(path, sizeof(path), name)) {
        return -EINVAL;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -ENOENT;
    }
    /* The guest reads raw bytes: no CRLF translation on the way through. */
    _setmode(fd, _O_BINARY);
    return fd;
}

static int64_t win32_asset_size(void* user, const char* name)
{
    char path[MAX_PATH + 128];
    struct stat st;

    (void)user;

    if (!asset_host_path(path, sizeof(path), name)) {
        return -EINVAL;
    }
    if (stat(path, &st) != 0) {
        return -ENOENT;
    }
    /* A directory says so by refusing to answer a size - the core then asks
     * open_dir() and reports S_IFDIR. */
    if (S_ISDIR(st.st_mode)) {
        return -EISDIR;
    }
    return (int64_t)st.st_size;
}

static void* win32_asset_open_dir(void* user, const char* name)
{
    char path[MAX_PATH + 128];

    (void)user;

    if (!name || !*name) {
        /* The mount root is the asset directory itself. */
        snprintf(path, sizeof(path), "%s", g_assets_dir);
    } else if (!asset_host_path(path, sizeof(path), name)) {
        return NULL;
    }
    return opendir(path);
}

static const char* win32_asset_dir_next(void* user, void* dir)
{
    struct dirent* de;
    (void)user;
    de = readdir(dir);
    return de ? de->d_name : NULL;   /* "." and ".." are the core's to report */
}

static void win32_asset_dir_close(void* user, void* dir)
{
    (void)user;
    closedir(dir);
}

static const rvvm_asset_ops_t win32_asset_ops = {
    .open_fd   = win32_asset_open_fd,
    .size      = win32_asset_size,
    .open_dir  = win32_asset_open_dir,
    .dir_next  = win32_asset_dir_next,
    .dir_close = win32_asset_dir_close,
};

void win32_host_set_assets_dir(const char* dir)
{
    if (dir && *dir) {
        snprintf(g_assets_dir, sizeof(g_assets_dir), "%s", dir);
    }
}

/* (Re)register every host-side cmdpost callback. Done once at init and again
 * before each launch.
 *
 * This used to be load-bearing: the core's guest-exit path called
 * cmdpost_cleanup(), which NULLed all callbacks and dropped the AAudio backend,
 * so a relaunched guest would probe a dead proxy (AAUDIO_QUERY -> 0 caps) and
 * exit(1) before ever reaching main(). The core now ends only the run
 * (cmdpost_end_run()) and leaves the host's registrations alone, so re-running
 * this is an idempotent belt rather than the thing that keeps relaunch working.
 * Runs on the UI thread before the guest thread exists, so there is no race
 * with in-flight guest dispatches. */
static void win32_cmdpost_register_callbacks(void)
{
    /* Device-level, so it outlives the run: the per-guest half (descriptor
     * table, queues) is the cmdpost instance's and is dropped by
     * vp_sensor_reset() when that guest exits. Re-registering per launch is an
     * idempotent restatement. */
    vp_sensor_set_ops(win32_sensor_stub_ops());
    cmdpost_set_window_callbacks(g_cmdpost, on_window_lock, on_window_unlock);
    cmdpost_set_window_size_callback(g_cmdpost, on_window_size);
    cmdpost_set_window_set_buf_callback(g_cmdpost, on_window_set_buf);
    cmdpost_set_config_callback(g_cmdpost, on_config_get);
    cmdpost_set_game_callbacks(g_cmdpost, on_game_lifecycle, on_game_input);
    /* Phase 3: GL/EGL dispatch callbacks */
    cmdpost_set_gl_callbacks(g_cmdpost, on_egl_dispatch, on_gl_dispatch);
    /* Phase 4: vsync source. Registering the blocker makes the guest advertise
     * the AChoreographer caps and, once the clock thread is up, it drives the
     * fd-wakeup path the guest's Looper polls. */
    cmdpost_set_choreographer_callback(g_cmdpost, on_choreographer_wait);
    /* Phase 5: AAudio backend (WASAPI). query() reports 0 caps when no audio
     * device exists, so every AAudio call on the guest fails gracefully. */
    cmdpost_set_audio_callbacks(g_cmdpost, win32_aaudio_ops());
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
     * virtual panel. It stays state of this file - it is the one layer the
     * session has no notion of, because no other host has one. */
    g_win_w = (win_w > 0) ? win_w : 1024;
    g_win_h = (win_h > 0) ? win_h : 768;

    /* Layer 1: the host-owned virtual display the guest renders into. When no
     * explicit panel geometry is requested it follows the window size, so the
     * panel fills the viewport without borders. Pinned here and never moved;
     * the guest's surface latches from it on first use, and the session keeps
     * that snapshot (init_*) as the fallback - the same hand-off the Android
     * host performs through its latch. */
    vp_session_init(&g_session);
    vp_session_set_panel(&g_session, (virt_w > 0) ? virt_w : g_win_w,
                                    (virt_h > 0) ? virt_h : g_win_h);
    vp_session_set_density(&g_session,
                           (virt_ppi > 0) ? virt_ppi : ACONFIGURATION_DENSITY_MEDIUM);

    winhost_log("window: %dx%d px | virtual display: %dx%d px @ %d ppi (bucket %d)",
                g_win_w, g_win_h, g_session.panel_w, g_session.panel_h,
                g_session.panel_ppi, density_bucket_for_ppi(g_session.panel_ppi));

    SetProcessDPIAware();
    InitializeCriticalSection(&g_surf_cs);
    g_cs_ready = true;

    /* The cmdpost instance for the first run, created before the window exists
     * because WM_CREATE queues the guest's startup sequence (APP_CMD_START and
     * friends) into it: an instance made after CreateWindowExA would swallow
     * them and the guest would never receive them. From the second run on, the
     * launcher creates one in launcher_launch_sel(), and each run frees its own
     * in guest_thread_main() as it hands control back. */
    if (!g_cmdpost) {
        g_cmdpost = cmdpost_create();
    }

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

    /* WS_CLIPCHILDREN: the launcher's idle WM_PAINT fills the whole client
     * area, and the guest view blits the scaled panel over it. Without it the
     * parent paints straight over the dropdown and the buttons, erasing them
     * until they happen to repaint (visible as a flicker / "dead" controls). */
    g_hwnd = CreateWindowExA(0, wc.lpszClassName, title,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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

    win32_cmdpost_register_callbacks();
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

    /* Reinstall the callbacks for this guest. The previous guest's exit no
     * longer clears them (cmdpost_end_run() ends the run, not the bridge), so
     * this is an idempotent belt rather than what keeps a relaunched guest
     * from probing a dead proxy. */
    win32_cmdpost_register_callbacks();

    /* Per-guest diagnostics: report this guest's first lock/frame too. */
    g_logged_lock  = false;
    g_logged_frame = false;

    /* Fresh userland instance per guest: its context (memory, harts, syscall
     * state) is fully owned by the machine, so nothing leaks between guests. */
    g_guest_machine = rvvm_user_create();
    if (!g_guest_machine) {
        winhost_log("rvvm_user_create failed");
        return false;
    }
    /* Guest exit: record the real exit code and route sys_exit through the
     * graceful unwind instead of rvvm_user.c's _Exit() fallback (see
     * host_guest_exit_cb). Same role as jni_bridge.c's on_guest_exit. */
    rvvm_user_set_exit_callback(g_guest_machine, host_guest_exit_cb);

    /* Guest filesystem view: host paths pass through unchanged unless the
     * launching shell asked for a prefix directory (RVVM_USER_PREFIX pointing
     * at a real rootfs). This must not rely on the environment alone - MinGW's
     * putenv("NAME=") *removes* the variable instead of setting it empty, and
     * rvvm_user.c reads a removed variable as "keep the build-time default",
     * which prefixes every guest absolute path with a directory that does not
     * exist on this host. */
    {
        const char* host_prefix = getenv("RVVM_USER_PREFIX");
        rvvm_user_set_prefix(g_guest_machine,
                             (host_prefix && host_prefix[0]) ? host_prefix : NULL);
    }

    /* Route guest fd 1/2 through the console session so CR / ANSI escapes
     * render correctly (the win32 host's stdout is a real tty today, but the
     * renderer draws the parsed screen matrix, which is what makes in-place
     * updates consistent across hosts). The session is host-owned: opened once
     * and reset per launch, so the last screen stays renderable after the guest
     * exits. */
    if (!g_tty) {
        /* First launch: the session is created here, and it owns the VTerm, the
         * lock that serializes access to it, the scrollback and the view.
         * UTF-8 and the initial screen reset are part of rvvm_tty_open. */
        g_tty = rvvm_tty_open(TTY_ROWS, TTY_COLS);
        if (g_tty) {
            g_tty_vt = rvvm_tty_vterm(g_tty);
        }
    } else {
        rvvm_tty_reset(g_tty);
    }
    g_tty_dirty = true;  /* fresh empty screen renders into the layer */
    g_tty_seen  = false; /* no guest output yet this launch */
    rvvm_tty_attach(g_tty, g_guest_machine);
    rvvm_user_set_tty_callback(g_guest_machine, host_tty_cb, NULL);

    /* Whoever queued this run's startup sequence made its instance (see
     * win32_host_init / launcher_launch_sel, and why it has to be before the
     * queueing). This is only for a caller that went straight here. */
    if (!g_cmdpost) {
        g_cmdpost = cmdpost_create();
    }

    /* Bind this run's machine to the host's cmdpost instance: it is how a
     * syscall arriving on a guest thread finds the state it belongs to. Done
     * before the thread is created, since the guest's first act may be one of
     * these syscalls. */
    rvvm_user_set_host_ctx(g_guest_machine, g_cmdpost);

    /* The host's asset tree, mounted at /assets. This host's tree is a real
     * directory, so the mount serves plain seekable files; see the ops above.
     * Registered per run like the rest of the host context. */
    rvvm_user_set_assets(g_guest_machine, &win32_asset_ops, NULL);

    g_guest_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    if (!g_guest_argv) {
        rvvm_user_free(g_guest_machine);
        g_guest_machine = NULL;
        return false;
    }
    for (i = 0; i < argc; i++) {
        g_guest_argv[i] = _strdup(argv[i]);
        if (!g_guest_argv[i]) {
            while (--i >= 0) free(g_guest_argv[i]);
            free(g_guest_argv);
            g_guest_argv = NULL;
            rvvm_user_free(g_guest_machine);
            g_guest_machine = NULL;
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
        rvvm_user_free(g_guest_machine);
        g_guest_machine = NULL;
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

    /* A suspended guest is parked in its wrap loop and cannot poll the teardown
     * WM_CLOSE queued, so wake it: it then takes the cooperative path below
     * instead of being forced down. No-op when it was not suspended. */
    guest_resume();

    /* Stop the audio backend before the guest thread: the WASAPI pump threads
     * must be joined before cmdpost_cleanup() tears down the stream table. */
    win32_aaudio_shutdown();

    bool guest_stopped = true;
    if (g_guest_thread) {
        if (g_stop_watchdog) {
            KillTimer(g_hwnd, STOP_TIMER_ID);
            g_stop_watchdog = false;
        }
        /* Give a well-behaved guest the same grace period the Stop button
         * does before taking it down from the host side. WM_CLOSE already
         * queued the teardown, so whoever is left after this ignored it (or
         * wedged) and would otherwise burn the whole budget below and force
         * us to skip post-guest cleanup. */
        if (WaitForSingleObject(g_guest_thread, STOP_GRACE_MS) == WAIT_TIMEOUT) {
            winhost_log("guest did not exit within %d ms; forcing rvvm_user_stop()",
                        STOP_GRACE_MS);
            rvvm_user_stop(g_guest_machine, STOP_FORCED_EXIT_CODE);
        }
        /* Bounded wait: a guest also stuck in a blocking host syscall must not
         * make a graceful exit hang forever (CTRL_CLOSE_EVENT only grants ~5s
         * before the OS kills us). */
        if (WaitForSingleObject(g_guest_thread, GUEST_EXIT_TIMEOUT_MS - STOP_GRACE_MS) == WAIT_TIMEOUT) {
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
    /* A guest run frees its own instance as it hands control back (see
     * guest_thread_main), so this is for the case where one was made and no run
     * ever took it - a launch that failed after the window was up. The guest is
     * stopped by this point, so nothing can be using it. */
    cmdpost_destroy(g_cmdpost);
    g_cmdpost = NULL;
    if (g_cs_ready) {
        DeleteCriticalSection(&g_surf_cs);
        g_cs_ready = false;
    }
}
