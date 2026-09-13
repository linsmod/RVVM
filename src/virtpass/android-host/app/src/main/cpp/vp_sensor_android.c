/*
 * vp_sensor_android.c - Android backend for the Virtpass sensor subsystem.
 *
 * Device facts come from ASensorManager_getSensorList(); events come from one
 * ASensorEventQueue created on a dedicated looper thread, which is the
 * canonical NDK usage. Every event is handed to vp_sensor_ingest(), which owns
 * the wire identity (handle/type/flags) and the per-queue fan-out - so this
 * file never has to know what a Virtpass queue is, and the guest never sees a
 * platform sensor handle.
 *
 * This replaces the old double registration where Java registered a
 * SensorEventListener *and* the native side enabled sensors on an NDK queue
 * nobody drained.
 */

#define LOG_TAG "RVVM-SENSOR"

#include <android/log.h>
#include <android/sensor.h>
#include <android/looper.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vp_sensor_android.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

/* The Virtpass sensor ABI is byte-identical to the NDK event, which is what
 * lets a platform event be memcpy'd straight into the wire struct (the
 * subsystem then overwrites version/sensor/type/flags from its descriptor).
 * If a future NDK ever changes that layout this must fail the build rather
 * than silently ship a wrong mapping. */
_Static_assert(sizeof(ASensorEvent) == sizeof(vp_sensor_event_t),
               "NDK ASensorEvent must match the Virtpass wire event");

static ASensorManager*     g_manager = NULL;
static const ASensor*      g_platform[VP_SENSOR_MAX_HANDLES];

/* Published with a release store after g_platform[] has been filled, so the
 * sensor thread's reverse mapping can acquire it without the mutex. Counting
 * is all it needs: the entries themselves never move afterwards. */
static _Atomic int32_t     g_platform_count = 0;

static ASensorEventQueue*  g_queue = NULL;
static ALooper*            g_looper = NULL;
static pthread_t           g_thread;
static bool                g_thread_started = false;
static volatile int        g_running = 0;
static bool                g_started = false;

/* Guest-visible enable/rate state, replayed onto the queue by the thread if a
 * guest got there before the queue existed. */
static pthread_mutex_t     g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool                g_enabled[VP_SENSOR_MAX_HANDLES];
static int32_t             g_rate_us[VP_SENSOR_MAX_HANDLES];

static void android_str_copy(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len > cap - 1) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ============================================================
 * vp_sensor_ops_t
 * ============================================================ */

static int32_t android_enumerate(vp_sensor_info_t* out, int32_t max)
{
    if (!g_manager || !out || max <= 0) {
        return 0;
    }

    ASensorList list = NULL;
    int listed = ASensorManager_getSensorList(g_manager, &list);
    if (listed < 0 || !list) {
        return 0;
    }

    int32_t count = 0;
    for (int i = 0; i < listed && count < max && count < VP_SENSOR_MAX_HANDLES; i++) {
        const ASensor* sensor = list[i];
        if (!sensor) {
            continue;
        }

        vp_sensor_info_t* info = &out[count];
        memset(info, 0, sizeof(*info));
        info->handle = count;   /* dense index; the subsystem enforces it too */
        info->type = ASensor_getType(sensor);
        info->reporting_mode = ASensor_getReportingMode(sensor);
        info->min_delay_us = ASensor_getMinDelay(sensor);
        info->fifo_max_events = ASensor_getFifoMaxEventCount(sensor);
        info->fifo_reserved_events = ASensor_getFifoReservedEventCount(sensor);
        info->wake_up = ASensor_isWakeUpSensor(sensor) ? 1 : 0;
        info->highest_direct_rate_level = ASENSOR_DIRECT_RATE_STOP;  /* unsupported */
        info->resolution = ASensor_getResolution(sensor);
        android_str_copy(info->string_type, sizeof(info->string_type),
                         ASensor_getStringType(sensor));
        android_str_copy(info->name, sizeof(info->name), ASensor_getName(sensor));
        android_str_copy(info->vendor, sizeof(info->vendor), ASensor_getVendor(sensor));

        g_platform[count] = sensor;
        count++;
    }
    atomic_store_explicit(&g_platform_count, count, memory_order_release);
    LOGI("enumerated %d sensors", count);
    return count;
}

