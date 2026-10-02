/*
rvvm_user_main.c - RVVM Linux userland emulator entry point
Thin wrapper around rvvm_user_linux_ex() from core/rvvm_user.c

Usage: rvvm_user [-prefix <dir>] [-cmdline <args>] [-append <args>] <guest_elf> [args...]

`-cmdline` and `-append` are src/main.c's own spellings, reused rather than
invented: the full-system binary (rvvm_x86_64) has always taken boot arguments
that way, and a command line with two spellings is worse than one with none. The
full-system binary can act on them because it hands the machine to a kernel
through a device tree; here there is no kernel, so the same string is also what
the guest reads back from /proc/cmdline.

`-prefix` is NOT a boot argument and is not one anywhere: the hostfs base is a
host path (see rvvm_user_set_prefix), and a command line that named one would
mean a different run on every machine. It is here because this binary *is* the
host - with no bundle to install, the only thing that can put a rootfs under the
guest is the thing that started it. A relative path is resolved against the
working directory; the setter does that, not this file.

Whether this run's console is a terminal is not an option: it is decided by
asking this process's own stdin, and both answers are honest. See "the host
console" below.
*/

#include <util/feature_test.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(HOST_TARGET_WIN32)
// For GetStdHandle(), ReadFile(), GetConsoleMode(), SetConsoleMode(), Sleep()
#include <windows.h>
#else
#include <errno.h>     // errno, EINTR
#include <signal.h>    // sigaction(), SIGWINCH: a resize is a signal on this host
#include <termios.h>   // struct termios, tc*attr(), struct winsize
#include <unistd.h>    // read(), isatty()
#include <sys/ioctl.h> // TIOCGWINSZ
#endif

#include "core/rvvm_user.h"
#include "rvvm/rvvm.h"        /* rvvm_set_cmdline / rvvm_append_cmdline */
#include "util/threading.h"   /* rvvm_thread_create */

/*
 * The program a run with nothing to say about it starts.
 *
 * "/bin/busybox sh", NOT "/bin/sh". Both name a shell and the second is what a
 * person would type, but they are not the same thing to exec:
 *
 *   /bin/sh is a symlink. Symlinks are deliberately never materialized on the
 *   host - that is what lets one busybox answer for its three hundred names
 *   (vp_rootfs.h) - so /bin/sh exists only in the archive's shadow. A run with a
 *   shadow installed resolves it; this binary has no shadow, because installing
 *   one is a host's job (rvvm_user_set_shadow()) and rvvm_user is the host with
 *   no bundle. Opening it therefore fails with ENOENT, which is a poor thing for
 *   a default to do on its first run.
 *
 *   /bin/busybox is a real file, and "busybox sh" is busybox's own documented
 *   dispatch - which is how Alpine invokes a shell anyway. So the default is the
 *   same shell, reached the way that works without a shadow.
 *
 * Not /sbin/init, which is also present: busybox init reads /etc/inittab and
 * claims a console, and there is no init here to supervise anything, so it would
 * come up printing complaints instead of a prompt.
 */
#define RVVM_USER_DEFAULT_INIT "/bin/busybox"
#define RVVM_USER_DEFAULT_VERB "sh"

