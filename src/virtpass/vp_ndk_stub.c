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
    vp_sensor_info_t raw[VP_SENSOR_MAX_HANDLES];

    if (g_sensor_count > 0 || !(g_sensor_caps & VP_SENSOR_CAP_LIST)) {
        return g_sensor_count;
    }

    int32_t n = (int32_t)virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_LIST,
                                         (long)raw, VP_SENSOR_MAX_HANDLES,
                                         0, 0, 0, 0);
    if (n < 0) n = 0;
    if (n > VP_SENSOR_MAX_HANDLES) n = VP_SENSOR_MAX_HANDLES;

    for (int32_t i = 0; i < n; i++) {
        ASensor* dst = &g_sensor_pool[i];
        memset(dst, 0, sizeof(*dst));
        dst->handle = raw[i].handle;
        dst->type = raw[i].type;
        dst->reporting_mode = raw[i].reporting_mode;
        dst->min_delay_us = raw[i].min_delay_us;
        dst->fifo_max_events = raw[i].fifo_max_events;
        dst->fifo_reserved_events = raw[i].fifo_reserved_events;
        dst->resolution = raw[i].resolution;
        dst->wake_up = raw[i].wake_up != 0;
        memcpy(dst->string_type, raw[i].string_type, sizeof(dst->string_type) - 1);
        memcpy(dst->name, raw[i].name, sizeof(dst->name) - 1);
        memcpy(dst->vendor, raw[i].vendor, sizeof(dst->vendor) - 1);
        g_sensor_refs[i] = dst;
    }
    g_sensor_count = n;
    return n;
}

/* NDK API: Get sensor manager instance */
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName)
{
    (void)packageName;  /* the host owns the per-package instance identity */

    if (!g_sensor_ready) {
        g_sensor_ready = true;
        long caps = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_MANAGER_INIT,
                                     0, 0, 0, 0, 0, 0);
        g_sensor_caps = caps > 0 ? (uint32_t)caps : 0;
    }
    return &g_sensor_manager;
}

ASensorManager* ASensorManager_getInstance(void)
{
    return ASensorManager_getInstanceForPackage(NULL);
}

/* NDK API: Get list of available sensors */
int ASensorManager_getSensorList(ASensorManager* manager, ASensorList* list)
{
    (void)manager;

    ASensorManager_getInstanceForPackage(NULL);
    int count = vp_sensor_pool_load();
    if (list) {
        /* Owned by us, never freed: the NDK contract says the caller must not
         * modify or free it. */
        *list = count > 0 ? g_sensor_refs : (ASensorList)0;
    }
    return count;
}

/* NDK API: Get default sensor by type. wakeUp selects the wake-up variant,
 * falling back to whatever the host has for that type. */
ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp)
{
    (void)manager;

    ASensorManager_getInstanceForPackage(NULL);
    int count = vp_sensor_pool_load();

    for (int i = 0; i < count; i++) {
        if (g_sensor_pool[i].type == type && g_sensor_pool[i].wake_up == wakeUp) {
            return &g_sensor_pool[i];
        }
    }
    for (int i = 0; i < count; i++) {
        if (g_sensor_pool[i].type == type) {
            return &g_sensor_pool[i];
        }
    }
    return (ASensor const*)0;
}

/* NDK API: Get default sensor by type */
ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type)
{
    return ASensorManager_getDefaultSensorEx(manager, type, false);
}

/* NDK API: Get dynamic sensor list (Virtpass has none) */
int ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensorList* list)
{
    (void)manager;
    if (list) {
        *list = (ASensorList)0;
    }
    return 0;
}

/* Consume every pending wake byte. Called before any queue consultation, so
 * the host can arm the fd again without ever filling the pipe. */
static void vp_sensor_queue_drain(ASensorEventQueue* queue)
{
    if (queue->wake_read_fd < 0) {
        return;
    }
    uint8_t buf[16];
    for (;;) {
        ssize_t got = read(queue->wake_read_fd, buf, sizeof(buf));
        if (got <= 0) {
            break;
        }
        if ((size_t)got < sizeof(buf)) {
            break;  /* most likely drained already */
        }
    }
}

