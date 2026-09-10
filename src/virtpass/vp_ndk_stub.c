/*
 * vp_ndk_stub.c - NDK API proxy via custom syscalls
 *
 * This library provides stub implementations of Android NDK APIs.
 * Instead of calling real NDK functions, it serializes the call
 * parameters and issues a custom syscall (0x10000+) to rvvm-user,
 * which dispatches to the host-side vp_cmdpost for real execution.
 *
 * Build: Static library linked into RISC-V Guest ELF
 * Target: rv64 (RISC-V 64-bit)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include "virtpass/vp_android.h"
/*
 * Custom syscall numbers for Android NDK API proxying.
 * These are in a private range (0x10000+) that doesn't conflict
 * with Linux RISC-V syscall numbers (which go up to ~439).
 */
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

/* GameActivity input constants */
#define AMOTION_EVENT_ACTION_DOWN         0
#define AMOTION_EVENT_ACTION_UP           1
#define AMOTION_EVENT_ACTION_MOVE         2

/* GameActivity input structures */
#define GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT 16
/*
 * Inline syscall wrapper for RISC-V 64-bit.
 * Uses ecall instruction to trap into rvvm-user.
 * Argument registers a0-a6 carry the sub-command and its operands; a7 carries
 * the syscall number. rvvm-user reads a0-a5 and forwards them to
 * cmdpost_dispatch(), a6 is reserved for future use.
 */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long t0 __asm__("a7") = nr;
    register long t1 __asm__("a0") = a0;
    register long t2 __asm__("a1") = a1;
    register long t3 __asm__("a2") = a2;
    register long t4 __asm__("a3") = a3;
    register long t5 __asm__("a4") = a4;
    register long t6 __asm__("a5") = a5;
    register long t7 __asm__("a6") = a6;

    __asm__ __volatile__(
        "ecall"
        : "+r"(t1)
        : "r"(t2), "r"(t3), "r"(t4), "r"(t5), "r"(t6), "r"(t7), "r"(t0)
        : "memory"
    );
    return t1;
}

/* ============================================================
 * Sensor API Stubs (android/sensor.h)
 * ============================================================ */

/* Opaque types (same as NDK) */
typedef struct ASensorManager ASensorManager;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ASensor ASensor;
typedef struct ALooper ALooper;

/* Sensor type constants */
#define ASENSOR_TYPE_ACCELEROMETER       1
#define ASENSOR_TYPE_MAGNETIC_FIELD      2
#define ASENSOR_TYPE_GYROSCOPE           4
#define ASENSOR_TYPE_LIGHT               5
#define ASENSOR_TYPE_PRESSURE            6
#define ASENSOR_TYPE_PROXIMITY           8

/* ASensorEvent is defined (packed, ABI stable) in virtpass/vp_android.h */

/* Stub sensor manager (just an ID) */
struct ASensorManager {
    int32_t id;
};

/* Stub sensor (just a type + handle) */
struct ASensor {
    int32_t type;
    int32_t handle;
    char name[64];
    char vendor[64];
    float resolution;
    int32_t min_delay;
};

/* Stub event queue */
struct ASensorEventQueue {
    int32_t id;
    int32_t fd;
};

/* Static instances for simplicity */
static ASensorManager g_sensor_manager = { .id = 0 };
static ASensorEventQueue g_sensor_queue = { .id = 0, .fd = -1 };
static ASensor g_sensors[8] = {
    { .type = ASENSOR_TYPE_ACCELEROMETER, .handle = 0, .name = "accel", .vendor = "stub", .resolution = 0.01f, .min_delay = 10000 },
    { .type = ASENSOR_TYPE_MAGNETIC_FIELD, .handle = 1, .name = "mag", .vendor = "stub", .resolution = 0.1f, .min_delay = 10000 },
    { .type = ASENSOR_TYPE_GYROSCOPE, .handle = 2, .name = "gyro", .vendor = "stub", .resolution = 0.01f, .min_delay = 10000 },
    { .type = ASENSOR_TYPE_LIGHT, .handle = 3, .name = "light", .vendor = "stub", .resolution = 1.0f, .min_delay = 0 },
    { .type = ASENSOR_TYPE_PRESSURE, .handle = 4, .name = "pressure", .vendor = "stub", .resolution = 0.1f, .min_delay = 0 },
    { .type = ASENSOR_TYPE_PROXIMITY, .handle = 5, .name = "proximity", .vendor = "stub", .resolution = 1.0f, .min_delay = 0 },
};

