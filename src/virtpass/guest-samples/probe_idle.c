/*
 * probe_idle.c - drive idle the way a host does, and say where it stops
 *
 * A diagnostic, not a test with a verdict. idle (guest-samples/idle.c) is the run
 * root of a core: it holds the machine open and forks a shell onto a terminal the
 * host names. Every part of that had been verified in pieces, and the whole thing
 * had never been run end to end - so this does exactly what the host does, in one
 * process, and prints what happened at each step:
 *
 *   1. make a pty pair, the way a host does (the guest's own /dev/ptmx, which on
 *      this host is a real kernel one and on Windows is the simulated table)
 *   2. start idle on the slave end of one pair, as its --control
 *   3. make a second pair for the session, and write "spawn <path> <cols> <rows>"
 *      into the first one's master - one line, exactly what the host sends
 *   4. read the shell's output from the second pair's master, which is where a
 *      prompt would arrive
 *
 * Step 4 is the one that decides whether the arrangement works at all: a shell
 * that never writes anything is a session nobody can type at, and it looks
 * exactly like a shell that was never forked.
 *
 * Usage: probe_idle [seconds]      (default 5)
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    long seconds = (argc > 1) ? strtol(argv[1], NULL, 10) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }
    printf("=== idle probe ===\n");

    /* 1. The control pair. grantpt/unlockpt because the slave may not be opened
     * before the pair is unlocked, which is the same order a host uses. */
    int ctl_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (ctl_master < 0) {
        printf("posix_openpt(control) failed: %s\n", strerror(errno));
        return 1;
    }
    if (grantpt(ctl_master) || unlockpt(ctl_master)) {
        printf("grantpt/unlockpt(control) failed: %s\n", strerror(errno));
        return 1;
    }
    const char* ctl_name = ptsname(ctl_master);
    printf("control terminal: %s\n", ctl_name ? ctl_name : "(no name)");
    if (!ctl_name) {
        return 1;
    }
    /* Raw on the control channel: idle reads lines, and a line discipline would
     * assemble and echo them before it saw any. */
    struct termios raw;
    if (tcgetattr(ctl_master, &raw) == 0) {
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(ctl_master, TCSANOW, &raw);
    }

    /* 2. idle as the run root would be: on the control slave, told that path. */
    int ctl_slave = open(ctl_name, O_RDWR);
    if (ctl_slave < 0) {
        printf("open(%s) failed: %s\n", ctl_name, strerror(errno));
        return 1;
    }
    pid_t idle_pid = fork();
    if (idle_pid < 0) {
        printf("fork(idle) failed: %s\n", strerror(errno));
        return 1;
    }
    if (idle_pid == 0) {
        setsid();
        ioctl(ctl_slave, TIOCSCTTY, 0);
        dup2(ctl_slave, 0);
        dup2(ctl_slave, 1);
        dup2(ctl_slave, 2);
        if (ctl_slave > 2) {
            close(ctl_slave);
        }
        execl("/sbin/idle", "/sbin/idle", "--control", ctl_name,
              "--shell", "/bin/sh", (char*)NULL);
        _exit(127);
    }
    close(ctl_slave);
    printf("idle started (pid %d)\n", (int)idle_pid);
    msleep(300);

    /* 3. A session terminal, and the one line the host writes for it. */
    int ses_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (ses_master < 0) {
        printf("posix_openpt(session) failed: %s\n", strerror(errno));
        return 1;
    }
    if (grantpt(ses_master) || unlockpt(ses_master)) {
        printf("grantpt/unlockpt(session) failed: %s\n", strerror(errno));
        return 1;
    }
    const char* ses_name = ptsname(ses_master);
    printf("session terminal: %s\n", ses_name ? ses_name : "(no name)");
    if (!ses_name) {
        return 1;
    }
    {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = 24;
        ws.ws_col = 80;
        ioctl(ses_master, TIOCSWINSZ, &ws);
    }

    char req[128];
    snprintf(req, sizeof(req), "spawn %s 80 24\n", ses_name);
    ssize_t wrote = write(ctl_master, req, strlen(req));
    printf("wrote the spawn request (%zd of %zu byte(s)): %s\n",
           wrote, strlen(req), wrote > 0 ? "ok" : strerror(errno));

    /* 4. Type something in, then read what comes back.
     *
     * A shell on a terminal with nothing typed prints *nothing* - no prompt,
     * because an interactive sh decides it is interactive from its own terminal
     * and a quiet one stays quiet - so waiting for output before typing proves
     * nothing either way. Typing first is what a client does, and what comes
     * back is the answer to a question the guest itself can answer.
     *
     * "ps -ef\n" rather than something simpler because the whole point of a
     * session is that it can see: if the reply lists the shell that ran it, the
     * machine is shared and the sessions see each other. */
    printf("typing `ps -ef` into the session...\n");
    {
        const char* typed = "ps -ef\n";
        ssize_t w = write(ses_master, typed, strlen(typed));
        printf("  wrote %zd byte(s): %s\n", w, w > 0 ? "ok" : strerror(errno));
    }

    /* Is the session's own terminal live at all? Before believing that the shell
     * is silent, ask the terminal directly: the echo of what was just written
     * comes back on the master if the pair is connected, and nothing at all if it
     * is not. That separates "the shell said nothing" from "the terminal is not
     * carrying anything", which look identical from the outside and are fixed in
     * completely different places. */
    printf("checking the session terminal is live (echo comes back?)...\n");
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ses_master, &r);
        struct timeval tv = { 0, 500000 };   /* 500ms */
        if (select(ses_master + 1, &r, NULL, NULL, &tv) > 0) {
            char buf[256];
            ssize_t got = read(ses_master, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                printf("  the terminal echoed %zd byte(s): ", got);
                for (ssize_t i = 0; i < got; i++) {
                    unsigned char c = (unsigned char)buf[i];
                    if (c == 0x1b) {
                        printf("<ESC>");
                    } else if (c < 0x20 || c == 0x7f) {
                        printf("<%02x>", c);
                    } else {
                        putchar(c);
                    }
                }
                putchar('\n');
            } else {
                printf("  the terminal gave end of output (nothing holds the slave)\n");
            }
        } else {
            printf("  the terminal echoed NOTHING - the pair is not connected\n");
        }
    }

    printf("reading the session for %lds...\n", seconds);
    long deadline_ms = seconds * 1000;
    long waited = 0;
    int  saw = 0;
    while (waited < deadline_ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ses_master, &r);
        struct timeval tv = { 0, 200000 };   /* 200ms */
        int n = select(ses_master + 1, &r, NULL, NULL, &tv);
        if (n > 0) {
            char buf[512];
            ssize_t got = read(ses_master, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                printf("session said (%zd byte(s)): ", got);
                for (ssize_t i = 0; i < got; i++) {
                    unsigned char c = (unsigned char)buf[i];
                    if (c == 0x1b) {
                        printf("<ESC>");
                    } else if (c < 0x20 || c == 0x7f) {
                        printf("<%02x>", c);
                    } else {
                        putchar(c);
                    }
                }
                putchar('\n');
                saw = 1;
                break;
            }
            if (got == 0) {
                printf("session reached end of output with nothing said\n");
                break;
            }
        } else if (n < 0) {
            printf("select failed: %s\n", strerror(errno));
            break;
        }
        waited += 200;
    }

    /* Did anything die? A shell that was forked and then failed claims its
     * terminal, execs, and exits on the spot - and an exited session's terminal
     * says end-of-output rather than anything, which is what the loop above
     * cannot tell from a shell that is merely quiet. waitpid(WNOHANG) names it. */
    printf("--- the shell idle forked ---\n");
    {
        int status = 0;
        pid_t gone = waitpid(-1, &status, WNOHANG);
        if (gone > 0) {
            if (WIFEXITED(status)) {
                printf("  pid %d exited with %d\n", (int)gone, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("  pid %d died on signal %d\n", (int)gone, WTERMSIG(status));
            } else {
                printf("  pid %d is %s\n", (int)gone,
                       WIFSTOPPED(status) ? "stopped" : "in some other state");
            }
            printf("  (126 = could not claim the terminal, 127 = no shell)\n");
        } else if (gone == 0) {
            printf("  nothing has exited yet\n");
        } else {
            printf("  nothing to reap: %s\n", strerror(errno));
        }
    }

    /* idle's own view: did it see the request, and did it fork? Its stderr went
     * to its terminal, which is this process's copy of the control slave - read
     * from the control master, which is where that output comes back out. */
    printf("--- what idle itself said ---\n");
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ctl_master, &r);
        struct timeval tv = { 0, 300000 };
        if (select(ctl_master + 1, &r, NULL, NULL, &tv) > 0) {
            char buf[512];
            ssize_t got = read(ctl_master, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                printf("%s", buf);
            }
        } else {
            printf("(nothing)\n");
        }
    }

    /* And the machine's process table, which is the thing this whole arrangement
     * is for: a session is only worth having if ps can see it. */
    printf("--- the machine's processes ---\n");
    {
        pid_t ps = fork();
        if (ps == 0) {
            execl("/bin/busybox", "busybox", "ps", (char*)NULL);
            _exit(127);
        }
        int status = 0;
        waitpid(ps, &status, 0);
    }

    printf("=== %s ===\n", saw ? "the session spoke" : "the session said nothing");
    kill(idle_pid, SIGTERM);
    return saw ? 0 : 1;
}
