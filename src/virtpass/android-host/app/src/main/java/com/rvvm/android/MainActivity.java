package com.rvvm.android;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Main Activity for RVVM Android host app.
 * This activity manages the RVVM process and sensor integration.
 */
public class MainActivity extends Activity implements SensorEventListener, SurfaceHolder.Callback2 {

    private static final String TAG = "RVVM-MainActivity";

    /**
     * Intent extra naming the guest app to run, for example:
     *   adb shell am start -n com.rvvm.android/.MainActivity --es guest test_render.exe
     * The value is matched against the *.exe entries in assets (the ".exe"
     * suffix is optional), overriding the default "first entry" selection.
     */
    public static final String EXTRA_GUEST_APP = "guest";

    // Lifecycle commands forwarded to the guest. These are the APP_CMD_* values
    // from include/virtpass/vp_android.h - they are the wire format of the
    // host -> guest lifecycle channel and must stay in sync with that enum.
    private static final int APP_CMD_INIT_WINDOW          = 1;
    private static final int APP_CMD_TERM_WINDOW          = 2;
    private static final int APP_CMD_WINDOW_RESIZED       = 3;
    private static final int APP_CMD_WINDOW_REDRAW_NEEDED = 4;
    private static final int APP_CMD_GAINED_FOCUS         = 6;
    private static final int APP_CMD_LOST_FOCUS           = 7;
    private static final int APP_CMD_CONFIG_CHANGED       = 8;
    private static final int APP_CMD_LOW_MEMORY           = 9;
    private static final int APP_CMD_START                = 10;
    private static final int APP_CMD_RESUME               = 11;
    private static final int APP_CMD_SAVE_STATE           = 12;
    private static final int APP_CMD_PAUSE                = 13;
    private static final int APP_CMD_STOP                 = 14;
    private static final int APP_CMD_DESTROY              = 15;

    private SensorManager sensorManager;
    private Sensor accelerometer;
    private Sensor gyroscope;
    private Sensor light;

    private TextView statusText;
    private TextView sensorDataText;
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private Button runButton;
    private Button suspendButton;
    private Button stopButton;
    private Spinner guestAppSpinner;

    // Guest console overlay, drawn on top of the SurfaceView. Visible before
    // the guest renders its first frame and again after it exits; hidden once
    // a frame has actually reached the surface.
    private HorizontalScrollView logOverlayScroll;  // X axis + tap-to-toggle
    private ScrollView logOverlayVScroll;           // Y axis
    private TextView logOverlayText;
    private final StringBuilder logBuffer = new StringBuilder();

    // Overlay text mode. Monospace always; tap toggles wrapping.
    private boolean logWrapText = true;

    // True from Run until the next cold start: while set, the console overlay
    // is shown whenever the guest is not actively rendering frames. Keeping
    // the overlay up even with no output yet (a placeholder line shows) is
    // what gives the tap-to-toggle-wrap gesture a stable target - a guest
    // that prints nothing would otherwise leave nothing to tap.
    private boolean consoleActive = false;

    // The run's log file. Opened in runGuestElf (UI thread), written from the
    // guest thread (onOutput), closed when the guest exits - hence the lock.
    private final Object logFileLock = new Object();
    private BufferedWriter logWriter;
    private File logFile;

    // Set once the first presented frame arrived (CPU unlock or GL swap).
    private boolean guestRendering = false;

    // Tail kept in the overlay text, so an endless guest cannot grow it
    // without bound.
    private static final int LOG_MAX_CHARS = 64 * 1024;
    private static final int LOG_MAX_FILES = 20;
    private static final String LOG_PLACEHOLDER = "[console] waiting for guest output...\n";

    // Guest app list: .exe files from assets
    private String[] guestApps;
    private String selectedGuestApp;

    // Reusable per-pointer buffers for multi-touch passthrough. Sized to match
    // CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT on the native side; reused to
    // avoid allocating on every touch event.
    private static final int MAX_POINTERS = 16;
    private final float[] motionX = new float[MAX_POINTERS];
    private final float[] motionY = new float[MAX_POINTERS];
    private final int[] motionId = new int[MAX_POINTERS];

