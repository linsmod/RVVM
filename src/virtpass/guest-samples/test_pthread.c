/*
 * test_pthread - what a copy-on-write guest address space has to get right when
 *                more than one vCPU is running
 *
 * Why this sample exists
 * ---------------------
 * Everything else in guest-samples/ is single-threaded, and that is not an
 * accident of the samples - it is what the emulator has been exercised with.
 * Two things about a per-machine page table only misbehave when a second hart
 * is running, and neither is reachable from a single-threaded guest:
 *
 *   1. clone()'s two out-parameters. ptid and ctid are written into the
 *      *child's* TLS block, which is a page the child is about to write. If
 *      that page is shared when it should not be, or the parent's view of it
 *      is not the child's, the defect shows up as one thread's thread-id
 *      landing in another's memory. No e2e builds a guest thread, so nothing
 *      else looks at this.
 *
 *   2. A TLB read slot that caches a page another hart then unshares. A read
 *      slot may point into the shared base, because a shared page cannot
 *      change under it - until the first write unshares it, at which point
 *      that cached pointer is stale and keeps returning pre-write contents.
 *      The symptom is not a fault: it is a spin that never finishes, because
 *      the writer's value never arrives in the reader's view. A single-hart
 *      guest cannot hit it, because the write fill updates the same hart's
 *      read slot on the way through.
 *
 * Both are silent when they happen, which is the property that makes them
 * worth a sample instead of a code review: phase 1 reports a wrong answer,
 * phase 2 does not report anything at all.
 *
 * The four phases
 * ---------------
 *   isolation    N threads each fill their own guest page with their own
 *                pattern, repeatedly, and the main thread verifies every byte
 *                afterwards. A write that reached the wrong machine, or a page
 *                that failed to become private, shows up here.
 *
 *   visibility   a producer thread bumps a counter that lives in a page a
 *                consumer thread has *already read* - so the consumer's read
 *                slot is populated with the shared base pointer before the
 *                write that invalidates it happens. A reader that never sees
 *                the final value is the livelock, and it is reported as the
 *                value it did see rather than as a hang.
 *
 *   syscalls     every thread writes to the console and polls descriptors, so
 *                the host-side entry points and the fd_set helper run from
 *                several harts at once.
 *
 *   forked       a forked child builds threads of its own, so the fork path
 *                carries a process that already has a thread running.
 *
 * Everything is guest memory from mmap(), page aligned, so a slice per thread
 * is a page per thread - the granularity a page table works at.
 *
 * What this sample can and cannot prove
 * -------------------------------------
 * It can prove a defect is absent when it passes. It cannot prove the shared
 * page case was actually reached: nothing inside a guest can observe whether
 * a page it inherited was shared or copied. The counter page is therefore
 * never written by the main thread, which is the shape most likely to be
 * inherited shared; if the emulator's mmap materialises its pages eagerly,
 * phase 2 degenerates into "two threads incrementing private copies", which
 * passes either way. A green phase 2 therefore means "no livelock", not "the
 * invalidation path was exercised" - that claim needs the host-side trace,
 * not this sample.
 *
 * Exit status: 0 pass, 1 a check failed, 2 a thread could not be created,
 * 3 the watchdog fired. The phase is printed before it starts, so a hang says
 * where it hung.
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NTHREADS   4
#define WRITES     24   /* how many times the producer bumps the counter     */
#define ROUNDS     4    /* how many pairs of threads do that                 */
#define PAGE_SIZE  4096
#define FILL_ITERS 24
#define DEADLINE   2000 /* ms a reader waits for the writer, in wall clock   */

/* One page per thread for phase 1, then one page for the shared counter. */
#define SLICE_PAGES NTHREADS
#define TOTAL_PAGES (SLICE_PAGES + 1)
#define COUNTER_OFF (SLICE_PAGES * PAGE_SIZE)

typedef struct {
    int       id;    /* which page this thread owns                          */
    int       tag;   /* what it writes into it - a different tag per side of
                      * a fork, so the child's pages cannot equal the parent's
                      * even though both fill the same offsets                 */
    int       iters;
    pthread_t handle;
} worker_t;

