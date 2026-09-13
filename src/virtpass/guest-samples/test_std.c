/*
 * test_std.c - guest self-test for the linux-user "std syscall" bridge
 * (host side lives in src/core/rvvm_user.c)
 *
 * What it is for
 * --------------
 * Every guest pointer a syscall hands to the host has to be translated into a
 * host pointer first. A translation that is missing (or only half checked)
 * gives host libc a wild pointer, so the emulator crashes instead of failing
 * the guest's syscall. This sample drives the syscalls that were hardened for
 * that, plus the ones that needed a conversion of their own:
 *
 *   to_ptr() / to_ptr_sz()
 *       Every buffer argument must lie inside guest RAM; a pointer outside it
 *       must come back to the guest as EFAULT. Stage 9 is the audit for this.
 *   futex(2)
 *       The timeout is a *guest* struct timespec (8 byte fields on riscv64)
 *       and must not be handed to the host as-is. A wait that expires has to
 *       report ETIMEDOUT, a wait whose value no longer matches has to report
 *       EAGAIN without blocking.
 *   gettimeofday(2)
 *       Implemented by user mode itself on top of CLOCK_REALTIME, so the value
 *       is cross checked against clock_gettime(CLOCK_REALTIME). Routing it
 *       through a Windows host struct timeval would truncate tv_sec to 32 bits.
 *   statx(2)
 *       A fixed width kernel UAPI layout, so the guest buffer is written
 *       directly; the result is cross checked against stat(2) for the same path.
 *
 * How to read the output
 * ----------------------
 *   PASS  the behaviour was observed
 *   FAIL  it was not (counted, non-zero exit status)
 *   SKIP  this host does not implement that syscall (epoll and statx are
 *         Linux-host only, sockets may be missing), so nothing could be shown
 *   note: a plain indented line is informational, it is not a check
 *
 * Stage 9 feeds NULL, a high user address and one-past-the-top-of-userspace
 * into every syscall that takes a buffer. All of them must answer EFAULT and
 * must keep the host alive: a missing range check shows up as a crashed
 * emulator and a log that stops in the middle of that stage.
 *
 * Three entries used to stay red on the win32 host: real gaps in
 * src/core/rvvm_user.c, reported here instead of being hidden. They are fixed,
 * and the checks (plus the note() guards in stage 9) are kept so that a
 * regression shows up as a FAIL instead of quietly hiding the difference again:
 *   - futex WAIT with a value that no longer matches returned 0, Linux says
 *     EAGAIN (the non-Linux path only polled the word, it had no mismatch exit)
 *   - getresuid(2)/getresgid(2) accepted NULL and reported success, Linux
 *     answers EFAULT
 *   - sched_getaffinity(2) accepted a NULL mask, Linux answers EFAULT
 * Everything else passes, or SKIPs on a host without epoll/statx/sockets.
 *
 * Build: picked up automatically by the guest-samples rule (zig cc, riscv64-musl)
 * Run:   rvvm_user[.exe] test_std[.exe]        or  WinHost -> Run -> test_std
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * riscv64 uses the asm-generic syscall numbers. The fallbacks keep this file
 * building against musl versions that predate some of the macros.
 * ---------------------------------------------------------------------- */
#ifndef SYS_getcwd
#define SYS_getcwd 17
#endif
#ifndef SYS_epoll_create1
#define SYS_epoll_create1 20
#endif
#ifndef SYS_epoll_ctl
#define SYS_epoll_ctl 21
#endif
#ifndef SYS_epoll_pwait
#define SYS_epoll_pwait 22
#endif
#ifndef SYS_statfs
#define SYS_statfs 43
#endif
#ifndef SYS_fstatfs
#define SYS_fstatfs 44
#endif
#ifndef SYS_pipe2
#define SYS_pipe2 59
#endif
#ifndef SYS_readv
#define SYS_readv 65
#endif
#ifndef SYS_fstat
#define SYS_fstat 80
#endif
#ifndef SYS_futex
#define SYS_futex 98
#endif
#ifndef SYS_nanosleep
#define SYS_nanosleep 101
#endif
#ifndef SYS_sched_getaffinity
#define SYS_sched_getaffinity 123
#endif
#ifndef SYS_getresuid
#define SYS_getresuid 148
#endif
#ifndef SYS_gettimeofday
#define SYS_gettimeofday 169
#endif
#ifndef SYS_socketpair
#define SYS_socketpair 199
#endif
#ifndef SYS_mremap
#define SYS_mremap 216
#endif
#ifndef SYS_getrandom
#define SYS_getrandom 278
#endif
#ifndef SYS_statx
#define SYS_statx 291
#endif

/* linux/futex.h is not part of musl, so spell the two ops out here */
#define K_FUTEX_WAIT        0
#define K_FUTEX_WAKE        1
#define K_FUTEX_WAIT_BITSET 9
#define K_FUTEX_BITSET_ANY  0xffffffffu

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

/* -------------------------------------------------------------------------
 * statx: the kernel ABI is a fixed 256 byte structure of 32/64 bit fields, so
 * the guest expects the host to fill exactly these offsets. Defining it here
 * (instead of pulling in a linux/ header) also documents the layout the host
 * side asserts on.
 * ---------------------------------------------------------------------- */
struct k_statx_ts {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t reserved;
};

struct k_statx {
    uint32_t mask;
    uint32_t blksize;
    uint64_t attributes;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t spare0;
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    struct k_statx_ts atime;
    struct k_statx_ts btime;
    struct k_statx_ts ctime;
    struct k_statx_ts mtime;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint64_t mnt_id;
    uint32_t dio_mem_align;
    uint32_t dio_offset_align;
    uint64_t spare3[12];
};

#define K_STATX_TYPE   0x00000001u
#define K_STATX_MODE   0x00000002u
#define K_STATX_NLINK  0x00000004u
#define K_STATX_SIZE   0x00000200u
#define K_STATX_BASIC  0x000007ffu

_Static_assert(sizeof(struct k_statx) == 256, "statx is a fixed 256 byte struct");
_Static_assert(sizeof(struct k_statx_ts) == 16, "statx timestamps are 16 bytes");
_Static_assert(__builtin_offsetof(struct k_statx, mode) == 28, "statx mode offset");
_Static_assert(__builtin_offsetof(struct k_statx, size) == 40, "statx size offset");