/* Looper callback for a queue created with ident == ALOOPER_POLL_CALLBACK: the
 * fd belongs to the stub, so the stub drains it and then hands control to the
 * callback the guest registered, exactly like the NDK's sensors fd. */
static int vp_sensor_queue_fd_ready(int fd, int events, void* data)
{
    ASensorEventQueue* queue = data;
    vp_sensor_queue_drain(queue);
    if (queue->cb) {
        queue->cb(fd, events, queue->cb_data);
    }
    return 1;  /* keep the fd registered until destroyEventQueue() */
}

static ASensorEventQueue* vp_sensor_queue_alloc(void)
{
    for (size_t i = 0; i < (sizeof(g_sensor_queues) / sizeof(g_sensor_queues[0])); i++) {
        if (!g_sensor_queues[i].used) {
            memset(&g_sensor_queues[i], 0, sizeof(g_sensor_queues[i]));
            g_sensor_queues[i].used = true;
            g_sensor_queues[i].wake_read_fd = -1;
            return &g_sensor_queues[i];
        }
    }
    return (ASensorEventQueue*)0;
}

static void vp_sensor_queue_free(ASensorEventQueue* queue)
{
    memset(queue, 0, sizeof(*queue));
    queue->wake_read_fd = -1;
}

/* NDK API: Create event queue */
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager,
        ALooper* looper, int ident, ALooper_callbackFunc callback, void* data)
{
    (void)manager;

    ASensorManager_getInstanceForPackage(NULL);

    ASensorEventQueue* queue = vp_sensor_queue_alloc();
    if (!queue) {
        return (ASensorEventQueue*)0;
    }

    /* The host answers with one byte per empty -> non-empty edge, so the write
     * end is what travels; the read end stays registered with our Looper. */
    int wake_write_fd = -1;
    if (looper && (g_sensor_caps & VP_SENSOR_CAP_FD_WAKEUP)) {
        int fds[2];
        if (pipe(fds) == 0) {
            int fl = fcntl(fds[0], F_GETFL, 0);
            if (fl >= 0) {
                /* Draining must never stall the Looper pump. */
                fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
            }
            queue->wake_read_fd = fds[0];
            wake_write_fd = fds[1];
        } else {
            fprintf(stderr, "vp_ndk_stub: sensor pipe() failed: %s\n", strerror(errno));
        }
    }

    int32_t id = (int32_t)virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_CREATE,
                                          wake_write_fd, 0, 0, 0, 0, 0);
    if (id < 0) {
        if (wake_write_fd >= 0) {
            close(wake_write_fd);
            close(queue->wake_read_fd);
        }
        vp_sensor_queue_free(queue);
        return (ASensorEventQueue*)0;
    }
    queue->id = id;

    if (wake_write_fd >= 0 && looper) {
        int added;
        if (ident == ALOOPER_POLL_CALLBACK) {
            queue->cb = callback;
            queue->cb_data = data;
            added = ALooper_addFd(looper, queue->wake_read_fd, ALOOPER_POLL_CALLBACK,
                                  ALOOPER_EVENT_INPUT, vp_sensor_queue_fd_ready, queue);
        } else {
            added = ALooper_addFd(looper, queue->wake_read_fd, ident,
                                  ALOOPER_EVENT_INPUT, callback, data);
        }
        if (added != 0) {
            /* Unusable as a wake source; the queue still works through
             * hasEvents(). Close the pair so nothing is left dangling. */
            fprintf(stderr, "vp_ndk_stub: ALooper_addFd failed for the sensor wake fd\n");
            close(wake_write_fd);
            close(queue->wake_read_fd);
            queue->wake_read_fd = -1;
        }
    }
    return queue;
}

/* NDK API: Destroy event queue */
int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue)
{
    (void)manager;
    if (!queue) {
        return -1;
    }
    if (queue->wake_read_fd >= 0) {
        ALooper_removeFd((ALooper*)0, queue->wake_read_fd);
        close(queue->wake_read_fd);
        queue->wake_read_fd = -1;
    }
    virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_DESTROY, queue->id, 0, 0, 0, 0, 0);
    vp_sensor_queue_free(queue);
    return 0;
}

