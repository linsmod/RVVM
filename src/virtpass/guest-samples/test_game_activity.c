/*
 * test_game_activity.c - Test program for GameActivity lifecycle + input
 *
 * This program tests the GameActivity API stubs:
 * - android_app_create/destroy
 * - android_app_read_cmd (lifecycle commands)
 * - android_app_swap_input_buffers (input events)
 * - android_app_clear_motion_events/key_events
 *
 * Runs as an endless game loop (app-like behavior): pumps lifecycle
 * commands, swaps input buffers and renders touch dots + state bar.
 *
 * The lifecycle is handled as a state machine over the APP_CMD_* stream (see
 * the flags below), not as "the last command received": that is what lets the
 * guest go back to showing its normal rendered content after the Activity was
 * backgrounded and resumed.
 *
 * Build (RISC-V cross-compiler):
 *   riscv64-unknown-linux-gnu-gcc -static -O2 -o test_game_activity.exe test_game_activity.c -L../vp_ndk_stub -landroid-stub
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Include the GameActivity API stubs */
#include "virtpass/vp_android.h"

/* Lifecycle command names */
static const char* cmd_names[] = {
    "INPUT_CHANGED",
    "INIT_WINDOW",
    "TERM_WINDOW",
    "WINDOW_RESIZED",
    "WINDOW_REDRAW_NEEDED",
    "CONTENT_RECT_CHANGED",
    "GAINED_FOCUS",
    "LOST_FOCUS",
    "CONFIG_CHANGED",
    "LOW_MEMORY",
    "START",
    "RESUME",
    "SAVE_STATE",
    "PAUSE",
    "STOP",
    "DESTROY",
    "WINDOW_INSETS_CHANGED",
};

/*
 * Lifecycle state.
 *
 * Android never hands the guest a single "current state" value: the APP_CMD_*
 * stream is a sequence of transitions whose order is not fixed (the surface
 * can appear before or after RESUME, TERM_WINDOW can land after STOP, and a
 * burst of transitions is drained at once if the guest was not polling). So
 * keep one flag per transition and derive the display state from all of them.
 * That is what makes "resume after backgrounding" restore the previous
 * rendered state instead of whatever command happened to arrive last.
 */
static bool g_has_window = false;
static bool g_started    = false;
static bool g_resumed    = false;
static bool g_focused    = false;
static bool g_destroy_requested = false;

/* True while the guest is on screen and may present its normal content. */
static bool is_live(void)
{
    return g_has_window && g_started && g_resumed;
}

/* Background colour for the current lifecycle state (RGBA_8888). */
static uint32_t lifecycle_bg_color(void)
{
    if (g_destroy_requested) return 0xFFE65100; /* orange     - destroying   */
    if (!g_has_window)       return 0xFF1B1B1B; /* dark grey  - no surface   */
    if (!g_started)          return 0xFF263238; /* blue grey  - stopped      */
    if (!g_resumed)          return 0xFFB71C1C; /* dark red   - paused       */
    return 0xFF0B3D14;                          /* dark green - live content */
}

/* Touch tracking: one slot per active pointer, keyed by pointer id */
#define MAX_TOUCH_POINTERS 16
static float g_touch_x[MAX_TOUCH_POINTERS];
static float g_touch_y[MAX_TOUCH_POINTERS];
static bool  g_touch_active[MAX_TOUCH_POINTERS];
static int g_touch_count = 0;

/*
 * Dump the layer-1 virtual display (the guest's "target machine") exactly as
 * a guest can observe it: only through the public NDK ABI, i.e.
 * ANativeWindow_getWidth/Height and the AConfiguration_* getters.
 */
