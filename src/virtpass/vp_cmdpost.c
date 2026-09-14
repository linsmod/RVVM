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

/* Shared API header (queue functions, event ABI structs) */
#include "vp_cmdpost.h"

/* Host-side logging (rvvm_warn) + fixed-width int types (int64_t, PRId64) */
#include "utils.h"
#include "rvvm_types.h"
#include "core/rvvm_user.h"
#include "virtpass/vp_gl.h" /* gl_call + fn_id macros (generated) */

/* ============================================================
 * Custom syscall numbers (must match the guest stub)
 *
 * Both sides now compile the same header, virtpass/vp_syscall.h, so there is
 * nothing left to keep in sync by hand. SYS_GL_CALL / SYS_EGL_CALL are the
 * marshalled GL/EGL entries (Phase 3 hardware GL proxy).
 *
 * gl_call itself comes from the generated virtpass/vp_gl.h. This file used to
 * carry a hand-copied struct mirroring only its first four fields - adding
 * gl_call.retbuf on the guest side then desynced the layouts and every
 * dispatch read the guest stack at the wrong offsets. Never duplicate it.
 * ============================================================ */
#include "virtpass/vp_syscall.h"

/* ============================================================
 * Callback function pointers (set by host via JNI)
 * ============================================================ */

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

/* ============================================================
 * Phase 5: AAudio proxy
 *
 * A stream is a slot in the instance's audio_streams table; the slot index is
 * what the guest carries around as its transport handle. The host backend owns
 * everything else (device, threads, format conversion) behind vp_audio_ops_t.
 *
 * The types sit here, above struct vp_cmdpost, because the instance embeds
 * them.
 * ============================================================ */
#define CMDPOST_MAX_AUDIO_STREAMS 8

typedef struct {
    bool                  used;
    void*                 user;      /* host backend handle                   */
    int32_t               direction; /* VP_AUDIO_DIR_*                        */
    int32_t               frame_bytes; /* negotiated frame size               */
    int32_t               state;     /* VP_AUDIO_STATE_*                      */
    /* The stream must outlive the guest's config object, which may be freed or
     * reused right after OPEN - and it lives in guest memory we cannot reach
     * from host state anyway. Keep a host-side copy. */
    vp_aaudio_config_t    cfg_copy;
} cmdpost_audio_stream_t;

/* ============================================================
 * Per-instance state
 *
 * Everything in here used to be a file-scope global: one set for the whole
 * process, which is why a second guest in the same process would have shared
 * (and cleared) the first one's callbacks, queues and streams. Each host now
 * owns one instance - it creates it with cmdpost_create(), registers its
 * callbacks into it, hands it to the core with rvvm_user_set_host_ctx() so a
 * guest's ecall path can find the instance its syscalls belong to, and frees it
 * with cmdpost_destroy() at teardown.
 *
 * The struct is private: only vp_cmdpost.h's opaque vp_cmdpost_t is visible to
 * callers.
 * ============================================================ */
struct vp_cmdpost {
    bool initialized;
    bool window_initialized;
    bool input_initialized;
    bool looper_initialized;

    /* ---- Callbacks (the host's, registered per run) ---- */
    window_lock_callback     window_lock_cb;
    window_unlock_callback   window_unlock_cb;
    window_size_callback     window_size_cb;
    window_set_buf_callback  window_set_buf_cb;
    config_get_callback      config_get_cb;

    game_lifecycle_callback  game_lifecycle_cb;
    game_input_callback      game_input_cb;

    /* Phase 3: GL/EGL dispatch callbacks (fn_id -> real host GL/EGL call) */
    egl_dispatch_callback    egl_dispatch_cb;
    gl_dispatch_callback     gl_dispatch_cb;

    /* Phase 4: AChoreographer vsync source (blocks until the next display
     * frame) */
    choreographer_wait_callback choreographer_wait_cb;

    /* Phase 4 (fd wakeup): guest-owned pipe that its Looper polls. The fd is a
     * real host fd (guest syscalls are passed through), so the vsync clock can
     * write the frame time straight into it. vsync_armed means "the guest is
     * waiting for exactly one vsync" (requestNextVsync semantics). */
    int           choreographer_fd;
    volatile bool vsync_armed;
    volatile bool vsync_source_lost;

