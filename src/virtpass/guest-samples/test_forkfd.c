/* test_forkfd - a shell's redirect has to survive fork().
 *
 * The chain every `cmd < file` does, and the one a session server stands on:
 * the shell opens the file, dup2()s it onto the descriptor the command reads,
 * closes the original, and forks. The child's copy must be a readable
 * descriptor on that file - not a leftover host number, and not one already
 * sitting at end of file.
 *
 * The child reports through its exit status (and a line on stdout, which is
 * the run's console): an in-process fork() gets an address space of its own,
 * so a counter it bumped would not be visible here.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int  fails = 0;
static char msg[256];

static void check(int ok, const char* what)
{
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static const char* path  = "/tmp/forkfd.txt";
static const char* body  = "fork-fd-content\n";
static const int   blen  = 16;

/* A pty pair held open, the way a session has one: they take host descriptor
 * numbers of their own (and the emulator's reserved ones for the guest), which
 * is what makes the numbers a later open() and dup() land on differ from a
 * plain run's. */
static int hold_pty(int* master_out)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return -1;
    }
    grantpt(master);
    unlockpt(master);
    const char* name = ptsname(master);
    if (!name) {
        close(master);
        return -1;
    }
    int slave = open(name, O_RDWR);
    if (slave < 0) {
        close(master);
        return -1;
    }
    *master_out = master;
    return slave;
}

/* One round of the chain, with @reader deciding how the child reads:
 * 0 = from the inherited fd 0, 1 = from a descriptor it dup()s itself.
 * @slave >= 0 makes 0/1/2 the pty first, which is what a session's shell has:
 * the redirect then replaces a pty on fd 0, and 1/2 stay a pty. */
