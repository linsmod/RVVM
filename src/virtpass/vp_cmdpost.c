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
/* write(): used to publish vsync frame times into the guest's Looper pipe */
#include <unistd.h>

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

/* Choreographer (Phase 4: display vsync source) */
#define SYS_ANDROID_CHOREOGRAPHER_INIT (SYS_ANDROID_BASE + 25)
#define SYS_ANDROID_CHOREOGRAPHER_WAIT (SYS_ANDROID_BASE + 26)
/* fd wakeup (方案 B): the guest hands us the write end of the pipe its Looper
 * polls, and asks for exactly one vsync at a time. */
#define SYS_ANDROID_CHOREOGRAPHER_SET_FD (SYS_ANDROID_BASE + 27)
#define SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC (SYS_ANDROID_BASE + 28)

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

/* Configuration callback: host resolves one AConfiguration field. */
typedef int32_t (*config_get_callback)(int32_t field, int32_t* outValue);

static sensor_init_callback g_sensor_init_cb = NULL;
static sensor_enable_callback g_sensor_enable_cb = NULL;
static sensor_data_callback g_sensor_data_cb = NULL;

static window_lock_callback g_window_lock_cb = NULL;
static window_unlock_callback g_window_unlock_cb = NULL;
static window_size_callback g_window_size_cb = NULL;
static window_set_buf_callback g_window_set_buf_cb = NULL;
static config_get_callback g_config_get_cb = NULL;

static game_lifecycle_callback g_game_lifecycle_cb = NULL;
static game_input_callback g_game_input_cb = NULL;

/* Phase 3: GL/EGL dispatch callbacks (fn_id -> real host GL/EGL call) */
static egl_dispatch_callback g_egl_dispatch_cb = NULL;
static gl_dispatch_callback  g_gl_dispatch_cb  = NULL;

/* Phase 4: AChoreographer vsync source (blocks until the next display frame) */
static choreographer_wait_callback g_choreographer_wait_cb = NULL;

/* Phase 4 (fd wakeup): guest-owned pipe that its Looper polls. The fd is a
 * real host fd (guest syscalls are passed through), so the vsync clock can
 * write the frame time straight into it. g_vsync_armed means "the guest is
 * waiting for exactly one vsync" (requestNextVsync semantics). */
static int  g_choreographer_fd = -1;
static volatile bool g_vsync_armed = false;
static volatile bool g_vsync_source_lost = false;

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

