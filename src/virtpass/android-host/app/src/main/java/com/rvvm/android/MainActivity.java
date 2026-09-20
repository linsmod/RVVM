package com.rvvm.android;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.SurfaceTexture;
import android.graphics.drawable.Drawable;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewConfiguration;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListPopupWindow;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;

/**
 * Main Activity for RVVM Android host app.
 * This activity manages the RVVM process: guest launch, surface and console.
 * Sensors are not handled here - the native sensor backend (vp_sensor_android.c)
 * owns the platform ASensorManager and feeds the guest directly, so no sensor
 * data crosses Java.
 */
public class MainActivity extends Activity {

    private static final String TAG = "RVVM-MainActivity";

    /**
     * Intent extra naming the guest app to run, for example:
     *   adb shell am start -n com.rvvm.android/.MainActivity --es guest test_render.exe
     * The value is matched against the *.exe entries in assets (the ".exe"
     * suffix is optional), overriding the default "first entry" selection.
     */
    public static final String EXTRA_GUEST_APP = "guest";

    /**
     * The virtual panel: the pixel geometry the guest renders into and the
     * space its input is expressed in.
     *
     * Pinned here rather than latched from the floating window's surface (see
     * RvvmNative.nativeSetPanelSize), because that surface is the size of a
     * card the user drags around. 720p landscape, so a guest that lays out for
     * a desktop screen gets one regardless of this device's own orientation.
     * The card is only a viewport onto the panel, which is why touches are
     * mapped from card pixels into panel pixels before they are forwarded.
     */
    // Package-visible: GlWindowCard sizes its letterbox to the panel's ratio.
    static final int PANEL_W = 1280;
    static final int PANEL_H = 720;

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
    private TextureView ttyView;          // Console render target
    private FrameLayout ttyViewport;      // Console viewport (holds ttyView)
    private TtyEditText ttyInput;         // Console keyboard/IME focus target
    private Button startButton;           // the launcher menu (new run)
    private Button suspendButton;         // acts on the focused window
    private Button stopButton;            // acts on the focused window

    // ---- Floating graphics windows ----
    // Each run gets its own floating card (GlWindowCard): a SurfaceView that
    // composites above the console (setZOrderMediaOverlay), a caption bar that
    // drags, and the Windows three-button set. Everything except the title bar
    // belongs to the guest: a touch on a card's video area is forwarded to that
    // card's run, which is also why the video cannot host a gesture of ours.
    //
    // The cards live in a map by guest id; the run and its card share a
    // lifetime (a closed card stops its run, an exited run takes its card
    // down). First-frame and exit signals still carry the runGeneration so a
    // late signal cannot land on the run that replaced this one.
    private View workspace;
    private FlowLayout glTaskbar;         // one chip per running guest window
    private final java.util.HashMap<Integer, GlWindowCard> glCards =
            new java.util.HashMap<>();
    // A run whose card is waiting for its surface: picked up by the card's
    // surfaceCreated (see runGuestElf). The ELF it will run is kept alongside.
    private int glRunPendingId = -1;
    private String pendingElfName = null;
    private String pendingElfPath = null;

    // ---- Run identity ----
    // Bumped by every run that actually starts, and read by the signals a run
    // leaves behind (its first frame, its exit). A window has no way of telling
    // those apart on its own - it is one window, and the run it belonged to may
    // already be over - so each of them carries the generation it was born with
    // and is dropped when the current one has moved on. Volatile: written on
    // the UI thread, read from the guest thread and the exit monitor.
    private volatile int runGeneration = 0;

    // Content-driven visibility (the 1x1 waiting state, the first-frame reveal)
    // is per-card now - see GlWindowCard. The rule it implements is unchanged:
    // the surface must exist from the start (an EGL guest builds its window
    // surface the moment it starts), but the card takes no room on screen
    // until the guest has a frame for it.

    // Guest runs kept under files/logs/, oldest pruned first.
    private static final int LOG_MAX_FILES = 20;

    // The run's log file. Opened in runGuestElf (UI thread), written from the
    // guest thread (onOutput), closed when the guest exits - hence the lock.
    private final Object logFileLock = new Object();
    private BufferedWriter logWriter;
    private File logFile;

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
    private boolean hasAutoStarted = false;
    // Host-side mirror of the suspend request, per guest. The button label
    // and the toggle action read this so they flip the instant the user
    // clicks, without waiting for vCPUs to actually reach the park point
    // (which can lag by a blocking host syscall). The teardown flow that
    // needs the real quiesced state queries nativeIsGuestParked instead.
    private final Set<Integer> suspendedGuests = new HashSet<>();

    // ---- TTY console (TextureView tab) ----
    // One cell = 4 ints from nativeTtySnapshot: [0] UCS-4 cp, [1] fg ARGB,
    // [2] bg ARGB, [3] flags (bit0 bold, bit1 underline, bit2 reverse,
    // bit3 wide). Drawn with a monospace Paint; nativeTtySerial() gates
    // re-snapshotting, so idle output costs nothing but a compare.
    // Grid height: TTY_ROWS is the default, the viewport-derived height is
    // clamped to [TTY_MIN_ROWS, TTY_MAX_ROWS] (see drawTty). The column count
    // is fixed - the font is scaled to the view width instead - which is what
    // keeps a wide terminal legible on a narrow phone.
    private static final int TTY_ROWS = 24;
    private static final int TTY_MIN_ROWS = 8;
    private static final int TTY_MAX_ROWS = 200;
    private static final int TTY_COLS = 80;
    private static final int TTY_CELL = 4;
    private static final float TTY_PAD = 8f;

    // Cell flags, packed by native into ttyCells[i + 3].
    private static final int TTY_FLAG_BOLD      = 1;
    private static final int TTY_FLAG_UNDERLINE = 1 << 1;
    private static final int TTY_FLAG_REVERSE   = 1 << 2;
    private static final int TTY_FLAG_WIDE      = 1 << 3;
    private static final int TTY_FLAG_CURSOR    = 1 << 4;

    /* Snapshot buffer: sized for the tallest grid the viewport can ask for, not
     * for the default height, because nativeTtySnapshot() fills however many
     * rows the VTerm currently has (see ttyRows). */
    private final int[] ttyCells = new int[TTY_MAX_ROWS * TTY_COLS * TTY_CELL];
    /* Grid height last requested from native. Native keeps the same number, so
     * a resize is only sent when the view actually changed height. */
    private int ttyRows = TTY_ROWS;
    private int ttySerial = -1;

    private volatile boolean ttyRunning = false;
    private final Paint ttyTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ttyBgPaint = new Paint();
    private final Paint ttyCursorPaint = new Paint();
    private final Paint ttyUnderlinePaint = new Paint();
    private final Paint ttyScrollPaint = new Paint();

