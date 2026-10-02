/*
 * vp_console.c - the guest console as a byte pipe, framed like adb's shell
 *
 * The Android host's console exists so a run can be driven and read by
 * something that is not looking at a screen. There are two ways to build
 * that, and this is the second.
 *
 * The first was a request/response protocol: a client asked for the screen
 * and got a picture of it, polling a serial number to notice that it had
 * changed. That is a good interface for a *test* - one consistent snapshot
 * per read, no partial frame, nothing to interleave - and a bad interface
 * for everything else. A full-screen program redraws in place and collapses
 * to its final state. A person typing gets a round trip per keystroke and a
 * heuristic for when the guest has finished talking. And a program that
 * never stops redrawing has no "finished" to detect.
 *
 * So the wire format here is the one adb already uses for exactly this
 * problem: ShellProtocol (adb/shell_protocol.h). One connection, a small
 * header per packet, and one id per logical stream:
 *
 *      0  stdin             client -> guest
 *      1  stdout            guest   -> client
 *      2  stderr            guest   -> client
 *      3  exit              guest   -> client, carrying the status
 *      4  close stdin       client -> guest
 *      5  window size       client -> guest, ASCII winsize
 *
 * Two of those are not decoration. stdout and stderr are indistinguishable
 * by the time they reach the virtual TTY - it shows both on one screen - so
 * keeping them apart is the only way a reader can act on the difference, and
 * it survives all the way to whoever is writing the client. And `exit`
 * arriving in band is what a result *is*: a batch guest's answer is its
 * status, and asking for it over a side channel afterwards is asking for a
 * race.
 *
 * Everything blocking lives here, off the guest's thread. The guest's vCPU
 * thread must never wait for a client: it drops bytes into a ring and returns,
 * and a writer thread does the blocking write. On Android that is the whole
 * reason the listener is native rather than in Java - a blocking push has no
 * counterpart across JNI, where there is no way to park a thread on a native
 * event.
 *
 * This file is shared by both hosts (it lives in src/virtpass, beside vp_core.c
 * and vp_cmdpost.c), because the framing, the ring and the session plumbing are
 * the same on each. What a host supplies is its log sink - logcat under Android,
 * stderr elsewhere (see the LOGI/LOGW/LOGE block below) - and, before this can
 * run on win32, the socket flavour behind the listener.
 */

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "vp_console.h"
#include <core/rvvm_user.h>

/* The console logs where its host logs, with the same lines either way: the
 * server above is one implementation for both hosts, only the sink differs -
 * logcat on Android, stderr elsewhere (which is where a console host such as
 * rvvm_ash already writes). */
