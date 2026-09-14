/*
 * vp_cmdpost.h - Host-side dispatch header for Android NDK API proxy
 */

#ifndef vp_cmdpost_H
#define vp_cmdpost_H

#include <stdint.h>
#include <stdbool.h>

/* Sensor subsystem: vp_sensor_ops_t, vp_sensor_set_ops(), vp_sensor_ingest()
 * and the vp_sensor_dispatch() entry point used by the dispatcher below. */
#include "vp_sensor.h"

/* Shared PCM ring + AAudio transport ABI */
#include "virtpass/vp_audio_ringbuf.h"

/* ============================================================
 * Window buffer structure (must match Guest-side)
 * ============================================================ */
typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} cmdpost_ARect;

typedef struct {
    void* bits;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
} cmdpost_ANativeWindow_Buffer;

/* ============================================================
 * Callback function types
 *
 * Every guest-facing callback carries the instance it was dispatched
 * through as its first argument. A host that runs several guests keeps a
 * cmdpost instance per guest, and the callback is the only place where
 * "which guest is calling" is knowable - the instance IS that identity,
 * the host maps it back to its own run record. Single-guest hosts (the
 * win32 launcher today) simply ignore the argument.
 * ============================================================ */
typedef struct vp_cmdpost vp_cmdpost_t;

/* Window callbacks */
typedef int32_t (*window_lock_callback)(vp_cmdpost_t* inst, void* window, void* outBuffer, void* dirtyBounds);
typedef int32_t (*window_unlock_callback)(vp_cmdpost_t* inst, void* window, void* guestPixels);
typedef void (*window_size_callback)(vp_cmdpost_t* inst, int64_t* width, int64_t* height);
typedef int32_t (*window_set_buf_callback)(vp_cmdpost_t* inst, int32_t width, int32_t height, int32_t format);

/* Field selectors for SYS_ANDROID_CONFIG (passed in a1).
 * Internal transport encoding; mirrored in virtpass/vp_android.h. */
#ifndef VP_ACONFIG_QUERY_ORIENTATION
#define VP_ACONFIG_QUERY_ORIENTATION      0
#define VP_ACONFIG_QUERY_DENSITY          1
#define VP_ACONFIG_QUERY_SCREEN_SIZE      2
#define VP_ACONFIG_QUERY_SCREEN_LONG      3
#define VP_ACONFIG_QUERY_SCREEN_ROUND     4
#define VP_ACONFIG_QUERY_SCREEN_WIDTH_DP  5
#define VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP 6
#endif

/* Configuration callback: host fills *outValue for the requested field
 * (VP_ACONFIG_QUERY_* selector) and returns 0 on success. */
typedef int32_t (*config_get_callback)(vp_cmdpost_t* inst, int32_t field, int32_t* outValue);

/* GameActivity callbacks */
typedef void (*game_lifecycle_callback)(int32_t cmd);
typedef void (*game_input_callback)(void* motionEvent);

/* Phase 3: GL dispatch callbacks. host 在回调内填 *ret.
 * 0x2000+ 扩展函数时 args[0] = 函数名字符串的 guest 地址. */
typedef void (*egl_dispatch_callback)(vp_cmdpost_t* inst, uint32_t fn_id, const int64_t* args, int64_t* ret);
typedef void (*gl_dispatch_callback) (vp_cmdpost_t* inst, uint32_t fn_id, const int64_t* args, int64_t* ret);

/* ============================================================
 * Per-instance state
 *
 * One instance per host instance (today: one per process - the Android
 * activity and the win32 launcher each own exactly one). Everything below that
 * used to be a file-scope global hangs off this: the callback table, the
 * host->guest queues, the vsync wait state and the AAudio slot table. The host
 * creates it, registers its callbacks into it, hands it to the core with
 * rvvm_user_set_host_ctx() so a guest's ecall path can find the instance its
 * syscalls belong to, and destroys it at teardown.
 * ============================================================ */
vp_cmdpost_t* cmdpost_create(void);
void cmdpost_destroy(vp_cmdpost_t* inst);

/* ============================================================
 * GameActivity event structures (must match Guest ABI, packed)
 * ============================================================ */
#define CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT 16
#define CMDPOST_MAX_MOTION_EVENTS 16
#define CMDPOST_MAX_LIFECYCLE_CMDS 32