static void print_usage(const char* self)
{
    fprintf(stderr,
            "usage: %s [-prefix <dir>] [-no-rootfs] [-cmdline <args>]"
            " [-append <args>] [guest_elf [args...]]\n"
            "\n"
            "With no guest_elf, boots %s in the default prefix"
            " (./runtime/rootfs).\n"
            "\n"
            "  -no-rootfs       no hostfs base at all: the guest's / is an empty\n"
            "                  memory filesystem, which dies with the run. For a\n"
            "                  program that needs no files, and for one that only\n"
            "                  wants somewhere to write.\n"
            "  -prefix <dir>    the hostfs base: where the guest's / lives.\n"
            "                  A HOST path and NOT a boot argument - it would\n"
            "                  mean a different run on every machine, since the\n"
            "                  rootfs sits under the APK's private directory on\n"
            "                  Android and under the release tree on win32.\n"
            "                  Relative paths resolve against the working"
            " directory.\n"
            "  -cmdline <args>  boot arguments, in Linux's spelling. The guest\n"
            "                  reads them back from /proc/cmdline.\n"
            "  -append <args>   more of them, keeping what -cmdline set.\n"
            "\n"
            "The console is whatever this process's stdin is, and both are honest.\n"
            "On a terminal: it is served as one - the guest's isatty() is true, its\n"
            "own line editor echoes what you type, ^C reaches its foreground job,\n"
            "and the window size is the terminal's. Redirected (`printf 'ls\\n' |\n"
            "rvvm_user`, `rvvm_user < script`) it stays a plain pipe to the host's\n"
            "stdin: no session is attached and isatty() answers false, which is\n"
            "what a script driving the run wants back.\n"
            "\n"
            "The program, in order of precedence: the ELF named here, else\n"
            "init= from -cmdline, else %s.\n"
            "\n"
            "loglevel=<name|0..4> and a bare debug set this process's logging.\n"
            "root= is recorded and reported but not applied - the guest's\n"
            "filesystem is answered from the archive index, which knows nothing\n"
            "of a sub-root; see rvvm_user_set_cmdline() for what that would take.\n",
            self, RVVM_USER_DEFAULT_INIT, RVVM_USER_DEFAULT_INIT);
}

/* ============================================================
 * The host console
 *
 * A run's console is a terminal or it is a pipe, and the only evidence there is
 * this process's own stdin. Both answers are honest, so the question is asked
 * once, here, and both are served the way a real host serves them:
 *
 *   Terminal. A session is attached (rvvm_tty_open + rvvm_tty_attach), the host
 *   console is put into the mode a terminal emulator is in, and the keyboard is
 *   pumped into the guest's line discipline. That is what makes the guest's
 *   isatty() true, and a shell that is told it has a terminal prints a prompt,
 *   edits the line itself and hands ^C to its own foreground job.
 *
 *   Pipe (`printf 'ls\n' | rvvm_user`, `rvvm_user < script`). Nothing is taken
 *   over. No session is attached, so the guest's fd 0 keeps reaching the host's
 *   stdin and isatty() answers false - the run is the honest non-interactive one,
 *   and the transcript a driver captured is the whole of what happened.
 *
 * No renderer, and none is needed. The session is here so the core's isatty() and
 * termios answers have somewhere to live and so the input ring has a discipline
 * to run; the guest's own output still reaches the host console, because the
 * core forwards fd 1/2 to the host fd and only *mirrors* them into the screen.
 * A shell that took over its line editing has ECHO off and echoes what you type
 * to fd 1 itself, so the typing is visible without a single cell of that screen
 * ever being drawn.
 * ============================================================ */

/* The grid the session is opened with when the console will not say how big it
 * is. The core's own default is the same 24x80; this is only what
 * rvvm_tty_open() is handed, and TIOCGWINSZ reports it to the guest. */
#define CONSOLE_FALLBACK_ROWS 24
#define CONSOLE_FALLBACK_COLS 80

/* How long the pump waits for the run to exist before feeding anyway. The guest
 * cannot have consumed a byte before jump_start() has wiped the console state,
 * so a run that never starts must not wedge the keyboard forever. */
#define CONSOLE_START_WAIT_MS 5000

/* The run the keyboard belongs to, or NULL once there is none.
 *
 * Read before every delivery rather than taken as an argument, because the run
 * this points at is freed inside rvvm_user_linux_ex() on its way out. The exit
 * callback clears it while the machine is still alive, so the pump stops feeding
 * a run that has ended instead of reaching into a freed one. */
static rvvm_machine_t* g_console_machine = NULL;

