package com.rvvm.android;

import android.app.Activity;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Main Activity for RVVM Android host app.
 * This activity manages the RVVM process and sensor integration.
 */
public class MainActivity extends Activity implements SensorEventListener, SurfaceHolder.Callback2 {

    private static final String TAG = "RVVM-MainActivity";

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
    private Button stopButton;
    private Spinner guestAppSpinner;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Find views
        statusText = findViewById(R.id.statusText);
        sensorDataText = findViewById(R.id.sensorDataText);
        surfaceView = findViewById(R.id.surfaceView);
        runButton = findViewById(R.id.runButton);
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
        
        boolean started = RvvmNative.nativeRunElf(elfPath, null);
        if (started) {
            statusText.setText("Guest started: " + elfName);
            Log.i(TAG, "Guest started: " + elfPath);
        } else {
            statusText.setText("Failed to start guest");
            Log.e(TAG, "Failed to start guest");
        }
        updateButtonStates();

        // Monitor guest exit: poll the flag and re-enable buttons when the
        // guest thread finishes (e.g. test_audio exits after 1 second).
        new Thread(() -> {
            while (RvvmNative.nativeIsGuestRunning()) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
            }
            runOnUiThread(this::updateButtonStates);
        }, "guest-exit-monitor").start();
    }

    private void stopGuestElf() {
        if (!isInitialized || !RvvmNative.nativeIsGuestRunning()) {
            return;
        }
        Log.i(TAG, "Stopping guest");
        RvvmNative.nativeStopGuest();
        statusText.setText("Guest stopped");
        updateButtonStates();
    }

    private void updateButtonStates() {
        boolean running = RvvmNative.nativeIsGuestRunning();
        runButton.setEnabled(!running);
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
        // Cleanup native resources
        if (isInitialized) {
            RvvmNative.nativeDestroy();
            isInitialized = false;
        }
    }
}