/*
 * vp_cmdpost.c - Host-side dispatch for Android NDK API proxy
 *
 * This library is called by rvvm-user when it receives custom syscalls
 * (0x10000+) from the Guest. It dispatches these to real Android NDK/JNI
 * APIs and returns the results back to the Guest.
 *
 * Build: Linked into the rvvm-user binary or loaded as a shared library
 * Target: Android host (ARM64/x86_64)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(ANDROID)
#include <android/log.h>
#define CMDLOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "CMDPOST", fmt, ##__VA_ARGS__)
#else
#define CMDLOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#endif

/* Ring buffer for sensor events */
#include "virtpass/vp_sensor_ringbuf.h"

/* Shared API header (queue functions, event ABI structs) */
#include "vp_cmdpost.h"

/* Host-side logging (rvvm_warn) + fixed-width int types (int64_t, PRId64) */
#include "utils.h"
#include "rvvm_types.h"

/* ============================================================
 * Custom syscall numbers (must match vp_ndk_stub)
 * ============================================================ */
#define SYS_ANDROID_BASE          0x10000
#define SYS_ANDROID_CALL          0x10022

/* Sub-commands passed in a0 for SYS_ANDROID_CALL */
#define SYS_ANDROID_SENSOR_INIT   (SYS_ANDROID_BASE + 1)
#define SYS_ANDROID_SENSOR_GET    (SYS_ANDROID_BASE + 2)
#define SYS_ANDROID_SENSOR_ENABLE (SYS_ANDROID_BASE + 3)
#define SYS_ANDROID_SENSOR_READ   (SYS_ANDROID_BASE + 4)
#define SYS_ANDROID_WINDOW_INIT   (SYS_ANDROID_BASE + 5)
#define SYS_ANDROID_INPUT_INIT    (SYS_ANDROID_BASE + 6)
#define SYS_ANDROID_LIFECYCLE     (SYS_ANDROID_BASE + 7)
#define SYS_ANDROID_CONFIG        (SYS_ANDROID_BASE + 8)
#define SYS_ANDROID_LOOPER_INIT   (SYS_ANDROID_BASE + 9)
#define SYS_ANDROID_ASSET_OPEN    (SYS_ANDROID_BASE + 10)

/* Window lock/unlock (Phase 1: Software Rendering) */
#define SYS_ANDROID_WINDOW_LOCK      (SYS_ANDROID_BASE + 11)
#define SYS_ANDROID_WINDOW_UNLOCK    (SYS_ANDROID_BASE + 12)
#define SYS_ANDROID_WINDOW_GET_SIZE  (SYS_ANDROID_BASE + 13)
#define SYS_ANDROID_WINDOW_SET_BUF   (SYS_ANDROID_BASE + 14)

/* GameActivity (Phase 2: Lifecycle + Input) */
#define SYS_ANDROID_GAME_CREATE      (SYS_ANDROID_BASE + 20)
#define SYS_ANDROID_GAME_DESTROY     (SYS_ANDROID_BASE + 21)
#define SYS_ANDROID_GAME_POLL_CMD    (SYS_ANDROID_BASE + 22)
#define SYS_ANDROID_GAME_SWAP_INPUT  (SYS_ANDROID_BASE + 23)
#define SYS_ANDROID_GAME_CLEAR_INPUT (SYS_ANDROID_BASE + 24)

/* Marshalled GL/EGL calls (Phase 3: hardware GL proxy) */
#define SYS_GL_CALL_BASE   0x10020
#define SYS_GL_CALL   (SYS_GL_CALL_BASE + 0)
#define SYS_EGL_CALL  (SYS_GL_CALL_BASE + 1)

/*
 * Phase 3: marshalled GL/EGL call struct - minimal controlled copy of the
 * guest ABI layout in vp_gles_stub.h (kept in sync by hand; see handover
 * 0.3-1). The guest allocates gl_call on its stack and passes its address
 * in a0. Pointers inside args[] are guest addresses; guest memory is
 * identity-mapped, so the host uses them directly. Floats travel
 * bit-packed through the int64_t slots.
 */