/* The console modes as this process found them. Restored on the way out: a tool
 * that exits leaving the terminal in raw mode makes the shell that comes next
 * unusable, which is the kind of bug that outlives the job that caused it. */
#if defined(HOST_TARGET_WIN32)
static DWORD g_console_mode_in  = 0;
static DWORD g_console_mode_out = 0;
#else
static struct termios g_console_mode_saved;
#endif
static bool g_console_taken = false;

static void console_sleep_ms(unsigned ms)
{
#if defined(HOST_TARGET_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* The host console's window size in rows/columns, or false when it has none.
 *
 * Reached through the console rather than through whichever handle stdout
 * happens to be: the geometry belongs to the terminal, not to the descriptor
 * pointing at it, and a host whose stdout is redirected is still attached to a
 * console. */
static bool console_geometry(int* rows, int* cols)
{
#if defined(HOST_TARGET_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    bool   owned = false;
    bool   ok;

    if (!h || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
        h = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (!h || h == INVALID_HANDLE_VALUE) {
            return false; /* no console at all (started detached from one) */
        }
        owned = true;
    }
    ok = GetConsoleScreenBufferInfo(h, &info) != 0;
    if (owned) {
        CloseHandle(h);
    }
    if (!ok) {
        return false;
    }
    /* The window rect, not the buffer rect: the buffer is usually taller than
     * what anyone can see, and the guest lays itself out for the window. */
    *rows = (int)(info.srWindow.Bottom - info.srWindow.Top) + 1;
    *cols = (int)(info.srWindow.Right - info.srWindow.Left) + 1;
#else
    struct winsize ws;

    /* Either descriptor answers on some hosts and only one on others, so both
     * are asked before giving up. */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 &&
        ioctl(STDIN_FILENO,  TIOCGWINSZ, &ws) != 0) {
        return false;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
#endif
    return *rows > 0 && *cols > 0;
}

/* Move the session's grid to what the console says now, and let the guest's
 * foreground job hear about it (see rvvm_user_tty_resize for both halves).
 *
 * Cheap and idempotent - a size the session already has costs one query and one
 * comparison - which is what makes it safe to call on a poll, and from a read
 * that a signal just interrupted. Nothing to do once the run has ended: the
 * machine pointer is cleared from the guest's own exit path (console_run_ended),
 * which is the last moment it can be read safely. */
static void console_apply_resize(void)
{
    rvvm_machine_t* machine = g_console_machine;
    int rows = 0, cols = 0;

    if (!machine || !console_geometry(&rows, &cols)) {
        return;
    }
    rvvm_user_tty_resize(machine, rows, cols);
}

#if defined(HOST_TARGET_WIN32)
/* Where a Windows resize is picked up from.
 *
 * There is no signal for it here, and no event that can be waited for: the
 * console's window-size event arrives through ReadConsoleInput, and the pump
 * owns that handle with ReadFile - the two cannot share it. So the size is
 * polled, at the same 250ms the win32 host polls it at, and the work per tick is
 * a console query plus a comparison. */
static void* console_geometry_pump(void* arg)
{
    (void)arg;
    for (;;) {
        console_sleep_ms(250);
        console_apply_resize();
    }
    return NULL;   /* not reached: the loop ends only with the process */
}
#else
/* Where a POSIX resize is picked up from.
 *
 * Here the resize *is* a signal - SIGWINCH, which the kernel raises for a
 * terminal whose window changed - so there is nothing to poll. The handler does
 * nothing but let the signal arrive: SIGWINCH's default disposition is "ignore",
 * so with no handler the pump's blocked read would never come back at all. With
 * one it returns EINTR, and that is where the size is applied (see
 * console_read_stdin). Nothing more may happen in here: taking locks or
 * delivering signals is not async-signal-safe. */
static void console_winch_handler(int sig)
{
    (void)sig;
}
#endif

/* Put the host console into the mode a terminal emulator is in, and say whether
 * there was one to do it to.
 *
 * Input raw on both platforms: no line assembly, no echo, and - the one that
 * matters - ^C/^Z arrive as bytes instead of being turned into console events by
 * the host, so the guest's own termios decides what they mean (a guest shell
 * gets its SIGINT, vi gets its literal ^Z). What the guest is sent is then the
 * byte sequence a terminal sends: printable UTF-8, '\r' for Enter, 0x7F for
 * Backspace, CSI sequences for the arrows, 0x03/0x04 for ^C/^D.
 *
 * OPOST/ONLCR is deliberately left alone on POSIX. The guest's stdio emits a bare
 * LF and nothing sits between write(1) and this terminal to turn it into CRLF -
 * the core rewrites LF for the session's screen only - so this terminal's own
 * ONLCR is what keeps the output from staircasing. Windows needs no such care:
 * the console renders what it is given and ENABLE_VIRTUAL_TERMINAL_PROCESSING is
 * what makes it interpret the guest's escapes instead of printing them.
 */
static bool console_take(void)
{
#if defined(HOST_TARGET_WIN32)
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;

    if (!hin || hin == INVALID_HANDLE_VALUE || GetFileType(hin) != FILE_TYPE_CHAR ||
        !GetConsoleMode(hin, &mode)) {
        return false; /* a pipe, a file, or no stdin at all */
    }
    g_console_mode_in = mode;
    if (!SetConsoleMode(hin, (mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                             ENABLE_PROCESSED_INPUT))
                                | ENABLE_VIRTUAL_TERMINAL_INPUT)) {
        return false;
    }
    /* ENABLE_WINDOW_INPUT is not requested: the pump reads bytes with ReadFile()
     * and ReadConsoleInput() cannot share the handle. */
    if (hout && hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &mode)) {
        g_console_mode_out = mode;
        SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                  ENABLE_PROCESSED_OUTPUT);
    }
    g_console_taken = true;
    return true;
