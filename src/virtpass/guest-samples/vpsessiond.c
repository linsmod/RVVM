/*
 * vpsessiond.c - a session server: one ssh-like session per connection.
 *
 * accept() a connection, make a pty pair, fork a session leader on the slave
 * end, exec the shell there, and relay bytes between the pty master and the
 * socket until either end goes away. The daemon itself never exits: it is what
 * makes a machine a *core* several clients attach to, rather than a one-shot
 * shell - the guest-side twin of an instance that holds its state while its
 * clients come and go.
 *
 * Each session gets a real terminal, not a pipe a shell only pretends about:
 *
 *   setsid() + TIOCSCTTY        -> /dev/tty inside the session answers *that*
 *                                  pty (the core records the owner; the run's
 *                                  console belongs to somebody else)
 *   tcsetpgrp(0, getpid())      -> the session is the terminal's foreground
 *                                  group, so ^C and ^Z written to the master
 *                                  signal the job, not the server that relayed
 *                                  the byte
 *   ISIG in the line discipline -> ^C / ^Z reach the foreground *group*
 *   TIOCSWINSZ on the master    -> SIGWINCH in the foreground group
 *
 * Protocol, client -> server. Frames are consumed, never forwarded, and a
 * client that sends none of them (netcat, a human typing) gets an interactive
 * shell at 24x80: the server waits GRACE_MS for a first frame and then starts
 * one anyway, so plain `nc 127.0.0.1:7900` works.
 *
 *   ESC ] 999 ; R<rows>;<cols> BEL   the session's window size
 *   ESC ] 999 ; C<command>     BEL   run `$SHELL -c <command>` instead of an
 *                                    interactive shell (before it starts)
 *
 * A frame has to arrive in one piece: a partial one is not buffered, it is
 * forwarded as data. Clients (the rvvm_ash relay, the test harness) write a
 * frame in a single call, which is what this expects.
 *
 * Usage: vpsessiond [port]     (default 7900, loopback only)
 *        SHELL=/bin/sh          (the shell each session runs)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SESSION_MAX     16
#define DEFAULT_PORT  7900
#define PORT_MAX     65535

/* How long a fresh connection may hold its shell back while a first frame is in
 * flight. Long enough for a client's preamble on loopback, short enough that an
 * interactive client sees its prompt immediately. */
#define GRACE_MS       120

/* How often the relay comes back to look at what the pty has produced. The
 * emulator's poll() re-checks an emulated descriptor (the pty master) on this
 * slice only - a cheap price for not needing a thread per direction. */
#define POLL_MS         20

/* Bytes held per session in each direction. They are not buffers of
 * convenience: the client may type while the shell is busy (the pty's input ring
 * is full), or the shell may write while the client is not reading yet, and
 * neither may lose data. A direction whose buffer is full stops being read
 * instead - backpressure, not a drop. */
#define IN_MAX        8192    // client -> pty, waiting for ring room
#define OUT_MAX      32768    // pty -> client, waiting for the socket

#define FRAME_HEAD     "\x1b]999;"
#define FRAME_HEAD_LEN 6
#define FRAME_END      0x07   // BEL closes a frame

#define CMD_MAX        512

struct session {
    bool    used;
    int     sock;             // the client
    int     master;           // its pty's master end, -1 until it is spawned
    pid_t   pid;              // the session leader running the shell
    bool    started;          // the shell has been spawned
    long    grace_at;         // When to spawn anyway (loop_ms()-relative)
    bool    child_done;       // The shell exited: drain, then close
    /* What a preamble asked for, held until the shell starts. */
    unsigned rows, cols;
    bool    has_cmd;
    char    cmd[CMD_MAX];
    uint8_t in[IN_MAX];       // Waiting for the pty's input ring
    size_t  in_len;
    uint8_t out[OUT_MAX];     // Waiting for the socket
    size_t  out_len;
};

