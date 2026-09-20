package com.rvvm.android;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.HashMap;
import java.util.Locale;

/**
 * Guest Activity: one Activity per running app.
 *
 * <p>Lifecycle model: <b>run / suspend / resume</b>.
 * <ul>
 *   <li>{@code onCreate}  → host.acquire + create guest + copy ELF</li>
 *   <li>{@code onResume}  → resume from suspension (if any) + INIT_WINDOW +
 *       RESUME + GAINED_FOCUS</li>
 *   <li>{@code onPause}   → PAUSE + LOST_FOCUS + suspend (park vCPUs) +
 *       tear down surface</li>
 *   <li>{@code onStop}    → no-op (guest stays suspended)</li>
 *   <li>{@code onDestroy} → resume if parked + nativeStopGuest + wait for
 *       exit + host.release</li>
 * </ul>
 * The guest is only fully stopped in {@code onDestroy}. Between pause and
 * resume the guest stays alive with its vCPUs parked — backgrounding and
 * switching away does not kill it.</p>
 *
 * <p>Surface management: when the Activity pauses, we wait for the guest's
 * vCPUs to park ({@link RvvmNative#nativeIsGuestParked(int)}) before
 * tearing down the surface. This ensures the guest doesn't write to a
 * destroyed buffer. When resumed, the surface is recreated and the guest
 * rebuilds its EGL context.</p>
 *
 * <p>The console and its keyboard remain on the hosting MainActivity
 * screen; this Activity only displays the guest's graphics and its own
 * TTY console overlay.</p>
 */
public class GuestActivity extends Activity {
    private static final String TAG = "RVVM-GuestActivity";

    public static final String EXTRA_APP_NAME = "guest_app_name";

    /** Guest exit behavior: "finish" closes the activity when the guest exits,
     *  any other value (e.g. "stay") keeps it up so the user can read the
     *  console / last screen. */
    public static final String EXTRA_AFTER_GUEST_EXIT = "after_guest_exit";
    public static final String AFTER_GUEST_EXIT_FINISH = "finish";
    public static final String AFTER_GUEST_EXIT_STAY = "stay";

    private static final int APP_CMD_INIT_WINDOW = 1;
    private static final int APP_CMD_TERM_WINDOW = 2;
    private static final int APP_CMD_WINDOW_RESIZED = 3;
    private static final int APP_CMD_WINDOW_REDRAW_NEEDED = 4;
    private static final int APP_CMD_GAINED_FOCUS = 6;
    private static final int APP_CMD_LOST_FOCUS = 7;
    private static final int APP_CMD_START = 10;
    private static final int APP_CMD_RESUME = 11;
    private static final int APP_CMD_PAUSE = 13;
    private static final int APP_CMD_STOP = 14;
    private static final int APP_CMD_DESTROY = 15;

    // intent paremeter
    private String afterGuestExit="finish";

    // Guest state
    private int guestId = -1;
    private String appName;
    private String elfPath;
    private boolean isSurfaceReady = false;
    private boolean isGuestStarted = false;
    private boolean isInitialized = false;
    private volatile boolean isSuspended = false;
    /** onResume found the surface still down: hold the resume until
     *  surfaceCreated binds the window (surface first, then guest). */
    private boolean pendingResume = false;

    // Surface
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private SurfaceHolder.Callback2 surfaceCallback;
    private TextureView consoleView;  // optional TTY overlay
    private FitFrameLayout videoArea; // letterbox container
    private FrameLayout consoleContainer; // holds consoleView + ttyInput
    private TtyEditText ttyInput;     // keyboard/IME focus target
    private TextView statusBar;       // floating overlay for status messages

    // Console
    private int[] ttyCells;
    private int ttyRows = 24;
    private int ttySerial = -1;
    private boolean ttyRunning = false;
    /** Tracks the TTY soft keyboard state; imm.isActive() only reflects view
     *  focus, so we maintain our own flag to toggle reliably. */
    private boolean ttyKeyboardVisible = false;

    // TTY paints
    private final Paint ttyTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ttyBgPaint = new Paint();
    private final Paint ttyCursorPaint = new Paint();
    private final Paint ttyUnderlinePaint = new Paint();
    private final Paint ttyScrollPaint = new Paint();

    // Terminal font
    private static final String TTY_FONT_ASSET = "fonts/JetBrainsMono-Regular.ttf";
    private static final String[] TTY_FONT_FILES = {
            "/system/fonts/NotoSansMono-Regular.ttf",
            "/system/fonts/DroidSansMono.ttf",
            "/system/fonts/CutiveMono.ttf",
            "/system/fonts/DejaVuSansMono.ttf",
    };
    private Typeface ttyFont;
    private final HashMap<Integer, Float> ttyGlyphWidths = new HashMap<>();
    private float ttyGlyphWidthSize = -1f;
    private final char[] ttyGlyph = new char[2];

    // Frame callback (receives onFirstFrame to reveal the video layer)
    private RvvmNative.FrameCallback frameCallback;

    // Log file
    private BufferedWriter logWriter;
    private File logFile;
    private final Object logFileLock = new Object();

    // Panel size from host
    private static final int PANEL_W = 1280;
    private static final int PANEL_H = 720;