void cmdpost_set_config_callback(config_get_callback get_cb)
{
    g_config_get_cb = get_cb;
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

void cmdpost_set_choreographer_callback(choreographer_wait_callback wait_cb)
{
    g_choreographer_wait_cb = wait_cb;
    /* A (re)registered clock is alive again: Android recreates the activity by
     * calling nativeInit once more, which lands here. */
    g_vsync_source_lost = false;
}

bool vp_cmdpost_vsync_tick(int64_t frame_time_ns)
{
    if (!g_vsync_armed) {
        return false;
    }
    g_vsync_armed = false;

    int fd = g_choreographer_fd;
    if (fd < 0) {
        return false;
    }

    int64_t payload = frame_time_ns;
    if (write(fd, &payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        /* The guest stopped draining (gone or falling back): drop the fd so we
         * do not keep writing into a dead pipe. */
        g_choreographer_fd = -1;
        return false;
    }
    return true;
}

void vp_cmdpost_vsync_source_lost(void)
{
    g_vsync_source_lost = true;
    g_vsync_armed = false;

    /* Wake a guest that is blocked in poll(): a negative frame time tells the
     * stub the clock is gone, so it degrades instead of hanging. */
    int fd = g_choreographer_fd;
    g_choreographer_fd = -1;
    if (fd >= 0) {
        int64_t payload = -1;
        write(fd, &payload, sizeof(payload));
    }
}

/* ============================================================
 * Phase 5: AAudio proxy
 *
 * A stream is a slot in g_audio_streams; the slot index is what the guest
 * carries around as its transport handle. The host backend owns everything
 * else (device, threads, format conversion) behind vp_audio_ops_t.
 * ============================================================ */
#define CMDPOST_MAX_AUDIO_STREAMS 8

typedef struct {
    bool                  used;
    void*                 user;      /* host backend handle                   */
    const vp_aaudio_config_t* cfg;   /* guest memory; valid while open        */
    int32_t               direction; /* VP_AUDIO_DIR_*                        */
    int32_t               frame_bytes; /* negotiated frame size               */
    int32_t               state;     /* VP_AUDIO_STATE_*                      */
} cmdpost_audio_stream_t;

static cmdpost_audio_stream_t g_audio_streams[CMDPOST_MAX_AUDIO_STREAMS];
static const vp_audio_ops_t*  g_audio_ops = NULL;

void cmdpost_set_audio_callbacks(const vp_audio_ops_t* ops)
{
    g_audio_ops = ops;
}

static int32_t cmdpost_audio_alloc_slot(void)
{
    for (int32_t i = 0; i < CMDPOST_MAX_AUDIO_STREAMS; i++) {
        if (!g_audio_streams[i].used) {
            memset(&g_audio_streams[i], 0, sizeof(g_audio_streams[i]));
            g_audio_streams[i].used = true;
            return i;
        }
    }
    return -1;
}

static void cmdpost_audio_free_slot(int32_t slot)
{
    if (slot >= 0 && slot < CMDPOST_MAX_AUDIO_STREAMS) {
        memset(&g_audio_streams[slot], 0, sizeof(g_audio_streams[slot]));
    }
}

static cmdpost_audio_stream_t* cmdpost_audio_lookup(int64_t handle)
{
    if (handle < 0 || handle >= CMDPOST_MAX_AUDIO_STREAMS) {
        return NULL;
    }
    return g_audio_streams[handle].used ? &g_audio_streams[handle] : NULL;
}

static void cmdpost_audio_refresh_state(cmdpost_audio_stream_t* stream)
{
    if (!g_audio_ops || !g_audio_ops->get_info) {
        return;
    }
    vp_aaudio_info_t info;
    memset(&info, 0, sizeof(info));
    if (g_audio_ops->get_info(stream->user, &info) == VP_AUDIO_OK) {
        stream->state = info.state;
    }
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
                    /* Get one device configuration field (a1 = VP_ACONFIG_QUERY_*) */
                    int32_t field = (int32_t)a1;
                    int32_t value = 0;
                    if (g_config_get_cb && g_config_get_cb(field, &value) == 0) {
                        return (int64_t)value;
                    }
                    return -1; /* host provided no configuration */
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
                    /* Lock window for drawing. A non-zero return here means the
                     * host surface itself is not ready; when it returns 0 the
                     * guest still has to allocate its own pixbuf, so a later
                     * failure is NOT a lock failure (see vp_ndk_stub.c). */
                    if (!g_window_lock_cb) {
                        CMDLOG("WINDOW_LOCK: no host callback registered");
                        return -1;
                    }
                    int32_t lock_rc = g_window_lock_cb((void*)a1, (void*)a2, (void*)a3);
                    if (lock_rc != 0) {
                        CMDLOG("WINDOW_LOCK: host refused lock -> %d", lock_rc);
                    }
                    return lock_rc;
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
                            int32_t max_pointers = 0;
                            for (int32_t i = 0; i < copy_count; i++) {
                                if (g_motion_events[i].pointerCount > max_pointers) {
                                    max_pointers = g_motion_events[i].pointerCount;
                                }
                            }
                            CMDLOG("Guest swapped input: %d motion events (capacity %d, max pointers %d)",
                                   copy_count, capacity, max_pointers);
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

                case SYS_ANDROID_CHOREOGRAPHER_INIT: {
                    /* The host owns the vsync source; nothing to arm here.
                     * Report what the guest can expect: whether a vsync clock
                     * exists at all, and whether we can wake its Looper fd
                     * directly instead of it calling WAIT every frame. */
                    if (!g_choreographer_wait_cb || g_vsync_source_lost) {
                        return 0;   /* no source: guest uses its own 60Hz clock */
                    }
                    return VP_VSYNC_CAP_SOURCE | VP_VSYNC_CAP_FD_WAKEUP;
                }

                case SYS_ANDROID_CHOREOGRAPHER_WAIT: {
                    if (!g_choreographer_wait_cb) {
                        return -1;   /* guest falls back to its own clock */
                    }
                    return g_choreographer_wait_cb();
                }

                case SYS_ANDROID_CHOREOGRAPHER_SET_FD: {
                    /* Guest hands us the write end of the vsync pipe its Looper
                     * polls; a1 < 0 unregisters. (a0 is the sub-command.) */
                    if (!g_choreographer_wait_cb || g_vsync_source_lost) {
                        return -1;
                    }
                    g_choreographer_fd = (int32_t)a1;
                    g_vsync_armed = false;
                    return 0;
                }

                case SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC: {
                    /* One outstanding request per frame, answered by the vsync
                     * clock through vp_cmdpost_vsync_tick(). */
                    if (!g_choreographer_wait_cb || g_vsync_source_lost ||
                        g_choreographer_fd < 0) {
                        return -1;   /* guest degrades to the blocking WAIT path */
                    }
                    g_vsync_armed = true;
                    return 0;
                }

                /* ---------- Phase 5: AAudio ---------- */

                case SYS_ANDROID_AAUDIO_OPEN: {
                    /* a1 = vp_aaudio_config_t* (guest memory). */
                    if (!g_audio_ops || !g_audio_ops->open) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    const vp_aaudio_config_t* cfg = (const vp_aaudio_config_t*)(size_t)a1;
                    if (!cfg) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t slot = cmdpost_audio_alloc_slot();
                    if (slot < 0) {
                        return VP_AUDIO_ERROR_NO_MEMORY;
                    }
                    void* user = NULL;
                    int32_t rc = g_audio_ops->open(cfg, &user);
                    if (rc != VP_AUDIO_OK) {
                        cmdpost_audio_free_slot(slot);
                        return rc;
                    }
                    cmdpost_audio_stream_t* stream = &g_audio_streams[slot];
                    stream->user = user;
                    stream->cfg = cfg;
                    stream->direction = cfg->direction;
                    stream->frame_bytes = (int32_t)vp_audio_frame_bytes(cfg->format, cfg->channel_count);
                    stream->state = VP_AUDIO_STATE_OPEN;
                    cmdpost_audio_refresh_state(stream);
                    return slot;
                }

                case SYS_ANDROID_AAUDIO_CLOSE: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t rc = g_audio_ops && g_audio_ops->close
                               ? g_audio_ops->close(stream->user) : VP_AUDIO_OK;
                    cmdpost_audio_free_slot((int32_t)a1);
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_START:
                case SYS_ANDROID_AAUDIO_PAUSE:
                case SYS_ANDROID_AAUDIO_STOP: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t rc;
                    if (a0 == SYS_ANDROID_AAUDIO_START) {
                        rc = g_audio_ops && g_audio_ops->start
                           ? g_audio_ops->start(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    } else if (a0 == SYS_ANDROID_AAUDIO_PAUSE) {
                        rc = g_audio_ops && g_audio_ops->pause
                           ? g_audio_ops->pause(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    } else {
                        rc = g_audio_ops && g_audio_ops->stop
                           ? g_audio_ops->stop(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    if (rc == VP_AUDIO_OK) {
                        cmdpost_audio_refresh_state(stream);
                    }
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_FLUSH: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return g_audio_ops && g_audio_ops->flush
                         ? g_audio_ops->flush(stream->user) : VP_AUDIO_ERROR_UNSUPPORTED;
                }

                case SYS_ANDROID_AAUDIO_WRITE: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream || !g_audio_ops || !g_audio_ops->write) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    const void* buf = (const void*)(size_t)a2;
                    int32_t frames = (int32_t)a3;
                    if (!buf || frames <= 0) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return g_audio_ops->write(stream->user, buf, frames, stream->frame_bytes);
                }

                case SYS_ANDROID_AAUDIO_READ: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream || !g_audio_ops || !g_audio_ops->read) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    void* buf = (void*)(size_t)a2;
                    int32_t frames = (int32_t)a3;
                    if (!buf || frames <= 0) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return g_audio_ops->read(stream->user, buf, frames, stream->frame_bytes);
                }

                case SYS_ANDROID_AAUDIO_INFO: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    vp_aaudio_info_t* out = (vp_aaudio_info_t*)(size_t)a2;
                    if (!stream || !out) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    if (!g_audio_ops || !g_audio_ops->get_info) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    int32_t rc = g_audio_ops->get_info(stream->user, out);
                    if (rc == VP_AUDIO_OK) {
                        stream->state = out->state;
                    }
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_TS: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    vp_aaudio_timestamp_t* out = (vp_aaudio_timestamp_t*)(size_t)a2;
                    if (!stream || !out) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return g_audio_ops && g_audio_ops->get_timestamp
                         ? g_audio_ops->get_timestamp(stream->user, out)
                         : VP_AUDIO_ERROR_UNSUPPORTED;
                }

                case SYS_ANDROID_AAUDIO_BUFSZ: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    if (!g_audio_ops || !g_audio_ops->set_buffer_size) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    int32_t applied = 0;
                    int32_t rc = g_audio_ops->set_buffer_size(stream->user, (int32_t)a2, &applied);
                    return rc == VP_AUDIO_OK ? (int64_t)applied : (int64_t)rc;
                }

                case SYS_ANDROID_AAUDIO_QUERY: {
                    /* Capability probe: 0 means this host has no audio backend,
                     * so the guest stubs fail fast instead of hanging. */
                    if (!g_audio_ops || !g_audio_ops->query) {
                        return 0;
                    }
                    return (int64_t)g_audio_ops->query();
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
        g_choreographer_wait_cb = NULL;
        g_choreographer_fd = -1;
        g_vsync_armed = false;
        g_vsync_source_lost = false;

        /* Tear down every live AAudio stream before dropping the backend. */
        if (g_audio_ops && g_audio_ops->close) {
            for (int32_t i = 0; i < CMDPOST_MAX_AUDIO_STREAMS; i++) {
                if (g_audio_streams[i].used) {
                    g_audio_ops->close(g_audio_streams[i].user);
                }
            }
        }
        memset(g_audio_streams, 0, sizeof(g_audio_streams));
        g_audio_ops = NULL;
    }
}
