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
 * Everything blocking lives on this side of the JNI boundary. The guest's
 * vCPU thread must never wait for a client: it drops bytes into a ring and
 * returns, and a writer thread does the blocking write. That is the whole
 * reason the listener is here rather than in Java - a blocking push has no
 * counterpart on the Java side of JNI, where there is no way to park a
 * thread on a native event.
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

#include <android/log.h>

#include "vp_console.h"
#include "rvvm_user.h"

#define LOG_TAG "RVVM-Console"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Loopback, never INADDR_ANY. This is a shell into every guest on the
 * device, and the whole point of adb forward is that it is reachable from
 * the machine holding the cable - not from the network the device is on. */
#define CONSOLE_BIND_ADDR "127.0.0.1"

/* One client at a time, and that is the whole point.
 *
 * The console is a single byte pipe onto a single foreground guest, so a
 * second client is not a second viewer - it is a second writer racing the
 * first into the same line discipline, and both are fanned the same output.
 * Two `vp exec-out`s on one phone did exactly that: the second attached to the
 * busybox the first had running, read its prompt and its `ps -ef`, and neither
 * command's own output arrived. Sized for the run table (4 guests) on the
 * assumption that clients and guests pair up one-to-one, which nothing here
 * enforced: vp_console_tap pushes to every connection, so N clients meant N
 * readers of one guest's stream.
 *
 * So the newest client takes the console and the previous one is dropped. A
 * client that wanted to be one of several has to say so, and the alternative -
 * interleaving two commands into one guest - is not a feature anyone can use
 * deliberately. */
#define CONSOLE_MAX_CONN 1

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
        if (c->dropped || c->len < CONSOLE_HDR_BYTES) {
            int gone = c->dropped;
            if (gone) {
                pthread_mutex_unlock(&c->lock);
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
                 * it typed in between loses the beginning of every script. */
                rvvm_machine_t* m = vp_console_machine();
                if (m) {
                    LOGI("stdin %zu byte(s) -> live guest", len);
                    rvvm_user_tty_input(m, buf, len);
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
            rvvm_machine_t* m = vp_console_machine();
            if (m) {
                rvvm_user_tty_input(m, "\x04", 1);
            } else {
                pthread_mutex_lock(&c->lock);
                c->pending_eof = 1;
                pthread_mutex_unlock(&c->lock);
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
     * A connection that was *taken over* is the exception, and skipping it is
     * the point: the ^D would go to whichever guest is current, which after a
     * handover is the new client's, not this one's. Sending it there would EOF
     * a run that had only just started - the takeover would kill the guest it
     * was making room for. */
    rvvm_machine_t* m = vp_console_machine();
    if (m && !c->taken_over) {
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