    /* ---- Host->Guest queues (lifecycle commands + input events) ---- */
    int32_t lifecycle_cmd_queue[CMDPOST_MAX_LIFECYCLE_CMDS];
    int32_t lifecycle_cmd_count;
    int32_t lifecycle_cmd_read;

    cmdpost_GameActivityMotionEvent motion_events[CMDPOST_MAX_MOTION_EVENTS];
    int32_t motion_event_count;
    int32_t motion_event_read;

    /* ---- Audio streams ---- */
    const vp_audio_ops_t*  audio_ops;
    cmdpost_audio_stream_t audio_streams[CMDPOST_MAX_AUDIO_STREAMS];

    /* ---- Sensors ----
     * The sensor subsystem's per-guest state (its descriptor table and its
     * queues) is over there; its device-level half (the backend, the aggregate
     * it is driven with, the fan-out) is process-wide. Owned here so a host
     * cannot end up with one of the two and not the other. */
    vp_sensor_t*           sensor;
};

/*
 * The instance used by runs that have no host to bind one: rvvm_user_main.c
 * boots a guest straight through rvvm_user_linux_ex() with nothing registered,
 * and its syscalls still arrive at cmdpost_dispatch(). This is what the old
 * file-scope globals were for exactly that case - one per process, because that
 * runner is one guest per process.
 *
 * It is created lazily by cmdpost_init(), which the core always calls before a
 * guest's first syscall, so no two guest threads race to create it.
 */
static vp_cmdpost_t* g_default_instance = NULL;

static vp_cmdpost_t* cmdpost_instance_or_default(vp_cmdpost_t* inst)
{
    return inst ? inst : g_default_instance;
}

/* Allocate a per-instance state block. The host owns it for the lifetime of its
 * bridge and frees it with cmdpost_destroy(). */
vp_cmdpost_t* cmdpost_create(void)
{
    vp_cmdpost_t* inst = calloc(1, sizeof(*inst));

    if (!inst) {
        return NULL;
    }
    /* choreographer_fd uses -1 as its "no pipe registered" sentinel, and 0 is a
     * valid fd, so it cannot be left to the calloc zeroing. Every other field is
     * fine as zero (callbacks unregistered, queues empty). */
    inst->choreographer_fd = -1;

    /* This guest's sensor state. NULL when the sensor subsystem is out of
     * instance slots: the guest then sees no sensors, exactly like a host with
     * no backend. */
    inst->sensor = vp_sensor_create();
    return inst;
}

/* Tear the bridge down and free the instance. Runs cmdpost_cleanup() first, so
 * a host that forgot to call it still gets its AAudio streams closed. */
void cmdpost_destroy(vp_cmdpost_t* inst)
{
    if (!inst) {
        return;
    }
    cmdpost_cleanup(inst);

    /* The sensor state goes last: cleanup() already ended this instance's run,
     * which is what drops its queues, so nothing can dispatch into it any more
     * (and the guest that owned them is gone). */
    vp_sensor_destroy(inst->sensor);
    inst->sensor = NULL;

    free(inst);
}

void cmdpost_queue_lifecycle_cmd(vp_cmdpost_t* inst, int32_t cmd)
{
    if (!inst) {
        return;
    }
    if (inst->lifecycle_cmd_count < CMDPOST_MAX_LIFECYCLE_CMDS) {
        inst->lifecycle_cmd_queue[inst->lifecycle_cmd_count++] = cmd;
    }
}

void cmdpost_clear_lifecycle_cmds(vp_cmdpost_t* inst)
{
    if (!inst) {
        return;
    }
    inst->lifecycle_cmd_count = 0;
    inst->lifecycle_cmd_read = 0;
}

void cmdpost_queue_motion_event(vp_cmdpost_t* inst, const cmdpost_GameActivityMotionEvent* ev)
{
    if (!inst || !ev) {
        return;
    }
    if (inst->motion_event_count < CMDPOST_MAX_MOTION_EVENTS) {
        inst->motion_events[inst->motion_event_count++] = *ev;
    }
}

void cmdpost_clear_motion_events(vp_cmdpost_t* inst)
{
    if (!inst) {
        return;
    }
    inst->motion_event_count = 0;
    inst->motion_event_read = 0;
}

void cmdpost_clear_key_events(vp_cmdpost_t* inst)
{
    /* No key events queued in this build */
    (void)inst;
}

/* ============================================================
 * Public API for setting callbacks (called from JNI/Android)
 * ============================================================ */