#define SENSOR_COUNT (sizeof(g_sensors) / sizeof(g_sensors[0]))

/* NDK API: Get sensor manager instance */
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName)
{
    /* Tell host to initialize sensor system */
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_INIT, 0, 0, 0, 0, 0, 0);
    return &g_sensor_manager;
}

ASensorManager* ASensorManager_getInstance(void)
{
    return ASensorManager_getInstanceForPackage(NULL);
}

/* NDK API: Get list of available sensors */
int ASensorManager_getSensorList(ASensorManager* manager, ASensor const** list)
{
    (void)manager;
    if (list) {
        static ASensor const* sensor_ptrs[SENSOR_COUNT];
        for (size_t i = 0; i < SENSOR_COUNT; i++) {
            sensor_ptrs[i] = &g_sensors[i];
        }
        *list = sensor_ptrs[0];
    }
    return (int)SENSOR_COUNT;
}

/* NDK API: Get default sensor by type */
ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type)
{
    (void)manager;
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        if (g_sensors[i].type == type) {
            return &g_sensors[i];
        }
    }
    return (void*)0;
}

ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp)
{
    (void)wakeUp;
    return ASensorManager_getDefaultSensor(manager, type);
}

/* NDK API: Create event queue */
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager,
        ALooper* looper, int ident, void* callback, void* data)
{
    (void)manager;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    /* Tell host to create sensor event queue */
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_INIT, 1, 0, 0, 0, 0, 0);
    return &g_sensor_queue;
}

/* NDK API: Destroy event queue */
int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue)
{
    (void)manager;
    (void)queue;
    return 0;
}

/* NDK API: Enable sensor */
int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    (void)queue;
    if (!sensor) return -1;
    /* Tell host to enable this sensor */
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_ENABLE, sensor->handle, 1, 0, 0, 0, 0);
    return 0;
}

/* NDK API: Disable sensor */
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    (void)queue;
    if (!sensor) return -1;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_ENABLE, sensor->handle, 0, 0, 0, 0, 0);
    return 0;
}

/* NDK API: Set event rate */
int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec)
{
    (void)queue;
    (void)sensor;
    (void)usec;
    return 0;
}

/* NDK API: Check for pending events */
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue)
{
    (void)queue;
    /* Ask host if there are events */
    return (int)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_READ, 0, 0, 0, 0, 0, 0);
}

/* NDK API: Get sensor events */
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count)
{
    (void)queue;
    if (!events || count == 0) return 0;
    /* Ask host to fill events buffer */
    return (ssize_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_SENSOR_READ,
                                    (long)events, (long)count, 0, 0, 0, 0);
}

/* NDK API: Get sensor name */
const char* ASensor_getName(ASensor const* sensor)
{
    if (!sensor) return "unknown";
    return sensor->name;
}

/* NDK API: Get sensor vendor */
const char* ASensor_getVendor(ASensor const* sensor)
{
    if (!sensor) return "unknown";
    return sensor->vendor;
}

/* NDK API: Get sensor type */
int ASensor_getType(ASensor const* sensor)
{
    if (!sensor) return -1;
    return sensor->type;
}

/* NDK API: Get sensor resolution */
float ASensor_getResolution(ASensor const* sensor)
{
    if (!sensor) return 0.0f;
    return sensor->resolution;
}

/* NDK API: Get minimum delay */
int ASensor_getMinDelay(ASensor const* sensor)
{
    if (!sensor) return 0;
    return sensor->min_delay;
}

/* NDK API: Get FIFO counts */
int ASensor_getFifoMaxEventCount(ASensor const* sensor)
{
    (void)sensor;
    return 0;
}

int ASensor_getFifoReservedEventCount(ASensor const* sensor)
{
    (void)sensor;
    return 0;
}

/* NDK API: Get string type */
const char* ASensor_getStringType(ASensor const* sensor)
{
    (void)sensor;
    return "";
}

