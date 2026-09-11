/*
 * vp_cmdpost.h - Host-side dispatch header for Android NDK API proxy
 */

#ifndef vp_cmdpost_H
#define vp_cmdpost_H

#include <stdint.h>
#include <stdbool.h>

/* Ring buffer for sensor events */
#include "virtpass/vp_sensor_ringbuf.h"

/* Shared PCM ring + AAudio transport ABI */
#include "virtpass/vp_audio_ringbuf.h"

/* Sensor event type (alias for compatibility) */
typedef sensor_event_t cmdpost_ASensorEvent;

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
 * ============================================================ */

/* Sensor callbacks */
typedef void (*sensor_init_callback)(void);
typedef void (*sensor_enable_callback)(int handle, bool enable);
typedef void (*sensor_data_callback)(sensor_event_t* event);

/* Window callbacks */
typedef int32_t (*window_lock_callback)(void* window, void* outBuffer, void* dirtyBounds);
typedef int32_t (*window_unlock_callback)(void* window, void* guestPixels);
typedef void (*window_size_callback)(int64_t* width, int64_t* height);
typedef int32_t (*window_set_buf_callback)(int32_t width, int32_t height, int32_t format);

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
typedef int32_t (*config_get_callback)(int32_t field, int32_t* outValue);

/* GameActivity callbacks */
typedef void (*game_lifecycle_callback)(int32_t cmd);
typedef void (*game_input_callback)(void* motionEvent);

/* Phase 3: GL dispatch callbacks. host 在回调内填 *ret.
 * 0x2000+ 扩展函数时 args[0] = 函数名字符串的 guest 地址. */
typedef void (*egl_dispatch_callback)(uint32_t fn_id, const int64_t* args, int64_t* ret);
typedef void (*gl_dispatch_callback) (uint32_t fn_id, const int64_t* args, int64_t* ret);

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
void cmdpost_queue_lifecycle_cmd(int32_t cmd);

/* Queue a motion event from the host side (called from JNI/Java) */
void cmdpost_queue_motion_event(const cmdpost_GameActivityMotionEvent* ev);

/* Clear queued host events */
void cmdpost_clear_lifecycle_cmds(void);
void cmdpost_clear_motion_events(void);
void cmdpost_clear_key_events(void);

/* Set callback functions (called from JNI/Android side) */
void cmdpost_set_sensor_callbacks(sensor_init_callback init,
                                   sensor_enable_callback enable,
                                   sensor_data_callback data);

void cmdpost_set_window_callbacks(window_lock_callback lock,
                                   window_unlock_callback unlock);

void cmdpost_set_window_size_callback(window_size_callback size_cb);

void cmdpost_set_window_set_buf_callback(window_set_buf_callback set_buf_cb);

/* Register the host-side device configuration provider (AConfiguration_*). */
void cmdpost_set_config_callback(config_get_callback get_cb);

void cmdpost_set_game_callbacks(game_lifecycle_callback lifecycle,
                                 game_input_callback input);

/* Phase 3: register GL/EGL dispatch callbacks (gl_call layout in vp_cmdpost.c).
 * 未注册时 dispatch 仍成功但 ret=0：guest 可检测并退回 CPU 像素路径. */
void cmdpost_set_gl_callbacks(egl_dispatch_callback egl, gl_dispatch_callback gl);

/* Phase 4: display vsync source for AChoreographer. The host registers a
 * blocking waiter returning the next frame time in nanoseconds (monotonic),
 * or a negative value when it has no vsync source. The guest's AChoreographer
 * stubs reach it through SYS_ANDROID_CHOREOGRAPHER_WAIT. */
typedef int64_t (*choreographer_wait_callback)(void);
void cmdpost_set_choreographer_callback(choreographer_wait_callback wait_cb);

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
bool vp_cmdpost_vsync_tick(int64_t frame_time_ns);
void vp_cmdpost_vsync_source_lost(void);

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
void cmdpost_set_audio_callbacks(const vp_audio_ops_t* ops);

/* Initialize the sensor ring buffer */
void cmdpost_init_sensor_ringbuf(sensor_ringbuf_t* ringbuf);

/* Push a sensor event from the host side */
void cmdpost_push_sensor_event(const sensor_event_t* event);

/* Unified Android NDK API proxy syscall: sub-command passed in a0 */
#define SYS_ANDROID_CALL   0x10022

/* Handle Android NDK API proxy syscall */
int64_t cmdpost_dispatch(int64_t syscall_nr, int64_t a0, int64_t a1, int64_t a2,
                      int64_t a3, int64_t a4, int64_t a5, void* guest_mem);

/* Initialization and cleanup */
void cmdpost_init(void);
void cmdpost_cleanup(void);

#endif /* vp_cmdpost_H */
