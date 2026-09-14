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
     * All pointers of the MotionEvent are forwarded so the guest sees true
     * multi-touch input.
     * @param xs Pointer X coordinates, indexed by pointer index
     * @param ys Pointer Y coordinates, indexed by pointer index
     * @param ids Pointer IDs (MotionEvent.getPointerId)
     * @param pointerCount Number of valid entries in the arrays
     * @param action Raw motion action (AMOTION_EVENT_ACTION_*). For
     *               ACTION_POINTER_DOWN/UP this carries the pointer index in
     *               the upper bits, matching the GameActivity ABI.
     * @param eventTime Event timestamp in nanoseconds
     */
    public static native void nativePostMotionEvent(float[] xs, float[] ys, int[] ids,
                                                    int pointerCount, int action, long eventTime);

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

    /**
     * Suspend the running guest: park its vCPUs and the frame clock.
     * No-op when no guest is running or it is already suspended.
     */
    public static native void nativeSuspendGuest();

    /**
     * Resume a guest suspended by {@link #nativeSuspendGuest()}.
     * No-op when no guest is running or it is not suspended.
     */
    public static native void nativeResumeGuest();

    /**
     * Check whether the running guest is suspended.
     * @return true if the guest is currently suspended
     */
    public static native boolean nativeIsGuestSuspended();

    /**
      * Set a callback invoked when the guest exits.
      * Called on the guest thread after rvvm_user_linux returns.
      * @param callback Listener for guest exit events, or null to clear
      */
    public static native void nativeSetExitCallback(ExitListener callback);

    /**
      * Listener interface for guest exit events.
      */
    public interface ExitListener {
        void onExit(int exitCode);
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
     * bit2 reverse (already swapped into fg/bg), bit3 wide glyph.
     * @return number of cells written (24*80), or 0 when no TTY exists
     */
    public static native int nativeTtySnapshot(int[] out);

    /**
     * Repaint hint for the TTY console: bumped by native on every guest
     * output burst. Poll at ~30 Hz and re-snapshot + redraw only when the
     * value changes.
     */
    public static native int nativeTtySerial();

    /**
     * Send host keyboard input to the guest's virtual TTY - the input half of
     * the console.
     *
     * {@code bytes} is the byte sequence a real terminal would receive from its
     * keyboard: UTF-8 text for typed characters, '\r' for Enter, 0x7F for
     * Backspace, "\u001b[A" (ESC [ A) and siblings for the arrow keys,
     * 0x03/0x04 for Ctrl-C/Ctrl-D. Native runs them through the line discipline
     * the guest's termios advertises (ICRNL, canonical line assembly with erase,
     * ECHO) and queues the cooked result for the guest's read(0, ...).
     *
     * Because the guest never saw the keystroke, native echoes it into the
     * libvterm screen itself - typed characters appear through
     * {@link #nativeTtySnapshot(int[])} just like guest output. No-op when no
     * guest is running or no TTY is attached.
     *
     * @param bytes Terminal input bytes, already encoded
     */
    public static native void nativeTtyInput(byte[] bytes);

    /**
      * Listener for the guest's console I/O. onOutput receives one line per
      * call (line breaks normalized); onFirstFrame fires once per guest run
      * when the first frame has actually reached the screen - either through
      * the CPU unlock path or a successful eglSwapBuffers - which is the UI's
      * cue to hand the surface over to the rendered content.
      */
    public interface ConsoleListener {
        void onOutput(String line);
        void onFirstFrame();
    }
}
