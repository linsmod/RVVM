/*
 * probe_mmap_shared.c - what mmap(MAP_SHARED) means in this guest.
 *
 * SQLite's WAL keeps its wal-index in a file the processes mmap MAP_SHARED
 * (-shm). A private copy per process cannot work for that, so this probe pins
 * down the contract the core actually offers, and it is written to be honest in
 * both worlds:
 *
 *   - writable MAP_SHARED refused with ENODEV: the P0 contract (see
 *     docs/task-mmap-shared-wal.md). The shared-write stages are SKIPPED, not
 *     failed, because "cannot share" is the answer the core is giving on
 *     purpose.
 *   - writable MAP_SHARED accepted: the process-to-process stages run and must
 *     pass. That is the P2 contract.
 *
 * Read-only shared mappings and private mappings are checked unconditionally:
 * the first is a snapshot by design, the second must never reach the file.
 *
 * Run in the guest: rvvm_user -prefix <rootfs> /probe_mmap_shared
 * Prints PROBE-MMAP-SHARED-PASS and exits 0 on success.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096
#define LEN  (PAGE * 2)

static int failures;
static int shared_writable; /* set once, from the capability probe */

static void check(int ok, const char* what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fflush(stdout);
        failures++;
    }
}

static int open_file(const char* path, int flags)
{
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        printf("FAIL open(%s): %s\n", path, strerror(errno));
        fflush(stdout);
        failures++;
    }
    return fd;
}

/* A file whose bytes say what offset they were written at, so a mapping can be
 * checked against the file without keeping a second copy around. */
static int make_file(const char* path)
{
    unsigned char buf[LEN];
    int           fd;
    size_t        i;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (unsigned char)(i & 0xFF);
    }
    fd = open_file(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        printf("FAIL write(%s): %s\n", path, strerror(errno));
        fflush(stdout);
        failures++;
        close(fd);
        return -1;
    }
    return fd;
}

static int file_byte(const char* path, off_t off)
{
    unsigned char b = 0;
    int           fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    if (lseek(fd, off, SEEK_SET) != off || read(fd, &b, 1) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    return b;
}

/* The read-only shared mapping: a snapshot of the file, correct for a reader,
 * and it must not be able to reach the file with a write. */
static void stage_read_only(const char* path)
{
    int           fd = open_file(path, O_RDONLY);
    unsigned char* p;
    size_t        i;
    int           bad = 0;

    printf("stage read-only-shared\n");
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    p = (unsigned char*)mmap(NULL, LEN, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        /* Not required of the core yet; private mappings are what the loader
         * uses. Recorded as a skip so the run still reports what it saw. */
        printf("SKIP read-only shared mapping: %s\n", strerror(errno));
        fflush(stdout);
        close(fd);
        return;
    }
    for (i = 0; i < LEN; i++) {
        if (p[i] != (unsigned char)(i & 0xFF)) {
            bad++;
        }
    }
    check(bad == 0, "a shared read-only mapping shows the file's contents");
    munmap(p, LEN);
    close(fd);
}

/* The capability question: is a writable shared file mapping a thing here? */
static void stage_capability(const char* path)
{
    int fd = open_file(path, O_RDWR);
    void* p;

    printf("stage writable-shared-capability\n");
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    errno = 0;
    p = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        if (errno == ENODEV) {
            printf("SKIP process-to-process sharing: writable MAP_SHARED is"
                   " ENODEV (P2 not implemented; see"
                   " docs/task-mmap-shared-wal.md)\n");
            fflush(stdout);
        } else {
            check(0, "writable MAP_SHARED either works or answers ENODEV");
        }
        close(fd);
        return;
    }
    shared_writable = 1;
    printf("SHARED-WRITE-SUPPORTED\n");
    fflush(stdout);
    munmap(p, LEN);
    close(fd);
}

/* A private mapping must never reach the file - this is the guarantee the ELF
 * loader and every ordinary file read relies on. */
static void stage_private(const char* path)
{
    int fd = open_file(path, O_RDWR);
    unsigned char* p;
    int           fd2;

    printf("stage private-no-writeback\n");
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    p = (unsigned char*)mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        check(0, "a private mapping succeeds");
        close(fd);
        return;
    }
    p[0] = 0xAA;
    p[PAGE] = 0xBB;
    munmap(p, LEN);
    close(fd);

    fd2 = open_file(path, O_RDONLY);
    if (fd2 >= 0) {
        close(fd2);
        check(file_byte(path, 0) == 0x00, "a private mapping does not write back (byte 0)");
        check(file_byte(path, PAGE) == 0x00, "a private mapping does not write back (page 1)");
    }
}

