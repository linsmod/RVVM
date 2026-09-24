/*
 * test_jobctl.c - Job-control regression for the rvvm_user core
 *
 * The interactive parts of job control (^C/^Z at a terminal) are exercised by
 * tools/jobctl_e2e.ps1 against a real shell. This sample covers what a shell
 * cannot be asked to demonstrate, and does it as a guest, so both halves of the
 * contract - what the kernel answers and what the terminal does - are checked
 * from inside the machine:
 *
 *   1. identity: a launched image leads its own process group and session
 *      (tcgetpgrp() == getpgrp() is what makes busybox ash turn job control ON
 *      instead of printing "can't access tty"), setpgid()'s two failures are
 *      ESRCH and EPERM, and setsid() refuses a session leader.
 *   2. group stop/continue: kill(-pgid, SIGSTOP) parks a whole job,
 *      wait4(WUNTRACED) reports WIFSTOPPED/WSTOPSIG, the stopped job really
 *      produces nothing, kill(-pgid, SIGCONT) resumes it and
 *      wait4(WCONTINUED) reports WIFCONTINUED. (busybox ash never asks for
 *      WCONTINUED, so only a guest can verify it.)
 *   3. pty line discipline: a pty opened by the test is made the child's
 *      controlling terminal, the child is put in the pty's foreground group
 *      with tcsetpgrp(), and ^C written to the *master* is what the pty's ISIG
 *      delivers - the notation is echoed back and the job dies with
 *      128+SIGINT. This is the shape a session of `rvvm_ash --serve` will use.
 *   4. the same session resized: TIOCSWINSZ on the master must raise SIGWINCH
 *      in the foreground group (a full-screen program only re-lays itself out
 *      when it receives it), and the new size must be what the slave reads.
 *
 * Expected console output: one "ok  <what>" line per check, then
 * "=== PASS: job control ===" (or "=== FAIL: N check(s) ===").
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void check(int ok, const char* what)
{
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
    if (!ok) {
        fails++;
    }
}

static void delay_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* One formatted message at a time: each is printed before the next is built. */
static char* msgf(const char* fmt, ...)
{
    static char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

/* --- 1: process group and session identity --- */
static void test_identity(void)
{
    pid_t pid = getpid();

    check(getsid(0) == pid, "getsid(0) == getpid(): the run root leads a session");
    check(getsid(pid) == pid, "getsid(pid) answers the same");
    check(getpgrp() == pid, "getpgrp() == getpid(): and its own process group");
    check(getpgid(0) == pid, "getpgid(0) answers the same");
    check(setpgid(0, 0) == 0, "setpgid(0,0) succeeds (already that group)");
    check(setpgid(0, pid) == 0, "setpgid(0,getpid()) succeeds");

    errno = 0;
    check(setsid() == -1 && errno == EPERM,
          "setsid() on a session leader is EPERM (the shell's own probe)");

    errno = 0;
    check(setpgid(pid + 4242, 0) == -1 && errno == ESRCH,
          "setpgid(unknown pid) is ESRCH");

    errno = 0;
    check(getpgid(pid + 4242) == -1 && errno == ESRCH,
          "getpgid(unknown pid) is ESRCH");

    errno = 0;
    check(getsid(pid + 4242) == -1 && errno == ESRCH,
          "getsid(unknown pid) is ESRCH");
}

/* --- 2: a whole group stopped, reported, and continued --- */
static void test_group_stop(void)
{
    int fds[2];
    if (pipe(fds)) {
        check(0, "pipe()");
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        setpgid(0, 0);                      /* its own group, like a shell's job */
        /* Runs for ~500 ms and then reports; short enough that "it did not
         * finish while stopped" is a sharp claim. */
        for (int i = 0; i < 10; i++) {
            delay_ms(50);
        }
        if (write(fds[1], "RAN", 3) != 3) {
            _exit(1);
        }
        _exit(0);
    }
    if (child < 0) {
        check(0, "fork()");
        return;
    }
    setpgid(child, child);                  /* both sides, so neither side races */

    delay_ms(50);
    check(kill(-child, SIGSTOP) == 0, "kill(-pgid, SIGSTOP) reaches the whole group");

    int st = 0;
    check(waitpid(child, &st, WUNTRACED) == child, "wait4(WUNTRACED) reports the stop");
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP,
          "WIFSTOPPED() with WSTOPSIG() == SIGSTOP");

    /* A stopped job is parked, not merely reported: let more than its whole
     * runtime pass and ask again - a job that was still running would have
     * exited (and WNOHANG would hand it over). The job's own output is checked
     * at the end instead of on a timed read, because a guest has no way to set
     * O_NONBLOCK on this host (see the note in test_group_stop's caller). */
    delay_ms(900);
    check(waitpid(child, &st, WNOHANG) == 0, "the stopped job is still there, not exited");

    check(kill(-child, SIGCONT) == 0, "kill(-pgid, SIGCONT) resumes the group");

    st = 0;
    check(waitpid(child, &st, WCONTINUED) == child, "wait4(WCONTINUED) reports the resume");
    check(WIFCONTINUED(st), "WIFCONTINUED() is what the status word says");

    /* ...and the job really carried on: this read blocks until its marker. */
    char probe[4] = {0};
    check(read(fds[0], probe, 3) == 3 && !memcmp(probe, "RAN", 3),
          "the continued job ran to its own marker");

    st = 0;
    check(waitpid(child, &st, 0) == child, "and then exits");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "with the status it asked for");

    close(fds[0]);
    close(fds[1]);
}