static int run(int reader, int slave)
{
    int saved[3] = {-1, -1, -1};
    if (slave >= 0) {
        for (int i = 0; i < 3; i++) {
            saved[i] = dup(i);
            dup2(slave, i);
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "open() the file for writing (%s)", strerror(errno));
        check(0, msg);
        return 0;
    }
    ssize_t w = write(fd, body, blen);
    close(fd);
    if (w != blen) {
        check(0, "the file is written");
        return 0;
    }

    /* The shell's half: open, dup2 onto 0, drop the original. */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "open() the file for reading (%s)", strerror(errno));
        check(0, msg);
        return 0;
    }
    if (dup2(fd, 0) != 0) {
        check(0, "dup2() onto fd 0");
        close(fd);
        return 0;
    }
    close(fd);

    /* A churner: another process opening and closing file descriptors at the
     * same time, which is what a session server is (its relay, its other
     * sessions). The host's descriptor numbers are process-wide, so this is
     * what decides which number the chain's own open() and dup() land on. */
    pid_t churn = -1;
    if (reader == 3) {
        churn = fork();
        if (churn == 0) {
            for (int i = 0; i < 400; i++) {
                int c = open("/tmp/forkfd-churn.txt", O_WRONLY | O_CREAT, 0644);
                if (c >= 0) {
                    int d = dup(c);
                    (void)write(c, "x", 1);
                    close(c);
                    if (d >= 0) {
                        close(d);
                    }
                }
            }
            _exit(0);
        }
    }

    pid_t p = fork();
    if (p == 0) {
        if (reader == 2) {
            /* A session's depth: the daemon forks the shell, the shell forks the
             * command, so the descriptor is inherited twice. */
            pid_t g = fork();
            if (g == 0) {
                char buf[64] = {0};
                ssize_t n = read(0, buf, sizeof(buf) - 1);
                printf("     grandchild: read %d byte(s) [%s]\n", (int)n, n > 0 ? buf : "");
                fflush(stdout);
                _exit(n == blen && !memcmp(buf, body, (size_t)blen) ? 0 : 1);
            }
            int gst = 0;
            waitpid(g, &gst, 0);
            _exit(WIFEXITED(gst) ? WEXITSTATUS(gst) : 2);
        }
        int  rfd = reader ? dup(0) : 0;
        char buf[64] = {0};
        ssize_t n = read(rfd, buf, sizeof(buf) - 1);
        printf("     child: read %d byte(s) [%s]\n", (int)n, n > 0 ? buf : "");
        fflush(stdout);
        _exit(n == blen && !memcmp(buf, body, (size_t)blen) ? 0 : 1);
    }
    if (p < 0) {
        check(0, "fork()");
        return 0;
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (churn > 0) {
        int cst = 0;
        waitpid(churn, &cst, 0);
    }
    /* The shell puts its terminal back. */
    if (slave >= 0) {
        for (int i = 0; i < 3; i++) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* The chain with the reader *executed*, which is what a shell really does: the
 * descriptor is inherited by fork() and then has to survive execve() (CLOEXEC,
 * the fd table the new image gets). Output goes to a file so the check does not
 * depend on a terminal. */
static int run_exec(void)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 0;
    }
    ssize_t w = write(fd, body, blen);
    close(fd);
    if (w != blen) {
        return 0;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    dup2(fd, 0);
    close(fd);

    unlink("/tmp/forkfd-out.txt");
    pid_t p = fork();
    if (p == 0) {
        int out = open("/tmp/forkfd-out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            _exit(3);
        }
        dup2(out, 1);
        if (out > 1) {
            close(out);
        }
        execl("/bin/busybox", "busybox", "cat", (char*)NULL);
        _exit(4);   // execve() failed
    }
    if (p < 0) {
        return 0;
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        snprintf(msg, sizeof(msg), "cat ran (status %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        check(0, msg);
        return 0;
    }
    struct stat stt = {0};
    if (stat("/tmp/forkfd-out.txt", &stt)) {
        return 0;
    }
    snprintf(msg, sizeof(msg), "the executed cat wrote the file through (%lld of %d byte(s))",
             (long long)stt.st_size, blen);
    check(stt.st_size == blen, msg);
    unlink("/tmp/forkfd-out.txt");
    return stt.st_size == blen;
}

/* The chain run inside a process that *inherited* its terminal: a server forks
 * it, hands it a pty as 0/1/2 (and nothing else to inherit), and it is that
 * process which runs `cmd < file`. This is the shape a session has, and the one
 * shape the chain has not been checked in yet. */
static int run_in_session(int master, int slave)
{
    pid_t shell = fork();
    if (shell == 0) {
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) {
            close(slave);
        }
        close(master);
        /* 0/1/2 are already the terminal, so the chain is run as it is. */
        int ok = run(0, -1);
        /* Reported on the terminal: the parent reads it off the master. */
        const char* line = ok ? "session: ok\n" : "session: LOST THE FILE\n";
        ssize_t    w     = write(2, line, strlen(line));
        (void)w;
        _exit(ok ? 0 : 1);
    }
    if (shell < 0) {
        return 0;
    }
    int st = 0;
    waitpid(shell, &st, 0);

    /* What the session said, for the log. */
    char    buf[256] = {0};
    ssize_t n        = read(master, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("     %s", buf);
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== fork + inherited descriptor test ===\n");

    unlink(path);

    check(run(0, -1), "a forked child reads the file from the fd it inherited");
    check(run(1, -1), "and from a dup() of it");

    /* The same chain repeated: a descriptor the emulator hands out again must
     * be as fresh as the first one (a recycled host number carrying an old
     * position would read empty the second time round). */
    check(run(0, -1), "the second time round reads it just as well");
    check(run(0, -1), "and the third");

    /* What a shell really does: the reader is execve()d. */
    check(run_exec(), "an execve()d cat gets the file through the inherited fd 0");
    check(run_exec(), "and again");

    /* The shape a session has: a pty pair held open while the same chain runs.
     * A session's shell is where `cat < file` was losing the file, and the pty
     * (plus what else the session inherited) is what moves the host numbers a
     * later open()/dup() lands on. */
    int master = -1;
    int slave  = hold_pty(&master);
    if (slave < 0) {
        check(0, "a pty pair is opened for the session-shaped round");
    } else {
        check(run(0, slave), "the same chain on a pty: 0/1/2 are the session's terminal");
        check(run(2, slave), "and with the descriptor inherited twice (daemon -> shell -> job)");
        check(run(0, slave), "and again (the numbers get recycled)");
        check(run(1, slave), "and with the child dup()ing the descriptor itself");
        /* Concurrent: another process churning through descriptors while the
         * chain runs, which is what a session server does around it. */
        check(run(3, slave), "and while another process churns through descriptors");
        check(run(3, slave), "and again");
        close(slave);
        close(master);

        /* And in the shape a session really has: a forked process whose 0/1/2
         * are a pty it inherited, running the chain itself. */
        for (int round = 0; round < 3; round++) {
            int m2 = -1;
            int s2 = hold_pty(&m2);
            if (s2 < 0) {
                check(0, "a pty pair for the inherited-terminal round");
                break;
            }
            check(run_in_session(m2, s2), "the chain in a process that inherited the pty as its terminal");
            close(s2);
            close(m2);
        }
    }

    unlink(path);
    unlink("/tmp/forkfd-churn.txt");

    if (fails) {
        printf("=== FAIL: %d check(s) ===\n", fails);
        return 1;
    }
    printf("=== PASS: fork + inherited descriptor ===\n");
    return 0;
}
