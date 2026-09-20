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
     * Create a guest run and return its id (or -1 when the run table is full).
     *
     * The rest of this class's run-scoped calls address "the" active guest
     * rather than taking an id on every signature: they are driven from the UI
     * thread, which serializes its own calls, so {@link #nativeSetActiveGuest}
     * followed by the calls that seed and start that guest is the whole
     * contract. A host that only ever runs one guest never needs these - the
     * active guest is created on demand.
     */
    public static native int nativeCreateGuest();

    /**
     * End a guest that never started. A guest that did start ends itself in its
     * own thread, and this call refuses to free a run that thread is standing
     * in.
     */
    public static native void nativeDestroyGuest(int guestId);

    /**
     * Make the given guest the one the run-scoped calls address. Returns false
     * when there is no guest with that id.
     */
    public static native boolean nativeSetActiveGuest(int guestId);

    /**
     * Clean up native resources.
     * Must be called when the app is destroyed.
     */
    public static native void nativeDestroy();

    /**
     * Set the native window for rendering, bound to the given guest: each run
     * owns its own surface (its floating card), so CPU locks and EGL window
     * surfaces for that guest work on this window only.
     * @param surface The Surface to render to, or null when it goes away
     * @param guestId guest the surface belongs to, or -1 for the active one
     */
    public static native void nativeSetWindow(Surface surface, int guestId);

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
     * Pin the virtual panel: the pixel geometry the guest renders into and
     * which {@link #nativePostMotionEvent} coordinates are expressed in.
     *
     * Without this the panel is latched from the first surface size the native
     * side sees, which ties the guest's resolution to the size of the floating
     * graphics window - and that window moves. Pinning it keeps the guest on a
     * fixed geometry (720p landscape by default) with the window acting purely
     * as a viewport onto it.
     *
     * Must be called before the guest observes a window geometry (i.e. before
     * starting it); once a guest has a buffer, the call is ignored.
     *
     * @param width  Panel width in pixels
     * @param height Panel height in pixels
     */
    public static native void nativeSetPanelSize(int width, int height);

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
      * Clear all lifecycle commands from the queue for the given guest.
       * @param guestId guest whose queue is cleared, or -1 for the active one
       * @param cmd The lifecycle command (APP_CMD_*)
       */
    public static native void nativeClearLifecycleCmds(int guestId);

    /**
      * Post a lifecycle command to the native queue for the given guest.
       * @param guestId guest to deliver the command to, or -1 for the active one
       * @param cmd The lifecycle command (APP_CMD_*)
       */
    public static native void nativePostLifecycleCmd(int guestId, int cmd);

    /**
     * Post a motion event to the guest-facing input queue.
     * All pointers of the MotionEvent are forwarded so the guest sees true
     * multi-touch input.
     * @param guestId guest to deliver to, or -1 for the active one
     * @param xs Pointer X coordinates, indexed by pointer index
     * @param ys Pointer Y coordinates, indexed by pointer index
     * @param ids Pointer IDs (MotionEvent.getPointerId)
     * @param pointerCount Number of valid entries in the arrays
     * @param action Raw motion action (AMOTION_EVENT_ACTION_*). For
     *               ACTION_POINTER_DOWN/UP this carries the pointer index in
     *               the upper bits, matching the GameActivity ABI.
     * @param eventTime Event timestamp in nanoseconds
     */
    public static native void nativePostMotionEvent(int guestId, float[] xs, float[] ys, int[] ids,
                                                    int pointerCount, int action, long eventTime);

    /**
     * Run a RISC-V ELF program in the given guest's machine.
     * @param guestId guest to run the ELF in, or -1 for the active one
     * @param elfPath Path to the ELF file
     * @param args Optional command-line arguments
     * @return true if the guest started successfully
     */
    public static native boolean nativeRunElf(int guestId, String elfPath, String[] args);

    /**
     * Check if the given guest is currently running.
     * @param guestId guest to query, or -1 for the active one
     * @return true if guest is running
     */
    public static native boolean nativeIsGuestRunning(int guestId);

    /**
      * Stop the given guest (it unwinds like a normal exit).
      * @param guestId guest to stop, or -1 for the active one
      */
    public static native void nativeStopGuest(int guestId);

    /**
     * Suspend the given guest: park its vCPUs and the frame clock.
     * No-op when no guest is running or it is already suspended.
     * @param guestId guest to suspend, or -1 for the active one
     */
    public static native void nativeSuspendGuest(int guestId);

    /**
     * Resume a guest suspended by {@link #nativeSuspendGuest(int)}.
     * No-op when no guest is running or it is not suspended.
     * @param guestId guest to resume, or -1 for the active one
     */
    public static native void nativeResumeGuest(int guestId);

/**
       * Check whether the given guest's vCPUs are parked (suspended).
       * @param guestId guest to query, or -1 for the active one
       * @return true if the guest is parked (all vCPUs in syscall wait)
       */
    public static native boolean nativeIsGuestParked(int guestId);

    /**
      * Set a callback invoked when a guest exits.
      * Called on the guest's vCPU thread while the run is winding down.
      * @param callback Listener for guest exit events, or null to clear
      */
    public static native void nativeSetExitCallback(ExitListener callback);

    /**
      * Listener interface for guest exit events.
      */
    public interface ExitListener {
        void onExit(int guestId, int exitCode);
    }

    /**
     * Set a listener that receives the guest's console output (stdout/stderr).
     * Lines are delivered on the guest thread as they are written.
     * @param listener Output listener, or null to clear
     */
    public static native void nativeSetConsoleListener(ConsoleListener listener);

    /**
     * Snapshot of the persistent guest TTY (libvterm screen matrix) into
     * {@code out}, one cell = 4 ints: [0] UCS-4 codepoint (0 = erased),
     * [1] fg ARGB, [2] bg ARGB, [3] flags: bit0 bold, bit1 underline,
     * bit2 reverse (already swapped into fg/bg), bit3 wide glyph, bit4 cursor
     * cell. The rows are the view's window into history: it follows the live
     * screen until {@link #nativeTtyScrollBy(int)} drags it back.
     *
     * {@code out} must hold at least TTY_MAX_ROWS*80*4 ints: the grid height is
     * whatever {@link #nativeTtyResize(int, int)} last set.
     *
     * @param guestId whose console to snapshot, or -1 for the foreground one
     *                (falling back to the last retired session while no run
     *                is alive)
     * @param info when non-null, filled with {lines the window sits above the
     *             live bottom, lines kept in the scrollback}
     * @return number of cells written (rows*80), or 0 when no TTY exists
     */
    public static native int nativeTtySnapshot(int guestId, int[] out, int[] info);

    /**
     * Resize the guest TTY grid to the console viewport.
     *
     * The renderer picks the font size that fits 80 columns into the view
     * width, then reports how many whole rows that leaves room for; native
     * resizes the libvterm grid so the snapshot, the scrollback window and the
     * guest's own TIOCGWINSZ all agree on the height. The rows are clamped
     * natively, and the request is remembered even before the first guest
     * starts, so the VTerm is created at the right height.
     *
     * The column count is fixed at 80 - the console scales the font to the view
     * width instead of changing it - so {@code cols} is pinned to TTY_COLS.
     *
     * @param rows grid height in cells
     * @param cols grid width in cells (fixed 80)
     */
    public static native void nativeTtyResize(int rows, int cols);

    /**
     * Drag the console's view through its scrollback, in whole terminal rows:
     * positive looks back into history. Clamped to what is stored, and dragging
     * back past the bottom re-pins the view to the live screen. While the view
     * is scrolled back it stays on the lines being read as new output arrives;
     * typing, and starting a guest, bring it home.
     *
     * @param guestId whose console to scroll, or -1 for the foreground one
     */
    public static native void nativeTtyScrollBy(int guestId, int lines);

    /**
     * Repaint hint for the TTY console: bumped by native on every guest
     * output burst. Poll at ~30 Hz and re-snapshot + redraw only when the
     * value changes.
     *
     * @param guestId whose console serial to read, or -1 for the foreground one
     */
    public static native int nativeTtySerial(int guestId);

    /**
     * Send host keyboard input to the guest's virtual TTY - the input half of
     * the console.
     *
     * {@code bytes} is the byte sequence a real terminal would receive from its
     * keyboard: UTF-8 text for typed characters, '\r' for Enter, 0x7F for
     * Backspace, "\u001b[A" (ESC [ A) and siblings for the arrow keys,
     * 0x03/0x04 for Ctrl-C/Ctrl-D (with ISIG on, Ctrl-C discards the pending
     * line and stops the guest). Native runs them through the line discipline
     * the guest's termios advertises (ICRNL, canonical line assembly with erase,
     * ECHO) and queues the cooked result for the guest's read(0, ...).
     *
     * Because the guest never saw the keystroke, native echoes it into the
     * libvterm screen itself - typed characters appear through
     * {@link #nativeTtySnapshot(int[], int[])} just like guest output. No-op when no
     * guest is running or no TTY is attached.
     *
     * @param guestId guest to type into, or -1 for the active one
     * @param bytes Terminal input bytes, already encoded
     */
    public static native void nativeTtyInput(int guestId, byte[] bytes);

/**
       * Listener for the guest's console I/O.  onOutput receives one line per
       * call (line breaks normalized).
       */
    public interface ConsoleListener {
        void onOutput(int guestId, String line);
    }

    /**
     * Callback for graphics frame lifecycle.  onFirstFrame fires once per
     * guest run when the first frame has actually reached the screen --
     * either through the CPU unlock path or a successful eglSwapBuffers --
     * which is the UI's cue to reveal that guest's card.
     */
    public interface FrameCallback {
        void onFirstFrame(int guestId);
    }

    /**
     * Set a callback invoked when a guest renders its first frame.
     * Called on the guest thread; hop to the UI thread for view work.
     * @param callback Frame callback, or null to clear
     */
    public static native void nativeSetFrameCallback(FrameCallback callback);
}