/* NDK API: Enable sensor */
int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    if (!queue || !sensor) {
        return -1;
    }
    long rc = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_ENABLE,
                               queue->id, sensor->handle, 0, 0, 0, 0);
    return rc == VP_SENSOR_OK ? 0 : -1;
}

/* NDK API: Disable sensor */
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor)
{
    if (!queue || !sensor) {
        return -1;
    }
    long rc = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_DISABLE,
                               queue->id, sensor->handle, 0, 0, 0, 0);
    return rc == VP_SENSOR_OK ? 0 : -1;
}

/* NDK API: Set event rate (microseconds per event; 0 = host default) */
int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec)
{
    if (!queue || !sensor) {
        return -1;
    }
    long rc = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_SET_RATE,
                               queue->id, sensor->handle, usec, 0, 0, 0);
    return rc == VP_SENSOR_OK ? 0 : -1;
}

/* NDK API: Register sensor with custom params (rate + batching), then enable */
int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor,
        int32_t samplingPeriodUs, int64_t maxBatchReportLatencyUs)
{
    if (!queue || !sensor) {
        return -1;
    }
    long rc = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_SET_RATE,
                               queue->id, sensor->handle,
                               samplingPeriodUs, maxBatchReportLatencyUs, 0, 0);
    if (rc != VP_SENSOR_OK) {
        return -1;
    }
    return ASensorEventQueue_enableSensor(queue, sensor);
}

/* NDK API: Check for pending events (1 = events, 0 = none, <0 = error) */
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue)
{
    if (!queue) {
        return -1;
    }
    vp_sensor_queue_drain(queue);
    long pending = virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_HAS,
                                    queue->id, 0, 0, 0, 0, 0);
    if (pending < 0) {
        return -1;
    }
    return pending > 0 ? 1 : 0;
}

/* NDK API: Get sensor events. events is the caller's own array: the host
 * copies out into it, which is the whole data path (no shared memory). */
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count)
{
    if (!queue) {
        return -1;
    }
    if (!events || count == 0) {
        return 0;
    }
    vp_sensor_queue_drain(queue);
    ssize_t got = (ssize_t)virtpass_syscall(SYS_ANDROID_CALL, VP_SENSOR_QUEUE_READ,
                                            queue->id, (long)events, (long)count,
                                            0, 0, 0);
    return got < 0 ? -1 : got;
}

/* NDK API: Request additional info events (not part of the Virtpass subset) */
int ASensorEventQueue_requestAdditionalInfoEvents(ASensorEventQueue* queue, bool enable)
{
    (void)queue;
    (void)enable;
    return 0;
}

/* NDK API: Direct channel support. Virtpass has no Direct Channel backend, so
 * the guest never gets told it may use one. */
bool ASensor_isDirectChannelTypeSupported(ASensor const* sensor, int channelType)
{
    (void)sensor;
    (void)channelType;
    return false;
}

int ASensor_getHighestDirectReportRateLevel(ASensor const* sensor)
{
    (void)sensor;
    return ASENSOR_DIRECT_RATE_STOP;
}

/* NDK API: Sensor info accessors (all local reads of the cached descriptor) */
const char* ASensor_getName(ASensor const* sensor)
{
    return sensor ? sensor->name : "";
}

const char* ASensor_getVendor(ASensor const* sensor)
{
    return sensor ? sensor->vendor : "";
}

const char* ASensor_getStringType(ASensor const* sensor)
{
    return sensor ? sensor->string_type : "";
}

int ASensor_getType(ASensor const* sensor)
{
    return sensor ? sensor->type : -1;
}

int ASensor_getHandle(ASensor const* sensor)
{
    return sensor ? sensor->handle : -1;
}

float ASensor_getResolution(ASensor const* sensor)
{
    return sensor ? sensor->resolution : 0.0f;
}

int ASensor_getMinDelay(ASensor const* sensor)
{
    return sensor ? sensor->min_delay_us : 0;
}

int ASensor_getFifoMaxEventCount(ASensor const* sensor)
{
    return sensor ? sensor->fifo_max_events : 0;
}

int ASensor_getFifoReservedEventCount(ASensor const* sensor)
{
    return sensor ? sensor->fifo_reserved_events : 0;
}

