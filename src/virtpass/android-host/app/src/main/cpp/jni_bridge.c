/*
 * jni_bridge.c - JNI bridge for Android NDK API proxy
 *
 * This file provides the JNI interface between Java (Android) and
 * the native rvvm library. It allows the Android side to:
 * - Initialize the sensor system
 * - Push sensor events to the ring buffer
 * - Handle lifecycle events
 * - Handle window operations (lock/unlock)
 * - Run RISC-V Guest ELF programs via rvvm-user
 */

#include <jni.h>
#include <android/log.h>
#include <android/sensor.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/configuration.h>  /* ACONFIGURATION_* constants */
#include <android/choreographer.h> /* AChoreographer_* (display vsync) */
#include <android/looper.h>        /* ALooper_* (vsync pump) */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

/* Include vp_cmdpost API */
#include "virtpass/vp_cmdpost.h"

/* Include rvvm-user API */
#include "rvvm_user.h"

#define LOG_TAG "RVVM-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Global references to Java objects */
static JavaVM* g_jvm = NULL;
static JNIEnv* g_env = NULL;
static jobject g_sensor_manager_obj = NULL;
static jobject g_sensor_listener_obj = NULL;

/* Sensor manager from Android */
static ASensorManager* g_sensor_manager = NULL;
static const ASensor* g_accelerometer = NULL;
static const ASensor* g_gyroscope = NULL;
static const ASensor* g_light = NULL;

/* Event queue */
static ASensorEventQueue* g_event_queue = NULL;
static ALooper* g_looper = NULL;

/* Window from Android */
static ANativeWindow* g_native_window = NULL;

/* Device configuration pushed from the Java Configuration object.
 * The public NDK AConfiguration only carries quantised values, and the
 * exact dp/density figures live on the Java side. */
static struct {
    int32_t width_dp;
    int32_t height_dp;
    int32_t density_dpi;
    int32_t orientation;
    int32_t screen_size;
    int32_t screen_long;
    int32_t screen_round;
} g_display_cfg = {
    .width_dp     = 640,
    .height_dp    = 480,
    .density_dpi  = ACONFIGURATION_DENSITY_MEDIUM,
    .orientation  = ACONFIGURATION_ORIENTATION_LAND,
    .screen_size  = ACONFIGURATION_SCREENSIZE_NORMAL,
    .screen_long  = ACONFIGURATION_SCREENLONG_NO,
    .screen_round = ACONFIGURATION_SCREENROUND_NO,
};

/* GameActivity state */
static int32_t g_lifecycle_cmd_queue[32];
static int32_t g_lifecycle_cmd_count = 0;
static int32_t g_lifecycle_cmd_read = 0;

/* ============================================================
 * Choreographer (Phase 4): display vsync source
 * ============================================================
 * The guest's AChoreographer stubs consume this from two directions:
 *  - fd wakeup (方案 B): the guest registers the write end of its Looper pipe
 *    and asks for one vsync per request; vp_cmdpost_vsync_tick() below writes
 *    the frame time into that fd, so the guest wakes in poll() instead of
 *    polling us.
 *  - blocking WAIT: a guest that could not use the fd path parks in
 *    SYS_ANDROID_CHOREOGRAPHER_WAIT and is released by the condvar below.
 *
 * A dedicated thread owns the real NDK AChoreographer instance (it is
 * per-thread and needs a Looper that is actually pumped), keeps one frame
 * callback armed at all times, and publishes each vsync's frame time. Guest
 * consumers only observe the latest tick, so a slow or stalled guest cannot
 * hold up the vsync source itself.
 */
static pthread_t       g_vsync_thread;
static ALooper*        g_vsync_looper = NULL;
static volatile int    g_vsync_running = 0;
static pthread_mutex_t g_vsync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vsync_cond  = PTHREAD_COND_INITIALIZER;
static int64_t         g_vsync_frame_time = 0;
static uint64_t        g_vsync_seq = 0;
static int             g_vsync_warned = 0;

