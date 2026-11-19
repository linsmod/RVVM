/*
 * probe_fcntl_lock.c - POSIX record locks, the interface SQLite's unix VFS
 * needs. Run in the guest; the host side is rvvm_user.c's emulator-owned
 * lock table, not the host's own fcntl (all guest processes share one host
 * process, so host locks would have the wrong owner).
 *
 * Prints PROBE-FCNTL-LOCK-PASS on success and exits non-zero on failure.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static int lock_fd(int fd, short type, off_t start, off_t len, int cmd)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    return fcntl(fd, cmd, &fl);
}

static int open_file(const char* path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL open(%s): %s\n", path, strerror(errno));
        failures++;
    }
    return fd;
}

static int child_getlk(int fd, int pipe_write)
{
    int base = failures;
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;

    check(fcntl(fd, F_GETLK, &fl) == 0, "F_GETLK succeeds");
    check(fl.l_type == F_WRLCK, "F_GETLK reports the parent's write lock");
    check(fl.l_pid > 0, "F_GETLK reports a lock owner pid");

    errno = 0;
    check(lock_fd(fd, F_WRLCK, 0, 0, F_SETLK) == -1 &&
          (errno == EAGAIN || errno == EACCES),
          "a second process's write lock is refused");
    errno = 0;
    check(lock_fd(fd, F_RDLCK, 0, 0, F_SETLK) == -1 &&
          (errno == EAGAIN || errno == EACCES),
          "a second process's read lock is refused");

    if (pipe_write >= 0) {
        char c = 'R';
        (void)write(pipe_write, &c, 1);
        close(pipe_write);
    }
    return failures != base;
}

static int child_blocking(int fd)
{
    int base = failures;

    printf("child entering F_SETLKW\n");
    fflush(stdout);
    check(lock_fd(fd, F_WRLCK, 0, 0, F_SETLKW) == 0,
          "F_SETLKW waits for the parent to unlock");
    printf("child left F_SETLKW\n");
    fflush(stdout);
    return failures != base;
}

static void wait_child(pid_t pid, const char* what)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        check(0, what);
    } else {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
    }
}

static int child_ranges(int fd)
{
    int base = failures;
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 100;
    fl.l_len = 10;
    check(fcntl(fd, F_GETLK, &fl) == 0, "range F_GETLK succeeds");
    check(fl.l_type == F_RDLCK, "range F_GETLK sees the parent's read lock");

    errno = 0;
    check(lock_fd(fd, F_WRLCK, 100, 10, F_SETLK) == -1 &&
          (errno == EAGAIN || errno == EACCES),
          "overlapping range write lock is refused");
    check(lock_fd(fd, F_WRLCK, 200, 10, F_SETLK) == 0,
          "a non-overlapping range write lock succeeds");
    check(lock_fd(fd, F_UNLCK, 200, 10, F_SETLK) == 0,
          "non-overlapping range unlock succeeds");
    return failures != base;
}

static void test_ranges(const char* path)
{
    int fd = open_file(path);
    pid_t pid;

    printf("stage ranges %s\n", path);
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    check(lock_fd(fd, F_RDLCK, 100, 10, F_SETLK) == 0,
          "parent takes a range read lock");
    pid = fork();
    if (pid == 0) {
        _exit(child_ranges(fd) ? 1 : 0);
    }
    wait_child(pid, "range conflicts are per byte range");
    check(lock_fd(fd, F_UNLCK, 100, 10, F_SETLK) == 0,
          "parent unlocks the range");
    close(fd);
    unlink(path);
}

static void test_path(const char* path)
{
    int fd = open_file(path);
    pid_t pid;

    printf("stage %s: open/lock\n", path);
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    check(lock_fd(fd, F_WRLCK, 0, 0, F_SETLK) == 0,
          "parent takes a write lock");
    pid = fork();
    if (pid == 0) {
        int rc = child_getlk(fd, -1);
        printf("child getlk done rc=%d\n", rc);
        fflush(stdout);
        _exit(rc ? 1 : 0);
    }
    printf("parent waits getlk child %d\n", (int)pid);
    fflush(stdout);
    wait_child(pid, "contending process sees the parent's lock");
    check(lock_fd(fd, F_UNLCK, 0, 0, F_SETLK) == 0,
          "parent unlocks");

    /* F_SETLKW must actually wait, then succeed once the parent unlocks. */
    printf("stage blocking\n");
    fflush(stdout);
    check(lock_fd(fd, F_WRLCK, 0, 0, F_SETLK) == 0,
          "parent takes the lock for the blocking test");
    pid = fork();
    if (pid == 0) {
        _exit(child_blocking(fd) ? 1 : 0);
    }
    /* The child is in F_SETLKW; the parent releases the lock after a delay.
     * No pipe handshake: the child may not be scheduled before the parent's
     * sleep ends, and that is still a successful (non-blocking) acquisition. */
    usleep(300000);
    check(lock_fd(fd, F_UNLCK, 0, 0, F_SETLK) == 0,
          "parent unlocks while the child blocks");
    wait_child(pid, "F_SETLKW acquires after the parent unlocks");

    /* Closing any descriptor for the file releases the process's locks. */
    check(lock_fd(fd, F_WRLCK, 0, 0, F_SETLK) == 0,
          "parent locks before close");
    close(fd);
    pid = fork();
    if (pid == 0) {
        int base = failures;
        int child_fd = open_file(path);
        if (child_fd < 0) {
            _exit(1);
        }
        check(lock_fd(child_fd, F_WRLCK, 0, 0, F_SETLK) == 0,
              "close released the parent's lock");
        check(lock_fd(child_fd, F_UNLCK, 0, 0, F_SETLK) == 0,
              "child unlocks");
        close(child_fd);
        _exit(failures != base ? 1 : 0);
    }
    wait_child(pid, "a lock is released by close()");

    unlink(path);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_path("/tmp/probe-fcntl-lock");
    test_path("/root/probe-fcntl-lock");
    test_ranges("/tmp/probe-fcntl-ranges");
    test_ranges("/root/probe-fcntl-ranges");

    if (failures) {
        printf("PROBE-FCNTL-LOCK-FAIL (%d)\n", failures);
        return 1;
    }
    printf("PROBE-FCNTL-LOCK-PASS\n");
    return 0;
}
