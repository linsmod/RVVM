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
    /** Told when a client attaches to the scripted console's socket. */
    private Runnable consoleClientHook;
    /** Whether the scripted console's listener is up. */
    private boolean consoleStarted;
    /** The property that turns the console on for every launch; its value is the port. */
    public static final String CONSOLE_PROP = "debug.rvvm.console";
    private static final int DEFAULT_CONSOLE_PORT = 7979;

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
            RvvmNative.nativeInit(getAssets(), getFilesDir().getAbsolutePath());
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
            if (consoleClientHook != null) {
                RvvmNative.nativeSetConsoleConnectCallback(consoleClientHook);
            }
            // Off unless asked for. The property is the switch the trace gates
            // and the guest-capacity knob already use, so there is one place to
            // look when asking "why is this or that on"; an Activity that wants
            // the console calls enableConsoleServer() itself, which is what an
            // `am start` carrying EXTRA_CONSOLE does.
            String want = RvvmNative.nativeGetSystemProperty(CONSOLE_PROP);
            if (want != null) {
                enableConsoleServer(parsePort(want));
            }
            Log.i(TAG, "Native host acquired");
        } catch (RuntimeException e) {
            acquireCount = 0;
            initialized = false;
            throw e;
        }
    }

    /**
     * The scripted console, off unless a driver asked for it.
     *
     * <p>Loopback only and off by default: this is a shell into every guest
     * on the device, and it is only meant to be reached from the machine
     * holding the cable. The port comes from {@code debug.rvvm.console} (its
     * value, or the default when it is not a number), and an Activity can also
     * turn it on for this process with an intent extra - which is the
     * difference between a driver that has to `setprop` before every launch
     * and one that does not.</p>
     *
     * <p>The socket, its threads and its framing all live in native
     * ({@code vp_console.c}). A blocking push has no counterpart on this side
     * of JNI - there is no way to park a Java thread on a native event - so
     * the split is not a preference, it is what makes the guest's writer able
     * to block without a poll loop.</p>
     */
    public synchronized void enableConsoleServer(int requestedPort) {
        int port = (requestedPort > 0 && requestedPort <= 65535)
                ? requestedPort : DEFAULT_CONSOLE_PORT;
        if (consoleStarted) {
            return;
        }
        if (RvvmNative.nativeConsoleStart(port)) {
            consoleStarted = true;
            Log.i(TAG, "Console listening on 127.0.0.1:" + port);
        } else {
            Log.w(TAG, "Console did not start on port " + port);
        }
    }

    /** Whether the scripted console is listening. */
    public synchronized boolean isConsoleServerRunning() {
        return consoleStarted;
    }

    private static int parsePort(String value) {
        try {
            int port = Integer.parseInt(value.trim());
            return (port > 0 && port <= 65535) ? port : DEFAULT_CONSOLE_PORT;
        } catch (NumberFormatException e) {
            return DEFAULT_CONSOLE_PORT;
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
        // The console deliberately outlives this. A guest that exits finishes
        // its Activity, and the console is stopped exactly when a driver would
        // most want to ask what happened - which is also why the run's tty
        // session is retired rather than destroyed, so the last screen and the
        // whole scrollback are still there to read. Loopback-only and off
        // unless asked for, so what it costs to keep is a parked socket.
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

    /* ==================================================================
     * The core: one machine, one session per client
     * ==================================================================
     *
     * This is the Application because the machine is a process-wide thing, not a
     * property of whichever Activity happens to be in front. A run's machine is
     * created once (rvvm_user_create() per run) and torn down by on_guest_exit
     * the moment its root process returns - so a core needs a root that does not
     * return, and that decision belongs here rather than in an Activity that a
     * client could navigate away from mid-session.
     *
     * It also matters for the path. A core's root is /sbin/idle, a *system*
     * program: it has no manifest and no per-id directory, so it is not in the
     * apps archive and nativeAppEntryPath() cannot name it. Resolving it here,
     * where the guest layout is a host-level fact, is what lets the Activity
     * layer keep resolving an app by name and nothing else. */

    /** The run root for a core: holds the machine open and spawns the sessions.
     *  A system program, so a guest path rather than an app id. */
    public static final String CORE_ROOT = "/sbin/idle";

    /** The shell each session runs. */
    private String sessionShell = "/bin/sh";

    /** The control terminal's path, once the core is up. Null until then. */
    private String controlPty;

    /** The shell each session runs. */
    public synchronized void setSessionShell(String path) {
        if (path != null && !path.isEmpty()) {
            this.sessionShell = path;
        }
    }

    /** Record the core's control terminal, once its run has started.
     *
     *  <p>Not something the host can ask for beforehand: a terminal belongs to a
     *  machine, and the machine is created by the run's start. So the run makes it
     *  and tells us, and from here on this is what session requests go into.</p> */
    public synchronized void noteControlPty(String path) {
        if (path != null && !path.isEmpty()) {
            this.controlPty = path;
        }
    }

    /** The control terminal's path, or null when no core is up. */
    public synchronized String getControlPty() {
        return controlPty;
    }

    /** The shell each session runs. A session is a terminal with this shell forked
     *  onto it, so this is the one piece of a session that is a host's choice
     *  rather than the core's. */
    public synchronized String getSessionShell() {
        return sessionShell;
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
            // Straight through, not wrapped. This used to tee the event into
            // the console so a driver could ask for the status afterwards; the
            // status now travels in band as its own packet, in order, after
            // the output - so the Activity's event is the only consumer of
            // this callback again.
            RvvmNative.nativeSetExitCallback(listener);
        }
    }

    public synchronized void setConsoleListener(RvvmNative.ConsoleListener listener) {
        consoleListener = listener;
        if (initialized) {
            RvvmNative.nativeSetConsoleListener(listener);
        }
    }

    /**
     * Told when a client attaches to the scripted console's socket - not when
     * the listener binds, which the launcher can learn on its own and which
     * tells it nothing about whether anyone is there.
     *
     * <p>The run it is used to start has to wait for this: the console is a
     * live pipe that does not replay, so a guest started before a client
     * attached would be writing into a pipe with no reader, and the client that
     * finally connects would find the run already over.</p>
     *
     * <p>Called on the accept thread, not the UI thread.</p>
     */
    public synchronized void setConsoleClientHook(Runnable hook) {
        consoleClientHook = hook;
        if (initialized) {
            RvvmNative.nativeSetConsoleConnectCallback(hook);
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
