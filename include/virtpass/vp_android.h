/*
 * vp_ndk_stub.h - NDK API proxy header
 *
 * This header provides the same API surface as Android NDK headers,
 * allowing RISC-V Guest programs to compile against it.
 *
 * Usage in Guest program:
 *   #include "vp_ndk_stub.h"
 *   ASensorManager* mgr = ASensorManager_getInstance();
 */

#ifndef VIRTPASS_ANDROID
#define VIRTPASS_ANDROID

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* ============================================================
 * Custom syscall numbers (must match rvvm-user handler)
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

/* ============================================================
 * Lifecycle Commands (NativeAppGlueAppCmd)
 * ============================================================ */
enum {
    APP_CMD_INPUT_CHANGED,
    APP_CMD_INIT_WINDOW,
    APP_CMD_TERM_WINDOW,
    APP_CMD_WINDOW_RESIZED,
    APP_CMD_WINDOW_REDRAW_NEEDED,
    APP_CMD_CONTENT_RECT_CHANGED,
    APP_CMD_GAINED_FOCUS,
    APP_CMD_LOST_FOCUS,
    APP_CMD_CONFIG_CHANGED,
    APP_CMD_LOW_MEMORY,
    APP_CMD_START,
    APP_CMD_RESUME,
    APP_CMD_SAVE_STATE,
    APP_CMD_PAUSE,
    APP_CMD_STOP,
    APP_CMD_DESTROY,
    APP_CMD_WINDOW_INSETS_CHANGED,
};

/* ============================================================
 * Input Event Constants (android/input.h)
 * ============================================================ */
#define AMOTION_EVENT_ACTION_MASK         0xFF
#define AMOTION_EVENT_ACTION_POINTER_INDEX_MASK  0xFF00
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8

#define AMOTION_EVENT_ACTION_DOWN         0
#define AMOTION_EVENT_ACTION_UP           1
#define AMOTION_EVENT_ACTION_MOVE         2
#define AMOTION_EVENT_ACTION_CANCEL       3
#define AMOTION_EVENT_ACTION_POINTER_DOWN 5
#define AMOTION_EVENT_ACTION_POINTER_UP   6

#define AMOTION_EVENT_AXIS_X              0
#define AMOTION_EVENT_AXIS_Y              1
#define AMOTION_EVENT_AXIS_PRESSURE       2
#define AMOTION_EVENT_AXIS_SIZE           3
#define AMOTION_EVENT_AXIS_TOUCH_MAJOR    4
#define AMOTION_EVENT_AXIS_TOUCH_MINOR    5
#define AMOTION_EVENT_AXIS_TOOL_MAJOR     6
#define AMOTION_EVENT_AXIS_TOOL_MINOR     7
#define AMOTION_EVENT_AXIS_ORIENTATION    8

#define AINPUT_SOURCE_TOUCHSCREEN         0x00001002
#define AINPUT_SOURCE_MOUSE              0x00002002
#define AINPUT_SOURCE_KEYBOARD           0x00000101
#define AINPUT_SOURCE_GAMEPAD            0x00000401

#define AKEYCODE_HOME                    3
#define AKEYCODE_BACK                    4

/* ============================================================
 * GameActivity MotionEvent (packed for ABI stability)
 * ============================================================ */
#define GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT 16

typedef struct {
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
} __attribute__((packed)) GameActivityPointerAxes;

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
    GameActivityPointerAxes pointers[GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT];
} __attribute__((packed)) GameActivityMotionEvent;

/* ============================================================
 * GameActivity KeyEvent (packed for ABI stability)
 * ============================================================ */
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
} __attribute__((packed)) GameActivityKeyEvent;

/* ============================================================
 * GameActivity Input Buffer
 * ============================================================ */
typedef struct {
    GameActivityMotionEvent* motionEvents;
    int32_t motionEventsCount;
    int32_t motionEventsCapacity;
    GameActivityKeyEvent* keyEvents;
    int32_t keyEventsCount;
    int32_t keyEventsCapacity;
} __attribute__((packed)) GameActivityInputBuffer;

/* ============================================================
 * ANativeWindow forward declaration
 * ============================================================ */
typedef struct ANativeWindow ANativeWindow;

/* ============================================================
 * android_app (simplified NativeAppGlue struct)
 * ============================================================ */
typedef struct android_app android_app;
typedef void (*app_cmd_handler)(android_app* app, int cmd);

typedef struct android_app {
    void* userData;
    app_cmd_handler onAppCmd;
    
    ANativeWindow* window;
    int32_t destroyRequested;
    int32_t activityState;
    
    /* Input buffer */
    GameActivityInputBuffer inputBuffer;
    
    /* Internal state */
    int32_t cmdPipe[2];  /* Pipe for lifecycle commands */
    int32_t inputPipe[2]; /* Pipe for input events */
} android_app;