static struct session sessions[SESSION_MAX];
static const char*    shell_path = "/bin/sh";
static int            listener   = -1;

static long loop_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Every line this daemon prints carries the clock, and the same epoch the core's
 * trace and the harness's transcript use: the three land in one file, and a
 * session's trouble is usually a race between them (a write the relay had not
 * drained, a signal that arrived a millisecond late). */
static void dlog(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%9ld ms] vpsessiond: ", loop_ms());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static int session_count(void)
{
    int n = 0;
    for (int i = 0; i < SESSION_MAX; ++i) {
        n += sessions[i].used ? 1 : 0;
    }
    return n;
}

static void session_close(struct session* s, const char* why);

/* ---------------------------------------------------------------
 * Frames
 * --------------------------------------------------------------- */

/* Act on a frame at @buf if the whole of one is there.
 *
 * Returns the bytes it took (the caller forwards the rest), 0 when @buf is not
 * a frame at all, or -1 when one is only starting. */
static ssize_t frame_apply(struct session* s, const uint8_t* buf, size_t len)
{
    if (len < FRAME_HEAD_LEN || memcmp(buf, FRAME_HEAD, FRAME_HEAD_LEN)) {
        return 0;
    }
    size_t end = 0;
    for (size_t i = FRAME_HEAD_LEN; i < len; ++i) {
        if (buf[i] == FRAME_END) {
            end = i;
            break;
        }
    }
    if (!end) {
        return -1;
    }
    const char* body = (const char*)buf + FRAME_HEAD_LEN;
    size_t      blen = end - FRAME_HEAD_LEN;

    if (blen >= 1 && body[0] == 'R') {
        /* The window size: what the client's terminal is, so the session's jobs
         * lay themselves out for it - and what SIGWINCH announces when it
         * changes while they run. */
        unsigned rows = 0, cols = 0;
        if (sscanf(body + 1, "%u;%u", &rows, &cols) == 2 && rows && cols) {
            s->rows = rows;
            s->cols = cols;
            if (s->master >= 0) {
                struct winsize ws;
                memset(&ws, 0, sizeof(ws));
                ws.ws_row = (unsigned short)rows;
                ws.ws_col = (unsigned short)cols;
                ioctl(s->master, TIOCSWINSZ, &ws);
            }
        }
    } else if (blen >= 1 && body[0] == 'C' && !s->started) {
        /* A command to run instead of an interactive shell. Only before the
         * spawn: afterwards there is a session already, and substituting what it
         * runs would throw away whatever it is doing. */
        size_t n = blen - 1 < CMD_MAX - 1 ? blen - 1 : CMD_MAX - 1;
        memcpy(s->cmd, body + 1, n);
        s->cmd[n]  = 0;
        s->has_cmd = n != 0;
    }
    return (ssize_t)(end + 1);
}

/* ---------------------------------------------------------------
 * The pty and the session's shell
 * --------------------------------------------------------------- */

static void session_close(struct session* s, const char* why)
{
    if (!s->used) {
        return;
    }
    int idx = (int)(s - sessions);
    dlog("  [slot %d] close why=%s sock=%d pid=%d child_done=%d",
         idx, why, s->sock, (int)s->pid, (int)s->child_done);
    fflush(stdout);
    if (s->pid > 0) {
        /* Hang the session up: its terminal is gone, and a shell reading from it
         * is meant to notice (Linux sends SIGHUP to the foreground group when a
         * pty master closes). */
        kill(-s->pid, SIGHUP);
        s->pid = 0;
    }
    if (s->master >= 0) {
        close(s->master);
        s->master = -1;
    }
    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }
    s->used = false;
    dlog("session closed (%s), %d of %d in use", why, session_count(), SESSION_MAX);
    fflush(stdout);
}

