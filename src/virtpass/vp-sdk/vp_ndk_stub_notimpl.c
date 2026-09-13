/*
 * GENERATED FILE - produced by tools/gen_stub_notimpl.py - DO NOT EDIT BY HAND.
 *
 * "Not implemented" build variant of src/virtpass/vp_ndk_stub.c.
 * Regenerate from the repository root:
 *     python tools/gen_stub_notimpl.py src/virtpass/vp_ndk_stub.c --outdir src/virtpass/vp-sdk
 *
 * Every API function below keeps its original signature and reports itself on
 * stderr instead of issuing a hypercall to the host. Link a guest against this
 * file instead of the real stub to see which host APIs it actually asks for.
 */

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
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include "virtpass/vp_android.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

/* ============================================================
 * Generated fallback reporter
 *
 * One line per distinct API is printed the first time it is reached: a guest
 * render loop calls these functions thousands of times per second, so
 * reporting every call would drown the host console in identical lines. The
 * dedup table compares the __func__ literals (stable per function); the race
 * between guest threads at worst repeats a line.
 * ============================================================ */
static void vp_stub_not_implemented(const char* api)
{
    static const char* reported[512];
    static unsigned     count = 0;
    unsigned            i;

    for (i = 0; i < count; i++) {
        if (reported[i] == api) {
            return;
        }
    }
    if (count < sizeof(reported) / sizeof(reported[0])) {
        reported[count++] = api;
    }
    fprintf(stderr, "[virtpass] not implemented: %s()\n", api);
}

/*
 * Custom syscall numbers for Android NDK API proxying come from
 * virtpass/vp_syscall.h (included through virtpass/vp_android.h above), the
 * same header the host dispatch compiles against.
 */

/*
 * stdio buffering for the host console. The guest's stdout goes to the
 * emulator's in-memory fd, which musl treats like a regular file: printf
 * output then sits in the 4 KB stdio buffer and reaches the host in coarse
 * blocks instead of as it is printed. Line-buffering stdout (unbuffered
 * stderr) makes every printf appear on the host console overlay in real
 * time. Runs before main in every guest linked against this stub.
 */
__attribute__((constructor))
static void vp_stub_init_stdio(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

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
    vp_stub_not_implemented(__func__);
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return 0;
}

/* ============================================================
 * Sensor API Stubs (android/sensor.h)
 * ============================================================
 * The device facts (which sensors exist, their names, ranges, FIFO counts and
 * capabilities) belong to the host: the pool below is filled once from
 * VP_SENSOR_LIST, and every ASensor_getXxx() then answers from it locally,
 * exactly like the NDK accessors read their ASensor object.
 *
 * Event delivery mirrors the NDK too. A queue that was created with a Looper
 * owns a pipe whose read end is registered with that Looper; the host writes
 * one byte into the write end whenever the queue goes from empty to
 * non-empty, so the canonical
 *
 *     while (running) ALooper_pollAll(-1, NULL, NULL, NULL);
 *
 * wakes up on sensor data. The stub drains the read end before consulting the
 * queue, which is what bounds the pipe to one byte and keeps the host's
 * write() from ever blocking. Hosts without VP_SENSOR_CAP_FD_WAKEUP leave the
 * guest with ASensorEventQueue_hasEvents() polling, and a host with no sensor
 * backend at all returns an empty list.
 */

/* Sensor type constants (values mirror <android/sensor.h>) */
#define ASENSOR_TYPE_ACCELEROMETER       1
#define ASENSOR_TYPE_MAGNETIC_FIELD      2
#define ASENSOR_TYPE_GYROSCOPE           4
#define ASENSOR_TYPE_LIGHT               5
#define ASENSOR_TYPE_PRESSURE            6
#define ASENSOR_TYPE_PROXIMITY           8

struct ASensorManager {
    int32_t id;
};

/* One cached descriptor. The strings are owned here, so the accessors can
 * hand out stable pointers for the lifetime of the process. */