/* ============================================================
 * ARect (android/rect.h)
 * ============================================================ */
typedef struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} ARect;

/* ============================================================
 * ANativeWindow_Buffer (android/native_window.h)
 * ============================================================ */
typedef struct ANativeWindow_Buffer {
    void* bits;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
} ANativeWindow_Buffer;

/* ============================================================
 * Pixel formats (android/native_window.h)
 * ============================================================ */
#define WINDOW_FORMAT_RGBA_8888    1
#define WINDOW_FORMAT_RGBX_8888    2
#define WINDOW_FORMAT_RGB_565      4

/* ============================================================
 * Sensor API (android/sensor.h)
 * ============================================================ */

/* Sensor types */
#define ASENSOR_TYPE_INVALID                (-1)
#define ASENSOR_TYPE_ACCELEROMETER          1
#define ASENSOR_TYPE_MAGNETIC_FIELD         2
#define ASENSOR_TYPE_GYROSCOPE              4
#define ASENSOR_TYPE_LIGHT                  5
#define ASENSOR_TYPE_PRESSURE               6
#define ASENSOR_TYPE_PROXIMITY              8
#define ASENSOR_TYPE_GRAVITY                9
#define ASENSOR_TYPE_LINEAR_ACCELERATION    10
#define ASENSOR_TYPE_ROTATION_VECTOR        11
#define ASENSOR_TYPE_RELATIVE_HUMIDITY      12
#define ASENSOR_TYPE_AMBIENT_TEMPERATURE    13

/* Sensor accuracy */
#define ASENSOR_STATUS_NO_CONTACT      (-1)
#define ASENSOR_STATUS_UNRELIABLE      0
#define ASENSOR_STATUS_ACCURACY_LOW    1
#define ASENSOR_STATUS_ACCURACY_MEDIUM 2
#define ASENSOR_STATUS_ACCURACY_HIGH   3

/* Reporting modes */
#define AREPORTING_MODE_INVALID       (-1)
#define AREPORTING_MODE_CONTINUOUS    0
#define AREPORTING_MODE_ON_CHANGE     1
#define AREPORTING_MODE_ONE_SHOT      2
#define AREPORTING_MODE_SPECIAL_TRIGGER 3

/* Sensor event (packed for ABI stability) */
typedef struct {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        float data[16];
        struct {
            float x;
            float y;
            float z;
            float pad[13];
        } vector;
        struct {
            float azimuth;
            float pitch;
            float roll;
            float pad[13];
        } orientation;
    };
    uint32_t flags;
    int32_t reserved1[3];
} __attribute__((packed)) ASensorEvent;

/* Opaque types */
typedef struct ASensorManager ASensorManager;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ASensor ASensor;
typedef struct ALooper ALooper;

/* Sensor Manager */
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName);
ASensorManager* ASensorManager_getInstance(void);
int ASensorManager_getSensorList(ASensorManager* manager, ASensor const** list);
int ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensor const** list);
ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type);
ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp);

/* Event Queue */
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager,
        ALooper* looper, int ident, void* callback, void* data);
int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue);
int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor);
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor);
int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec);
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue);
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count);
int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor,
        int32_t samplingPeriodUs, int64_t maxBatchReportLatencyUs);
int ASensorEventQueue_requestAdditionalInfoEvents(ASensorEventQueue* queue, bool enable);

/* Sensor Info */
const char* ASensor_getName(ASensor const* sensor);
const char* ASensor_getVendor(ASensor const* sensor);
int ASensor_getType(ASensor const* sensor);
float ASensor_getResolution(ASensor const* sensor);
int ASensor_getMinDelay(ASensor const* sensor);
int ASensor_getFifoMaxEventCount(ASensor const* sensor);
int ASensor_getFifoReservedEventCount(ASensor const* sensor);
const char* ASensor_getStringType(ASensor const* sensor);
int ASensor_getReportingMode(ASensor const* sensor);
bool ASensor_isWakeUpSensor(ASensor const* sensor);
int ASensor_getHandle(ASensor const* sensor);
bool ASensor_isDirectChannelTypeSupported(ASensor const* sensor, int channelType);
int ASensor_getHighestDirectReportRateLevel(ASensor const* sensor);

/* ============================================================
 * Window API (android/native_window.h)
 * ============================================================ */

ANativeWindow* ANativeWindow_acquire(void* window);
void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_getWidth(ANativeWindow* window);
int32_t ANativeWindow_getHeight(ANativeWindow* window);
int32_t ANativeWindow_getFormat(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);