    private boolean isInitialized = false;
    private boolean isSurfaceReady = false;
    private boolean hasAutoStarted = false;

    // Exit code of the most recent guest, delivered by the native exit
    // callback. The callback fires on the guest thread while the guest is
    // still winding down, so it only records the value; the exit-monitor
    // thread publishes it to the UI once nativeIsGuestRunning() has cleared.
    private volatile int lastExitCode = -1;

    // Assets file name of the guest currently launched, or null when none runs.
    // Remembered so an Intent asking for the same guest is not torn down.
    private String currentGuestApp;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Find views
        statusText = findViewById(R.id.statusText);
        sensorDataText = findViewById(R.id.sensorDataText);
        surfaceView = findViewById(R.id.surfaceView);
        logOverlayScroll = findViewById(R.id.logOverlayScroll);
        logOverlayVScroll = findViewById(R.id.logOverlayVScroll);
        logOverlayText = findViewById(R.id.logOverlayText);

        // Tap the overlay (a tap, not a scroll) to toggle word wrap; the
        // current mode is confirmed with a toast. Monospace is always on.
        //
        // The tap is detected on the inner ScrollView: it is the view that
        // actually owns the touch stream (a scrolling view consumes every
        // gesture in onTouchEvent), so an OnClickListener on either scroller
        // never fires. The listener returns false so the normal drag/fling
        // handling of both scrollers is left untouched.
        final GestureDetector logTapDetector = new GestureDetector(this,
                new GestureDetector.SimpleOnGestureListener() {
                    @Override
                    public boolean onSingleTapUp(MotionEvent e) {
                        toggleLogWrap();
                        return true;
                    }
                });
        logOverlayVScroll.setOnTouchListener((v, event) -> {
            logTapDetector.onTouchEvent(event);
            return false;
        });
        runButton = findViewById(R.id.runButton);
        suspendButton = findViewById(R.id.suspendButton);
        stopButton = findViewById(R.id.stopButton);
        guestAppSpinner = findViewById(R.id.guestAppSpinner);