/* Start the shell for @s on a pty of its own. */
static bool session_spawn(struct session* s)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return false;
    }
    /* The master stays with the daemon: the session works on the slave end, and
     * a shell holding the master would keep the pair's read side alive. */
    fcntl(master, F_SETFD, FD_CLOEXEC);
    if (grantpt(master) || unlockpt(master)) {
        close(master);
        return false;
    }
    char* name = ptsname(master);
    if (!name) {
        close(master);
        return false;
    }
    char slave_path[64];
    snprintf(slave_path, sizeof(slave_path), "%s", name);
    int slave = open(slave_path, O_RDWR);
    if (slave < 0) {
        close(master);
        return false;
    }

    /* The size goes in before the shell can look at it, so its first prompt and
     * any program it starts already lay out for the client's terminal. */
    if (s->rows && s->cols) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)s->rows;
        ws.ws_col = (unsigned short)s->cols;
        ioctl(master, TIOCSWINSZ, &ws);
    }

    /* The master is what the relay reads and writes, and it must not park the
     * daemon's single thread when a ring is full: it is non-blocking, and the
     * two buffered directions handle EAGAIN. */
    fcntl(master, F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        close(slave);
        close(master);
        return false;
    }
    if (pid == 0) {
        /* The session's own address space from here on. The descriptors it
         * inherited are not the session's: the client's socket belongs to the
         * daemon, and a shell holding it would keep a gone client's connection
         * alive. (The emulator drops a process's whole descriptor table at
         * execve() anyway; this is what makes the same program correct on a real
         * kernel, where execve() keeps every descriptor that is not CLOEXEC.) */
        if (s->sock >= 0) {
            close(s->sock);
        }
        if (listener >= 0) {
            close(listener);
        }
        setsid();                    // A session of its own: no terminal yet
        ioctl(slave, TIOCSCTTY, 0);  // ...this pty is ours (/dev/tty answers it)
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        /* The session is the terminal's foreground group: ^C and ^Z written to
         * the master signal the shell (and whatever it runs), not the daemon
         * that relayed the byte. */
        tcsetpgrp(0, getpid());
        if (s->has_cmd) {
            execl(shell_path, shell_path, "-c", s->cmd, (char*)NULL);
        } else {
            execl(shell_path, shell_path, (char*)NULL);
        }
        _exit(127);                  // exec failed: nothing to serve
    }

    /* Only the session holds the slave end now: that is what makes a hangup real
     * - the master reads EIO once the shell has closed its side. */
    close(slave);
    s->master  = master;
    s->pid     = pid;
    s->started = true;
    dlog("session %d started (pid %d, %s), %d of %d in use", s->sock, (int)pid,
         s->has_cmd ? "command" : "shell", session_count(), SESSION_MAX);
    fflush(stdout);
    return true;
}

/* ---------------------------------------------------------------
 * The three data paths
 * --------------------------------------------------------------- */

/* Client -> pty, as much as the ring takes. The rest stays in @in. */
static void session_pump_in(struct session* s)
{
    while (s->in_len && s->master >= 0) {
        ssize_t n = write(s->master, s->in, s->in_len);
        if (n > 0) {
            memmove(s->in, s->in + n, s->in_len - (size_t)n);
            s->in_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;   // The shell is not reading yet: the rest waits here
        }
        session_close(s, "hangup on input");
        return;
    }
}

/* pty -> client, as much as the socket takes. The rest stays in @out. */
static void session_pump_out(struct session* s)
{
    while (s->out_len && s->sock >= 0) {
        ssize_t n = write(s->sock, s->out, s->out_len);
        if (n > 0) {
            memmove(s->out, s->out + n, s->out_len - (size_t)n);
            s->out_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;   // The client is not reading yet
        }
        session_close(s, "client write failed");
        return;
    }
}