/* -------------------------------------------------------------------------
 * Tiny test harness
 * ---------------------------------------------------------------------- */
static int g_stage;
static int g_checked;
static int g_fail;
static int g_skip;

static void stage(int id, const char* name)
{
    g_stage = id;
    printf("\n[stage %d] %s\n", id, name);
}

static char* msgf(const char* fmt, ...)
{
    static char bufs[4][192];
    static int next;
    char* out = bufs[next = (next + 1) % 4];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, sizeof(bufs[0]), fmt, ap);
    va_end(ap);
    return out;
}

static void check(int ok, const char* what)
{
    g_checked++;
    printf("    %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!g_fail) {
            printf("    *** FIRST FAILURE: %s\n", what);
        }
        g_fail++;
    }
}

static void skip(const char* what, const char* why)
{
    g_skip++;
    printf("    %-62s SKIP (%s)\n", what, why);
}

static void note(const char* fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("    - %s\n", buf);
}

/* Host does not implement this syscall at all (as opposed to rejecting us) */
static bool host_lacks(int err)
{
    return err == ENOSYS || err == ENOTSUP;
}

/* Sockets are ENOSYS stubs on the win32 host (src/win/posix_shim.c). The stub's
 * host ENOSYS used to reach the guest as the *guest's* ELOOP (both are 40 on
 * their own platform) while last_errno() was still handing host values through
 * untranslated; it reports an honest ENOSYS now, so the family is recognised by
 * host_lacks() and the ELOOP special case is gone - keeping it would hide a real
 * ELOOP from a broken syscall. Classify the whole family as "this host has no
 * sockets" rather than a bridge failure. */
static bool no_sockets(int err)
{
    return host_lacks(err) || err == EOPNOTSUPP || err == EAFNOSUPPORT ||
           err == EPROTONOSUPPORT;
}

/* -------------------------------------------------------------------------
 * State shared between the stages (kept open for the stage 8 audit)
 * ---------------------------------------------------------------------- */
#define TMP_FILE "rvvm_test_std.tmp"

static int  g_file_fd = -1;                  /* regular file: created, or found */
static const char* g_file_name = TMP_FILE;   /* its guest visible path */
static bool g_file_created;                  /* we made it, so unlink it at exit */
static int  g_pipe[2] = {-1, -1};            /* read/writev/readv/epoll source */
static int  g_epfd = -1;                     /* epoll instance, -1 if unsupported */
static int  g_sock = -1;                     /* socketpair end, -1 if unsupported */
static bool g_sockpair_ok;                   /* sockets are usable on this host */

/* -------------------------------------------------------------------------
 * Stage 1: environment baseline
 * ---------------------------------------------------------------------- */
static void stage_env(void)
{
    stage(1, "environment baseline");

    long page = sysconf(_SC_PAGESIZE);
    check(page == 4096, msgf("page size is 4096 (got %ld)", page));

    struct timespec mono = {0}, wall = {0};
    int rc_mono = clock_gettime(CLOCK_MONOTONIC, &mono);
    int rc_wall = clock_gettime(CLOCK_REALTIME, &wall);
    check(rc_mono == 0 && mono.tv_sec >= 0, "clock_gettime(CLOCK_MONOTONIC) works");
    check(rc_wall == 0 && wall.tv_nsec >= 0 && wall.tv_nsec < 1000000000L,
          msgf("clock_gettime(CLOCK_REALTIME) is sane (%lld.%09ld)", (long long)wall.tv_sec, wall.tv_nsec));
    check(wall.tv_sec > 1600000000LL && wall.tv_sec < 4200000000LL,
          msgf("wall clock is a real date, not truncated (%lld)", (long long)wall.tv_sec));
}

/* -------------------------------------------------------------------------
 * Stage 2: identity & path information syscalls
 * ---------------------------------------------------------------------- */