/* ============================================================
 * Configuration API (android/configuration.h)
 *
 * Mirrors the public NDK surface. The device configuration is owned
 * by the host; every getter proxies to it through SYS_ANDROID_CONFIG
 * (field selector passed in a1). Exact PPI is intentionally absent:
 * the public NDK only exposes quantised density buckets.
 * ============================================================ */

typedef struct AConfiguration AConfiguration;

/* Screen size classification */
enum {
    ACONFIGURATION_SCREENSIZE_ANY    = 0x00,
    ACONFIGURATION_SCREENSIZE_SMALL  = 0x01,
    ACONFIGURATION_SCREENSIZE_NORMAL = 0x02,
    ACONFIGURATION_SCREENSIZE_LARGE  = 0x03,
    ACONFIGURATION_SCREENSIZE_XLARGE = 0x04,
};

/* Density buckets (quantised, not exact PPI) */
enum {
    ACONFIGURATION_DENSITY_DEFAULT = 0,
    ACONFIGURATION_DENSITY_LOW     = 120,
    ACONFIGURATION_DENSITY_MEDIUM  = 160,
    ACONFIGURATION_DENSITY_TV      = 213,
    ACONFIGURATION_DENSITY_HIGH    = 240,
    ACONFIGURATION_DENSITY_XHIGH   = 320,
    ACONFIGURATION_DENSITY_XXHIGH  = 480,
    ACONFIGURATION_DENSITY_XXXHIGH = 640,
    ACONFIGURATION_DENSITY_ANY     = 0xfffe,
    ACONFIGURATION_DENSITY_NONE    = 0xffff,
};

/* Screen aspect (long / not long) */
enum {
    ACONFIGURATION_SCREENLONG_ANY = 0x00,
    ACONFIGURATION_SCREENLONG_NO  = 0x1,
    ACONFIGURATION_SCREENLONG_YES = 0x2,
};

/* Screen shape (round / not round) */
enum {
    ACONFIGURATION_SCREENROUND_ANY = 0x00,
    ACONFIGURATION_SCREENROUND_NO  = 0x1,
    ACONFIGURATION_SCREENROUND_YES = 0x2,
};

/* Screen orientation */
enum {
    ACONFIGURATION_ORIENTATION_ANY    = 0x0000,
    ACONFIGURATION_ORIENTATION_PORT   = 0x0001,
    ACONFIGURATION_ORIENTATION_LAND   = 0x0002,
    ACONFIGURATION_ORIENTATION_SQUARE = 0x0003,
};

/* Field selectors for SYS_ANDROID_CONFIG (passed in a1).
 * Internal transport encoding; mirrored in vp_cmdpost.h. */
#ifndef VP_ACONFIG_QUERY_ORIENTATION
#define VP_ACONFIG_QUERY_ORIENTATION      0
#define VP_ACONFIG_QUERY_DENSITY          1
#define VP_ACONFIG_QUERY_SCREEN_SIZE      2
#define VP_ACONFIG_QUERY_SCREEN_LONG      3
#define VP_ACONFIG_QUERY_SCREEN_ROUND     4
#define VP_ACONFIG_QUERY_SCREEN_WIDTH_DP  5
#define VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP 6
#endif

AConfiguration* AConfiguration_new(void);
void AConfiguration_delete(AConfiguration* config);
void AConfiguration_copy(AConfiguration* dest, AConfiguration* src);

int32_t AConfiguration_getScreenSize(AConfiguration* config);
int32_t AConfiguration_getScreenWidthDp(AConfiguration* config);
int32_t AConfiguration_getScreenHeightDp(AConfiguration* config);
int32_t AConfiguration_getDensity(AConfiguration* config);
int32_t AConfiguration_getScreenLong(AConfiguration* config);
int32_t AConfiguration_getScreenRound(AConfiguration* config);
int32_t AConfiguration_getOrientation(AConfiguration* config);

/* ============================================================
 * Input API (android/input.h)
 * ============================================================ */

#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2

typedef struct AInputEvent AInputEvent;
typedef struct AInputQueue AInputQueue;

AInputQueue* AInputQueue_create(void* looper, int ident);
void AInputQueue_destroy(AInputQueue* queue);
int AInputQueue_getEvent(AInputQueue* queue, AInputEvent** event);
int AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event);
void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled);

int32_t AInputEvent_getType(AInputEvent* event);
int32_t AKeyEvent_getKeyCode(AInputEvent* event);
float AMotionEvent_getX(AInputEvent* event, int32_t pointerIndex);
float AMotionEvent_getY(AInputEvent* event, int32_t pointerIndex);
int32_t AMotionEvent_getAction(AInputEvent* event);

/* ============================================================
 * GameActivity API (game-activity/GameActivity.h)
 * ============================================================ */

/* Create/destroy the android_app */
android_app* android_app_create(void);
void android_app_destroy(android_app* app);

