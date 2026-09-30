/*
 * probe_pty.c - does a shell on a simulated pty answer what is typed at it?
 *
 * A diagnostic, not a test with a verdict. idle (guest-samples/idle.c) forks a
 * session shell the way vpsessiond does, line for line, and both are believed to
 * be right - yet a session's shell sat there answering nothing. So the shell and
 * the pty were taken out of idle's hands and put in this process's, where every
 * step can be looked at:
 *
 *   1. a pair from /dev/ptmx, which on this host is the machine's simulated one
 *   2. whether the pair carries anything at all - a byte written to the master
 *      and read straight back says the two ends are joined, and distinguishes
 *      "the shell is quiet" from "the terminal is dead" once and for all
 *   3. the login sequence, done here: setsid, TIOCSCTTY, dup2, tcsetpgrp, exec
 *   4. what the shell's own descriptors turned out to be, read out of /proc
 *   5. `ps -ef` typed into it, and the reply
 *
 * Step 4 is the one worth reading even if step 5 works: a shell that is alive but
 * holding something other than this pty explains every symptom at once.
 *
 * Usage: probe_pty [seconds]      (default 5)
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

/* readlink("/proc/<pid>/fd/<n>") - where a descriptor of somebody else points. */
static void where(int pid, int fd, const char* label)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, fd);
    char buf[256];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("  %s: cannot read (%s)\n", label, strerror(errno));
        return;
    }
    buf[n] = '\0';
    printf("  %s -> %s\n", label, buf);
}