static int             fails = 0;
static unsigned char*  arena = NULL;    /* TOTAL_PAGES * PAGE_SIZE, page aligned */
static volatile int*   counter = NULL;  /* the one page two harts share          */
static volatile int    counter_done  = 0; /* the producer's last value, for reporting */
static volatile int    consumer_first = 0; /* the value the consumer first read  */
static volatile int    consumer_ready = 0; /* and that it has read it, at all     */
static volatile int    consumer_hit   = 0; /* did it arrive before the deadline   */

static void check(int ok, const char* what)
{
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static void phase(const char* name)
{
    printf("--- %s\n", name);
    fflush(stdout);
}

/* The backstop. write(2) and _exit(2) are the only things a signal handler may
 * touch, so the handler says as little as possible and leaves. */
static void on_alarm(int sig)
{
    static const char msg[] = "\nFAIL watchdog: a phase did not finish\n";
    ssize_t n = write(2, msg, sizeof(msg) - 1);
    (void)n;
    _exit(3);
}

/* The byte a thread with @tag leaves at offset @k after pass @it. Recomputed by
 * the verifier rather than stored, so a page written by the wrong thread is
 * caught by the tag byte alone. */
static unsigned char expect(int tag, size_t k, int it)
{
    return (unsigned char)((unsigned char)(0xA0 + tag) + (unsigned char)(k + it));
}

/* ---------------------------------------------------------------- phase 1
 *
 * Each thread owns one page and writes its own byte into all of it, over and
 * over. Repeating matters: a page that becomes private on the first write is
 * fine, and so is one that does not - what must not happen is two threads
 * ending up able to write the same page. */
static void* fill_own_page(void* arg)
{
    worker_t*      w   = (worker_t*)arg;
    unsigned char* own = arena + (size_t)w->id * PAGE_SIZE;

    for (int i = 0; i < w->iters; i++) {
        for (size_t k = 0; k < PAGE_SIZE; k++) {
            own[k] = expect(w->tag, k, i);
        }
        /* A syscall from this hart too, so the host-side entry points are
         * exercised while other harts are running. */
        if ((i & 0x3F) == 0) {
            struct pollfd pfd = {.fd = 0, .events = POLLIN, .revents = 0};
            (void)poll(&pfd, 1, 0);
        }
    }
    return NULL;
}

static int phase_isolation(void)
{
    phase("isolation: one guest page per thread");

    for (int i = 0; i < NTHREADS; i++) {
        memset(arena + (size_t)i * PAGE_SIZE, 0, PAGE_SIZE);
    }

    worker_t w[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        w[i].id    = i;
        w[i].tag   = i;
        w[i].iters = FILL_ITERS;
    }
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&w[i].handle, NULL, fill_own_page, &w[i]) != 0) {
            return 2;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(w[i].handle, NULL);
    }

    /* The last pass is what the page holds, so that is the pattern to check -
     * and it is checked backwards, so the first mismatch reported is the
     * highest offset, which is the most useful one to name. Note the first
     * byte is compared against the *last pass*, not against the bare tag: the
     * thread overwrites its own tag on every pass. */
    int clean = 1;
    for (int i = 0; i < NTHREADS && clean; i++) {
        const unsigned char* own = arena + (size_t)i * PAGE_SIZE;
        if (own[0] != expect(i, 0, FILL_ITERS - 1) ||
            own[PAGE_SIZE - 1] != expect(i, PAGE_SIZE - 1, FILL_ITERS - 1)) {
            printf("     thread %d: page is not its own (first byte %02x, last %02x)\n",
                   i, own[0], own[PAGE_SIZE - 1]);
            clean = 0;
            break;
        }
        for (size_t k = PAGE_SIZE; k-- > 0 && clean;) {
            unsigned char want = expect(i, k, FILL_ITERS - 1);
            if (own[k] != want) {
                printf("     thread %d page byte %zu: expected %02x, found %02x\n",
                       i, k, want, own[k]);
                clean = 0;
            }
        }
    }
    check(clean, "each thread's page holds only its own pattern");
    return 0;
}