/* NDK API: Get reporting mode */
int ASensor_getReportingMode(ASensor const* sensor)
{
    (void)sensor;
    return 0; /* AREPORTING_MODE_CONTINUOUS */
}

/* NDK API: Is wake-up sensor */
bool ASensor_isWakeUpSensor(ASensor const* sensor)
{
    (void)sensor;
    return false;
}

/* NDK API: Get handle */
int ASensor_getHandle(ASensor const* sensor)
{
    if (!sensor) return -1;
    return sensor->handle;
}

/* NDK API: Register sensor with custom params */
int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor,
        int32_t samplingPeriodUs, int64_t maxBatchReportLatencyUs)
{
    (void)queue;
    if (!sensor) return -1;
    (void)samplingPeriodUs;
    (void)maxBatchReportLatencyUs;
    return ASensorEventQueue_enableSensor(queue, sensor);
}

/* NDK API: Request additional info events */
int ASensorEventQueue_requestAdditionalInfoEvents(ASensorEventQueue* queue, bool enable)
{
    (void)queue;
    (void)enable;
    return 0;
}

/* NDK API: Direct channel support */
bool ASensor_isDirectChannelTypeSupported(ASensor const* sensor, int channelType)
{
    (void)sensor;
    (void)channelType;
    return false;
}

int ASensor_getHighestDirectReportRateLevel(ASensor const* sensor)
{
    (void)sensor;
    return 0; /* ASENSOR_DIRECT_RATE_STOP */
}

/* NDK API: Get dynamic sensor list */
int ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensor const** list)
{
    (void)manager;
    (void)list;
    return 0;
}

/* ============================================================
 * Window API Stubs (android/native_window.h)
 * ============================================================ */

typedef struct ANativeWindow ANativeWindow;

/* ARect and ANativeWindow_Buffer are defined in virtpass/vp_android.h */

/* Pixel formats */
#define WINDOW_FORMAT_RGBA_8888    1
#define WINDOW_FORMAT_RGBX_8888    2
#define WINDOW_FORMAT_RGB_565      4

struct ANativeWindow {
    int32_t width;
    int32_t height;
    int32_t format;
    void* shared_buffer;  /* Pointer to shared memory for pixel data */
};

static ANativeWindow g_window = { .width = 0, .height = 0, .format = 0 };

ANativeWindow* ANativeWindow_acquire(void* window)
{
    (void)window;
    return &g_window;
}

void ANativeWindow_release(ANativeWindow* window)
{
    (void)window;
}

/* Query the host for the real window size and cache it in g_window */
static void guest_window_query_size(void)
{
    int64_t packed = (int64_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_GET_SIZE, 0, 0, 0, 0, 0, 0);
    int32_t w = (int32_t)((uint64_t)packed & 0xFFFFFFFFu);
    int32_t h = (int32_t)(((uint64_t)packed >> 32) & 0xFFFFFFFFu);
    if (w > 0) g_window.width = w;
    if (h > 0) g_window.height = h;
}

int32_t ANativeWindow_getWidth(ANativeWindow* window)
{
    if (!window) window = &g_window;
    if (window->width <= 0) guest_window_query_size();
    return window->width;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window)
{
    if (!window) window = &g_window;
    if (window->height <= 0) guest_window_query_size();
    return window->height;
}

int32_t ANativeWindow_getFormat(ANativeWindow* window)
{
    if (!window) window = &g_window;
    return window->format;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t width, int32_t height, int32_t format)
{
    if (!window) window = &g_window;
    window->width = width;
    window->height = height;
    window->format = format;

    /* Forward to the host so the real ANativeWindow gets the format */
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_SET_BUF, width, height, format, 0, 0, 0);
    return 0;
}

/* Guest-side pixel buffer for window rendering.
 * The host cannot hand out its own surface pointer (guest addr space is
 * identity-mapped, but the surface buffer belongs to the host-side graphics
 * allocator), so the guest renders into its own buffer and the host copies
 * it into the real surface on unlock. */
static void* g_pixbuf = NULL;
static size_t g_pixbuf_size = 0;

