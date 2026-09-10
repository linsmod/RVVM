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
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* Include vp_cmdpost API */
#include "virtpass/vp_cmdpost.h"

/* Include rvvm-user API */
#include "rvvm_user.h"

#define LOG_TAG "RVVM-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
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

/* GameActivity state */
static int32_t g_lifecycle_cmd_queue[32];
static int32_t g_lifecycle_cmd_count = 0;
static int32_t g_lifecycle_cmd_read = 0;

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
    
    /* Set GameActivity callbacks */
    cmdpost_set_game_callbacks(on_game_lifecycle, on_game_input);

    /* Get Android sensor manager */
    g_sensor_manager = ASensorManager_getInstance();
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

    /* Poll for events */
    int events;
    void* data;
    int result = ALooper_pollOnce(0, &events, &data, NULL);

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

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    (void)thiz;
    
    /* Release previous window if any */
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = NULL;
    }
    
    if (surface) {
        /* SurfaceCreated and SurfaceChanged both hand us a surface; the guest
         * may hold a locked buffer across the transition, so only re-acquire
         * when the window was actually cleared or replaced. */
        if (g_native_window) {
            LOGI("Native window already set, keeping existing window");
            return;
        }
        /* Get ANativeWindow from Java Surface */
        g_native_window = ANativeWindow_fromSurface(env, surface);
        if (g_native_window) {
            LOGI("Native window set: %dx%d", 
                 ANativeWindow_getWidth(g_native_window),
                 ANativeWindow_getHeight(g_native_window));
        } else {
            LOGE("Failed to get native window from surface");
        }
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