/* What the master has produced, up to the room @out has left. */
static void session_drain_pty(struct session* s)
{
    if (s->master < 0) {
        return;
    }
    while (s->out_len < sizeof(s->out)) {
        ssize_t n = read(s->master, s->out + s->out_len, sizeof(s->out) - s->out_len);
        if (n > 0) {
            s->out_len += (size_t)n;
            continue;
        }
        if (n == 0) {
            /* VEOF on an empty line, or the slave end is gone: the session is
             * over, and whatever it wrote is already in @out. */
            s->child_done = true;
            return;
        }
        if (errno == EAGAIN || errno == EINTR) {
            return;
        }
        /* EIO is what a master reports once its slave is closed. */
        s->child_done = true;
        return;
    }
    /* The buffer is full: the pty is not read again until the client catches up,
     * which is also what blocks the shell on its next write - the same thing a
     * terminal does when nobody drains it. */
}

/* Classify what the client sent, then hand the data to the pty. */
static void session_input(struct session* s, uint8_t* buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t used = frame_apply(s, buf + off, len - off);
        if (used > 0) {
            off += (size_t)used;
            continue;
        }
        /* A frame that is starting but not all here is passed on as data: it
         * would need a hold deadline of its own to keep a lone ESC from stalling
         * the stream, and a client writing a frame in one call - this protocol's
         * contract - never splits one. */
        break;
    }
    if (off < len) {
        size_t room = sizeof(s->in) - s->in_len;
        size_t take = len - off > room ? room : len - off;
        memcpy(s->in + s->in_len, buf + off, take);
        s->in_len += take;
        /* Typed bytes end the wait for a preamble: the client is talking to a
         * shell, so it is time to start one. */
        s->grace_at = 0;
    }
    session_pump_in(s);
}

/* Take what the client has sent, every round rather than when poll() names the
 * socket.
 *
 * The relay is a single thread, so it cannot afford to miss a wakeup. An
 * emulated poll() over a *mixed* set - a host socket next to the pty's own
 * descriptors - was observed here reporting a session socket readable once and
 * then never again while the client's bytes were still queued: that session
 * echoed its input (the line discipline does that in the core) and never ran it.
 * One non-blocking read per session per POLL_MS cannot miss anything, and
 * EAGAIN is the normal answer. */
static void session_read_sock(struct session* s)
{
    if (s->sock < 0) {
        return;
    }
    for (;;) {
        size_t room = sizeof(s->in) - s->in_len;
        if (!room) {
            return;   // Backpressure: the pty has not taken the last batch yet
        }
        uint8_t buf[1024];
        size_t  want = room < sizeof(buf) ? room : sizeof(buf);
        ssize_t n    = read(s->sock, buf, want);
        if (n > 0) {
            session_input(s, buf, (size_t)n);
            if (!s->used) {
                return;
            }
            continue;
        }
        if (n == 0) {
            int idx = (int)(s - sessions);
            dlog("  [slot %d] read EOF on sock=%d", idx, s->sock);
            fflush(stdout);
            session_close(s, "client closed");
            return;
        }
        if (errno != EAGAIN && errno != EINTR) {
            session_close(s, "client read failed");
            return;
        }
        return;
    }
}

static bool session_open(int sock, long now)
{
    for (int i = 0; i < SESSION_MAX; ++i) {
        struct session* s = &sessions[i];
        if (s->used) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used     = true;
        s->sock     = sock;
        s->master   = -1;
        s->grace_at = now + GRACE_MS;
        fcntl(sock, F_SETFL, O_NONBLOCK);
        /* See the listener: a session forked later must not inherit this. */
        fcntl(sock, F_SETFD, FD_CLOEXEC);
        dlog("  [slot %d] open sock=%d", i, sock);
        fflush(stdout);
        return true;
    }
    return false;
}

/* Reap a shell that has exited, so its session can drain and close. */
static void session_check_child(struct session* s)
{
    if (s->pid <= 0) {
        return;
    }
    int   status = 0;
    pid_t pid    = waitpid(s->pid, &status, WNOHANG);
    if (pid == s->pid) {
        s->pid        = 0;
        s->child_done = true;
    }
}