#else
    struct termios raw;

    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_console_mode_saved)) {
        return false; /* a pipe, a file, or no terminal at all */
    }
    raw = g_console_mode_saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) {
        return false;
    }
    g_console_taken = true;
    return true;
#endif
}

/* Give the console back the way it was found. */
static void console_give_back(void)
{
    if (!g_console_taken) {
        return;
    }
    g_console_taken = false;
#if defined(HOST_TARGET_WIN32)
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hin && hin != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hin, g_console_mode_in);
    }
    if (hout && hout != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hout, g_console_mode_out);
    }
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &g_console_mode_saved);
#endif
}

/* One read of the host's keyboard. ReadFile() and not read(2) on Windows: the
 * CRT's own buffering sits between read(2) and a console handle, and the point
 * here is to hand each keystroke over as it arrives. Zero means the input is
 * over - a closed console, or a pipe that ended - and there is no other way to
 * learn that from a read.
 *
 * EINTR is not that: a read interrupted by a signal (a window resized under a
 * full-screen guest, say) resumes where it left off, and treating it as the end
 * of the keyboard would answer ^D to a shell that is still being typed at. */
static size_t console_read_stdin(void* buf, size_t len)
{
#if defined(HOST_TARGET_WIN32)
    DWORD got = 0;

    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, (DWORD)len, &got, NULL)) {
        return 0;
    }
    return got;
#else
    for (;;) {
        ssize_t got = read(STDIN_FILENO, buf, len);

        if (got < 0 && errno == EINTR) {
            /* The interruption is itself the message: on this host a window
             * resize arrives as exactly this (SIGWINCH), and the read that came
             * back is the one that was waiting for the next keystroke - so the
             * new size is applied here rather than on a poll, and there may be
             * no further keystroke to hang a check on. */
            console_apply_resize();
            continue;
        }
        return got > 0 ? (size_t)got : 0;
    }
#endif
}

/* The host keyboard -> the guest's line discipline.
 *
 * Never joined. It spends its life parked in a read that the console only ends
 * when a key is pressed, so the thread is left to the process exit - the same
 * reasoning the win32 host's pump uses. What does have to be true is that it
 * stops feeding a run that has ended, which is what the machine pointer being
 * cleared on exit is for. */
