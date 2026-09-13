package com.rvvm.android;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
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
import android.widget.ViewFlipper;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;

/**
 * Main Activity for RVVM Android host app.
 * This activity manages the RVVM process: guest launch, surface and console.
 * Sensors are not handled here - the native sensor backend (vp_sensor_android.c)
 * owns the platform ASensorManager and feeds the guest directly, so no sensor
 * data crosses Java.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback2 {

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

    private TextView statusText;
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private TextureView ttyView;          // Console tab render target
    private ViewFlipper viewFlipper;
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

    // ---- TTY console (TextureView tab) ----
    // One cell = 4 ints from nativeTtySnapshot: [0] UCS-4 cp, [1] fg ARGB,
    // [2] bg ARGB, [3] flags (bit0 bold, bit1 underline, bit2 reverse,
    // bit3 wide). Drawn with a monospace Paint; nativeTtySerial() gates
    // re-snapshotting, so idle output costs nothing but a compare.
    private static final int TTY_ROWS = 24;
    private static final int TTY_COLS = 80;
    private static final int TTY_CELL = 4;
    private static final float TTY_PAD = 8f;
    private final int[] ttyCells = new int[TTY_ROWS * TTY_COLS * TTY_CELL];
    private int ttySerial = -1;
    private volatile boolean ttyRunning = false;
    private final Paint ttyTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ttyBgPaint = new Paint();
    private final Paint ttyUnderlinePaint = new Paint();

    // Terminal font. Typeface.MONOSPACE is only a generic family: on plenty of
    // devices it is mapped to a proportional face (or resolves per character
    // through fallback), which is exactly what made the console look ragged and
    // smeared on some phones and fine on others. JetBrains Mono is bundled as
    // an asset so every device renders the identical grid; the system monospace
    // files below are only a ladder for the case the asset is missing.
    private static final String TTY_FONT_ASSET = "fonts/JetBrainsMono-Regular.ttf";
    private static final String[] TTY_FONT_FILES = {
            "/system/fonts/NotoSansMono-Regular.ttf",
            "/system/fonts/DroidSansMono.ttf",
            "/system/fonts/CutiveMono.ttf",
            "/system/fonts/DejaVuSansMono.ttf",
    };
    private Typeface ttyFont;                               // resolved on first use
    private final HashMap<Integer, Float> ttyGlyphWidths = new HashMap<>();
    private float ttyGlyphWidthSize = -1f;                  // size the cache is for
    private final char[] ttyGlyph = new char[2];            // one code point, no alloc

    // 30 Hz poll: snapshot + redraw only when the native serial changed.
    private final Runnable ttyTick = new Runnable() {
        @Override public void run() {
            if (!ttyRunning) return;
            if (ttyView != null && ttyView.isAvailable()) {
                int serial = RvvmNative.nativeTtySerial();
                if (serial != ttySerial) {
                    ttySerial = serial;
                    drawTty();
                }
            }
            ttyView.postDelayed(this, 33);
        }
    };

    private final TextureView.SurfaceTextureListener ttyTextureListener =
            new TextureView.SurfaceTextureListener() {
        @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
            startTtyLoop();
        }
        @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {
            if (ttyRunning) drawTty();
        }
        @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
            stopTtyLoop();
            return true;
        }
        @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {}
    };

    private void startTtyLoop() {
        if (ttyRunning) return;
        ttyRunning = true;
        ttyTextPaint.setTypeface(ttyFont());
        // Hinting on, subpixel accumulation off: the grid snaps every origin to
        // a whole pixel (see drawTty), so stems land on pixel boundaries - the
        // fractional size/advance left in place before is what blurred them.
        ttyTextPaint.setAntiAlias(true);
        ttyTextPaint.setHinting(Paint.HINTING_ON);
        ttyTextPaint.setSubpixelText(false);
        ttyUnderlinePaint.setStyle(Paint.Style.STROKE);
        ttySerial = -1;
        ttyView.post(ttyTick);
    }

    /** Terminal font, resolved on first use (the asset load needs the Activity). */
    private Typeface ttyFont() {
        if (ttyFont == null) {
            ttyFont = loadTtyFont();
        }
        return ttyFont;
    }

    /** Bundled asset first, then known system monospace files, then the generic
     *  family. Every candidate is measured before it is accepted. */
    private Typeface loadTtyFont() {
        Typeface asset = loadTtyFontAsset(TTY_FONT_ASSET);
        if (asset != null && isMonospaced(asset)) {
            return asset;
        }
        Typeface first = asset;
        for (String path : TTY_FONT_FILES) {
            Typeface tf = loadTtyFontFile(path);
            if (tf == null) {
                continue;
            }
            if (isMonospaced(tf)) {
                return tf;
            }
            if (first == null) {
                first = tf;
            }
        }
        Typeface generic = Typeface.MONOSPACE;
        return isMonospaced(generic) ? generic : (first != null ? first : generic);
    }

    private Typeface loadTtyFontAsset(String name) {
        try {
            return Typeface.createFromAsset(getAssets(), name);
        } catch (Exception e) {
            Log.w(TAG, "No bundled TTY font " + name + ": " + e);
            return null;
        }
    }

    private static Typeface loadTtyFontFile(String path) {
        if (!new File(path).isFile()) {
            return null;
        }
        try {
            return Typeface.createFromFile(path);
        } catch (Exception e) {
            return null;
        }
    }

    /** True when the probe glyphs all share one advance: catches OEM "monospace"
     *  mappings that really are proportional fonts. */
    private static boolean isMonospaced(Typeface tf) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setTypeface(tf);
        p.setTextSize(100f);
        float ref = p.measureText("M");
        if (ref <= 0f) {
            return false;
        }
        for (String probe : new String[]{"i", "l", ".", "W", "0", "g", " "}) {
            if (Math.abs(p.measureText(probe) - ref) > ref * 0.01f) {
                return false;
            }
        }
        return true;
    }

    private void stopTtyLoop() {
        ttyRunning = false;
        if (ttyView != null) ttyView.removeCallbacks(ttyTick);
    }

    /** Snapshot the native TTY and render the grid onto the TextureView. */
    private void drawTty() {
        if (ttyView.getSurfaceTexture() == null) return;
        if (RvvmNative.nativeTtySnapshot(ttyCells) <= 0) return;

        Canvas canvas = ttyView.lockCanvas(null);
        if (canvas == null) return;
        try {
            int w = canvas.getWidth(), h = canvas.getHeight();
            canvas.drawColor(0xFF000000);

            /* Fit the 80x24 grid by measuring the real monospace advance.
             * At the 100px probe size the advance is adv100, so the size
             * that fits TTY_COLS cells is 100 * availW / (cols * adv100);
             * the height cap works directly on lineH = 1.2 * size. The size
             * is then floored to a whole pixel - a fractional text size is
             * what smeared the glyphs on low-density screens. */
            ttyTextPaint.setTextSize(100f);
            float adv100 = ttyTextPaint.measureText("M");
            float maxByWidth = 100f * (w - 2 * TTY_PAD) / (TTY_COLS * adv100);
            float maxByHeight = (h - 2 * TTY_PAD) / (TTY_ROWS * 1.2f);
            float size = (float) Math.floor(Math.min(100f, Math.min(maxByWidth, maxByHeight)));
            size = Math.max(size, 9f);
            ttyTextPaint.setTextSize(size);
            if (ttyGlyphWidthSize != size) {
                ttyGlyphWidths.clear();
                ttyGlyphWidthSize = size;
            }

            // Whole-pixel grid: cellW may differ from the raw advance by a
            // fraction, so glyphs are centered in their cell instead of being
            // appended to a run - that keeps the columns exact even if the
            // font that answered is not really monospaced.
            float cellW = Math.max(1f, (float) Math.round(ttyTextPaint.measureText("M")));
            float cellH = (float) Math.round(size * 1.2f);
            float gridW = cellW * TTY_COLS, gridH = cellH * TTY_ROWS;
            float ox = (float) Math.floor((w - gridW) / 2f);
            float oy = (float) Math.floor((h - gridH) / 2f);

            /* TEMP DIAG: one line per repaint, so the frame the renderer was
             * asked to draw can be matched against the native screen dump. */
            Log.i(TAG, "tty draw: canvas " + w + "x" + h + " size=" + size +
                    " cell=" + cellW + "x" + cellH + " origin=(" + ox + "," + oy +
                    ") grid=" + gridW + "x" + gridH + " serial=" + ttySerial);

            Paint.FontMetrics fm = ttyTextPaint.getFontMetrics();
            float baselineOff = (float) Math.round((cellH - (fm.descent - fm.ascent)) / 2f - fm.ascent);

            ttyUnderlinePaint.setStyle(Paint.Style.STROKE);
            ttyUnderlinePaint.setStrokeWidth(Math.max(1f, size / 14f));

            for (int r = 0; r < TTY_ROWS; r++) {
                float y0 = oy + r * cellH;
                float baseline = y0 + baselineOff;

                // Background: merged per run of equal colour, black skipped.
                int bgStart = -1, bgColor = 0;
                for (int c = 0; c <= TTY_COLS; c++) {
                    int bg = (c < TTY_COLS) ? ttyCells[(r * TTY_COLS + c) * TTY_CELL + 2] : 0;
                    boolean painted = c < TTY_COLS && (bg & 0x00FFFFFF) != 0;
                    if (!painted || (bgStart >= 0 && bg != bgColor)) {
                        if (bgStart >= 0) {
                            ttyBgPaint.setColor(bgColor);
                            canvas.drawRect(ox + bgStart * cellW, y0,
                                    ox + c * cellW, y0 + cellH, ttyBgPaint);
                            bgStart = -1;
                        }
                    }
                    if (painted && bgStart < 0) {
                        bgStart = c;
                        bgColor = bg;
                    }
                }

                // Glyphs: one draw per cell. A missing glyph still falls back
                // to a proportional face, so each one is placed on the grid -
                // centered when it fits, horizontally squeezed when it does not
                // (wide/CJK and fallback glyphs) - rather than appended to a run.
                for (int c = 0; c < TTY_COLS; c++) {
                    int i = (r * TTY_COLS + c) * TTY_CELL;
                    int cp = ttyCells[i];
                    if (cp <= 0 || cp == (int) ' ' || cp > Character.MAX_CODE_POINT
                            || (cp >= Character.MIN_SURROGATE && cp <= Character.MAX_SURROGATE)) {
                        continue;   // blank cell or an unpaired surrogate
                    }
                    int flags = ttyCells[i + 3];
                    boolean wide = (flags & 8) != 0 && c + 1 < TTY_COLS;
                    int len = Character.toChars(cp, ttyGlyph, 0);

                    float target = cellW * (wide ? 2 : 1);
                    float gw = ttyGlyphWidths.containsKey(cp)
                            ? ttyGlyphWidths.get(cp)
                            : measureGlyph(cp, len);
                    float x = ox + c * cellW;
                    if (gw > target && gw > 0f) {
                        ttyTextPaint.setTextScaleX(target / gw);
                    } else if (gw > 0f) {
                        x += (target - gw) / 2f;   // zero-width: keep the origin
                    }

                    ttyTextPaint.setColor(ttyCells[i + 1]);
                    ttyTextPaint.setFakeBoldText((flags & 1) != 0);
                    canvas.drawText(ttyGlyph, 0, len, x, baseline, ttyTextPaint);
                    ttyTextPaint.setTextScaleX(1f);   // next measure must be unscaled

                    if ((flags & 2) != 0) {
                        ttyUnderlinePaint.setColor(ttyCells[i + 1]);
                        canvas.drawLine(x, y0 + cellH - 1f,
                                x + target, y0 + cellH - 1f, ttyUnderlinePaint);
                    }
                    if (wide) {
                        c++;    // the wide glyph's filler cell
                    }
                }
            }
        } finally {
            ttyView.unlockCanvasAndPost(canvas);
        }
    }

    /** Natural advance of one code point at the current text size, cached:
     *  the grid is redrawn on every guest output burst, and measureText is the
     *  expensive half of the render. Text scale must be 1 here. */
    private float measureGlyph(int cp, int len) {
        float w = ttyTextPaint.measureText(ttyGlyph, 0, len);
        ttyGlyphWidths.put(cp, w);
        return w;
    }

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
        surfaceView = findViewById(R.id.surfaceView);
        logOverlayScroll = findViewById(R.id.logOverlayScroll);
        logOverlayVScroll = findViewById(R.id.logOverlayVScroll);
        logOverlayText = findViewById(R.id.logOverlayText);
        // The XML monospace attribute has the same OEM problem as the TTY
        // canvas did; the resolved terminal font is used for both.
        logOverlayText.setTypeface(ttyFont());

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

        // Tab switcher: graphics (SurfaceView) vs TTY console (TextureView)
        ViewFlipper flipper = findViewById(R.id.viewFlipper);
        Button tabGraphics = findViewById(R.id.tabGraphicsButton);
        Button tabConsole = findViewById(R.id.tabConsoleButton);
        tabGraphics.setOnClickListener(v -> flipper.setDisplayedChild(0));
        tabConsole.setOnClickListener(v -> flipper.setDisplayedChild(1));
        ttyView = findViewById(R.id.ttyView);
        ttyView.setSurfaceTextureListener(ttyTextureListener);

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
        postLifecycleCmd(APP_CMD_RESUME);
    }

    @Override
    protected void onPause() {
        super.onPause();
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