int main(int argc, char** argv)
{
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > PORT_MAX) {
            port = DEFAULT_PORT;
        }
    }
    const char* env_shell = getenv("SHELL");
    if (env_shell && *env_shell) {
        shell_path = env_shell;
    }

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        dlog("socket(): %s", strerror(errno));
        return 1;
    }
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    /* Every descriptor this daemon owns is closed at execve() of the session it
     * forks: a shell that kept a client's socket (or another session's) would
     * hold its refcount up forever, and the client waiting for the FIN of a
     * session that ended would wait for a close that never comes. */
    fcntl(listener, F_SETFD, FD_CLOEXEC);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listener, SESSION_MAX) < 0) {
        dlog("bind/listen(%d): %s", port, strerror(errno));
        return 1;
    }

    dlog("listening on 127.0.0.1:%d, shell %s (in %d, out %d)", port, shell_path, IN_MAX, OUT_MAX);
    fflush(stdout);

    for (;;) {
        /* poll() is asked about the listener (a host socket) and about each
         * session's pty - an emulated descriptor the emulator answers itself.
         * The session *sockets* are not in the set: they are read every round
         * instead (see session_read_sock). */
        struct pollfd pfd[1 + SESSION_MAX];
        int           kind[1 + SESSION_MAX];   // session index, -1 for the listener
        int           nfds = 0;

        pfd[nfds].fd      = listener;
        pfd[nfds].events  = POLLIN;
        pfd[nfds].revents = 0;
        kind[nfds++]      = -1;

        for (int i = 0; i < SESSION_MAX; ++i) {
            struct session* s = &sessions[i];
            if (s->used && s->master >= 0 && s->out_len < sizeof(s->out)) {
                pfd[nfds].fd      = s->master;
                pfd[nfds].events  = POLLIN;
                pfd[nfds].revents = 0;
                kind[nfds++]      = i;
            }
        }

        int  ready = poll(pfd, (nfds_t)nfds, POLL_MS);
        long now   = loop_ms();

        if (ready > 0) {
            for (int i = 0; i < nfds; ++i) {
                if (!pfd[i].revents) {
                    continue;
                }
                if (kind[i] < 0) {
                    /* A new client: its shell waits for a first frame or for the
                     * grace to run out, whichever comes first (see the tick). */
                    struct sockaddr_in peer;
                    socklen_t          plen = sizeof(peer);
                    int                c    = accept(listener, (struct sockaddr*)&peer, &plen);
                    if (c < 0) {
                        continue;
                    }
                    if (session_open(c, now)) {
                        dlog("client connected, %d of %d in use", session_count(), SESSION_MAX);
                    } else {
                        dlog("%d sessions already, refusing a client", SESSION_MAX);
                        close(c);
                    }
                    continue;
                }
                struct session* s = &sessions[kind[i]];
                if (s->used) {
                    session_drain_pty(s);
                }
            }
        }

        /* Per round: take what each client sent, start what is due, feed both
         * directions, notice a shell that exited, and close what has nothing
         * left to say. */
        for (int i = 0; i < SESSION_MAX; ++i) {
            struct session* s = &sessions[i];
            if (!s->used) {
                continue;
            }
            session_read_sock(s);
            if (!s->used) {
                continue;
            }
            if (!s->started && now >= s->grace_at) {
                if (!session_spawn(s)) {
                    session_close(s, "no terminal available");
                    continue;
                }
            }
            session_check_child(s);
            session_pump_in(s);
            if (!s->used) {
                continue;
            }
            /* Drain even after the child is gone. A one-shot command
             * (`sh -c 'echo hi'`, `ash -c`) writes its output and exits in the
             * same instant, so the round that reaps it must still read the
             * master - the drain stops on EOF/EIO, which is also what marks the
             * session finished, and the last output is the whole reason the
             * session exists. */
            session_drain_pty(s);
            session_pump_out(s);
            if (!s->used) {
                continue;
            }
            if (s->child_done && !s->out_len) {
                session_close(s, "shell exited");
            }
        }
    }
}