static void stage_identity(void)
{
    stage(2, "identity & path info syscalls (getcwd, stat, statfs, uname, uid, affinity)");

    /* getcwd(2) itself writes the guest buffer. Whether the guest may *use*
     * the result then depends on the host path being inside the userland
     * prefix (musl rejects a cwd that is not absolute), so the syscall and the
     * libc wrapper are checked separately. */
    char cwd[512] = {0};
    errno = 0;
    long cw = syscall(SYS_getcwd, cwd, sizeof(cwd));
    int cwerr = errno;
    check(cw >= 0 && cwd[0] != 0,
          msgf("getcwd(2) fills the guest buffer (ret=%ld, \"%s\")", cw, cwerr ? strerror(cwerr) : cwd));

    char cwd2[2048] = {0};
    errno = 0;
    long cw2 = syscall(SYS_getcwd, cwd2, sizeof(cwd2));
    check(cw2 >= 0 && !strcmp(cwd, cwd2), "getcwd(2) is stable across buffer sizes");

    if (cwd[0] != '/') {
        skip("libc getcwd() returns an absolute path",
             "host cwd is not inside the userland prefix (the prefix is not mounted here)");
        note("guest visible cwd is the host path \"%s\"", cwd);
    } else {
        char cwd3[512] = {0};
        char* got = getcwd(cwd3, sizeof(cwd3));
        check(got == cwd3 && !strcmp(cwd3, cwd), msgf("libc getcwd() -> \"%s\"", cwd3));
    }

    /* Path lookups: "." always exists, the guest root may be a host prefix */
    struct stat st;
    memset(&st, 0, sizeof(st));
    int rc = stat(".", &st);
    check(rc == 0 && S_ISDIR(st.st_mode), msgf("stat(\".\") is a directory (rc=%d, mode=%o)", rc, st.st_mode));
    check(rc == 0 && st.st_nlink >= 1, msgf("stat(\".\") nlink >= 1 (nlink=%lu)", (unsigned long)st.st_nlink));
    check(rc == 0 && st.st_blksize != 0, msgf("stat(\".\") blksize set (%ld)", (long)st.st_blksize));

    int dfd = open(".", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        skip("fstat(open(\".\"))", msgf("open failed: %s", strerror(errno)));
    } else {
        struct stat fst;
        memset(&fst, 0, sizeof(fst));
        rc = fstat(dfd, &fst);
        check(rc == 0 && S_ISDIR(fst.st_mode), msgf("fstat(dirfd) is a directory (rc=%d)", rc));
        check(rc == 0 && fst.st_dev == st.st_dev && fst.st_ino == st.st_ino,
              "fstat(dirfd) agrees with stat(\".\") on dev/ino");
        close(dfd);
    }

    struct statfs sfs;
    memset(&sfs, 0, sizeof(sfs));
    rc = statfs(".", &sfs);
    if (rc == 0) {
        check(sfs.f_bsize != 0 || sfs.f_blocks != 0,
              msgf("statfs(\".\") wrote the struct (bsize=%lu, blocks=%llu)",
                   (unsigned long)sfs.f_bsize, (unsigned long long)sfs.f_blocks));
    } else if (host_lacks(errno)) {
        skip("statfs(\".\")", strerror(errno));
    } else {
        check(0, msgf("statfs(\".\") failed (rc=%d, %s)", rc, strerror(errno)));
    }

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    rc = uname(&uts);
    check(rc == 0, "uname(2) succeeds");
    if (rc == 0) {
        check(!strcmp(uts.sysname, "Linux"), msgf("uname sysname is \"Linux\" (\"%s\")", uts.sysname));
        check(!strcmp(uts.machine, "riscv64"), msgf("uname machine is \"riscv64\" (\"%s\")", uts.machine));
        check(uts.release[0] && uts.version[0] && uts.nodename[0],
              msgf("uname release/version/nodename filled (\"%s\" \"%s\" \"%s\")",
                   uts.release, uts.version, uts.nodename));
    }

    uid_t ru = (uid_t)-1, eu = (uid_t)-1, su = (uid_t)-1;
    rc = getresuid(&ru, &eu, &su);
    check(rc == 0 && ru == eu && eu == su, msgf("getresuid all equal (rc=%d: %u/%u/%u)", rc, ru, eu, su));
    check(rc == 0 && ru == getuid(), msgf("getresuid matches getuid (%u vs %u)", ru, getuid()));

    gid_t rg = (gid_t)-1, eg = (gid_t)-1, sg = (gid_t)-1;
    rc = getresgid(&rg, &eg, &sg);
    check(rc == 0 && rg == eg && eg == sg, msgf("getresgid all equal (rc=%d: %u/%u/%u)", rc, rg, eg, sg));
    check(rc == 0 && rg == getgid(), msgf("getresgid matches getgid (%u vs %u)", rg, getgid()));

    cpu_set_t set;
    CPU_ZERO(&set);
    long aff = syscall(SYS_sched_getaffinity, 0, sizeof(set), &set);
    if (aff >= 0) {
        check(CPU_COUNT(&set) > 0, msgf("sched_getaffinity wrote a non-empty mask (%d cpus)", CPU_COUNT(&set)));
    } else if (host_lacks(errno)) {
        skip("sched_getaffinity", strerror(errno));
    } else {
        check(0, msgf("sched_getaffinity failed (%s)", strerror(errno)));
    }
}

/* -------------------------------------------------------------------------
 * Stage 3: file & pipe data path
 * ---------------------------------------------------------------------- */