struct ASensor {
    int32_t handle;
    int32_t type;
    int32_t reporting_mode;
    int32_t min_delay_us;
    int32_t fifo_max_events;
    int32_t fifo_reserved_events;
    float   resolution;
    bool    wake_up;
    char    string_type[VP_SENSOR_STRING_TYPE_MAX];
    char    name[VP_SENSOR_NAME_MAX];
    char    vendor[VP_SENSOR_VENDOR_MAX];
};

struct ASensorEventQueue {
    bool                 used;
    int32_t              id;
    int                  wake_read_fd;  /* -1 when there is no fd wakeup */
    ALooper_callbackFunc cb;            /* only for ALOOPER_POLL_CALLBACK */
    void*                cb_data;
};

static ASensorManager g_sensor_manager = { .id = 0 };
static ASensor g_sensor_pool[VP_SENSOR_MAX_HANDLES];
static ASensor const* g_sensor_refs[VP_SENSOR_MAX_HANDLES];
static int32_t g_sensor_count = 0;
static bool g_sensor_ready = false;
static uint32_t g_sensor_caps = 0;

static ASensorEventQueue g_sensor_queues[VP_SENSOR_MAX_QUEUES];

/* Enumerate the host's sensors once and cache them as ASensor objects. */
static int vp_sensor_pool_load(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

/* NDK API: Get sensor manager instance */
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName)
{
    vp_stub_not_implemented(__func__);
    (void)packageName;
    return NULL;
}

ASensorManager* ASensorManager_getInstance(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

/* NDK API: Get list of available sensors */
int ASensorManager_getSensorList(ASensorManager* manager, ASensorList* list)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)list;
    return 0;
}

/* NDK API: Get default sensor by type. wakeUp selects the wake-up variant,
 * falling back to whatever the host has for that type. */
ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)type;
    (void)wakeUp;
    return NULL;
}

/* NDK API: Get default sensor by type */
ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)type;
    return NULL;
}

/* NDK API: Get dynamic sensor list (Virtpass has none) */
int ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensorList* list)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)list;
    return 0;
}

/* Consume every pending wake byte. Called before any queue consultation, so
 * the host can arm the fd again without ever filling the pipe. */
static void vp_sensor_queue_drain(ASensorEventQueue* queue)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
}

/* Looper callback for a queue created with ident == ALOOPER_POLL_CALLBACK: the
 * fd belongs to the stub, so the stub drains it and then hands control to the
 * callback the guest registered, exactly like the NDK's sensors fd. */
static int vp_sensor_queue_fd_ready(int fd, int events, void* data)
{
    vp_stub_not_implemented(__func__);
    (void)fd;
    (void)events;
    (void)data;
    return 0;
}

static ASensorEventQueue* vp_sensor_queue_alloc(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

static void vp_sensor_queue_free(ASensorEventQueue* queue)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
}

/* NDK API: Create event queue */
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager,
        ALooper* looper, int ident, ALooper_callbackFunc callback, void* data)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    return NULL;
}

/* NDK API: Destroy event queue */
int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue)
{
    vp_stub_not_implemented(__func__);
    (void)manager;
    (void)queue;
    return 0;
}

/* NDK API: Enable sensor */
int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)sensor;
    return 0;
}

/* NDK API: Disable sensor */
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)sensor;
    return 0;
}

/* NDK API: Set event rate (microseconds per event; 0 = host default) */
int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)sensor;
    (void)usec;
    return 0;
}

/* NDK API: Register sensor with custom params (rate + batching), then enable */
int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor,
        int32_t samplingPeriodUs, int64_t maxBatchReportLatencyUs)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)sensor;
    (void)samplingPeriodUs;
    (void)maxBatchReportLatencyUs;
    return 0;
}

/* NDK API: Check for pending events (1 = events, 0 = none, <0 = error) */
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    return 0;
}

/* NDK API: Get sensor events. events is the caller's own array: the host
 * copies out into it, which is the whole data path (no shared memory). */
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)events;
    (void)count;
    return 0;
}