static int32_t android_platform_count(void)
{
    return atomic_load_explicit(&g_platform_count, memory_order_acquire);
}

static void android_apply_rate(int32_t handle, int32_t period_us)
{
    if (!g_queue || handle < 0 || handle >= android_platform_count() || period_us <= 0) {
        return;
    }
    ASensorEventQueue_setEventRate(g_queue, g_platform[handle], period_us);
}

static int32_t android_set_rate(int32_t handle, int32_t period_us, int32_t max_batch_us)
{
    (void)max_batch_us;  /* the platform batches on its own schedule */

    if (handle < 0 || handle >= android_platform_count()) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }

    pthread_mutex_lock(&g_lock);
    g_rate_us[handle] = period_us;
    bool live = g_queue && g_enabled[handle];
    pthread_mutex_unlock(&g_lock);

    if (live) {
        android_apply_rate(handle, period_us);
    }
    return VP_SENSOR_OK;
}

static int32_t android_set_enabled(int32_t handle, bool enable)
{
    if (handle < 0 || handle >= android_platform_count()) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }

    pthread_mutex_lock(&g_lock);
    g_enabled[handle] = enable;
    ASensorEventQueue* queue = g_queue;
    const ASensor* sensor = g_platform[handle];
    int32_t rate = g_rate_us[handle];
    pthread_mutex_unlock(&g_lock);

    if (!queue || !sensor) {
        /* The thread replays this once its queue is up. */
        return VP_SENSOR_OK;
    }

    if (enable) {
        android_apply_rate(handle, rate);
        return ASensorEventQueue_enableSensor(queue, sensor) == 0
             ? VP_SENSOR_OK : VP_SENSOR_ERROR_UNSUPPORTED;
    }
    return ASensorEventQueue_disableSensor(queue, sensor) == 0
         ? VP_SENSOR_OK : VP_SENSOR_ERROR_UNSUPPORTED;
}

static uint32_t android_query(void)
{
    /* FD_WAKEUP: the queue's wake pipe is a real host fd, so the subsystem can
     * write into it from the sensor thread. */
    return VP_SENSOR_CAP_LIST | VP_SENSOR_CAP_RATE | VP_SENSOR_CAP_FD_WAKEUP;
}

static const vp_sensor_ops_t g_android_sensor_ops = {
    .enumerate = android_enumerate,
    .set_enabled = android_set_enabled,
    .set_rate = android_set_rate,
    .query = android_query,
};

const vp_sensor_ops_t* android_sensor_ops(void)
{
    return &g_android_sensor_ops;
}

/* ============================================================
 * Platform event source
 * ============================================================ */

/* Map a platform event back to the dense handle the guest was given.
 *
 * The mapping goes through the type, not through ASensorEvent.sensor: that
 * field carries the platform's own sensor handle, which is neither dense nor
 * stable across devices, and the accessor that reads it back on the ASensor
 * side (ASensor_getHandle) is __INTRODUCED_IN(29) while this app runs on
 * minSdk 28. The platform handle therefore never crosses the ABI at all. */
static int32_t android_slot_for(const ASensorEvent* event)
{
    int32_t count = android_platform_count();

    for (int32_t i = 0; i < count; i++) {
        if (ASensor_getType(g_platform[i]) == event->type) {
            return i;
        }
    }
    return -1;
}

