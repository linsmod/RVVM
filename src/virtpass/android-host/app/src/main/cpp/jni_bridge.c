/*
 * jni_bridge.c - JNI bridge for Android NDK API proxy
 *
 * This file provides the JNI interface between Java (Android) and
 * the native rvvm library. It allows the Android side to:
 * - Handle lifecycle events
 * - Handle window operations (lock/unlock)
 * - Run RISC-V Guest ELF programs via rvvm-user
 *
 * Sensors are not part of this surface: the platform ASensorManager backs the
 * Virtpass sensor ABI through vp_sensor_android.c, so the guest's sensor data
 * never crosses Java.
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/configuration.h>  /* ACONFIGURATION_* constants */
#include <android/choreographer.h> /* AChoreographer_* (display vsync) */
#include <android/looper.h>        /* ALooper_* (vsync pump) */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

/* Include vp_cmdpost API */
#include "virtpass/vp_cmdpost.h"
#include "virtpass/vp_session.h"

/* System EGL/GLES backend (marshalled GL dispatch) */
#include "android_gl_host.h"

/* Sensor backend (platform ASensorManager / ASensorEventQueue) */
#include "vp_sensor_android.h"

/* Include rvvm-user API */
#include "rvvm_user.h"

#define LOG_TAG "RVVM-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Global references to Java objects */
static JavaVM* g_jvm = NULL;
static JNIEnv* g_env = NULL;

/* This host's cmdpost instance for the run in progress: made by
 * nativeClearLifecycleCmds() (the run preparation Java calls before it seeds
 * the startup sequence, which has to land in this instance), bound to the run's
 * machine with rvvm_user_set_host_ctx() so the guest's syscalls reach it, and
 * freed by that run's guest thread as it ends. NULL between runs. It is what
 * every cmdpost_* call in this file writes into - that state used to be
 * file-scope globals in vp_cmdpost.c, one set for the whole process.
 *
 * Declared here rather than next to g_guest_machine because the vsync pump
 * (defined above that point) is one of its callers. */
static vp_cmdpost_t* g_cmdpost = NULL;

/* Window from Android (Layer 2: the viewport) */
static ANativeWindow* g_native_window = NULL;

/* ------------------------------------------------------------------
 * Two-layer display model (mirrors the win32 host).
 *
 * Layer 1 - the virtual panel - is host-owned and guest-invisible: the guest
 * can only observe it through the public NDK ABI (window size and
 * AConfiguration). Layer 2 - the real SurfaceView - is merely a viewport and
 * may resize at any time without ever reaching the guest. Keeping the two
 * apart is what stops a surface resize from moving the geometry the guest is
 * already rendering into (that mismatch was what made the unlock copy run off
 * the end of the guest pixel buffer).
 *
 * The geometry state is not kept here: it lives in the session object
 * (virtpass/vp_session.h), which is the same state the win32 host keeps for its
 * own window and the shape a second instance would need a copy of. What is
 * left in this file is what is genuinely Android's business - the window, the
 * locks and the Java callbacks - driving that session under g_surf_cs.
 * ------------------------------------------------------------------ */
static vp_session_t g_session;

/* Guards the session's geometry and the window. Held for short reads/writes
 * only, never across an ANativeWindow_lock()/unlockAndPost() pair. */
static pthread_mutex_t g_surf_cs = PTHREAD_MUTEX_INITIALIZER;

/* GameActivity state */
static int32_t g_lifecycle_cmd_queue[32];
static int32_t g_lifecycle_cmd_count = 0;
static int32_t g_lifecycle_cmd_read = 0;

/* ============================================================
 * Choreographer (Phase 4): display vsync source
 * ============================================================
 * The guest's AChoreographer stubs consume this from two directions:
 *  - fd wakeup (方案 B): the guest registers the write end of its Looper pipe
 *    and asks for one vsync per request; vp_cmdpost_vsync_tick() below writes
 *    the frame time into that fd, so the guest wakes in poll() instead of
 *    polling us.
 *  - blocking WAIT: a guest that could not use the fd path parks in
 *    SYS_ANDROID_CHOREOGRAPHER_WAIT and is released by the condvar below.
 *
 * A dedicated thread owns the real NDK AChoreographer instance (it is
 * per-thread and needs a Looper that is actually pumped), keeps one frame
 * callback armed at all times, and publishes each vsync's frame time. Guest
 * consumers only observe the latest tick, so a slow or stalled guest cannot
 * hold up the vsync source itself.
 */
static pthread_t       g_vsync_thread;
static ALooper*        g_vsync_looper = NULL;
static volatile int    g_vsync_running = 0;
static pthread_mutex_t g_vsync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vsync_cond  = PTHREAD_COND_INITIALIZER;
static int64_t         g_vsync_frame_time = 0;
static uint64_t        g_vsync_seq = 0;
static int             g_vsync_warned = 0;

static void on_vsync_frame(long frame_time_nanos, void* data)
{
    (void)data;

    pthread_mutex_lock(&g_vsync_mutex);
    g_vsync_frame_time = frame_time_nanos;
    g_vsync_seq++;
    pthread_cond_broadcast(&g_vsync_cond);
    pthread_mutex_unlock(&g_vsync_mutex);

    /* fd path: hand the frame time to the guest's Looper pipe if it asked for
     * this vsync. Cheap no-op while nothing is armed. */
    vp_cmdpost_vsync_tick(g_cmdpost, (int64_t)frame_time_nanos);

    /* Keep the tick continuous: re-arm immediately from inside the callback. */
    if (g_vsync_running) {
        AChoreographer* choreographer = AChoreographer_getInstance();
        if (choreographer) {
            AChoreographer_postFrameCallback(choreographer, on_vsync_frame, NULL);
        }
    }
}

/*
 * Owns the AChoreographer instance. AChoreographer is per-thread and only
 * dispatches while its Looper is pumped, so this thread prepares a Looper and
 * then simply drains it; every drained frame callback publishes a new tick.
 */
static void* vsync_thread_func(void* arg)
{
    (void)arg;

    g_vsync_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

    AChoreographer* choreographer = AChoreographer_getInstance();
    if (!choreographer) {
        LOGE("vsync: AChoreographer_getInstance() failed, guest will fall back");
        return NULL;
    }

    AChoreographer_postFrameCallback(choreographer, on_vsync_frame, NULL);
    LOGI("vsync: AChoreographer source started");

    while (g_vsync_running) {
        int result = ALooper_pollOnce(-1, NULL, NULL, NULL);
        if (result == ALOOPER_POLL_ERROR) {
            LOGE("vsync: ALooper_pollOnce() error, stopping vsync source");
            break;
        }
    }

    LOGI("vsync: AChoreographer source stopped");
    return NULL;
}

/*
 * Blocks the caller until the next display vsync and returns its frame time in
 * nanoseconds. Returns -1 when the source is unavailable, so the guest can
 * fall back to its own clock instead of stalling.
 */
static int64_t android_vsync_wait(void)
{
    int64_t frame_time = -1;

    pthread_mutex_lock(&g_vsync_mutex);

    uint64_t start_seq = g_vsync_seq;
    while (g_vsync_running && g_vsync_seq == start_seq) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000 * 1000;   /* 100ms safety net */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&g_vsync_cond, &g_vsync_mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }

    if (g_vsync_seq != start_seq) {
        frame_time = g_vsync_frame_time;
    } else if (!g_vsync_warned) {
        g_vsync_warned = 1;
        LOGW("vsync: no tick within 100ms, guest falls back to its own clock");
    }

    pthread_mutex_unlock(&g_vsync_mutex);
    return frame_time;
}

/*
 * Start the AChoreographer vsync source thread. Idempotent: a no-op while the
 * source already runs, so suspend/resume cycles can call it unconditionally.
 */
static void jni_vsync_start(void)
{
    if (g_vsync_running) {
        return;
    }

    g_vsync_running = 1;
    if (pthread_create(&g_vsync_thread, NULL, vsync_thread_func, NULL) != 0) {
        g_vsync_running = 0;
        LOGE("vsync: failed to start AChoreographer thread, guest will fall back");
    }
}

/*
 * Stop the vsync source thread and wait for it to unwind. Idempotent.
 * Marks the source as lost so a guest currently blocked on the vsync fd is
 * released to its own clock instead of waiting for a tick that will not come.
 */
static void jni_vsync_stop(void)
{
    if (!g_vsync_running) {
        return;
    }

    g_vsync_running = 0;

    /* Release a guest blocked in poll() on the vsync fd. */
    vp_cmdpost_vsync_source_lost(g_cmdpost);

    /* Wake a thread parked in ALooper_pollOnce(-1) ... */
    if (g_vsync_looper) {
        ALooper_wake(g_vsync_looper);
    }
    /* ... and one parked in android_vsync_wait(). */
    pthread_mutex_lock(&g_vsync_mutex);
    pthread_cond_broadcast(&g_vsync_cond);
    pthread_mutex_unlock(&g_vsync_mutex);

    pthread_join(g_vsync_thread, NULL);
    g_vsync_looper = NULL;
}