    // Scrollback view: native holds the position (and clamps it), these are the
    // numbers it reports back - {lines the window sits above the live bottom,
    // lines stored} - plus the grid's cell height, which is what turns a drag
    // in pixels into whole terminal lines.
    private final int[] ttyScrollInfo = new int[2];
    private float ttyCellH = 24f;
    private float ttyDragFromY = 0f, ttyDragLastY = 0f, ttyDragRest = 0f;
    private boolean ttyDragging = false;

    // Cursor blink. Its own clock is what makes the tick repaint even when the
    // guest is silent, which is the point: a console waiting for input has to
    // show where that input will land. A repaint is one canvas pass over the
    // visible grid, so 2 Hz of them costs less than the guest's own output does.
    private static final long TTY_BLINK_MS = 500;
    private boolean ttyCursorOn = true;
    private long ttyBlinkNext = 0;

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

    // 30 Hz poll: snapshot + redraw when the native serial changed, plus the
    // cursor's own blink phase.
    private final Runnable ttyTick = new Runnable() {
        @Override public void run() {
            if (!ttyRunning) return;
            if (ttyView != null && ttyView.isAvailable()) {
                int serial = RvvmNative.nativeTtySerial(activeGuestId);
                long now = SystemClock.uptimeMillis();
                boolean redraw = serial != ttySerial;
                if (redraw) {
                    // New output: the cursor is shown at once and the blink
                    // restarts from there, the way a terminal behaves when
                    // something is typed or printed.
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
        // A full blink period of grace, so the first repaint does not arrive
        // with the cursor already in its off phase.
        ttyCursorOn = true;
        ttyBlinkNext = SystemClock.uptimeMillis() + TTY_BLINK_MS;
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
        int w = ttyView.getWidth(), h = ttyView.getHeight();
        if (w <= 0 || h <= 0) return;

        /* Font size: fit TTY_COLS columns into the view width by measuring
         * the real monospace advance. At the 100px probe size the advance
         * is adv100, so the size that fits TTY_COLS cells is
         * 100 * availW / (cols * adv100), floored to a whole pixel - a
         * fractional text size is what smeared the glyphs on low-density
         * screens. The height only enters as a floor for a view too short to
         * show TTY_MIN_ROWS rows: the row count itself is derived from the
         * height below, so the grid fills the console instead of leaving
         * black bands above and below a fixed 24. */
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

        // Whole-pixel grid: cellW may differ from the raw advance by a
        // fraction, so glyphs are centered in their cell instead of being
        // appended to a run - that keeps the columns exact even if the
        // font that answered is not really monospaced.
        float cellW = Math.max(1f, (float) Math.round(ttyTextPaint.measureText("M")));
        float cellH = (float) Math.round(size * 1.2f);

        /* Rows that fit under this cell height. The console grows and shrinks
         * with the view (rotation, soft keyboard), so the height is handed to
         * native, which resizes the VTerm: the snapshot below and the guest's
         * own TIOCGWINSZ then agree with what is drawn. Sent before the
         * snapshot, which reads the grid native was just given. */
        int rows = (int) ((h - 2 * TTY_PAD) / cellH);
        if (rows < TTY_MIN_ROWS) rows = TTY_MIN_ROWS;
        if (rows > TTY_MAX_ROWS) rows = TTY_MAX_ROWS;
        if (rows != ttyRows) {
            ttyRows = rows;
            RvvmNative.nativeTtyResize(rows, TTY_COLS);
        }

        

        Canvas canvas = ttyView.lockCanvas(null);
        if (canvas == null) return;
        try {
            canvas.drawColor(0xFF000000);
            if (RvvmNative.nativeTtySnapshot(activeGuestId, ttyCells, ttyScrollInfo) <= 0) return;
            float gridW = cellW * TTY_COLS, gridH = cellH * rows;
            /* A drag on the console is turned into terminal lines by the touch
             * listener using the same cell height the grid was laid out with. */
            ttyCellH = cellH;
            float ox = (float) Math.floor((w - gridW) / 2f);
            float oy = (float) Math.floor((h - gridH) / 2f);

            // The cursor spans one cell and native flags it in the cell
            // attributes; located up front so its block can be painted before
            // the glyphs that sit on it.
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

            /* TEMP DIAG: one line per repaint, so the frame the renderer was
             * asked to draw can be matched against the native screen dump. */
            Log.i(TAG, "tty draw: canvas " + w + "x" + h + " size=" + size +
                    " cell=" + cellW + "x" + cellH + " rows=" + rows +
                    " origin=(" + ox + "," + oy +
                    ") grid=" + gridW + "x" + gridH + " serial=" + ttySerial +
                    " cursor=" + curRow + "," + curCol + (ttyCursorOn ? " on" : " off"));

            Paint.FontMetrics fm = ttyTextPaint.getFontMetrics();
            float baselineOff = (float) Math.round((cellH - (fm.descent - fm.ascent)) / 2f - fm.ascent);

            ttyUnderlinePaint.setStyle(Paint.Style.STROKE);
            ttyUnderlinePaint.setStrokeWidth(Math.max(1f, size / 14f));

            for (int r = 0; r < rows; r++) {
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

                // Cursor block: the cell filled with its own foreground colour,
                // with the glyph (when there is one, see below) redrawn in the
                // background colour on top of it - the reverse-video block a
                // real terminal shows. Painted before the glyphs so it lies
                // underneath them; on a blank cell it is simply a solid block.
                if (r == curRow && ttyCursorOn) {
                    int i = (curRow * TTY_COLS + curCol) * TTY_CELL;
                    boolean cursorWide = (ttyCells[i + 3] & TTY_FLAG_WIDE) != 0
                            && curCol + 1 < TTY_COLS;
                    ttyCursorPaint.setColor(ttyCells[i + 1]);
                    float cx = ox + curCol * cellW;
                    canvas.drawRect(cx, y0, cx + (cursorWide ? 2f : 1f) * cellW,
                            y0 + cellH, ttyCursorPaint);
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
                    boolean wide = (flags & TTY_FLAG_WIDE) != 0 && c + 1 < TTY_COLS;
                    // Under the block the glyph has to trade colours with it.
                    boolean onCursor = ttyCursorOn && r == curRow && c == curCol;
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

                    ttyTextPaint.setColor(onCursor ? ttyCells[i + 2] : ttyCells[i + 1]);
                    ttyTextPaint.setFakeBoldText((flags & TTY_FLAG_BOLD) != 0);
                    canvas.drawText(ttyGlyph, 0, len, x, baseline, ttyTextPaint);
                    ttyTextPaint.setTextScaleX(1f);   // next measure must be unscaled

                    if ((flags & TTY_FLAG_UNDERLINE) != 0) {
                        ttyUnderlinePaint.setColor(onCursor ? ttyCells[i + 2] : ttyCells[i + 1]);
                        canvas.drawLine(x, y0 + cellH - 1f,
                                x + target, y0 + cellH - 1f, ttyUnderlinePaint);
                    }
                    if (wide) {
                        c++;    // the wide glyph's filler cell
                    }
                }
            }

            /* Scrollback thumb, in the padding right of the grid so it never
             * covers the last column. It is drawn whenever lines have scrolled
             * off: dim while the live screen is showing, brighter while the
             * view sits in history - the only cue that there is more to read
             * than the rows on screen, and where in it the view is. Drag the
             * console to move through it. */
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

    /* ============================================================
     * Console keyboard input
     *
     * The console used to be read-only: guest fd 1/2 was parsed into the VTerm
     * and rendered, but guest fd 0 was the *host* process's stdin, so nothing
     * typed here could ever reach it. Keyboard input now travels
     *
     *     this field  ->  RvvmNative.nativeTtyInput()
     *                 ->  rvvm_user_tty_input()      (core: line discipline)
     *                 ->  guest read(0, ...)
     *
     * and returns to the screen through the core's VTerm echo, i.e. through the
     * same nativeTtySnapshot() path as guest output. Everything is funnelled
     * through one 1x1 invisible EditText because that is what Android routes
     * IME text, paste and hardware key events to; the field is kept empty (its
     * text is forwarded and cleared as it arrives) so the terminal never gets a
     * local echo, a cursor or a selection competing with the guest's.
     * ============================================================ */

    // Byte sequences a real terminal sends for its non-printing keys.
    private static final byte[] TTY_ENTER = { '\r' };
    private static final byte[] TTY_TAB   = { '\t' };
    private static final byte[] TTY_ESC   = { 0x1B };
    private static final byte[] TTY_BS    = { 0x7F };                  // Backspace
    private static final byte[] TTY_DEL   = { 0x1B, '[', '3', '~' };   // Delete
    private static final byte[] TTY_UP    = { 0x1B, '[', 'A' };
    private static final byte[] TTY_DOWN  = { 0x1B, '[', 'B' };
    private static final byte[] TTY_RIGHT = { 0x1B, '[', 'C' };
    private static final byte[] TTY_LEFT  = { 0x1B, '[', 'D' };
    private static final byte[] TTY_HOME  = { 0x1B, '[', 'H' };
    private static final byte[] TTY_END   = { 0x1B, '[', 'F' };
    private static final byte[] TTY_PGUP  = { 0x1B, '[', '5', '~' };
    private static final byte[] TTY_PGDN  = { 0x1B, '[', '6', '~' };

    /** Create the console's keyboard target and wire it up to the guest. */
    private void initTtyInput() {
        ttyInput = new TtyEditText(this);
        // 1x1 and fully transparent: a focus/IME target that is never drawn
        // over the terminal grid.
        ttyInput.setAlpha(0f);
        ttyInput.setBackground(null);
        ttyInput.setPadding(0, 0, 0, 0);
        ttyInput.setCursorVisible(false);
        ttyInput.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        // A terminal offers the IME nothing to navigate to, and the extract UI
        // would cover the very console being typed into.
        ttyInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_ACTION_NONE);
        ttyViewport.addView(ttyInput, new FrameLayout.LayoutParams(1, 1));

        // The single text path. Whatever an IME commit, a hardware key or a
        // paste puts in the field is forwarded to the guest and removed right
        // away: the guest's line discipline owns the echo, so the field must
        // keep no copy of it (and must not grow without bound).
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

        // Tap the terminal to summon the soft keyboard. It is not popped up
        // automatically with the console: it is often looked at just to read
        // the last screen, and a keyboard over it would be in the way.
        ttyView.setOnClickListener(v -> toggleTtyKeyboard());
        watchTtyKeyboard();

        bindTtyKey(R.id.ttyKeyEsc, TTY_ESC);
        bindTtyKey(R.id.ttyKeyTab, TTY_TAB);
        bindTtyKey(R.id.ttyKeyCtrlC, new byte[] { 0x03 });
        bindTtyKey(R.id.ttyKeyLeft, TTY_LEFT);
        bindTtyKey(R.id.ttyKeyUp, TTY_UP);
        bindTtyKey(R.id.ttyKeyDown, TTY_DOWN);
        bindTtyKey(R.id.ttyKeyRight, TTY_RIGHT);

        // The console is the only full-screen pane and is on screen from the
        // start, so it takes focus right away: a hardware keyboard types
        // straight into the guest. Focus alone does not raise the soft
        // keyboard - that still only happens on a tap.
        ttyInput.requestFocus();
    }

    private void bindTtyKey(int id, final byte[] bytes) {
        View key = findViewById(id);
        if (key != null) {
            key.setOnClickListener(v -> sendTtyBytes(bytes));
        }
    }

    /**
     * Drag the console up and down through its scrollback. The position itself
     * is native state: native holds it, clamps it to what is stored, keeps a
     * view that was dragged back anchored on the lines it is showing while
     * output keeps arriving, and hands it home on the next keystroke. So a drag
     * only has to turn pixels into terminal lines - downward looks back into
     * history, and carrying on past the bottom returns to the live screen. The
     * view is tapped for the keyboard, so a drag only starts on touch slop.
     */
    private void initTtyScrolling() {
        final int slop = ViewConfiguration.get(this).getScaledTouchSlop();
        ttyView.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    ttyDragFromY = ttyDragLastY = event.getY();
                    ttyDragRest = 0f;
                    ttyDragging = false;
                    return false;   // a tap must still reach the click listener
                case MotionEvent.ACTION_MOVE: {
                    if (!ttyDragging) {
                        if (Math.abs(event.getY() - ttyDragFromY) < slop) {
                            return false;   // not a drag yet
                        }
                        ttyDragging = true;
                    }
                    ttyDragRest += event.getY() - ttyDragLastY;
                    ttyDragLastY = event.getY();
                    int lines = (int) (ttyDragRest / ttyCellH);
                    if (lines != 0) {
                        ttyDragRest -= lines * ttyCellH;
                        RvvmNative.nativeTtyScrollBy(activeGuestId, lines);
                    }
                    return true;    // consumed: no click at the end of a drag
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    boolean dragged = ttyDragging;
                    ttyDragging = false;
                    return dragged;
                default:
                    return false;
            }
        });
    }

    /* ============================================================
     * Floating graphics windows (one card per run)
     *
     * The guest renders into a SurfaceView card that floats over the console.
     * z-order is the whole trick: a SurfaceView is normally composited *below*
     * its window, which is what lets ordinary views draw on top of it - and
     * what would otherwise let the console paint over the graphics. Calling
     * setZOrderMediaOverlay(true) lifts the surface above the window, so the
     * card sits over the console and can be dragged or expanded anywhere on
     * the screen.
     *
     * Each run owns one card (GlWindowCard): its own surface, its own
     * geometry, its own three-button set. The host side - everything that
     * crosses runs - lives here: the card table, the taskbar strip, touch
     * forwarding to the right guest, and the close button ending that card's
     * run. A card's surface lifecycle is bound to its run's native window
     * with the card's guest id (nativeSetWindow).
     *
     * The three buttons are the Windows set and mean what they mean there.
     * Minimize takes the window off the workspace and leaves a chip in its
     * place; maximize fills the workspace and restore puts the window back at
     * its floating corner; close stops the guest and takes the window down
     * with it. Minimize leaves the guest running and close is the only one
     * that ends it, the same split a desktop draws between a minimized window
     * and a closed one.
     * ============================================================ */

    private final GlWindowCard.Host glCardHost = new GlWindowCard.Host() {
        @Override public void onCardSurfaceCreated(GlWindowCard card, SurfaceHolder holder) {
            if (!isInitialized) {
                return;
            }
            RvvmNative.nativeSetWindow(holder.getSurface(), card.getGuestId());
            postLifecycleCmd(APP_CMD_INIT_WINDOW);
            if (card.getGuestId() == glRunPendingId) {
                // A run held back for exactly this surface (runGuestElf). The
                // window is in native hands now - that is the line above - so
                // the guest can go ahead.
                Log.i(TAG, "Window up: starting the held-back run " + card.getGuestId());
                glRunPendingId = -1;
                startRun(card.getGuestId(), pendingElfName, pendingElfPath);
            }
        }

        @Override public void onCardSurfaceChanged(GlWindowCard card, SurfaceHolder holder,
                                                  int format, int width, int height) {
            Log.i(TAG, "Card " + card.getGuestId() + " surface changed: " + width + "x" + height);
            if (isInitialized) {
                RvvmNative.nativeSetWindow(holder.getSurface(), card.getGuestId());
                postLifecycleCmd(APP_CMD_WINDOW_RESIZED);
            }
        }

        @Override public void onCardSurfaceDestroyed(GlWindowCard card, SurfaceHolder holder) {
            if (isInitialized) {
                RvvmNative.nativeSetWindow(null, card.getGuestId());
                postLifecycleCmd(APP_CMD_TERM_WINDOW);
            }
        }

        @Override public void onCardRedrawNeeded(GlWindowCard card, SurfaceHolder holder) {
            postLifecycleCmd(APP_CMD_WINDOW_REDRAW_NEEDED);
        }

        @Override public void onCardTouch(GlWindowCard card, MotionEvent event) {
            // The card is a viewport: the guest's input space is the panel,
            // and the frame buffer fills the whole card. Touches arrive in
            // card coordinates (the listener owns the whole card, black
            // letterbox bars included) - translate into the surface's own
            // space first, then scale linearly onto the panel.
            if (!isInitialized) {
                return;
            }
            // Touching a window focuses it: the tap itself is forwarded to the
            // run that just became the foreground one.
            focusCard(card);
            float viewW = card.surfaceView.getWidth(), viewH = card.surfaceView.getHeight();
            if (viewW <= 0f || viewH <= 0f) {
                return;
            }
            // The surface sits centered inside the video area, which itself
            // sits below the caption bar inside the card - offset by both.
            ViewGroup video = (ViewGroup) card.surfaceView.getParent();
            float offX = video.getLeft() + card.surfaceView.getLeft();
            float offY = video.getTop() + card.surfaceView.getTop();
            float sx = PANEL_W / viewW, sy = PANEL_H / viewH;
            int count = event.getPointerCount();
            if (count > MAX_POINTERS) {
                count = MAX_POINTERS;
            }
            for (int i = 0; i < count; i++) {
                // Touches on the bars land outside the frame: clamped onto the
                // panel edge, the same way a mouse dragged off a desktop
                // window's content just stops at its border.
                motionX[i] = Math.max(0f, Math.min(PANEL_W, (event.getX(i) - offX) * sx));
                motionY[i] = Math.max(0f, Math.min(PANEL_H, (event.getY(i) - offY) * sy));
                motionId[i] = event.getPointerId(i);
            }
            // Use the raw action: ACTION_POINTER_DOWN/UP encode the pointer
            // index in the upper bits, which the guest's GameActivity expects.
            RvvmNative.nativePostMotionEvent(card.getGuestId(), motionX, motionY, motionId, count,
                    event.getAction(), event.getEventTime() * 1000000L);
        }

        @Override public void onCardClosed(GlWindowCard card) {
            // Close button: stop the guest, and take the window down with it -
            // the Stop button's action plus the card's dismissal.
            stopGuestElf(card.getGuestId());
            dismissCard(card);
        }

        @Override public void onCardFocused(GlWindowCard card) {
            focusCard(card);
        }

        @Override public void onCardMaximized() {
            // The console the card just covered is also where the keyboard
            // would have gone.
            hideTtyKeyboard();
        }

        @Override public void onCardMinimized(GlWindowCard card) {
            rebuildTaskbar();
        }

        @Override public void onCardRestored(GlWindowCard card) {
            rebuildTaskbar();
        }
    };

    /** The Start menu: every guest app in assets. Tapping one starts a NEW
     *  run of it - the launcher never "switches", that is what the taskbar
     *  chips are for. */
    private void showStartMenu() {
        if (guestApps == null || guestApps.length == 0) {
            return;
        }
        ListPopupWindow menu = new ListPopupWindow(this);
        menu.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_1, guestApps));
        menu.setAnchorView(startButton);
        menu.setModal(true);
        // The popup defaults to the anchor's width - the Start button is a
        // small square, so every item would truncate. Size it to the longest
        // app name instead, clamped to something sane on both ends.
        android.graphics.Paint paint = new android.graphics.Paint();
        paint.setTextSize(18f * getResources().getDisplayMetrics().density);
        float widest = 0f;
        for (String app : guestApps) {
            widest = Math.max(widest, paint.measureText(app));
        }
        float dp = getResources().getDisplayMetrics().density;
        menu.setContentWidth((int) Math.max(200f * dp,
                Math.min(400f * dp, widest + 64f * dp)));
        menu.setOnItemClickListener((parent, view, position, id) -> {
            selectedGuestApp = guestApps[position];
            Log.i(TAG, "Start menu launch: " + selectedGuestApp);
            menu.dismiss();
            runGuestElf();
        });
        menu.show();
    }