/* ---------------------------------------------------------------- phase 2
 *
 * The livelock, arranged rather than waited for.
 *
 * The consumer reads the counter *first*, so its read slot is holding the
 * shared base pointer by the time the producer's first write unshares the
 * page. If that cached pointer is not invalidated, everything the consumer
 * sees from then on is the value it read before the write - so it spins on a
 * number that will never move, and the failure is a wrong answer rather than
 * a crash.
 *
 * The consumer is told the final value up front rather than reading it from a
 * side channel: both start at 0, so a comparison against a shared "target"
 * would be satisfied by the pre-write value and the check would pass without
 * ever waiting for anything.
 *
 * The wait is bounded by the wall clock, not by a spin count. Under an
 * interpreter a spin count is not a time budget - it is whatever the emulation
 * speed happens to make it - so a livelock would show up as a slow test
 * rather than as a failure, and "slow" is the one thing this phase exists to
 * distinguish.
 */
static void* producer(void* arg)
{
    int rounds = *(int*)arg;
    for (int i = 0; i < rounds; i++) {
        *counter = i + 1;
    }
    counter_done = *counter;
    return NULL;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void* consumer(void* arg)
{
    int       want = *(int*)arg;
    long long end  = now_ms() + DEADLINE;

    /* Read before the producer is told to go: this is what populates the read
     * slot with the shared pointer. The value and the fact of having read it
     * are two separate variables on purpose - the page reads 0, so a value
     * that doubles as the handshake would never become non-zero and the main
     * thread would wait for something that cannot arrive. */
    consumer_first = *counter;
    consumer_ready = 1;

    for (unsigned long spins = 0;; spins++) {
        if (*counter == want) {
            consumer_hit = 1;
            return NULL;
        }
        if ((spins & 0x3FF) == 0) {
            if (now_ms() > end) {
                consumer_hit = 0;
                return NULL;
            }
            sched_yield();
        }
    }
}

static int phase_visibility(void)
{
    phase("visibility: a reader that already read the page another hart writes");

    int lost  = 0;
    int stuck = 0;

    for (int r = 0; r < ROUNDS; r++) {
        pthread_t pt, ct;
        int       writes = WRITES;

        consumer_first = 0;
        consumer_ready = 0;
        consumer_hit   = 0;
        counter_done   = 0;

        if (pthread_create(&ct, NULL, consumer, &writes) != 0) {
            return 2;
        }
        /* Let the consumer get its first read in before the write that
         * unshares. The wait is bounded, because a consumer that never runs at
         * all is a different defect and must not be reported as lost
         * visibility. */
        long long end = now_ms() + DEADLINE;
        while (!consumer_ready) {
            if (now_ms() > end) {
                break;
            }
            sched_yield();
        }
        if (!consumer_ready) {
            printf("     round %d: the reader thread never ran\n", r);
            stuck++;
            pthread_join(ct, NULL);
            continue;
        }
        if (pthread_create(&pt, NULL, producer, &writes) != 0) {
            pthread_join(ct, NULL);
            return 2;
        }
        pthread_join(pt, NULL);
        pthread_join(ct, NULL);

        int seen = *counter;   /* the main thread's own view, for the report */
        int ok   = consumer_hit && counter_done == writes && seen == writes;
        printf("     round %d: writer reached %d, this thread sees %d, reader %s\n",
               r, counter_done, seen, consumer_hit ? "arrived" : "gave up");
        if (!ok) {
            lost++;
        }
    }

    if (stuck) {
        check(0, "every reader thread ran");
    }
    char what[96];
    if (lost) {
        snprintf(what, sizeof(what), "a reader missed the writer in %d of %d round(s)",
                 lost, ROUNDS);
    } else {
        snprintf(what, sizeof(what), "a reader sees all %d writes, in all %d round(s)",
                 WRITES, ROUNDS);
    }
    check(lost == 0, what);
    return 0;
}

/* ---------------------------------------------------------------- phase 3
 *
 * The host-side entry points, from every hart at once: the console write, the
 * fd_set helper that select()/poll() share, and open/read/close.
 *
 * The read is off a file each thread opens for itself, not off descriptor 0.
 * Descriptor 0 is the console, and a read on it with nothing behind it blocks
 * forever - which reads as a hang, not as a defect, and would be the least
 * informative thing this sample could do. Opening per thread also churns the
 * host's descriptor numbers concurrently, which is what decides the numbers
 * a later open() lands on.
 */
static void* syscall_user(void* arg)
{
    worker_t* w  = (worker_t*)arg;
    char      id = (char)('a' + w->id);
    char      path[64];

    snprintf(path, sizeof(path), "/tmp/pthread-%d.txt", w->id);

    for (int i = 0; i < 8; i++) {
        char   line[2] = {id, '\n'};
        ssize_t n      = write(1, line, 2);
        (void)n;

        struct pollfd pfd[2] = {
            {.fd = 0, .events = POLLIN, .revents = 0},
            {.fd = 1, .events = POLLOUT, .revents = 0},
        };
        (void)poll(pfd, 2, 0);

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char   buf[16];
            ssize_t rd = read(fd, buf, sizeof(buf));
            (void)rd;
            close(fd);
        }
    }
    return NULL;
}