static void* pixbuf_ensure(size_t need)
{
    if (!g_pixbuf || g_pixbuf_size < need) {
        free(g_pixbuf);
        g_pixbuf = malloc(need);
        g_pixbuf_size = g_pixbuf ? need : 0;
    }
    return g_pixbuf;
}

/* NDK API: Lock the window's drawing surface for writing */
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds)
{
    if (!window) window = &g_window;
    if (!outBuffer) return -1;

    /* Tell host to lock the window and fill in buffer geometry (not bits) */
    long result = virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_LOCK, 
                                  (long)window,
                                  (long)outBuffer,
                                  (long)inOutDirtyBounds,
                                  0, 0, 0);
    if (result == 0) {
        /* Provide the guest-owned pixel buffer */
        size_t bpp = (outBuffer->format == WINDOW_FORMAT_RGB_565) ? 2 : 4;
        outBuffer->bits = pixbuf_ensure((size_t)outBuffer->stride * outBuffer->height * bpp);
        if (!outBuffer->bits) {
            virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_UNLOCK, (long)window, 0, 0, 0, 0, 0);
            return -1;
        }
    }
    return (int32_t)result;
}

/* NDK API: Unlock the window's drawing surface and post the new buffer */
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window)
{
    if (!window) window = &g_window;

    /* Host copies pixels out of the guest buffer before posting */
long result = virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_UNLOCK,
                              (long)window,
                              (long)g_pixbuf,
                              0, 0, 0, 0);
    return (int32_t)result;
}

/* ============================================================
 * Input API Stubs (android/input.h)
 * ============================================================ */

typedef struct AInputEvent AInputEvent;
typedef struct AInputQueue AInputQueue;

struct AInputEvent {
    int32_t type;
    int32_t device_id;
    int32_t source;
    int32_t action;
    float x;
    float y;
    int32_t keyCode;
};

struct AInputQueue {
    int32_t id;
};

static AInputQueue g_input_queue = { .id = 0 };

AInputQueue* AInputQueue_create(void* looper, int ident)
{
    (void)looper;
    (void)ident;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_INPUT_INIT, 0, 0, 0, 0, 0, 0);
    return &g_input_queue;
}

void AInputQueue_destroy(AInputQueue* queue)
{
    (void)queue;
}

int AInputQueue_getEvent(AInputQueue* queue, AInputEvent** event)
{
    (void)queue;
    (void)event;
    return -1; /* No events by default */
}

int AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event)
{
    (void)queue;
    (void)event;
    return 0;
}

void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled)
{
    (void)queue;
    (void)event;
    (void)handled;
}

int32_t AInputEvent_getType(AInputEvent* event)
{
    if (!event) return 0;
    return event->type;
}

int32_t AKeyEvent_getKeyCode(AInputEvent* event)
{
    if (!event) return 0;
    return event->keyCode;
}

float AMotionEvent_getX(AInputEvent* event, int32_t pointerIndex)
{
    (void)pointerIndex;
    if (!event) return 0.0f;
    return event->x;
}

float AMotionEvent_getY(AInputEvent* event, int32_t pointerIndex)
{
    (void)pointerIndex;
    if (!event) return 0.0f;
    return event->y;
}

int32_t AMotionEvent_getAction(AInputEvent* event)
{
    if (!event) return 0;
    return event->action;
}

/* ============================================================
 * Looper API Stubs (android/looper.h)
 * ============================================================ */

typedef struct ALooper ALooper;

struct ALooper {
    int32_t id;
};

static ALooper g_looper = { .id = 0 };

ALooper* ALooper_prepare(int opts)
{
    (void)opts;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_LOOPER_INIT, 0, 0, 0, 0, 0, 0);
    return &g_looper;
}

int ALooper_pollAll(int timeoutMillis, int* events, void** data, void** source)
{
    (void)timeoutMillis;
    (void)events;
    (void)data;
    (void)source;
    return -1; /* ALOOPER_POLL_TIMEOUT */
}

int ALooper_pollOnce(int timeoutMillis, int* events, void** data, void** source)
{
    (void)timeoutMillis;
    (void)events;
    (void)data;
    (void)source;
    return -1; /* ALOOPER_POLL_TIMEOUT */
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events, void* callback, void* data)
{
    (void)looper;
    (void)fd;
    (void)ident;
    (void)events;
    (void)callback;
    (void)data;
    return 0;
}