static void print_virtual_display(void)
{
    static const char* size_names[]   = { "ANY", "SMALL", "NORMAL", "LARGE", "XLARGE" };
    static const char* tri_names[]    = { "ANY", "NO", "YES" };
    static const char* orient_names[] = { "ANY", "PORT", "LAND", "SQUARE" };

    AConfiguration* cfg;
    int32_t size, slong, sround, orient;

    printf("GameActivity: --- layer-1 virtual display ---\n");
    printf("GameActivity:   panel      : %d x %d px\n",
           (int)ANativeWindow_getWidth(NULL), (int)ANativeWindow_getHeight(NULL));

    cfg = AConfiguration_new();
    if (!cfg) {
        printf("GameActivity:   config     : unavailable\n");
        return;
    }

    size   = AConfiguration_getScreenSize(cfg);
    slong  = AConfiguration_getScreenLong(cfg);
    sround = AConfiguration_getScreenRound(cfg);
    orient = AConfiguration_getOrientation(cfg);

    printf("GameActivity:   density    : %d dpi\n", (int)AConfiguration_getDensity(cfg));
    printf("GameActivity:   size       : %s\n",
           (size >= 0 && size <= 4) ? size_names[size] : "?");
    printf("GameActivity:   widthDp    : %d dp\n", (int)AConfiguration_getScreenWidthDp(cfg));
    printf("GameActivity:   heightDp   : %d dp\n", (int)AConfiguration_getScreenHeightDp(cfg));
    printf("GameActivity:   long/round : %s / %s\n",
           (slong >= 0 && slong <= 2) ? tri_names[slong] : "?",
           (sround >= 0 && sround <= 2) ? tri_names[sround] : "?");
    printf("GameActivity:   orientation: %s\n",
           (orient >= 0 && orient <= 3) ? orient_names[orient] : "?");
    printf("GameActivity: ---------------------------------\n");

    AConfiguration_delete(cfg);
}

/* Simple callback handler */
void on_app_cmd(struct android_app* app, int cmd)
{
    (void)app;

    if (cmd >= 0 && cmd < sizeof(cmd_names) / sizeof(cmd_names[0])) {
        printf("GameActivity: Received command: %s (%d)\n", cmd_names[cmd], cmd);
    } else {
        printf("GameActivity: Unknown command: %d\n", cmd);
    }

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            printf("GameActivity: Window initialized\n");
            print_virtual_display();
            g_has_window = true;
            break;
        case APP_CMD_TERM_WINDOW:
            printf("GameActivity: Window terminated\n");
            g_has_window = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            printf("GameActivity: Gained focus\n");
            g_focused = true;
            break;
        case APP_CMD_LOST_FOCUS:
            printf("GameActivity: Lost focus\n");
            g_focused = false;
            break;
        case APP_CMD_START:
            printf("GameActivity: Started\n");
            g_started = true;
            break;
        case APP_CMD_STOP:
            printf("GameActivity: Stopped\n");
            g_started = false;
            break;
        case APP_CMD_RESUME:
            printf("GameActivity: Resumed\n");
            g_resumed = true;
            break;
        case APP_CMD_PAUSE:
            printf("GameActivity: Paused\n");
            g_resumed = false;
            break;
        case APP_CMD_DESTROY:
            printf("GameActivity: Destroy requested\n");
            g_destroy_requested = true;
            break;
        default:
            break;
    }

    /* One line per transition telling exactly which state the guest derived,
     * so logcat shows the resume/background cycle end to end. */
    printf("GameActivity: state window=%d started=%d resumed=%d focused=%d -> %s\n",
           (int)g_has_window, (int)g_started, (int)g_resumed, (int)g_focused,
           is_live() ? "LIVE (showing rendered content)"
                     : "NOT presenting content");
}

/* Fill a pixel with RGBA color */
static inline void put_pixel(ANativeWindow_Buffer* buf, int x, int y, uint32_t color)
{
    if (!buf || !buf->bits) return;
    if (x < 0 || y < 0 || x >= buf->width || y >= buf->height) return;
    uint32_t* pixels = (uint32_t*)buf->bits;
    pixels[y * buf->stride + x] = color;
}