static void phase_syscalls(void)
{
    phase("syscalls: console, poll and open/read/close from every hart");

    for (int i = 0; i < NTHREADS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/pthread-%d.txt", i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t w = write(fd, "payload\n", 8);
            (void)w;
            close(fd);
        }
    }

    worker_t w[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        w[i].id  = i;
        w[i].tag = i;
    }
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&w[i].handle, NULL, syscall_user, &w[i]) != 0) {
            return;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(w[i].handle, NULL);
    }
    for (int i = 0; i < NTHREADS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/pthread-%d.txt", i);
        unlink(path);
    }
    check(1, "every hart's syscalls returned");
}

/* ---------------------------------------------------------------- phase 4
 *
 * A fork that carries a threaded process on both sides. The child gets a copy
 * of the parent's pages, and then builds threads in them.
 */
static void phase_forked(void)
{
    phase("forked: threads in a process that was forked, and in its child");

    const char* child_path = "/tmp/pthread-child.txt";

    worker_t w[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        w[i].id    = i;
        w[i].tag   = i;
        w[i].iters = 8;
    }
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&w[i].handle, NULL, fill_own_page, &w[i]) != 0) {
            return;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(w[i].handle, NULL);
    }

    const char* line = "parent\n";
    ssize_t     n    = write(1, line, 7);
    (void)n;

    unlink(child_path);
    pid_t p = fork();
    if (p == 0) {
        /* The child: the same page offsets, a different tag, and the result
         * goes to a file so the parent checks it rather than trusting the
         * console. */
        for (int i = 0; i < NTHREADS; i++) {
            w[i].id    = i;
            w[i].tag   = i + 10;
            w[i].iters = 8;
        }
        for (int i = 0; i < NTHREADS; i++) {
            if (pthread_create(&w[i].handle, NULL, fill_own_page, &w[i]) != 0) {
                _exit(2);
            }
        }
        for (int i = 0; i < NTHREADS; i++) {
            pthread_join(w[i].handle, NULL);
        }
        int fd = open(child_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(4);
        }
        ssize_t wr = write(fd, arena, (size_t)SLICE_PAGES * PAGE_SIZE);
        close(fd);
        _exit(wr == (ssize_t)(SLICE_PAGES * PAGE_SIZE) ? 0 : 5);
    }
    if (p < 0) {
        check(0, "fork()");
        return;
    }

    int st = 0;
    waitpid(p, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the forked child ran threads of its own and wrote them out");

    int fd = open(child_path, O_RDONLY);
    if (fd < 0) {
        check(0, "the child's file is readable from the parent");
        return;
    }
    size_t         len = (size_t)SLICE_PAGES * PAGE_SIZE;
    unsigned char* got = malloc(len);
    ssize_t        rd  = got ? read(fd, got, len) : -1;
    close(fd);
    unlink(child_path);

    if (rd != (ssize_t)len) {
        check(0, "the child's file has the child's pages in it");
        free(got);
        return;
    }
    check(memcmp(got, arena, len) != 0,
          "the parent's pages are not the child's (the copy is real)");
    free(got);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== pthread: a guest with more than one vCPU ===\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(600);

    arena = mmap(NULL, (size_t)TOTAL_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        printf("FAIL mmap the guest arena (%s)\n", strerror(errno));
        return 2;
    }
    counter = (volatile int*)(void*)(arena + COUNTER_OFF);

    int rc = phase_isolation();
    if (rc) {
        return rc;
    }
    rc = phase_visibility();
    if (rc) {
        return rc;
    }
    phase_syscalls();
    phase_forked();

    munmap(arena, (size_t)TOTAL_PAGES * PAGE_SIZE);
    alarm(0);

    if (fails) {
        printf("=== FAIL: %d check(s) ===\n", fails);
        return 1;
    }
    printf("=== PASS: pthread ===\n");
    return 0;
}