/* Read the next lifecycle command (returns APP_CMD_* or -1 if none) */
int32_t android_app_read_cmd(android_app* app);

/* Execute a lifecycle command (calls onAppCmd callback) */
void android_app_exec_cmd(android_app* app, int32_t cmd);

/* Swap input buffers (returns number of motion events) */
int32_t android_app_swap_input_buffers(android_app* app);

/* Clear motion events after processing */
void android_app_clear_motion_events(android_app* app);

/* Clear key events after processing */
void android_app_clear_key_events(android_app* app);

/* Get pointer axis value */
float GameActivityPointerAxes_getAxisValue(const GameActivityPointerAxes* pointer, int32_t axis);

/* ============================================================
 * Looper API (android/looper.h)
 * ============================================================
 * The Looper is fd-driven, like the real one: ALooper_addFd() registers a
 * descriptor with the looper, and ALooper_pollOnce()/ALooper_pollAll() block
 * in a real poll() until one of them is ready, then dispatch every ready fd
 * to its own callback. The AChoreographer vsync source is registered as one
 * more fd, so it composes with any other event source the guest adds (input,
 * assets, sockets, ...) instead of owning the whole pump.
 */

#define ALOOPER_POLL_WAKE       (-1)
#define ALOOPER_POLL_CALLBACK   (-2)
#define ALOOPER_POLL_TIMEOUT    (-3)
#define ALOOPER_POLL_ERROR      (-4)
#define ALOOPER_POLL_INVALID    (-5)

#define ALOOPER_EVENT_INPUT     (1 << 0)
#define ALOOPER_EVENT_OUTPUT    (1 << 1)
#define ALOOPER_EVENT_ERROR     (1 << 2)
#define ALOOPER_EVENT_HANGUP    (1 << 3)
#define ALOOPER_EVENT_INVALID   (1 << 4)

#define ALOOPER_PREPARE_ALLOW_NON_CALLBACKS (1 << 0)

/* Invoked when a fd registered with ident == ALOOPER_POLL_CALLBACK becomes
 * ready. Returning 0 removes the fd from the looper (NDK contract). */
typedef int (*ALooper_callbackFunc)(int fd, int events, void* data);

ALooper* ALooper_prepare(int opts);
int ALooper_pollAll(int timeoutMillis, int* events, void** data, void** source);
int ALooper_pollOnce(int timeoutMillis, int* events, void** data, void** source);
int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                  ALooper_callbackFunc callback, void* data);
int ALooper_removeFd(ALooper* looper, int fd);

/* ============================================================
 * Choreographer API (android/choreographer.h)
 * ============================================================
 * Mirrors the NDK contract: postFrameCallback() queues the callback and
 * returns immediately; the callback runs on the thread that pumps the
 * Looper, once per display vsync, with the vsync frame time in nanoseconds.
 *
 * The vsync source itself is owned by the host (real AChoreographer on
 * Android, the compositor clock elsewhere). Like the real NDK, it is
 * delivered as a fd that the stub registers into the Looper: the stub asks
 * the host for exactly one vsync per postFrameCallback() (requestNextVsync
 * semantics) and the host answers by writing the frame time into a pipe. A
 * guest that pumps the Looper with ALooper_pollAll(-1) therefore blocks in
 * poll() until the display frame lands, instead of polling on its own.
 *
 * Hosts without fd wakeup support answer CHOREOGRAPHER_INIT without the
 * VP_VSYNC_CAP_FD_WAKEUP capability; the stub then degrades to a blocking
 * CHOREOGRAPHER_WAIT call, and finally to a 60Hz guest-clock tick.
 */

typedef struct AChoreographer AChoreographer;

typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void* data);
typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void* data);

/* Capability bits returned by SYS_ANDROID_CHOREOGRAPHER_INIT (bitmask in a0,
 * negative when the host has no vsync support at all). */
#define VP_VSYNC_CAP_SOURCE    (1 << 0)   /* host has a vsync clock (WAIT works) */
#define VP_VSYNC_CAP_FD_WAKEUP (1 << 1)   /* host can wake a guest fd per vsync */

AChoreographer* AChoreographer_getInstance(void);
void AChoreographer_postFrameCallback(AChoreographer* choreographer,
                                      AChoreographer_frameCallback callback,
                                      void* data);
void AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer,
                                             AChoreographer_frameCallback callback,
                                             void* data, long delayMillis);
void AChoreographer_postFrameCallback64(AChoreographer* choreographer,
                                        AChoreographer_frameCallback64 callback,
                                        void* data);
void AChoreographer_postFrameCallbackDelayed64(AChoreographer* choreographer,
                                               AChoreographer_frameCallback64 callback,
                                               void* data, uint32_t delayMillis);

#endif /* VIRTPASS_ANDROID */