void cmdpost_set_window_callbacks(vp_cmdpost_t* inst,
                                   window_lock_callback lock,
                                   window_unlock_callback unlock)
{
    if (!inst) {
        return;
    }
    inst->window_lock_cb = lock;
    inst->window_unlock_cb = unlock;
}

void cmdpost_set_window_size_callback(vp_cmdpost_t* inst, window_size_callback size_cb)
{
    if (!inst) {
        return;
    }
    inst->window_size_cb = size_cb;
}

void cmdpost_set_window_set_buf_callback(vp_cmdpost_t* inst, window_set_buf_callback set_buf_cb)
{
    if (!inst) {
        return;
    }
    inst->window_set_buf_cb = set_buf_cb;
}

void cmdpost_set_config_callback(vp_cmdpost_t* inst, config_get_callback get_cb)
{
    if (!inst) {
        return;
    }
    inst->config_get_cb = get_cb;
}

void cmdpost_set_game_callbacks(vp_cmdpost_t* inst,
                                 game_lifecycle_callback lifecycle,
                                 game_input_callback input)
{
    if (!inst) {
        return;
    }
    inst->game_lifecycle_cb = lifecycle;
    inst->game_input_cb = input;
}

void cmdpost_set_gl_callbacks(vp_cmdpost_t* inst,
                              egl_dispatch_callback egl, gl_dispatch_callback gl)
{
    if (!inst) {
        return;
    }
    inst->egl_dispatch_cb = egl;
    inst->gl_dispatch_cb  = gl;
}

void cmdpost_set_choreographer_callback(vp_cmdpost_t* inst, choreographer_wait_callback wait_cb)
{
    if (!inst) {
        return;
    }
    inst->choreographer_wait_cb = wait_cb;
    /* A (re)registered clock is alive again: Android recreates the activity by
     * calling nativeInit once more, which lands here. */
    inst->vsync_source_lost = false;
}