static void stage_data(void)
{
    stage(3, "file & pipe data path (pipe2, read, write, pread, pwrite, readv, writev, getdents64)");

    if (pipe2(g_pipe, O_CLOEXEC)) {
        check(0, msgf("pipe2 failed (%s)", strerror(errno)));
        g_pipe[0] = g_pipe[1] = -1;
    } else {
        check(g_pipe[0] >= 0 && g_pipe[1] >= 0, "pipe2 returns two fd");

        ssize_t n = write(g_pipe[1], "hello", 5);
        check(n == 5, msgf("write(pipe) -> 5 (got %ld)", (long)n));
        char buf[8] = {0};
        n = read(g_pipe[0], buf, sizeof(buf));
        check(n == 5 && !memcmp(buf, "hello", 5), msgf("read(pipe) round trip (got %ld, \"%s\")", (long)n, buf));

        struct iovec wv[3];
        wv[0].iov_base = (void*)"ab"; wv[0].iov_len = 2;
        wv[1].iov_base = (void*)"cd"; wv[1].iov_len = 2;
        wv[2].iov_base = (void*)"ef"; wv[2].iov_len = 2;
        n = writev(g_pipe[1], wv, 3);
        check(n == 6, msgf("writev(pipe, 3 segments) -> 6 (got %ld)", (long)n));

        char rbuf[8] = {0};
        struct iovec rv[3];
        rv[0].iov_base = rbuf;     rv[0].iov_len = 1;
        rv[1].iov_base = rbuf + 1; rv[1].iov_len = 2;
        rv[2].iov_base = rbuf + 3; rv[2].iov_len = 3;
        n = readv(g_pipe[0], rv, 3);
        check(n == 6 && !memcmp(rbuf, "abcdef", 6),
              msgf("readv(pipe, 1+2+3) -> \"%s\" (got %ld)", rbuf, (long)n));

        /* Too many segments must be refused, not attempted */
        struct iovec one = {rbuf, 1};
        errno = 0;
        long many = syscall(SYS_readv, g_pipe[0], &one, 5000);
        check(many < 0 && errno == EINVAL, msgf("readv with 5000 segments rejected (errno=%s)", strerror(errno)));

        struct statfs pfs;
        memset(&pfs, 0, sizeof(pfs));
        long frc = syscall(SYS_fstatfs, g_pipe[0], &pfs);
        if (frc == 0) {
            check(pfs.f_bsize != 0 || pfs.f_blocks != 0, "fstatfs(pipe) wrote the guest struct");
        } else if (host_lacks(errno)) {
            skip("fstatfs(pipe)", strerror(errno));
        } else {
            check(0, msgf("fstatfs(pipe) failed (%s)", strerror(errno)));
        }
    }

    /* Regular file. Creating one is not always possible: the guest root may be
     * read-only, and a host that does not translate open(2) flags cannot honour
     * O_CREAT at all (the win32 host's O_CREAT value differs from the guest's).
     * The read side of the path is the interesting one for this sample, so fall
     * back to a file that is already in the guest cwd. */
    g_file_fd = open(TMP_FILE, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (g_file_fd >= 0) {
        g_file_created = true;
    } else {
        note("cannot create %s (%s), falling back to an existing file", TMP_FILE, strerror(errno));
        static const char* const candidates[] = {"Makefile", "README.md", "project.mk", "CMakeLists.txt"};
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && g_file_fd < 0; i++) {
            struct stat probe;
            memset(&probe, 0, sizeof(probe));
            if (stat(candidates[i], &probe) == 0 && S_ISREG(probe.st_mode) && probe.st_size > 0) {
                g_file_fd = open(candidates[i], O_RDONLY);
                if (g_file_fd >= 0) {
                    g_file_name = candidates[i];
                }
            }
        }
        if (g_file_fd < 0) {
            skip("regular file I/O", "no writable location and no readable file in the guest cwd");
        }
    }

    if (g_file_fd >= 0) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        int rc = fstat(g_file_fd, &st);
        check(rc == 0 && S_ISREG(st.st_mode),
              msgf("fstat(%s) is a regular file (mode=%o, size=%lld)", g_file_name, st.st_mode, (long long)st.st_size));

        struct stat fst = st;
        memset(&st, 0, sizeof(st));
        rc = stat(g_file_name, &st);
        check(rc == 0 && S_ISREG(st.st_mode), msgf("stat(%s) is a regular file (mode=%o)", g_file_name, st.st_mode));
        check(rc == 0 && st.st_size == fst.st_size && st.st_ino == fst.st_ino,
              "stat(2) and fstat(2) agree on size/ino");

        /* A file this sample just created and truncated is empty, so there is
         * nothing to read back yet; the write path below covers it. Only the
         * fallback (a pre-existing file in the guest cwd) has contents. */
        if (st.st_size >= 4) {
            lseek(g_file_fd, 0, SEEK_SET);
            char head[8] = {0};
            ssize_t n = read(g_file_fd, head, 4);
            check(n == 4, msgf("read(file, 4) -> %ld", (long)n));

            char ph[8] = {0};
            n = pread(g_file_fd, ph, 4, 0);
            check(n == 4 && !memcmp(ph, head, 4), msgf("pread(file, 4, 0) matches read() (\"%s\")", ph));
        } else {
            skip("read/pread on the file head",
                 "the file was just created empty (covered by the write/pwrite block)");
        }
    }

    if (g_file_created) {
        ssize_t n = write(g_file_fd, "ABCDEFGH", 8);
        check(n == 8, msgf("write(file) -> 8 (got %ld)", (long)n));

        struct stat st;
        memset(&st, 0, sizeof(st));
        int rc = fstat(g_file_fd, &st);
        check(rc == 0 && st.st_size == 8, msgf("fstat(file) size is 8 (got %lld)", (long long)st.st_size));
        check(rc == 0 && st.st_nlink == 1, msgf("fstat(file) nlink is 1 (got %lu)", (unsigned long)st.st_nlink));

        rc = stat(TMP_FILE, &st);
        check(rc == 0 && st.st_size == 8, msgf("stat(file) size is 8 (got %lld)", (long long)st.st_size));

        n = pwrite(g_file_fd, "xy", 2, 2);
        check(n == 2, msgf("pwrite(file, offset 2) -> 2 (got %ld)", (long)n));
        check(lseek(g_file_fd, 0, SEEK_CUR) == 8, "pwrite does not move the file offset");

        char pread_buf[5] = {0};
        n = pread(g_file_fd, pread_buf, 4, 0);
        check(n == 4 && !memcmp(pread_buf, "ABxy", 4), msgf("pread(file, 4, 0) -> \"%s\"", pread_buf));

        lseek(g_file_fd, 0, SEEK_SET);
        char whole[9] = {0};
        n = read(g_file_fd, whole, 8);
        check(n == 8 && !memcmp(whole, "ABxyEFGH", 8), msgf("file contents after pwrite: \"%s\"", whole));
    } else if (g_file_fd >= 0) {
        skip("write/pwrite on the regular file", "the temp file could not be created here");
    }

    /* Directory listing: the guest must see "." and ".." at least */
    DIR* dir = opendir(".");
    if (!dir) {
        skip("readdir(\".\")", strerror(errno));
    } else {
        bool dot = false, dotdot = false;
        int entries = 0;
        struct dirent* de;
        errno = 0;
        while ((de = readdir(dir)) != NULL && entries < 8) {
            entries++;
            if (!strcmp(de->d_name, ".")) dot = true;
            if (!strcmp(de->d_name, "..")) dotdot = true;
        }
        int listerr = errno;
        closedir(dir);
        if (!entries) {
            if (listerr && (host_lacks(listerr) || listerr == EINVAL)) {
                skip("readdir(\".\")", strerror(listerr));
            } else {
                check(0, "readdir(.) yields entries");
            }
        } else {
            check(dot && dotdot, msgf("readdir(\".\") finds \".\" and \"..\" among %d entries", entries));
        }
    }

    /* getrandom: two calls must not return the same bytes */
    unsigned char r1[32] = {0}, r2[32] = {0};
    long ra = syscall(SYS_getrandom, r1, sizeof(r1), 0);
    long rb = syscall(SYS_getrandom, r2, sizeof(r2), 0);
    if (ra < 0 && host_lacks(errno)) {
        skip("getrandom", strerror(errno));
    } else {
        check(ra == (long)sizeof(r1) && rb == (long)sizeof(r2),
              msgf("getrandom fills the buffer (%ld, %ld)", ra, rb));
        check(memcmp(r1, r2, sizeof(r1)) != 0, "getrandom returns different bytes per call");
    }
}

/* -------------------------------------------------------------------------
 * Stage 4: wall clock - clock_gettime vs the gettimeofday syscall
 * ---------------------------------------------------------------------- */
