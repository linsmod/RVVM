/*
 * idle.c - a run root that stays up and spawns the sessions on it
 *
 * The Android host's console is a byte pipe onto one machine, and a machine
 * comes from a run: `run->machine = rvvm_user_create()` per run, and the run is
 * torn down by on_guest_exit the moment its root process returns. That is right
 * for a command - `vp exec-out "ls"` should boot, answer and go - and it is
 * exactly wrong for a core, where the machine has to outlive any single client.
 * Every client that arrived with its own `am start` got its own machine, so two
 * of them could not see each other's processes: separate machines are separate
 * /proc, and `ps` in one names only its own.
 *
 * So this is the run root for a core. It holds the machine open, and it is also
 * what spawns the sessions that run inside it - one per client, each on its own
 * pty out of the machine's pool, so their line disciplines, echo, window sizes
 * and ^C handling are their own while `ps` still names all of them.
 *
 * The host does the forking on purpose. It has a pty pool the guest cannot see
 * from outside, and reaching it needs a call that only a host thread can make.
 * So the arrangement is: this process is told which pty is the control channel,
 * and the host writes one short request per session into it. That is the whole
 * protocol, and it is deliberately not a socket - a socket here would be a second
 * protocol between two ends that already have one.
 *
 *   idle --control <path> [--shell <path>] [--rows N] [--cols N]
 *
 *   read a line from the control terminal:
 *     "spawn <path> [cols] [rows]"   fork a session on that terminal
 *     "bye"                           exit, and take the machine down
 *
 * The control terminal is named by path (/dev/pts/N), not by number: the host
 * makes a real pty and both sides open the same one, rather than the host
 * handing this process a descriptor it would then have to keep in step with.
 *
 * A session is the ordinary sequence a terminal login does, because that is
 * what makes /dev/tty inside it answer *its* pty rather than the run's console
 * (a shell that cannot reach its own terminal turns job control off and prints
 * "can't access tty"):
 *
 *   setsid()                 a session of its own, led by itself
 *   ioctl(slave, TIOCSCTTY)   whose terminal is this pty
 *   dup2(slave, 0/1/2)       the session's standard descriptors
 *   tcsetpgrp(0, getpid())    the foreground group, so ^C and ^Z written to the
 *                             master signal the job and not this daemon
 *   TIOCSWINSZ on the master  the size the client is looking at
 *   execl(shell)              and finally the shell itself
 *
 * What this process must not do is the rest of what a shell would do by reflex:
 *
 *   - read or write the run's console. It belongs to the host, and the first
 *     client to connect is the thing that gets it. A root that read stdin would
 *     swallow the bytes a session is about to be sent; a root that printed
 *     would put its own words ahead of that client's shell, and READY rides
 *     behind the guest's first output, so a chatty root delays the handshake
 *     for everyone.
 *   - exit while a session is alive. on_guest_exit destroys the run and the
 *     machine with it, and every open session goes at once. There is no
 *     supervisor to restart it: this *is* the thing a core is made of, and a
 *     restarted core is a new machine, which is the state this exists to avoid.
 *
 * So the loop is a read on the control pty and nothing else. Sessions are
 * reaped as they end, and a session that dies takes nothing else with it - that
 * is what SIGCHLD here is for, and it is the only reason this installs a
 * handler at all.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_SHELL "/bin/sh"
#define DEFAULT_ROWS  24
#define DEFAULT_COLS  80

static const char* shell_path  = DEFAULT_SHELL;
static int         rows       = DEFAULT_ROWS;
static int         cols       = DEFAULT_COLS;

static volatile sig_atomic_t got_child = 0;

static void on_sigchld(int sig)
{
    (void)sig;
    got_child = 1;
}

/* Raw mode on the control channel.
 *
 * The line discipline would otherwise assemble a line before this process ever
 * sees it, and echo every byte back - onto the pty the host reads as "what the
 * guest said". ICANON and ECHO off, and the output side untouched so replies
 * still go out unchanged. */
static void make_raw(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        return;
    }
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

/* Spawn one session on the terminal the host named.
 *
 * The fork is what puts a second shell in a machine that already has one; there
 * is no other way in from the host, and no reason to want another - the child is
 * an ordinary process as far as the machine is concerned, which is exactly why
 * `ps` in any session names it. */