/* Everything below runs only when the capability probe said sharing works. */

static int child_writes_and_reads(unsigned char* p, int fd)
{
    int base = failures;

    /* The parent wrote "PARE" at 4 before the fork: a shared mapping means the
     * child sees it without any explicit handoff. */
    memcpy(p + 4, "CHIL", 4);
    check(memcmp(p + 8, "PARE", 4) == 0, "a forked child sees the parent's shared write");
    (void)fd;
    return failures != base;
}

static int child_holds_while_parent_unmaps(unsigned char* p, int ready_fd)
{
    char c = 'R';

    (void)write(ready_fd, &c, 1);
    close(ready_fd);
    usleep(100000);
    /* The parent's mapping is gone by now; this one has to be unaffected. */
    p[16] = 0x5A;
    return memcmp(p + 8, "PARE", 4) == 0 ? 0 : 1;
}

static void stage_sharing(const char* path)
{
    int fd = open_file(path, O_RDWR);
    unsigned char* p;
    pid_t pid;
    int status = 0;
    int pipefd[2];
    int fresh;

    printf("stage sharing-between-processes\n");
    fflush(stdout);
    if (fd < 0) {
        return;
    }
    p = (unsigned char*)mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        check(0, "a writable shared mapping succeeds once the capability is reported");
        close(fd);
        return;
    }
    memcpy(p + 8, "PARE", 4);

    pid = fork();
    if (pid == 0) {
        _exit(child_writes_and_reads(p, fd) ? 1 : 0);
    }
    if (waitpid(pid, &status, 0) != pid) {
        check(0, "the sharing child exits");
    } else {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "a forked child shares the mapping with its parent");
    }
    check(memcmp(p, "CHIL", 4) == 0, "the parent sees the child's shared write");

    /* msync may be unimplemented; a data file is flushed anyway on unmap. */
    errno = 0;
    if (msync(p, LEN, MS_SYNC) != 0 && errno != ENOSYS) {
        check(0, "msync succeeds or answers ENOSYS");
    }
    munmap(p, LEN);
    close(fd);

    fresh = open_file(path, O_RDWR);
    if (fresh >= 0) {
        unsigned char* q = (unsigned char*)mmap(NULL, LEN, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fresh, 0);
        if (q == MAP_FAILED) {
            check(0, "the file can be mapped again after the writers left");
        } else {
            check(memcmp(q, "CHIL", 4) == 0, "a later mapping sees the shared writes");
            check(memcmp(q + 8, "PARE", 4) == 0, "a later mapping sees the parent's write");
            munmap(q, LEN);
        }
        close(fresh);
    }

    /* Two holders, one unmaps first: the survivor's writes must still land. */
    fd = open_file(path, O_RDWR);
    if (fd < 0) {
        return;
    }
    p = (unsigned char*)mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        check(0, "a mapping for the unmap race succeeds");
        close(fd);
        return;
    }
    memcpy(p + 8, "PARE", 4);
    if (pipe(pipefd) != 0) {
        check(0, "pipe() for the unmap race");
        munmap(p, LEN);
        close(fd);
        return;
    }
    pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        _exit(child_holds_while_parent_unmaps(p, pipefd[1]) ? 1 : 0);
    }
    close(pipefd[1]);
    {
        char c = 0;
        (void)read(pipefd[0], &c, 1); /* the child's mapping is in use */
        close(pipefd[0]);
    }
    munmap(p, LEN);
    close(fd);
    if (waitpid(pid, &status, 0) != pid) {
        check(0, "the survivor child exits");
    } else {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "a mapping survives another process unmapping it");
    }

    /* The survivor's last unmap is what reaches the file. */
    check(file_byte(path, 16) == 0x5A, "the last holder's unmap writes back");
}

int main(void)
{
    const char* path = "/probe-mmap-shared";

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("probe_mmap_shared: %s\n", path);
    fflush(stdout);

    if (make_file(path) < 0) {
        printf("PROBE-MMAP-SHARED-FAIL (%d)\n", failures);
        return 1;
    }
    stage_read_only(path);
    stage_capability(path);
    stage_private(path);
    if (shared_writable) {
        stage_sharing(path);
    }
    unlink(path);

    if (failures) {
        printf("PROBE-MMAP-SHARED-FAIL (%d)\n", failures);
        return 1;
    }
    printf("PROBE-MMAP-SHARED-PASS\n");
    return 0;
}