bool vp_cmdpost_vsync_tick(vp_cmdpost_t* inst, int64_t frame_time_ns)
{
    if (!inst || !inst->vsync_armed) {
        return false;
    }
    inst->vsync_armed = false;

    int fd = inst->choreographer_fd;
    if (fd < 0) {
        return false;
    }

    int64_t payload = frame_time_ns;
    if (write(fd, &payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        /* The guest stopped draining (gone or falling back): drop the fd so we
         * do not keep writing into a dead pipe. */
        inst->choreographer_fd = -1;
        return false;
    }
    return true;
}

void vp_cmdpost_vsync_source_lost(vp_cmdpost_t* inst)
{
    if (!inst) {
        return;
    }
    inst->vsync_source_lost = true;
    inst->vsync_armed = false;

    /* Wake a guest that is blocked in poll(): a negative frame time tells the
     * stub the clock is gone, so it degrades instead of hanging. */
    int fd = inst->choreographer_fd;
    inst->choreographer_fd = -1;
    if (fd >= 0) {
        int64_t payload = -1;
        write(fd, &payload, sizeof(payload));
    }
}

void cmdpost_set_audio_callbacks(vp_cmdpost_t* inst, const vp_audio_ops_t* ops)
{
    if (!inst) {
        return;
    }
    inst->audio_ops = ops;
}

static int32_t cmdpost_audio_alloc_slot(vp_cmdpost_t* inst)
{
    for (int32_t i = 0; i < CMDPOST_MAX_AUDIO_STREAMS; i++) {
        if (!inst->audio_streams[i].used) {
            memset(&inst->audio_streams[i], 0, sizeof(inst->audio_streams[i]));
            inst->audio_streams[i].used = true;
            return i;
        }
    }
    return -1;
}

static void cmdpost_audio_free_slot(vp_cmdpost_t* inst, int32_t slot)
{
    if (slot >= 0 && slot < CMDPOST_MAX_AUDIO_STREAMS) {
        memset(&inst->audio_streams[slot], 0, sizeof(inst->audio_streams[slot]));
    }
}

static cmdpost_audio_stream_t* cmdpost_audio_lookup(vp_cmdpost_t* inst, int64_t handle)
{
    if (handle < 0 || handle >= CMDPOST_MAX_AUDIO_STREAMS) {
        return NULL;
    }
    return inst->audio_streams[handle].used ? &inst->audio_streams[handle] : NULL;
}

static void cmdpost_audio_refresh_state(vp_cmdpost_t* inst, cmdpost_audio_stream_t* stream)
{
    if (!inst->audio_ops || !inst->audio_ops->get_info) {
        return;
    }
    vp_aaudio_info_t info;
    memset(&info, 0, sizeof(info));
    if (inst->audio_ops->get_info(stream->user, &info) == VP_AUDIO_OK) {
        stream->state = info.state;
    }
}

/* ============================================================
 * Command dispatch (called from rvvm-user syscall handler)
 * ============================================================ */

/*
 * Handle Android NDK API proxy syscall (unified syscall with sub-command in a0).
 *
 * @param inst        Instance this guest's syscalls belong to, as bound with
 *                    rvvm_user_set_host_ctx(); NULL means no host is bound and
 *                    the process-wide default instance is used instead
 * @param syscall_nr  Should be SYS_ANDROID_CALL; GL/EGL calls use their own numbers
 * @param a0          Android sub-command (when syscall_nr == SYS_ANDROID_CALL)
 *                    or gl_call* pointer (for SYS_GL_CALL / SYS_EGL_CALL)
 * @param a1-a5       Additional syscall arguments
 * @param guest_mem   Base address of guest memory (for pointer conversion)
 * @return            Syscall return value
 */
int64_t cmdpost_dispatch(vp_cmdpost_t* inst, int64_t syscall_nr, int64_t a0, int64_t a1, int64_t a2,
                       int64_t a3, int64_t a4, int64_t a5, void* guest_mem)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)guest_mem;

    inst = cmdpost_instance_or_default(inst);

    switch (syscall_nr) {
        case SYS_ANDROID_CALL: {
            /* Sub-command passed in a0 */
            switch (a0) {
                /* ---------- Phase 6: sensors ----------
                 * The whole sensor proxy lives in vp_sensor.c; here we only
                 * forward the sub-command, so the dispatcher stays a table. */
                case VP_SENSOR_MANAGER_INIT:
                case VP_SENSOR_LIST:
                case VP_SENSOR_DEFAULT:
                case VP_SENSOR_QUEUE_CREATE:
                case VP_SENSOR_QUEUE_DESTROY:
                case VP_SENSOR_QUEUE_ENABLE:
                case VP_SENSOR_QUEUE_DISABLE:
                case VP_SENSOR_QUEUE_SET_RATE:
                case VP_SENSOR_QUEUE_HAS:
                case VP_SENSOR_QUEUE_READ:
                    return vp_sensor_dispatch(inst->sensor, a0, a1, a2, a3, a4);

                case SYS_ANDROID_WINDOW_INIT: {
                    /* Initialize window */
                    if (!inst->window_initialized) {
                        // TODO: Call ANativeWindow APIs
                        inst->window_initialized = true;
                    }
                    return 0;
                }

                case SYS_ANDROID_INPUT_INIT: {
                    /* Initialize input */
                    if (!inst->input_initialized) {
                        // TODO: Call AInputQueue APIs
                        inst->input_initialized = true;
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
                    if (inst->config_get_cb && inst->config_get_cb(field, &value) == 0) {
                        return (int64_t)value;
                    }
                    return -1; /* host provided no configuration */
                }

                case SYS_ANDROID_LOOPER_INIT: {
                    /* Initialize looper */
                    if (!inst->looper_initialized) {
                        // TODO: Call ALooper APIs
                        inst->looper_initialized = true;
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
                    if (!inst->window_lock_cb) {
                        CMDLOG("WINDOW_LOCK: no host callback registered");
                        return -1;
                    }
                    /* window (a1) is a guest static, outBuffer (a2) and
                     * dirtyBounds (a3) are guest buffers; a3 may be NULL */
                    int32_t lock_rc = inst->window_lock_cb(rvvm_user_guest_ptr((uint64_t)a1),
                                                       rvvm_user_guest_ptr((uint64_t)a2),
                                                       rvvm_user_guest_ptr((uint64_t)a3));
                    if (lock_rc != 0) {
                        CMDLOG("WINDOW_LOCK: host refused lock -> %d", lock_rc);
                    }
                    return lock_rc;
                }

                case SYS_ANDROID_WINDOW_UNLOCK: {
                    /* Unlock window and post buffer; a2 = guest pixel buffer */
                    if (inst->window_unlock_cb) {
                        return inst->window_unlock_cb(rvvm_user_guest_ptr((uint64_t)a1),
                                                  rvvm_user_guest_ptr((uint64_t)a2));
                    }
                    return -1;
                }

                case SYS_ANDROID_WINDOW_GET_SIZE: {
                    /* Get window size: pack (height << 32) | width into a0 */
                    int64_t w = 0, h = 0;
                    if (inst->window_size_cb) {
                        inst->window_size_cb(&w, &h);
                    }
                    CMDLOG("Host window get size: %" PRId64 "x%" PRId64, w, h);
                    return (int64_t)(((uint64_t)w & 0xFFFFFFFFu) |
                                     (((uint64_t)h & 0xFFFFFFFFu) << 32));
                }

                case SYS_ANDROID_WINDOW_SET_BUF: {
                    /* Guest requested buffer geometry change (width, height, format) */
                    if (inst->window_set_buf_cb) {
                        int32_t result = inst->window_set_buf_cb((int32_t)a1, (int32_t)a2, (int32_t)a3);
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
                    cmdpost_clear_lifecycle_cmds(inst);
                    cmdpost_clear_motion_events(inst);
                    return 0;
                }

                case SYS_ANDROID_GAME_POLL_CMD: {
                    /* Poll for lifecycle command */
                    if (inst->lifecycle_cmd_read < inst->lifecycle_cmd_count) {
                        int32_t cmd = inst->lifecycle_cmd_queue[inst->lifecycle_cmd_read++];
                        CMDLOG("Guest polled lifecycle cmd: %d", cmd);
                        return cmd;
                    }
                    return VP_GAME_CMD_NONE;  /* Nothing pending; see above */
                }

                case SYS_ANDROID_GAME_SWAP_INPUT: {
                    /* Swap input buffers: a1 = guest pointer to GameActivityInputBuffer */
                    if (!a1) {
                        return 0;
                    }
                    cmdpost_GameActivityInputBuffer* guest_buf = rvvm_user_guest_ptr((uint64_t)a1);
                    if (!guest_buf) {
                        return 0;
                    }
                    int32_t out_count = inst->motion_event_count;
                    if (out_count > 0) {
                        /* Copy motion events into guest-provided array */
                        cmdpost_GameActivityMotionEvent* dst =
                            rvvm_user_guest_ptr((uint64_t)(size_t)guest_buf->motionEvents);
                        int32_t capacity = guest_buf->motionEventsCapacity > 0
                                         ? guest_buf->motionEventsCapacity : CMDPOST_MAX_MOTION_EVENTS;
                        int32_t copy_count = out_count < capacity ? out_count : capacity;
                        if (dst) {
                            memcpy(dst, inst->motion_events, sizeof(cmdpost_GameActivityMotionEvent) * copy_count);
                            guest_buf->motionEventsCount = copy_count;
                            int32_t max_pointers = 0;
                            for (int32_t i = 0; i < copy_count; i++) {
                                if (inst->motion_events[i].pointerCount > max_pointers) {
                                    max_pointers = inst->motion_events[i].pointerCount;
                                }
                            }
                            CMDLOG("Guest swapped input: %d motion events (capacity %d, max pointers %d)",
                                   copy_count, capacity, max_pointers);
                        }
                    }
                    cmdpost_clear_motion_events(inst);
                    return out_count;
                }

                case SYS_ANDROID_GAME_CLEAR_INPUT: {
                    /* Clear input events: a1 = 0 for motion, 1 for key */
                    if (a1 == 0) {
                        cmdpost_clear_motion_events(inst);
                    } else {
                        cmdpost_clear_key_events(inst);
                    }
                    return 0;
                }

                case SYS_ANDROID_CHOREOGRAPHER_INIT: {
                    /* The host owns the vsync source; nothing to arm here.
                     * Report what the guest can expect: whether a vsync clock
                     * exists at all, and whether we can wake its Looper fd
                     * directly instead of it calling WAIT every frame. */
                    if (!inst->choreographer_wait_cb || inst->vsync_source_lost) {
                        return 0;   /* no source: guest uses its own 60Hz clock */
                    }
                    return VP_VSYNC_CAP_SOURCE | VP_VSYNC_CAP_FD_WAKEUP;
                }

                case SYS_ANDROID_CHOREOGRAPHER_WAIT: {
                    if (!inst->choreographer_wait_cb) {
                        return -1;   /* guest falls back to its own clock */
                    }
                    return inst->choreographer_wait_cb();
                }

                case SYS_ANDROID_CHOREOGRAPHER_SET_FD: {
                    /* Guest hands us the write end of the vsync pipe its Looper
                     * polls; a1 < 0 unregisters. (a0 is the sub-command.) */
                    if (!inst->choreographer_wait_cb || inst->vsync_source_lost) {
                        return -1;
                    }
                    inst->choreographer_fd = (int32_t)a1;
                    inst->vsync_armed = false;
                    return 0;
                }

                case SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC: {
                    /* One outstanding request per frame, answered by the vsync
                     * clock through vp_cmdpost_vsync_tick(). */
                    if (!inst->choreographer_wait_cb || inst->vsync_source_lost ||
                        inst->choreographer_fd < 0) {
                        return -1;   /* guest degrades to the blocking WAIT path */
                    }
                    inst->vsync_armed = true;
                    return 0;
                }

                /* ---------- Phase 5: AAudio ---------- */

                case SYS_ANDROID_AAUDIO_OPEN: {
                    /* a1 = vp_aaudio_config_t* (guest memory). */
                    if (!inst->audio_ops || !inst->audio_ops->open) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    /* a1 is a guest address: guest memory is no longer mapped
                     * into the host, so it has to be translated. */
                    const vp_aaudio_config_t* cfg = rvvm_user_guest_ptr((uint64_t)a1);
                    if (!cfg) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t slot = cmdpost_audio_alloc_slot(inst);
                    if (slot < 0) {
                        return VP_AUDIO_ERROR_NO_MEMORY;
                    }
                    void* user = NULL;
                    int32_t rc = inst->audio_ops->open(cfg, &user);
                    if (rc != VP_AUDIO_OK) {
                        cmdpost_audio_free_slot(inst, slot);
                        return rc;
                    }
                    cmdpost_audio_stream_t* stream = &inst->audio_streams[slot];
                    stream->user = user;
                    stream->cfg_copy = *cfg;
                    stream->direction = stream->cfg_copy.direction;
                    stream->frame_bytes = (int32_t)vp_audio_frame_bytes(stream->cfg_copy.format,
                                                                      stream->cfg_copy.channel_count);
                    stream->state = VP_AUDIO_STATE_OPEN;
                    cmdpost_audio_refresh_state(inst, stream);
                    return slot;
                }

                case SYS_ANDROID_AAUDIO_CLOSE: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t rc = inst->audio_ops && inst->audio_ops->close
                               ? inst->audio_ops->close(stream->user) : VP_AUDIO_OK;
                    cmdpost_audio_free_slot(inst, (int32_t)a1);
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_START:
                case SYS_ANDROID_AAUDIO_PAUSE:
                case SYS_ANDROID_AAUDIO_STOP: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    int32_t rc;
                    if (a0 == SYS_ANDROID_AAUDIO_START) {
                        rc = inst->audio_ops && inst->audio_ops->start
                           ? inst->audio_ops->start(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    } else if (a0 == SYS_ANDROID_AAUDIO_PAUSE) {
                        rc = inst->audio_ops && inst->audio_ops->pause
                           ? inst->audio_ops->pause(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    } else {
                        rc = inst->audio_ops && inst->audio_ops->stop
                           ? inst->audio_ops->stop(stream->user, a2) : VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    if (rc == VP_AUDIO_OK) {
                        cmdpost_audio_refresh_state(inst, stream);
                    }
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_FLUSH: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return inst->audio_ops && inst->audio_ops->flush
                         ? inst->audio_ops->flush(stream->user) : VP_AUDIO_ERROR_UNSUPPORTED;
                }

                case SYS_ANDROID_AAUDIO_WRITE: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream || !inst->audio_ops || !inst->audio_ops->write) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    const void* buf = rvvm_user_guest_ptr((uint64_t)a2);
                    int32_t frames = (int32_t)a3;
                    if (!buf || frames <= 0) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return inst->audio_ops->write(stream->user, buf, frames, stream->frame_bytes);
                }

                case SYS_ANDROID_AAUDIO_READ: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream || !inst->audio_ops || !inst->audio_ops->read) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    void* buf = rvvm_user_guest_ptr((uint64_t)a2);
                    int32_t frames = (int32_t)a3;
                    if (!buf || frames <= 0) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return inst->audio_ops->read(stream->user, buf, frames, stream->frame_bytes);
                }

                case SYS_ANDROID_AAUDIO_INFO: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    vp_aaudio_info_t* out = rvvm_user_guest_ptr((uint64_t)a2);
                    if (!stream || !out) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    if (!inst->audio_ops || !inst->audio_ops->get_info) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    int32_t rc = inst->audio_ops->get_info(stream->user, out);
                    if (rc == VP_AUDIO_OK) {
                        stream->state = out->state;
                    }
                    return rc;
                }

                case SYS_ANDROID_AAUDIO_TS: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    vp_aaudio_timestamp_t* out = rvvm_user_guest_ptr((uint64_t)a2);
                    if (!stream || !out) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    return inst->audio_ops && inst->audio_ops->get_timestamp
                         ? inst->audio_ops->get_timestamp(stream->user, out)
                         : VP_AUDIO_ERROR_UNSUPPORTED;
                }

                case SYS_ANDROID_AAUDIO_BUFSZ: {
                    cmdpost_audio_stream_t* stream = cmdpost_audio_lookup(inst, a1);
                    if (!stream) {
                        return VP_AUDIO_ERROR_INVALID_ARG;
                    }
                    if (!inst->audio_ops || !inst->audio_ops->set_buffer_size) {
                        return VP_AUDIO_ERROR_UNSUPPORTED;
                    }
                    int32_t applied = 0;
                    int32_t rc = inst->audio_ops->set_buffer_size(stream->user, (int32_t)a2, &applied);
                    return rc == VP_AUDIO_OK ? (int64_t)applied : (int64_t)rc;
                }

                case SYS_ANDROID_AAUDIO_QUERY: {
                    /* Capability probe: 0 means this host has no audio backend,
                     * so the guest stubs fail fast instead of hanging. */
                    if (!inst->audio_ops || !inst->audio_ops->query) {
                        return 0;
                    }
                    return (int64_t)inst->audio_ops->query();
                }

                default:
                    /*
                     * a0 doubles as this call's sub-command input and its return
                     * value, so a vCPU interrupted mid-experiment (stop/suspend)
                     * can present a leftover return value as if it were a command
                     * number. rvvm_user.c now drops those traps before dispatch,
                     * so reaching here means a genuinely unexpected command: keep
                     * the diagnostic, but do not feed -ENOSYS back into a guest
                     * that is no longer listening for a result.
                     */
                    rvvm_warn("cmdpost: Unknown Android sub-command %" PRId64, a0);
                    return 0;
            }
            break;
        }

        case SYS_EGL_CALL: {   /* a0 = guest gl_call* */
            gl_call* c = rvvm_user_guest_ptr((uint64_t)a0);
            if (!c) return -1;
            /* args[] carries guest addresses, which the GL backend is expected
             * to translate via rvvm_user_guest_ptr() before dereferencing */
            if (inst->egl_dispatch_cb) inst->egl_dispatch_cb((uint32_t)c->fn_id, c->args, &c->ret);
            else c->ret = 0;
            return 0;
        }
        case SYS_GL_CALL: {
            gl_call* c = rvvm_user_guest_ptr((uint64_t)a0);
            if (!c) return -1;
            if (inst->gl_dispatch_cb) inst->gl_dispatch_cb((uint32_t)c->fn_id, c->args, &c->ret);
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

/* Resolve the instance a lifecycle call applies to: the host's, or - for a run
 * with no host bound (rvvm_user_main.c) - the process-wide default one. */
static vp_cmdpost_t* cmdpost_lifecycle_instance(vp_cmdpost_t* inst)
{
    if (!inst && !g_default_instance) {
        g_default_instance = cmdpost_create();
        if (!g_default_instance) {
            /* Out of memory: nothing to reset, and every dispatch will have no
             * callbacks either - the same state a host-less run starts in. */
            return NULL;
        }
    }
    return inst ? inst : g_default_instance;
}

void cmdpost_init(vp_cmdpost_t* inst)
{
    inst = cmdpost_lifecycle_instance(inst);
    if (!inst) {
        return;
    }

    if (!inst->initialized) {
        printf("vp_cmdpost: Initializing Android NDK API proxy\n");
        inst->initialized = true;
    } else {
        printf("vp_cmdpost: Guest run starting\n");
    }

    /* Per-run state belongs to the run that is starting, not to the one that
     * ended. The core calls this from rvvm_user_linux_ex() on the guest thread
     * *after* the host has registered its callbacks for this run, so nothing
     * the host just installed (window/GL callbacks, the audio backend, the
     * sensor ops) may be touched here - only what describes this run.
     *
     * The APP_CMD_* dedup flags especially: window_initialized left true by
     * the previous guest would mean this one never receives its INIT_WINDOW. */
    inst->window_initialized = false;
    inst->input_initialized  = false;
    inst->looper_initialized = false;

    /* Vsync wait state: the guest starting now has its own Looper fd, and the
     * previous guest's "the clock is gone" verdict must not outlive the run it
     * was about. */
    inst->choreographer_fd   = -1;
    inst->vsync_armed        = false;
    inst->vsync_source_lost  = false;

    /* Nothing drains these queues while no guest is running, so anything a
     * host queued against the previous one must not reach this one. The hosts
     * clear them before a run as well; doing it here too is what makes this
     * function sufficient on its own. */
    cmdpost_clear_lifecycle_cmds(inst);
    cmdpost_clear_motion_events(inst);
}

/* Drop everything that belonged to the run that just ended. No printing: the
 * two callers below say which of them is running. */
static void cmdpost_drop_run_state(vp_cmdpost_t* inst)
{
    inst->window_initialized = false;
    inst->input_initialized  = false;
    inst->looper_initialized = false;
    inst->choreographer_fd   = -1;
    inst->vsync_armed        = false;
    inst->vsync_source_lost  = false;

    /* Sensor queues, the staging FIFO and the descriptor table this guest
     * enumerated all belong to the guest that just exited, and only to it:
     * vp_sensor_reset() drops this instance's queues and recomputes the device
     * aggregate, which switches off any sensor this guest was the last
     * subscriber of. The backend registration is the device's and stays. */
    vp_sensor_reset(inst->sensor);

    /* The guest that owned these queues is gone, so nothing will ever drain
     * them. A host that reuses the process (launcher: Run after Run) must not
     * hand the next guest the PAUSE/STOP/DESTROY left over from the previous
     * teardown - it would destroy the new activity on its very first poll. */
    cmdpost_clear_lifecycle_cmds(inst);
    cmdpost_clear_motion_events(inst);

    /* Tear down every live AAudio stream before dropping the backend. The next
     * run's registration points this at a fresh backend, and the guest that
     * drove these streams has exited, so its pump has nothing left to feed. */
    if (inst->audio_ops && inst->audio_ops->close) {
        for (int32_t i = 0; i < CMDPOST_MAX_AUDIO_STREAMS; i++) {
            if (inst->audio_streams[i].used) {
                inst->audio_ops->close(inst->audio_streams[i].user);
            }
        }
    }
    memset(inst->audio_streams, 0, sizeof(inst->audio_streams));
    inst->audio_ops = NULL;
}

void cmdpost_end_run(vp_cmdpost_t* inst)
{
    inst = cmdpost_instance_or_default(inst);
    if (!inst) {
        return;
    }
    printf("vp_cmdpost: Guest run ended\n");
    cmdpost_drop_run_state(inst);
}

void cmdpost_cleanup(vp_cmdpost_t* inst)
{
    inst = cmdpost_instance_or_default(inst);
    if (!inst || !inst->initialized) {
        return;
    }
    printf("vp_cmdpost: Cleaning up\n");

    /* The per-run state first: a run whose exit never came through
     * cmdpost_end_run() still gets its streams closed and its queues dropped
     * here, rather than being handed to whoever runs next. */
    cmdpost_drop_run_state(inst);

    /* Then the bridge itself. This is the host's to dismantle, and the host is
     * the only caller left (nativeDestroy / win32_host_shutdown): a guest's own
     * exit path must not do it, or the next run - which may already be starting
     * - finds its callbacks gone. */
    inst->window_lock_cb = NULL;
    inst->window_unlock_cb = NULL;
    inst->window_size_cb = NULL;
    inst->window_set_buf_cb = NULL;
    inst->config_get_cb = NULL;
    inst->game_lifecycle_cb = NULL;
    inst->game_input_cb = NULL;
    inst->egl_dispatch_cb = NULL;
    inst->gl_dispatch_cb = NULL;
    inst->choreographer_wait_cb = NULL;

    inst->initialized = false;
}