static void stage_clock(void)
{
    stage(4, "wall clock: gettimeofday(2) must agree with clock_gettime(CLOCK_REALTIME)");

    struct timespec rt = {0};
    clock_gettime(CLOCK_REALTIME, &rt);

    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    errno = 0;
    long rc = syscall(SYS_gettimeofday, &tv, 0);
    if (rc < 0 && host_lacks(errno)) {
        skip("gettimeofday(2)", strerror(errno));
        return;
    }
    check(rc == 0, msgf("gettimeofday(2) syscall succeeds (rc=%ld, %s)", rc, rc < 0 ? strerror(errno) : "ok"));
    if (rc != 0) {
        return;
    }

    check(tv.tv_sec > 1600000000LL && tv.tv_sec < 4200000000LL,
          msgf("gettimeofday seconds are a real date, not truncated (%lld)", (long long)tv.tv_sec));
    check(tv.tv_usec >= 0 && tv.tv_usec < 1000000, msgf("gettimeofday usec in range (%lld)", (long long)tv.tv_usec));

    long long delta = (long long)(tv.tv_sec - rt.tv_sec) * 1000000 + (tv.tv_usec - rt.tv_nsec / 1000);
    check(delta > -2000000 && delta < 2000000,
          msgf("gettimeofday agrees with CLOCK_REALTIME within 2s (delta %lld us)", delta));

    /* the obsolete struct timezone* is accepted (and dropped) - the syscall
     * must not reject a non-NULL second argument */
    struct timeval tv2;
    errno = 0;
    rc = syscall(SYS_gettimeofday, &tv2, &tv2);
    check(rc == 0, msgf("gettimeofday with a timezone pointer succeeds (rc=%ld)", rc));

    /* NULL timeval is legal for the kernel too - it must not crash the host */
    errno = 0;
    rc = syscall(SYS_gettimeofday, 0, 0);
    check(rc == 0, msgf("gettimeofday(NULL, NULL) succeeds (rc=%ld, %s)", rc, rc < 0 ? strerror(errno) : "ok"));
}

/* -------------------------------------------------------------------------
 * Stage 5: statx
 * ---------------------------------------------------------------------- */
static void stage_statx(void)
{
    stage(5, "statx(2): fixed width guest struct, written in place");

    struct k_statx stx;
    memset(&stx, 0, sizeof(stx));
    errno = 0;
    long rc = syscall(SYS_statx, AT_FDCWD, ".", 0, K_STATX_BASIC, &stx);
    if (rc < 0 && host_lacks(errno)) {
        skip("statx(2)", msgf("host does not implement it (%s)", strerror(errno)));
        return;
    }
    check(rc == 0, msgf("statx(\".\") succeeds (rc=%ld, %s)", rc, rc < 0 ? strerror(errno) : "ok"));
    if (rc != 0) {
        return;
    }

    check(stx.mask & K_STATX_TYPE, msgf("statx mask reports STATX_TYPE (0x%x)", stx.mask));
    check(S_ISDIR(stx.mode), msgf("statx(\".\") mode is a directory (mode=%o)", stx.mode));
    check(stx.blksize != 0, msgf("statx blksize set (%u)", stx.blksize));
    check(stx.nlink >= 1, msgf("statx nlink >= 1 (%u)", stx.nlink));

    struct stat st;
    memset(&st, 0, sizeof(st));
    if (stat(".", &st) == 0) {
        check((uint64_t)st.st_size == stx.size,
              msgf("statx size agrees with stat(2) (%llu vs %lld)", (unsigned long long)stx.size, (long long)st.st_size));
        check((uint64_t)st.st_ino == stx.ino, "statx ino agrees with stat(2)");
    }

    if (g_file_fd >= 0) {
        memset(&stx, 0, sizeof(stx));
        rc = syscall(SYS_statx, AT_FDCWD, g_file_name, 0, K_STATX_BASIC, &stx);
        check(rc == 0 && S_ISREG(stx.mode), msgf("statx(%s) is a regular file (mode=%o)", g_file_name, stx.mode));

        struct stat fst;
        memset(&fst, 0, sizeof(fst));
        if (rc == 0 && stat(g_file_name, &fst) == 0) {
            check((uint64_t)fst.st_size == stx.size && (uint64_t)fst.st_ino == stx.ino,
                  "statx(file) agrees with stat(2) on size/ino");
        }
    }
}

/* -------------------------------------------------------------------------
 * Stage 6: mremap (grow a mapping by relocating it)
 * ---------------------------------------------------------------------- */
static void stage_mremap(void)
{
    stage(6, "mremap(2): relocate a growing mapping, keep its contents");

    void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        skip("mmap", strerror(errno));
        return;
    }
    memset(p, 0xAA, 4096);

    errno = 0;
    long grown = syscall(SYS_mremap, p, 4096, 8192, MREMAP_MAYMOVE, 0);
    if (grown < 0 && host_lacks(errno)) {
        skip("mremap(2)", strerror(errno));
        munmap(p, 4096);
        return;
    }
    check(grown > 0, msgf("mremap(page -> 2 pages, MAYMOVE) returns the new address (%s)",
                          grown < 0 ? strerror(errno) : "ok"));
    if (grown > 0) {
        unsigned char* np = (unsigned char*)grown;
        bool kept = true;
        for (int i = 0; i < 4096; i++) {
            if (np[i] != 0xAA) {
                kept = false;
                break;
            }
        }
        check(kept, "mremap preserved the first page");

        bool zeroed = true;
        for (int i = 4096; i < 8192; i++) {
            if (np[i] != 0) {
                zeroed = false;
                break;
            }
        }
        check(zeroed, "mremap zero filled the added page");

        np[8191] = 0x5A; /* must be writable */
        check(np[8191] == 0x5A, "the relocated mapping is writable end to end");
        munmap((void*)grown, 8192);
    }

    /* In place growth cannot be tracked by this allocator, so it is refused
     * instead of silently corrupting whatever was mapped behind it */
    errno = 0;
    long inplace = syscall(SYS_mremap, p, 4096, 8192, 0, 0);
    check(inplace < 0 && errno == ENOMEM,
          msgf("mremap without MAYMOVE is refused (errno=%s)", strerror(errno)));
    munmap(p, 4096);
}

/* -------------------------------------------------------------------------
 * Stage 7: futex timeouts and a real wake-up
 * ---------------------------------------------------------------------- */