    // TTY constants
    private static final int TTY_COLS = 80;
    private static final int TTY_CELL = 4;
    private static final int TTY_MIN_ROWS = 8;
    private static final int TTY_MAX_ROWS = 200;
    private static final float TTY_PAD = 8f;

    // Cell flags
    private static final int TTY_FLAG_BOLD      = 1;
    private static final int TTY_FLAG_UNDERLINE = 1 << 1;
    private static final int TTY_FLAG_REVERSE   = 1 << 2;
    private static final int TTY_FLAG_WIDE      = 1 << 3;
    private static final int TTY_FLAG_CURSOR    = 1 << 4;

    // Cursor blink
    private static final long TTY_BLINK_MS = 500;
    private boolean ttyCursorOn = true;
    private long ttyBlinkNext = 0;

    // Scrollback
    private final int[] ttyScrollInfo = new int[2];
    private float ttyCellH = 24f;
    private float ttyScrollRest = 0f; // sub-line drag remainder, see onScroll

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Get the app name from the intent
        Intent intent = getIntent();
        appName = intent.getStringExtra(EXTRA_APP_NAME);
        if (appName == null || appName.isEmpty()) {
            appName = getIntent().getAction();
        }
        if (appName == null) {
            appName = "unknown";
        }

        // Exit behavior when the guest run ends (default: finish this activity)
        afterGuestExit = intent.getStringExtra(EXTRA_AFTER_GUEST_EXIT);
        if (afterGuestExit == null || afterGuestExit.isEmpty()) {
            afterGuestExit = AFTER_GUEST_EXIT_FINISH;
        }

        // Create the layout
        setContentView(R.layout.activity_guest);

        // Initialize surface and console views
        surfaceView = findViewById(R.id.surfaceView);
        consoleView = findViewById(R.id.consoleView);
        videoArea = findViewById(R.id.videoArea);
        consoleContainer = findViewById(R.id.consoleContainer);

        // Letterbox: fit the surface to the panel's aspect ratio, never stretch.
        // Same approach as GlWindowCard — the black bars are this container.
        videoArea.setAspectRatio((float) PANEL_W / PANEL_H);

        // Consume touches while the video is visible so taps/swipes on the
        // guest graphics never fall through to the console gesture handler
        // (which would toggle the TTY keyboard underneath).
        videoArea.setClickable(true);

        // Hidden until the guest's first frame — same content-driven
        // visibility as GlWindowCard: the surface stays alive at 1x1 but
        // nothing is shown until the guest has something to draw.
        videoArea.setVisibility(View.INVISIBLE);

        // Status text lives in the layout at the key bar's slot; it replaces
        // the key bar when the guest exits in stay-open mode.
        statusBar = findViewById(R.id.ttyStatusText);

        // Acquire the process-wide native host (ref-counted; safe if already acquired)
        RvvmHost host = RvvmHost.getInstance();
        host.acquire();

        // Get notified when this guest's run ends (fired on a vCPU thread)
        host.setExitListener((gid, code) -> {
            if (gid == guestId) {
                runOnUiThread(() -> handleGuestExit(code));
            }
        });

        // Pin the virtual panel before the guest observes a window geometry
        host.setPanelSize(PANEL_W, PANEL_H);

        // Push display config
        pushDisplayConfig();

        isInitialized = true;

        // Copy the ELF from assets to internal storage
        File elfFile = new File(getFilesDir(), appName);
        try {
            copyAssetToFile(appName, elfFile);
            elfPath = elfFile.getAbsolutePath();
        } catch (IOException e) {
            Log.e(TAG, "Failed to copy ELF: " + appName, e);
            Toast.makeText(this, "Failed to copy ELF: " + e.getMessage(),
                    Toast.LENGTH_SHORT).show();
            finish();
            return;
        }

        // Create the guest (native)
        guestId = RvvmNative.nativeCreateGuest();
        if (guestId < 0) {
            Toast.makeText(this, "No guest slot available", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }

        // Set the active guest
        RvvmNative.nativeSetActiveGuest(guestId);

        // Set up the surface callback
        setupSurfaceCallback();

        // Open log file
        openLogFile(appName);

        // Set up console
        setupConsole();
        initTtyInput();

        // Register frame callback: onFirstFrame reveals the video layer
        frameCallback = guestId -> runOnUiThread(() -> {
            if (videoArea != null && videoArea.getVisibility() != View.VISIBLE) {
                videoArea.setVisibility(View.VISIBLE);
            }
        });
        RvvmNative.nativeSetFrameCallback(frameCallback);

        Log.i(TAG, "GuestActivity created for " + appName
                + " (guestId=" + guestId + ", elf=" + elfPath + ")");
    }

    /**
     * Re-entry through the launcher: documentLaunchMode="intoExisting" brings
     * the existing task forward instead of starting a new guest run. The
     * suspended (or running) guest keeps its state; onResume() resumes it.
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        String incoming = intent.getStringExtra(EXTRA_APP_NAME);
        if (incoming != null && !incoming.equals(appName)) {
            // intoExisting matches the intent action (the app name), so this
            // should not happen; keep the live guest over a silent swap.
            Log.w(TAG, "Re-entry intent names a different app: " + incoming
                    + " (running " + appName + ")");
        }
    }

    /** Terminal font, resolved on first use. */
    private Typeface ttyFont() {
        if (ttyFont == null) {
            ttyFont = loadTtyFont();
        }
        return ttyFont;
    }