int ASensor_getReportingMode(ASensor const* sensor)
{
    return sensor ? sensor->reporting_mode : AREPORTING_MODE_INVALID;
}

bool ASensor_isWakeUpSensor(ASensor const* sensor)
{
    return sensor ? sensor->wake_up : false;
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
        g_pixbuf = NULL;
        g_pixbuf_size = 0;

        g_pixbuf = malloc(need);
        if (!g_pixbuf) {
            /* This is the real failure point when the guest cannot render:
             * the anonymous mapping behind malloc() was refused. Log it here
             * so callers do not have to guess from a generic -1. */
            fprintf(stderr,
                    "vp_ndk_stub: pixbuf_ensure: malloc(%zu) failed, errno=%d (%s)\n",
                    need, errno, strerror(errno));
            return NULL;
        }
        g_pixbuf_size = need;
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
    if (result != 0) {
        /* The host refused the lock: the surface itself is not ready. */
        fprintf(stderr, "vp_ndk_stub: ANativeWindow_lock: host LOCK failed -> %ld\n", result);
        return (int32_t)result;
    }

    /* Host locked successfully: the window IS ready. Fill in the guest-owned
     * pixel buffer. Any failure from here on is a guest-side allocation
     * problem, not a "window not ready" condition. */
    size_t bpp  = (outBuffer->format == WINDOW_FORMAT_RGB_565) ? 2 : 4;
    size_t need = (size_t)outBuffer->stride * outBuffer->height * bpp;
    outBuffer->bits = pixbuf_ensure(need);
    if (!outBuffer->bits) {
        fprintf(stderr,
                "vp_ndk_stub: ANativeWindow_lock: guest pixel buffer unavailable "
                "(need=%zu bytes = %dx%d stride=%d bpp=%zu); window is ready, "
                "host surface stays untouched\n",
                need, outBuffer->width, outBuffer->height,
                outBuffer->stride, bpp);
        virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_WINDOW_UNLOCK, (long)window, 0, 0, 0, 0, 0);
        return -1;
    }
    return 0;
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
    return (int32_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_CONFIG,
                                     field, 0, 0, 0, 0, 0);
}

AConfiguration* AConfiguration_new(void)
{
    AConfiguration* config = (AConfiguration*)malloc(sizeof(AConfiguration));
    if (config) config->reserved = 0;
    return config;
}

void AConfiguration_delete(AConfiguration* config)
{
    if (config) free(config);
}

void AConfiguration_copy(AConfiguration* dest, AConfiguration* src)
{
    if (dest && src) *dest = *src;
}

int32_t AConfiguration_getScreenSize(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_SCREEN_SIZE);
}

int32_t AConfiguration_getScreenWidthDp(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_SCREEN_WIDTH_DP);
}

int32_t AConfiguration_getScreenHeightDp(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP);
}

int32_t AConfiguration_getDensity(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_DENSITY);
}

int32_t AConfiguration_getScreenLong(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_SCREEN_LONG);
}

int32_t AConfiguration_getScreenRound(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_SCREEN_ROUND);
}

int32_t AConfiguration_getOrientation(AConfiguration* config)
{
    (void)config;
    return guest_config_query(VP_ACONFIG_QUERY_ORIENTATION);
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
    for (int i = 0; i < g_looper_fd_count; i++) {
        if (g_looper_fds[i].fd == fd) {
            return &g_looper_fds[i];
        }
    }
    return NULL;
}

static int vp_looper_add(int fd, int ident, int events,
                         ALooper_callbackFunc cb, void* data)
{
    if (fd < 0 || g_looper_fd_count >= VP_LOOPER_MAX_FDS || vp_looper_find(fd)) {
        return -1;
    }
    vp_looper_fd_t* e = &g_looper_fds[g_looper_fd_count++];
    e->fd = fd;
    e->ident = ident;
    e->events = events;
    e->cb = cb;
    e->data = data;
    return 0;
}