    /** The taskbar strip: one chip per running window - focused, on-screen or
     *  minimized. Tap focuses (or restores) it; long-press closes it. The
     *  focused chip is highlighted, the way the active taskbar button is on a
     *  desktop. */
    private void rebuildTaskbar() {
        glTaskbar.removeAllViews();
        ArrayList<Integer> ids = new ArrayList<>(glCards.keySet());
        java.util.Collections.sort(ids);
        for (Integer id : ids) {
            final GlWindowCard card = glCards.get(id);
            boolean focused = id == activeGuestId;
            TextView chip = new TextView(this);
            String name = card.getTitle().isEmpty() ? getString(R.string.gl_title) : card.getTitle();
            chip.setText(card.isMinimized() ? getString(R.string.gl_taskbar_entry, name) : name);
            // Uniform chip size: the flow layout wraps them into tidy rows,
            // long names ellipsize instead of pushing the row wider.
            float density = getResources().getDisplayMetrics().density;
            chip.setWidth((int) (150 * density));
            chip.setMaxLines(1);
            chip.setEllipsize(android.text.TextUtils.TruncateAt.END);
            chip.setPadding(24, 12, 24, 12);
            chip.setGravity(android.view.Gravity.CENTER);
            chip.setTextSize(13f);
            chip.setTypeface(null, focused ? Typeface.BOLD : Typeface.NORMAL);
            chip.setBackgroundColor(focused ? 0xFF1F5FA8 : 0xFF2A2A2A);
            chip.setTextColor(0xFFE0E0E0);
            chip.setOnClickListener(v -> {
                if (card.isMinimized()) {
                    card.restore();
                }
                focusCard(card);
            });
            chip.setOnLongClickListener(v -> {
                // The taskbar's "close window": same as the card's ✕.
                stopGuestElf(id);
                dismissCard(card);
                return true;
            });
            glTaskbar.addView(chip);
        }
    }

