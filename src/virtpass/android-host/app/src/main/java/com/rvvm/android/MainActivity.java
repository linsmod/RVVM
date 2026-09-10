package com.rvvm.android;

import android.app.Activity;
import android.content.res.AssetManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Main Activity for RVVM Android host app.
 * This activity manages the RVVM process and sensor integration.
 */
public class MainActivity extends Activity implements SensorEventListener, SurfaceHolder.Callback {

    private static final String TAG = "RVVM-MainActivity";

    private SensorManager sensorManager;
    private Sensor accelerometer;
    private Sensor gyroscope;
    private Sensor light;

    private TextView statusText;
    private TextView sensorDataText;
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private Button runButton;

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

        // Setup surface holder
        surfaceHolder = surfaceView.getHolder();
        surfaceHolder.addCallback(this);

        // Initialize sensor manager
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        gyroscope = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        light = sensorManager.getDefaultSensor(Sensor.TYPE_LIGHT);

        // Setup run button
        runButton.setOnClickListener(v -> runGuestElf());

        // Forward touches on the surface to the guest
        surfaceView.setOnTouchListener((v, event) -> {
            if (!isInitialized) {
                return false;
            }
            int action = event.getActionMasked();
            RvvmNative.nativePostMotionEvent(event.getX(), event.getY(), action, event.getEventTime() * 1000000L);
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

        // Copy ELF from assets to internal storage (always refresh so updated builds take effect)
        String elfName = "test_game_activity.exe";
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
            RvvmNative.nativePostLifecycleCmd(1); // APP_CMD_INIT_WINDOW
            maybeAutoStartGuest();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "Surface changed: " + width + "x" + height);
        
        // Update the native window
        if (isInitialized) {
            RvvmNative.nativeSetWindow(holder.getSurface());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "Surface destroyed");
        isSurfaceReady = false;
        
        // Clear the native window
        if (isInitialized) {
            RvvmNative.nativeSetWindow(null);
            RvvmNative.nativePostLifecycleCmd(2); // APP_CMD_TERM_WINDOW
        }
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
        if (isInitialized) {
            RvvmNative.nativePostLifecycleCmd(11); // APP_CMD_RESUME
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        // Unregister sensors
        sensorManager.unregisterListener(this);
        if (isInitialized) {
            RvvmNative.nativePostLifecycleCmd(13); // APP_CMD_PAUSE
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isInitialized) {
            RvvmNative.nativePostLifecycleCmd(15); // APP_CMD_DESTROY
        }
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