#if defined(ANDROID)
#include <android/log.h>
#define CONSOLE_LOG_TAG "RVVM-Console"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  CONSOLE_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  CONSOLE_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, CONSOLE_LOG_TAG, __VA_ARGS__)
#else
#include <stdarg.h>
static void vp_console_logf(const char* level, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "vp-console: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define LOGI(...) vp_console_logf("info",  __VA_ARGS__)
#define LOGW(...) vp_console_logf("warn",  __VA_ARGS__)
#define LOGE(...) vp_console_logf("error", __VA_ARGS__)
#endif

/* Loopback, never INADDR_ANY. This is a shell into every guest on the
 * device, and the whole point of adb forward is that it is reachable from
 * the machine holding the cable - not from the network the device is on. */
#define CONSOLE_BIND_ADDR "127.0.0.1"

/* How many clients a core serves at once.
 *
 * This was 1, and it was right while a console *was* one session: the byte pipe
 * is a single machine's single keyboard, so a second client was a second writer
 * racing the first into the same line discipline, both fanned the same output.
 * Two `vp exec-out`s on one phone did exactly that - the second attached to the
 * busybox the first had running and read its prompt and its `ps -ef`, while its
 * own output went to both.
 *
 * It is not 1 any more because a client no longer shares a keyboard. Each gets a
 * terminal of its own out of the machine (see vp_core.h), so two clients are two
 * terminals in one machine: separate line discipline, separate echo, separate ^C,
 * and - the point of sharing the machine - each one's `ps` names the other's
 * processes.
 *
 * The cap is a resource bound now rather than a correctness one, and it sits under
 * what vp_core will hand out so the refusal is "no sessions" rather than a
 * terminal that cannot be had. */
#define CONSOLE_MAX_CONN 8

/* Bytes a client may fall behind by before it is dropped rather than waited
 * for. A real terminal makes the writer block; here the writer is a guest
 * thread and a wedged client would take the emulator with it. Overflowing
 * is a client that stopped reading, and dropping it says so - which is more
 * useful than losing the run to a hang, and more useful than silently
 * truncating output that a test is about to assert on. */
#define CONSOLE_RING_BYTES (1024 * 1024)

/* Header: one id byte and a 4-byte little-endian length, as adb's
 * ShellProtocol has it. A mismatch in the sender's idea of the buffer size
 * is not a problem: the length is in the packet, so a reader just consumes
 * what it can and keeps going. */
#define CONSOLE_HDR_BYTES 5

struct console_conn {
    int             fd;
    /* guest -> client, a plain byte FIFO. Written by whichever vCPU thread
     * produced the bytes, drained by this connection's writer thread.
     * head/tail/len rather than a pair of indices that wrap independently:
     * the append position and the drain position are different cursors, and
     * conflating them makes the writer consume what was just written. */
    uint8_t*        ring;
    size_t          head;      /* next byte to write out */
    size_t          tail;      /* where the next byte goes */
    size_t          len;       /* bytes buffered */
    int             dropped;
    /* Replaced by a newer client, as opposed to hung up on. The difference
     * decides whether this connection's end sends the guest a ^D - see the
     * eviction in accept_loop and conn_destroy. */
    int             taken_over;
    int             finished;  /* the reader reached the end of the socket */
    /* The session's guest end hung up - its shell is gone. Nothing more will
     * arrive on this connection, so the writer ends the client's half of the
     * stream once the ring drains (see conn_writer). 0 for a plain console
     * connection, where a guest exit is announced in band instead. */
    int             guest_gone;
    pthread_mutex_t lock;
    pthread_cond_t  cond;      /* room in the ring */
    pthread_cond_t  filled;    /* ring has something in it */
    pthread_cond_t  done;      /* the reader is done with this connection */
    pthread_t       writer;
    pthread_t       reader;
    struct console_conn* next;
    /* Typed before there was a guest to type into. A client connects the
     * moment the listener is up, and the run it names may not have started
     * yet - so the first thing a scripted driver sends arrives while the
     * machine is still NULL. Dropping it there loses the whole script: the
     * guest then comes up to a pipe whose contents are already gone. Held
     * here instead and handed over at bind time. */
    uint8_t*        pending;
    size_t          pending_len;
    size_t          pending_cap;
    int             pending_eof;   /* close-stdin arrived before the guest */
    /* Whether this connection has been told it may type. Not on accept: the
     * guest the client named is started *because* it connected, so at that
     * moment there is nothing to type into. See vp_console_tap. */
    int             ready_sent;
    /* This client's session, or NULL when it has none - which is the case for
     * every client on a host that launched a single named guest, and the reason
     * the tap below still fans to connections that have no session.
     *
     * A session is a terminal (see vp_core.h), so its bytes come from that
     * terminal and never from the run's console: the two paths cannot see each
     * other, which is what makes two clients two terminals rather than one
     * keyboard with two typists. */
    struct vp_core_session* session;
    /* Whether this connection has already been offered one. A client that
     * arrives before the run has started is offered one when it does, and this
     * is what stops that happening twice - which would orphan the first
     * terminal, with a shell forked onto it and its output read by nobody. */
    int             session_tried;
    /* The client's terminal size, as its last WINDOWSIZE packet said. Held
     * because a session can be opened before any packet has been read - the run
     * is held until a client arrives, and the terminal is opened the moment the
     * machine exists - so a client that sends its size first would otherwise get
     * its shell laid out for the default and then resized, a visible jump on the
     * first prompt. */
    int             winsz_rows;
    int             winsz_cols;
};

#define CONSOLE_PENDING_MAX (256 * 1024)

static struct {
    int              listen_fd;
    pthread_t        accept_thread;
    int              running;
    pthread_mutex_t  lock;          /* g_conns */
    struct console_conn* conns;
    void           (*on_connect)(void*);
    void*             on_connect_ud;
} g_console = { -1, 0, 0, PTHREAD_MUTEX_INITIALIZER, NULL, NULL, NULL };

static rvvm_machine_t* g_console_machine = NULL;

/* The core this machine is serving sessions out of, and the only place sessions
 * come from. Created with the machine and gone with it - see vp_console_set_core.
 *
 * The session work itself is not here: it is vp_core.c, shared with the other
 * host, because it is the same code there and this is not the place for a second
 * copy of it. */
static vp_core_t* g_core = NULL;

/* Whether the next run to start is a core. See vp_console_arm_core. */
static int g_core_armed = 0;

void vp_console_arm_core(void)
{
    g_core_armed = 1;
}

int vp_console_core_armed(void)
{
    return g_core_armed;
}

/* Defined below: a connection's terminal, and the thread that moves its output.
 * Declared here because vp_console_set_core() - which runs from
 * vp_console_set_machine(), well above them - is what starts both. */
static void conn_open_session(struct console_conn* c);
static void* conn_session_pump(void* arg);
static void vp_console_open_pending_sessions(rvvm_machine_t* machine);

const char* vp_console_control_path(void)
{
    return g_core ? vp_core_control_path(g_core) : NULL;
}

void vp_console_set_core(rvvm_machine_t* machine)
{
    if (!machine) {
        g_core = NULL;
        return;
    }
    /* Only when the host armed one. A machine appears for every run, and most of
     * them are a command - `vp exec-out "ls"` boots a guest, answers and is gone.
     * Making a core of every one of those would hand each a terminal and a shell
     * nobody asked for, and - worse - the client's output would arrive by the
     * session path while the command that produced it is writing to the run's
     * console, which is the crossed-wires arrangement in a new place. A run with
     * no core keeps the plain console: one machine, one keyboard, this client. */
    if (!vp_console_core_armed()) {
        g_core = NULL;
        return;
    }
    /* Made here and not at accept, because a terminal belongs to a machine and a
     * core has no machine until its run starts. The run is held until a client is
     * connected, so a client on a core always arrives *before* this - which is
     * exactly why opening a session at accept always found nothing to open one
     * on, and fell back to the single keyboard this whole arrangement exists to
     * stop using. */
    g_core = vp_core_new(machine);
    if (!g_core) {
        LOGE("no core: %s", strerror(rvvm_session_pty_errno()));
        return;
    }
    LOGI("core up, control terminal is %s", vp_core_control_path(g_core));
    /* Now there is a machine to own terminals, every waiting client gets one. */
    vp_console_open_pending_sessions(machine);
}

static void conn_open_session(struct console_conn* c)
{
    int rows = 24, cols = 80;
    pthread_mutex_lock(&c->lock);
    if (c->winsz_rows > 0 && c->winsz_cols > 0) {
        rows = c->winsz_rows;
        cols = c->winsz_cols;
    }
    pthread_mutex_unlock(&c->lock);

    struct vp_core_session* s = vp_core_session_open(g_core, rows, cols);
    if (!s) {
        LOGE("no session for this client; it keeps the run's own console");
        return;
    }
    c->session = s;
    LOGI("client has a session on %s (%dx%d)", vp_core_session_path(s), cols, rows);

    /* The pump starts here rather than at accept, because the session does not
     * exist until now. */
    pthread_t pump;
    if (pthread_create(&pump, NULL, conn_session_pump, c) == 0) {
        pthread_detach(pump);
    } else {
        LOGE("cannot start the session pump: %s", strerror(errno));
    }
}

void vp_console_set_machine(rvvm_machine_t* machine)
{
    g_console_machine = machine;
    if (!machine) {
        return;
    }
    /* Hand every connection what it typed while there was nothing to type
     * into, in arrival order, and only now - the bytes have to land in the
     * guest's own termios, and the guest is the thing that has just appeared. */
    pthread_mutex_lock(&g_console.lock);
    for (struct console_conn* c = g_console.conns; c; c = c->next) {
        pthread_mutex_lock(&c->lock);
        uint8_t* buf = c->pending;
        size_t   len = c->pending_len;
        int      eof = c->pending_eof;
        c->pending     = NULL;
        c->pending_len = 0;
        c->pending_cap = 0;
        c->pending_eof = 0;
        pthread_mutex_unlock(&c->lock);
        if (len) {
            rvvm_user_tty_input(machine, buf, len);
            LOGI("flushed %zu byte(s) typed before the guest existed", len);
        }
        free(buf);
        if (eof) {
            rvvm_user_tty_input(machine, "\x04", 1);
        }
    }
    pthread_mutex_unlock(&g_console.lock);

    /* And the sessions: a machine is the one thing a session needs to exist, and
     * this is the only moment there is one. */
    vp_console_set_core(machine);
}

/* Give @c a terminal if a core is up and it has none yet. This is the one place
 * a connection becomes a session, and it runs at the two moments that can
 * happen: when the machine appears (a run held until a client arrived) and when
 * a client arrives at a core that is already up (a long-lived one serves
 * whoever comes). Caller holds g_console.lock; the connection lock guards
 * session/session_tried, so a connection racing the two cannot be given two. */
static void conn_take_session(struct console_conn* c)
{
    if (!g_core) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    if (c->session || c->session_tried) {
        pthread_mutex_unlock(&c->lock);
        return;
    }
    c->session_tried = 1;
    pthread_mutex_unlock(&c->lock);
    conn_open_session(c);
}

static void vp_console_open_pending_sessions(rvvm_machine_t* machine)
{
    if (!machine || !g_core) {
        return;
    }
    pthread_mutex_lock(&g_console.lock);
    /* Under the lock because a connection can end at any moment, and a session
     * handed to one that has just gone would be a terminal nobody holds: the
     * shell would be forked onto it and its output read by nobody. */
    for (struct console_conn* c = g_console.conns; c; c = c->next) {
        conn_take_session(c);
    }
    pthread_mutex_unlock(&g_console.lock);
}

rvvm_machine_t* vp_console_machine(void)
{
    return g_console_machine;
}

void vp_console_set_connect_hook(void (*hook)(void*), void* ud)
{
    g_console.on_connect    = hook;
    g_console.on_connect_ud = ud;
}

/* ------------------------------------------------------------------
 * framing
 * ------------------------------------------------------------------ */

static void put_header(uint8_t* p, uint8_t id, uint32_t len)
{
    p[0] = id;
    /* little endian, written by hand: the wire order is part of the format
     * and memcpy of a uint32 would be the host's order. */
    p[1] = (uint8_t)(len & 0xff);
    p[2] = (uint8_t)((len >> 8) & 0xff);
    p[3] = (uint8_t)((len >> 16) & 0xff);
    p[4] = (uint8_t)((len >> 24) & 0xff);
}

static int write_all(int fd, const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n > 0) {
            p   += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_all(int fd, void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n > 0) {
            p   += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * the guest -> client direction
 *
 * A plain byte FIFO. Packets go in through it whole and come out of it
 * whole; the writer is the only thing that reads it, so it never has to
 * deal with a half-arrived packet and the framing stays out of the data
 * path. Caller holds the connection lock.
 * ------------------------------------------------------------------ */

/* Copy @n bytes out starting @from bytes into the ring, without consuming. */
static void fifo_copyout(struct console_conn* c, size_t from, void* dst, size_t n)
{
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = c->ring[(c->head + from + i) % CONSOLE_RING_BYTES];
    }
}

static void fifo_drop_locked(struct console_conn* c, size_t n)
{
    c->head = (c->head + n) % CONSOLE_RING_BYTES;
    c->len -= n;
    if (!c->len) {
        c->head = c->tail = 0;
    }
}

/* A packet, or -1 when the ring cannot hold it. */
static int conn_push_locked(struct console_conn* c, uint8_t id,
                            const void* data, size_t len)
{
    size_t need = CONSOLE_HDR_BYTES + len;
    if (c->dropped) {
        return -1;
    }
    if (c->len + need > CONSOLE_RING_BYTES) {
        c->dropped = 1;
        LOGW("client fell %zu byte(s) behind; dropping it", c->len);
        pthread_cond_broadcast(&c->filled);
        return -1;
    }
    uint8_t hdr[CONSOLE_HDR_BYTES];
    put_header(hdr, id, (uint32_t)len);
    size_t off = 0;
    for (size_t i = 0; i < CONSOLE_HDR_BYTES; i++) {
        c->ring[c->tail] = hdr[i];
        c->tail = (c->tail + 1) % CONSOLE_RING_BYTES;
    }
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        c->ring[c->tail] = src[i];
        c->tail = (c->tail + 1) % CONSOLE_RING_BYTES;
    }
    (void)off;
    c->len += need;
    pthread_cond_signal(&c->filled);
    return 0;
}

/* The thread that does the blocking write. One per connection, so a slow
 * client costs that client a thread and nothing else. */
static void* conn_writer(void* arg)
{
    struct console_conn* c = (struct console_conn*)arg;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        if (c->dropped) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        if (c->len < CONSOLE_HDR_BYTES) {
            if (c->guest_gone) {
                /* The session's guest end hung up and its output is drained:
                 * nothing more can arrive, so end our half of the stream and
                 * let the client read EOF. It is not a close - whatever the
                 * client is still sending is still read, and the client's own
                 * close is still what ends the connection (see conn_serve). */
                pthread_mutex_unlock(&c->lock);
                shutdown(c->fd, SHUT_WR);
                break;
            }
            pthread_cond_wait(&c->filled, &c->lock);
            pthread_mutex_unlock(&c->lock);
            continue;
        }

        uint8_t hdr[CONSOLE_HDR_BYTES];
        fifo_copyout(c, 0, hdr, sizeof(hdr));
        uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                       ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        if (c->len < CONSOLE_HDR_BYTES + (size_t)len) {
            /* The header is here but the body is not all in yet. */
            pthread_cond_wait(&c->filled, &c->lock);
            pthread_mutex_unlock(&c->lock);
            continue;
        }

        /* Take the packet out of the ring, then release the lock for the
         * write - the write is the part that blocks, and holding the ring
         * across it would stop the tap as well. */
        uint8_t stackbuf[8192];
        uint8_t* payload = stackbuf;
        uint8_t* owned   = NULL;
        if (len > sizeof(stackbuf)) {
            owned = (uint8_t*)malloc(len);
            if (!owned) {
                pthread_mutex_unlock(&c->lock);
                break;
            }
            payload = owned;
        }
        fifo_copyout(c, CONSOLE_HDR_BYTES, payload, len);
        fifo_drop_locked(c, CONSOLE_HDR_BYTES + len);
        pthread_cond_broadcast(&c->cond);
        uint8_t id = hdr[0];
        pthread_mutex_unlock(&c->lock);

        uint8_t out[CONSOLE_HDR_BYTES];
        put_header(out, id, len);
        int bad = (write_all(c->fd, out, sizeof(out)) < 0) ||
                  (len && write_all(c->fd, payload, len) < 0);
        free(owned);
        if (bad) {
            LOGW("client write failed; dropping it");
            pthread_mutex_lock(&c->lock);
            c->dropped = 1;
            pthread_cond_broadcast(&c->filled);
            pthread_mutex_unlock(&c->lock);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * the tap: called on the guest's own thread, never blocks
 * ------------------------------------------------------------------ */

void vp_console_tap(int fd, const void* data, size_t len)
{
    if (!data || !len) {
        return;
    }
    uint8_t id = (fd == 2) ? VP_CONSOLE_STDERR : VP_CONSOLE_STDOUT;

    pthread_mutex_lock(&g_console.lock);
    for (struct console_conn* c = g_console.conns; c; c = c->next) {
        /* Only the run's own console. A client with a session reads that
         * session's terminal instead, pumped by its own thread, and pushing the
         * machine's fd 1/2 at it as well is what crossed two clients: the tap
         * reached every connection, so a second client read the first one's
         * output as if it were its own. A session shell's output never passes
         * through here at all - it writes to its terminal - so skipping these
         * loses nothing. */
        if (c->session) {
            continue;
        }
        pthread_mutex_lock(&c->lock);
        conn_push_locked(c, id, data, len);
        /* READY goes out just behind the guest's first words, and that is the
         * only moment it means anything. The client waits for it before
         * typing, so what it sends arrives after the prompt - and after the
         * ESC[6n the prompt usually carries, which the terminal has answered
         * by the time these bytes were tapped. Sent earlier (on accept) it
         * would only say the socket is up while the guest is still starting,
         * and the driver's script would be queued in front of a prompt that
         * has not been printed yet - read by the shell as part of the answer
         * it is waiting for, and lost. */
        if (!c->ready_sent) {
            conn_push_locked(c, VP_CONSOLE_READY, "console ready\n", 14);
            c->ready_sent = 1;
        }
        pthread_mutex_unlock(&c->lock);
    }
    pthread_mutex_unlock(&g_console.lock);
}

/* Move one session's output from its terminal to its client.
 *
 * This is the session's half of the byte pipe, and it is what lets a session and
 * a plain console client coexist: a session's bytes come from the terminal its
 * shell runs on, a plain client's from the run's fd 1/2, and neither path can see
 * the other's.
 *
 * Ends when the session's guest end is gone - the terminal answers a hangup,
 * which is how a shell that exited reaches its client as an end of output rather
 * than a client left waiting on a shell that is not there. The connection is not
 * closed here: the client may still be reading its transcript, and the reader
 * thread is what decides the socket is done. */
static void* conn_session_pump(void* arg)
{
    struct console_conn* c = (struct console_conn*)arg;
    for (;;) {
        uint8_t buf[4096];
        int64_t n = vp_core_session_output(c->session, buf, sizeof(buf));
        if (n < 0) {
            LOGI("session %s ended (%lld)", vp_core_session_path(c->session),
                 (long long)n);
            /* The shell is gone: hand the writer the end of the stream so the
             * client learns the session is over (see conn_writer). Handed over
             * rather than closed here, because the client may still be reading
             * its transcript and the writer is the only thing that owns the
             * socket. */
            pthread_mutex_lock(&c->lock);
            c->guest_gone = 1;
            pthread_cond_broadcast(&c->filled);
            pthread_mutex_unlock(&c->lock);
            break;
        }
        if (n == 0) {
            /* Nothing yet. A short sleep rather than a spin: this thread belongs
             * to a client probably sitting at a prompt, and would otherwise run
             * flat out for the life of the session. 5ms is well inside what a
             * person perceives as instant and costs nothing when idle. */
            struct timespec ts = { 0, 5 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&c->lock);
        if (c->dropped) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        conn_push_locked(c, VP_CONSOLE_STDOUT, buf, (size_t)n);
        /* READY just behind the session's first words, for the same reason and
         * at the same moment as the console path: a client must not type before
         * there is a prompt to type at. */
        if (!c->ready_sent) {
            conn_push_locked(c, VP_CONSOLE_READY, "console ready\n", 14);
            c->ready_sent = 1;
        }
        pthread_cond_broadcast(&c->filled);
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

/* The run's status, as its own packet. Sent after everything the guest
 * wrote has been handed to the writer, and the writer is the only thing that
 * has touched the socket - so a client that sees `exit` has already been
 * given the output, which is the ordering that decides whether a test reads
 * a complete transcript or a truncated one. */
void vp_console_exit(int code)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", code);
    pthread_mutex_lock(&g_console.lock);
    for (struct console_conn* c = g_console.conns; c; c = c->next) {
        pthread_mutex_lock(&c->lock);
        conn_push_locked(c, VP_CONSOLE_EXIT, buf, (size_t)(n > 0 ? n : 0));
        pthread_mutex_unlock(&c->lock);
    }
    pthread_mutex_unlock(&g_console.lock);
}

/* ------------------------------------------------------------------
 * the client -> guest direction
 * ------------------------------------------------------------------ */

/* Blocking, on this connection's own thread: read a packet, act on it. */
static void* conn_reader(void* arg)
{
    struct console_conn* c = (struct console_conn*)arg;
    for (;;) {
        uint8_t hdr[CONSOLE_HDR_BYTES];
        if (read_all(c->fd, hdr, sizeof(hdr)) < 0) {
            break;
        }
        uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                       ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        if (len > CONSOLE_RING_BYTES) {
            LOGW("client sent a %u byte packet; dropping the connection", len);
            break;
        }
        if (len) {
            char* buf = (char*)malloc(len + 1);
            if (!buf) {
                break;
            }
            if (read_all(c->fd, buf, len) < 0) {
                free(buf);
                break;
            }
            if (hdr[0] == VP_CONSOLE_STDIN) {
                /* The keystrokes a real terminal would deliver. The core's own
                 * line discipline runs them - ICRNL, ISIG for Ctrl-C, erase,
                 * echo - so a client types and does not emulate a terminal.
                 *
                 * With no machine yet there is no line discipline to run them
                 * through and no ring to hold them, so they are kept here: the
                 * client connects as soon as the listener is up, which is
                 * before the run it named has started, and throwing away what
                 * it typed in between loses the beginning of every script.                  * A client with a session types into *its* terminal, not the run's
                 * shared console. That is what makes two clients two terminals:
                 * the bytes go through that shell's own line discipline, so its
                 * echo, its erase and its ^C are its own and another session's
                 * keystrokes are nowhere near it. */
                struct vp_core_session* s = c->session;
                if (s) {
                    LOGI("stdin %zu byte(s) -> session %s", len,
                         vp_core_session_path(s));
                    vp_core_session_input(s, buf, len);
                } else if (vp_console_machine()) {
                    LOGI("stdin %zu byte(s) -> live guest", len);
                    rvvm_user_tty_input(vp_console_machine(), buf, len);
                } else {
                    LOGI("stdin %zu byte(s) held: no guest yet", len);
                    pthread_mutex_lock(&c->lock);
                    if (c->pending_len + len <= CONSOLE_PENDING_MAX) {
                        if (c->pending_len + len > c->pending_cap) {
                            size_t cap = c->pending_cap ? c->pending_cap * 2 : 4096;
                            while (cap < c->pending_len + len) {
                                cap *= 2;
                            }
                            uint8_t* grown = (uint8_t*)realloc(c->pending, cap);
                            if (grown) {
                                c->pending     = grown;
                                c->pending_cap = cap;
                            }
                        }
                        if (c->pending_len + len <= c->pending_cap) {
                            memcpy(c->pending + c->pending_len, buf, len);
                            c->pending_len += len;
                        }
                    }
                    pthread_mutex_unlock(&c->lock);
                }
            }
            free(buf);
        } else if (hdr[0] == VP_CONSOLE_CLOSESTDIN) {
            /* A real pty has an EOF. The run keeps going, but nothing more
             * can be typed into it. Held with the rest when the guest is not
             * up yet, so a script's EOF does not overtake its own lines. */
            struct vp_core_session* s = c->session;
            if (s) {
                /* Into that session's terminal, so the ^D reaches that shell's
                 * read() and not the run's console - and not some other
                 * session's. */
                vp_core_session_input(s, "\x04", 1);
            } else if (vp_console_machine()) {
                rvvm_user_tty_input(vp_console_machine(), "\x04", 1);
            } else {
                pthread_mutex_lock(&c->lock);
                c->pending_eof = 1;
                pthread_mutex_unlock(&c->lock);
            }
        } else if (hdr[0] == VP_CONSOLE_WINDOWSIZE) {
            /* The client's terminal, as "<rows>:<cols>" - the same text the win32
             * client sends and the same shape it parses, so the wire format is
             * untouched by any of the session work.
             *
             * Recorded as well as applied: a session can be opened before any
             * packet has been read, so a client that sends its size first still
             * gets its shell laid out correctly.
             *
             * The body is read here rather than in the block above because that
             * block owns the buffer and frees it. A body too long to be
             * "<rows>:<cols>" is not one, and ends the connection rather than
             * being truncated into something that would parse. */
            char sz[32];
            if (len >= sizeof(sz)) {
                LOGW("client sent a %u byte window size; dropping it", len);
                break;
            }
            if (len && read_all(c->fd, sz, len) < 0) {
                break;
            }
            sz[len] = '\0';
            int rows = 0, cols = 0;
            if (sscanf(sz, "%d:%d", &rows, &cols) == 2 &&
                rows > 0 && cols > 0) {
                pthread_mutex_lock(&c->lock);
                c->winsz_rows = rows;
                c->winsz_cols = cols;
                struct vp_core_session* s2 = c->session;
                pthread_mutex_unlock(&c->lock);
                if (s2) {
                    vp_core_session_resize(s2, rows, cols);
                } else {
                    rvvm_user_tty_resize(vp_console_machine(), rows, cols);
                }
                LOGI("client window is now %dx%d", cols, rows);
            }
        }
    }
    /* The socket is done. Announcing it on a condvar is what lets the accept
     * thread wait for this connection without polling - and it is the reason
     * the accept loop is free the moment both per-connection threads exist
     * rather than being tied up as their handler. */
    pthread_mutex_lock(&c->lock);
    c->finished = 1;
    c->dropped  = 1;
    pthread_cond_broadcast(&c->filled);
    pthread_cond_broadcast(&c->done);
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

/* ------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------ */

static void conn_destroy(struct console_conn* c)
{
    /* The client is gone, so the guest's input is over - said here, once, for
     * every way a connection can end. A shell parked on read(0) otherwise waits
     * forever, because nothing about "the socket closed" reaches the guest: the
     * TTY outlives the connection, and only this knows the difference.
     *
     * The console does not get to choose this either - a client can hang up
     * deliberately while leaving its guest running, and a shell that sees EOF
     * exits, which is the same thing a real terminal's hangup does. The guest's
     * lifetime past that is the Activity's business, not the pipe's.
     *
     * A connection with a *session* is the other exception, for the same reason:
     * its shell was on a terminal of its own, so a ^D aimed at the run's console
     * would not reach its shell at all - it would reach some *other* guest's.
     * Its own shell is ended by the hangup instead, which is what releasing the
     * session does, and that happens in conn_serve before this. */
    rvvm_machine_t* m = vp_console_machine();
    if (m && !c->taken_over && !c->session) {
        rvvm_user_tty_input(m, "\x04", 1);
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    pthread_cond_destroy(&c->filled);
    pthread_cond_destroy(&c->done);
    free(c->ring);
    free(c->pending);
    free(c);
}

static void conn_unlink(struct console_conn* c)
{
    pthread_mutex_lock(&g_console.lock);
    struct console_conn** pp = &g_console.conns;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_console.lock);
}

/* One client's whole life: the two stream threads, and the reap when they end.
 * Runs on its own thread so the accept loop is free the moment the connection
 * exists - which is what lets a second client attach while the first is still
 * typing, instead of queueing behind it. */
static void* conn_serve(void* arg)
{
    struct console_conn* c = (struct console_conn*)arg;

    if (pthread_create(&c->writer, NULL, conn_writer, c) != 0 ||
        pthread_create(&c->reader, NULL, conn_reader, c) != 0) {
        LOGE("cannot start the client threads: %s", strerror(errno));
        conn_unlink(c);
        conn_destroy(c);
        return NULL;
    }
    LOGI("client connected");

    /* A driver that named a guest in its launch intent is waiting on the
     * other end of this socket and cannot tell the difference between "the
     * listener is up" and "someone is actually there". The host uses this to
     * start the guest only once there is a client to read it, which is what
     * keeps a run from starting into a pipe nobody is holding. */
    void (*hook)(void*) = g_console.on_connect;
    void*  hud        = g_console.on_connect_ud;
    if (hook) {
        hook(hud);
    }

    /* A core that is already up hands this client its terminal here; when no
     * machine exists yet - a run held until its first client - vp_console_set_core
     * does it instead (see conn_take_session). */
    pthread_mutex_lock(&g_console.lock);
    conn_take_session(c);
    pthread_mutex_unlock(&g_console.lock);

    /* Wait for the reader to reach the end of the socket, then reap both
     * threads. A condvar, not a sleep: this thread owns this client and the
     * next one belongs to another. */
    pthread_mutex_lock(&c->lock);
    while (!c->finished) {
        pthread_cond_wait(&c->done, &c->lock);
    }
    pthread_mutex_unlock(&c->lock);

    pthread_join(c->reader, NULL);
    pthread_join(c->writer, NULL);
    conn_unlink(c);
    /* The session's terminal goes back with the client, or a long-lived core
     * would run out of them after a handful of visitors. Releasing it is also
     * what ends the shell that was on it - the hangup is the same thing closing
     * a terminal window does - so the shell is gone before its terminal is. */
    if (c->session) {
        LOGI("session %s released", vp_core_session_path(c->session));
        vp_core_session_release(c->session);
        c->session = NULL;
    }
    conn_destroy(c);
    LOGI("client disconnected");
    return NULL;
}

static void* accept_loop(void* arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(g_console.listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;               /* the listener was closed by stop() */
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct console_conn* c = (struct console_conn*)calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd   = fd;
        c->ring = (uint8_t*)malloc(CONSOLE_RING_BYTES);
        pthread_mutex_init(&c->lock, NULL);
        pthread_cond_init(&c->cond, NULL);
        pthread_cond_init(&c->filled, NULL);
        pthread_cond_init(&c->done, NULL);
        if (!c->ring) {
            conn_destroy(c);
            continue;
        }

        pthread_mutex_lock(&g_console.lock);
        /* Whatever was here goes: with CONSOLE_MAX_CONN 1 that is the previous
         * client, every time, not only once a cap is reached. A client that
         * vanished without closing is also handled here - it leaves a ring
         * nothing will ever drain, and it would sit in front of every later
         * packet. */
        int n = 0;
        for (struct console_conn* p = g_console.conns; p; p = p->next) {
            n++;
        }
        struct console_conn* evict = NULL;
        if (n >= CONSOLE_MAX_CONN && g_console.conns) {
            evict = g_console.conns;
            g_console.conns = evict->next;
        }
        c->next        = g_console.conns;
        g_console.conns = c;
        pthread_mutex_unlock(&g_console.lock);

        if (evict) {
            /* Marked and shut down, but not reaped here: its own serve thread
             * does that once its reader sees the socket die. Joining it inline
             * would put the accept loop back to waiting on a client, which is
             * the thing just removed.
             *
             * `taken_over` and not merely `dropped`, because this connection is
             * being replaced rather than hung up on: it must not be told its
             * input is over. conn_destroy sends the guest a ^D on the way out,
             * and that ^D belongs to the guest the *departed* client was
             * talking to - sending it here would EOF the new client's guest
             * out from under it, which is a race rather than a handover. The
             * new client starts its own run, and the old one is simply no
             * longer in the room. */
            LOGW("another client took the console; dropping the previous one");
            pthread_mutex_lock(&evict->lock);
            evict->dropped    = 1;
            evict->taken_over = 1;
            pthread_cond_broadcast(&evict->filled);
            pthread_mutex_unlock(&evict->lock);
            shutdown(evict->fd, SHUT_RDWR);
        }

        pthread_t serve;
        if (pthread_create(&serve, NULL, conn_serve, c) != 0) {
            LOGE("cannot start the client handler: %s", strerror(errno));
            conn_unlink(c);
            conn_destroy(c);
            continue;
        }
        pthread_detach(serve);
    }
    return NULL;
}

int vp_console_start(int port)
{
    if (g_console.running) {
        return 0;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket(): %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(CONSOLE_BIND_ADDR);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(fd, CONSOLE_MAX_CONN) < 0) {
        LOGE("bind/listen(%s:%d): %s", CONSOLE_BIND_ADDR, port, strerror(errno));
        close(fd);
        return -1;
    }
    g_console.listen_fd = fd;
    g_console.running   = 1;
    if (pthread_create(&g_console.accept_thread, NULL, accept_loop, NULL) != 0) {
        LOGE("cannot start the accept thread: %s", strerror(errno));
        close(fd);
        g_console.listen_fd = -1;
        g_console.running   = 0;
        return -1;
    }
    LOGI("listening on %s:%d  (adb forward tcp:%d tcp:%d)", CONSOLE_BIND_ADDR,
         port, port, port);
    return 0;
}

void vp_console_stop(void)
{
    if (!g_console.running) {
        return;
    }
    g_console.running = 0;
    if (g_console.listen_fd >= 0) {
        shutdown(g_console.listen_fd, SHUT_RDWR);
        close(g_console.listen_fd);
        g_console.listen_fd = -1;
    }
    if (g_console.accept_thread) {
        pthread_join(g_console.accept_thread, NULL);
        g_console.accept_thread = 0;
    }
    while (g_console.conns) {
        struct console_conn* c = g_console.conns;
        g_console.conns = c->next;
        pthread_mutex_lock(&c->lock);
        c->dropped = 1;
        pthread_cond_broadcast(&c->filled);
        pthread_mutex_unlock(&c->lock);
        shutdown(c->fd, SHUT_RDWR);
        pthread_join(c->reader, NULL);
        pthread_join(c->writer, NULL);
        conn_destroy(c);
    }
    LOGI("stopped");
}