typedef struct {
    int64_t eventTime;
    int32_t deviceId;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t metaState;
    int32_t buttonState;
    float xPrecision;
    float yPrecision;
    float edgeFlags;
    int32_t pointerCount;
    struct {
        float x;
        float y;
        float rawX;
        float rawY;
        float pressure;
        float size;
        float touchMajor;
        float touchMinor;
        float toolMajor;
        float toolMinor;
        float orientation;
        int32_t id;
        int32_t toolType;
    } pointers[CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT];
} __attribute__((packed)) cmdpost_GameActivityMotionEvent;

typedef struct {
    int64_t eventTime;
    int32_t deviceId;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t keyCode;
    int32_t scanCode;
    int32_t metaState;
    int32_t repeatCount;
} __attribute__((packed)) cmdpost_GameActivityKeyEvent;

/* Input buffer layout shared with the Guest (must match ABI) */
typedef struct {
    cmdpost_GameActivityMotionEvent* motionEvents;
    int32_t motionEventsCount;
    int32_t motionEventsCapacity;
    cmdpost_GameActivityKeyEvent* keyEvents;
    int32_t keyEventsCount;
    int32_t keyEventsCapacity;
} __attribute__((packed)) cmdpost_GameActivityInputBuffer;

/* Queue lifecycle commands from the host side (called from JNI/Java) */
void cmdpost_queue_lifecycle_cmd(vp_cmdpost_t* inst, int32_t cmd);

/* Queue a motion event from the host side (called from JNI/Java) */
void cmdpost_queue_motion_event(vp_cmdpost_t* inst, const cmdpost_GameActivityMotionEvent* ev);

/* Clear queued host events */
void cmdpost_clear_lifecycle_cmds(vp_cmdpost_t* inst);
void cmdpost_clear_motion_events(vp_cmdpost_t* inst);
void cmdpost_clear_key_events(vp_cmdpost_t* inst);

/* Set callback functions (called from JNI/Android side) */
void cmdpost_set_window_callbacks(vp_cmdpost_t* inst,
                                   window_lock_callback lock,
                                   window_unlock_callback unlock);

void cmdpost_set_window_size_callback(vp_cmdpost_t* inst, window_size_callback size_cb);

void cmdpost_set_window_set_buf_callback(vp_cmdpost_t* inst, window_set_buf_callback set_buf_cb);

/* Register the host-side device configuration provider (AConfiguration_*). */
void cmdpost_set_config_callback(vp_cmdpost_t* inst, config_get_callback get_cb);

void cmdpost_set_game_callbacks(vp_cmdpost_t* inst,
                                 game_lifecycle_callback lifecycle,
                                 game_input_callback input);

/* Phase 3: register GL/EGL dispatch callbacks (gl_call layout in vp_cmdpost.c).
 * 未注册时 dispatch 仍成功但 ret=0：guest 可检测并退回 CPU 像素路径. */
void cmdpost_set_gl_callbacks(vp_cmdpost_t* inst,
                              egl_dispatch_callback egl, gl_dispatch_callback gl);

/* Phase 4: display vsync source for AChoreographer. The host registers a
 * blocking waiter returning the next frame time in nanoseconds (monotonic),
 * or a negative value when it has no vsync source. The guest's AChoreographer
 * stubs reach it through SYS_ANDROID_CHOREOGRAPHER_WAIT. */
typedef int64_t (*choreographer_wait_callback)(void);
void cmdpost_set_choreographer_callback(vp_cmdpost_t* inst, choreographer_wait_callback wait_cb);

/* Phase 4 (fd wakeup / 方案 B): the same vsync source can wake the guest
 * directly. The guest hands us the write end of a pipe that its Looper polls
 * (SYS_ANDROID_CHOREOGRAPHER_SET_FD) and asks for exactly one vsync per
 * request (SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC); we answer a request by
 * writing the frame time into that fd, without the guest ever polling.
 *
 * The platform host calls vp_cmdpost_vsync_tick() from its vsync clock. If the
 * clock goes away (activity destroyed) it must call
 * vp_cmdpost_vsync_source_lost(): that wakes a guest blocked in poll() once
 * with a negative frame time, so the stub degrades instead of hanging. */
bool vp_cmdpost_vsync_tick(vp_cmdpost_t* inst, int64_t frame_time_ns);
void vp_cmdpost_vsync_source_lost(vp_cmdpost_t* inst);

/* Capability bits reported to the guest by CHOREOGRAPHER_INIT.
 * Mirrored in virtpass/vp_android.h. */