static void spawn_session(const char* path, int c_cols, int c_rows)
{
    if (!path || !*path) {
        return;
    }
    /* No O_NOCTTY on the slave: claiming the terminal is the point, and it has to
     * happen through the ioctl below where the errors are visible. */
    int slave = open(path, O_RDWR);
    if (slave < 0) {
        return;
    }

    /* The size before the fork, so it is already right when the shell asks. */
    if (c_cols > 0 && c_rows > 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)c_cols;
        ws.ws_row = (unsigned short)c_rows;
        ioctl(slave, TIOCSWINSZ, &ws);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(slave);
        return;
    }
    if (pid == 0) {
        close(0);
        close(1);
        close(2);
        /* Detach from the control channel: a session that inherited it would
         * compete with this process for the host's requests, and two readers on
         * one terminal each get half of them. */
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            _exit(126);
        }
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        /* The session is the terminal's foreground group: ^C and ^Z written to
         * the master reach the shell and whatever it runs, not the daemon that
         * relayed the byte. Without this a ^C would be delivered to a group
         * nobody is in, which is how a session's Ctrl-C ends up killing the
         * wrong thing. */
        tcsetpgrp(0, getpid());
        execl(shell_path, shell_path, (char*)NULL);
        _exit(127);          /* no shell: nothing to serve */
    }
    close(slave);
    /* The master end is the host's, deliberately left closed here. Holding a
     * second reader on it would take bytes the host is waiting for, and the host
     * is what makes this session's output reachable from outside the machine. */
}

/* One line from the control pty, without the newline. Returns NULL at end of
 * input, which is the host closing the channel and means the core is done. */
static char* read_line(int fd, char* buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return NULL;
        }
        if (r == 0) {
            return NULL;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n ? buf : NULL;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: idle --control <path> [--shell <path>] [--rows N] [--cols N]\n"
            "  --control PATH  the terminal the host writes session requests into\n"
            "  --shell PATH    the shell each session runs (default %s)\n"
            "  --rows N        the session window size (default %d)\n"
            "  --cols N      (default %d)\n",
            DEFAULT_SHELL, DEFAULT_ROWS, DEFAULT_COLS);
}

int main(int argc, char** argv)
{
    const char* control = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--control") && i + 1 < argc) {
            control = argv[++i];
        } else if (!strcmp(argv[i], "--shell") && i + 1 < argc) {
            shell_path = argv[++i];
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            rows = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
            cols = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    /* SIGPIPE would kill this process if the host went away mid-write, and a
     * core that dies when its client disconnects is the thing this whole
     * arrangement exists to prevent: the machine would go with it, taking every
     * other session's shell down. SIGCHLD is what makes a finished session
     * observable; nothing here signals, so nothing needs a disposition. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, on_sigchld);

    if (!control) {
        /* No control channel: hold the machine open and do nothing else, which
         * is still a useful thing for a host to boot (a machine with no sessions
         * in it, to be driven by a client that brings its own). */
        for (;;) {
            pause();
        }
    }

    int ctl = open(control, O_RDWR);
    if (ctl < 0) {
        /* The host named a terminal this machine does not have. Exiting here
         * would take the run down, and a run that cannot be spoken to is
         * indistinguishable from no run at all - so the machine is held and the
         * fault is on stderr, which the host's log collects. */
        fprintf(stderr, "idle: cannot open control terminal %s: %s\n", control,
                strerror(errno));
        for (;;) {
            pause();
        }
    }
    make_raw(ctl);

    char line[256];
    for (;;) {
        if (got_child) {
            got_child = 0;
            /* Reap every finished session. One waitpid() would leave the rest
             * as zombies, and a zombie still holds a slot in the machine's
             * process table - the table the sessions read with `ps`. */
            while (waitpid(-1, NULL, WNOHANG) > 0) {
                /* keep going */
            }
        }
        if (!read_line(ctl, line, sizeof(line))) {
            /* The host closed the channel. Hold the machine anyway: the sessions
             * already started are still somebody's, and exiting would take them
             * with this process. */
            for (;;) {
                pause();
            }
        }
        if (!strncmp(line, "spawn ", 6)) {
            char path[128];
            int c = cols, r = rows;
            /* "spawn <path> [cols] [rows]" - the size rides along so the shell is
             * laid out for the client from its first prompt, rather than after a
             * resize it may never get. */
            if (sscanf(line + 6, "%127s %d %d", path, &c, &r) >= 1) {
                spawn_session(path, c, r);
            }
        } else if (!strncmp(line, "bye", 3)) {
            break;
        }
        /* Anything else is not a request this version knows. Ignoring it is
         * deliberate: a host speaking a newer dialect should still be able to
         * hold a core, and the alternative - exiting - throws away every
         * session in the machine over a line. */
    }
    return 0;
}