/* Guest execution state */
static pthread_t g_guest_thread;
static int g_guest_running = 0;
/* Host-side mirror of the suspend request. rvvm_user_is_suspended() would answer
 * the same, but only by dereferencing the machine - which the guest thread frees
 * the instant it exits, while this flag stays valid from any thread (the UI
 * polls it). */
static volatile int g_guest_suspended = 0;
static char g_guest_elf_path[512];
static int g_guest_argc = 0;
static char* g_guest_argv[16];
/* Userland machine for the running guest. Created in nativeRunElf(), freed by
 * rvvm_user_linux_ex() on the guest thread. Each run binds it to g_cmdpost (the
 * host's cmdpost instance, declared at the top of this file). */
static rvvm_machine_t* g_guest_machine = NULL;

/* Guest exit callback (Java object reference, held by global ref) */
static jobject g_exit_listener = NULL;

/* ============================================================
 * Guest console I/O: native -> Java bridge
 * ============================================================
 * The guest's stdout/stderr arrive here from rvvm_user's io callback
 * (jni_guest_output), are buffered into lines, and handed to the Java
 * ConsoleListener: MainActivity renders them in the on-surface overlay and
 * persists them to a log file. jni_guest_first_frame() marks the moment the
 * first frame actually reached the screen, which switches the overlay off
 * until the guest exits.
 *
 * A pending line is also flushed when the guest exits, so output that does
 * not end with a newline is not lost.
 * ============================================================ */
static jobject    g_console_listener = NULL;
static jmethodID  g_console_output_mid = NULL;
static jmethodID  g_console_first_frame_mid = NULL;

/* The pending line and the first-frame flag are per-run state and live in
 * g_session (virtpass/vp_session.h): this mutex is what serialises access to
 * them, since the session itself takes no locks. */
static pthread_mutex_t g_console_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * Guest virtual TTY: host-owned libvterm session
 * ============================================================
 * Mirrors the win32 host: the process owns one persistent console session
 * (rvvm_tty_open: 80 columns - the renderer scales the font to the view width -
 * and a host-driven row count that follows the console viewport, see
 * nativeTtyResize), attached to each guest run with rvvm_tty_attach(). Guest
 * fd 1/2 output is parsed into the screen matrix there, so the terminal
 * survives guest exit (the TextureView keeps showing the frozen last screen
 * until the next run resets it) and guest libc's isatty() probes are answered
 * (line-buffered stdio).
 *
 * The session owns everything the console tab draws: the packed cells, the
 * scrollback, the view position and the repaint serial the Java-side poller
 * compares (nativeTtySerial() / nativeTtySnapshot()). This bridge is left with
 * the JNI plumbing and nothing else - no libvterm calls, no machine pointer.
 * With a session attached rvvm_user routes fd 1/2 into the VTerm, so the old
 * io_callback console bridge no longer sees stdout: the TextureView console
 * replaces the text overlay.
 *
 * That split is also why the rendering entries below keep working after the
 * guest is gone: the session's lock travels with the session, not with a
 * machine, so there is nothing left to be missing when the run ends.
 *
 * NOTE on in-place '\r' updates (progress lines): these are governed by the
 * GUEST's stdio, not by this renderer. fd 1/2 is answered as a tty
 * (user_tty_ioctl -> TCGETS), so guest stdio is line buffered and '\r' does
 * not flush. The one-second loop of guest-samples/test_audio.c
 * ("wrote N / M frames\r", no fflush) therefore sat in the guest's stdio
 * buffer and arrived as a single burst at the trailing "\n    Done: ..." -
 * measured with a write/snapshot probe: 1051 ms during which not one write
 * reached the VTerm, then every '\r' at once. Having nothing to draw for that
 * second looks exactly like dropped rows, but ONLCR -> libvterm -> snapshot
 * -> drawTty were correct throughout, as was the final screen. How live such
 * a line looks depends only on writes/second vs the guest stdio buffer size
 * (test_audio prints once per AAudio burst: burst=2048 -> ~24 writes/s, under
 * the buffer, nothing flushed mid-loop; burst=120 -> hundreds of writes, the
 * buffer overflows repeatedly and the line updates live). The win32 host only
 * looks better because its WASAPI burst is smaller. A guest that wants live
 * progress must fflush (test_tty.c does). Do not "fix" this in the renderer.
 * ============================================================ */
#define TTY_DEF_ROWS 24
#define TTY_MAX_ROWS 200
#define TTY_COLS     80

/* Grid height, in cells. The columns stay fixed at TTY_COLS - the renderer
 * scales the font to the view width rather than moving the column count - but
 * the row count is host-driven: the Android console measures how many whole
 * rows fit under the cell height it laid the grid out with and reports it
 * through nativeTtyResize(), so the VTerm, the snapshot window and the guest's
 * TIOCGWINSZ all agree on the height. */
static int            g_tty_rows   = TTY_DEF_ROWS;

/* The console session (rvvm_tty_open). It owns the VTerm, the lock that
 * serializes access to it, the scrollback and the view state, and it outlives
 * every guest: the renderer keeps snapshotting and scrolling the frozen last
 * screen with no machine - and no libvterm knowledge - of its own. */
static rvvm_tty_t* g_tty = NULL;

/* The session is opened on first use: the renderer lays its grid out as soon as
 * the console tab is up, which can precede the first guest, so
 * nativeTtyResize() remembers a height that arrives before this. */
static void jni_tty_init(void)
{
    if (g_tty) {
        return;
    }
    g_tty = rvvm_tty_open(g_tty_rows, TTY_COLS);
    if (!g_tty) {
        LOGE("rvvm_tty_open failed");
        return;
    }
    LOGI("Guest TTY session opened (%dx%d)", g_tty_rows, TTY_COLS);
}

/* Cell snapshot for the Java renderer. The session packs rows*cols cells into
 * the buffer below - rvvm_tty_cell_t is exactly 4 x uint32, the layout Java
 * reads: [0] codepoint, [1] fg ARGB, [2] bg ARGB, [3] flags (bold / underline
 * / reverse / wide / cursor, see rvvm_user.h) - and the int[] is filled from it
 * with a copy (the two layouts are identical, but going through memcpy keeps
 * the array's own type honest). info[] gets {lines the view sits above the live
 * bottom, lines stored}, which is what places the scrollbar.
 *
 * Cells, cursor and view come out of one locked pass inside the session, so
 * they cannot disagree by a frame - and the same call works after the guest is
 * gone, which is exactly when the frozen last screen is read back.
 *
 * Returns 0 when there is no session yet, or when the array cannot hold the
 * grid. */
JNIEXPORT jint JNICALL
Java_com_rvvm_android_RvvmNative_nativeTtySnapshot(JNIEnv* env, jobject thiz,
                                                   jintArray out, jintArray info)
{
    static rvvm_tty_cell_t cells[TTY_MAX_ROWS * TTY_COLS];
    (void)thiz;
    if (!g_tty || !out) {
        return 0;
    }
    jsize len = (*env)->GetArrayLength(env, out);
    if (len <= 0) {
        return 0;
    }
    jint* buf = (*env)->GetIntArrayElements(env, out, NULL);
    if (!buf) {
        return 0;
    }

    rvvm_tty_view_t view;
    int n = rvvm_tty_snapshot(g_tty, cells, TTY_MAX_ROWS * TTY_COLS, &view);
    if (n > 0 && len >= (jsize)(n * 4)) {
        memcpy(buf, cells, (size_t)n * sizeof(cells[0]));
    } else {
        n = 0;
    }
    (*env)->ReleaseIntArrayElements(env, out, buf, 0);
    if (n <= 0) {
        return 0;
    }

    /* Handed over after the snapshot: JNI array access can allocate, which must
     * not happen while the session lock is held. */
    if (info && (*env)->GetArrayLength(env, info) >= 2) {
        jint* ibuf = (*env)->GetIntArrayElements(env, info, NULL);
        if (ibuf) {
            ibuf[0] = view.scroll;
            ibuf[1] = view.scrollback_lines;
            (*env)->ReleaseIntArrayElements(env, info, ibuf, 0);
        }
    }
    return n;
}

/* Host-driven console resize: the renderer measures how many whole rows fit
 * under the cell height it laid the grid out with and reports it here, so the
 * snapshot window, the scrollback and the guest's TIOCGWINSZ all agree on the
 * height. `cols` is accepted for symmetry but pinned to TTY_COLS: the renderer
 * scales the font to the view width rather than changing the column count, and
 * the session ignores a different width anyway.
 *
 * A resize is remembered even before the session exists: jni_tty_init() then
 * opens it at the requested height instead of the default. */
JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeTtyResize(JNIEnv* env, jobject thiz,
                                                 jint rows, jint cols)
{
    (void)env; (void)thiz; (void)cols;

    if (rows < 1) {
        rows = 1;
    }
    if (rows > TTY_MAX_ROWS) {
        rows = TTY_MAX_ROWS;
    }
    if (rows == g_tty_rows) {
        return;
    }
    g_tty_rows = rows;

    if (g_tty) {
        rvvm_tty_resize(g_tty, rows, TTY_COLS);   /* bumps the session serial */
    }
}