/* Render the frame: background + a bottom state bar + touch dot */
static void render_frame(ANativeWindow_Buffer* buf, int frame)
{
    if (!buf || !buf->bits) return;
    int width = buf->width;
    int height = buf->height;
    int stride = buf->stride;
    uint32_t* pixels = (uint32_t*)buf->bits;
    uint32_t bg = lifecycle_bg_color();
    bool live = is_live();

    /* Solid background. The colour encodes the lifecycle state, so the guest
     * turns green again as soon as it is resumed on screen. */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = bg;
        }
    }

    /* Animated top strip (proves the loop is running). The animation is slow on
     * purpose: stepping the colour every frame wraps all three channels several
     * times a second, which reads as flicker rather than motion. This is a
     * highlight band sweeping across the strip roughly every 3 seconds. */
    int strip_h = height / 12;
    if (strip_h > 0 && width > 0) {
        const int period = 180;          /* frames per sweep (~3 s at 60 fps) */
        int band = width / 10;
        if (band < 8) band = 8;
        int pos = (frame % period) * width / period;
        for (int y = 0; y < strip_h; y++) {
            for (int x = 0; x < width; x++) {
                int d = x - pos;
                if (d < 0) d = -d;
                if (d > width - d) d = width - d;   /* wrap around the strip */
                pixels[y * stride + x] = (d < band) ? 0xFF20D0FFu : 0xFF184058u;
            }
        }
    }

    /* State bar at the bottom: bright green while live, otherwise the colour
     * of the lifecycle state the guest is parked in. */
    int bar_h = height / 16;
    uint32_t bar_color = live ? 0xFF35E07A : (bg ^ 0x00FFFF00);
    for (int y = height - bar_h; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = bar_color;
        }
    }

    /* Touch dots: one per active pointer (multi-touch) */
    {
        int r = width / 40;
        if (r < 8) r = 8;
        for (int i = 0; i < MAX_TOUCH_POINTERS; i++) {
            if (!g_touch_active[i] || g_touch_x[i] < 0 || g_touch_y[i] < 0) {
                continue;
            }
            int cx = (int)g_touch_x[i];
            int cy = (int)g_touch_y[i];
            uint32_t dot_color = (i == 0) ? 0xFFFF0000u : 0xFFFFFF00u;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy <= r * r) {
                        put_pixel(buf, cx + dx, cy + dy, dot_color);
                    }
                }
            }
            printf("GameActivity: drawing touch dot %d at %d,%d\n", i, cx, cy);
        }
    }
}

/* ============================================================
 * Frame callback (runs once per display vsync)
 * ============================================================ */

static int g_render_frame = 0;