static int vp_looper_del(int fd)
{
    for (int i = 0; i < g_looper_fd_count; i++) {
        if (g_looper_fds[i].fd != fd) {
            continue;
        }
        for (int j = i + 1; j < g_looper_fd_count; j++) {
            g_looper_fds[j - 1] = g_looper_fds[j];
        }
        g_looper_fd_count--;
        return 0;
    }
    return -1;
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* Re-queue an entry as-is (used to carry not-yet-due delayed callbacks). */
static void vp_choreographer_enqueue_entry(const vp_frame_callback_t* e)
{
    if (g_frame_callback_count >= VP_CHOREOGRAPHER_MAX_PENDING) {
        return;
    }
    g_frame_callbacks[g_frame_callback_count++] = *e;
}

static void vp_choreographer_enqueue(AChoreographer_frameCallback cb,
                                     AChoreographer_frameCallback64 cb64,
                                     void* data, uint32_t delay_ms, bool is64)
{
    if ((!cb && !cb64) || g_frame_callback_count >= VP_CHOREOGRAPHER_MAX_PENDING) {
        fprintf(stderr, "vp_ndk_stub: AChoreographer queue full, callback dropped\n");
        return;
    }
    vp_frame_callback_t* e = &g_frame_callbacks[g_frame_callback_count++];
    e->cb = cb;
    e->cb64 = cb64;
    e->data = data;
    /* due_ns lives on the GUEST monotonic clock, the same domain the dispatcher
     * compares against. 0 means "no delay": the plain postFrameCallback() case
     * must fire on the next frame unconditionally. The host's frame time is a
     * different clock domain (it is only handed to the callback as-is) and must
     * never take part in this comparison. */
    e->due_ns = delay_ms
        ? (vp_guest_now_ns() + (int64_t)delay_ms * 1000000LL)
        : 0;
    e->is64 = is64;

    /* One outstanding request per frame, like the real requestNextVsync(). */
    vp_choreographer_arm();
}

/*
 * Block until the next display vsync and return its frame time in
 * nanoseconds. A negative host result means no vsync source is available;
 * fall back to the guest monotonic clock so callers still make progress
 * instead of stalling the render loop forever.
 */
static long vp_choreographer_wait_vsync(void)
{
    int64_t frame_time = (int64_t)virtpass_syscall(SYS_ANDROID_CALL,
                                                   SYS_ANDROID_CHOREOGRAPHER_WAIT,
                                                   0, 0, 0, 0, 0, 0);
    if (frame_time < 0) {
        /* No host vsync source (backend without a display clock yet): pace at
         * 60Hz and report the guest clock, so callers still see a steady
         * cadence instead of spinning at full speed. */
        const int64_t fallback_period_ns = 16666667;
        struct timespec now, sleep_for;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
        int64_t next_ns = ((now_ns / fallback_period_ns) + 1) * fallback_period_ns;
        sleep_for.tv_sec = (next_ns - now_ns) / 1000000000;
        sleep_for.tv_nsec = (next_ns - now_ns) % 1000000000;
        nanosleep(&sleep_for, NULL);

        clock_gettime(CLOCK_MONOTONIC, &now);
        frame_time = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
    }
    return (long)frame_time;
}

/* Capability bits the host reports for CHOREOGRAPHER_INIT. */
static long vp_choreographer_caps(void)
{
    long caps = virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_CHOREOGRAPHER_INIT,
                                 0, 0, 0, 0, 0, 0);
    return caps < 0 ? 0 : caps;
}

/* Give up on the fd wakeup path and go back to the blocking fallback. */
static void vp_choreographer_drop_fd(void)
{
    if (g_vsync_read_fd >= 0) {
        vp_looper_del(g_vsync_read_fd);
        close(g_vsync_read_fd);
        g_vsync_read_fd = -1;
    }
    if (g_vsync_write_fd >= 0) {
        close(g_vsync_write_fd);
        g_vsync_write_fd = -1;
    }
    g_vsync_fd_mode = false;
}

/* Ask the host for exactly one more vsync. In fd mode the host answers by
 * writing the frame time into our pipe, which is what wakes poll(). */
static void vp_choreographer_arm(void)
{
    if (g_vsync_fd_mode) {
        virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC,
                         0, 0, 0, 0, 0, 0);
        return;
    }
    /* Degraded path: keep probing, so a host that gains a vsync source later
     * (window created after the guest started) upgrades without a restart. */
    vp_choreographer_fd_setup();
}