/* Repaint hint for the Java poller: the session bumps its serial on output,
 * resize, reset and scroll, so a poll that finds it unchanged has nothing to
 * redraw. Cheap enough to call at any rate - it is a plain int read. */
JNIEXPORT jint JNICALL
Java_com_rvvm_android_RvvmNative_nativeTtySerial(JNIEnv* env, jobject thiz)
{
    (void)env; (void)thiz;
    return g_tty ? rvvm_tty_serial(g_tty) : 0;
}

/* Drag the console's view through its scrollback: `lines` is a drag in whole
 * terminal rows, positive looking back into history. The session owns the
 * position: it clamps it, keeps a view dragged back anchored on the lines being
 * read while output arrives, and hands it home when the guest is typed into.
 *
 * No machine is involved here either: the console outlives the guest - it keeps
 * the last screen and the whole scrollback after the run ends (Ctrl-C ends it
 * too, through the core's ISIG handling) - and reading that output back is
 * exactly what the scrollback is for. */
JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeTtyScrollBy(JNIEnv* env, jobject thiz, jint lines)
{
    (void)env; (void)thiz;
    if (g_tty) {
        rvvm_tty_scroll(g_tty, lines);
    }
}

/* Host keyboard -> guest console, the input half of the TTY.
 *
 * `bytes` is what a real terminal receives from its keyboard: UTF-8 text,
 * '\r' for Enter, 0x7F for Backspace, "\x1b[A" and friends for the arrows,
 * 0x03/0x04 for Ctrl-C/Ctrl-D. The core runs the line discipline the guest's
 * termios advertises and queues the cooked bytes for the guest's read(0, ...);
 * typing comes back to the screen through nativeTtySnapshot() because the core
 * echoes it into the session.
 *
 * The input half stays machine-bound (it is the guest's fd 0, and it is the
 * core that wakes a blocked read), but the view state is not: the core brings
 * a view parked in history home before echoing, so the console cannot end up
 * looking dead while the user types. */
JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeTtyInput(JNIEnv* env, jobject thiz, jbyteArray in)
{
    rvvm_machine_t* machine = g_guest_machine;
    jsize len;
    jbyte* buf;

    (void)thiz;
    if (!machine || !in) {
        return;
    }
    len = (*env)->GetArrayLength(env, in);
    if (len <= 0) {
        return;
    }
    buf = (*env)->GetByteArrayElements(env, in, NULL);
    if (!buf) {
        return;
    }
    rvvm_user_tty_input(machine, buf, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, in, buf, JNI_ABORT);
}

/* JNIEnv for the calling thread, attaching it first when needed. The caller
 * must detach when *attached comes back non-zero. */
static JNIEnv* console_env(int* attached)
{
    JNIEnv* env = NULL;
    *attached = 0;
    if (!g_jvm) return NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == JNI_OK) {
        *attached = 1;
        return env;
    }
    return NULL;
}

/* A completed console line, from the session's line assembly. Called with
 * g_console_mutex held; the session's own sanitising already turned the guest's
 * bytes into text NewStringUTF accepts. */