static void on_vsync_frame(long frame_time_nanos, void* data)
{
    (void)data;

    pthread_mutex_lock(&g_vsync_mutex);
    g_vsync_frame_time = frame_time_nanos;
    g_vsync_seq++;
    pthread_cond_broadcast(&g_vsync_cond);
    pthread_mutex_unlock(&g_vsync_mutex);

    /* fd path: hand the frame time to the guest's Looper pipe if it asked for
     * this vsync. Cheap no-op while nothing is armed. */
    vp_cmdpost_vsync_tick((int64_t)frame_time_nanos);

    /* Keep the tick continuous: re-arm immediately from inside the callback. */
    if (g_vsync_running) {
        AChoreographer* choreographer = AChoreographer_getInstance();
        if (choreographer) {
            AChoreographer_postFrameCallback(choreographer, on_vsync_frame, NULL);
        }
    }
}

/*
 * Owns the AChoreographer instance. AChoreographer is per-thread and only
 * dispatches while its Looper is pumped, so this thread prepares a Looper and
 * then simply drains it; every drained frame callback publishes a new tick.
 */
static void* vsync_thread_func(void* arg)
{
    (void)arg;

    g_vsync_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    AChoreographer* choreographer = AChoreographer_getInstance();
    if (!choreographer) {
        LOGE("vsync: AChoreographer_getInstance() failed, guest will fall back");
        return NULL;
    }

    AChoreographer_postFrameCallback(choreographer, on_vsync_frame, NULL);
    LOGI("vsync: AChoreographer source started");

    while (g_vsync_running) {
        int result = ALooper_pollOnce(-1, NULL, NULL, NULL);
        if (result == ALOOPER_POLL_ERROR) {
            LOGE("vsync: ALooper_pollOnce() error, stopping vsync source");
            break;
        }
    }

    LOGI("vsync: AChoreographer source stopped");
    return NULL;
}

/*
 * Blocks the caller until the next display vsync and returns its frame time in
 * nanoseconds. Returns -1 when the source is unavailable, so the guest can
 * fall back to its own clock instead of stalling.
 */
static int64_t android_vsync_wait(void)
{
    int64_t frame_time = -1;

    pthread_mutex_lock(&g_vsync_mutex);

    uint64_t start_seq = g_vsync_seq;
    while (g_vsync_running && g_vsync_seq == start_seq) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000 * 1000;   /* 100ms safety net */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&g_vsync_cond, &g_vsync_mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }

    if (g_vsync_seq != start_seq) {
        frame_time = g_vsync_frame_time;
    } else if (!g_vsync_warned) {
        g_vsync_warned = 1;
        LOGW("vsync: no tick within 100ms, guest falls back to its own clock");
    }

    pthread_mutex_unlock(&g_vsync_mutex);
    return frame_time;
}

/* Guest execution state */
static pthread_t g_guest_thread;
static int g_guest_running = 0;
static char g_guest_elf_path[512];
static int g_guest_argc = 0;
static char* g_guest_argv[16];

/* Guest thread function */
static void* guest_thread_func(void* arg)
{
    (void)arg;
    
    LOGI("Guest thread started, ELF: %s", g_guest_elf_path);
    
    /* Build argc/argv for rvvm_user_linux() */
    /* argv[0] = ELF path, argv[1..] = guest args */
    g_guest_argv[0] = g_guest_elf_path;
    
    /* On non-riscv hosts rvvm_user.c defaults prefix_path to a hardcoded
     * Debian userland path. Disable it so host paths pass through unchanged. */
    putenv("RVVM_USER_PREFIX=");
    
    int result = rvvm_user_linux(g_guest_argc, g_guest_argv, NULL);
    
    LOGI("Guest thread finished with code: %d", result);
    g_guest_running = 0;
    
    return NULL;
}