static void* console_pump(void* arg)
{
    char buf[512];

    (void)arg;

    /* Do not consume the keyboard before the run exists: jump_start() wipes the
     * console state as it spins the vCPU up ("no type-ahead left in the ring"),
     * so bytes handed over before that point are discarded. Waiting costs
     * nothing - the keys simply stay in the console's queue. */
    for (unsigned waited_ms = 0;
         g_console_machine && !rvvm_user_is_started(g_console_machine);
         waited_ms++) {
        if (waited_ms >= CONSOLE_START_WAIT_MS) {
            break; /* a run that never starts must not wedge the pump */
        }
        console_sleep_ms(1);
    }

    for (;;) {
        size_t got = console_read_stdin(buf, sizeof(buf));
        /* Read after the blocking read, not before: this is what stops the feed
         * when the run has ended while the keyboard was idle. */
        rvvm_machine_t* machine = g_console_machine;

        if (!got) {
            /* The input ended. The guest's end is a terminal, and a terminal
             * says so with Ctrl-D - which is also what keeps a shell parked on
             * read(0) from waiting forever for input that cannot come. */
            if (machine) {
                rvvm_user_tty_input(machine, "\x04", 1);
            }
            return NULL;
        }
        if (machine) {
            rvvm_user_tty_input(machine, buf, got);
        }
    }
}

/* The run ended: nothing more may be typed into it. Fired from inside the guest's
 * own exit path, while the machine is still alive - which is the last moment at
 * which the pointer above can be cleared safely. */
static void console_run_ended(rvvm_machine_t* machine, int exit_code)
{
    (void)machine;
    (void)exit_code;
    g_console_machine = NULL;
}

/*
 * Serve this run's console, if it is one worth serving.
 *
 * Doing nothing is a complete answer, not a failure: with stdin redirected that
 * is exactly what the run should be, and the console modes were never touched in
 * that case.
 *
 * Every step is undone if a later one fails. A session with no pump would be
 * worse than no session at all: the guest's fd 0 would read from a discipline
 * nothing ever feeds, and read(0) would park forever instead of failing.
 */
static void console_serve(rvvm_machine_t* machine)
{
    rvvm_tty_t* tty;
    int         rows = CONSOLE_FALLBACK_ROWS;
    int         cols = CONSOLE_FALLBACK_COLS;

    if (!console_take()) {
        return; /* stdin is not a terminal: the run stays a pipe */
    }
    if (!console_geometry(&rows, &cols)) {
        rows = CONSOLE_FALLBACK_ROWS;
        cols = CONSOLE_FALLBACK_COLS;
    }
    tty = rvvm_tty_open(rows, cols);
    if (!tty) {
        console_give_back();
        fprintf(stderr, "rvvm_user: console: terminal, but no session could be"
                        " opened; stdin passes straight through\n");
        return;
    }
    rvvm_tty_attach(tty, machine);
    g_console_machine = machine;
    /* Set after the session is attached, so the run cannot end before the pump
     * exists and the callback that stops it is already in place. */
    rvvm_user_set_exit_callback(machine, console_run_ended);
    if (!rvvm_thread_create(console_pump, NULL)) {
        rvvm_tty_detach(tty, machine);
        rvvm_tty_close(tty);
        g_console_machine = NULL;
        console_give_back();
        fprintf(stderr, "rvvm_user: console: terminal, but no pump thread;"
                        " stdin passes straight through\n");
        return;
    }
#if defined(HOST_TARGET_WIN32)
    /* The other half of serving a terminal: a window that changes size has to
     * reach the guest. Polled, because that is the only way here (see
     * console_geometry_pump). Not fatal if it fails - the grid then stays the
     * size the session was opened with. */
    if (!rvvm_thread_create(console_geometry_pump, NULL)) {
        fprintf(stderr, "rvvm_user: console: no geometry thread; the grid stays"
                        " at the size it was opened with\n");
    }
#else
    /* On this host the resize comes to us: SIGWINCH interrupts the pump's read,
     * and that is where the new size is applied (see console_read_stdin).
     * Installed here, once, and only for a console this process actually took
     * over - which is also why nothing has to take it down again. */
    struct sigaction winch;
    memset(&winch, 0, sizeof(winch));
    winch.sa_handler = console_winch_handler;
    sigemptyset(&winch.sa_mask);
    winch.sa_flags = 0;   /* no SA_RESTART: the blocked read has to come back */
    sigaction(SIGWINCH, &winch, NULL);
#endif
    fprintf(stderr, "rvvm_user: console: terminal %dx%d"
                    " (host keyboard, guest line discipline)\n", rows, cols);
}