#define GL_CALL_MAX_ARGS 9
typedef struct {
    uint32_t fn_id;                  /* GL_FN_* / EGL_FN_*           */
    uint32_t nargs;                  /* number of valid args[] slots */
    int64_t  ret;                    /* host writes the return value */
    int64_t  args[GL_CALL_MAX_ARGS];
} gl_call;

/* ============================================================
 * Sensor types (must match vp_ndk_stub)
 * ============================================================ */
#define ASENSOR_TYPE_ACCELEROMETER       1
#define ASENSOR_TYPE_MAGNETIC_FIELD      2
#define ASENSOR_TYPE_GYROSCOPE           4
#define ASENSOR_TYPE_LIGHT               5
#define ASENSOR_TYPE_PRESSURE            6
#define ASENSOR_TYPE_PROXIMITY           8

/* ============================================================
 * Internal state
 * ============================================================ */

static bool g_initialized = false;
static bool g_sensor_initialized = false;
static bool g_window_initialized = false;
static bool g_input_initialized = false;
static bool g_looper_initialized = false;

/* Sensor state */
static bool g_sensors_enabled[8] = { false };
static sensor_ringbuf_t* g_sensor_ringbuf = NULL;

/* ============================================================
 * Callback function pointers (set by host via JNI)
 * ============================================================ */

typedef void (*sensor_init_callback)(void);
typedef void (*sensor_enable_callback)(int handle, bool enable);
typedef void (*sensor_data_callback)(sensor_event_t* event);

/* Window callbacks */
typedef int32_t (*window_lock_callback)(void* window, void* outBuffer, void* dirtyBounds);
typedef int32_t (*window_unlock_callback)(void* window, void* guestPixels);

/* GameActivity callbacks */
typedef void (*game_lifecycle_callback)(int32_t cmd);
typedef void (*game_input_callback)(void* motionEvent);

/* Window callbacks */
typedef void (*window_size_callback)(int64_t* width, int64_t* height);
typedef int32_t (*window_set_buf_callback)(int32_t width, int32_t height, int32_t format);

static sensor_init_callback g_sensor_init_cb = NULL;
static sensor_enable_callback g_sensor_enable_cb = NULL;
static sensor_data_callback g_sensor_data_cb = NULL;

static window_lock_callback g_window_lock_cb = NULL;
static window_unlock_callback g_window_unlock_cb = NULL;
static window_size_callback g_window_size_cb = NULL;
static window_set_buf_callback g_window_set_buf_cb = NULL;

static game_lifecycle_callback g_game_lifecycle_cb = NULL;
static game_input_callback g_game_input_cb = NULL;

/* Phase 3: GL/EGL dispatch callbacks (fn_id -> real host GL/EGL call) */
static egl_dispatch_callback g_egl_dispatch_cb = NULL;
static gl_dispatch_callback  g_gl_dispatch_cb  = NULL;

/* ============================================================
 * Host->Guest queues (lifecycle commands + input events)
 * ============================================================ */

static int32_t g_lifecycle_cmd_queue[CMDPOST_MAX_LIFECYCLE_CMDS];
static int32_t g_lifecycle_cmd_count = 0;
static int32_t g_lifecycle_cmd_read = 0;

static cmdpost_GameActivityMotionEvent g_motion_events[CMDPOST_MAX_MOTION_EVENTS];
static int32_t g_motion_event_count = 0;
static int32_t g_motion_event_read = 0;

void cmdpost_queue_lifecycle_cmd(int32_t cmd)
{
    if (g_lifecycle_cmd_count < CMDPOST_MAX_LIFECYCLE_CMDS) {
        g_lifecycle_cmd_queue[g_lifecycle_cmd_count++] = cmd;
    }
}

void cmdpost_clear_lifecycle_cmds(void)
{
    g_lifecycle_cmd_count = 0;
    g_lifecycle_cmd_read = 0;
}