/* Hand the host the write end of our vsync pipe and register the read end
 * with the Looper. Returns true once the fd wakeup path is live. */
static bool vp_choreographer_fd_setup(void)
{
    if (g_vsync_fd_mode) {
        return true;
    }
    if (!(vp_choreographer_caps() & VP_VSYNC_CAP_FD_WAKEUP)) {
        return false;
    }

    if (g_vsync_read_fd < 0) {
        int fds[2];
        if (pipe(fds) != 0) {
            fprintf(stderr, "vp_ndk_stub: vsync pipe() failed: %s\n", strerror(errno));
            return false;
        }
        /* Draining must never stall the pump. */
        int fl = fcntl(fds[0], F_GETFL, 0);
        if (fl >= 0) {
            fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
        }
        g_vsync_read_fd = fds[0];
        g_vsync_write_fd = fds[1];
    }

    if (virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_CHOREOGRAPHER_SET_FD,
                         (long)g_vsync_write_fd, 0, 0, 0, 0, 0) != 0) {
        return false;   /* host refused: keep the pipe and retry later */
    }

    ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (vp_looper_add(g_vsync_read_fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                      vp_choreographer_fd_ready, NULL) != 0) {
        return false;
    }
    g_vsync_fd_mode = true;
    return true;
}

/*
 * Run every queued callback that is due for this frame. Callbacks posted from
 * inside a callback (the usual render loop re-arming itself) land in the next
 * frame, i.e. one iteration of the real vsync loop.
 */
static void vp_choreographer_dispatch(long frame_time)
{
    int n = g_frame_callback_count;
    if (n == 0) {
        return;
    }

    vp_frame_callback_t batch[VP_CHOREOGRAPHER_MAX_PENDING];
    memcpy(batch, g_frame_callbacks, sizeof(batch[0]) * (size_t)n);
    g_frame_callback_count = 0;   /* callbacks posted below queue up here */

    for (int i = 0; i < n; i++) {
        /* due_ns == 0: not a delayed posting, run on this frame. Otherwise
         * compare against the guest clock - frame_time belongs to the host's
         * clock domain and mixing the two would keep callbacks "not due"
         * forever (stalling the whole render loop). */
        if (batch[i].due_ns != 0 && batch[i].due_ns > vp_guest_now_ns()) {
            /* postFrameCallbackDelayed() not due yet: keep it for a later
             * frame and make sure we get one. */
            vp_choreographer_enqueue_entry(&batch[i]);
            continue;
        }
        if (batch[i].is64) {
            if (batch[i].cb64) {
                batch[i].cb64((int64_t)frame_time, batch[i].data);
            }
        } else if (batch[i].cb) {
            batch[i].cb(frame_time, batch[i].data);
        }
    }

    if (g_frame_callback_count > 0) {
        vp_choreographer_arm();
    }
}

/*
 * The vsync fd became readable: the host wrote the frame time of the vsync we
 * requested. Drain it (newest frame wins) and run the due callbacks.
 */
static int vp_choreographer_fd_ready(int fd, int events, void* data)
{
    (void)events;
    (void)data;

    int64_t frame_time = 0;
    bool got = false;

    for (;;) {
        uint8_t buf[sizeof(int64_t)];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n != (ssize_t)sizeof(buf)) {
            break;   /* drained (or a partial write: drop it) */
        }
        memcpy(&frame_time, buf, sizeof(frame_time));
        got = true;

        /* Only keep reading while more data is queued: where O_NONBLOCK is not
         * honoured, a blind read would block the pump. */
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) {
            break;
        }
    }
    if (!got) {
        return 1;
    }

    if (frame_time < 0) {
        /* Host lost its vsync clock (activity gone): degrade instead of
         * leaving the guest blocked on a fd that will never be written. */
        vp_choreographer_drop_fd();
        frame_time = vp_choreographer_wait_vsync();
    }

    vp_choreographer_dispatch((long)frame_time);
    return 1;   /* keep the fd registered */
}