static void on_console_line(void* user, const char* line)
{
    JNIEnv* env;
    int attached;
    jstring str;

    (void)user;

    if (!g_console_listener) {
        return;     /* nobody to deliver to; the line is dropped */
    }
    env = console_env(&attached);
    if (!env) {
        return;
    }
    str = (*env)->NewStringUTF(env, line);
    if (str) {
        (*env)->CallVoidMethod(env, g_console_listener, g_console_output_mid, str);
        (*env)->DeleteLocalRef(env, str);
    }
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/* Called by rvvm_user's io callback for every write to fd 1/2. The splitting
 * into lines lives in the session, so both hosts split identically; what is
 * here is the locking and the crossing into Java. */
void jni_guest_output(const char* data, size_t count)
{
    if (!data || !count) return;

    pthread_mutex_lock(&g_console_mutex);
    vp_session_console_output(&g_session, data, count);
    pthread_mutex_unlock(&g_console_mutex);
}

/* Mark the first presented frame. Called from both present paths: the CPU
 * unlock here in jni_bridge.c and eglSwapBuffers in android_gl_host.c. */
void jni_guest_first_frame(void)
{
    JNIEnv* env;
    int attached;
    int first;

    pthread_mutex_lock(&g_console_mutex);
    first = vp_session_note_first_frame(&g_session);
    if (first) {
        /* Flush whatever output precedes the frame. */
        vp_session_console_flush(&g_session);
    }
    pthread_mutex_unlock(&g_console_mutex);

    if (!first) return;
    if (!g_console_listener || !g_console_first_frame_mid) return;
    env = console_env(&attached);
    if (!env) return;
    (*env)->CallVoidMethod(env, g_console_listener, g_console_first_frame_mid);
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/* Flush a trailing partial line and re-arm the first-frame note. */
static void console_reset(void)
{
    pthread_mutex_lock(&g_console_mutex);
    vp_session_reset_run(&g_session);
    pthread_mutex_unlock(&g_console_mutex);
}

/* Defined with the window callbacks below; called from the guest thread once
 * the guest is gone. */
static void surf_finish_pending_lock(void);

/* Guest thread function */
static void* guest_thread_func(void* arg)
{
    rvvm_machine_t* machine = g_guest_machine;
    /* The cmdpost instance this run was started with. Taken now, while the
     * UI thread is still inside nativeRunElf() (so it cannot be racing us), and
     * released at the end of this function. */
    vp_cmdpost_t* cmdpost = g_cmdpost;
    (void)arg;

    /* Detach: the launcher runs guest after guest, so this thread must release
     * its own resources on exit instead of lingering as a joinable zombie
     * (nothing ever pthread_join()s it). */
    pthread_detach(pthread_self());

    LOGI("Guest thread started, ELF: %s", g_guest_elf_path);
    
    /* Build argc/argv for rvvm_user_linux_ex() */
    /* argv[0] = ELF path, argv[1..] = guest args */
    g_guest_argv[0] = g_guest_elf_path;
    
    /* On non-riscv hosts rvvm_user.c defaults prefix_path to a hardcoded
     * Debian userland path. Disable it so host paths pass through unchanged. */
    putenv("RVVM_USER_PREFIX=");
    
    int result = rvvm_user_linux_ex(machine, g_guest_argc, g_guest_argv, NULL);
    
    LOGI("Guest thread finished with code: %d", result);

    /* The guest is gone: if it died between ANativeWindow_lock() and
     * ANativeWindow_unlockAndPost() (a Stop landing mid-frame), the surface is
     * still locked and would refuse every lock from the next guest. Release it
     * here, where no guest code can run anymore. */
    surf_finish_pending_lock();

    /* rvvm_user_linux_ex() owns and has just freed the machine */
    g_guest_machine = NULL;

    /* This run is over, so its cmdpost instance - and the sensor state hanging
     * off it - goes now, *before* g_guest_running drops: that flag is what Java
     * polls to allow the next Run, and the next run makes its own instance in
     * nativeClearLifecycleCmds(). Releasing it after the flag would let the next
     * run create one and this thread free it. Compare before clearing so a
     * teardown that already dropped the pointer cannot make us clear a
     * successor's. */
    if (g_cmdpost == cmdpost) {
        g_cmdpost = NULL;
    }
    cmdpost_destroy(cmdpost);

    g_guest_running = 0;
    g_guest_suspended = 0;
    
    return NULL;
}

/* Locked window buffer info (kept between lock and unlock) */
static ANativeWindow_Buffer g_locked_buffer;

/* The surface the buffer above came from, holding a reference taken at lock
 * time and dropped at unlock time. The Java thread can destroy the Surface at
 * any moment (surfaceDestroyed / surface replaced), and touching a released
 * Surface aborts inside libgui:
 *   FATAL: 'FORTIFY: pthread_mutex_lock called on a destroyed mutex'
 *   #04 android::Surface::lock()
 *   #05 android::Surface::hook_perform()
 * so a lock()/unlockAndPost() pair must own the object it is working on for
 * its whole duration. g_native_window itself is only ever touched under
 * g_surf_cs. */
static ANativeWindow* g_locked_window = NULL;

/* Take a reference to the current surface, or NULL if there is none. The
 * caller owns the reference and must ANativeWindow_release() it. This is how
 * the guest-driven callbacks obtain the window: they must never use
 * g_native_window directly, since it can be swapped out underneath them. */
static ANativeWindow* surf_acquire(void)
{
    ANativeWindow* w;

    pthread_mutex_lock(&g_surf_cs);
    w = g_native_window;
    if (w) {
        ANativeWindow_acquire(w);
    }
    pthread_mutex_unlock(&g_surf_cs);

    return w;
}

/* Broadcast when a window is installed, under g_surf_cs (nativeSetWindow). */
static pthread_cond_t g_window_cond = PTHREAD_COND_INITIALIZER;

/* The window to bind, with a reference held for the caller - release it with
 * ANativeWindow_release(). NULL when there is none.
 *
 * The GL path has to go through this instead of keeping its own copy of the
 * pointer: the host swaps windows from the UI thread (every surfaceCreated /
 * surfaceChanged) and releases the previous wrapper right away, so a pointer
 * read outside this lock can be freed under the reader - which is exactly how a
 * guest's eglCreateWindowSurface ends up handed a dead ANativeWindow and fails
 * with EGL_BAD_NATIVE_WINDOW. The reference is only needed across the call: EGL
 * keeps one of its own from the moment the EGLSurface exists.
 *
 * A window that is not there yet is given up to wait_ms to arrive.
 *
 * "Not there yet" is the normal state for a moment: the window is the card the
 * host shows, and the host swaps it from the UI thread on every surfaceCreated/
 * surfaceChanged - that is, every time the card is re-laid out or brought back
 * from the taskbar chip. Most of those gaps are one UI pass wide, and losing a
 * guest over one is the difference between a run that works and an
 * eglCreateWindowSurface failure nothing was wrong with.
 *
 * The wait is bounded because the other way a window goes missing is not
 * transient at all: the user minimized or closed it. Neither comes back because
 * a guest asked - on a desktop a minimized window does not restore itself
 * either - so after the timeout the guest is told "no surface" and can decide
 * what to do about it. Called from the guest thread, inside a graphics entry
 * point; the lock is released while waiting, so the UI thread can hand the
 * window over. */
struct ANativeWindow* jni_wait_surface(int wait_ms)
{
    struct timespec deadline;
    ANativeWindow* w = NULL;

    w = surf_acquire();
    if (w || wait_ms <= 0) {
        return w;
    }

    LOGI("No window yet: waiting up to %d ms for one", wait_ms);

    /* Clock: the same one pthread_cond_timedwait() defaults to. */
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += wait_ms / 1000;
    deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_surf_cs);
    while (!g_native_window) {
        /* 0 is a wake-up call, from nativeSetWindow's broadcast or a spurious
         * one - the predicate is re-tested either way. Anything else ends the
         * wait: ETIMEDOUT in the normal case, and a bad clock or timespec must
         * not turn this into a spin on a deadline that never expires. */
        if (pthread_cond_timedwait(&g_window_cond, &g_surf_cs, &deadline) != 0) {
            break;
        }
    }
    w = g_native_window;
    if (w) {
        ANativeWindow_acquire(w);
    }
    pthread_mutex_unlock(&g_surf_cs);

    if (!w) {
        /* The window was not coming: minimized or closed by the user, or a host
         * that cannot show it at all. Reported so the guest's own failure has a
         * cause next to it in the log. */
        LOGW("No window after %d ms", wait_ms);
    }

    return w;
}

/* Per-frame chatter guard. At 60 fps the lock/unlock path used to emit four
 * log lines per frame, which filled the logcat ring buffer in seconds and
 * buried the crash reports. Log one line whenever the geometry changes. */
static int32_t g_log_gw = -1, g_log_gh = -1, g_log_gf = -1;

/* Set while "no window" has already been reported for the current surface. */
static int g_no_window_logged = 0;

/* Set while a failed ANativeWindow_lock() has already been reported for the
 * current guest. Lock failures are per-frame (the guest keeps retrying), so
 * without this the first wedge would flood logcat. */
static int g_lock_fail_logged = 0;

static void log_frame_geometry(int32_t gw, int32_t gh, int32_t gf,
                               const ANativeWindow_Buffer* buf, int32_t cols, int32_t rows)
{
    if (gw == g_log_gw && gh == g_log_gh && gf == g_log_gf) {
        return;
    }
    g_log_gw = gw;
    g_log_gh = gh;
    g_log_gf = gf;

    LOGI("Frame: guest %dx%d fmt=%d, surface buffer %dx%d stride=%d fmt=%d, copy %dx%d",
         gw, gh, gf,
         buf->width, buf->height, buf->stride, buf->format, cols, rows);
}

/* Drop the reference kept between lock and unlock. Safe to call when idle. */
static void surf_drop_locked(void)
{
    ANativeWindow* w;

    pthread_mutex_lock(&g_surf_cs);
    w = g_locked_window;
    g_locked_window = NULL;
    pthread_mutex_unlock(&g_surf_cs);

    if (w) {
        ANativeWindow_release(w);
    }
}

/* Close a window lock the guest never closed itself.
 *
 * Android's Surface remembers the buffer ANativeWindow_lock() handed out and
 * refuses every later lock() with
 *   Surface::lock failed, already locked (INVALID_OPERATION)
 * until that buffer is handed back through ANativeWindow_unlockAndPost(). The
 * Surface outlives the guest, so a guest killed between lock and unlock wedges
 * the window for every guest started afterwards: they run, every frame's lock
 * fails, nothing is ever drawn, and the screen keeps showing the last frame of
 * the previous guest. Stop lands in that window most of the time for a software
 * renderer, which clears the whole frame in between the two calls.
 *
 * unlockAndPost (rather than ANativeWindow_release) is the only NDK way to end
 * the lock and give the slot back to the surface. The abandoned buffer still
 * holds its previous frame, so one stale frame may be shown until the next
 * guest posts - the alternative is a window that never updates again.
 *
 * Must be called when no guest can touch the lock anymore (guest thread wound
 * down), so nothing is posting behind our back. */
static void surf_finish_pending_lock(void)
{
    ANativeWindow* w;

    pthread_mutex_lock(&g_surf_cs);
    w = g_locked_window;
    g_locked_window = NULL;
    memset(&g_locked_buffer, 0, sizeof(g_locked_buffer));
    pthread_mutex_unlock(&g_surf_cs);

    if (!w) {
        return;
    }

    LOGW("Guest died holding the window lock; posting the abandoned buffer to release it");
    ANativeWindow_unlockAndPost(w);
    ANativeWindow_release(w);
}

/* The buffer descriptor handed back on WINDOW_LOCK uses the shared guest-ABI
 * type cmdpost_ANativeWindow_Buffer (vp_cmdpost.h), the same one the win32 host
 * uses. Note `bits` is a 64-bit pointer here because the guest is riscv64:
 * writing this structure as a flat int32_t array shifts every following field
 * down by four bytes and corrupts width/height/stride/format. Address it by
 * field name only. */

/* Latch the panel from the first real surface size we ever see. Caller holds
 * g_surf_cs.
 *
 * The policy is the session's (vp_session_latch_panel): the panel is frozen
 * once the guest has observed a geometry, because moving it afterwards would
 * desynchronise the guest buffer from the bytes we copy on unlock - the rule
 * the win32 host follows too, where the virtual panel defaults to the window
 * size and then stays put across resizes. What is Android's here is the
 * reading of the window. */
static void panel_latch_locked(void)
{
    ANativeWindow* win = g_native_window;
    int32_t w, h;

    if (!win) return;

    w = ANativeWindow_getWidth(win);
    h = ANativeWindow_getHeight(win);
    if (vp_session_latch_panel(&g_session, w, h)) {
        LOGI("Virtual panel latched: %dx%d", w, h);
    }
}

/* Effective guest geometry. Caller holds g_surf_cs. */
static void guest_geometry_locked(int32_t* w, int32_t* h, int32_t* fmt)
{
    if (vp_session_guest_geometry(&g_session, w, h, fmt)) {
        LOGI("Guest surface latched to panel: %dx%d",
             g_session.gfx_w, g_session.gfx_h);
    }
}

/* Push the latched panel geometry onto a real surface.
 *
 * Both present paths have to do this before handing a surface to the platform,
 * and for the same reason: a surface's own geometry is the *viewport's* (the
 * floating graphics card is much smaller than the panel), while the guest
 * renders a buffer laid out for the panel. The CPU path needs lock() to hand
 * back a buffer of exactly the guest's size, because the frame is copied into
 * it one-for-one; the GL path needs it before eglCreateWindowSurface, which
 * takes its buffer size from the window's current geometry - without it the
 * guest's glViewport(0, 0, panelW, panelH) would only cover the bottom-left
 * corner of a viewport-sized surface.
 *
 * Called with g_surf_cs NOT held. */
void jni_apply_surface_geometry(struct ANativeWindow* w)
{
    int32_t gw, gh, gf;

    if (!w) {
        return;
    }

    pthread_mutex_lock(&g_surf_cs);
    panel_latch_locked();
    guest_geometry_locked(&gw, &gh, &gf);
    if (vp_session_geometry_dirty(&g_session, gw, gh, gf)) {
        if (ANativeWindow_setBuffersGeometry(w, gw, gh, gf) == 0) {
            vp_session_geometry_pushed(&g_session, gw, gh, gf);
        }
    }
    pthread_mutex_unlock(&g_surf_cs);
}

/* Window lock callback (called from vp_cmdpost)
 * Fills geometry only; the guest renders into its own buffer and we copy
 * pixels on unlock. Never expose the host surface pointer to the guest. The
 * geometry handed to the guest is the virtual panel, never the live viewport,
 * so a surface resize cannot move the buffer the guest is mid-frame on.
 *
 * outBuffer is already a HOST pointer: cmdpost translates the guest address
 * before calling in, since guest memory is the userland machine's own buffer
 * and is not mapped into the host. */
static int32_t on_window_lock(void* window, void* outBuffer, void* dirtyBounds)
{
    (void)window;
    (void)dirtyBounds;

    int32_t gw, gh, gf;

    /* Own the surface for the whole lock() ... unlockAndPost() pair; the Java
     * thread may drop it right now (see g_locked_window). */
    ANativeWindow* w = surf_acquire();
    if (!w) {
        /* Normal while the surface is being recreated: the host cleared the
         * window and the guest has not drained APP_CMD_TERM_WINDOW yet. Log it
         * once per window (not once per frame) so it cannot flood logcat. */
        if (!g_no_window_logged) {
            g_no_window_logged = 1;
            LOGW("Window not initialized, frame dropped");
        }
        return -1;
    }

    pthread_mutex_lock(&g_surf_cs);
    panel_latch_locked();
    guest_geometry_locked(&gw, &gh, &gf);
    pthread_mutex_unlock(&g_surf_cs);

    /* Push the panel geometry onto the real surface so lock() hands back a
     * buffer we can copy the guest frame into one-for-one. */
    jni_apply_surface_geometry(w);

    ANativeWindow_Buffer buffer;
    ARect dirty;

    int32_t result = ANativeWindow_lock(w, &buffer, &dirty);
    if (result != 0) {
        ANativeWindow_release(w);
        /* Reported once per guest: the guest retries every frame, and the usual
         * cause is a lock left outstanding by the guest before it (see
         * surf_finish_pending_lock()), which fails every frame until the
         * surface is recreated. */
        if (!g_lock_fail_logged) {
            g_lock_fail_logged = 1;
            LOGW("Window lock failed (%d): nothing will be drawn until the surface is released", result);
        }
        return result;
    }

    int32_t cols = (gw < buffer.width)  ? gw : buffer.width;
    int32_t rows = (gh < buffer.height) ? gh : buffer.height;
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;

    /* Hand the reference over to on_window_unlock(). */
    pthread_mutex_lock(&g_surf_cs);
    g_locked_window = w;
    g_locked_buffer = buffer;
    pthread_mutex_unlock(&g_surf_cs);

    if (outBuffer) {
        /* outBuffer is a host pointer to the guest's buffer; geometry only,
         * bits stays 0 */
        cmdpost_ANativeWindow_Buffer* dst = (cmdpost_ANativeWindow_Buffer*)outBuffer;
        dst->bits   = NULL;       /* guest supplies its own buffer       */
        dst->width  = gw;         /* guest geometry == virtual panel     */
        dst->height = gh;
        dst->stride = gw;         /* guest stride == panel width         */
        dst->format = gf;
    }

    log_frame_geometry(gw, gh, gf, &buffer, cols, rows);

    return result;
}

/* Window unlock callback (called from vp_cmdpost)
 * Copies the guest-rendered pixels into the real surface buffer, then posts. */
static int32_t on_window_unlock(void* window, void* guestPixels)
{
    (void)window;

    int32_t gw, gh, gf;
    ANativeWindow_Buffer lbuf;
    ANativeWindow* w;

    pthread_mutex_lock(&g_surf_cs);
    guest_geometry_locked(&gw, &gh, &gf);
    w = g_locked_window;
    lbuf = g_locked_buffer;
    pthread_mutex_unlock(&g_surf_cs);

    if (!w) {
        /* No lock outstanding: the surface went away between lock and unlock
         * (that is now a clean no-op instead of a libgui abort). */
        LOGW("Unlock without a locked window, frame dropped");
        return -1;
    }

    if (guestPixels && lbuf.bits) {
        size_t sbpp = (gf == WINDOW_FORMAT_RGB_565) ? 2 : 4;
        size_t dbpp = (lbuf.format == WINDOW_FORMAT_RGB_565) ? 2 : 4;

        /* Copy only what BOTH sides agree on. The guest buffer is panel-sized
         * (the session's gfx_w x gfx_h); the surface buffer is whatever the
         * platform granted for the lock. Clamping here means a surface that
         * ignored our geometry can never turn this into an out-of-bounds
         * access. */
        int32_t cols = (gw < lbuf.width)  ? gw : lbuf.width;
        int32_t rows = (gh < lbuf.height) ? gh : lbuf.height;
        if (cols < 0) cols = 0;
        if (rows < 0) rows = 0;

        size_t sstride = (size_t)gw * sbpp;             /* guest row pitch   */
        size_t dstride = (size_t)lbuf.stride * dbpp;    /* surface row pitch */
        size_t srow    = (size_t)cols * sbpp;
        size_t drow    = (size_t)cols * dbpp;

        uint8_t* src = (uint8_t*)guestPixels;
        uint8_t* dst = (uint8_t*)lbuf.bits;
        if (srow == drow) {
            for (int32_t y = 0; y < rows; y++) {
                memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, srow);
            }
        } else {
            /* Pixel-format mismatch (e.g. an RGBA panel into an RGB_565
             * surface): copy the low bytes of each pixel. */
            size_t px = (sbpp < dbpp) ? sbpp : dbpp;
            for (int32_t y = 0; y < rows; y++) {
                uint8_t* s = src + (size_t)y * sstride;
                uint8_t* d = dst + (size_t)y * dstride;
                for (int32_t x = 0; x < cols; x++) {
                    memcpy(d + (size_t)x * dbpp, s + (size_t)x * sbpp, px);
                }
            }
        }
    } else {
        /* Lock succeeded but there is no frame to copy (guest pixbuf missing,
         * or the surface buffer pointer was invalidated by a window change).
         * Still post so the buffer queue keeps flowing, but make the cause
         * visible instead of silently presenting an untouched frame. */
        LOGW("Unlock: no frame copied (guestPixels=%p, surfaceBits=%p); posting untouched buffer",
             guestPixels, lbuf.bits);
    }

    int32_t result = ANativeWindow_unlockAndPost(w);

    /* Drop the reference taken in on_window_lock(). */
    surf_drop_locked();

    /* The CPU present path: once a frame is actually on the surface the
     * rendered content takes over from the console overlay. */
    if (result == 0) {
        jni_guest_first_frame();
    }

    return result;
}