static int android_sensor_events(int fd, int events, void* data)
{
    (void)fd;
    (void)events;
    (void)data;

    /* Only the sensor thread writes g_queue, and shutdown() clears it after
     * joining this thread, so a local snapshot is all the validity we need. */
    ASensorEventQueue* queue = g_queue;
    if (!queue) {
        return 1;
    }

    ASensorEvent batch[16];
    for (;;) {
        ssize_t got = ASensorEventQueue_getEvents(queue, batch, 16);
        if (got <= 0) {
            break;
        }
        for (ssize_t i = 0; i < got; i++) {
            int32_t slot = android_slot_for(&batch[i]);
            if (slot < 0) {
                continue;
            }
            /* Layouts are asserted identical above; vp_sensor_ingest() replaces
             * version/sensor/type/flags with the descriptor's own values. */
            vp_sensor_event_t wire;
            memcpy(&wire, &batch[i], sizeof(wire));
            vp_sensor_ingest(slot, &wire);
        }
    }
    return 1;  /* stay registered */
}

static void* android_sensor_thread(void* arg)
{
    (void)arg;

    g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorEventQueue* queue = g_looper
        ? ASensorManager_createEventQueue(g_manager, g_looper, ALOOPER_POLL_CALLBACK,
                                          android_sensor_events, NULL)
        : NULL;
    if (!queue) {
        LOGW("sensor event queue unavailable; guest will see no events");
    }

    /* Publish the queue and snapshot the requested state in one critical
     * section, so a set_enabled() that raced us is either already visible here
     * or sees the queue and applies itself. */
    bool enabled[VP_SENSOR_MAX_HANDLES];
    int32_t rate[VP_SENSOR_MAX_HANDLES];
    pthread_mutex_lock(&g_lock);
    g_queue = queue;
    memcpy(enabled, g_enabled, sizeof(enabled));
    memcpy(rate, g_rate_us, sizeof(rate));
    pthread_mutex_unlock(&g_lock);

    if (queue) {
        for (int32_t i = 0; i < android_platform_count(); i++) {
            if (enabled[i]) {
                android_apply_rate(i, rate[i]);
                ASensorEventQueue_enableSensor(queue, g_platform[i]);
            }
        }
    }

    while (g_running) {
        ALooper_pollAll(-1, NULL, NULL, NULL);
    }

    if (queue) {
        for (int32_t i = 0; i < android_platform_count(); i++) {
            if (enabled[i]) {
                ASensorEventQueue_disableSensor(queue, g_platform[i]);
            }
        }
    }
    return NULL;
}

/* ============================================================
 * Lifetime
 * ============================================================ */

void android_sensor_start(void)
{
    if (g_started) {
        return;
    }
    g_started = true;

    /* Per-package singleton (API 26+); NULL asks for this app's instance. */
    g_manager = ASensorManager_getInstanceForPackage(NULL);
    if (!g_manager) {
        LOGW("no sensor manager; the guest sensor list stays empty");
        return;
    }

    g_running = 1;
    if (pthread_create(&g_thread, NULL, android_sensor_thread, NULL) != 0) {
        LOGW("failed to start the sensor thread");
        g_running = 0;
        return;
    }
    g_thread_started = true;
}

void android_sensor_shutdown(void)
{
    if (g_thread_started) {
        g_running = 0;
        if (g_looper) {
            ALooper_wake(g_looper);  /* unblock pollAll so it sees g_running */
        }
        pthread_join(g_thread, NULL);
        g_thread_started = false;
    }

    pthread_mutex_lock(&g_lock);
    ASensorEventQueue* queue = g_queue;
    g_queue = NULL;
    memset(g_enabled, 0, sizeof(g_enabled));
    memset(g_rate_us, 0, sizeof(g_rate_us));
    pthread_mutex_unlock(&g_lock);

    if (g_manager && queue) {
        ASensorManager_destroyEventQueue(g_manager, queue);
    }
    g_looper = NULL;
    atomic_store_explicit(&g_platform_count, 0, memory_order_release);
    g_started = false;
}