    /** Take a card down for good: off the workspace and out of the table. */
    private void dismissCard(GlWindowCard card) {
        glCards.remove(card.getGuestId());
        card.dismiss();
        rebuildTaskbar();
    }

    /**
     * Make the given card's run the foreground one: the console, the keyboard
     * input, the Suspend/Stop buttons and the touch stream all act on it.
     *
     * The guest losing the foreground hears LOST_FOCUS *before* the handover
     * (lifecycle commands are queued to whichever run is active at that
     * moment), the winner hears GAINED_FOCUS right after. Focus does not
     * pause anyone: both guests keep rendering - a background window on a
     * desktop keeps painting unless its app decides otherwise.
     */
    private void focusCard(GlWindowCard card) {
        int id = card.getGuestId();
        if (id == activeGuestId || !RvvmNative.nativeIsGuestRunning(id)) {
            return;
        }
        Log.i(TAG, "Foreground: guest " + activeGuestId + " -> " + id);
        postLifecycleCmd(APP_CMD_LOST_FOCUS);
        activeGuestId = id;
        RvvmNative.nativeSetActiveGuest(id);
        postLifecycleCmd(APP_CMD_GAINED_FOCUS);
        // The focused window comes to the front and takes the active-caption
        // tint; the previous foreground drops back to grey.
        GlWindowCard prev = glCards.get(id);
        for (GlWindowCard c : glCards.values()) {
            if (c != card) {
                c.setFocused(false);
            }
        }
        card.setFocused(true);
        card.cardView.bringToFront();
        rebuildTaskbar();
    }