/* Drain the master, printing whatever comes, until it goes quiet. */
static void drain(int master, int max_ms, const char* label)
{
    long waited = 0;
    int  any = 0;
    while (waited < max_ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(master, &r);
        struct timeval tv = { 0, 200000 };
        int n = select(master + 1, &r, NULL, NULL, &tv);
        if (n > 0) {
            char buf[512];
            ssize_t got = read(master, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                if (!any) {
                    printf("%s:\n", label);
                    any = 1;
                }
                printf("  <");
                for (ssize_t i = 0; i < got; i++) {
                    unsigned char c = (unsigned char)buf[i];
                    if (c == 0x1b)        printf("<ESC>");
                    else if (c == '\r')    printf("\\r");
                    else if (c == '\n')    printf("\\n");
                    else if (c < 0x20 || c == 0x7f) printf("<%02x>", c);
                    else putchar(c);
                }
                printf(">\n");
                waited += 200;
                continue;
            }
            if (got == 0) {
                printf("%s: end of output (nothing holds the slave)\n", label);
                return;
            }
        } else if (n < 0) {
            printf("%s: select failed: %s\n", label, strerror(errno));
            return;
        }
        waited += 200;
    }
    if (!any) {
        printf("%s: said nothing in %dms\n", label, max_ms);
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    long seconds = (argc > 1) ? strtol(argv[1], NULL, 10) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }
    printf("=== pty probe ===\n");

    /* 1. The pair. */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        printf("posix_openpt failed: %s\n", strerror(errno));
        return 1;
    }
    if (grantpt(master) || unlockpt(master)) {
        printf("grantpt/unlockpt failed: %s\n", strerror(errno));
        return 1;
    }
    const char* name = ptsname(master);
    printf("terminal: %s (master is descriptor %d)\n", name ? name : "?", master);
    if (!name) {
        return 1;
    }
    {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = 24;
        ws.ws_col = 80;
        ioctl(master, TIOCSWINSZ, &ws);
    }

    /* 2. The pair on its own, in both directions, with the slave held open here.
     * A shell is deliberately not involved yet: this says whether the terminal
     * carries anything at all, and which way, so that a silent shell afterwards
     * is unmistakably the shell's doing.
     *
     *   master -> slave   what a host types
     *   slave  -> master   what the guest answers
     */
    printf("the pair on its own, both directions:\n");
    int slave_here = open(name, O_RDWR);
    if (slave_here < 0) {
        printf("  open(%s) failed: %s\n", name, strerror(errno));
        return 1;
    }
    printf("  slave opened as descriptor %d\n", slave_here);
    {
        /* master -> slave */
        ssize_t w = write(master, "typed-by-the-host\n", 18);
        printf("  master -> slave: wrote %zd byte(s)\n", w);
        fd_set r;
        FD_ZERO(&r);
        FD_SET(slave_here, &r);
        struct timeval tv = { 0, 500000 };
        if (select(slave_here + 1, &r, NULL, NULL, &tv) > 0) {
            char buf[128];
            ssize_t got = read(slave_here, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                printf("  slave read back: <%s>\n", buf);
            } else {
                printf("  slave read: end of output\n");
            }
        } else {
            printf("  slave read: NOTHING came back\n");
        }
    }
    {
        /* slave -> master */
        ssize_t w = write(slave_here, "answered-by-the-guest\n", 21);
        printf("  slave -> master: wrote %zd byte(s)\n", w);
        fd_set r;
        FD_ZERO(&r);
        FD_SET(master, &r);
        struct timeval tv = { 0, 500000 };
        if (select(master + 1, &r, NULL, NULL, &tv) > 0) {
            char buf[128];
            ssize_t got = read(master, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                printf("  master read back: <%s>\n", buf);
            } else {
                printf("  master read: end of output\n");
            }
        } else {
            printf("  master read: NOTHING came back\n");
        }
    }
    close(slave_here);

    /* 3. The login sequence, here rather than in idle, so the shell is a child
     * whose pid is known and every step's failure is visible. */
    int slave = open(name, O_RDWR);
    if (slave < 0) {
        printf("open(%s) failed: %s\n", name, strerror(errno));
        return 1;
    }
    printf("slave opened as descriptor %d\n", slave);

    pid_t pid = fork();
    if (pid < 0) {
        printf("fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            printf("  TIOCSCTTY failed: %s\n", strerror(errno));
            _exit(126);
        }
        printf("  TIOCSCTTY ok\n");
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        if (tcsetpgrp(0, getpid()) < 0) {
            printf("  tcsetpgrp failed: %s\n", strerror(errno));
        } else {
            printf("  tcsetpgrp ok\n");
        }
        execl("/bin/sh", "/bin/sh", (char*)NULL);
        _exit(127);
    }
    close(slave);
    msleep(500);
    printf("shell forked as pid %d\n", (int)pid);

    /* 4. Where its descriptors ended up. Read from outside, because the shell
     * cannot report this itself and the answer is what every other symptom
     * depends on. */
    printf("the shell's descriptors:\n");
    where((int)pid, 0, "stdin ");
    where((int)pid, 1, "stdout");
    where((int)pid, 2, "stderr");

    /* Anything the shell said on the way up - a prompt, a complaint about the
     * terminal. That is the difference between a shell that is waiting for
     * someone and one that has already given up. */
    drain(master, 1000, "what the shell said on starting");

    /* 5. Type at it. `ps -ef` rather than something simpler because the reply
     * naming this very shell is what says the machine is shared. */
    printf("typing `ps -ef` into the terminal...\n");
    {
        const char* typed = "ps -ef\n";
        ssize_t w = write(master, typed, strlen(typed));
        printf("  wrote %zd byte(s): %s\n", w, w > 0 ? "ok" : strerror(errno));
    }
    drain(master, (int)(seconds * 1000), "what the shell said");

    /* And is it even still there? A shell that exited leaves nothing to answer,
     * which is not the same thing as a shell that is quiet. */
    {
        int status = 0;
        pid_t gone = waitpid(pid, &status, WNOHANG);
        if (gone == 0) {
            printf("the shell is still running\n");
        } else if (WIFEXITED(status)) {
            printf("the shell exited with %d (126 = no terminal, 127 = no shell)\n",
                   WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("the shell died on signal %d\n", WTERMSIG(status));
        }
    }

    printf("=== done ===\n");
    kill(pid, SIGTERM);
    return 0;
}