/* Window size callback (called from vp_cmdpost).
 * Reports the virtual panel, not the viewport: this is the number the guest
 * caches, so it must not move when the surface is resized. */
static void on_window_size(int64_t* width, int64_t* height)
{
    int32_t gw = 0, gh = 0;

    pthread_mutex_lock(&g_surf_cs);
    panel_latch_locked();
    guest_geometry_locked(&gw, &gh, NULL);
    pthread_mutex_unlock(&g_surf_cs);

    *width  = (int64_t)gw;
    *height = (int64_t)gh;
}

/* Window set-buffers-geometry callback (called from vp_cmdpost).
 * The guest's ANativeWindow_setBuffersGeometry is a stub that forwards here.
 * A concrete width/height redefines the guest surface - the only path by which
 * the guest moves its own geometry, exactly like win32 - while the usual
 * (0, 0, format) call only selects the pixel format and leaves the panel
 * untouched. The real surface is re-applied lazily at the next lock. */
static int32_t on_window_set_buf(int32_t width, int32_t height, int32_t format)
{
    pthread_mutex_lock(&g_surf_cs);
    /* The session applies the guest's rule - a concrete size redefines the
     * surface, a bare format select leaves it alone - and retires the cached
     * push, so the real surface is re-applied at the next lock. */
    vp_session_set_guest_geometry(&g_session, width, height, format);
    pthread_mutex_unlock(&g_surf_cs);

    LOGI("Window geometry set: %dx%d format=%d (guest geometry %s)",
         width, height, format,
         (width > 0 && height > 0) ? "redefined" : "unchanged");
    return 0;
}

/* Quantise a physical PPI to the nearest public NDK density bucket. The exact
 * PPI stays host-private; only these buckets are exposed to the guest. */