void cmdpost_queue_motion_event(const cmdpost_GameActivityMotionEvent* ev)
{
    if (g_motion_event_count < CMDPOST_MAX_MOTION_EVENTS) {
        g_motion_events[g_motion_event_count++] = *ev;
    }
}

void cmdpost_clear_motion_events(void)
{
    g_motion_event_count = 0;
    g_motion_event_read = 0;
}

void cmdpost_clear_key_events(void)
{
    /* No key events queued in this build */
}

/* ============================================================
 * Public API for setting callbacks (called from JNI/Android)
 * ============================================================ */

void cmdpost_set_sensor_callbacks(sensor_init_callback init,
                                   sensor_enable_callback enable,
                                   sensor_data_callback data)
{
    g_sensor_init_cb = init;
    g_sensor_enable_cb = enable;
    g_sensor_data_cb = data;
}

void cmdpost_set_window_callbacks(window_lock_callback lock,
                                   window_unlock_callback unlock)
{
    g_window_lock_cb = lock;
    g_window_unlock_cb = unlock;
}

void cmdpost_set_window_size_callback(window_size_callback size_cb)
{
    g_window_size_cb = size_cb;
}

void cmdpost_set_window_set_buf_callback(window_set_buf_callback set_buf_cb)
{
    g_window_set_buf_cb = set_buf_cb;
}

void cmdpost_set_game_callbacks(game_lifecycle_callback lifecycle,
                                 game_input_callback input)
{
    g_game_lifecycle_cb = lifecycle;
    g_game_input_cb = input;
}

void cmdpost_set_gl_callbacks(egl_dispatch_callback egl, gl_dispatch_callback gl)
{
    g_egl_dispatch_cb = egl;
    g_gl_dispatch_cb  = gl;
}

/*
 * Initialize the sensor ring buffer.
 * Must be called before any sensor operations.
 */
void cmdpost_init_sensor_ringbuf(sensor_ringbuf_t* ringbuf)
{
    g_sensor_ringbuf = ringbuf;
    sensor_ringbuf_init(ringbuf);
}

/*
 * Push a sensor event from the host side.
 * This is called by the Android sensor listener.
 */
void cmdpost_push_sensor_event(const sensor_event_t* event)
{
    if (g_sensor_ringbuf) {
        sensor_ringbuf_push(g_sensor_ringbuf, event);
    }
}

/* ============================================================
 * Command dispatch (called from rvvm-user syscall handler)
 * ============================================================ */

/*
 * Handle Android NDK API proxy syscall (unified syscall with sub-command in a0).
 *
 * @param syscall_nr  Should be SYS_ANDROID_CALL; GL/EGL calls use their own numbers
 * @param a0          Android sub-command (when syscall_nr == SYS_ANDROID_CALL)
 *                    or gl_call* pointer (for SYS_GL_CALL / SYS_EGL_CALL)
 * @param a1-a5       Additional syscall arguments
 * @param guest_mem   Base address of guest memory (for pointer conversion)
 * @return            Syscall return value
 */