/* Sensor data callback (called from Java) */
static void on_sensor_data(float x, float y, float z, int type, int64_t timestamp)
{
    sensor_event_t event = {0};
    event.version = 0;
    event.sensor = type;
    event.type = type;
    event.timestamp = timestamp;
    event.vector.x = x;
    event.vector.y = y;
    event.vector.z = z;

    cmdpost_push_sensor_event(&event);
}

/* Locked window buffer info (kept between lock and unlock) */
static ANativeWindow_Buffer g_locked_buffer;

/* Window lock callback (called from vp_cmdpost)
 * Fills geometry only; the guest renders into its own buffer (identity-mapped
 * guest/host addresses) and we copy pixels on unlock. Never expose the host
 * surface pointer to the guest. */
static int32_t on_window_lock(void* window, void* outBuffer, void* dirtyBounds)
{
    (void)window;
    (void)dirtyBounds;

    if (!g_native_window) {
        LOGE("Window not initialized");
        return -1;
    }

    ANativeWindow_Buffer buffer;
    ARect dirty;

    int32_t result = ANativeWindow_lock(g_native_window, &buffer, &dirty);
    if (result == 0) {
        g_locked_buffer = buffer;
        if (outBuffer) {
            /* outBuffer points into guest memory; geometry only, bits stays 0 */
            int32_t* dst = (int32_t*)outBuffer;
            dst[0] = 0;               /* bits: guest supplies its own buffer */
            dst[1] = buffer.width;
            dst[2] = buffer.height;
            dst[3] = buffer.stride;
            dst[4] = buffer.format;

            LOGI("Window locked: %dx%d stride=%d format=%d",
                 buffer.width, buffer.height, buffer.stride, buffer.format);
        }
    }
    return result;
}

/* Window unlock callback (called from vp_cmdpost)
 * Copies the guest-rendered pixels into the real surface buffer, then posts. */
static int32_t on_window_unlock(void* window, void* guestPixels)
{
    (void)window;

    if (!g_native_window) {
        LOGE("Window not initialized");
        return -1;
    }

    if (guestPixels && g_locked_buffer.bits) {
        size_t bpp = (g_locked_buffer.format == WINDOW_FORMAT_RGB_565) ? 2 : 4;
        size_t copy_size = (size_t)g_locked_buffer.stride * g_locked_buffer.height * bpp;
        LOGI("Unlock: copying %zu bytes from guest pixbuf %p -> %p",
             copy_size, guestPixels, g_locked_buffer.bits);
        {
            /* Row-by-row copy with progress traces to bisect the fault */
            uint8_t* src = (uint8_t*)guestPixels;
            uint8_t* dst = (uint8_t*)g_locked_buffer.bits;
            size_t row = (size_t)g_locked_buffer.stride * bpp;
            for (int32_t y = 0; y < g_locked_buffer.height; y++) {
                memcpy(dst + (size_t)y * row, src + (size_t)y * row, row);
                if ((y & 255) == 255) {
                    LOGI("Unlock: copied %d/%d rows", y + 1, g_locked_buffer.height);
                }
            }
        }
        LOGI("Unlock: copy done");
    } else {
        /* Lock succeeded but there is no frame to copy (guest pixbuf missing,
         * or the surface buffer pointer was invalidated by a window change).
         * Still post so the buffer queue keeps flowing, but make the cause
         * visible instead of silently presenting an untouched frame. */
        LOGW("Unlock: no frame copied (guestPixels=%p, surfaceBits=%p); posting untouched buffer",
             guestPixels, g_locked_buffer.bits);
    }

    int32_t result = ANativeWindow_unlockAndPost(g_native_window);
    if (result == 0) {
        LOGI("Window unlocked and posted");
    }
    return result;
}

/* Window size callback (called from vp_cmdpost) */
static void on_window_size(int64_t* width, int64_t* height)
{
    if (g_native_window) {
        *width  = (int64_t)ANativeWindow_getWidth(g_native_window);
        *height = (int64_t)ANativeWindow_getHeight(g_native_window);
    } else {
        *width = 0;
        *height = 0;
    }
}