static int32_t density_bucket_for_ppi(int32_t ppi)
{
    static const int32_t buckets[] = {
        ACONFIGURATION_DENSITY_LOW,     /* 120 */
        ACONFIGURATION_DENSITY_MEDIUM,  /* 160 */
        ACONFIGURATION_DENSITY_TV,      /* 213 */
        ACONFIGURATION_DENSITY_HIGH,    /* 240 */
        ACONFIGURATION_DENSITY_XHIGH,   /* 320 */
        ACONFIGURATION_DENSITY_XXHIGH,  /* 480 */
        ACONFIGURATION_DENSITY_XXXHIGH, /* 640 */
    };
    size_t i, best = 0;
    int32_t best_d = -1;

    if (ppi <= 0) return ACONFIGURATION_DENSITY_MEDIUM;
    for (i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
        int32_t d = ppi - buckets[i];
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return buckets[best];
}

/* Device configuration callback (called from vp_cmdpost).
 * Everything is derived from the virtual panel, never from the real device:
 * the guest must observe a stable host-owned display, the same way the win32
 * host synthesises it from its surface size and virtual PPI. */
static int32_t on_config_get(int32_t field, int32_t* outValue)
{
    int32_t w, h, ppi, width_dp, height_dp, long_dp, short_dp;

    if (!outValue) return -1;

    pthread_mutex_lock(&g_surf_cs);
    panel_latch_locked();
    guest_geometry_locked(&w, &h, NULL);
    ppi = (g_session.panel_ppi > 0) ? g_session.panel_ppi
                                    : ACONFIGURATION_DENSITY_MEDIUM;
    pthread_mutex_unlock(&g_surf_cs);

    width_dp  = w * 160 / ppi;
    height_dp = h * 160 / ppi;
    long_dp   = (width_dp >= height_dp) ? width_dp : height_dp;
    short_dp  = (width_dp >= height_dp) ? height_dp : width_dp;

    switch (field) {
    case VP_ACONFIG_QUERY_ORIENTATION:
        *outValue = (w >= h) ? ACONFIGURATION_ORIENTATION_LAND
                             : ACONFIGURATION_ORIENTATION_PORT;
        return 0;
    case VP_ACONFIG_QUERY_DENSITY:
        *outValue = density_bucket_for_ppi(ppi);
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_SIZE:
        *outValue = (short_dp < 320) ? ACONFIGURATION_SCREENSIZE_SMALL
                  : (short_dp < 480) ? ACONFIGURATION_SCREENSIZE_NORMAL
                  : (short_dp < 720) ? ACONFIGURATION_SCREENSIZE_LARGE
                                     : ACONFIGURATION_SCREENSIZE_XLARGE;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_LONG:
        *outValue = (long_dp * 5 >= short_dp * 8) ? ACONFIGURATION_SCREENLONG_YES
                                                  : ACONFIGURATION_SCREENLONG_NO;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_ROUND:
        *outValue = ACONFIGURATION_SCREENROUND_NO;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_WIDTH_DP:
        *outValue = width_dp;
        return 0;
    case VP_ACONFIG_QUERY_SCREEN_HEIGHT_DP:
        *outValue = height_dp;
        return 0;
    default:
        return -1;
    }
}

/* GameActivity lifecycle callback (called from vp_cmdpost) */
static void on_game_lifecycle(int32_t cmd)
{
    LOGI("GameActivity lifecycle cmd=%d", cmd);
    
    /* Queue the command for polling by Guest */
    if (g_lifecycle_cmd_count < 32) {
        g_lifecycle_cmd_queue[g_lifecycle_cmd_count++] = cmd;
    }
}

/* GameActivity input callback (called from vp_cmdpost) */
static void on_game_input(void* motionEvent)
{
    (void)motionEvent;
    LOGI("GameActivity input event received");
}

/* ============================================================
 * JNI Initialization
 * ============================================================ */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    g_jvm = vm;
    LOGI("JNI_OnLoad: RVVM JNI bridge loaded");
    return JNI_VERSION_1_6;
}

/* ============================================================
 * Native methods called from Java
 * ============================================================ */

/* Forward declaration */
static void on_guest_exit(int exit_code);

/* ============================================================
 * (Re)install every vp_cmdpost callback the guest depends on.
 *
 * A guest's exit no longer dismantles this bridge: the core now ends the *run*
 * (queues, streams, sensor state - cmdpost_end_run()) and leaves the host's
 * registrations alone, so the relaunch-racing-a-teardown failure is gone at the
 * source. Re-running this before each guest is therefore no longer load-bearing
 * - it stays as an idempotent belt (ten pointer stores) for a callback the
 * guest or a mid-run failure might have replaced.
 *
 * nativeInit() runs it once for the first guest; nativeRunElf() re-runs it for
 * every later one - the same role win32_cmdpost_register_callbacks() plays on
 * the win32 side.
 * ============================================================ */