        // Populate guest app spinner from assets
        populateGuestApps();
        ArrayAdapter<String> spinnerAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, guestApps);
        spinnerAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        guestAppSpinner.setAdapter(spinnerAdapter);
        guestAppSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, android.view.View view, int position, long id) {
                selectedGuestApp = guestApps[position];
                Log.i(TAG, "Selected guest app: " + selectedGuestApp);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });

        // Honor an explicit "which guest to run" Intent before the auto-start
        // path picks its default. The launch itself happens in
        // maybeAutoStartGuest() once the surface is ready, so only the
        // selection is applied here.
        applyGuestSelectionFromIntent(getIntent());

        // Setup surface holder
        surfaceHolder = surfaceView.getHolder();
        surfaceHolder.addCallback(this);

        // Initialize sensor manager
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        gyroscope = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        light = sensorManager.getDefaultSensor(Sensor.TYPE_LIGHT);

        // Setup buttons
        runButton.setOnClickListener(v -> runGuestElf());
        suspendButton.setOnClickListener(v -> toggleSuspendGuest());
        stopButton.setOnClickListener(v -> stopGuestElf());
        updateButtonStates();

        // Forward touches on the surface to the guest (all pointers)
        surfaceView.setOnTouchListener((v, event) -> {
            if (!isInitialized) {
                return false;
            }
            int count = event.getPointerCount();
            if (count > MAX_POINTERS) {
                count = MAX_POINTERS;
            }
            for (int i = 0; i < count; i++) {
                motionX[i] = event.getX(i);
                motionY[i] = event.getY(i);
                motionId[i] = event.getPointerId(i);
            }
            // Use the raw action: ACTION_POINTER_DOWN/UP encode the pointer
            // index in the upper bits, which the guest's GameActivity expects.
            RvvmNative.nativePostMotionEvent(motionX, motionY, motionId, count,
                    event.getAction(), event.getEventTime() * 1000000L);
            return true;
        });

        // Initialize native RVVM
        initializeRvvm();
    }

    private void initializeRvvm() {
        try {
            // Initialize native library
            RvvmNative.nativeInit();
            isInitialized = true;

            // Record the guest's exit code as it fires (guest thread). The UI
            // is updated by the guest-exit-monitor, not here: the callback
            // runs while the guest thread has not finished unwinding yet.
            RvvmNative.nativeSetExitCallback(code -> {
                lastExitCode = code;
                Log.i(TAG, "Guest exit callback: code " + code);
            });

            // Guest console I/O: rendered in the overlay and persisted to a
            // file. Both callbacks fire on the guest thread; anything that
            // touches a view hops to the UI thread first.
            RvvmNative.nativeSetConsoleListener(new RvvmNative.ConsoleListener() {
                @Override
                public void onOutput(String line) {
                    handleGuestOutput(line);
                }

                @Override
                public void onFirstFrame() {
                    handleFirstFrame();
                }
            });

            // Push the real screen metrics (the AConfiguration source of truth)
            pushDisplayConfig();

            // Enable sensors
            if (accelerometer != null) {
                RvvmNative.nativeEnableSensor(Sensor.TYPE_ACCELEROMETER);
                sensorManager.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_NORMAL);
            }
            if (gyroscope != null) {
                RvvmNative.nativeEnableSensor(Sensor.TYPE_GYROSCOPE);
                sensorManager.registerListener(this, gyroscope, SensorManager.SENSOR_DELAY_NORMAL);
            }
            if (light != null) {
                RvvmNative.nativeEnableSensor(Sensor.TYPE_LIGHT);
                sensorManager.registerListener(this, light, SensorManager.SENSOR_DELAY_NORMAL);
            }

            statusText.setText("RVVM initialized\nVersion: " + RvvmNative.nativeGetVersion());
            Log.i(TAG, "RVVM initialized");

            maybeAutoStartGuest();

        } catch (Exception e) {
            statusText.setText("Failed to initialize RVVM: " + e.getMessage());
            Log.e(TAG, "Failed to initialize RVVM", e);
        }
    }

    /**
     * Push the real device configuration to native.
     *
     * The exact dp/density figures only exist on the Java side, so the guest
     * learns them through the AConfiguration_* proxies. screenLong and
     * screenRound are mapped to the ACONFIGURATION_* values the NDK uses.
     */
    private void pushDisplayConfig() {
        Configuration config = getResources().getConfiguration();
        int screenLayout = config.screenLayout;

        int longMode = (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK) == Configuration.SCREENLAYOUT_LONG_YES ? 2
                     : (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK) == Configuration.SCREENLAYOUT_LONG_NO ? 1
                     : 0;
        int roundMode = config.isScreenRound() ? 2 : 1;

        RvvmNative.nativeSetDisplayConfig(
                config.screenWidthDp,
                config.screenHeightDp,
                config.densityDpi,
                config.orientation,
                screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK,
                longMode,
                roundMode);
    }

    /**
     * Forward one Activity/Surface lifecycle transition to the guest.
     *
     * The guest runs its own GameActivity-style loop and consumes these through
     * android_app_read_cmd(), so the host must deliver the same transitions, in
     * the same order, that a real GameActivity would. The native side keeps the
     * aggregate state (window / activityState) in sync around the guest's
     * onAppCmd() callback, which is what lets the guest restore its "showing
     * rendered content" state after the Activity was backgrounded and resumed.
     */
    private void postLifecycleCmd(int cmd) {
        if (!isInitialized) {
            return;
        }
        Log.i(TAG, "Lifecycle -> guest: cmd=" + cmd);
        RvvmNative.nativePostLifecycleCmd(cmd);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (isInitialized) {
            pushDisplayConfig();
            postLifecycleCmd(APP_CMD_CONFIG_CHANGED);
        }
    }

    private void maybeAutoStartGuest() {
        if (!isInitialized || !isSurfaceReady || hasAutoStarted) {
            return;
        }
        hasAutoStarted = true;
        Log.i(TAG, "Auto-starting guest ELF");
        runGuestElf();
    }

    private void runGuestElf() {
        if (!isInitialized) {
            statusText.setText("RVVM not initialized");
            return;
        }

        if (RvvmNative.nativeIsGuestRunning()) {
            statusText.setText("Guest already running");
            return;
        }

        String elfName = selectedGuestApp;
        if (elfName == null || elfName.isEmpty()) {
            statusText.setText("No guest app selected");
            return;
        }

        // Copy ELF from assets to internal storage (always refresh so updated builds take effect)
        File elfFile = new File(getFilesDir(), elfName);

        try {
            copyAssetToFile(elfName, elfFile);
        } catch (IOException e) {
            statusText.setText("Failed to copy ELF: " + e.getMessage());
            Log.e(TAG, "Failed to copy ELF", e);
            return;
        }

        // Run the ELF
        String elfPath = elfFile.getAbsolutePath();
        statusText.setText("Running: " + elfName + "\nPath: " + elfPath);

        // New run: clear the console overlay and start a fresh log file. The
        // overlay stays visible (showing the guest's early output) until the
        // first presented frame arrives, then hides; on exit it comes back.
        clearGuestConsole();
        openGuestLogFile(elfName);

        replayGuestStartupState();

        boolean started = RvvmNative.nativeRunElf(elfPath, null);
        if (started) {
            currentGuestApp = elfName;
            statusText.setText("Guest started: " + elfName);
            Log.i(TAG, "Guest started: " + elfPath);
        } else {
            statusText.setText("Failed to start guest");
            Log.e(TAG, "Failed to start guest");
        }
        updateButtonStates();

        // Monitor guest exit: poll the flag and report the outcome when the
        // guest thread finishes (e.g. test_audio exits after 1 second). The
        // exit code itself arrives earlier through the native exit callback
        // (lastExitCode); polling is still what says the guest is really gone,
        // because nativeIsGuestRunning() only clears after the guest thread
        // has fully unwound - flipping buttons in onExit would be premature.
        new Thread(() -> {
            while (RvvmNative.nativeIsGuestRunning()) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
            }
            int code = lastExitCode;
            runOnUiThread(() -> {
                currentGuestApp = null;
                closeGuestLogFile();
                // The guest is gone: bring the console overlay back over the
                // last frame (or the black surface), showing the tail of its
                // output next to the exit status.
                guestRendering = false;
                updateLogOverlay();
                statusText.setText("Guest exited: " + elfName + " (exit " + code + ")");
                Log.i(TAG, "Guest exited: " + elfName + " (exit " + code + ")");
                updateButtonStates();
            });
        }, "guest-exit-monitor").start();
    }

    /**
     * Re-deliver the lifecycle state a freshly started guest has to observe.
     *
     * Lifecycle reaches the guest as *transitions* only (onStart, onResume,
     * surfaceCreated, focus). A guest launched after those already happened -
     * the normal case when switching targets inside this singleTask Activity,
     * or a Run after a previous guest exited - therefore misses all of them:
     * android_app ends up with window == NULL and no activityState bits, and a
     * GameActivity-style loop correctly concludes it has nothing to draw on and
     * never presents a single frame, even though the surface is right there
     * (this is why "run a guest, then run test_game_activity" showed a black
     * screen while a direct launch worked).
     *
     * Called before the guest thread exists, so the queue is cleared of the
     * previous guest's leftovers and seeded with the commands this guest would
     * have seen on a cold start: START -> RESUME -> INIT_WINDOW -> focus.
     */
    private void replayGuestStartupState() {
        RvvmNative.nativeClearLifecycleCmds();
        postLifecycleCmd(APP_CMD_START);
        postLifecycleCmd(APP_CMD_RESUME);
        if (isSurfaceReady) {
            postLifecycleCmd(APP_CMD_INIT_WINDOW);
            if (hasWindowFocus()) {
                postLifecycleCmd(APP_CMD_GAINED_FOCUS);
            }
        }
    }

    private void stopGuestElf() {
        if (!isInitialized || !RvvmNative.nativeIsGuestRunning()) {
            return;
        }
        Log.i(TAG, "Stopping guest");
        RvvmNative.nativeStopGuest();
        currentGuestApp = null;
        // Provisional: the stop is asynchronous (the guest unwinds like a
        // normal exit), so the guest-exit-monitor lands the final state with
        // the actual exit code in a moment.
        statusText.setText("Guest stopping...");
        updateButtonStates();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // The launcher Activity is singleTask, so a repeated
        // "am start --es guest ..." lands here instead of onCreate: reselect the
        // requested guest and relaunch if it differs from the running one.
        if (applyGuestSelectionFromIntent(intent)) {
            startSelectedGuestIfNeeded();
        }
    }

    /**
     * Resolve an Intent's EXTRA_GUEST_APP against the assets *.exe list and
     * select it in the spinner. Returns true when the request was honored.
     */
    private boolean applyGuestSelectionFromIntent(Intent intent) {
        if (intent == null || guestApps == null || guestApps.length == 0) {
            return false;
        }
        String requested = intent.getStringExtra(EXTRA_GUEST_APP);
        if (requested == null || requested.trim().isEmpty()) {
            return false;
        }
        requested = requested.trim();
        // Accept both "test_render" and "test_render.exe".
        String withSuffix = requested.endsWith(".exe") ? requested : requested + ".exe";
        for (int i = 0; i < guestApps.length; i++) {
            if (guestApps[i].equalsIgnoreCase(requested) || guestApps[i].equalsIgnoreCase(withSuffix)) {
                selectedGuestApp = guestApps[i];
                guestAppSpinner.setSelection(i);
                statusText.setText("Selected guest: " + guestApps[i]);
                Log.i(TAG, "Intent selected guest app: " + guestApps[i]);
                return true;
            }
        }
        Log.w(TAG, "Intent requested unknown guest app: " + requested);
        return false;
    }

    /**
     * Launch the selected guest unless that exact guest is already running.
     * Stopping is asynchronous (the guest thread clears the running flag only
     * after it unwinds), so switching targets waits for the old guest to retire
     * before launching the new one.
     */
    private void startSelectedGuestIfNeeded() {
        if (!isInitialized || selectedGuestApp == null || selectedGuestApp.isEmpty()) {
            return;
        }
        if (!RvvmNative.nativeIsGuestRunning()) {
            runGuestElf();
            return;
        }
        if (selectedGuestApp.equals(currentGuestApp)) {
            Log.i(TAG, "Guest already running: " + currentGuestApp);
            return;
        }
        Log.i(TAG, "Switching guest: " + currentGuestApp + " -> " + selectedGuestApp);
        RvvmNative.nativeStopGuest();
        currentGuestApp = null;
        new Thread(() -> {
            while (RvvmNative.nativeIsGuestRunning()) {
                try { Thread.sleep(50); } catch (InterruptedException e) { return; }
            }
            runOnUiThread(this::runGuestElf);
        }, "guest-switch").start();
    }

    /**
     * Pause/resume the guest: the native side parks the guest's vCPUs (and the
     * frame clock) without tearing anything down, so the guest keeps its state
     * and continues exactly where it left off on resume.
     */
    private void toggleSuspendGuest() {
        if (!isInitialized || !RvvmNative.nativeIsGuestRunning()) {
            return;
        }
        if (RvvmNative.nativeIsGuestSuspended()) {
            RvvmNative.nativeResumeGuest();
            statusText.setText("Guest resumed");
            Log.i(TAG, "Guest resumed");
        } else {
            RvvmNative.nativeSuspendGuest();
            statusText.setText("Guest suspended");
            Log.i(TAG, "Guest suspended");
        }
        updateButtonStates();
    }

    private void updateButtonStates() {
        boolean running = RvvmNative.nativeIsGuestRunning();
        // nativeIsGuestSuspended() is only meaningful while a guest runs; it
        // reports the requested state, so the label flips immediately even when
        // a vCPU is still unwinding a blocking host syscall.
        boolean suspended = running && RvvmNative.nativeIsGuestSuspended();
        runButton.setEnabled(!running);
        suspendButton.setEnabled(running);
        suspendButton.setText(suspended ? R.string.resume_guest : R.string.suspend_guest);
        stopButton.setEnabled(running);
        guestAppSpinner.setEnabled(!running);
    }

    /**
     * Scan assets for .exe files and populate the guest app list.
     */
    private void populateGuestApps() {
        try {
            String[] assets = getAssets().list("");
            java.util.List<String> exeList = new java.util.ArrayList<>();
            if (assets != null) {
                for (String name : assets) {
                    if (name.endsWith(".exe")) {
                        exeList.add(name);
                    }
                }
            }
            java.util.Collections.sort(exeList);
            guestApps = exeList.toArray(new String[0]);
            if (guestApps.length > 0) {
                selectedGuestApp = guestApps[0];
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to list assets", e);
            guestApps = new String[]{ "test_game_activity.exe" };
            selectedGuestApp = guestApps[0];
        }
    }

    private void copyAssetToFile(String assetName, File outFile) throws IOException {
        AssetManager assetManager = getAssets();
        InputStream in = assetManager.open(assetName);
        FileOutputStream out = new FileOutputStream(outFile);
        
        byte[] buffer = new byte[4096];
        int read;
        while ((read = in.read(buffer)) != -1) {
            out.write(buffer, 0, read);
        }
        
        out.close();
        in.close();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "Surface created");
        isSurfaceReady = true;

        // Set the native window
        if (isInitialized) {
            RvvmNative.nativeSetWindow(holder.getSurface());
            postLifecycleCmd(APP_CMD_INIT_WINDOW);
            maybeAutoStartGuest();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "Surface changed: " + width + "x" + height);
        isSurfaceReady = true;

        // Update the native window; the guest gets a resize notification too so
        // it can re-query the panel geometry.
        if (isInitialized) {
            RvvmNative.nativeSetWindow(holder.getSurface());
            postLifecycleCmd(APP_CMD_WINDOW_RESIZED);
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "Surface destroyed");
        isSurfaceReady = false;

        // Clear the native window
        if (isInitialized) {
            RvvmNative.nativeSetWindow(null);
            postLifecycleCmd(APP_CMD_TERM_WINDOW);
        }
    }

    /**
     * Called by the framework right before the surface is shown again (and
     * after every surfaceCreated/surfaceChanged) to ask the view to redraw its
     * content. Forwarding it lets the guest repaint immediately instead of
     * waiting for the next vsync.
     *
     * Implementing SurfaceHolder.Callback2 rather than Callback is what makes
     * SurfaceView deliver this callback; the async variant has a default
     * implementation that calls this one, so only the synchronous form is
     * needed here.
     */
    @Override
    public void surfaceRedrawNeeded(SurfaceHolder holder) {
        postLifecycleCmd(APP_CMD_WINDOW_REDRAW_NEEDED);
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!isInitialized) {
            return;
        }

        // Push sensor data to native ring buffer
        RvvmNative.nativePushSensorData(
            event.values[0],
            event.values[1],
            event.values[2],
            event.sensor.getType(),
            event.timestamp
        );

        // Update UI
        String sensorName = getSensorName(event.sensor.getType());
        String data = String.format("%s: %.2f, %.2f, %.2f",
            sensorName, event.values[0], event.values[1], event.values[2]);
        sensorDataText.setText(data);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // Not used
    }

    // --- Guest console overlay ------------------------------------------------
    //
    // Three states, driven by two signals:
    //   running, no frame yet  -> overlay visible, showing guest output
    //   first frame arrived    -> overlay hidden, the surface shows the render
    //   guest exited           -> overlay visible again over the last frame
    //
    // Output is also appended to a per-run log file under files/logs/.

    /** Clears the overlay text (guest (re)start). UI thread only. */
    private void clearGuestConsole() {
        logBuffer.setLength(0);
        // Placeholder keeps the overlay renderable (and tappable) from the
        // first moment; real output lines replace it as they arrive.
        logBuffer.append(LOG_PLACEHOLDER);
        logOverlayText.setText(logBuffer);
        consoleActive = true;
        guestRendering = false;
        updateLogOverlay();
    }

    /** Opens a fresh log file for this run: logs/<guest>-<timestamp>.log. */
    private void openGuestLogFile(String guestName) {
        synchronized (logFileLock) {
            closeGuestLogFileLocked();
            try {
                File dir = new File(getFilesDir(), "logs");
                if (!dir.exists() && !dir.mkdirs()) {
                    Log.w(TAG, "Could not create logs dir; file logging disabled");
                    return;
                }
                pruneGuestLogs(dir);

                String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss",
                        java.util.Locale.US).format(new java.util.Date());
                String base = guestName.endsWith(".exe")
                        ? guestName.substring(0, guestName.length() - 4) : guestName;
                logFile = new File(dir, base + "-" + stamp + ".log");
                logWriter = new BufferedWriter(new FileWriter(logFile));
                Log.i(TAG, "Guest log: " + logFile.getAbsolutePath());
            } catch (IOException e) {
                Log.w(TAG, "Guest log file disabled: " + e.getMessage());
                logWriter = null;
                logFile = null;
            }
        }
    }

    /** Closes the current log file. Safe to call from any thread. */
    private void closeGuestLogFile() {
        synchronized (logFileLock) {
            closeGuestLogFileLocked();
        }
    }

    /** Caller holds logFileLock. */
    private void closeGuestLogFileLocked() {
        if (logWriter != null) {
            try { logWriter.flush(); logWriter.close(); }
            catch (IOException e) { Log.w(TAG, "Closing guest log failed: " + e.getMessage()); }
            logWriter = null;
        }
    }

    /** Keep only the newest LOG_MAX_FILES logs. Called with no writer open. */
    private void pruneGuestLogs(File dir) {
        File[] files = dir.listFiles((d, name) -> name.endsWith(".log"));
        if (files == null || files.length <= LOG_MAX_FILES) return;
        java.util.Arrays.sort(files,
                (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        for (int i = LOG_MAX_FILES; i < files.length; i++) {
            if (!files[i].delete()) {
                Log.w(TAG, "Could not prune old guest log: " + files[i].getName());
            }
        }
    }

    /** One console line from the guest. Guest thread. */
    private void handleGuestOutput(String line) {
        synchronized (logFileLock) {
            if (logWriter != null) {
                try {
                    logWriter.write(line);
                    logWriter.newLine();
                    logWriter.flush();
                } catch (IOException e) {
                    Log.w(TAG, "Guest log write failed: " + e.getMessage());
                    closeGuestLogFileLocked();
                }
            }
        }

        // Overlay text is a tail; the actual history lives in the file.
        runOnUiThread(() -> {
            // First real line replaces the placeholder.
            if (LOG_PLACEHOLDER.contentEquals(logBuffer)) {
                logBuffer.setLength(0);
            }
            logBuffer.append(line).append('\n');
            int over = logBuffer.length() - LOG_MAX_CHARS;
            if (over > 0) {
                int cut = logBuffer.indexOf("\n", over);
                logBuffer.delete(0, (cut >= 0) ? cut + 1 : over);
            }
            logOverlayText.setText(logBuffer);
            updateLogOverlay();
            logOverlayVScroll.post(() ->
                    logOverlayVScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    /** The first presented frame is on the surface. Guest thread. */
    private void handleFirstFrame() {
        runOnUiThread(() -> {
            guestRendering = true;
            updateLogOverlay();
        });
    }

    /** Show the overlay while a console session is live and no frame has
     *  taken over the surface. */
    private void updateLogOverlay() {
        boolean show = consoleActive && !guestRendering;
        logOverlayScroll.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    /** Tap on the overlay: toggle word wrap, confirm with a toast. */
    private void toggleLogWrap() {
        logWrapText = !logWrapText;
        Log.i(TAG, "Log wrap toggled: " + (logWrapText ? "on" : "off"));
        applyLogWrapMode();
    }

    /**
     * Apply the wrap mode. Wrapping needs the text view to fill the overlay
     * width; single-line mode lets it extend past the surface width, with the
     * outer HorizontalScrollView providing the sideways scroll.
     */
    private void applyLogWrapMode() {
        logOverlayText.setHorizontallyScrolling(!logWrapText);

        ViewGroup.LayoutParams tvLp = logOverlayText.getLayoutParams();
        ViewGroup.LayoutParams svLp = logOverlayVScroll.getLayoutParams();
        int width = logWrapText ? ViewGroup.LayoutParams.MATCH_PARENT
                                : ViewGroup.LayoutParams.WRAP_CONTENT;
        tvLp.width = width;
        svLp.width = width;
        logOverlayText.setLayoutParams(tvLp);
        logOverlayVScroll.setLayoutParams(svLp);

        // Keep the tail in view after the re-layout.
        logOverlayVScroll.post(() ->
                logOverlayVScroll.fullScroll(View.FOCUS_DOWN));

        Toast.makeText(this,
                logWrapText ? R.string.log_wrap_on : R.string.log_wrap_off,
                Toast.LENGTH_SHORT).show();
    }

    private String getSensorName(int type) {
        switch (type) {
            case Sensor.TYPE_ACCELEROMETER:
                return "Accel";
            case Sensor.TYPE_GYROSCOPE:
                return "Gyro";
            case Sensor.TYPE_LIGHT:
                return "Light";
            default:
                return "Unknown";
        }
    }

    // --- Activity lifecycle --------------------------------------------------
    // The whole Activity lifecycle is mirrored to the guest, at the same points
    // the framework runs it, so the guest game loop can drive itself from
    // android_app_read_cmd() exactly as it would on a real GameActivity:
    //   onStart/onStop        -> APP_CMD_START / APP_CMD_STOP
    //   onResume/onPause      -> APP_CMD_RESUME / APP_CMD_PAUSE
    //   onWindowFocusChanged  -> APP_CMD_GAINED_FOCUS / APP_CMD_LOST_FOCUS
    //   onSaveInstanceState   -> APP_CMD_SAVE_STATE
    //   onDestroy             -> APP_CMD_DESTROY
    // The surface (window) side is handled in the SurfaceHolder callbacks above.

    @Override
    protected void onStart() {
        super.onStart();
        postLifecycleCmd(APP_CMD_START);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Re-register sensors
        if (accelerometer != null) {
            sensorManager.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_NORMAL);
        }
        if (gyroscope != null) {
            sensorManager.registerListener(this, gyroscope, SensorManager.SENSOR_DELAY_NORMAL);
        }
        if (light != null) {
            sensorManager.registerListener(this, light, SensorManager.SENSOR_DELAY_NORMAL);
        }
        postLifecycleCmd(APP_CMD_RESUME);
    }

    @Override
    protected void onPause() {
        super.onPause();
        // Unregister sensors
        sensorManager.unregisterListener(this);
        postLifecycleCmd(APP_CMD_PAUSE);
    }

    @Override
    protected void onStop() {
        super.onStop();
        postLifecycleCmd(APP_CMD_STOP);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        postLifecycleCmd(hasFocus ? APP_CMD_GAINED_FOCUS : APP_CMD_LOST_FOCUS);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        postLifecycleCmd(APP_CMD_SAVE_STATE);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        postLifecycleCmd(APP_CMD_LOW_MEMORY);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        postLifecycleCmd(APP_CMD_DESTROY);
        // Stop guest if running
        if (RvvmNative.nativeIsGuestRunning()) {
            RvvmNative.nativeStopGuest();
        }
        // Stop the console bridge before the native side goes away, so no
        // callback can reach this half-torn-down Activity.
        RvvmNative.nativeSetConsoleListener(null);
        closeGuestLogFile();
        // Cleanup native resources
        if (isInitialized) {
            RvvmNative.nativeDestroy();
            isInitialized = false;
        }
    }
}