/* Window set-buffers-geometry callback (called from vp_cmdpost).
 * The guest's ANativeWindow_setBuffersGeometry is a stub that forwards here,
 * so the real surface matches the guest's pixel format (avoids buffer
 * overrun when the guest writes RGBA_8888 into an RGB_565 surface). */
static int32_t on_window_set_buf(int32_t width, int32_t height, int32_t format)
{
    if (!g_native_window) {
        LOGE("Window not initialized (set buf %dx%d fmt=%d)", width, height, format);
        return -1;
    }

    int32_t result = ANativeWindow_setBuffersGeometry(g_native_window,
                                                      width, height, format);
    if (result == 0) {
        LOGI("Window geometry set: %dx%d format=%d", width, height, format);
    } else {
        LOGE("ANativeWindow_setBuffersGeometry(%dx%d fmt=%d) failed: %d",
             width, height, format, result);
    }
    return result;
}

/* Device configuration callback (called from vp_cmdpost).
 * Serves the values pushed by nativeSetDisplayConfig(). */
static int32_t on_config_get(int32_t field, int32_t* outValue)
{
    if (!outValue) return -1;

    switch (field) {
    case VP_ACONFIG_QUERY_ORIENTATION:
        *outValue = g_display_cfg.orientation;
        return 0;
    case VP_ACONFIG_QUERY_DENSITY:
        *outValue = g_display_cfg.density_dpi;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_SIZE:
        *outValue = g_display_cfg.screen_size;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_LONG:
        *outValue = g_display_cfg.screen_long;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_ROUND:
        *outValue = g_display_cfg.screen_round;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_WIDTH_DP:
        *outValue = g_display_cfg.width_dp;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP:
        *outValue = g_display_cfg.height_dp;
        return 0;
    default:
        return -1;
    }
}

/* GameActivity lifecycle callback (called from vp_cmdpost) */
static void on_game_lifecycle(int32_t cmd)
{
    LOGI("GameActivity lifecycle cmd=%d", cmd);
    
    /* Queue the command for polling by Guest */
    if (g_lifecycle_cmd_count < 32) {
        g_lifecycle_cmd_queue[g_lifecycle_cmd_count++] = cmd;
    }
}

/* GameActivity input callback (called from vp_cmdpost) */
static void on_game_input(void* motionEvent)
{
    (void)motionEvent;
    LOGI("GameActivity input event received");
}

/* ============================================================
 * JNI Initialization
 * ============================================================ */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    g_jvm = vm;
    LOGI("JNI_OnLoad: RVVM JNI bridge loaded");
    return JNI_VERSION_1_6;
}