static void stage_futex(void)
{
    stage(7, "futex(2): value mismatch and guest timespec timeouts");

    static uint32_t word;
    struct timespec t0, t1, ts;

    /* Value mismatch: returns EAGAIN at once, with and without a timeout.
     * The timeout pointer is what used to be handed to the host as a guest
     * pointer, so it has to be converted rather than dereferenced. */
    word = 123;
    errno = 0;
    long rc = syscall(SYS_futex, &word, K_FUTEX_WAIT, 999, 0, 0, 0);
    check(rc < 0 && errno == EAGAIN, msgf("futex WAIT with a mismatching value -> EAGAIN (%s)",
                                          rc < 0 ? strerror(errno) : "returned success"));

    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    errno = 0;
    rc = syscall(SYS_futex, &word, K_FUTEX_WAIT, 999, &ts, 0, 0);
    check(rc < 0 && errno == EAGAIN, msgf("futex WAIT (5s timeout in the guest struct) -> immediate EAGAIN (%s)",
                                          rc < 0 ? strerror(errno) : "returned success"));
    if (rc == 0) {
        note("regression: the non-Linux futex path has no value-mismatch exit, it answers 0 not EAGAIN");
    }

    /* Wake with no waiter */
    errno = 0;
    rc = syscall(SYS_futex, &word, K_FUTEX_WAKE, 1, 0, 0, 0);
    check(rc >= 0, msgf("futex WAKE with no waiter returns a count (%ld)", rc));

    /* Real expiry: the value matches, so the wait blocks for the timeout */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    word = 0;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    errno = 0;
    rc = syscall(SYS_futex, &word, K_FUTEX_WAIT, 0, &ts, 0, 0);
    int werr = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(rc < 0 && werr == ETIMEDOUT,
          msgf("futex WAIT(relative 50ms) expires with ETIMEDOUT (%s)", rc < 0 ? strerror(werr) : "returned"));
    check(ms >= 20 && ms < 3000, msgf("futex WAIT actually waited for the timeout (%lld ms)", ms));

    /* Absolute deadline (WAIT_BITSET counts on CLOCK_MONOTONIC unless
     * FUTEX_CLOCK_REALTIME is asked for) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    word = 0;
    t0.tv_nsec += 50 * 1000 * 1000;
    if (t0.tv_nsec >= 1000000000L) {
        t0.tv_sec++;
        t0.tv_nsec -= 1000000000L;
    }
    errno = 0;
    rc = syscall(SYS_futex, &word, K_FUTEX_WAIT_BITSET, 0, &t0, 0, K_FUTEX_BITSET_ANY);
    werr = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(rc < 0 && werr == ETIMEDOUT,
          msgf("futex WAIT_BITSET(absolute 50ms) expires with ETIMEDOUT (%s)", rc < 0 ? strerror(werr) : "returned"));
    check(ms >= 0 && ms < 3000, msgf("futex WAIT_BITSET waited until its deadline (%lld ms past it)", ms));

    /* A cross thread wake-up (WAIT -> 0) is deliberately not exercised here:
     * the win32 host tears the whole emulator down when a guest thread exits,
     * so a failing handshake would hang or kill the sample instead of failing
     * a check. RVVM_USER_NO_THREADS=1 makes pthread_create fail cleanly, which
     * is why the wake-up path is covered by the host's own unit harness. */
    note("cross thread futex wake-up is not exercised (guest thread exit is unsafe here)");
}

/* -------------------------------------------------------------------------
 * Stage 9: hostile guest pointers
 *
 * Each of these syscalls hands a guest buffer to the host (to be written, or
 * to be dereferenced). A pointer outside guest RAM must fail with EFAULT
 * instead of reaching host libc. Predates the range checks, the log stops in
 * the middle of this stage and the emulator is gone.
 * ---------------------------------------------------------------------- */
#define WILD_COUNT 3

static void* wild_ptr(int i)
{
    switch (i) {
        case 0: return (void*)(uintptr_t)0;
        case 1: return (void*)(uintptr_t)0x7ffffffff000ull;
        default: return (void*)(uintptr_t)-4096; /* one past the top of userspace */
    }
}

static const char* wild_name(int i)
{
    switch (i) {
        case 0: return "NULL";
        case 1: return "0x7ffffffff000";
        default: return "-4096";
    }
}

typedef long (*wild_call_t)(void* p);

/* i == 0 is the NULL case. Almost every syscall here must reject NULL with
 * EFAULT, but a few (gettimeofday) define it as "do not fill this in", so the
 * caller can opt out of asserting on it. */
static void audit_group_ex(const char* what, wild_call_t fn, bool null_is_legal)
{
    for (int i = 0; i < WILD_COUNT; i++) {
        const char* label = msgf("%s(%s)", what, wild_name(i));
        if (i == 0 && null_is_legal) {
            skip(msgf("%s -> EFAULT", label), "NULL is a legal argument here");
            continue;
        }
        errno = 0;
        long rc = fn(wild_ptr(i));
        int err = errno;
        if (rc >= 0) {
            check(0, msgf("%s -> EFAULT [was accepted, rc=%ld]", label, rc));
        } else if (err == EFAULT) {
            check(1, msgf("%s -> EFAULT", label));
        } else if (host_lacks(err)) {
            skip(msgf("%s -> EFAULT", label), "host does not implement the syscall");
        } else {
            check(0, msgf("%s -> EFAULT [got %s]", label, strerror(err)));
        }
    }
}

static void audit_group(const char* what, wild_call_t fn)
{
    audit_group_ex(what, fn, false);
}