    private Typeface loadTtyFont() {
        Typeface asset = loadTtyFontAsset(TTY_FONT_ASSET);
        if (asset != null && isMonospaced(asset)) {
            return asset;
        }
        Typeface first = asset;
        for (String path : TTY_FONT_FILES) {
            Typeface tf = loadTtyFontFile(path);
            if (tf == null) continue;
            if (isMonospaced(tf)) return tf;
            if (first == null) first = tf;
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
        if (!new File(path).isFile()) return null;
        try {
            return Typeface.createFromFile(path);
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean isMonospaced(Typeface tf) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setTypeface(tf);
        p.setTextSize(100f);
        float ref = p.measureText("M");
        if (ref <= 0f) return false;
        for (String probe : new String[]{"i", "l", ".", "W", "0", "g", " "}) {
            if (Math.abs(p.measureText(probe) - ref) > ref * 0.01f) return false;
        }
        return true;
    }

    /** Natural advance of one code point at the current text size, cached. */
    private float measureGlyph(int cp, int len) {
        float w = ttyTextPaint.measureText(ttyGlyph, 0, len);
        ttyGlyphWidths.put(cp, w);
        return w;
    }

    /** Snapshot the native TTY and render the grid onto the TextureView. */
    private void drawTty() {
        if (consoleView == null || consoleView.getSurfaceTexture() == null) return;
        int w = consoleView.getWidth(), h = consoleView.getHeight();
        if (w <= 0 || h <= 0) return;

        ttyTextPaint.setTextSize(100f);
        float adv100 = ttyTextPaint.measureText("M");
        float maxByWidth = 100f * (w - 2 * TTY_PAD) / (TTY_COLS * adv100);
        float maxByHeight = (h - 2 * TTY_PAD) / (TTY_MIN_ROWS * 1.2f);
        float size = (float) Math.floor(Math.min(100f, Math.min(maxByWidth, maxByHeight)));
        size = Math.max(size, 9f);
        ttyTextPaint.setTextSize(size);
        if (ttyGlyphWidthSize != size) {
            ttyGlyphWidths.clear();
            ttyGlyphWidthSize = size;
        }

        float cellW = Math.max(1f, (float) Math.round(ttyTextPaint.measureText("M")));
        float cellH = (float) Math.round(size * 1.2f);

        int rows = (int) ((h - 2 * TTY_PAD) / cellH);
        if (rows < TTY_MIN_ROWS) rows = TTY_MIN_ROWS;
        if (rows > TTY_MAX_ROWS) rows = TTY_MAX_ROWS;
        if (rows != ttyRows) {
            ttyRows = rows;
            RvvmNative.nativeTtyResize(rows, TTY_COLS);
        }

        if (RvvmNative.nativeTtySnapshot(guestId, ttyCells, ttyScrollInfo) <= 0) return;

        Canvas canvas = consoleView.lockCanvas(null);
        if (canvas == null) return;
        try {
            canvas.drawColor(0xFF000000);
            float gridW = cellW * TTY_COLS, gridH = cellH * rows;
            ttyCellH = cellH;
            float ox = (float) Math.floor((w - gridW) / 2f);
            float oy = (float) Math.floor((h - gridH) / 2f);

            int curRow = -1, curCol = -1;
            for (int r = 0; r < rows && curRow < 0; r++) {
                for (int c = 0; c < TTY_COLS; c++) {
                    if ((ttyCells[(r * TTY_COLS + c) * TTY_CELL + 3] & TTY_FLAG_CURSOR) != 0) {
                        curRow = r;
                        curCol = c;
                        break;
                    }
                }
            }

            Paint.FontMetrics fm = ttyTextPaint.getFontMetrics();
            float baselineOff = (float) Math.round((cellH - (fm.descent - fm.ascent)) / 2f - fm.ascent);

            ttyUnderlinePaint.setStyle(Paint.Style.STROKE);
            ttyUnderlinePaint.setStrokeWidth(Math.max(1f, size / 14f));

            for (int r = 0; r < rows; r++) {
                float y0 = oy + r * cellH;
                float baseline = y0 + baselineOff;

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

                if (r == curRow && ttyCursorOn) {
                    int i = (curRow * TTY_COLS + curCol) * TTY_CELL;
                    boolean cursorWide = (ttyCells[i + 3] & TTY_FLAG_WIDE) != 0
                            && curCol + 1 < TTY_COLS;
                    ttyCursorPaint.setColor(ttyCells[i + 1]);
                    float cx = ox + curCol * cellW;
                    canvas.drawRect(cx, y0, cx + (cursorWide ? 2f : 1f) * cellW,
                            y0 + cellH, ttyCursorPaint);
                }

                for (int c = 0; c < TTY_COLS; c++) {
                    int i = (r * TTY_COLS + c) * TTY_CELL;
                    int cp = ttyCells[i];
                    if (cp <= 0 || cp == (int) ' ' || cp > Character.MAX_CODE_POINT
                            || (cp >= Character.MIN_SURROGATE && cp <= Character.MAX_SURROGATE)) {
                        continue;
                    }
                    int flags = ttyCells[i + 3];
                    boolean wide = (flags & TTY_FLAG_WIDE) != 0 && c + 1 < TTY_COLS;
                    boolean onCursor = ttyCursorOn && r == curRow && c == curCol;
                    int len = Character.toChars(cp, ttyGlyph, 0);

                    float target = cellW * (wide ? 2 : 1);
                    float gw = ttyGlyphWidths.containsKey(cp)
                            ? ttyGlyphWidths.get(cp) : measureGlyph(cp, len);
                    float x = ox + c * cellW;
                    if (gw > target && gw > 0f) {
                        ttyTextPaint.setTextScaleX(target / gw);
                    } else if (gw > 0f) {
                        x += (target - gw) / 2f;
                    }

                    ttyTextPaint.setColor(onCursor ? ttyCells[i + 2] : ttyCells[i + 1]);
                    ttyTextPaint.setFakeBoldText((flags & TTY_FLAG_BOLD) != 0);
                    canvas.drawText(ttyGlyph, 0, len, x, baseline, ttyTextPaint);
                    ttyTextPaint.setTextScaleX(1f);

                    if ((flags & TTY_FLAG_UNDERLINE) != 0) {
                        ttyUnderlinePaint.setColor(onCursor ? ttyCells[i + 2] : ttyCells[i + 1]);
                        canvas.drawLine(x, y0 + cellH - 1f,
                                x + target, y0 + cellH - 1f, ttyUnderlinePaint);
                    }
                    if (wide) c++;
                }
            }

            int sbLines = ttyScrollInfo[1], sbScroll = ttyScrollInfo[0];
            if (sbLines > 0) {
                float span = sbLines + rows;
                float barW = Math.max(2f, TTY_PAD / 3f);
                float thumbH = Math.max(cellH, gridH * rows / span);
                float thumbY = oy + gridH * (sbLines - sbScroll) / span;
                ttyScrollPaint.setColor(sbScroll > 0 ? 0xB0FFFFFF : 0x38FFFFFF);
                canvas.drawRect(ox + gridW + barW, thumbY,
                        ox + gridW + 2f * barW, thumbY + thumbH, ttyScrollPaint);
            }
        } finally {
            consoleView.unlockCanvasAndPost(canvas);
        }
    }

    /** Set up the SurfaceHolder callback for surface lifecycle events. */
    private void setupSurfaceCallback() {
        surfaceHolder = surfaceView.getHolder();
        surfaceCallback = new SurfaceHolder.Callback2() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Log.i(TAG, "Surface created for guest " + guestId);
                isSurfaceReady = true;
                RvvmNative.nativeSetWindow(holder.getSurface(), guestId);
                postLifecycleCmd(APP_CMD_INIT_WINDOW);
                // A resume held back in onResume (surface was still down):
                // replay the lifecycle and unpark now that the window exists.
                if (pendingResume) {
                    pendingResume = false;
                    postLifecycleCmd(APP_CMD_RESUME);
                    postLifecycleCmd(APP_CMD_GAINED_FOCUS);
                    if (isSuspended && guestId >= 0) {
                        RvvmNative.nativeResumeGuest(guestId);
                        isSuspended = false;
                        Log.i(TAG, "Guest " + appName + " resumed with its surface");
                    }
                }
                // Start the guest now that the surface is ready
                startGuestIfNeeded();
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.i(TAG, "Surface changed for guest " + guestId + ": " + width + "x" + height);
                if (isSurfaceReady && !isSuspended) {
                    RvvmNative.nativeSetWindow(holder.getSurface(), guestId);
                    postLifecycleCmd(APP_CMD_WINDOW_RESIZED);
                }
                // videoArea visibility is now driven by FrameCallback.onFirstFrame()
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "Surface destroyed for guest " + guestId);
                isSurfaceReady = false;
                videoArea.setVisibility(View.INVISIBLE);
                RvvmNative.nativeSetWindow(null, guestId);
                postLifecycleCmd(APP_CMD_TERM_WINDOW);
            }

            @Override
            public void surfaceRedrawNeeded(SurfaceHolder holder) {
                postLifecycleCmd(APP_CMD_WINDOW_REDRAW_NEEDED);
            }
        };
        surfaceHolder.addCallback(surfaceCallback);
    }

    /** Set up the TTY console. */
    private void setupConsole() {
        ttyCells = new int[TTY_MAX_ROWS * TTY_COLS * TTY_CELL];
        ttySerial = -1;
        ttyCursorOn = true;
        ttyBlinkNext = SystemClock.uptimeMillis() + TTY_BLINK_MS;
        ttyTextPaint.setTypeface(ttyFont());
        ttyTextPaint.setAntiAlias(true);
        ttyTextPaint.setHinting(Paint.HINTING_ON);
        ttyTextPaint.setSubpixelText(false);
        ttyUnderlinePaint.setStyle(Paint.Style.STROKE);
    }

    /**
     * Start the guest ELF when the surface is ready. Mirrors the startup
     * sequence from MainActivity.replayGuestStartupState() + startRun():
     * clear any stale lifecycle commands, deliver START -> RESUME -> INIT_WINDOW
     * -> GAINED_FOCUS, then hand the ELF to native.
     */
    private void startGuestIfNeeded() {
        if (isGuestStarted || !isInitialized || !isSurfaceReady || elfPath == null) {
            return;
        }
        if (guestId < 0) {
            return;
        }

        // Re-deliver the lifecycle state a freshly started guest expects to see
        RvvmNative.nativeClearLifecycleCmds(guestId);
        postLifecycleCmd(APP_CMD_START);
        postLifecycleCmd(APP_CMD_RESUME);
        postLifecycleCmd(APP_CMD_INIT_WINDOW);
        if (hasWindowFocus()) {
            postLifecycleCmd(APP_CMD_GAINED_FOCUS);
        }

        // Actually run the ELF
        boolean started = RvvmNative.nativeRunElf(guestId, elfPath, null);
        if (started) {
            isGuestStarted = true;
            Log.i(TAG, "Guest started: " + appName + " (guestId=" + guestId + ")");
        } else {
            Log.e(TAG, "Failed to start guest: " + appName);
            Toast.makeText(this, "Failed to start guest", Toast.LENGTH_SHORT).show();
        }
    }

    /** Push the real device configuration to native. */
    private void pushDisplayConfig() {
        Configuration config = getResources().getConfiguration();
        int screenLayout = config.screenLayout;

        int longMode = (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK)
                == Configuration.SCREENLAYOUT_LONG_YES ? 2
                : (screenLayout & Configuration.SCREENLAYOUT_LONG_MASK)
                == Configuration.SCREENLAYOUT_LONG_NO ? 1 : 0;
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

    /** Copy an asset file to internal storage. */
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

    /** Open a log file for this guest run. */
    private void openLogFile(String guestName) {
        synchronized (logFileLock) {
            try {
                File dir = new File(getFilesDir(), "logs");
                if (!dir.exists() && !dir.mkdirs()) {
                    return;
                }
                String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss",
                        Locale.US).format(new Date());
                String base = guestName.endsWith(".exe")
                        ? guestName.substring(0, guestName.length() - 4) : guestName;
                logFile = new File(dir, base + "-" + stamp + ".log");
                logWriter = new BufferedWriter(new FileWriter(logFile));
            } catch (IOException e) {
                logWriter = null;
                logFile = null;
            }
        }
    }

    /** Post a lifecycle command to the guest. */
    private void postLifecycleCmd(int cmd) {
        if (guestId < 0) return;
        RvvmNative.nativePostLifecycleCmd(guestId, cmd);
    }

    /**
     * Called when the Activity is resumed. The guest is resumed from
     * suspension and the surface is recreated if needed.
     */
    @Override
    protected void onResume() {
        super.onResume();

        if (guestId < 0) return;

        // Reveal the video layer for a live, already-started guest. The
        // first-frame reveal fired once per run and is already consumed, so
        // waiting for it on re-entry would leave an INVISIBLE SurfaceView -
        // which gets no surface at all, and the guest would run windowless
        // (every frame's lock failing).
        if (isGuestStarted && RvvmNative.nativeIsGuestRunning(guestId)) {
            videoArea.setVisibility(View.VISIBLE);
        }

        // If surface was destroyed while paused, recreate it here
        if (!isSurfaceReady && surfaceHolder.getSurface().isValid()) {
            surfaceCallback.surfaceCreated(surfaceHolder);
        }

        if (isSurfaceReady) {
            // Window up: rebind and resume right away
            RvvmNative.nativeSetWindow(surfaceHolder.getSurface(), guestId);
            postLifecycleCmd(APP_CMD_INIT_WINDOW);
            postLifecycleCmd(APP_CMD_RESUME);
            postLifecycleCmd(APP_CMD_GAINED_FOCUS);
            if (isSuspended) {
                RvvmNative.nativeResumeGuest(guestId);
                isSuspended = false;
                Log.i(TAG, "Guest " + appName + " resumed from suspension");
            }
        } else if (isSuspended && isGuestStarted
                && RvvmNative.nativeIsGuestRunning(guestId)) {
            // Surface still down: the framework recreates it asynchronously.
            // Hold the resume until surfaceCreated binds the window, so the
            // guest never runs windowless and no frame's lock can fail - the
            // same hold-back MainActivity applies to a run waiting for its
            // card. Guest stays parked until then.
            pendingResume = true;
        }

        // Try starting the guest if surface became ready before onResume
        startGuestIfNeeded();

        // Start the console loop
        startTtyLoop();

        Log.i(TAG, "Guest " + appName + " resumed (guestId=" + guestId + ")");
    }

    /**
     * Called when the Activity is paused. The guest is suspended (vCPUs
     * parked) and the surface is torn down. The guest remains alive and
     * resumes from suspension in onResume.
     */
    @Override
    protected void onPause() {
        super.onPause();

        if (guestId < 0) return;

        // Stop the console loop
        stopTtyLoop();

        // The keyboard is dismissed along with the activity's input state
        ttyKeyboardVisible = false;

        // A resume held back for a surface that never came up in this
        // foreground round must not fire on a stale surfaceCreated later.
        pendingResume = false;

        // Notify the guest it is losing focus and pausing
        postLifecycleCmd(APP_CMD_PAUSE);
        postLifecycleCmd(APP_CMD_LOST_FOCUS);

        // Suspend: park the guest's vCPUs so they stop touching the surface
        RvvmNative.nativeSuspendGuest(guestId);
        boolean parked = waitForGuestParked(300);
        isSuspended = true;

        // Tear down the surface while the guest is parked
        if (isSurfaceReady) {
            RvvmNative.nativeSetWindow(null, guestId);
            postLifecycleCmd(APP_CMD_TERM_WINDOW);
            isSurfaceReady = false;
        }

        Log.i(TAG, "Guest " + appName + " paused (guestId=" + guestId + "), parked=" + parked);
    }

    /**
     * Called when the Activity is stopped. The guest stays suspended;
     * it is only stopped in onDestroy. This keeps the run/suspend/resume
     * cycle intact so the guest survives backgrounding.
     */
    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "Guest " + appName + " stopped (still suspended, guestId=" + guestId + ")");
    }

    /**
     * Back does not tear the guest down. Like Home, it backgrounds the whole
     * task: onPause suspends the guest (vCPUs parked) and it stays alive in
     * the process-wide run table, exactly the window-card model. Re-entry
     * through the launcher reuses this task (documentLaunchMode="intoExisting")
     * and onResume resumes the guest where it left off. A guest that has
     * already exited has nothing worth keeping - close the shell instead.
     */
    @Override
    public void onBackPressed() {
        if (guestId >= 0 && RvvmNative.nativeIsGuestRunning(guestId)) {
            moveTaskToBack(true);
        } else {
            super.onBackPressed();
        }
    }

    /**
     * Called when the Activity is destroyed. The guest is stopped and
     * resources are cleaned up. If the guest was suspended (paused),
     * it is resumed first so it can unwind its normal exit path, then
     * stopped. We wait for the guest thread to fully exit before
     * releasing the native host, because android_aaudio_shutdown()
     * (called from nativeDestroy) closes AAudio streams — doing that
     * while the guest is still using them causes SIGSEGV.
     */
    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopTtyLoop();
        closeLogFile();

        if (guestId >= 0) {
            // Resume a suspended guest so it can unwind its exit path
            if (isSuspended && RvvmNative.nativeIsGuestRunning(guestId)) {
                RvvmNative.nativeResumeGuest(guestId);
                isSuspended = false;
            }

            // Kick the guest to exit
            if (RvvmNative.nativeIsGuestRunning(guestId)) {
                RvvmNative.nativeStopGuest(guestId);
            }

            // Wait for the guest thread to fully exit before tearing down
            // the native host (android_aaudio_shutdown races otherwise).
            waitForGuestExit(2000);

            RvvmNative.nativeDestroyGuest(guestId);
            guestId = -1;
        }

        // Release the process-wide native host reference.
        if (isInitialized) {
            RvvmNative.nativeSetFrameCallback(null);
            RvvmHost.getInstance().setExitListener(null);
            RvvmHost.getInstance().release();
            isInitialized = false;
        }

        Log.i(TAG, "GuestActivity destroyed for " + appName);
    }

    /**
     * Block until the guest thread finishes (run->running clears) or the
     * timeout expires. Safe to call when no guest is running.
     */
    private void waitForGuestExit(int timeoutMs) {
        long start = System.currentTimeMillis();
        while (System.currentTimeMillis() - start < timeoutMs) {
            if (!RvvmNative.nativeIsGuestRunning(guestId)) {
                return;
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                return;
            }
        }
        Log.w(TAG, "Guest " + appName + " did not exit within " + timeoutMs + " ms");
    }

    /**
     * Wait for the guest's vCPUs to park (with timeout). Returns true
     * if all vCPUs parked, false if timeout.
     */
    private boolean waitForGuestParked(int timeoutMs) {
        long startTime = System.currentTimeMillis();
        while (System.currentTimeMillis() - startTime < timeoutMs) {
            if (RvvmNative.nativeIsGuestParked(guestId)) {
                return true;
            }
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                return false;
            }
        }
        return false;
    }

    /** Start the TTY console rendering loop. */
    private void startTtyLoop() {
        if (ttyRunning) return;
        ttyRunning = true;
        // Poll at ~30 Hz for console output
        consoleView.postDelayed(ttyTick, 33);
    }

    /** Stop the TTY console rendering loop. */
    private void stopTtyLoop() {
        ttyRunning = false;
        if (consoleView != null) {
            consoleView.removeCallbacks(ttyTick);
        }
    }

    /** TTY rendering tick. */
    private final Runnable ttyTick = new Runnable() {
        @Override
        public void run() {
            if (!ttyRunning) return;
            if (consoleView != null && consoleView.isAvailable()) {
                int serial = RvvmNative.nativeTtySerial(guestId);
                long now = SystemClock.uptimeMillis();
                boolean redraw = serial != ttySerial;
                if (redraw) {
                    Log.i(TAG, "[scroll-dbg] ttyTick serial " + ttySerial + " -> " + serial);
                    ttySerial = serial;
                    ttyCursorOn = true;
                    ttyBlinkNext = now + TTY_BLINK_MS;
                } else if (now >= ttyBlinkNext) {
                    ttyCursorOn = !ttyCursorOn;
                    ttyBlinkNext = now + TTY_BLINK_MS;
                    redraw = true;
                }
                if (redraw) {
                    drawTty();
                }
            }
            consoleView.postDelayed(this, 33);
        }
    };

    // ---- Console keyboard input ----

    private static final byte[] TTY_ENTER = { '\r' };
    private static final byte[] TTY_TAB   = { '\t' };
    private static final byte[] TTY_ESC   = { 0x1B };
    private static final byte[] TTY_BS    = { 0x7F };
    private static final byte[] TTY_DEL   = { 0x1B, '[', '3', '~' };
    private static final byte[] TTY_UP    = { 0x1B, '[', 'A' };
    private static final byte[] TTY_DOWN  = { 0x1B, '[', 'B' };
    private static final byte[] TTY_RIGHT = { 0x1B, '[', 'C' };
    private static final byte[] TTY_LEFT  = { 0x1B, '[', 'D' };

    /** Create the console's keyboard target and wire it up to the guest. */
    private void initTtyInput() {
        if (consoleContainer == null || consoleView == null) return;
        ttyInput = new TtyEditText(this);
        ttyInput.setAlpha(0f);
        ttyInput.setBackground(null);
        ttyInput.setPadding(0, 0, 0, 0);
        ttyInput.setCursorVisible(false);
        ttyInput.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        ttyInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_ACTION_NONE);
        consoleContainer.addView(ttyInput, new FrameLayout.LayoutParams(1, 1));

        ttyInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                if (s.length() > 0) {
                    sendTtyText(s);
                    s.clear();
                }
            }
        });

        // If the keyboard goes away through any other path (back key, IME
        // close, focus stolen), the flag must drop so the next tap can
        // reopen it.
        ttyInput.setOnFocusChangeListener((v, hasFocus) -> {
            if (!hasFocus) ttyKeyboardVisible = false;
        });

        final GestureDetector consoleGesture = new GestureDetector(this,
                new GestureDetector.SimpleOnGestureListener() {
                    @Override
                    public boolean onDown(MotionEvent e) {
                        ttyScrollRest = 0f;
                        Log.i(TAG, "[scroll-dbg] onDown y=" + e.getY()
                                + " ttyReady=" + isTtyReady()
                                + " ttyCellH=" + ttyCellH
                                + " ttySerial=" + ttySerial
                                + " guestStarted=" + isGuestStarted);
                        return true;
                    }

                    @Override
                    public boolean onSingleTapUp(MotionEvent e) {
                        Log.i(TAG, "[scroll-dbg] onSingleTapUp");
                        toggleTtyKeyboard();
                        return true;
                    }

                    @Override
                    public boolean onScroll(MotionEvent e1, MotionEvent e2,
                            float distanceX, float distanceY) {
                        boolean ready = isTtyReady();
                        // GestureDetector's distanceY is lastY - currentY, the
                        // inverse of the drag delta MainActivity uses: negate it
                        // so a downward drag looks back into history there too.
                        // The fraction of a line left over by each callback is
                        // carried in ttyScrollRest - rounding per event would
                        // drop it, and a slow drag would barely move at all.
                        ttyScrollRest += -distanceY;
                        int lines = (int) (ttyScrollRest / ttyCellH);
                        Log.i(TAG, "[scroll-dbg] onScroll dy=" + distanceY
                                + " ttyCellH=" + ttyCellH
                                + " lines=" + lines
                                + " ttyReady=" + ready
                                + " scrollInfo=" + ttyScrollInfo[0] + "/" + ttyScrollInfo[1]);
                        if (!ready || guestId < 0) return false;
                        if (lines != 0) {
                            ttyScrollRest -= lines * ttyCellH;
                            RvvmNative.nativeTtyScrollBy(guestId, lines);
                            Log.i(TAG, "[scroll-dbg] nativeTtyScrollBy(" + guestId + "," + lines + ")");
                        }
                        return true;
                    }

                    @Override
                    public boolean onFling(MotionEvent e1, MotionEvent e2,
                            float velocityX, float velocityY) {
                        Log.i(TAG, "[scroll-dbg] onFling vy=" + velocityY);
                        if (!isTtyReady() || guestId < 0) return false;
                        // velocityY is positive for a downward fling: same
                        // convention as the drag above - downward = history.
                        int lines = Math.round(velocityY / (ttyCellH * 10f));
                        if (lines != 0) {
                            RvvmNative.nativeTtyScrollBy(guestId, lines);
                            Log.i(TAG, "[scroll-dbg] nativeTtyScrollBy(fling) lines=" + lines);
                        }
                        return true;
                    }
                });
        Log.i(TAG, "[scroll-dbg] setup: consoleView=" + consoleView
                + " clickable=" + consoleView.isClickable()
                + " enabled=" + consoleView.isEnabled()
                + " containerChildren=" + consoleContainer.getChildCount()
                + " videoAreaVis=" + videoArea.getVisibility());
        consoleView.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN
                    || event.getActionMasked() == MotionEvent.ACTION_UP
                    || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                Log.i(TAG, "[scroll-dbg] consoleView touch action="
                        + event.getActionMasked() + " y=" + event.getY());
            }
            return consoleGesture.onTouchEvent(event);
        });

        bindTtyKey(R.id.ttyKeyEsc, TTY_ESC);
        bindTtyKey(R.id.ttyKeyTab, TTY_TAB);
        bindTtyKey(R.id.ttyKeyCtrlC, new byte[] { 0x03 });
        bindTtyKey(R.id.ttyKeyLeft, TTY_LEFT);
        bindTtyKey(R.id.ttyKeyUp, TTY_UP);
        bindTtyKey(R.id.ttyKeyDown, TTY_DOWN);
        bindTtyKey(R.id.ttyKeyRight, TTY_RIGHT);
    }

    private void bindTtyKey(int id, final byte[] bytes) {
        View key = findViewById(id);
        if (key != null) {
            key.setOnClickListener(v -> sendTtyBytes(bytes));
        }
    }

    private boolean isTtyReady() {
        return isGuestStarted && ttySerial >= 0;
    }

    private void toggleTtyKeyboard() {
        if (!isTtyReady()) return;
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm == null || ttyInput == null) return;
        if (ttyKeyboardVisible) {
            imm.hideSoftInputFromWindow(ttyInput.getWindowToken(), 0);
            ttyInput.clearFocus();
            ttyKeyboardVisible = false;
        } else {
            ttyInput.requestFocus();
            ttyInput.post(() -> {
                if (ttyInput.hasWindowFocus()
                        && imm.showSoftInput(ttyInput, InputMethodManager.SHOW_IMPLICIT)) {
                    ttyKeyboardVisible = true;
                }
            });
        }
    }

    private void sendTtyText(CharSequence text) {
        if (text == null || text.length() == 0) return;
        sendTtyBytes(text.toString().getBytes(StandardCharsets.UTF_8));
    }

    private void sendTtyBytes(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return;
        if (!RvvmNative.nativeIsGuestRunning(guestId)) return;
        RvvmNative.nativeTtyInput(guestId, bytes);
    }

    private static byte[] ttyKeyBytes(KeyEvent event) {
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return TTY_ENTER;
            case KeyEvent.KEYCODE_DEL:          return TTY_BS;
            case KeyEvent.KEYCODE_FORWARD_DEL:  return TTY_DEL;
            case KeyEvent.KEYCODE_TAB:          return TTY_TAB;
            case KeyEvent.KEYCODE_ESCAPE:       return TTY_ESC;
            case KeyEvent.KEYCODE_DPAD_UP:      return TTY_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:    return TTY_DOWN;
            case KeyEvent.KEYCODE_DPAD_RIGHT:   return TTY_RIGHT;
            case KeyEvent.KEYCODE_DPAD_LEFT:    return TTY_LEFT;
            default: break;
        }
        if (!event.isCtrlPressed()) return null;
        int c = event.getUnicodeChar(0);
        if (c > 0 && c < 0x20) return new byte[] { (byte)c };
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c >= 'A' && c <= 'Z') return new byte[] { (byte)(c - 'A' + 1) };
        return null;
    }

    /** Console keyboard target. */
    private final class TtyEditText extends EditText {
        TtyEditText(android.content.Context context) { super(context); }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent event) {
            byte[] bytes = ttyKeyBytes(event);
            if (bytes != null) {
                if (event.getRepeatCount() == 0) sendTtyBytes(bytes);
                return true;
            }
            return super.onKeyDown(keyCode, event);
        }

        @Override
        public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
            byte[] bytes = ttyKeyBytes(event);
            if (bytes != null) {
                for (int i = 0; i < Math.max(repeatCount, 1); i++) sendTtyBytes(bytes);
                return true;
            }
            return super.onKeyMultiple(keyCode, repeatCount, event);
        }

        @Override
        public boolean onKeyUp(int keyCode, KeyEvent event) {
            if (ttyKeyBytes(event) != null) return true;
            return super.onKeyUp(keyCode, event);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            InputConnection base = super.onCreateInputConnection(outAttrs);
            outAttrs.imeOptions |= EditorInfo.IME_FLAG_NO_EXTRACT_UI
                    | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            if (base == null) return null;
            return new InputConnectionWrapper(base, false) {
                @Override
                public boolean setComposingText(CharSequence text, int newCursorPosition) {
                    return true;
                }

                @Override
                public boolean finishComposingText() { return true; }

                @Override
                public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                    for (int i = 0; i < beforeLength; i++) sendTtyBytes(TTY_BS);
                    for (int i = 0; i < afterLength; i++) sendTtyBytes(TTY_DEL);
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    byte[] bytes = ttyKeyBytes(event);
                    if (bytes != null) {
                        if (event.getAction() == KeyEvent.ACTION_DOWN) sendTtyBytes(bytes);
                        return true;
                    }
                    return super.sendKeyEvent(event);
                }
            };
        }
    }

    /** Close the log file. */
    private void closeLogFile() {
        synchronized (logFileLock) {
            if (logWriter != null) {
                try {
                    logWriter.close();
                } catch (IOException e) {
                    Log.w(TAG, "Failed to close log file", e);
                }
                logWriter = null;
            }
        }
    }

    /**
     * Handle guest exit. Called on the UI thread when the guest thread finishes.
     * The console stays up: the retired session keeps the last screen and the
     * whole scrollback, which is exactly when the user reads back the output.
     */
    private void handleGuestExit(int exitCode) {
        Log.i(TAG, "Guest " + appName + " exited with code " + exitCode);
        if (videoArea != null) videoArea.setVisibility(View.GONE);
        showStatusBar("Guest exited: " + appName
                + " (exit " + exitCode + ") — tap back to close");
        if (!AFTER_GUEST_EXIT_FINISH.equals(afterGuestExit)) {
            return;
        }
        closeLogFile();
        finish();
    }

    private void showStatusBar(String text) {
        if (statusBar == null) return;
        statusBar.setText(text);
        // Replace the key bar: hide it and take its exact slot at the bottom.
        View keyBar = findViewById(R.id.ttyKeyBar);
        if (keyBar != null) keyBar.setVisibility(View.GONE);
        statusBar.setVisibility(View.VISIBLE);
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
