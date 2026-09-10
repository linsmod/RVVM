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

/* Per-lifecycle-state background colors (RGBA_8888) */
static uint32_t state_colors[] = {
    0xFF1B1B1B, /* INIT_WINDOW  - dark gray */
    0xFF1B1B1B, /* TERM_WINDOW  - dark gray */
    0xFF1B5E20, /* RESUME       - dark green  */
    0xFFB71C1C, /* PAUSE        - dark red    */
    0xFFE65100, /* DESTROY      - orange      */
};

static uint32_t g_bg_color = 0xFF1B1B1B;
static bool g_destroy_requested = false;

/* Touch tracking */
static float g_touch_x = -1, g_touch_y = -1;
static bool g_touch_active = false;
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
            g_bg_color = state_colors[0];
            break;
        case APP_CMD_TERM_WINDOW:
            printf("GameActivity: Window terminated\n");
            g_bg_color = state_colors[1];
            break;
        case APP_CMD_RESUME:
            printf("GameActivity: Resumed\n");
            g_bg_color = state_colors[2];
            break;
        case APP_CMD_PAUSE:
            printf("GameActivity: Paused\n");
            g_bg_color = state_colors[3];
            break;
        case APP_CMD_DESTROY:
            printf("GameActivity: Destroy requested\n");
            g_bg_color = state_colors[4];
            g_destroy_requested = true;
            break;
        default:
            break;
    }
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

    /* Solid background */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = g_bg_color;
        }
    }

    /* Animated top strip (proves the loop is running) */
    int strip_h = height / 12;
    uint32_t strip_color = ((uint32_t)(frame * 40) << 16) | ((uint32_t)(frame * 80) << 8) | (uint32_t)(frame * 120);
    for (int y = 0; y < strip_h; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = strip_color;
        }
    }

    /* State bar at the bottom */
    int bar_h = height / 16;
    for (int y = height - bar_h; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = g_bg_color ^ 0x00FFFF00;
        }
    }

    /* Touch dot */
    if (g_touch_active && g_touch_x >= 0 && g_touch_y >= 0) {
        int cx = (int)g_touch_x;
        int cy = (int)g_touch_y;
        int r = width / 40;
        if (r < 8) r = 8;
        uint32_t dot_color = 0xFFFF0000;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) {
                    put_pixel(buf, cx + dx, cy + dy, dot_color);
                }
            }
        }
        printf("GameActivity: drawing touch dot at %d,%d\n", cx, cy);
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
            GameActivityPointerAxes* p = &app->inputBuffer.motionEvents[i].pointers[0];
            int32_t action = app->inputBuffer.motionEvents[i].action & AMOTION_EVENT_ACTION_MASK;
            float x = GameActivityPointerAxes_getAxisValue(p, AMOTION_EVENT_AXIS_X);
            float y = GameActivityPointerAxes_getAxisValue(p, AMOTION_EVENT_AXIS_Y);
            printf("GameActivity: motion event %d action=%d at %.0f,%.0f\n", i, action, x, y);

            if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE) {
                g_touch_x = x;
                g_touch_y = y;
                g_touch_active = true;
            } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
                g_touch_active = false;
            }
        }
        android_app_clear_motion_events(app);
        android_app_clear_key_events(app);
    }

    /* 3. Render one frame */
    {
        ANativeWindow_Buffer buffer;
        ARect dirty;
        if (ANativeWindow_lock(NULL, &buffer, &dirty) == 0) {
            render_frame(&buffer, g_render_frame);
            ANativeWindow_unlockAndPost(NULL);
        } else {
            /* ANativeWindow_lock() failed. This does NOT imply the window is
             * missing: vp_ndk_stub prints the actual cause on stderr (host
             * LOCK refused vs. guest pixel-buffer malloc failure). The next
             * vsync tick retries automatically. */
            printf("GameActivity: ANativeWindow_lock() failed, retrying "
                   "(cause reported by vp_ndk_stub)\n");
        }
    }
    g_render_frame++;

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