static long call_getcwd(void* p)        { return syscall(SYS_getcwd, p, 64); }
static long call_statfs(void* p)        { return syscall(SYS_statfs, ".", p); }
static long call_fstatfs(void* p)       { return syscall(SYS_fstatfs, g_pipe[0], p); }
static long call_pipe2(void* p)         { return syscall(SYS_pipe2, p, 0); }
static long call_read(void* p)          { return read(g_pipe[0], p, 1); }
static long call_write(void* p)         { return write(g_pipe[1], p, 1); }
static long call_pread(void* p)         { return pread(g_file_fd, p, 1, 0); }
static long call_pwrite(void* p)        { return pwrite(g_file_fd, p, 1, 0); }
static long call_readv_arr(void* p)     { return syscall(SYS_readv, g_pipe[0], p, 1); }
static long call_stat(void* p)          { return syscall(SYS_newfstatat, AT_FDCWD, ".", p, 0); }
static long call_fstat(void* p)         { return syscall(SYS_fstat, g_file_fd, p); }
static long call_getresuid(void* p)     { return syscall(SYS_getresuid, p, 0, 0); }
static long call_affinity(void* p)      { return syscall(SYS_sched_getaffinity, 0, 8, p); }
static long call_getrandom(void* p)     { return syscall(SYS_getrandom, p, 8, 0); }
static long call_gettimeofday(void* p)  { return syscall(SYS_gettimeofday, p, 0); }
static long call_nanosleep(void* p)     { return syscall(SYS_nanosleep, p, 0); }
static long call_statx(void* p)         { return syscall(SYS_statx, AT_FDCWD, ".", 0, K_STATX_BASIC, p); }
static long call_mremap(void* p)        { return syscall(SYS_mremap, p, 4096, 8192, MREMAP_MAYMOVE, 0); }
static long call_socketpair(void* p)    { return syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, p); }
static long call_epoll_ctl(void* p)     { return syscall(SYS_epoll_ctl, g_epfd, EPOLL_CTL_ADD, g_pipe[0], p); }
static long call_epoll_wait(void* p)    { return syscall(SYS_epoll_pwait, g_epfd, p, 1, 100, 0, 0); }

/* The iovec array is valid, one segment points outside guest RAM: the whole
 * call has to fail rather than letting the host write to a wild address */
static long call_writev_badseg(void* p)
{
    struct iovec v = {p, 4};
    return syscall(SYS_writev, g_pipe[1], &v, 1);
}

/* Valid msghdr on the guest stack, wild iovec array behind it */
static long call_sendmsg_badiov(void* p)
{
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = p;
    m.msg_iovlen = 1;
    return syscall(SYS_sendmsg, g_sock, &m, 0);
}

static long call_sendmsg(void* p)       { return syscall(SYS_sendmsg, g_sock, p, 0); }
static long call_recvmsg(void* p)       { return syscall(SYS_recvmsg, g_sock, p, 0); }

static void stage_audit(void)
{
    stage(9, "hostile guest pointer audit: every buffer must answer EFAULT");

    if (g_pipe[0] >= 0) {
        write(g_pipe[1], "!", 1); /* keep the pipe readable for the audit calls */
        audit_group("read(pipe)", call_read);
        audit_group("readv(pipe)", call_readv_arr);
        audit_group("write(pipe)", call_write);
        audit_group("writev(pipe, bad segment)", call_writev_badseg);
        audit_group("pipe2", call_pipe2);
        audit_group("fstatfs(pipe)", call_fstatfs);
    }

    if (g_file_fd >= 0) {
        audit_group("pread(file)", call_pread);
        audit_group("pwrite(file)", call_pwrite);
        audit_group("fstat(file)", call_fstat);
    }

    audit_group("getcwd", call_getcwd);
    audit_group("stat(\".\")", call_stat);
    audit_group("statfs(\".\")", call_statfs);
    audit_group("getresuid", call_getresuid);
    if (syscall(SYS_getresuid, 0, 0, 0) >= 0) {
        note("regression: getresuid(NULL) reports success, Linux answers EFAULT");
    }
    audit_group("sched_getaffinity", call_affinity);
    if (syscall(SYS_sched_getaffinity, 0, 8, 0) >= 0) {
        note("regression: sched_getaffinity(NULL) reports success, Linux answers EFAULT");
    }
    audit_group("getrandom", call_getrandom);
    audit_group_ex("gettimeofday", call_gettimeofday, true); /* NULL is legal */
    audit_group("nanosleep", call_nanosleep);
    audit_group("statx(\".\")", call_statx);
    audit_group("mremap(MAYMOVE)", call_mremap);
    if (g_sockpair_ok) {
        audit_group("socketpair", call_socketpair);
    } else {
        note("socketpair is not audited: the host has no socket support");
    }

    /* The futex address itself: a wild address must not reach the host */
    for (int i = 0; i < WILD_COUNT; i++) {
        errno = 0;
        long rc = syscall(SYS_futex, wild_ptr(i), K_FUTEX_WAKE, 1, 0, 0, 0);
        int err = errno;
        const char* label = msgf("futex(WAKE, %s)", wild_name(i));
        if (err == EFAULT) {
            check(1, msgf("%s -> EFAULT", label));
        } else if (host_lacks(err)) {
            skip(msgf("%s -> EFAULT", label), "host does not implement the syscall");
        } else {
            check(0, msgf("%s -> EFAULT [rc=%ld, %s]", label, rc, strerror(err)));
        }
    }

    if (g_epfd >= 0) {
        /* an event must be pending, otherwise the guest buffer is never touched */
        write(g_pipe[1], "!", 1);
        audit_group("epoll_ctl(ADD, bad event)", call_epoll_ctl);
        for (int i = 0; i < WILD_COUNT; i++) {
            write(g_pipe[1], "!", 1); /* the host dequeues it, so refill each time */
            errno = 0;
            long rc = call_epoll_wait(wild_ptr(i));
            int err = errno;
            const char* label = msgf("epoll_wait(%s)", wild_name(i));
            if (rc < 0 && err == EFAULT) {
                check(1, msgf("%s -> EFAULT", label));
            } else if (host_lacks(err)) {
                skip(msgf("%s -> EFAULT", label), "host does not implement the syscall");
            } else {
                check(0, msgf("%s -> EFAULT [rc=%ld, %s]", label, rc, strerror(err)));
            }
        }
    }

    if (g_sock >= 0) {
        audit_group("sendmsg", call_sendmsg);
        audit_group("sendmsg(bad iov)", call_sendmsg_badiov);
        audit_group("recvmsg", call_recvmsg);
    }
}

/* -------------------------------------------------------------------------
 * Optional: sockets, epoll, pthreads
 * ---------------------------------------------------------------------- */