/*
 * Why this does not just call rvvm_user_linux().
 *
 * rvvm_user_linux() creates the machine itself and hands it straight to
 * rvvm_user_linux_ex(), so its caller never holds a handle and there is nowhere
 * to set a command line before the image is loaded - which is the whole of what
 * a boot argument is. The seam is already public: rvvm_user_create() and
 * rvvm_user_linux_ex() are both PUBLIC, so the three steps are done here
 * instead, in the order a boot does them.
 *
 * Doing it this way rather than adding a setter to the core is deliberate: a
 * "pending cmdline" global would have to be consumed by rvvm_user_linux_ex() and
 * would then be a second way for every host to configure a machine - the thing
 * the win32 bridge and the Android host already have (win32_host_set_cmdline /
 * nativeSetCmdline) and the reason rvvm_user_linux_ex(machine, ...) exists as a
 * separate entry point at all.
 */
int main(int argc, char** argv, char** envp)
{
    rvvm_machine_t* machine;
    const char*     prefix  = NULL;
    const char*     cmdline = NULL;
    const char*     append  = NULL;
    bool            no_rootfs = false;
    int             guest_argc;
    char**          guest_argv;
    /* Hoisted out of the default-program branch below so the debug line can see
     * them; the branch is the only writer. */
    char          chosen[512];
    char          verb[64];
    char*         args[3];

    /* stderr unbuffered, before anything is printed.
     *
     * This binary is the one people run with its output redirected to a file or
     * a pipe - that is how a driver reads a guest's transcript - and two things
     * go wrong when it is not. A guest that dies takes the process with it
     * without flushing, so the last lines written are the ones lost, and those
     * are the ones naming the crash. Worse, stacktrace_print() writes its frames
     * straight to the stderr FILE* (stacktrace.c, via libbacktrace) rather than
     * through rvvm_warn, so a crash *inside* a stdio operation on stderr leaves
     * that stream's lock held and the frames never appear at all - the output
     * stops at "Stacktrace:" with nothing under it, which is the one case where
     * the trace was wanted.
     *
     * Unbuffered costs nothing here: a handful of lines per run, and rvvm_warn's
     * own sink (utils.c) is where the per-syscall volume goes regardless. */
    setvbuf(stderr, NULL, _IONBF, 0);

    int i = 1;
    for (; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            /* Handled here rather than in the no-arguments branch below, so it
             * works in any position: `rvvm_user --help` and
             * `rvvm_user -cmdline "..." --help` are both a person asking for
             * help, and a branch that only looked at argc would answer the
             * second one by trying to boot "--help". */
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-prefix") || !strcmp(argv[i], "--prefix")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            prefix = argv[++i];
        } else if (!strncmp(argv[i], "-prefix=", 8)) {
            prefix = argv[i] + 8;
        } else if (!strncmp(argv[i], "--prefix=", 9)) {
            prefix = argv[i] + 9;
        } else if (!strcmp(argv[i], "-no-rootfs") || !strcmp(argv[i], "--no-rootfs")) {
            /* An explicit ask for the run with no rootfs at all, which used to be
             * unexpressible from here: the default prefix is compiled in, and the
             * only way past it was a caller of the library that never set one. What
             * it produces is not a passthrough run - the guest's "/" is an empty
             * memory filesystem that dies with the run, which is what a program
             * that needs no files can be run against and what a program that wants
             * somewhere to write can use. See userland_root_backing_sync(). */
            no_rootfs = true;
        } else if (!strcmp(argv[i], "-cmdline") || !strcmp(argv[i], "--cmdline")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            cmdline = argv[++i];
        } else if (!strncmp(argv[i], "-cmdline=", 9)) {
            cmdline = argv[i] + 9;
        } else if (!strncmp(argv[i], "--cmdline=", 10)) {
            cmdline = argv[i] + 10;
        } else if (!strcmp(argv[i], "-append") || !strcmp(argv[i], "--append")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            append = argv[++i];
        } else if (!strncmp(argv[i], "-append=", 8)) {
            append = argv[i] + 8;
        } else if (!strncmp(argv[i], "--append=", 9)) {
            append = argv[i] + 9;
        } else {
            break;   /* the first thing that is not an option is the ELF */
        }
    }

    if (argc < 2) {
        /* No arguments at all is a run, not a usage error. Both defaults already
         * exist - the prefix is USERLAND_DEFAULT_PREFIX (./runtime/rootfs, see
         * rvvm_user_create) and the program is RVVM_USER_DEFAULT_INIT below - so
         * the shortest useful command is the one with nothing in it. A userland
         * emulator whose no-argument case prints help has a userland emulator
         * that cannot be run by a person who has just installed it.
         *
         * Nothing to do here: control falls through to the defaults. -h is
         * handled in the option loop above, which cannot run in this case. */
    } else if (i >= argc && !(cmdline || append || no_rootfs)) {
        /* Options given, no ELF, and nothing that could name one: that IS a
         * mistake, and saying so beats quietly booting a shell the caller did not
         * ask for. With -cmdline/-append it is not a mistake, because init= in
         * there is the program (see the precedence below) - so this has to
         * look at what those were set to rather than only at i. -no-rootfs counts
         * for the same reason: it changes where the guest's / is and nothing about
         * which program runs, so asking for a run with no rootfs and the default
         * program is a complete request. */
        fprintf(stderr, "%s: options given but no guest program\n\n", argv[0]);
        print_usage(argv[0]);
        return 1;
    }

    machine = rvvm_user_create();
    if (!machine) {
        return -1;
    }

    /* Before the image is opened: both of these have to be in effect before the
     * first path the guest resolves, and rvvm_user_linux_ex() loads the ELF
     * immediately after this. */
    if (no_rootfs) {
        /* After the default prefix was applied by rvvm_user_create(), so this is an
         * override rather than an absence - and it moves the root row with it,
         * which is the whole of what "no rootfs" now means. */
        rvvm_user_set_prefix(machine, NULL);
        fprintf(stderr, "rvvm_user: prefix: none; the guest's / is an empty "
                        "filesystem\n");
    } else if (prefix) {
        rvvm_user_set_prefix(machine, prefix);
        /* Echoed resolved, because that is the string every guest path is built
         * from, and "it found my directory" is worth seeing before the guest runs
         * rather than inferred from a run that happened to work. */
        fprintf(stderr, "rvvm_user: prefix: %s\n", rvvm_user_get_prefix(machine));
    }

    /* Two calls rather than one, and the same two src/main.c makes (lines
     * 379-383): the appending setter is what puts the separator between the
     * arguments and bounds the result, so building a merged string here would be
     * a second set of rules for something the machine already knows how to do. */
    if (cmdline) {
        rvvm_user_set_cmdline(machine, cmdline);
    }
    if (append) {
        rvvm_append_cmdline(machine, append);
    }

    /* What the guest will see, printed rather than assumed: /proc/cmdline is
     * generated from this string and a run whose arguments silently did not
     * land is a run that looks like the guest ignored them. */
    if (cmdline || append) {
        fprintf(stderr, "rvvm_user: cmdline: %s\n", rvvm_user_get_cmdline(machine));
    }

    /* Which program runs, in Linux's order of precedence:
     *
     *   1. an ELF named on the command line. Explicit beats everything, and it is
     *      the only form that can also pass the program arguments.
     *   2. init= from the boot arguments. This is where init= finally means
     *      something: this binary *is* the host, so it is the one that has to
     *      honour it, which is exactly why the core refuses to (see
     *      rvvm_user_set_cmdline - the core cannot know what programs exist).
     *   3. /bin/sh.
     *
     * The default is a shell rather than an init because that is what this is: a
     * userland emulator with no init to run. The rootfs does ship /sbin/init (a
     * busybox link, in the archive's shadow - symlinks are never materialized on
     * the host), and it would be a worse default: busybox init reads /etc/inittab
     * and wants a console it does not have here, so the run would come up as a
     * process printing complaints instead of a prompt.
     */
    if (i < argc) {
        guest_argc = argc - i;
        guest_argv = argv + i;
    } else {
        const char* init = rvvm_user_cmdline_arg(machine, "init");
        /* argv[argc] is NULL by the C standard, and guest_setup_stack() walks
         * the array to that terminator, hence three slots for two entries. */
        if (init && init[0]) {
            fprintf(stderr, "rvvm_user: init=%s\n", init);
            snprintf(chosen, sizeof(chosen), "%s", init);
        } else {
            snprintf(chosen, sizeof(chosen), "%s", RVVM_USER_DEFAULT_INIT);
        }
        snprintf(verb, sizeof(verb), "%s", RVVM_USER_DEFAULT_VERB);
        /* A char* pointing at the buffers, not a cast of them: argv is an array
         * of pointers, and (char**)&chosen would make guest_argv[0] read the
         * first eight bytes of the path as an address. */
        args[0]    = chosen;
        args[1]    = verb;
        args[2]    = NULL;
        guest_argc = 2;
        guest_argv = args;
    }

    /* One line naming the program and the prefix it will see: both are decided
     * above from three sources, and "which shell did that actually start" is not
     * a question the output can always answer. */
    /* One line naming the program and the prefix it will see: both are decided
     * above from three sources, and "which shell did that actually start" is not
     * a question the output can always answer. */
    fprintf(stderr, "rvvm_user: boot %s\n", guest_argv[0]);
    /* "no prefix" is not "passthrough" any more: it is an empty memory filesystem
     * for the guest's /, and saying passthrough here would name the one thing it
     * is not. */
    fprintf(stderr, "rvvm_user: prefix %s\n",
            rvvm_user_get_prefix(machine) ? rvvm_user_get_prefix(machine)
                                          : "(none; the guest's / is memory)");
    fprintf(stderr, "rvvm_user: guest root %s\n",
            rvvm_user_get_guest_root(machine) ? rvvm_user_get_guest_root(machine)
                                              : "(none)");

    /* The console, before the guest is loaded rather than after it starts: the
     * session attached here is what makes the guest's first isatty() - the probe
     * every libc makes on its first stdio call - answer. A shell that is told it
     * has a terminal prints a prompt and edits its own lines; one that is told it
     * has a pipe does neither, which is the whole of what a redirected run
     * should be. */
    console_serve(machine);

    int rc = rvvm_user_linux_ex(machine, guest_argc, guest_argv, envp);

    /* The run is over and rvvm_user_linux_ex() has freed the machine on its way
     * out, so this is the last moment the host console is ours to give back.
     * Nothing is detached from the machine afterwards - it no longer exists, and
     * the session is not closed either: a run whose threads did not wind down
     * inside the core's grace period may still be writing into that screen, and
     * the process is about to exit anyway. */
    g_console_machine = NULL;
    console_give_back();
    return rc;
}