    /** Create the card for a run: added to the workspace at 1x1, waiting for
     *  its surface (which the run is then held back for, see runGuestElf).
     *  The card cascades off the cards already on screen, so simultaneously
     *  running guests - even several of the same program - spawn visibly
     *  staggered instead of stacked pixel-for-pixel. */
    private GlWindowCard createCard(int guestId, String guestName) {
        int visible = 0;
        for (GlWindowCard c : glCards.values()) {
            if (!c.isMinimized()) visible++;
        }
        GlWindowCard card = new GlWindowCard(this, (ViewGroup) workspace, guestId, guestName, glCardHost);
        glCards.put(guestId, card);
        card.cascadeTo(visible);
        // The new run is the foreground one: it takes the active-caption tint.
        for (GlWindowCard c : glCards.values()) {
            c.setFocused(c.getGuestId() == guestId);
        }
        rebuildTaskbar();
        return card;
    }

    // Whether a soft keyboard is on screen, and the tallest the window has been
    // (its keyboard-less height, see watchTtyKeyboard).
    private boolean ttyKeyboardVisible = false;
    private int ttyWindowHeight = 0;

    /**
     * Track the soft keyboard through the window's visible frame.
     *
     * InputMethodManager.isActive() is not an answer to "is a keyboard up": it
     * reports whether the view is the one the IME serves, which is already true
     * from the moment the console takes focus. The tap toggle used to ask
     * it that question, so it picked its hide branch on the very tap that
     * should have raised the keyboard - a keyboard only appeared in the rare
     * interleaving where the tab's focus request had not been served yet, which
     * is exactly the "almost never, and inexplicably sometimes" behaviour.
     * The IME's own height cannot be wrong about this, and unlike a flag of our
     * own it stays right when the keyboard is dismissed with Back.
     */
    private void watchTtyKeyboard() {
        final View root = getWindow().getDecorView();
        final int minKeyboardHeight = getResources().getDisplayMetrics().heightPixels / 4;
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            // adjustResize shrinks the window itself on some versions and only
            // the content on others: the tallest height ever seen is the
            // keyboard-less one either way, so the difference to the bottom of
            // the visible frame is the keyboard's height.
            if (root.getHeight() > ttyWindowHeight) {
                ttyWindowHeight = root.getHeight();
            }
            Rect visible = new Rect();
            root.getWindowVisibleDisplayFrame(visible);
            ttyKeyboardVisible = ttyWindowHeight - visible.bottom > minKeyboardHeight;
        });
    }

    /** Tap behaviour: raise the keyboard when it is down, drop it when it is up. */
    private void toggleTtyKeyboard() {
        if (ttyKeyboardVisible) {
            hideTtyKeyboard();
        } else {
            showTtyKeyboard();
        }
    }

    private void showTtyKeyboard() {
        ttyInput.requestFocus();
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm == null) {
            return;
        }
        // Posted rather than called here: a showSoftInput() issued while the
        // touch is still being dispatched - or while the focus change from a
        // tab switch is still in flight, in which case requestFocus() above
        // returns without dispatching anything at all - is dropped by the IME.
        // That is the other half of why the keyboard came up so rarely.
        ttyInput.post(() -> imm.showSoftInput(ttyInput, InputMethodManager.SHOW_IMPLICIT));
    }

    private void hideTtyKeyboard() {
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm != null) {
            imm.hideSoftInputFromWindow(ttyInput.getWindowToken(), 0);
        }
    }

    /** Forward text to the guest as UTF-8. */
    private void sendTtyText(CharSequence text) {
        if (text == null || text.length() == 0) return;
        sendTtyBytes(text.toString().getBytes(StandardCharsets.UTF_8));
    }

    private void sendTtyBytes(byte[] bytes) {
        if (bytes == null || bytes.length == 0) return;
        if (!RvvmNative.nativeIsGuestRunning(activeGuestId)) return;
        RvvmNative.nativeTtyInput(activeGuestId, bytes);
    }

    /**
     * Terminal byte sequence for a hardware key, or null when the key has none.
     * This is what gives a physical keyboard its arrows, Esc, Tab, Delete and
     * Ctrl+&lt;letter&gt; - none of which a soft keyboard can produce.
     */
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
            case KeyEvent.KEYCODE_MOVE_HOME:    return TTY_HOME;
            case KeyEvent.KEYCODE_MOVE_END:     return TTY_END;
            case KeyEvent.KEYCODE_PAGE_UP:      return TTY_PGUP;
            case KeyEvent.KEYCODE_PAGE_DOWN:    return TTY_PGDN;
            default: break;
        }
        if (!event.isCtrlPressed()) {
            return null;
        }
        // Ctrl+<key> is the control character at the key's position in the
        // alphabet - ^C is 0x03, ^D is 0x04 and so on - which is how a terminal
        // gets its line-editing and interrupt keys. Some keymaps hand over the
        // control character directly and some the plain letter, so both are
        // accepted.
        int c = event.getUnicodeChar(0);
        if (c > 0 && c < 0x20) {
            return new byte[] { (byte)c };
        }
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c >= 'A' && c <= 'Z') {
            return new byte[] { (byte)(c - 'A' + 1) };
        }
        switch (c) {
            case ' ': case '@': return new byte[] { 0x00 };  // ^Space / ^@
            case '[':           return TTY_ESC;              // ^[
            case '\\':          return new byte[] { 0x1C };  // ^\
            case ']':           return new byte[] { 0x1D };  // ^]
            case '^':           return new byte[] { 0x1E };  // ^^
            case '_':           return new byte[] { 0x1F };  // ^_
            default: break;
        }
        return null;
    }

    /**
     * The console's keyboard target.
     *
     * An EditText is used only because it is the one view Android routes IME
     * text, paste and key events to; it must never behave like a text field.
     * The two hooks below are what keep it out of the way:
     *
     *  - the input connection drops IME preedit (an intermediate pinyin or
     *    gesture-typing string is not what the user typed - only the committed
     *    text is) and turns the soft keyboard's Backspace into a real 0x7F,
     *    which on an always-empty field would otherwise delete nothing and so
     *    reach the guest as no input at all;
     *  - key events are translated to terminal bytes instead of being handed to
     *    the field, so a physical keyboard cannot edit the (invisible) text.
     */
    private final class TtyEditText extends EditText {

        TtyEditText(Context context) {
            super(context);
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent event) {
            byte[] bytes = ttyKeyBytes(event);
            if (bytes != null) {
                // Held keys repeat through onKeyMultiple on real hardware;
                // sending from both would double every repeat.
                if (event.getRepeatCount() == 0) {
                    sendTtyBytes(bytes);
                }
                return true;
            }
            // Printable keys are deliberately left to the field: they land in
            // the Editable and the TextWatcher forwards them.
            return super.onKeyDown(keyCode, event);
        }

        @Override
        public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
            byte[] bytes = ttyKeyBytes(event);
            if (bytes != null) {
                for (int i = 0; i < Math.max(repeatCount, 1); i++) {
                    sendTtyBytes(bytes);
                }
                return true;
            }
            return super.onKeyMultiple(keyCode, repeatCount, event);
        }

        @Override
        public boolean onKeyUp(int keyCode, KeyEvent event) {
            // Swallowed so the field's own handling never sees half a key. A
            // terminal acts on the press, so nothing is sent here.
            if (ttyKeyBytes(event) != null) {
                return true;
            }
            return super.onKeyUp(keyCode, event);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            InputConnection base = super.onCreateInputConnection(outAttrs);
            outAttrs.imeOptions |= EditorInfo.IME_FLAG_NO_EXTRACT_UI
                    | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            if (base == null) {
                return null;
            }
            return new InputConnectionWrapper(base, false) {
                @Override
                public boolean setComposingText(CharSequence text, int newCursorPosition) {
                    // Preedit is not forwarded and not put in the field either,
                    // so no candidate string shows up on the guest's screen.
                    return true;
                }

                @Override
                public boolean finishComposingText() {
                    // Nothing to finish: setComposingText() never inserted.
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                    // The soft keyboard's Backspace. On an empty field the base
                    // implementation has nothing to delete, so it is translated
                    // into the byte the guest expects instead.
                    for (int i = 0; i < beforeLength; i++) {
                        sendTtyBytes(TTY_BS);
                    }
                    for (int i = 0; i < afterLength; i++) {
                        sendTtyBytes(TTY_DEL);
                    }
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    // IMEs that translate their keys themselves arrive here
                    // rather than as key events on the view.
                    byte[] bytes = ttyKeyBytes(event);
                    if (bytes != null) {
                        if (event.getAction() == KeyEvent.ACTION_DOWN) {
                            sendTtyBytes(bytes);
                        }
                        return true;
                    }
                    return super.sendKeyEvent(event);
                }
            };
        }
    }

    // Exit code of the most recent guest, delivered by the native exit
    // callback. The callback fires on the guest thread while the guest is
    // still winding down, so it only records the value; the exit-monitor
    // thread publishes it to the UI once nativeIsGuestRunning() has cleared.
    private volatile int lastExitCode = -1;

    // The guest the UI drives (Run/Stop/Suspend, touch, console input). Set
    // when a run is created, cleared when it exits. All run-scoped native
    // calls take an id; -1 means "the active one" on the native side too.
    private volatile int activeGuestId = -1;

    // Assets file name of the guest currently launched, or null when none runs.
    // Remembered so an Intent asking for the same guest is not torn down.
    private String currentGuestApp;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Acquire the process-wide native host. Native init/destroy is now
        // reference-counted by RvvmHost, so multiple Activities can share it.
        RvvmHost host = RvvmHost.getInstance();
        host.acquire();

        // Find views
        statusText = findViewById(R.id.statusText);
        startButton = findViewById(R.id.startButton);
        suspendButton = findViewById(R.id.suspendButton);
        stopButton = findViewById(R.id.stopButton);

        // Populate the launcher menu from assets
        populateGuestApps();

        // Honor an explicit "which guest to run" Intent before the auto-start
        // path picks its default. The launch itself happens in
        // maybeAutoStartGuest(), so only the selection is applied here.
        applyGuestSelectionFromIntent(getIntent());

        // The console owns the workspace; the guest's graphics output lives in
        // floating windows above it (one per run, created with the run).
        workspace = findViewById(R.id.workspace);
        ttyViewport = findViewById(R.id.ttyViewport);
        ttyView = findViewById(R.id.ttyView);
        ttyView.setSurfaceTextureListener(ttyTextureListener);
        initTtyInput();
        initTtyScrolling();

        glTaskbar = findViewById(R.id.glTaskbar);

        // The workspace shrinks under the windows when the soft keyboard comes
        // up: pull every card back inside so none ends up half off-screen.
        workspace.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            for (GlWindowCard card : glCards.values()) {
                card.clampToWorkspace();
            }
        });

        // Setup the taskbar: the Start button opens the launcher menu (tapping
        // an app starts a NEW run beside the running ones); SUSPEND/STOP act
        // on the focused window.
        startButton.setOnClickListener(v -> showStartMenu());
        suspendButton.setOnClickListener(v -> toggleSuspendGuest());
        stopButton.setOnClickListener(v -> stopGuestElf());
        updateButtonStates();

        // Initialize native RVVM
        initializeRvvm(host);
    }

    private void initializeRvvm(RvvmHost host) {
        try {
            // host.acquire() (in onCreate) has booted the process-wide native
            // host; this Activity now owns a reference for its lifetime.
            isInitialized = true;

            // Pin the virtual panel before any guest can observe a geometry:
            // after that the call is refused, by design (the buffer is what the
            // guest is mid-frame on).
            host.setPanelSize(PANEL_W, PANEL_H);

            // Record the guest's exit code as it fires (guest thread). The UI
            // is updated by the guest-exit-monitor, not here: the callback
            // runs while the guest thread has not finished unwinding yet.
            host.setExitListener((guestId, code) -> {
                // Only the guest the UI drives updates the status line; a
                // background run's exit is visible through its own monitor.
                if (guestId == activeGuestId) {
                    lastExitCode = code;
                }
                Log.i(TAG, "Guest exit callback: guest " + guestId + " code " + code);
            });

            // Guest console I/O: rendered in the overlay and persisted to a
            // file.  Both callbacks fire on the guest thread; anything that
            // touches a view hops to the UI thread first.
            host.setConsoleListener(new RvvmNative.ConsoleListener() {
                @Override
                public void onOutput(int guestId, String line) {
                    handleGuestOutput(guestId, line);
                }
            });

            host.setFrameCallback(guestId -> handleFirstFrame(guestId));

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
        if (!isInitialized || activeGuestId < 0) {
            return;
        }
        Log.i(TAG, "Lifecycle -> guest " + activeGuestId + ": cmd=" + cmd);
        RvvmNative.nativePostLifecycleCmd(activeGuestId, cmd);
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
        if (!isInitialized || hasAutoStarted) {
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

        // This run's guest handle: created before its card, because the card is
        // keyed by the guest id. The slot only feeds the table; the machine is
        // not created until startRun() below (nativeRunElf).
        final int guestId = RvvmNative.nativeCreateGuest();
        if (guestId < 0) {
            statusText.setText("No guest slot left");
            Toast.makeText(this, R.string.no_slot_left, Toast.LENGTH_SHORT).show();
            Log.i(TAG, "No guest slot left");
            return;
        }
        RvvmNative.nativeSetActiveGuest(guestId);
        activeGuestId = guestId;

        // The card first: a run gets its own window, created waiting at 1x1
        // with its surface coming up on the next UI pass (the framework creates
        // it after this method returns). A guest needing the window - an EGL
        // guest asks for one in its very first statements, a GameActivity guest
        // draws only after INIT_WINDOW - is held back here and picked up by the
        // card's surfaceCreated (glCardHost), which hands native the window
        // before calling startRun.
        GlWindowCard card = createCard(guestId, elfName);
        updateButtonStates();

        if (!card.isSurfaceReady()) {
            glRunPendingId = guestId;
            pendingElfName = elfName;
            pendingElfPath = elfFile.getAbsolutePath();
            statusText.setText("Waiting for the graphics window...");
            Log.i(TAG, "Run " + guestId + " held back until the card's surface is up");
            return;
        }

        startRun(guestId, elfName, elfFile.getAbsolutePath());
    }

    /** A run whose card now has its surface: seed the lifecycle state and hand
     *  the ELF to the core. Called straight through when the card's surface was
     *  already up, from onCardSurfaceCreated otherwise. */
    private void startRun(int guestId, String elfName, String elfPath) {
        statusText.setText("Running: " + elfName + "\nPath: " + elfPath);

        // From here on this is a run of its own: it gets a generation, and the
        // signals it will leave behind (a first frame, an exit) are stamped
        // with it. Bumped only once the run is really starting, so a refused or
        // held-back attempt does not invalidate the run that is still going.
        final int gen = ++runGeneration;

        // New run: a fresh log file. The console on screen is the guest's own
        // TTY, which keeps the previous run's last screen until new output
        // arrives, so there is nothing to reset here.
        openGuestLogFile(elfName);

        replayGuestStartupState();

        boolean started = RvvmNative.nativeRunElf(guestId, elfPath, null);
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
            while (RvvmNative.nativeIsGuestRunning(guestId)) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
            }
            int code = lastExitCode;
            runOnUiThread(() -> {
                /* Everything here belongs to the run that just ended, and its
                 * card goes with it. A run can start in between - the
                 * launcher Activity is singleTask, so "am start --es guest ..."
                 * goes through onNewIntent and can launch a new guest while
                 * this callback is still on its way to the UI thread - and
                 * applying a dead run's exit to it would close ITS card and
                 * overwrite the status line. The generation is what tells the
                 * two apart. */
                if (gen != runGeneration) {
                    Log.i(TAG, "Exit of run " + gen + " ignored: run " + runGeneration + " owns the foreground now");
                } else {
                    currentGuestApp = null;
                    closeGuestLogFile();
                    statusText.setText("Guest exited: " + elfName + " (exit " + code + ")");
                    Log.i(TAG, "Guest exited: " + elfName + " (exit " + code + ")");
                    // Nothing left to type into; the frozen last screen stays as
                    // it is, but the keyboard should not sit over it.
                    hideTtyKeyboard();
                }
                // The guest is gone: its card goes with it, the way closing an
                // application takes its window with it. The next run brings a
                // new one up - with whatever frame that guest produces.
                suspendedGuests.remove(guestId);
                GlWindowCard card = glCards.get(guestId);
                if (card != null) {
                    dismissCard(card);
                }
                // A foreground run's exit hands the console to the next card
                // still alive (or to the last retired screen when none is).
                if (guestId == activeGuestId) {
                    Integer next = null;
                    for (Integer id : glCards.keySet()) {
                        if (next == null || id < next) next = id;
                    }
                    activeGuestId = (next != null) ? next : -1;
                    if (next != null) {
                        RvvmNative.nativeSetActiveGuest(next);
                    }
                }
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
        RvvmNative.nativeClearLifecycleCmds(activeGuestId);
        postLifecycleCmd(APP_CMD_START);
        postLifecycleCmd(APP_CMD_RESUME);
        // INIT_WINDOW: the card's surface is handed to native by
        // onCardSurfaceCreated, which queues the window command there - the
        // seed here only carries the state the guest would have seen before
        // its surface existed.
        if (hasWindowFocus()) {
            postLifecycleCmd(APP_CMD_GAINED_FOCUS);
        }
    }

    private void stopGuestElf() {
        stopGuestElf(activeGuestId);
    }

    private void stopGuestElf(int guestId) {
        if (!isInitialized || !RvvmNative.nativeIsGuestRunning(guestId)) {
            return;
        }
        Log.i(TAG, "Stopping guest " + guestId);
        RvvmNative.nativeStopGuest(guestId);
        // stop unsuspends natively (nativeStopGuest resumes first if the
        // guest was parked); clear the request mirror so the label does not
        // say "resume" while the guest is unwinding its way out.
        suspendedGuests.remove(guestId);
        if (guestId == activeGuestId) {
            currentGuestApp = null;
        }
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
                statusText.setText("Selected guest: " + guestApps[i]);
                Log.i(TAG, "Intent selected guest app: " + guestApps[i]);
                return true;
            }
        }
        Log.w(TAG, "Intent requested unknown guest app: " + requested);
        return false;
    }

    /**
     * Launch the selected guest. With one card per run there is nothing to
     * switch: a repeated "am start --es guest ..." simply starts another run
     * beside the running ones - which is the whole point of the multi-run
     * host. Stopping a guest is the close button's or the Stop button's job.
     */
    private void startSelectedGuestIfNeeded() {
        if (!isInitialized || selectedGuestApp == null || selectedGuestApp.isEmpty()) {
            return;
        }
        runGuestElf();
    }

    /**
     * Pause/resume the guest: the native side parks the guest's vCPUs (and the
     * frame clock) without tearing anything down, so the guest keeps its state
     * and continues exactly where it left off on resume.
     */
    private void toggleSuspendGuest() {
        if (!isInitialized || !RvvmNative.nativeIsGuestRunning(activeGuestId)) {
            return;
        }
        if (suspendedGuests.contains(activeGuestId)) {
            RvvmNative.nativeResumeGuest(activeGuestId);
            suspendedGuests.remove(activeGuestId);
            statusText.setText("Guest resumed");
            Log.i(TAG, "Guest resumed");
        } else {
            RvvmNative.nativeSuspendGuest(activeGuestId);
            suspendedGuests.add(activeGuestId);
            statusText.setText("Guest suspended");
            Log.i(TAG, "Guest suspended");
        }
        updateButtonStates();
    }

    private void updateButtonStates() {
        boolean running = RvvmNative.nativeIsGuestRunning(activeGuestId);
        // suspendedGuests is the request view, so the label flips the moment
        // the user clicks - before the vCPUs have actually parked (which can
        // lag by a blocking host syscall). The teardown flow that needs to
        // know "is it safe to tear down the window" queries
        // nativeIsGuestParked instead; that one only returns true once every
        // vCPU has reached the park point.
        boolean suspended = running && suspendedGuests.contains(activeGuestId);
        suspendButton.setEnabled(running);
        suspendButton.setText(suspended ? R.string.resume_guest : R.string.suspend_guest);
        stopButton.setEnabled(running);
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

    // --- Guest console output -------------------------------------------------
    //
    // The console on screen is the guest's own TTY: fd 1/2 is parsed into a
    // libvterm screen by the core and read back through nativeTtySnapshot(),
    // so guest output and host keyboard echo arrive by the same path. That is
    // the only console now - the text overlay this Activity used to paint over
    // the SurfaceView duplicated it, and only existed because the tab that hid
    // the console needed a console of its own.
    //
    // The raw line stream is still appended to a per-run log file under
    // files/logs/, which is what makes a run readable after the guest is gone.

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

    /** One line of guest output, for the log file. Guest thread. */
    private void handleGuestOutput(int guestId, String line) {
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
    }

    /**
     * The first presented frame is on the surface. Guest thread.
     *
     * This is what the window's visibility is driven by: the card has been
     * waiting at 1x1 px (see the content-driven visibility note), and one frame
     * is all it takes to bring it up. A guest that never draws never gets here,
     * so it never puts a window on screen at all; a guest that does gets its
     * window at the moment it has something to put in it.
     */
    private void handleFirstFrame(final int guestId) {
        Log.i(TAG, "First frame presented by guest " + guestId);
        /* The frame carries the id of the run that drew it (native resolves it
         * through the calling cmdpost), so attribution is exact: one guest's
         * frame can never reveal another guest's card - which matters, because
         * revealing a card resizes its surface and tears down the 1x1 window
         * the guest may still be bringing EGL up on. The run's own card, and
         * only it, is revealed by the run's own frame. */
        runOnUiThread(() -> {
            GlWindowCard card = glCards.get(guestId);
            if (card != null && RvvmNative.nativeIsGuestRunning(guestId)) {
                card.revealOnFirstFrame();
            }
        });
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
        if (RvvmNative.nativeIsGuestRunning(activeGuestId)) {
            RvvmNative.nativeStopGuest(activeGuestId);
        }
        // Stop the console bridge before the native side goes away, so no
        // callback can reach this half-torn-down Activity.
        RvvmNative.nativeSetConsoleListener(null);
        RvvmNative.nativeSetFrameCallback(null);
        closeGuestLogFile();
        // Cleanup native resources through RvvmHost singleton
        if (isInitialized) {
            RvvmHost.getInstance().release();
            isInitialized = false;
        }
    }
}