int64_t cmdpost_dispatch(int64_t syscall_nr, int64_t a0, int64_t a1, int64_t a2,
                       int64_t a3, int64_t a4, int64_t a5, void* guest_mem)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)guest_mem;

    switch (syscall_nr) {
        case SYS_ANDROID_CALL: {
            /* Sub-command passed in a0 */
            switch (a0) {
                case SYS_ANDROID_SENSOR_INIT: {
                    int mode = (int)a1;
                    if (mode == 0) {
                        /* Initialize sensor manager */
                        if (!g_sensor_initialized) {
                            if (g_sensor_init_cb) {
                                g_sensor_init_cb();
                            }
                            g_sensor_initialized = true;
                        }
                    } else if (mode == 1) {
                        /* Create event queue */
                        // TODO: Create actual sensor event queue
                    }
                    return 0;
                }

                case SYS_ANDROID_SENSOR_GET: {
                    /* Get sensor info */
                    // TODO: Return actual sensor info from host
                    return 0;
                }

                case SYS_ANDROID_SENSOR_ENABLE: {
                    int handle = (int)a1;
                    bool enable = (bool)a2;
                    if (handle >= 0 && handle < 8) {
                        g_sensors_enabled[handle] = enable;
                        if (g_sensor_enable_cb) {
                            g_sensor_enable_cb(handle, enable);
                        }
                    }
                    return 0;
                }

                case SYS_ANDROID_SENSOR_READ: {
                    /* Read sensor events from ring buffer */
                    // TODO: Copy events from ring buffer to guest memory
                    // For now, return number of events available
                    if (g_sensor_ringbuf) {
                        return sensor_ringbuf_count(g_sensor_ringbuf);
                    }
                    return 0;
                }

                case SYS_ANDROID_WINDOW_INIT: {
                    /* Initialize window */
                    if (!g_window_initialized) {
                        // TODO: Call ANativeWindow APIs
                        g_window_initialized = true;
                    }
                    return 0;
                }

                case SYS_ANDROID_INPUT_INIT: {
                    /* Initialize input */
                    if (!g_input_initialized) {
                        // TODO: Call AInputQueue APIs
                        g_input_initialized = true;
                    }
                    return 0;
                }

                case SYS_ANDROID_LIFECYCLE: {
                    /* Handle lifecycle event */
                    int event = (int)a1;
                    // TODO: Dispatch lifecycle event to guest
                    (void)event;
                    return 0;
                }

                case SYS_ANDROID_CONFIG: {
                    /* Get configuration */
                    // TODO: Return device configuration
                    return 0;
                }

                case SYS_ANDROID_LOOPER_INIT: {
                    /* Initialize looper */
                    if (!g_looper_initialized) {
                        // TODO: Call ALooper APIs
                        g_looper_initialized = true;
                    }
                    return 0;
                }

                case SYS_ANDROID_ASSET_OPEN: {
                    /* Open asset file */
                    // TODO: Open asset from APK
                    return -1; /* Not implemented */
                }

                case SYS_ANDROID_WINDOW_LOCK: {
                    /* Lock window for drawing */
                    if (g_window_lock_cb) {
                        return g_window_lock_cb((void*)a1, (void*)a2, (void*)a3);
                    }
                    return -1;
                }

                case SYS_ANDROID_WINDOW_UNLOCK: {
                    /* Unlock window and post buffer; a2 = guest pixel buffer */
                    if (g_window_unlock_cb) {
                        return g_window_unlock_cb((void*)a1, (void*)a2);
                    }
                    return -1;
                }

                case SYS_ANDROID_WINDOW_GET_SIZE: {
                    /* Get window size: pack (height << 32) | width into a0 */
                    int64_t w = 0, h = 0;
                    if (g_window_size_cb) {
                        g_window_size_cb(&w, &h);
                    }
                    CMDLOG("Host window get size: %" PRId64 "x%" PRId64, w, h);
                    return (int64_t)(((uint64_t)w & 0xFFFFFFFFu) |
                                     (((uint64_t)h & 0xFFFFFFFFu) << 32));
                }

                case SYS_ANDROID_WINDOW_SET_BUF: {
                    /* Guest requested buffer geometry change (width, height, format) */
                    if (g_window_set_buf_cb) {
                        int32_t result = g_window_set_buf_cb((int32_t)a1, (int32_t)a2, (int32_t)a3);
                        CMDLOG("Host window set buf: %dx%d fmt=%d -> %d",
                               (int32_t)a1, (int32_t)a2, (int32_t)a3, result);
                        return result;
                    }
                    return -1;
                }

                case SYS_ANDROID_GAME_CREATE: {
                    /* Create GameActivity */
                    printf("vp_cmdpost: GameActivity create (cmdpost)\n");
                    return 0;
                }

                case SYS_ANDROID_GAME_DESTROY: {
                    /* Destroy GameActivity */
                    printf("vp_cmdpost: GameActivity destroy (cmdpost)\n");
                    cmdpost_clear_lifecycle_cmds();
                    cmdpost_clear_motion_events();
                    return 0;
                }

                case SYS_ANDROID_GAME_POLL_CMD: {
                    /* Poll for lifecycle command */
                    if (g_lifecycle_cmd_read < g_lifecycle_cmd_count) {
                        int32_t cmd = g_lifecycle_cmd_queue[g_lifecycle_cmd_read++];
                        CMDLOG("Guest polled lifecycle cmd: %d", cmd);
                        return cmd;
                    }
                    return -1;  /* No command available */
                }

                case SYS_ANDROID_GAME_SWAP_INPUT: {
                    /* Swap input buffers: a1 = guest pointer to GameActivityInputBuffer */
                    if (!a1) {
                        return 0;
                    }
                    cmdpost_GameActivityInputBuffer* guest_buf = (cmdpost_GameActivityInputBuffer*)(size_t)a1;
                    int32_t out_count = g_motion_event_count;
                    if (out_count > 0) {
                        /* Copy motion events into guest-provided array */
                        cmdpost_GameActivityMotionEvent* dst =
                            (cmdpost_GameActivityMotionEvent*)(size_t)guest_buf->motionEvents;
                        int32_t capacity = guest_buf->motionEventsCapacity > 0
                                         ? guest_buf->motionEventsCapacity : CMDPOST_MAX_MOTION_EVENTS;
                        int32_t copy_count = out_count < capacity ? out_count : capacity;
                        if (dst) {
                            memcpy(dst, g_motion_events, sizeof(cmdpost_GameActivityMotionEvent) * copy_count);
                            guest_buf->motionEventsCount = copy_count;
                            CMDLOG("Guest swapped input: %d motion events (capacity %d)", copy_count, capacity);
                        }
                    }
                    cmdpost_clear_motion_events();
                    return out_count;
                }

                case SYS_ANDROID_GAME_CLEAR_INPUT: {
                    /* Clear input events: a1 = 0 for motion, 1 for key */
                    if (a1 == 0) {
                        cmdpost_clear_motion_events();
                    } else {
                        cmdpost_clear_key_events();
                    }
                    return 0;
                }

                default:
                    rvvm_warn("cmdpost: Unknown Android sub-command %" PRId64, a0);
                    return -38;
            }
            break;
        }

        case SYS_EGL_CALL: {   /* a0 = guest gl_call* */
            gl_call* c = (gl_call*)(size_t)a0;
            if (!c) return -1;
            if (g_egl_dispatch_cb) g_egl_dispatch_cb((uint32_t)c->fn_id, c->args, &c->ret);
            else c->ret = 0;
            return 0;
        }
        case SYS_GL_CALL: {
            gl_call* c = (gl_call*)(size_t)a0;
            if (!c) return -1;
            if (g_gl_dispatch_cb) g_gl_dispatch_cb((uint32_t)c->fn_id, c->args, &c->ret);
            else c->ret = 0;
            return 0;
        }

        default:
            fprintf(stderr, "cmdpost: Unknown syscall %" PRId64 "\n", syscall_nr);
            return -38; /* -ENOSYS */
    }
}

/* ============================================================
 * Initialization
 * ============================================================ */

void cmdpost_init(void)
{
    if (!g_initialized) {
        printf("vp_cmdpost: Initializing Android NDK API proxy\n");
        g_initialized = true;
    }
}

void cmdpost_cleanup(void)
{
    if (g_initialized) {
        printf("vp_cmdpost: Cleaning up\n");
        g_initialized = false;
        g_sensor_initialized = false;
        g_window_initialized = false;
        g_input_initialized = false;
        g_looper_initialized = false;
        g_sensor_ringbuf = NULL;
        g_window_lock_cb = NULL;
        g_window_unlock_cb = NULL;
        g_game_lifecycle_cb = NULL;
        g_game_input_cb = NULL;
        g_egl_dispatch_cb = NULL;
        g_gl_dispatch_cb = NULL;
    }
}