static void stage_events(void)
{
    stage(8, "socket & event loop (socketpair, sendmsg/recvmsg, epoll)");

    int sv[2] = {-1, -1};
    if (syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv)) {
        if (no_sockets(errno)) {
            skip("socketpair(AF_UNIX)", strerror(errno));
        } else {
            check(0, msgf("socketpair(AF_UNIX, SOCK_STREAM) failed: %s", strerror(errno)));
        }
    } else {
        g_sock = sv[0];
        g_sockpair_ok = true;
        check(sv[0] >= 0 && sv[1] >= 0, "socketpair returns two sockets");

        struct iovec wv[3];
        wv[0].iov_base = (void*)"ab"; wv[0].iov_len = 2;
        wv[1].iov_base = (void*)"cd"; wv[1].iov_len = 2;
        wv[2].iov_base = (void*)"ef"; wv[2].iov_len = 2;
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = wv;
        m.msg_iovlen = 3;
        long n = syscall(SYS_sendmsg, g_sock, &m, 0);
        check(n == 6, msgf("sendmsg(3 segments) -> 6 (got %ld)", n));

        char rbuf[8] = {0};
        struct iovec rv[2];
        rv[0].iov_base = rbuf;     rv[0].iov_len = 4;
        rv[1].iov_base = rbuf + 4; rv[1].iov_len = 4;
        struct msghdr rm;
        memset(&rm, 0, sizeof(rm));
        rm.msg_iov = rv;
        rm.msg_iovlen = 2;
        n = syscall(SYS_recvmsg, sv[1], &rm, 0);
        check(n == 6 && !memcmp(rbuf, "abcdef", 6),
              msgf("recvmsg(4+4 segments) -> \"%s\" (got %ld)", rbuf, n));

        /* Passing a file descriptor exercises the control message conversion */
        char ctrl[CMSG_SPACE(sizeof(int))];
        memset(ctrl, 0, sizeof(ctrl));
        struct msghdr cm;
        memset(&cm, 0, sizeof(cm));
        cm.msg_iov = rv;
        cm.msg_iovlen = 1;
        rv[0].iov_len = 1;
        cm.msg_control = ctrl;
        cm.msg_controllen = sizeof(ctrl);
        struct cmsghdr* ch = CMSG_FIRSTHDR(&cm);
        ch->cmsg_level = SOL_SOCKET;
        ch->cmsg_type = SCM_RIGHTS;
        ch->cmsg_len = CMSG_LEN(sizeof(int));
        int passed = g_file_fd >= 0 ? g_file_fd : sv[1];
        memcpy(CMSG_DATA(ch), &passed, sizeof(int));
        n = syscall(SYS_sendmsg, g_sock, &cm, 0);
        if (n < 0) {
            skip("sendmsg(SCM_RIGHTS fd passing)", strerror(errno));
        } else {
            char cbuf[64];
            memset(cbuf, 0, sizeof(cbuf));
            char one = 0;
            struct iovec rv1 = {&one, 1};
            struct msghdr rc;
            memset(&rc, 0, sizeof(rc));
            rc.msg_iov = &rv1;
            rc.msg_iovlen = 1;
            rc.msg_control = cbuf;
            rc.msg_controllen = sizeof(cbuf);
            n = syscall(SYS_recvmsg, sv[1], &rc, 0);
            int got_fd = -1;
            struct cmsghdr* gc = CMSG_FIRSTHDR(&rc);
            if (n == 1 && gc && gc->cmsg_type == SCM_RIGHTS) {
                memcpy(&got_fd, CMSG_DATA(gc), sizeof(int));
            }
            check(got_fd >= 0, msgf("SCM_RIGHTS round trip returns a descriptor (%d)", got_fd));
            if (got_fd >= 0) {
                close(got_fd);
            }
        }

    }

    /* epoll: Linux-host only, so probe before using it. The instance stays
     * open for the audit stage, which needs a pending event. */
    if (g_pipe[0] < 0) {
        skip("epoll", "no pipe to poll");
    } else {
        long ep = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
        if (ep < 0) {
            skip("epoll", msgf("epoll_create1: %s", strerror(errno)));
        } else {
            g_epfd = (int)ep;
            /* riscv64 lays the UAPI epoll_event out naturally aligned (16
             * bytes), which is what musl does on this target too, so the
             * subscriber data travels as a whole */
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN;
            ev.data.u64 = 0xC0FFEE;
            long cr = syscall(SYS_epoll_ctl, g_epfd, EPOLL_CTL_ADD, g_pipe[0], &ev);
            check(cr == 0, msgf("epoll_ctl(ADD pipe) -> 0 (got %ld, %s)", cr, cr < 0 ? strerror(errno) : "ok"));

            struct epoll_event out;
            memset(&out, 0, sizeof(out));
            long w = syscall(SYS_epoll_pwait, g_epfd, &out, 1, 50, 0, 0);
            check(w == 0, msgf("epoll_wait on an idle pipe times out (got %ld)", w));

            write(g_pipe[1], "e", 1);
            memset(&out, 0, sizeof(out));
            w = syscall(SYS_epoll_pwait, g_epfd, &out, 1, 1000, 0, 0);
            check(w == 1, msgf("epoll_wait reports the readable fd (got %ld)", w));
            check(w == 1 && (out.events & EPOLLIN), msgf("epoll_wait reports EPOLLIN (events=0x%x)", out.events));
            check(w == 1 && out.data.u64 == 0xC0FFEE,
                  msgf("epoll_wait returns the subscriber data (0x%llx)", (unsigned long long)out.data.u64));
            char sink;
            read(g_pipe[0], &sink, 1);
        }
    }
}

/* ---------------------------------------------------------------------- */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("RVVM linux-user std syscall self-test\n");
    printf("(guest pointer range checks, futex timeouts, gettimeofday, statx)\n");

    stage_env();
    stage_identity();
    stage_data();      /* opens the pipe and the temp file the rest reuses */
    stage_clock();
    stage_statx();
    stage_mremap();
    stage_futex();
    stage_events();
    stage_audit();     /* last: it needs the fds the earlier stages opened */

    if (g_epfd >= 0) close(g_epfd);
    if (g_sock >= 0) close(g_sock);
    if (g_pipe[0] >= 0) close(g_pipe[0]);
    if (g_pipe[1] >= 0) close(g_pipe[1]);
    if (g_file_fd >= 0) {
        close(g_file_fd); /* Windows cannot unlink an open file */
    }
    if (g_file_created) {
        unlink(TMP_FILE);
    }

    printf("\n%d checks run, %d failed, %d skipped\n", g_checked, g_fail, g_skip);
    if (g_fail) {
        printf("RESULT: FAIL\n");
    } else {
        printf("RESULT: PASS\n");
    }
    return g_fail != 0;
}