AChoreographer* AChoreographer_getInstance(void)
{
    if (!g_choreographer_ready) {
        g_choreographer_ready = true;
        /* Bring up the host-side vsync source: fd wakeup when the host
         * supports it, otherwise the stub keeps the blocking fallback. */
        vp_choreographer_fd_setup();
    }
    return &g_choreographer;
}

void AChoreographer_postFrameCallback(AChoreographer* choreographer,
                                      AChoreographer_frameCallback callback,
                                      void* data)
{
    (void)choreographer;
    vp_choreographer_enqueue(callback, NULL, data, 0, false);
}

void AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer,
                                             AChoreographer_frameCallback callback,
                                             void* data, long delayMillis)
{
    (void)choreographer;
    vp_choreographer_enqueue(callback, NULL, data,
                             delayMillis > 0 ? (uint32_t)delayMillis : 0, false);
}

void AChoreographer_postFrameCallback64(AChoreographer* choreographer,
                                        AChoreographer_frameCallback64 callback,
                                        void* data)
{
    (void)choreographer;
    vp_choreographer_enqueue(NULL, callback, data, 0, true);
}

void AChoreographer_postFrameCallbackDelayed64(AChoreographer* choreographer,
                                               AChoreographer_frameCallback64 callback,
                                               void* data, uint32_t delayMillis)
{
    (void)choreographer;
    vp_choreographer_enqueue(NULL, callback, data, delayMillis, true);
}

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

/*
 * The real pump: block in poll() over every registered fd (the choreographer
 * vsync pipe is one of them) until something is ready, then dispatch each
 * ready fd. Callbacks that were registered with ident ALOOPER_POLL_CALLBACK
 * run here; an explicit ident is reported back to the caller instead, as the
 * NDK contract specifies.
 */
int ALooper_pollOnce(int timeoutMillis, int* events, void** data, void** source)
{
    if (events) *events = 0;
    if (data) *data = NULL;
    if (source) *source = NULL;

    if (g_looper_fd_count == 0) {
        /* Nothing registered: there is no fd to block on. While frame
         * callbacks are pending, fall back to the blocking vsync call so the
         * render loop still advances on hosts without fd wakeup support. */
        if (g_frame_callback_count > 0) {
            vp_choreographer_dispatch(vp_choreographer_wait_vsync());
            return ALOOPER_POLL_CALLBACK;
        }
        /* Block a frame's worth so an idle caller cannot spin hot. */
        poll(NULL, 0, timeoutMillis >= 0 ? timeoutMillis : 16);
        return ALOOPER_POLL_TIMEOUT;
    }

    /* Snapshot: callbacks may add or remove fds while we dispatch. */
    int n = g_looper_fd_count;
    struct pollfd pfd[VP_LOOPER_MAX_FDS];
    int snap_fd[VP_LOOPER_MAX_FDS];
    for (int i = 0; i < n; i++) {
        pfd[i].fd = g_looper_fds[i].fd;
        pfd[i].events = (short)g_looper_fds[i].events;
        pfd[i].revents = 0;
        snap_fd[i] = g_looper_fds[i].fd;
    }

    for (;;) {
        /* A host-owned clock must not be able to hang the guest: while a vsync
         * is outstanding, wait with a bounded timeout and re-check the source. */
        bool guarded = g_vsync_fd_mode && timeoutMillis < 0 && g_frame_callback_count > 0;
        int wait_ms = guarded ? VP_VSYNC_WAIT_GUARD_MS : timeoutMillis;

        int rc = poll(pfd, (nfds_t)n, wait_ms);
        if (rc < 0) {
            return ALOOPER_POLL_ERROR;
        }
        if (rc > 0) {
            break;
        }
        if (!guarded) {
            return ALOOPER_POLL_TIMEOUT;
        }
        if (!(vp_choreographer_caps() & VP_VSYNC_CAP_FD_WAKEUP)) {
            /* The host clock went away: degrade rather than block forever. */
            vp_choreographer_drop_fd();
            vp_choreographer_dispatch(vp_choreographer_wait_vsync());
            return ALOOPER_POLL_CALLBACK;
        }
        /* Clock alive but no frame delivered yet: ask again. */
        virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC,
                         0, 0, 0, 0, 0, 0);
    }

    bool ran_callback = false;
    for (int i = 0; i < n; i++) {
        if (!pfd[i].revents) {
            continue;
        }
        vp_looper_fd_t* e = vp_looper_find(snap_fd[i]);
        if (!e) {
            continue;   /* a previous callback removed it */
        }
        if (e->ident == ALOOPER_POLL_CALLBACK) {
            ran_callback = true;
            if (e->cb && e->cb(e->fd, pfd[i].revents, e->data) == 0) {
                vp_looper_del(e->fd);   /* NDK: returning 0 unregisters the fd */
            }
            continue;
        }
        if (events) *events = pfd[i].revents;
        if (data) *data = e->data;
        if (source) *source = &g_looper;
        return e->ident;
    }
    return ran_callback ? ALOOPER_POLL_CALLBACK : ALOOPER_POLL_TIMEOUT;
}