#ifndef VP_VSYNC_CAP_SOURCE
#define VP_VSYNC_CAP_SOURCE    (1 << 0)
#define VP_VSYNC_CAP_FD_WAKEUP (1 << 1)
#endif

/* ============================================================
 * Phase 5: AAudio proxy
 *
 * vp_cmdpost.c owns the SYS_ANDROID_AAUDIO_* sub-commands and stream handle
 * table; it never talks to the platform audio stack itself. Each host instead
 * registers a small ops table below. With no table registered, every AAudio
 * call fails with VP_AUDIO_ERROR_UNSUPPORTED so guests degrade gracefully.
 * ============================================================ */
typedef struct vp_audio_ops {
    int32_t (*open)(const vp_aaudio_config_t* cfg, void** out_user);
    int32_t (*close)(void* user);
    int32_t (*start)(void* user, int64_t timeout_ns);
    int32_t (*pause)(void* user, int64_t timeout_ns);
    int32_t (*stop)(void* user, int64_t timeout_ns);
    int32_t (*flush)(void* user);
    /* Data-path passthrough: host copies between guest buffer and real AAudio.
     * `buf` is already a *host* pointer - cmdpost translates the guest address
     * before calling in. `frame_bytes` tells the host how many bytes per frame
     * (from the negotiated geometry). */
    int32_t (*write)(void* user, const void* buf, int32_t frames, int32_t frame_bytes);
    int32_t (*read)(void* user, void* buf, int32_t frames, int32_t frame_bytes);
    int32_t (*get_info)(void* user, vp_aaudio_info_t* out);
    int32_t (*get_timestamp)(void* user, vp_aaudio_timestamp_t* out);
    int32_t (*set_buffer_size)(void* user, int32_t frames, int32_t* applied_out);
    /* VP_AUDIO_CAP_* bitmask; 0 means this host has no audio backend. */
    uint32_t (*query)(void);
} vp_audio_ops_t;

/* Register the host audio backend. Pass NULL to detach (used on teardown). */
void cmdpost_set_audio_callbacks(vp_cmdpost_t* inst, const vp_audio_ops_t* ops);

/* Unified Android NDK API proxy syscall (sub-command passed in a0). The
 * numbers themselves live in virtpass/vp_syscall.h, shared verbatim with the
 * guest stub. */
#include "virtpass/vp_syscall.h"

/* Handle Android NDK API proxy syscall.
 *
 * `inst` is the host instance this guest's syscalls belong to, as bound with
 * rvvm_user_set_host_ctx(). It may be NULL: a host-less run (rvvm_user_main.c
 * boots a guest without any host bridge) has no instance to bind, and then the
 * call works against a stand-in whose callbacks are all unregistered - which is
 * exactly what the old file-scope globals meant in that case. */
int64_t cmdpost_dispatch(vp_cmdpost_t* inst, int64_t syscall_nr, int64_t a0, int64_t a1, int64_t a2,
                      int64_t a3, int64_t a4, int64_t a5, void* guest_mem);

/* ============================================================
 * Lifecycle: three calls with three different owners
 *
 * cmdpost_init()     per run, called by the core when a guest starts
 *                    (rvvm_user_linux_ex). Resets everything that belongs to
 *                    that run - the APP_CMD_* dedup flags, the vsync wait
 *                    state, the host->guest queues - and must NOT touch the
 *                    host's registrations, which were just made for this run
 *                    (window/GL callbacks, audio backend, sensor ops).
 *
 * cmdpost_end_run()  per run, called by the core when the guest has exited.
 *                    Drops that guest's queues, audio streams and sensor
 *                    state, and leaves the host bridge standing: a relaunched
 *                    guest must find its host still registered.
 *
 * cmdpost_cleanup()  once, called by the HOST when no guest will run again
 *                    (Android nativeDestroy / win32_host_shutdown). Dismantles
 *                    the bridge itself - callback table, audio backend.
 *
 * The split matters: before it, the guest-exit path called cmdpost_cleanup(),
 * so the previous guest's unwinding thread cleared callbacks the next run had
 * already registered, and the relaunch raced the teardown and lost (the new
 * guest then probed a dead proxy: no window, no GL, no audio).
 * ============================================================ */
void cmdpost_init(vp_cmdpost_t* inst);
void cmdpost_end_run(vp_cmdpost_t* inst);
void cmdpost_cleanup(vp_cmdpost_t* inst);

#endif /* vp_cmdpost_H */