/* ============================================================
 * Native methods called from Java
 * ============================================================ */

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeInit(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    g_env = env;

    /* Initialize vp_cmdpost */
    cmdpost_init();
    
    /* Set window callbacks */
    cmdpost_set_window_callbacks(on_window_lock, on_window_unlock);
    cmdpost_set_window_size_callback(on_window_size);
    cmdpost_set_window_set_buf_callback(on_window_set_buf);
    cmdpost_set_config_callback(on_config_get);
    
    /* Set GameActivity callbacks */
    cmdpost_set_game_callbacks(on_game_lifecycle, on_game_input);

    /* Expose the real display vsync as the guest's AChoreographer source. */
    cmdpost_set_choreographer_callback(android_vsync_wait);
    g_vsync_running = 1;
    if (pthread_create(&g_vsync_thread, NULL, vsync_thread_func, NULL) != 0) {
        g_vsync_running = 0;
        cmdpost_set_choreographer_callback(NULL);
        LOGE("vsync: failed to start AChoreographer thread, guest will fall back");
    }

    /* Get Android sensor manager (per-package singleton, API 26+) */
    g_sensor_manager = ASensorManager_getInstanceForPackage(NULL);
    if (g_sensor_manager) {
        LOGI("Sensor manager initialized");

        /* Get default sensors */
        g_accelerometer = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_ACCELEROMETER);
        g_gyroscope = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_GYROSCOPE);
        g_light = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_LIGHT);

        LOGI("Accelerometer: %s", g_accelerometer ? "found" : "not found");
        LOGI("Gyroscope: %s", g_gyroscope ? "found" : "not found");
        LOGI("Light: %s", g_light ? "found" : "not found");
    } else {
        LOGE("Failed to get sensor manager");
    }

    /* Create looper and event queue */
    g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (g_looper) {
        g_event_queue = ASensorManager_createEventQueue(g_sensor_manager, g_looper, 0, NULL, NULL);
        LOGI("Event queue created");
    }

    LOGI("Native init complete");
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeDestroy(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    /* Disable sensors */
    if (g_event_queue && g_accelerometer) {
        ASensorEventQueue_disableSensor(g_event_queue, g_accelerometer);
    }
    if (g_event_queue && g_gyroscope) {
        ASensorEventQueue_disableSensor(g_event_queue, g_gyroscope);
    }
    if (g_event_queue && g_light) {
        ASensorEventQueue_disableSensor(g_event_queue, g_light);
    }

    /* Destroy event queue */
    if (g_sensor_manager && g_event_queue) {
        ASensorManager_destroyEventQueue(g_sensor_manager, g_event_queue);
    }

    /* Stop the vsync source before tearing down the bridge. */
    if (g_vsync_running) {
        g_vsync_running = 0;

        /* Release a guest blocked in poll() on the vsync fd: it is told the
         * clock is gone so it degrades instead of waiting forever. */
        vp_cmdpost_vsync_source_lost();

        /* Wake a thread parked in ALooper_pollOnce(-1) ... */
        if (g_vsync_looper) {
            ALooper_wake(g_vsync_looper);
        }
        /* ... and one parked in android_vsync_wait(). */
        pthread_mutex_lock(&g_vsync_mutex);
        pthread_cond_broadcast(&g_vsync_cond);
        pthread_mutex_unlock(&g_vsync_mutex);

        pthread_join(g_vsync_thread, NULL);
        g_vsync_looper = NULL;
    }

    /* Cleanup vp_cmdpost */
    cmdpost_cleanup();

    LOGI("Native destroy complete");
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeEnableSensor(JNIEnv* env, jobject thiz, jint sensorType)
{
    (void)env;
    (void)thiz;

    if (!g_event_queue) {
        LOGE("Event queue not initialized");
        return;
    }

    const ASensor* sensor = NULL;
    switch (sensorType) {
        case ASENSOR_TYPE_ACCELEROMETER:
            sensor = g_accelerometer;
            break;
        case ASENSOR_TYPE_GYROSCOPE:
            sensor = g_gyroscope;
            break;
        case ASENSOR_TYPE_LIGHT:
            sensor = g_light;
            break;
        default:
            LOGE("Unknown sensor type: %d", sensorType);
            return;
    }

    if (sensor) {
        int result = ASensorEventQueue_enableSensor(g_event_queue, sensor);
        if (result == 0) {
            LOGI("Sensor %d enabled", sensorType);
        } else {
            LOGE("Failed to enable sensor %d", sensorType);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeDisableSensor(JNIEnv* env, jobject thiz, jint sensorType)
{
    (void)env;
    (void)thiz;

    if (!g_event_queue) {
        return;
    }

    const ASensor* sensor = NULL;
    switch (sensorType) {
        case ASENSOR_TYPE_ACCELEROMETER:
            sensor = g_accelerometer;
            break;
        case ASENSOR_TYPE_GYROSCOPE:
            sensor = g_gyroscope;
            break;
        case ASENSOR_TYPE_LIGHT:
            sensor = g_light;
            break;
        default:
            return;
    }

    if (sensor) {
        ASensorEventQueue_disableSensor(g_event_queue, sensor);
        LOGI("Sensor %d disabled", sensorType);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativePollEvents(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    if (!g_event_queue) {
        return JNI_FALSE;
    }

    /* Poll for events: (timeoutMillis, outFd, outEvents, outData) */
    int events;
    void* data;
    int result = ALooper_pollOnce(0, NULL, &events, &data);

    if (result >= 0) {
        /* Read sensor events */
        ASensorEvent event[16];
        ssize_t count = ASensorEventQueue_getEvents(g_event_queue, event, 16);

        if (count > 0) {
            LOGI("Polled %zd sensor events", count);

            /* Push events to ring buffer */
            for (ssize_t i = 0; i < count; i++) {
                on_sensor_data(
                    event[i].acceleration.x,
                    event[i].acceleration.y,
                    event[i].acceleration.z,
                    event[i].type,
                    event[i].timestamp
                );
            }
            return JNI_TRUE;
        }
    }

    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativePushSensorData(
    JNIEnv* env, jobject thiz,
    jfloat x, jfloat y, jfloat z,
    jint sensorType, jlong timestamp)
{
    (void)env;
    (void)thiz;
    on_sensor_data(x, y, z, sensorType, timestamp);
}

JNIEXPORT jstring JNICALL
Java_com_rvvm_android_RvvmNative_nativeGetVersion(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return (*env)->NewStringUTF(env, "1.0.0");
}

/* Push the real device configuration from the Java Configuration object.
 * Called by MainActivity on start and on configuration changes. */
JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetDisplayConfig(
    JNIEnv* env, jobject thiz,
    jint widthDp, jint heightDp, jint densityDpi,
    jint orientation, jint screenSize, jint screenLong, jint screenRound)
{
    (void)env;
    (void)thiz;

    g_display_cfg.width_dp     = (int32_t)widthDp;
    g_display_cfg.height_dp    = (int32_t)heightDp;
    g_display_cfg.density_dpi  = (int32_t)densityDpi;
    g_display_cfg.orientation  = (int32_t)orientation;
    g_display_cfg.screen_size  = (int32_t)screenSize;
    g_display_cfg.screen_long  = (int32_t)screenLong;
    g_display_cfg.screen_round = (int32_t)screenRound;

    LOGI("Display config: %dx%d dp, density=%d, orient=%d, size=%d, long=%d, round=%d",
         widthDp, heightDp, densityDpi, orientation, screenSize, screenLong, screenRound);
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    (void)thiz;
    
    /* SurfaceCreated and SurfaceChanged both hand us a surface. The guest may
     * hold a locked buffer across that transition, so acquire the window first
     * and, if it is the same one we already track, keep it as-is (the extra
     * reference is dropped immediately). */
    ANativeWindow* new_window = NULL;
    if (surface) {
        new_window = ANativeWindow_fromSurface(env, surface);
        if (!new_window) {
            LOGE("Failed to get native window from surface");
            return;
        }
        if (new_window == g_native_window) {
            ANativeWindow_release(new_window);
            LOGI("Native window unchanged, keeping existing window");
            return;
        }
    }

    /* A different window (or NULL) means the surface is really going away or
     * being replaced: release the old one before adopting the new. */
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = NULL;
    }

    if (new_window) {
        g_native_window = new_window;
        LOGI("Native window set: %dx%d",
             ANativeWindow_getWidth(g_native_window),
             ANativeWindow_getHeight(g_native_window));
    } else {
        LOGI("Native window cleared");
    }
}

JNIEXPORT jint JNICALL
Java_com_rvvm_android_RvvmNative_nativePollLifecycleCmd(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    
    /* Return next lifecycle command from queue, or -1 if empty */
    if (g_lifecycle_cmd_read < g_lifecycle_cmd_count) {
        return g_lifecycle_cmd_queue[g_lifecycle_cmd_read++];
    }
    return -1;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeClearLifecycleCmds(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    g_lifecycle_cmd_count = 0;
    g_lifecycle_cmd_read = 0;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativePostLifecycleCmd(JNIEnv* env, jobject thiz, jint cmd)
{
    (void)env;
    (void)thiz;
    
    /* Queue a lifecycle command (called from Java when activity state changes)
     * Both into the guest-facing queue (via vp_cmdpost) and the local queue. */
    cmdpost_queue_lifecycle_cmd(cmd);
    if (g_lifecycle_cmd_count < 32) {
        g_lifecycle_cmd_queue[g_lifecycle_cmd_count++] = cmd;
        LOGI("Lifecycle command %d queued", cmd);
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativePostMotionEvent(JNIEnv* env, jobject thiz,
                                                        jfloat x, jfloat y, jint action,
                                                        jlong eventTime)
{
    (void)env;
    (void)thiz;
    
    /* Build a motion event matching the guest ABI and queue it for the guest */
    cmdpost_GameActivityMotionEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.eventTime = (int64_t)eventTime;
    ev.deviceId = 0;
    ev.source = 0x0002; /* AINPUT_SOURCE_TOUCHSCREEN */
    ev.action = (int32_t)action;
    ev.pointerCount = 1;
    ev.pointers[0].x = x;
    ev.pointers[0].y = y;
    ev.pointers[0].rawX = x;
    ev.pointers[0].rawY = y;
    ev.pointers[0].pressure = 1.0f;
    ev.pointers[0].size = 1.0f;
    ev.pointers[0].id = 0;
    ev.pointers[0].toolType = 1; /* AMOTION_EVENT_TOOL_TYPE_FINGER */
    
    cmdpost_queue_motion_event(&ev);
    LOGI("Motion event queued: x=%.0f y=%.0f action=%d", x, y, action);
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativeRunElf(JNIEnv* env, jobject thiz, jstring elfPath, jobjectArray args)
{
    (void)thiz;
    
    if (g_guest_running) {
        LOGE("Guest already running");
        return JNI_FALSE;
    }
    
    /* Get ELF path from Java string */
    const char* path = (*env)->GetStringUTFChars(env, elfPath, NULL);
    if (!path) {
        LOGE("Failed to get ELF path");
        return JNI_FALSE;
    }
    
    strncpy(g_guest_elf_path, path, sizeof(g_guest_elf_path) - 1);
    g_guest_elf_path[sizeof(g_guest_elf_path) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, elfPath, path);
    
    /* Get optional arguments */
    g_guest_argc = 1;  /* argv[0] = ELF path */
    if (args) {
        jsize len = (*env)->GetArrayLength(env, args);
        for (int i = 0; i < len && g_guest_argc < 15; i++) {
            jstring jstr = (jstring)(*env)->GetObjectArrayElement(env, args, i);
            const char* str = (*env)->GetStringUTFChars(env, jstr, NULL);
            if (str) {
                /* Store in static buffer (simplified - no dynamic alloc) */
                static char arg_buf[16][128];
                strncpy(arg_buf[g_guest_argc], str, 127);
                arg_buf[g_guest_argc][127] = '\0';
                g_guest_argv[g_guest_argc] = arg_buf[g_guest_argc];
                g_guest_argc++;
                (*env)->ReleaseStringUTFChars(env, jstr, str);
            }
            (*env)->DeleteLocalRef(env, jstr);
        }
    }
    
    LOGI("Starting guest: %s (argc=%d)", g_guest_elf_path, g_guest_argc);
    
    /* Start guest thread */
    g_guest_running = 1;
    if (pthread_create(&g_guest_thread, NULL, guest_thread_func, NULL) != 0) {
        LOGE("Failed to create guest thread");
        g_guest_running = 0;
        return JNI_FALSE;
    }
    
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativeIsGuestRunning(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return g_guest_running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeStopGuest(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    
    if (g_guest_running) {
        LOGI("Stopping guest...");
        /* TODO: Send signal to guest thread to stop */
        /* For now, we just mark it as not running */
        g_guest_running = 0;
    }
}