static void on_frame_callback(long frame_time_nanos, void* data)
{
    struct android_app* app = (struct android_app*)data;
    (void)frame_time_nanos;

    /* 1. Pump lifecycle commands */
    int32_t cmd;
    while ((cmd = android_app_read_cmd(app)) != -1) {
        android_app_exec_cmd(app, cmd);
    }

    /* 2. Swap input buffers */
    int32_t motion_count = android_app_swap_input_buffers(app);
    if (motion_count > 0) {
        g_touch_count += motion_count;
        for (int32_t i = 0; i < motion_count && i < 16; i++) {
            GameActivityMotionEvent* me = &app->inputBuffer.motionEvents[i];
            int32_t action = me->action & AMOTION_EVENT_ACTION_MASK;
            int32_t action_index =
                (me->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            int32_t pcount = me->pointerCount;
            if (pcount < 1) pcount = 1;
            if (pcount > MAX_TOUCH_POINTERS) pcount = MAX_TOUCH_POINTERS;
            printf("GameActivity: motion event %d action=%d index=%d pointers=%d\n",
                   i, action, action_index, pcount);

            /* A cancel lifts every pointer at once. */
            if (action == AMOTION_EVENT_ACTION_CANCEL) {
                for (int p = 0; p < MAX_TOUCH_POINTERS; p++) {
                    g_touch_active[p] = false;
                }
                continue;
            }

            for (int32_t p = 0; p < pcount; p++) {
                GameActivityPointerAxes* axes = &me->pointers[p];
                float x = GameActivityPointerAxes_getAxisValue(axes, AMOTION_EVENT_AXIS_X);
                float y = GameActivityPointerAxes_getAxisValue(axes, AMOTION_EVENT_AXIS_Y);
                int slot = axes->id % MAX_TOUCH_POINTERS;

                /* Only the pointer named by actionIndex lifts on POINTER_UP/UP;
                 * every other pointer in the event is still down. */
                bool lifting = (action == AMOTION_EVENT_ACTION_UP ||
                                action == AMOTION_EVENT_ACTION_POINTER_UP)
                               && p == action_index;

                g_touch_x[slot] = x;
                g_touch_y[slot] = y;
                g_touch_active[slot] = !lifting;
            }
        }
        android_app_clear_motion_events(app);
        android_app_clear_key_events(app);
    }

    /* 3. Render one frame.
     *
     * The frame callback always re-arms (step 4), so the guest keeps drawing
     * for every lifecycle state. "Live" draws the usual content, any other
     * state draws the same scene tinted by the state; only the presentation
     * itself is skipped when the host has no surface to present on.
     *
     * g_render_frame advances only for frames that actually reached the
     * display, so the animation phase (and everything else the guest keeps in
     * memory: touch dots, counters) is exactly where it was left when the
     * guest comes back from a background trip. */
    if (g_has_window) {
        ANativeWindow_Buffer buffer;
        ARect dirty;
        if (ANativeWindow_lock(NULL, &buffer, &dirty) == 0) {
            render_frame(&buffer, g_render_frame);
            ANativeWindow_unlockAndPost(NULL);
            g_render_frame++;
        } else {
            /* ANativeWindow_lock() failed. This does NOT imply the window is
             * missing: vp_ndk_stub prints the actual cause on stderr (host
             * LOCK refused vs. guest pixel-buffer malloc failure). The next
             * vsync tick retries automatically. */
            printf("GameActivity: ANativeWindow_lock() failed, retrying "
                   "(cause reported by vp_ndk_stub)\n");
        }
    }

    /* 4. Re-arm for the next vsync (standard Choreographer pattern). */
    if (!g_destroy_requested) {
        AChoreographer_postFrameCallback(AChoreographer_getInstance(),
                                         on_frame_callback, app);
    }
}

int main(void)
{
    printf("=== GameActivity Test Program ===\n\n");

    /* Create android_app */
    printf("1. Creating android_app...\n");
    struct android_app* app = android_app_create();
    if (!app) {
        printf("   FAILED: android_app_create returned NULL\n");
        return 1;
    }
    printf("   OK: android_app created\n");

    /* Set callback handler */
    app->onAppCmd = (app_cmd_handler)on_app_cmd;

    /* Set buffer format to RGBA_8888 (matching render_frame) */
    ANativeWindow_setBuffersGeometry(NULL, 0, 0, WINDOW_FORMAT_RGBA_8888);

    /* Main loop: rendering is driven by the display vsync, NDK-style. The
     * frame callback re-arms itself, so all the main thread has to do is pump
     * the Looper.
     *
     * Use ALooper_pollOnce(), NOT ALooper_pollAll(): pollAll keeps draining
     * while the poll result is ALOOPER_POLL_CALLBACK, and the vsync frame
     * callback always reports exactly that - so with the display running
     * pollAll never returns and the loop below never re-evaluates
     * g_destroy_requested. DESTROY would be consumed (and the callback chain
     * stopped) yet the program would never leave the loop: pollOnce returns
     * after each dispatched callback so the exit condition is actually seen. */
    AChoreographer_getInstance();
    AChoreographer_postFrameCallback(AChoreographer_getInstance(),
                                     on_frame_callback, app);

    while (!g_destroy_requested) {
        ALooper_pollOnce(-1, NULL, NULL, NULL);
    }

    /* Destroy android_app */
    printf("\n5. Destroying android_app...\n");
    android_app_destroy(app);
    printf("   OK: android_app destroyed\n");

    printf("\n=== Test Complete ===\n");
    return 0;
}