/* --- 3: ^C on a pty reaches the terminal's foreground group --- */
static void test_pty_sigint(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0, "posix_openpt()");
    if (master < 0) {
        return;
    }
    check(grantpt(master) == 0, "grantpt()");
    check(unlockpt(master) == 0, "unlockpt()");

    const char* name = ptsname(master);
    check(name != NULL, "ptsname()");
    if (!name) {
        close(master);
        return;
    }
    char slave_path[64];
    snprintf(slave_path, sizeof(slave_path), "%s", name);

    int slave = open(slave_path, O_RDWR);   /* no O_NOCTTY: this pty is the point */
    check(slave >= 0, "open() the slave");
    if (slave < 0) {
        close(master);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(master);
        setsid();                           /* its own session, led by itself... */
        ioctl(slave, TIOCSCTTY, 0);         /* ...whose terminal is this pty */
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        /* Nothing else to do: it must be the ^C that ends this. If the signal
         * never arrives the test still terminates, with a wrong status. */
        delay_ms(5000);
        _exit(7);
    }
    if (child < 0) {
        check(0, "fork()");
        close(master);
        close(slave);
        return;
    }

    /* The child called setsid(), so its group is its own pid. A shell sets the
     * foreground group from its own side of the terminal (the slave); the
     * session server holding the master reads it back - one terminal, one
     * foreground group. */
    delay_ms(300);
    errno = 0;
    check(ioctl(slave, TIOCSPGRP, &child) == 0, "tcsetpgrp() on the slave");
    pid_t fg = 0;
    check(ioctl(master, TIOCGPGRP, &fg) == 0 && fg == child,
          "tcgetpgrp() on the master reads back the same group");

    /* This is the whole point of a pty: the byte a terminal sends, not a call. */
    const char ctrl_c = 0x03;
    check(write(master, &ctrl_c, 1) == 1, "write ^C to the master");

    struct pollfd pfd = { master, POLLIN, 0 };
    char echo[32] = {0};
    int got = (poll(&pfd, 1, 1000) > 0) ? (int)read(master, echo, sizeof(echo) - 1) : -1;
    check(got > 0 && strstr(echo, "^C") != NULL,
          "the line discipline echoed the ^C notation back");

    int st = 0;
    check(waitpid(child, &st, 0) == child, "wait4() reaps the signalled job");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGINT,
          "the job died with 128+SIGINT, the shell's view of a ^C'd command");

    close(slave);
    close(master);
}

/* The child's SIGWINCH handler: reports the resize through the pty, which is
 * where a full-screen program would redraw. */
static void on_winch(int sig)
{
    (void)sig;
    ssize_t n = write(1, "WINCH\n", 6);
    (void)n;
}

/* --- 4: a resize on the terminal reaches the session's foreground group ---
 *
 * TIOCSWINSZ is not just a number to store: a session server resizing the
 * terminal (a client's window, an ssh resize request) has to raise SIGWINCH in
 * the job that is drawing on it, or vim/top/less keep laying themselves out for
 * the old size. The signal goes to the terminal's foreground group - the same
 * set ^C and ^Z reach - and it is raised by whoever resizes the master, no
 * matter which address space that group lives in. */
static void test_pty_winch(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        check(0, msgf("posix_openpt() for the resize check (%s)", strerror(errno)));
        return;
    }
    grantpt(master);
    unlockpt(master);

    const char* name = ptsname(master);
    char slave_path[64];
    snprintf(slave_path, sizeof(slave_path), "%s", name ? name : "");
    int slave = name ? open(slave_path, O_RDWR) : -1;
    if (slave < 0) {
        check(0, "open() the slave for the resize check");
        close(master);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_winch;
        sigaction(SIGWINCH, &sa, NULL);
        /* Ready: the session is set up and the handler is armed. The newline
         * makes it a complete line, since the slave starts in canonical mode. */
        ssize_t r = write(1, "R\n", 2);
        (void)r;
        for (int i = 0; i < 200; i++) {
            delay_ms(50);
        }
        _exit(7);
    }
    if (child < 0) {
        check(0, "fork() for the resize check");
        close(slave);
        close(master);
        return;
    }

    /* Wait for the child's "ready" instead of racing its setsid(): the group a
     * resize is delivered to has to be the child's own. */
    struct pollfd pfd = { master, POLLIN, 0 };
    char buf[64] = {0};
    int ready = (poll(&pfd, 1, 2000) > 0) ? (int)read(master, buf, sizeof(buf) - 1) : -1;
    check(ready > 0 && strchr(buf, 'R') != NULL, "the session reports itself ready on the pty");

    check(ioctl(slave, TIOCSPGRP, &child) == 0, "the session is the pty's foreground group");

    struct winsize ws = {0};
    ws.ws_row = 30;
    ws.ws_col = 100;
    check(ioctl(master, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ(master) resizes to 30x100");

    struct winsize back = {0};
    check(ioctl(slave, TIOCGWINSZ, &back) == 0 && back.ws_row == 30 && back.ws_col == 100,
          msgf("the session reads the new size back (%ux%u)",
               (unsigned)back.ws_row, (unsigned)back.ws_col));

    pfd.revents = 0;
    memset(buf, 0, sizeof(buf));
    int got = (poll(&pfd, 1, 1500) > 0) ? (int)read(master, buf, sizeof(buf) - 1) : -1;
    check(got > 0 && strstr(buf, "WINCH") != NULL,
          "the resize raised SIGWINCH in the session's foreground group");

    kill(child, SIGKILL);
    int st = 0;
    waitpid(child, &st, 0);
    close(slave);
    close(master);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== job control test ===\n");

    test_identity();
    test_group_stop();
    test_pty_sigint();
    test_pty_winch();

    if (fails) {
        printf("=== FAIL: %d check(s) ===\n", fails);
        return 1;
    }
    printf("=== PASS: job control ===\n");
    return 0;
}