/* Keep draining callbacks until a poll returns something other than them. */
int ALooper_pollAll(int timeoutMillis, int* events, void** data, void** source)
{
    for (;;) {
        int r = ALooper_pollOnce(timeoutMillis, events, data, source);
        if (r != ALOOPER_POLL_CALLBACK) {
            return r;
        }
    }
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                  ALooper_callbackFunc callback, void* data)
{
    (void)looper;
    return vp_looper_add(fd, ident, events, callback, data);
}

int ALooper_removeFd(ALooper* looper, int fd)
{
    (void)looper;
    return vp_looper_del(fd);
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
    int32_t cmd = (int32_t)virtpass_syscall(SYS_ANDROID_CALL, SYS_ANDROID_GAME_POLL_CMD, 0, 0, 0, 0, 0, 0);

    /* "Nothing pending" travels as a non-negative sentinel so the per-frame
     * poll does not trip rvvm-user's "negative syscall result" warning; the
     * NDK contract this function exposes is still -1. Hosts that answer -1
     * directly (older builds, the Win32 host) keep working unchanged. */
    if (cmd == VP_GAME_CMD_NONE) {
        return -1;
    }
    return cmd;
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
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            /* Refresh the cached panel geometry, then publish the window. */
            guest_window_query_size();
            app->window = &g_window;
            break;

        case APP_CMD_TERM_WINDOW:
            app->window = NULL;
            break;

        case APP_CMD_WINDOW_RESIZED:
            /* Keep the cached geometry in sync for ANativeWindow_query() users. */
            guest_window_query_size();
            break;

        case APP_CMD_START:
            app->activityState |= (1 << APP_CMD_START);
            break;

        case APP_CMD_STOP:
            app->activityState &= ~(1 << APP_CMD_START);
            /* A stopped Activity can never still be resumed. The host always
             * sends PAUSE first, this only guarantees the invariant. */
            app->activityState &= ~(1 << APP_CMD_RESUME);
            break;

        case APP_CMD_RESUME:
            app->activityState |= (1 << APP_CMD_RESUME);
            break;

        case APP_CMD_PAUSE:
            app->activityState &= ~(1 << APP_CMD_RESUME);
            break;

        case APP_CMD_GAINED_FOCUS:
            app->activityState |= (1 << APP_CMD_GAINED_FOCUS);
            break;

        case APP_CMD_LOST_FOCUS:
            app->activityState &= ~(1 << APP_CMD_GAINED_FOCUS);
            break;

        case APP_CMD_DESTROY:
            app->destroyRequested = 1;
            break;

        default:
            break;
    }
}

/* GameActivity API: Execute lifecycle command */
void android_app_exec_cmd(android_app* app, int32_t cmd)
{
    if (!app) {
        return;
    }

    vp_app_pre_exec_cmd(app, cmd);

    if (app->onAppCmd) {
        app->onAppCmd(app, cmd);
    }
}

/* GameActivity API: Swap input buffers */
int32_t android_app_swap_input_buffers(android_app* app)
{
    if (app) {
        /* Pass the guest input buffer address so the host can fill events into
         * it. Guest addresses are NOT host addresses (guest memory is the
         * userland machine's own buffer), so the host translates it with
         * rvvm_user_guest_ptr() - we just hand over the guest address. */
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