/* NDK API: Request additional info events (not part of the Virtpass subset) */
int ASensorEventQueue_requestAdditionalInfoEvents(ASensorEventQueue* queue, bool enable)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)enable;
    return 0;
}

/* NDK API: Direct channel support. Virtpass has no Direct Channel backend, so
 * the guest never gets told it may use one. */
bool ASensor_isDirectChannelTypeSupported(ASensor const* sensor, int channelType)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    (void)channelType;
    return 0;
}

int ASensor_getHighestDirectReportRateLevel(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

/* NDK API: Sensor info accessors (all local reads of the cached descriptor) */
const char* ASensor_getName(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return NULL;
}

const char* ASensor_getVendor(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return NULL;
}

const char* ASensor_getStringType(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return NULL;
}

int ASensor_getType(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

int ASensor_getHandle(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

float ASensor_getResolution(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0.0f;
}

int ASensor_getMinDelay(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

int ASensor_getFifoMaxEventCount(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

int ASensor_getFifoReservedEventCount(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

int ASensor_getReportingMode(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
    return 0;
}

bool ASensor_isWakeUpSensor(ASensor const* sensor)
{
    vp_stub_not_implemented(__func__);
    (void)sensor;
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
    vp_stub_not_implemented(__func__);
    (void)window;
    return NULL;
}

void ANativeWindow_release(ANativeWindow* window)
{
    vp_stub_not_implemented(__func__);
    (void)window;
}

/* Query the host for the real window size and cache it in g_window */
static void guest_window_query_size(void)
{
    vp_stub_not_implemented(__func__);
}

int32_t ANativeWindow_getWidth(ANativeWindow* window)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    return 0;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    return 0;
}

int32_t ANativeWindow_getFormat(ANativeWindow* window)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    return 0;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t width, int32_t height, int32_t format)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    (void)width;
    (void)height;
    (void)format;
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
    vp_stub_not_implemented(__func__);
    (void)need;
    return NULL;
}

/* NDK API: Lock the window's drawing surface for writing */
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    (void)outBuffer;
    (void)inOutDirtyBounds;
    return 0;
}

/* NDK API: Unlock the window's drawing surface and post the new buffer */
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window)
{
    vp_stub_not_implemented(__func__);
    (void)window;
    return 0;
}

/* ============================================================
 * Configuration API Stubs (android/configuration.h)
 *
 * The device configuration is owned by the host, so every getter
 * proxies to it through SYS_ANDROID_CONFIG. The handle is an opaque
 * token; no configuration state is cached on the guest side.
 * ============================================================ */

struct AConfiguration {
    int32_t reserved;
};

static int32_t guest_config_query(int32_t field)
{
    vp_stub_not_implemented(__func__);
    (void)field;
    return 0;
}

AConfiguration* AConfiguration_new(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

void AConfiguration_delete(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
}

void AConfiguration_copy(AConfiguration* dest, AConfiguration* src)
{
    vp_stub_not_implemented(__func__);
    (void)dest;
    (void)src;
}

int32_t AConfiguration_getScreenSize(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getScreenWidthDp(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getScreenHeightDp(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getDensity(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getScreenLong(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getScreenRound(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
}

int32_t AConfiguration_getOrientation(AConfiguration* config)
{
    vp_stub_not_implemented(__func__);
    (void)config;
    return 0;
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
    vp_stub_not_implemented(__func__);
    (void)looper;
    (void)ident;
    return NULL;
}

void AInputQueue_destroy(AInputQueue* queue)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
}

int AInputQueue_getEvent(AInputQueue* queue, AInputEvent** event)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)event;
    return 0;
}

int AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)event;
    return 0;
}

void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled)
{
    vp_stub_not_implemented(__func__);
    (void)queue;
    (void)event;
    (void)handled;
}

int32_t AInputEvent_getType(AInputEvent* event)
{
    vp_stub_not_implemented(__func__);
    (void)event;
    return 0;
}

int32_t AKeyEvent_getKeyCode(AInputEvent* event)
{
    vp_stub_not_implemented(__func__);
    (void)event;
    return 0;
}

float AMotionEvent_getX(AInputEvent* event, int32_t pointerIndex)
{
    vp_stub_not_implemented(__func__);
    (void)event;
    (void)pointerIndex;
    return 0.0f;
}

float AMotionEvent_getY(AInputEvent* event, int32_t pointerIndex)
{
    vp_stub_not_implemented(__func__);
    (void)event;
    (void)pointerIndex;
    return 0.0f;
}

int32_t AMotionEvent_getAction(AInputEvent* event)
{
    vp_stub_not_implemented(__func__);
    (void)event;
    return 0;
}

/* ============================================================
 * Looper fd registry (android/looper.h)
 * ============================================================
 * Mirrors the NDK model instead of faking a pump: the Looper owns a set of
 * fds, ALooper_pollOnce()/ALooper_pollAll() block in a real poll() until one
 * of them is ready, and each ready fd is dispatched to its own callback.
 *
 * The vsync source is just another registered fd (see the Choreographer
 * section below), so guest code can drive frames, input, assets and anything
 * else from the single Looper loop exactly like with the real NDK.
 */

typedef struct ALooper ALooper;

#define VP_LOOPER_MAX_FDS 16

typedef struct {
    int   fd;
    int   ident;
    int   events;
    ALooper_callbackFunc cb;   /* only meaningful for ALOOPER_POLL_CALLBACK */
    void* data;
} vp_looper_fd_t;

static vp_looper_fd_t g_looper_fds[VP_LOOPER_MAX_FDS];
static int g_looper_fd_count = 0;

static vp_looper_fd_t* vp_looper_find(int fd)
{
    vp_stub_not_implemented(__func__);
    (void)fd;
    return NULL;
}

static int vp_looper_add(int fd, int ident, int events,
                         ALooper_callbackFunc cb, void* data)
{
    vp_stub_not_implemented(__func__);
    (void)fd;
    (void)ident;
    (void)events;
    (void)cb;
    (void)data;
    return 0;
}

static int vp_looper_del(int fd)
{
    vp_stub_not_implemented(__func__);
    (void)fd;
    return 0;
}

/* ============================================================
 * Choreographer API Stubs (android/choreographer.h)
 * ============================================================
 * postFrameCallback() only queues the callback (NDK semantics: it never
 * blocks) and asks the host for exactly one more vsync, mirroring the real
 * requestNextVsync(). The host answers by writing the frame time into a pipe
 * we own, which is registered with the Looper above, so the queued callbacks
 * are drained from whatever poll() that frame wakes up. Guest source stays
 * indistinguishable from real NDK usage:
 *
 *   static void on_frame(long t, void* d) {
 *       render();
 *       AChoreographer_postFrameCallback(AChoreographer_getInstance(), on_frame, d);
 *   }
 *   ... while (running) ALooper_pollAll(-1, NULL, NULL, NULL);
 *
 * Hosts without fd wakeup support (capability bit missing) degrade to a
 * blocking CHOREOGRAPHER_WAIT per frame, and to a 60Hz guest-clock tick when
 * even that is unavailable.
 */

struct AChoreographer {
    int32_t id;
};

#define VP_CHOREOGRAPHER_MAX_PENDING 8

/* Safety net for a host-owned clock: never block forever on it. If a vsync is
 * outstanding and nothing arrives within this window, the source is re-checked
 * and the stub degrades instead of stalling the guest's render loop. */
#define VP_VSYNC_WAIT_GUARD_MS 250

typedef struct {
    AChoreographer_frameCallback   cb;
    AChoreographer_frameCallback64 cb64;
    void*    data;
    int64_t  due_ns;   /* guest-clock deadline (delayed postings) */
    bool     is64;
} vp_frame_callback_t;

static vp_frame_callback_t g_frame_callbacks[VP_CHOREOGRAPHER_MAX_PENDING];
static int g_frame_callback_count = 0;

static AChoreographer g_choreographer = { .id = 0 };
static bool g_choreographer_ready = false;

/* fd-based vsync delivery: the host writes one frame time per requested vsync
 * into g_vsync_write_fd; the read end is registered with the Looper. */
static int  g_vsync_read_fd = -1;
static int  g_vsync_write_fd = -1;
static bool g_vsync_fd_mode = false;

static void vp_choreographer_dispatch(long frame_time);
static void vp_choreographer_arm(void);
static bool vp_choreographer_fd_setup(void);
static int vp_choreographer_fd_ready(int fd, int events, void* data);

static int64_t vp_guest_now_ns(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

/* Re-queue an entry as-is (used to carry not-yet-due delayed callbacks). */
static void vp_choreographer_enqueue_entry(const vp_frame_callback_t* e)
{
    vp_stub_not_implemented(__func__);
    (void)e;
}

static void vp_choreographer_enqueue(AChoreographer_frameCallback cb,
                                     AChoreographer_frameCallback64 cb64,
                                     void* data, uint32_t delay_ms, bool is64)
{
    vp_stub_not_implemented(__func__);
    (void)cb;
    (void)cb64;
    (void)data;
    (void)delay_ms;
    (void)is64;
}

/*
 * Block until the next display vsync and return its frame time in
 * nanoseconds. A negative host result means no vsync source is available;
 * fall back to the guest monotonic clock so callers still make progress
 * instead of stalling the render loop forever.
 */
static long vp_choreographer_wait_vsync(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

/* Capability bits the host reports for CHOREOGRAPHER_INIT. */
static long vp_choreographer_caps(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

/* Give up on the fd wakeup path and go back to the blocking fallback. */
static void vp_choreographer_drop_fd(void)
{
    vp_stub_not_implemented(__func__);
}

/* Ask the host for exactly one more vsync. In fd mode the host answers by
 * writing the frame time into our pipe, which is what wakes poll(). */
static void vp_choreographer_arm(void)
{
    vp_stub_not_implemented(__func__);
}

/* Hand the host the write end of our vsync pipe and register the read end
 * with the Looper. Returns true once the fd wakeup path is live. */
static bool vp_choreographer_fd_setup(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

/*
 * Run every queued callback that is due for this frame. Callbacks posted from
 * inside a callback (the usual render loop re-arming itself) land in the next
 * frame, i.e. one iteration of the real vsync loop.
 */
static void vp_choreographer_dispatch(long frame_time)
{
    vp_stub_not_implemented(__func__);
    (void)frame_time;
}

/*
 * The vsync fd became readable: the host wrote the frame time of the vsync we
 * requested. Drain it (newest frame wins) and run the due callbacks.
 */
static int vp_choreographer_fd_ready(int fd, int events, void* data)
{
    vp_stub_not_implemented(__func__);
    (void)fd;
    (void)events;
    (void)data;
    return 0;
}

AChoreographer* AChoreographer_getInstance(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

void AChoreographer_postFrameCallback(AChoreographer* choreographer,
                                      AChoreographer_frameCallback callback,
                                      void* data)
{
    vp_stub_not_implemented(__func__);
    (void)choreographer;
    (void)callback;
    (void)data;
}

void AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer,
                                             AChoreographer_frameCallback callback,
                                             void* data, long delayMillis)
{
    vp_stub_not_implemented(__func__);
    (void)choreographer;
    (void)callback;
    (void)data;
    (void)delayMillis;
}

void AChoreographer_postFrameCallback64(AChoreographer* choreographer,
                                        AChoreographer_frameCallback64 callback,
                                        void* data)
{
    vp_stub_not_implemented(__func__);
    (void)choreographer;
    (void)callback;
    (void)data;
}

void AChoreographer_postFrameCallbackDelayed64(AChoreographer* choreographer,
                                               AChoreographer_frameCallback64 callback,
                                               void* data, uint32_t delayMillis)
{
    vp_stub_not_implemented(__func__);
    (void)choreographer;
    (void)callback;
    (void)data;
    (void)delayMillis;
}

struct ALooper {
    int32_t id;
};

static ALooper g_looper = { .id = 0 };

ALooper* ALooper_prepare(int opts)
{
    vp_stub_not_implemented(__func__);
    (void)opts;
    return NULL;
}

/*
 * The real pump: block in poll() over every registered fd (the choreographer
 * vsync pipe is one of them) until something is ready, then dispatch each
 * ready fd. Callbacks that were registered with ident ALOOPER_POLL_CALLBACK
 * run here; an explicit ident is reported back to the caller instead, as the
 * NDK contract specifies.
 */
int ALooper_pollOnce(int timeoutMillis, int* events, void** data, void** source)
{
    vp_stub_not_implemented(__func__);
    (void)timeoutMillis;
    (void)events;
    (void)data;
    (void)source;
    return 0;
}

/* Keep draining callbacks until a poll returns something other than them. */
int ALooper_pollAll(int timeoutMillis, int* events, void** data, void** source)
{
    vp_stub_not_implemented(__func__);
    (void)timeoutMillis;
    (void)events;
    (void)data;
    (void)source;
    return 0;
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                  ALooper_callbackFunc callback, void* data)
{
    vp_stub_not_implemented(__func__);
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
    vp_stub_not_implemented(__func__);
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
    vp_stub_not_implemented(__func__);
    return NULL;
}

/* GameActivity API: Destroy android_app */
void android_app_destroy(android_app* app)
{
    vp_stub_not_implemented(__func__);
    (void)app;
}

/* GameActivity API: Read lifecycle command */
int32_t android_app_read_cmd(android_app* app)
{
    vp_stub_not_implemented(__func__);
    (void)app;
    return 0;
}

/*
 * Bring the aggregate android_app state up to date for a lifecycle command,
 * mirroring android_native_app_glue.c's android_app_pre_exec_cmd(): the user
 * callback always observes app->window / app->activityState as the real glue
 * would leave them, so guest code can derive "may I present content?" from the
 * struct instead of re-implementing the state machine itself.
 *
 * activityState is a bitmask built from the transitions currently in effect:
 *   APP_CMD_START        - Activity is started (foreground-ish, surface may exist)
 *   APP_CMD_RESUME       - Activity is resumed (visible, interactive)
 *   APP_CMD_GAINED_FOCUS - window currently holds input focus
 * Each bit is set by its positive transition and cleared by the matching
 * negative one, so the usual test
 *
 *     bool live = app->window != NULL && (app->activityState & APP_CMD_RESUME);
 *
 * stays correct regardless of the order the host delivers the commands in
 * (surface before/after RESUME, TERM_WINDOW after STOP, commands drained in a
 * burst after the guest was not polling, ...).
 *
 * Note: for APP_CMD_INIT_WINDOW the window is installed *before* the callback
 * (the callback must be able to use it) and for APP_CMD_TERM_WINDOW it is
 * removed *before* the callback (the surface is already gone and must not be
 * touched), same as the NDK glue.
 */
static void vp_app_pre_exec_cmd(android_app* app, int32_t cmd)
{
    vp_stub_not_implemented(__func__);
    (void)app;
    (void)cmd;
}

/* GameActivity API: Execute lifecycle command */
void android_app_exec_cmd(android_app* app, int32_t cmd)
{
    vp_stub_not_implemented(__func__);
    (void)app;
    (void)cmd;
}

/* GameActivity API: Swap input buffers */
int32_t android_app_swap_input_buffers(android_app* app)
{
    vp_stub_not_implemented(__func__);
    (void)app;
    return 0;
}

/* GameActivity API: Clear motion events */
void android_app_clear_motion_events(android_app* app)
{
    vp_stub_not_implemented(__func__);
    (void)app;
}

/* GameActivity API: Clear key events */
void android_app_clear_key_events(android_app* app)
{
    vp_stub_not_implemented(__func__);
    (void)app;
}

/* GameActivity API: Get pointer axis value */
float GameActivityPointerAxes_getAxisValue(const GameActivityPointerAxes* pointer, int32_t axis)
{
    vp_stub_not_implemented(__func__);
    (void)pointer;
    (void)axis;
    return 0.0f;
}