static void jni_register_cmdpost_callbacks(void)
{
    extern const vp_audio_ops_t* android_aaudio_ops(void);

    /* Window / surface + configuration */
    cmdpost_set_window_callbacks(g_cmdpost, on_window_lock, on_window_unlock);
    cmdpost_set_window_size_callback(g_cmdpost, on_window_size);
    cmdpost_set_window_set_buf_callback(g_cmdpost, on_window_set_buf);
    cmdpost_set_config_callback(g_cmdpost, on_config_get);

    /* GameActivity lifecycle + input */
    cmdpost_set_game_callbacks(g_cmdpost, on_game_lifecycle, on_game_input);

    /* Real display vsync as the guest's AChoreographer source. Only re-armed
     * while its owner thread is alive; otherwise the source stays unavailable
     * and the guest falls back to its own clock instead of waiting for a tick
     * that will never come. */
    if (g_vsync_running) {
        cmdpost_set_choreographer_callback(g_cmdpost, android_vsync_wait);
        g_vsync_warned = 0;
    }

    /* Real AAudio backend: the pump thread in vp_aaudio_android.c bridges the
     * guest's SPSC ring to AAudioStream. */
    cmdpost_set_audio_callbacks(g_cmdpost, android_aaudio_ops());

    /* Real sensor backend: vp_sensor_android.c owns the platform
     * ASensorEventQueue and feeds the subsystem through vp_sensor_ingest().
     * The registration is device-level and survives a run (the per-guest half
     * - descriptor table, queues - is the cmdpost instance's, and is dropped by
     * vp_sensor_reset() when that guest exits), so re-running this per guest is
     * just an idempotent restatement of "the platform source is still ours". */
    android_sensor_start();
    vp_sensor_set_ops(android_sensor_ops());

    /* The GL backend registers its dispatch callbacks into the host's cmdpost
     * instance, so it has to be told which one this is before init() reaches
     * them. Set here rather than once at startup because the instance is per
     * run now. */
    android_gl_set_cmdpost(g_cmdpost);

    /* System EGL/GLES backend for the marshalled GL calls. Loads the system
     * libraries on first use and re-installs the dispatch callbacks; on
     * failure the guest falls back to CPU rendering like on win32. */
    android_gl_host_init();
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeInit(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    g_env = env;

    /* The host session: the display geometry the guest observes, and the
     * assembly of its output into console lines. Everything in it that is
     * per-run is reset by console_reset() in nativeRunElf(); the one thing it
     * needs from this side is where a finished line goes. */
    vp_session_init(&g_session);
    g_session.on_line = on_console_line;

    /* No cmdpost instance is made here: it belongs to a run, and is created by
     * nativeClearLifecycleCmds() (before Java seeds that run) and freed by the
     * run's own guest thread. The callback registration below still runs, into
     * whatever instance exists at the time - a no-op when there is none. */

    /* Expose the real display vsync as the guest's AChoreographer source. */
    jni_vsync_start();

    /* Install every bridge callback the guest needs: window/config/game, the
     * vsync source and the real AAudio backend. nativeRunElf() re-runs this
     * before every later guest - see jni_register_cmdpost_callbacks(). */
    jni_register_cmdpost_callbacks();

    LOGI("Native init complete");
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeDestroy(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    /* Stops the platform sensor thread and destroys its event queue. */
    android_sensor_shutdown();

    /* A suspended guest is parked and would never poll again; wake it before
     * the teardown below so it is not left parked against a torn-down bridge. */
    if (g_guest_suspended && g_guest_machine) {
        rvvm_user_resume(g_guest_machine);
        g_guest_suspended = 0;
    }

    /* Stop the vsync source before tearing down the bridge. */
    jni_vsync_stop();

    /* Close any AAudio stream the guest did not release itself. */
    extern void android_aaudio_shutdown(void);
    android_aaudio_shutdown();

    /* Drop the console session - but only once no guest can still be parsing
     * its output into it. With one still unwinding, its ctx holds the session
     * and the process is going away anyway; the next run reopens it (the
     * screen is not kept across a native teardown, only across guest runs). */
    if (!g_guest_machine && g_tty) {
        rvvm_tty_close(g_tty);
        g_tty = NULL;
    }

    /* A run frees its own instance as it ends (guest_thread_func), so this is
     * for the case where one was made and never ran - a start that failed
     * between nativeClearLifecycleCmds() and the guest thread. */
    cmdpost_destroy(g_cmdpost);
    g_cmdpost = NULL;

    LOGI("Native destroy complete");
}

JNIEXPORT jstring JNICALL
Java_com_rvvm_android_RvvmNative_nativeGetVersion(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return (*env)->NewStringUTF(env, "1.0.0");
}

/* Real device configuration pushed from the Java Configuration object.
 * Called by MainActivity on start and on configuration changes.
 *
 * Only the physical density is consumed: it becomes the private PPI of the
 * virtual panel. The dp/orientation/size figures are deliberately ignored -
 * the guest derives those from the panel instead, so the real device
 * configuration never leaks into the guest's ABI (matching the win32 host,
 * which synthesises AConfiguration from its own panel + PPI). */
JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetDisplayConfig(
    JNIEnv* env, jobject thiz,
    jint widthDp, jint heightDp, jint densityDpi,
    jint orientation, jint screenSize, jint screenLong, jint screenRound)
{
    (void)env;
    (void)thiz;
    (void)widthDp;
    (void)heightDp;
    (void)orientation;
    (void)screenSize;
    (void)screenLong;
    (void)screenRound;

    pthread_mutex_lock(&g_surf_cs);
    vp_session_set_density(&g_session, (int32_t)densityDpi);
    pthread_mutex_unlock(&g_surf_cs);

    LOGI("Display config: density=%d (real %dx%d dp ignored; panel-owned)",
         densityDpi, widthDp, heightDp);
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetPanelSize(JNIEnv* env, jobject thiz,
                                                    jint width, jint height)
{
    (void)env;
    (void)thiz;

    if (width <= 0 || height <= 0) {
        return;
    }

    /* The panel is normally latched from the first surface size this side ever
     * sees (panel_latch_locked), which ties the guest's resolution to the size
     * of the floating graphics window - and that window is draggable. The host
     * pins it here instead (720p landscape), leaving the window as a viewport
     * onto a fixed geometry.
     *
     * Refused by the session once the guest has observed a geometry: that is
     * the buffer it may be mid-frame on, and moving it under a running guest is
     * exactly the desync this panel/surface split exists to prevent. Before
     * that, an already-latched panel is overwritten - it was latched from a
     * viewport size nobody has rendered into yet. */
    pthread_mutex_lock(&g_surf_cs);
    if (vp_session_set_panel(&g_session, width, height)) {
        LOGI("Virtual panel pinned: %dx%d", width, height);
    } else {
        LOGI("Virtual panel already in use (%dx%d), ignoring %dx%d",
             g_session.gfx_w, g_session.gfx_h, width, height);
    }
    pthread_mutex_unlock(&g_surf_cs);
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    (void)thiz;

    /* SurfaceCreated and SurfaceChanged both hand us a surface. Acquire it
     * first; a NULL surface means the surface is going away. */
    ANativeWindow* new_window = NULL;
    if (surface) {
        new_window = ANativeWindow_fromSurface(env, surface);
        if (!new_window) {
            LOGE("Failed to get native window from surface");
            return;
        }
    }

    int32_t pw, ph;

    /* Swap the tracked window while holding g_surf_cs. The guest-driven
     * callbacks take their own reference under the same mutex, so the Surface
     * is never released while a lock()/unlockAndPost() pair is using it (that
     * race is what aborted in Surface::lock). The old reference is dropped
     * only after the swap, outside the critical section. */
    pthread_mutex_lock(&g_surf_cs);
    ANativeWindow* old_window = g_native_window;
    g_native_window = new_window;

    if (new_window) {
        /* Freeze the virtual panel at the first surface size we ever see. From
         * here on the surface is only a viewport: later resizes repaint, they
         * do not move the geometry the guest renders into. */
        panel_latch_locked();
        g_no_window_logged = 0;
    }
    if (old_window != new_window) {
        /* A brand new Surface starts with the platform's default geometry:
         * forget what we pushed so the next lock re-applies the panel size. */
        vp_session_forget_geometry(&g_session);
    }

    /* Tells the GL backend a window came or went - a log line only: the pointer
     * is not kept, because it is released right below and re-taken with a
     * reference by whoever needs it (jni_wait_surface). */
    android_gl_set_native_window(new_window);

    /* Wake any guest parked in jni_wait_surface(): a window is here now, which
     * is the only thing it was waiting for. Broadcast under the mutex the
     * waiter sleeps on. */
    pthread_cond_broadcast(&g_window_cond);

    pw = g_session.panel_w;
    ph = g_session.panel_h;
    pthread_mutex_unlock(&g_surf_cs);

    if (old_window == new_window) {
        /* SurfaceCreated/SurfaceChanged handed us the very same surface: keep
         * it, just drop the extra reference we took. */
        if (new_window) {
            ANativeWindow_release(new_window);
            LOGI("Native window unchanged, keeping existing window");
        }
        return;
    }

    if (old_window) {
        ANativeWindow_release(old_window);
    }

    if (new_window) {
        LOGI("Native window set: %dx%d (panel %dx%d)",
             ANativeWindow_getWidth(new_window),
             ANativeWindow_getHeight(new_window), pw, ph);
    } else {
        LOGI("Native window cleared");
    }
}

JNIEXPORT jint JNICALL
Java_com_rvvm_android_RvvmNative_nativePollLifecycleCmd(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    
    /* Return next lifecycle command from queue, or -1 if empty */
    if (g_lifecycle_cmd_read < g_lifecycle_cmd_count) {
        return g_lifecycle_cmd_queue[g_lifecycle_cmd_read++];
    }
    return -1;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeClearLifecycleCmds(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    /* Java calls this to prepare a run (MainActivity.replayGuestStartupState:
     * clear, then seed with the commands this guest would have seen on a cold
     * start), so it is also where that run's cmdpost instance is made: the
     * commands queued next have to land in the instance the guest will poll.
     * The previous run freed its own as it handed control back
     * (guest_thread_func), so this is normally NULL. */
    if (!g_cmdpost) {
        g_cmdpost = cmdpost_create();
    }

    g_lifecycle_cmd_count = 0;
    g_lifecycle_cmd_read = 0;

    /* Called right before the machine is handed to the next guest, so drop
     * what the *guest* still has queued too: pending commands and input belong
     * to the previous guest (a stale TERM_WINDOW/DESTROY would take the new one
     * down on its very first poll, a stale touch would be delivered as if the
     * user had just tapped). The seed state for the new guest is then queued
     * explicitly by the Java side. */
    cmdpost_clear_lifecycle_cmds(g_cmdpost);
    cmdpost_clear_motion_events(g_cmdpost);
    cmdpost_clear_key_events(g_cmdpost);
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativePostLifecycleCmd(JNIEnv* env, jobject thiz, jint cmd)
{
    (void)env;
    (void)thiz;
    
    /* Queue a lifecycle command (called from Java when activity state changes)
     * Both into the guest-facing queue (via vp_cmdpost) and the local queue. */
    cmdpost_queue_lifecycle_cmd(g_cmdpost, cmd);
    if (g_lifecycle_cmd_count < 32) {
        g_lifecycle_cmd_queue[g_lifecycle_cmd_count++] = cmd;
        LOGI("Lifecycle command %d queued", cmd);
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativePostMotionEvent(JNIEnv* env, jobject thiz,
                                                       jfloatArray xs, jfloatArray ys,
                                                       jintArray ids, jint pointerCount,
                                                       jint action, jlong eventTime)
{
    (void)thiz;

    if (!xs || !ys) {
        return;
    }

    /* Clamp to the smaller of the caller's count, the array length and the ABI
     * limit (CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT). */
    jsize count = (*env)->GetArrayLength(env, xs);
    if (pointerCount < count) {
        count = pointerCount;
    }
    if (count > CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT) {
        count = CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT;
    }
    if (count <= 0) {
        return;
    }

    float  xbuf[CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT];
    float  ybuf[CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT];
    int32_t idbuf[CMDPOST_MAX_NUM_POINTERS_IN_MOTION_EVENT];

    (*env)->GetFloatArrayRegion(env, xs, 0, count, xbuf);
    (*env)->GetFloatArrayRegion(env, ys, 0, count, ybuf);
    if (ids && (*env)->GetArrayLength(env, ids) >= count) {
        (*env)->GetIntArrayRegion(env, ids, 0, count, idbuf);
    } else {
        for (jsize i = 0; i < count; i++) {
            idbuf[i] = (int32_t)i;
        }
    }

    /* Build a motion event matching the guest ABI and queue it for the guest */
    cmdpost_GameActivityMotionEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.eventTime = (int64_t)eventTime;
    ev.deviceId = 0;
    ev.source = 0x0002; /* AINPUT_SOURCE_TOUCHSCREEN */
    ev.action = (int32_t)action;
    ev.pointerCount = (int32_t)count;

    for (jsize i = 0; i < count; i++) {
        ev.pointers[i].x = xbuf[i];
        ev.pointers[i].y = ybuf[i];
        ev.pointers[i].rawX = xbuf[i];
        ev.pointers[i].rawY = ybuf[i];
        ev.pointers[i].pressure = 1.0f;
        ev.pointers[i].size = 1.0f;
        ev.pointers[i].id = idbuf[i];
        ev.pointers[i].toolType = 1; /* AMOTION_EVENT_TOOL_TYPE_FINGER */
    }

    cmdpost_queue_motion_event(g_cmdpost, &ev);
    LOGI("Motion event queued: pointers=%d action=0x%x first=(%.0f,%.0f)",
         (int)count, (unsigned)action, xbuf[0], ybuf[0]);
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativeRunElf(JNIEnv* env, jobject thiz, jstring elfPath, jobjectArray args)
{
    (void)thiz;
    
    if (g_guest_running) {
        LOGE("Guest already running");
        return JNI_FALSE;
    }
    
    /* Get ELF path from Java string */
    const char* path = (*env)->GetStringUTFChars(env, elfPath, NULL);
    if (!path) {
        LOGE("Failed to get ELF path");
        return JNI_FALSE;
    }
    
    strncpy(g_guest_elf_path, path, sizeof(g_guest_elf_path) - 1);
    g_guest_elf_path[sizeof(g_guest_elf_path) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, elfPath, path);
    
    /* Get optional arguments */
    g_guest_argc = 1;  /* argv[0] = ELF path */
    if (args) {
        jsize len = (*env)->GetArrayLength(env, args);
        for (int i = 0; i < len && g_guest_argc < 15; i++) {
            jstring jstr = (jstring)(*env)->GetObjectArrayElement(env, args, i);
            const char* str = (*env)->GetStringUTFChars(env, jstr, NULL);
            if (str) {
                /* Store in static buffer (simplified - no dynamic alloc) */
                static char arg_buf[16][128];
                strncpy(arg_buf[g_guest_argc], str, 127);
                arg_buf[g_guest_argc][127] = '\0';
                g_guest_argv[g_guest_argc] = arg_buf[g_guest_argc];
                g_guest_argc++;
                (*env)->ReleaseStringUTFChars(env, jstr, str);
            }
            (*env)->DeleteLocalRef(env, jstr);
        }
    }
    
    LOGI("Starting guest: %s (argc=%d)", g_guest_elf_path, g_guest_argc);

    /* New run: flush the previous guest's trailing partial line and let the
     * first frame of THIS guest re-hide the console overlay. */
    console_reset();

    /* Reinstall the callbacks for this guest. The exit of the previous one no
     * longer clears them (it ends the run, not the bridge - cmdpost_end_run()),
     * so this is an idempotent belt rather than the thing that keeps a
     * relaunched guest from probing a dead proxy. */
    jni_register_cmdpost_callbacks();

    /* Per-guest diagnostics: report this guest's first frame/geometry too, not
     * just the first guest the process ever ran. */
    g_log_gw = g_log_gh = g_log_gf = -1;
    g_no_window_logged = 0;
    g_lock_fail_logged = 0;

    /* Safety net for a window wedged by a guest that died holding a lock before
     * this guest ever started (or by a run that failed to create its thread):
     * starting a guest with the surface still locked would let it render
     * nothing at all. Normally a no-op - the previous guest's thread already
     * released the lock on its way out. */
    surf_finish_pending_lock();
    
    /* Fresh userland instance per guest, so nothing leaks between runs. */
    g_guest_machine = rvvm_user_create();
    if (!g_guest_machine) {
        LOGE("Failed to create userland machine");
        return JNI_FALSE;
    }
    /* on_guest_exit re-reads g_exit_listener when it fires, so registering it
     * once per run covers a listener set before or after this point. */
    rvvm_user_set_exit_callback(g_guest_machine, on_guest_exit);

    /* This run's cmdpost instance is normally made by nativeClearLifecycleCmds()
     * (Java queues the startup sequence right after that call, and it has to
     * land in this run's instance); this is the safety net for a caller that
     * went straight to nativeRunElf. */
    if (!g_cmdpost) {
        g_cmdpost = cmdpost_create();
    }

    /* Bind this run's machine to the host's cmdpost instance: it is how a
     * syscall arriving on a guest thread finds the state it belongs to. Done
     * before the thread starts, since the first thing the guest does may be one
     * of these syscalls. */
    rvvm_user_set_host_ctx(g_guest_machine, g_cmdpost);

    /* Host-owned console session: wipe the screen for this run (the cells, the
     * scrollback, the view and the cursor state all belong to the run that just
     * ended - rvvm_tty_reset covers those) and attach it to the machine for the
     * length of the run. The session itself outlives the guest, so the console
     * tab keeps showing - and scrolling through - the last screen afterwards.
     * No output callback is registered: the session bumps its own serial, which
     * is what the Java poller reads. */
    jni_tty_init();
    if (g_tty) {
        rvvm_tty_reset(g_tty);
        rvvm_tty_attach(g_tty, g_guest_machine);
    }
    
    /* Start guest thread */
    g_guest_running = 1;
    g_guest_suspended = 0;
    if (pthread_create(&g_guest_thread, NULL, guest_thread_func, NULL) != 0) {
        LOGE("Failed to create guest thread");
        g_guest_running = 0;
        g_guest_suspended = 0;
        rvvm_user_free(g_guest_machine);
        g_guest_machine = NULL;
        return JNI_FALSE;
    }
    
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativeIsGuestRunning(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return g_guest_running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeStopGuest(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    
    if (g_guest_running) {
        LOGI("Stopping guest...");
        /* A suspended guest is parked and cannot poll for the stop; resume it
         * first so it unwinds the normal way instead of being forced down. The
         * frame clock stays off: the guest degrades to its own clock while it
         * tears down, so there is no point restarting a source we are about to
         * retire. */
        if (g_guest_suspended && g_guest_machine) {
            rvvm_user_resume(g_guest_machine);
            g_guest_suspended = 0;
            LOGI("Stop: resumed the suspended guest first");
        }
        /* Kick every guest vCPU out of its run loop; the guest unwinds like a
         * sys_exit_group(0), so on_guest_exit fires and guest_thread_func
         * clears g_guest_running once rvvm_user_linux_ex() returns. */
        if (g_guest_machine) {
            rvvm_user_stop(g_guest_machine, 0);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSuspendGuest(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    if (!g_guest_running || !g_guest_machine || g_guest_suspended) {
        return;
    }

    bool parked = rvvm_user_suspend(g_guest_machine);
    g_guest_suspended = 1;

    /* Park the frame clock too: a parked guest polls neither frames nor
     * lifecycle commands, so a running clock would only pile up vsync ticks
     * for it to burn through on resume. Mirrors win32's vsync_clock_stop(). */
    jni_vsync_stop();

    LOGI("Suspend: vCPUs %s", parked ? "parked"
                                     : "parking (one is in a blocking syscall)");
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeResumeGuest(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;

    if (!g_guest_running || !g_guest_machine || !g_guest_suspended) {
        return;
    }

    /* Bring the clock back up before the guest: stopping it raised the
     * source-lost flag, and re-registering the callbacks both clears that flag
     * and lets the resumed guest use the fd-wakeup path again. */
    jni_vsync_start();
    jni_register_cmdpost_callbacks();

    g_guest_suspended = 0;
    rvvm_user_resume(g_guest_machine);
    LOGI("Suspend: guest resumed");
}

JNIEXPORT jboolean JNICALL
Java_com_rvvm_android_RvvmNative_nativeIsGuestSuspended(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    /* Reported from the host-side flag, not the machine: see g_guest_suspended. */
    return g_guest_suspended ? JNI_TRUE : JNI_FALSE;
}

/* C callback invoked by rvvm_user when the guest exits.
 * NOTE: g_guest_running is NOT cleared here - the guest native thread is
 * still winding down (rvvm_user_linux has not returned yet). guest_thread_func
 * clears it after rvvm_user_linux() returns, keeping the "running" state in
 * sync with the actual thread and preventing a premature re-Run from starting
 * a second userland over the existing one. */
static void on_guest_exit(int exit_code)
{
    LOGI("Guest exited with code: %d", exit_code);

    if (g_exit_listener) {
        JNIEnv* env = NULL;
        int attached = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
        if (attached == JNI_EDETACHED) {
            (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        }
        if (env) {
            jclass clazz = (*env)->GetObjectClass(env, g_exit_listener);
            jmethodID mid = (*env)->GetMethodID(env, clazz, "onExit", "(I)V");
            if (mid) {
                (*env)->CallVoidMethod(env, g_exit_listener, mid, (jint)exit_code);
            }
            (*env)->DeleteLocalRef(env, clazz);
        }
        if (attached == JNI_EDETACHED) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetConsoleListener(JNIEnv* env, jobject thiz, jobject listener)
{
    (void)thiz;

    if (g_console_listener) {
        (*env)->DeleteGlobalRef(env, g_console_listener);
        g_console_listener = NULL;
        g_console_output_mid = NULL;
        g_console_first_frame_mid = NULL;
    }

    if (listener) {
        jclass clazz = (*env)->GetObjectClass(env, listener);
        g_console_listener = (*env)->NewGlobalRef(env, listener);
        g_console_output_mid = (*env)->GetMethodID(env, clazz, "onOutput", "(Ljava/lang/String;)V");
        g_console_first_frame_mid = (*env)->GetMethodID(env, clazz, "onFirstFrame", "()V");
        (*env)->DeleteLocalRef(env, clazz);
        if (!g_console_output_mid || !g_console_first_frame_mid) {
            LOGE("ConsoleListener method lookup failed");
            (*env)->DeleteGlobalRef(env, g_console_listener);
            g_console_listener = NULL;
        } else {
            LOGI("Guest console listener registered");
        }
    }
}

JNIEXPORT void JNICALL
Java_com_rvvm_android_RvvmNative_nativeSetExitCallback(JNIEnv* env, jobject thiz, jobject listener)
{
    (void)thiz;

    if (g_exit_listener) {
        (*env)->DeleteGlobalRef(env, g_exit_listener);
        g_exit_listener = NULL;
    }

    if (listener) {
        g_exit_listener = (*env)->NewGlobalRef(env, listener);
        /* The per-run machine is not alive yet (or already gone); on_guest_exit
         * is registered on it in nativeRunElf() and re-reads g_exit_listener
         * when it fires, so there is nothing to wire up here. */
    }
}
