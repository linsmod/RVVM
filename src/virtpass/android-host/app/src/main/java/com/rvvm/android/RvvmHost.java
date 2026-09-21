package com.rvvm.android;

import android.app.Application;
import android.content.res.Configuration;
import android.util.Log;

/**
 * Process-wide RVVM host.
 *
 * <p>Native RVVM state belongs to the process, not to any one Activity:
 * {@link RvvmNative#nativeInit()} boots the run table, vsync, audio and sensor
 * backends, while {@link RvvmNative#nativeDestroy()} tears all of those down.
 * Multiple guest Activities therefore share one {@code RvvmHost} and acquire a
 * reference for their lifetime. The native layer is initialized exactly once
 * and destroyed only when the last Activity releases its reference.</p>
 *
 * <p>This class also owns process-level native settings and central callbacks.
 * Per-guest UI state (surface, TTY, log file, lifecycle) stays in each
 * Activity.</p>
 */
public final class RvvmHost extends Application {
    private static final String TAG = "RVVM-RvvmHost";

    private static volatile RvvmHost instance;

    private final Object lock = new Object();
    private int acquireCount;
    private boolean initialized;
    private int panelWidth;
    private int panelHeight;
    private RvvmNative.ExitListener exitListener;
    private RvvmNative.ConsoleListener consoleListener;
    private RvvmNative.FrameCallback frameCallback;

    public static RvvmHost getInstance() {
        if (instance == null) {
            synchronized (RvvmHost.class) {
                if (instance == null) {
                    throw new IllegalStateException("RvvmHost is not registered in AndroidManifest.xml");
                }
            }
        }
        return instance;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        synchronized (RvvmHost.class) {
            if (instance != null && instance != this) {
                throw new IllegalStateException("More than one RvvmHost instance");
            }
            instance = this;
        }
    }

    /**
     * Acquire the process-wide native host. The matching {@link #release()}
     * must run from the Activity's {@code onDestroy()}.
     */
    public synchronized void acquire() {
        if (acquireCount++ > 0) {
            return;
        }
        try {
            RvvmNative.nativeInit(getAssets());
            initialized = true;
            if (panelWidth > 0 && panelHeight > 0) {
                RvvmNative.nativeSetPanelSize(panelWidth, panelHeight);
            }
            if (exitListener != null) {
                RvvmNative.nativeSetExitCallback(exitListener);
            }
            if (consoleListener != null) {
                RvvmNative.nativeSetConsoleListener(consoleListener);
            }
            if (frameCallback != null) {
                RvvmNative.nativeSetFrameCallback(frameCallback);
            }
            Log.i(TAG, "Native host acquired");
        } catch (RuntimeException e) {
            acquireCount = 0;
            initialized = false;
            throw e;
        }
    }

    /**
     * Release one Activity's reference. Native resources remain alive while
     * another guest Activity holds a reference.
     */
    public synchronized void release() {
        if (acquireCount <= 0) {
            return;
        }
        if (--acquireCount > 0) {
            return;
        }
        acquireCount = 0;
        RvvmNative.nativeSetConsoleListener(null);
        RvvmNative.nativeSetFrameCallback(null);
        RvvmNative.nativeSetExitCallback(null);
        if (initialized) {
            RvvmNative.nativeDestroy();
            initialized = false;
        }
        Log.i(TAG, "Native host released");
    }

    public synchronized boolean isInitialized() {
        return initialized;
    }

    /** Pin the guest panel before any guest can observe its geometry. */
    public synchronized void setPanelSize(int width, int height) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("Panel size must be positive");
        }
        panelWidth = width;
        panelHeight = height;
        if (initialized) {
            RvvmNative.nativeSetPanelSize(width, height);
        }
    }

    public synchronized int getPanelWidth() {
        return panelWidth;
    }

    public synchronized int getPanelHeight() {
        return panelHeight;
    }

    /** Push the current device configuration to native. */
    public synchronized void setDisplayConfig(Configuration config) {
        int screenLayout = config.screenLayout;
        int longMode = (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK)
                        == Configuration.SCREENLAYOUT_LONG_YES ? 2
                        : (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK)
                        == Configuration.SCREENLAYOUT_LONG_NO ? 1 : 0;
        int roundMode = config.isScreenRound() ? 2 : 1;
        if (initialized) {
            RvvmNative.nativeSetDisplayConfig(
                    config.screenWidthDp,
                    config.screenHeightDp,
                    config.densityDpi,
                    config.orientation,
                    screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK,
                    longMode,
                    roundMode);
        }
    }

    public synchronized void setExitListener(RvvmNative.ExitListener listener) {
        exitListener = listener;
        if (initialized) {
            RvvmNative.nativeSetExitCallback(listener);
        }
    }

    public synchronized void setConsoleListener(RvvmNative.ConsoleListener listener) {
        consoleListener = listener;
        if (initialized) {
            RvvmNative.nativeSetConsoleListener(listener);
        }
    }

    public synchronized RvvmNative.ExitListener getExitListener() {
        return exitListener;
    }

    public synchronized RvvmNative.ConsoleListener getConsoleListener() {
        return consoleListener;
    }

    public synchronized void setFrameCallback(RvvmNative.FrameCallback callback) {
        frameCallback = callback;
        if (initialized) {
            RvvmNative.nativeSetFrameCallback(callback);
        }
    }

    public synchronized RvvmNative.FrameCallback getFrameCallback() {
        return frameCallback;
    }
}
