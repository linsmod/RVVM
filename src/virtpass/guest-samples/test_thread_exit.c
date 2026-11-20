/*
 * test_thread_exit.c - A thread parked in a host call must not outlive its process
 *
 * rvvm_user destroys a forked address space only after every thread of it has
 * left the interpreter (userland_threads_gone()). A thread parked in a *host*
 * call - accept(), connect(), recv() on an idle socket - is not reachable by
 * riscv_hart_queue_pause(), so if the call does not come back the machine is
 * leaked with a "fork: child threads linger" warning: one machine plus its RVJIT
 * heap per event.
 *
 * Each case below forks a child, parks one of its threads in exactly such a call,
 * lets the main thread exit, and asks the parent to report whether the child
 * address space went away. The child is the one being torn down: exit(2) from its
 * main thread is what starts the teardown while the worker thread is parked.
 *
 * The parent cannot see the core's warning, so what it checks is time: each case
 * has to finish well inside a second, and the helper that reports it is a plain
 * wait4() with a deadline. Running the sample under `RVVM_TRACE=job` and grepping
 * the instance log for "child threads linger" is the direct check.
 *
 * Expected console output: one "ok  <what>" line per case, then
 * "=== PASS: thread exit ===" (or "=== FAIL: N check(s) ===").
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;

static void ok(const char *what)
{
    checks++;
    printf("ok   %s\n", what);
}

static void fail(const char *what, const char *why)
{
    checks++;
    failures++;
    printf("FAIL %s%s%s\n", what, why ? ": " : "", why ? why : "");
}

/* A listening socket on a loopback port nobody connects to. */
static int idle_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    int one = 1;

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;                       /* any free port */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        listen(fd, 1) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
        close(fd);
        return -1;
    }
    if (sa.sin_port == 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* A connected pair: one end is read by the parked thread, the other stays idle. */
static int idle_pair(int sv[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Fork a child that starts @worker in a second thread, then exits from its main
 * thread. Returns 0 when the child's address space was torn down within
 * @budget_s, -1 otherwise. */
typedef void *(*worker_fn)(void *);

static int child_exits_with_parked_thread(worker_fn worker, void *arg,
                                          double budget_s, const char *what)
{
    pid_t pid = fork();
    double start;

    if (pid < 0) {
        fail(what, "fork");
        return -1;
    }
    if (pid == 0) {
        pthread_t th;
        /* The child: park a thread, then let the main thread exit. exit() is
         * what starts the teardown; the parked thread is what it has to wait
         * for. */
        if (pthread_create(&th, NULL, worker, arg) != 0) {
            _exit(90);
        }
        usleep(100 * 1000);   /* let it reach the blocking call */
        _exit(0);
    }

    start = now_secs();
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            double took = now_secs() - start;
            if (took > budget_s) {
                char why[64];
                snprintf(why, sizeof why, "took %.1fs", took);
                fail(what, why);
                return -1;
            }
            ok(what);
            return 0;
        }
        if (r < 0 && errno != EINTR) {
            fail(what, "waitpid");
            return -1;
        }
        if (now_secs() - start > budget_s + 2.0) {
            /* The child is stuck: killing it would not prove anything about the
             * teardown, so report and move on. */
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            fail(what, "child never exited");
            return -1;
        }
        usleep(10 * 1000);
    }
}

static void *worker_accept(void *arg)
{
    int fd = (int)(long)arg;
    accept(fd, NULL, NULL);
    return NULL;
}

static void *worker_connect(void *arg)
{
    int fd = (int)(long)arg;
    struct sockaddr_in sa;

    /* 203.0.113.0/24 is TEST-NET-3: never routed, so a blocking connect parks
     * until the host gives up (tens of seconds on Windows). */
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    inet_pton(AF_INET, "203.0.113.1", &sa.sin_addr);
    connect(fd, (struct sockaddr *)&sa, sizeof sa);
    return NULL;
}

static void *worker_recv(void *arg)
{
    int fd = (int)(long)arg;
    char byte;
    recv(fd, &byte, 1, 0);
    return NULL;
}

int main(void)
{
    int fd;

    printf("thread exit: a parked thread must not keep its address space alive\n");

    fd = idle_listener();
    if (fd < 0) {
        fail("an idle listener could be created", "socket/bind/listen");
    } else {
        child_exits_with_parked_thread(worker_accept, (void *)(long)fd, 2.0,
                                       "exit() with a thread parked in accept()");
        close(fd);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("a socket for connect() could be created", "socket");
    } else {
        child_exits_with_parked_thread(worker_connect, (void *)(long)fd, 2.0,
                                       "exit() with a thread parked in connect()");
        close(fd);
    }

    {
        int sv[2];
        if (idle_pair(sv) < 0) {
            fail("a connected pair could be created", "socketpair");
        } else {
            child_exits_with_parked_thread(worker_recv, (void *)(long)sv[0], 2.0,
                                           "exit() with a thread parked in recv()");
            close(sv[0]);
            close(sv[1]);
        }
    }

    if (failures) {
        printf("=== FAIL: %d check(s) ===\n", failures);
        return 1;
    }
    printf("=== PASS: thread exit ===\n");
    return 0;
}