int ALooper_removeFd(ALooper* looper, int fd)
{
    (void)looper;
    (void)fd;
    return 0;
}

/* ============================================================
 * GameActivity Stubs (game-activity/GameActivity.h)
 * ============================================================ */

/* android_app structure (defined in header, implementation here) */
static android_app g_app = {0};

/* GameActivity API: Create android_app */
static GameActivityMotionEvent g_motion_event_buf[GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT + 8];
static GameActivityKeyEvent g_key_event_buf[16];

android_app* android_app_create(void)
{
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_CREATE, 0, 0, 0, 0, 0, 0);

    /* Point the input buffer at guest-visible arrays so the host side
     * (which shares the address space in rvvm-user mode) can fill them. */
    g_app.inputBuffer.motionEvents = g_motion_event_buf;
    g_app.inputBuffer.motionEventsCount = 0;
    g_app.inputBuffer.motionEventsCapacity = sizeof(g_motion_event_buf) / sizeof(g_motion_event_buf[0]);
    g_app.inputBuffer.keyEvents = g_key_event_buf;
    g_app.inputBuffer.keyEventsCount = 0;
    g_app.inputBuffer.keyEventsCapacity = sizeof(g_key_event_buf) / sizeof(g_key_event_buf[0]);
    return &g_app;
}

/* GameActivity API: Destroy android_app */
void android_app_destroy(android_app* app)
{
    (void)app;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_DESTROY, 0, 0, 0, 0, 0, 0);
}

/* GameActivity API: Read lifecycle command */
int32_t android_app_read_cmd(android_app* app)
{
    (void)app;
    return (int32_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_POLL_CMD, 0, 0, 0, 0, 0, 0);
}

/* GameActivity API: Execute lifecycle command */
void android_app_exec_cmd(android_app* app, int32_t cmd)
{
    if (app && app->onAppCmd) {
        app->onAppCmd(app, cmd);
    }
}

/* GameActivity API: Swap input buffers */
int32_t android_app_swap_input_buffers(android_app* app)
{
    if (app) {
        /* Pass the guest input buffer address so the host can fill events into it.
         * In RVVM user-mode the guest address space aliases the host, so the
         * raw pointer value is directly usable by the host side. */
        return (int32_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_SWAP_INPUT, 
                                        (uintptr_t)&app->inputBuffer, 0, 0, 0, 0, 0);
    }
    return 0;
}

/* GameActivity API: Clear motion events */
void android_app_clear_motion_events(android_app* app)
{
    (void)app;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_CLEAR_INPUT, 0, 0, 0, 0, 0, 0);
}

/* GameActivity API: Clear key events */
void android_app_clear_key_events(android_app* app)
{
    (void)app;
    virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_CLEAR_INPUT, 1, 0, 0, 0, 0, 0);
}

/* GameActivity API: Get pointer axis value */
float GameActivityPointerAxes_getAxisValue(const GameActivityPointerAxes* pointer, int32_t axis)
{
    if (!pointer) return 0.0f;
    
    switch (axis) {
        case 0: return pointer->x;      /* AMOTION_EVENT_AXIS_X */
        case 1: return pointer->y;      /* AMOTION_EVENT_AXIS_Y */
        case 2: return pointer->pressure; /* AMOTION_EVENT_AXIS_PRESSURE */
        case 3: return pointer->size;    /* AMOTION_EVENT_AXIS_SIZE */
        case 4: return pointer->touchMajor; /* AMOTION_EVENT_AXIS_TOUCH_MAJOR */
        case 5: return pointer->touchMinor; /* AMOTION_EVENT_AXIS_TOUCH_MINOR */
        case 6: return pointer->toolMajor;  /* AMOTION_EVENT_AXIS_TOOL_MAJOR */
        case 7: return pointer->toolMinor;  /* AMOTION_EVENT_AXIS_TOOL_MINOR */
        case 8: return pointer->orientation; /* AMOTION_EVENT_AXIS_ORIENTATION */
        default: return 0.0f;
    }
}
