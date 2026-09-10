package com.rvvm.android;

import android.view.Surface;

/**
 * Native interface for RVVM integration.
 * This class provides JNI bindings to the native rvvm_jni library.
 */
public class RvvmNative {

    static {
        System.loadLibrary("rvvm_jni");
    }

    /**
     * Initialize the native RVVM system.
     * Must be called before any other native methods.
     */
    public static native void nativeInit();

    /**
     * Clean up native resources.
     * Must be called when the app is destroyed.
     */
    public static native void nativeDestroy();

    /**
     * Set the native window for rendering.
     * @param surface The Surface to render to
     */
    public static native void nativeSetWindow(Surface surface);

    /**
     * Push the real device configuration (AConfiguration values) to native.
     * Every metric is read from the Java Configuration object, which is the
     * only place the exact dp/density figures are available.
     *
     * @param widthDp     Current screen width in dp
     * @param heightDp    Current screen height in dp
     * @param densityDpi  Density bucket in dpi (e.g. 160, 320)
     * @param orientation Screen orientation (Configuration.ORIENTATION_*)
     * @param screenSize  Screen size class (Configuration.SCREENLAYOUT_SIZE_*)
     * @param screenLong  Long-screen flag (ACONFIGURATION_SCREENLONG_*)
     * @param screenRound Round-screen flag (ACONFIGURATION_SCREENROUND_*)
     */
    public static native void nativeSetDisplayConfig(int widthDp, int heightDp, int densityDpi,
                                                     int orientation, int screenSize,
                                                     int screenLong, int screenRound);

    /**
     * Enable a specific sensor.
     * @param sensorType The sensor type (e.g., Sensor.TYPE_ACCELEROMETER)
     */
    public static native void nativeEnableSensor(int sensorType);

    /**
     * Disable a specific sensor.
     * @param sensorType The sensor type to disable
     */
    public static native void nativeDisableSensor(int sensorType);

    /**
     * Poll for sensor events.
     * @return true if events were available, false otherwise
     */
    public static native boolean nativePollEvents();

    /**
     * Push sensor data directly to the ring buffer.
     * @param x X-axis value
     * @param y Y-axis value
     * @param z Z-axis value
     * @param sensorType The sensor type
     * @param timestamp Timestamp in nanoseconds
     */
    public static native void nativePushSensorData(float x, float y, float z, int sensorType, long timestamp);

    /**
     * Get the library version.
     * @return Version string
     */
    public static native String nativeGetVersion();

    /**
     * Poll for lifecycle command from native side.
     * @return The lifecycle command, or -1 if none available
     */
    public static native int nativePollLifecycleCmd();

    /**
     * Clear all lifecycle commands from the queue.
     */
    public static native void nativeClearLifecycleCmds();

    /**
     * Post a lifecycle command to the native queue.
     * @param cmd The lifecycle command (APP_CMD_*)
     */
    public static native void nativePostLifecycleCmd(int cmd);

    /**
     * Post a motion event to the guest-facing input queue.
     * @param x X coordinate
     * @param y Y coordinate
     * @param action Motion action (AMOTION_EVENT_ACTION_*)
     * @param eventTime Event timestamp in nanoseconds
     */
    public static native void nativePostMotionEvent(float x, float y, int action, long eventTime);

    /**
     * Run a RISC-V ELF program.
     * @param elfPath Path to the ELF file
     * @param args Optional command-line arguments
     * @return true if the guest started successfully
     */
    public static native boolean nativeRunElf(String elfPath, String[] args);

    /**
     * Check if the guest is currently running.
     * @return true if guest is running
     */
    public static native boolean nativeIsGuestRunning();

    /**
     * Stop the running guest.
     */
    public static native void nativeStopGuest();
}
