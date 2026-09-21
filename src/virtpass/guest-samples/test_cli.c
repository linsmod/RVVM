/*
 * test_cli.c - interactive VirtPass guest shell
 *
 * What it is for
 * --------------
 * The other samples are batch self-tests: they print, check and exit. This one
 * is the interactive counterpart - a small shell that reads commands from the
 * guest's console and answers them. It is the end-to-end exercise of the
 * *input* half of the VirtPass console, which nothing else covers:
 *
 *   read(0, ...)      with a TTY attached, fd 0 is the host console, not the
 *                     host process's stdin. The host assembles a line (echo,
 *                     backspace, ^C via the line discipline, rvvm_user_tty_input)
 *                     and the guest drains the cooked bytes. This sample reads
 *                     one byte at a time and rebuilds the line itself, so it
 *                     stays independent of stdio buffering and a ^C arrives as
 *                     EINTR between bytes.
 *   SIGINT            the handler installed below proves ^C is delivered *in
 *                     guest* while blocked on read(0): the read returns EINTR,
 *                     the loop prints ^C and re-prompts instead of the run
 *                     being killed.
 *   ioctl(TIOCGWINSZ) the shell sizes its rules and ls columns from the grid
 *                     the host actually renders (24x80 when nothing is
 *                     attached), rather than assuming a width.
 *   isatty()          probes fd 0/1 through ioctl(TCGETS), which is how the
 *                     shell decides whether to emit ANSI color at all.
 *
 * Every command doubles as a probe of a syscall the userland bridge has to
 * translate: pwd/cd (getcwd, chdir), ls (opendir/readdir/stat), cat (open/read),
 * stat, mkdir/touch/rm/mv/ln/readlink/truncate (mkdirat, openat with O_CREAT,
 * unlinkat/rmdir, renameat2, symlinkat, readlinkat, truncate), asset
 * (AAssetManager_*, a shell over the /assets mount), uname, date
 * (clock_gettime), id (uid/gid/getpid), env (the host environ passed into the
 * guest), calc (integer arithmetic through the JIT), rand (getrandom), sleep
 * (nanosleep), tty, clear, history, exit.
 *
 * Commands are the interesting part of the output; each keeps its failure text
 * on stderr and its result on stdout, so replaying a script against the shell
 * gives usable output.
 *
 * How to use it
 * -------------
 *   WinHost -> Run -> test_cli        (or: rvvm_user[.exe] test_cli[.exe])
 *
 *   vp> help
 *   vp> ls /
 *   vp> cat README.md
 *   vp> calc 0x1000 + 0x23
 *   vp> exit
 *
 * With stdin connected to something that is not a terminal (a pipe or a file)
 * the prompt is suppressed and the program runs as a script: echo the commands
 * in, and the exit status is non-zero when any command failed. That makes it
 * usable as a smoke test:
 *
 *   printf 'uname\ncalc 6 * 7\nhistory\nexit\n' | test_cli
 *
 * Arguments are the third form: everything after argv[0] is one command line,
 * run once, with nothing interactive about it. That is the form a host can drive
 * without a console - on Android:
 *
 *   adb shell am start -n com.rvvm.android/.MainActivity \
 *     --es guest test_cli.exe --esa argv "cat,/assets/fonts/JetBrainsMono-OFL.txt"
 *
 *   test_cli -c "cat /assets/x"     # == test_cli cat /assets/x
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* The guest NDK surface, for the one command that reaches outside the guest
 * file system (AAssetManager_*). */
#include "virtpass/vp_android.h"

/* riscv64 uses the asm-generic syscall numbers; keep this file building against
 * libcs that predate the macro (the same fallback test_std.c carries). */
#ifndef SYS_getrandom
#define SYS_getrandom 278
#endif

#define CSI "\033["

#define CLI_LINE_MAX 512
#define CLI_ARG_MAX  32
#define CLI_PATH_MAX 512

#define HIST_MAX  16
#define HIST_LINE 128

#define LS_MAX  512
#define LS_NAME 96

/* -------------------------------------------------------------------------
 * Console state
 * ---------------------------------------------------------------------- */
static int g_color;          /* stdout is a terminal: ANSI is wanted */
static int g_width = 80;     /* console grid, from TIOCGWINSZ when available */
static int g_rows  = 24;
static int g_quit;
static int g_commands;
static int g_errors;

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */
static void errf(const char* fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("test_cli: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Ask the host-rendered grid how wide it is. Fails quietly on a console with no
 * session attached (ENOTTY), which is exactly when the 24x80 default applies. */
static void term_query(void)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col >= 20) g_width = ws.ws_col;
        if (ws.ws_row >= 5)  g_rows  = ws.ws_row;
    }
}

static void rule(char c)
{
    if (g_width < 10) {
        return;
    }
    for (int i = 0; i < g_width - 1; i++) {
        putchar(c);
    }
    putchar('\n');
}

/* One byte at a time on purpose: the host's line discipline hands over a cooked
 * line, and stepping through it keeps EINTR (^C) observable between two bytes
 * instead of being swallowed by a stdio retry loop.
 *
 * Returns the line length, or -1 when a signal interrupted the read. g_eof is
 * set when the console is gone. */
static int g_eof;

static int read_line(char* buf, size_t cap)
{
    size_t n = 0;

    buf[0] = 0;
    g_eof = 0;

    for (;;) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);

        if (r < 0) {
            if (errno == EINTR) {
                return -1;
            }
            g_eof = 1;
            return (int)n;
        }
        if (r == 0) {
            g_eof = 1;
            return (int)n;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;                    /* CRLF, and lone CR from a host that sends it */
        }
        if (n + 1 < cap) {
            buf[n++] = c;
        }
        /* Longer than the buffer: the surplus is dropped, the line still ends. */
    }

    buf[n] = 0;
    return (int)n;
}

/* Split in place on whitespace. A single or double quote groups a token, so
 * `echo "two words"` reaches the echo command as one argument. No escapes: the
 * shell is a diagnostic, not a POSIX sh. */
static int tokenize(char* line, char** argv, int max)
{
    int argc = 0;
    char* p = line;

    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) {
                p++;
            }
            if (*p) {
                *p++ = 0;
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p++ = 0;
            }
        }
    }

    argv[argc] = NULL;
    return argc;
}

/* -------------------------------------------------------------------------
 * Command history (this session only, no file)
 * ---------------------------------------------------------------------- */
static char g_hist[HIST_MAX][HIST_LINE];
static int  g_hist_count;
static int  g_hist_next;

static void hist_push(const char* line)
{
    if (!line[0]) {
        return;
    }
    snprintf(g_hist[g_hist_next], HIST_LINE, "%s", line);
    g_hist_next = (g_hist_next + 1) % HIST_MAX;
    if (g_hist_count < HIST_MAX) {
        g_hist_count++;
    }
}

/* -------------------------------------------------------------------------
 * Command table
 * ---------------------------------------------------------------------- */
typedef int (*cmd_fn_t)(int argc, char** argv);

typedef struct {
    const char* name;
    const char* usage;
    const char* help;
    cmd_fn_t    fn;
} cmd_t;

static const cmd_t* find_cmd(const char* name);
static void list_commands(void);

/* -------------------------------------------------------------------------
 * Filesystem & process commands
 * ---------------------------------------------------------------------- */
static int cmd_pwd(int argc, char** argv)
{
    char cwd[CLI_PATH_MAX];

    (void)argc;
    (void)argv;

    if (!getcwd(cwd, sizeof(cwd))) {
        errf("pwd: getcwd: %s", strerror(errno));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int cmd_cd(int argc, char** argv)
{
    const char* dest = argc > 1 ? argv[1] : getenv("HOME");

    if (!dest) {
        errf("cd: no argument and HOME is not set");
        return 1;
    }
    if (chdir(dest)) {
        errf("cd: %s: %s", dest, strerror(errno));
        return 1;
    }
    /* Shells echo the new directory, but a host whose guest cwd is not inside
     * a mounted prefix cannot report one - that must not fail the cd. */
    (void)cmd_pwd(0, NULL);
    return 0;
}

/* Multi-column listing in the ls layout (fill down each column, then across),
 * so the output adapts to whatever grid the host is rendering. */
static int cmd_ls(int argc, char** argv)
{
    static char names[LS_MAX][LS_NAME];
    static char kinds[LS_MAX];
    const char* path = argc > 1 ? argv[1] : ".";
    int n = 0;

    DIR* dir = opendir(path);
    if (!dir) {
        errf("ls: %s: %s", path, strerror(errno));
        return 1;
    }

    struct dirent* de;
    while (n < LS_MAX && (de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        snprintf(names[n], LS_NAME, "%s", de->d_name);

        char full[CLI_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);

        struct stat st;
        if (stat(full, &st) == 0) {
            kinds[n] = S_ISDIR(st.st_mode) ? 'd' : (S_ISLNK(st.st_mode) ? 'l' : '-');
        } else {
            kinds[n] = '?';
        }
        n++;
    }
    closedir(dir);

    if (!n) {
        printf("(%s: empty)\n", path);
        return 0;
    }

    size_t widest = 0;
    for (int i = 0; i < n; i++) {
        size_t w = strlen(names[i]) + (kinds[i] == 'd' ? 1 : 0);
        if (w > widest) {
            widest = w;
        }
    }

    size_t colw = widest + 2;
    int cols = (int)((size_t)g_width / colw);
    if (cols < 1) {
        cols = 1;
    }
    if (cols > n) {
        cols = n;
    }
    int rows = (n + cols - 1) / cols;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r;
            size_t used;

            if (i >= n) {
                continue;
            }
            if (g_color && kinds[i] == 'd') {
                fputs(CSI "1;34m", stdout);
            } else if (g_color && kinds[i] == 'l') {
                fputs(CSI "1;36m", stdout);
            }
            printf("%s%s", names[i], kinds[i] == 'd' ? "/" : "");
            if (g_color) {
                fputs(CSI "0m", stdout);
            }

            used = strlen(names[i]) + (kinds[i] == 'd' ? 1 : 0);
            /* Pad only when another entry follows on this screen row. */
            if ((c + 1) * rows + r < n) {
                for (size_t k = used; k < colw; k++) {
                    putchar(' ');
                }
            }
        }
        putchar('\n');
    }

    if (n == LS_MAX) {
        printf("(listing truncated at %d entries)\n", LS_MAX);
    }
    return 0;
}

static int cmd_cat(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("cat: missing file operand");
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_RDONLY);
        if (fd < 0) {
            errf("cat: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }

        fflush(stdout);                  /* keep ordering against the raw writes */

        char buf[1024];
        int last = '\n';
        ssize_t r;

        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            if (write(STDOUT_FILENO, buf, (size_t)r) != r) {
                errf("cat: %s: write failed: %s", argv[a], strerror(errno));
                rc = 1;
                break;
            }
            last = buf[r - 1];
        }
        if (r < 0) {
            errf("cat: %s: %s", argv[a], strerror(errno));
            rc = 1;
        }
        close(fd);

        if (last != '\n') {
            putchar('\n');               /* do not glue the next prompt onto the file */
        }
    }
    return rc;
}

static int cmd_stat(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("stat: missing operand");
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        struct stat st;
        if (stat(argv[a], &st)) {
            errf("stat: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }

        const char* type =
            S_ISDIR(st.st_mode)  ? "directory" :
            S_ISLNK(st.st_mode)  ? "symlink"   :
            S_ISREG(st.st_mode)  ? "regular"   :
            S_ISCHR(st.st_mode)  ? "char"      :
            S_ISBLK(st.st_mode)  ? "block"     :
            S_ISFIFO(st.st_mode) ? "fifo"      :
            S_ISSOCK(st.st_mode) ? "socket"    : "unknown";

        printf("%s\n", argv[a]);
        printf("  type   : %s (mode %04o)\n", type, (unsigned)(st.st_mode & 07777));
        printf("  size   : %lld bytes (%lld blocks of 512)\n",
               (long long)st.st_size, (long long)st.st_blocks);
        printf("  inode  : %llu   hard links: %lu\n",
               (unsigned long long)st.st_ino, (unsigned long)st.st_nlink);
        printf("  owner  : uid %u gid %u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
    }
    return rc;
}

/* -------------------------------------------------------------------------
 * Filesystem mutation
 *
 * Each of these is also the probe for a path syscall that a read-only shell
 * never reaches: mkdirat, unlinkat/rmdir, renameat2, symlinkat, readlinkat,
 * openat with O_CREAT, and truncate. The emulator's path mapping is what they
 * pin down - a wrong cwd or a wrong dirfd shows up here first.
 * ---------------------------------------------------------------------- */
static int cmd_mkdir(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("mkdir: missing operand");
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        if (mkdir(argv[a], 0755)) {
            errf("mkdir: %s: %s", argv[a], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_touch(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("touch: missing operand");
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            errf("touch: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}

static int cmd_rm(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("rm: missing operand");
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        struct stat st;
        /* lstat, not stat: a symlink is removed as the link itself, and a
         * dangling one has no target type to ask about. */
        if (lstat(argv[a], &st)) {
            errf("rm: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }
        /* unlink() refuses directories and rmdir() refuses files, so the type
         * decides which of the two syscalls this is. */
        if ((S_ISDIR(st.st_mode) ? rmdir(argv[a]) : unlink(argv[a]))) {
            errf("rm: %s: %s", argv[a], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_mv(int argc, char** argv)
{
    if (argc != 3) {
        errf("mv: usage: mv <src> <dst>");
        return 1;
    }
    if (rename(argv[1], argv[2])) {
        errf("mv: %s -> %s: %s", argv[1], argv[2], strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_ln(int argc, char** argv)
{
    if (argc != 3) {
        errf("ln: usage: ln <target> <linkpath>");
        return 1;
    }
    if (symlink(argv[1], argv[2])) {
        errf("ln: %s -> %s: %s", argv[2], argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_readlink(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("readlink: missing operand");
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        char buf[CLI_PATH_MAX];
        ssize_t n = readlink(argv[a], buf, sizeof(buf) - 1);

        if (n < 0) {
            errf("readlink: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }
        buf[n] = 0;
        printf("%s\n", buf);
    }
    return rc;
}

static int cmd_truncate(int argc, char** argv)
{
    if (argc != 3) {
        errf("truncate: usage: truncate <file> <size>");
        return 1;
    }

    char* end = NULL;
    errno = 0;
    long long size = strtoll(argv[2], &end, 0);
    if (errno || !end || *end || size < 0) {
        errf("truncate: bad size '%s'", argv[2]);
        return 1;
    }
    if (truncate(argv[1], (off_t)size)) {
        errf("truncate: %s: %s", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Bundled assets
 *
 * The guest's AAssetManager_* path reaches files inside the host's asset tree
 * rather than the guest's file system - the one thing no POSIX open() can do,
 * because an asset lives inside the APK. The length/read/seek below therefore
 * exercise the whole asset ABI: ask for the size, get the bytes, then treat
 * them as a random-access copy.
 *
 * The manager argument is ignored: VirtPass has one asset tree per guest and
 * the host owns it. An Android-shaped app takes the non-NULL placeholder from
 * android_app::assetManager.
 * ---------------------------------------------------------------------- */
static int cmd_asset(int argc, char** argv)
{
    int rc = 0;

    if (argc < 2) {
        errf("asset: usage: asset <name...>");
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        AAsset* asset = AAssetManager_open(NULL, argv[a], AASSET_MODE_BUFFER);
        if (!asset) {
            errf("asset: %s: %s", argv[a], strerror(errno));
            rc = 1;
            continue;
        }

        int64_t len = AAsset_getLength64(asset);
        printf("%s: %lld byte(s)\n", argv[a], (long long)len);

        unsigned char head[16];
        int got = AAsset_read(asset, head, sizeof(head));
        if (got > 0) {
            printf("  head:");
            for (int i = 0; i < got; i++) {
                printf(" %02x", head[i]);
            }
            putchar('\n');
        }

        /* A seek away from the cursor proves it is a random-access copy and
         * not a stream that only ever moves forward. */
        if (len > 0 && AAsset_seek(asset, -1, SEEK_END) >= 0) {
            unsigned char tail = 0;
            if (AAsset_read(asset, &tail, 1) == 1) {
                printf("  tail: %02x\n", tail);
            }
        }

        /* Direct access: a descriptor to mmap/pread, or a refusal. A refusal is a
         * valid answer - it depends on whether the host can address the asset at
         * all - so it is reported rather than counted as a failure. A descriptor
         * that does come back is checked instead of trusted: it has to describe
         * this asset's bytes at the offset it named, or it is worse than none. */
        {
            off_t start = -1, extent = -1;
            int fd = AAsset_openFileDescriptor(asset, &start, &extent);
            if (fd < 0) {
                printf("  fdesc: unavailable (%s)\n", strerror(errno));
            } else {
                const unsigned char* copy = AAsset_getBuffer(asset);
                /* pread(), not read(): the question is what the descriptor holds
                 * at the offset it named, not where its cursor happens to be. */
                unsigned char* via_fd = extent > 0 ? malloc((size_t)extent) : NULL;
                ssize_t got = via_fd ? pread(fd, via_fd, (size_t)extent, start) : -1;

                printf("  fdesc: fd=%d start=%lld len=%lld\n",
                       fd, (long long)start, (long long)extent);

                if (extent != len) {
                    errf("asset: %s: descriptor covers %lld byte(s), the asset is %lld",
                         argv[a], (long long)extent, (long long)len);
                    rc = 1;
                } else if (extent > 0 && (!via_fd || !copy)) {
                    errf("asset: %s: out of memory checking the descriptor", argv[a]);
                    rc = 1;
                } else if (extent > 0 && got != (ssize_t)extent) {
                    errf("asset: %s: descriptor gave %lld of %lld byte(s)",
                         argv[a], (long long)got, (long long)extent);
                    rc = 1;
                } else if (extent > 0 && memcmp(via_fd, copy, (size_t)extent) != 0) {
                    errf("asset: %s: descriptor bytes differ from the asset", argv[a]);
                    rc = 1;
                } else {
                    printf("  fdesc: content matches (%lld byte(s))\n",
                           (long long)extent);
                }
                free(via_fd);
                close(fd);
            }
        }

        AAsset_close(asset);
    }
    return rc;
}

/* Deliberately leaks the descriptors: they stay open until the run ends. This is
 * the regression probe for the core's run-end reap of the asset mount - a host
 * whose open_fd() hangs background work off the descriptor (the Android stream
 * pump) must not be left with it alive once the guest is gone, and that sweep is
 * the only thing that closes these.
 *
 * Point it at a resource the host cannot buffer whole (larger than a pipe, on the
 * Android host) or the probe is worthless: the host's pump then finishes feeding,
 * sees EOF and exits on its own, so nothing was ever waiting on the descriptor and
 * only its number leaks. */
static int cmd_hold(int argc, char** argv)
{
    int held = 0;

    if (argc < 2) {
        errf("hold: usage: hold <path...>");
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_RDONLY);
        if (fd < 0) {
            errf("hold: %s: %s", argv[a], strerror(errno));
            return 1;
        }
        held++;                 /* no close(): that is the whole point */
    }

    printf("held %d descriptor(s), exiting without closing\n", held);
    return 0;
}

static int cmd_id(int argc, char** argv)
{
    char cwd[CLI_PATH_MAX];

    (void)argc;
    (void)argv;

    printf("uid=%u euid=%u gid=%u egid=%u pid=%d\n",
           (unsigned)getuid(), (unsigned)geteuid(),
           (unsigned)getgid(), (unsigned)getegid(), (int)getpid());

    if (getcwd(cwd, sizeof(cwd))) {
        printf("cwd=%s\n", cwd);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Host identity, time and environment
 * ---------------------------------------------------------------------- */
static int cmd_uname(int argc, char** argv)
{
    struct utsname u;

    (void)argc;
    (void)argv;

    if (uname(&u)) {
        errf("uname: %s", strerror(errno));
        return 1;
    }
    printf("%s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    printf("nodename: %s\n", u.nodename);
    return 0;
}

static int cmd_date(int argc, char** argv)
{
    struct timespec ts;
    struct tm tmv;
    char stamp[64];

    (void)argc;
    (void)argv;

    if (clock_gettime(CLOCK_REALTIME, &ts)) {
        errf("date: clock_gettime: %s", strerror(errno));
        return 1;
    }

    /* UTC explicitly: the guest has no /etc/localtime, and a wrong local zone
     * would be reported as a bridge failure. gmtime_r never fails here. */
    time_t sec = (time_t)ts.tv_sec;
    if (!gmtime_r(&sec, &tmv) ||
        !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv)) {
        errf("date: cannot format the wall clock");
        return 1;
    }

    printf("%s UTC  (epoch %lld, +%09ld ns)\n", stamp,
           (long long)ts.tv_sec, ts.tv_nsec);
    return 0;
}

extern char** environ;

static int cmd_env(int argc, char** argv)
{
    if (argc > 1) {
        const char* value = getenv(argv[1]);
        if (!value) {
            printf("%s is not set\n", argv[1]);
            return 1;
        }
        printf("%s=%s\n", argv[1], value);
        return 0;
    }

    if (!environ) {
        printf("(no environment)\n");
        return 0;
    }

    int shown = 0;
    for (char** e = environ; *e; e++) {
        if (shown >= 200) {
            printf("... (truncated, more than %d variables)\n", shown);
            break;
        }
        printf("%s\n", *e);
        shown++;
    }
    if (!shown) {
        printf("(no environment)\n");
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Arithmetic, entropy and timeouts
 * ---------------------------------------------------------------------- */
static int cmd_calc(int argc, char** argv)
{
    if (argc != 4) {
        errf("calc: usage: calc <a> <op> <b>");
        return 1;
    }

    char* end = NULL;
    long long a, b;

    errno = 0;
    a = strtoll(argv[1], &end, 0);       /* base 0: 42, 0x2a and 052 all work */
    if (errno || !end || *end || end == argv[1]) {
        errf("calc: '%s' is not an integer", argv[1]);
        return 1;
    }

    errno = 0;
    b = strtoll(argv[3], &end, 0);
    if (errno || !end || *end || end == argv[3]) {
        errf("calc: '%s' is not an integer", argv[3]);
        return 1;
    }

    const char* op = argv[2];
    long long r;

    if (!strcmp(op, "+")) {
        r = a + b;
    } else if (!strcmp(op, "-")) {
        r = a - b;
    } else if (!strcmp(op, "*")) {
        r = a * b;
    } else if (!strcmp(op, "/")) {
        if (!b) {
            errf("calc: division by zero");
            return 1;
        }
        r = a / b;
    } else if (!strcmp(op, "%")) {
        if (!b) {
            errf("calc: modulo by zero");
            return 1;
        }
        r = a % b;
    } else if (!strcmp(op, "&")) {
        r = a & b;
    } else if (!strcmp(op, "|")) {
        r = a | b;
    } else if (!strcmp(op, "^")) {
        r = a ^ b;
    } else if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
        if (b < 0 || b > 63) {
            errf("calc: shift count %lld is outside 0..63", b);
            return 1;
        }
        r = !strcmp(op, "<<") ? (long long)((unsigned long long)a << b) : (a >> b);
    } else {
        errf("calc: unknown operator '%s' (try + - * / %% & | ^ << >>)", op);
        return 1;
    }

    printf("%lld %s %lld = %lld  (0x%llx)\n", a, op, b, r, (unsigned long long)r);
    return 0;
}

static int cmd_rand(int argc, char** argv)
{
    long bytes = 16;
    unsigned char buf[64];

    if (argc > 1) {
        char* end = NULL;
        errno = 0;
        bytes = strtol(argv[1], &end, 0);
        if (errno || !end || *end || bytes < 1) {
            errf("rand: bad byte count '%s'", argv[1]);
            return 1;
        }
        if (bytes > (long)sizeof(buf)) {
            bytes = (long)sizeof(buf);
        }
    }

    /* getrandom(2) is the honest source; a host without it gets a
     * clock-seeded xorshift so the command still produces something useful
     * instead of an error. */
    long got = syscall(SYS_getrandom, buf, (size_t)bytes, 0);
    int fallback = got < 0;

    if (fallback) {
        struct timespec ts;
        unsigned long long s = (unsigned long long)getpid();

        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            s ^= (unsigned long long)ts.tv_sec * 1000000000ull;
            s ^= (unsigned long long)ts.tv_nsec;
        }
        s |= 1ull;                       /* xorshift must not start at 0 */
        for (long i = 0; i < bytes; i++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            buf[i] = (unsigned char)(s >> 24);
        }
        got = bytes;
    }

    printf("%ld byte(s)%s:\n", bytes,
           fallback ? " (getrandom unavailable, clock-seeded fallback)" : " from getrandom");

    for (long i = 0; i < got; i++) {
        printf("%02x", buf[i]);
        if ((i & 15) == 15) {
            putchar('\n');
        } else if ((i & 3) == 3) {
            putchar(' ');
        }
    }
    if (got & 15) {
        putchar('\n');
    }
    return 0;
}

static int cmd_sleep(int argc, char** argv)
{
    if (argc < 2) {
        errf("sleep: usage: sleep <milliseconds>");
        return 1;
    }

    char* end = NULL;
    errno = 0;
    long ms = strtol(argv[1], &end, 0);
    if (errno || !end || *end || ms < 0) {
        errf("sleep: bad duration '%s'", argv[1]);
        return 1;
    }

    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;

    int interrupted = 0;
    if (nanosleep(&req, NULL) != 0) {
        if (errno != EINTR) {
            errf("sleep: nanosleep: %s", strerror(errno));
            return 1;
        }
        interrupted = 1;              /* ^C: worth reporting, but not an error */
    }

    printf("slept %ld ms%s\n", ms, interrupted ? " (interrupted)" : "");
    return 0;
}

/* -------------------------------------------------------------------------
 * Console commands
 * ---------------------------------------------------------------------- */
static int cmd_tty(int argc, char** argv)
{
    struct winsize ws;

    (void)argc;
    (void)argv;

    /* Take the ioctl reading (and its errno) before isatty() clobbers errno. */
    memset(&ws, 0, sizeof(ws));
    errno = 0;
    int have_size = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0;
    int size_err = errno;

    printf("stdin : %s\n", isatty(STDIN_FILENO) ? "terminal" : "not a terminal");
    printf("stdout: %s\n", isatty(STDOUT_FILENO) ? "terminal" : "not a terminal");
    if (have_size) {
        printf("grid  : %u rows x %u cols\n", (unsigned)ws.ws_row, (unsigned)ws.ws_col);
    } else {
        printf("grid  : unknown (TIOCGWINSZ failed: %s)\n", strerror(size_err));
        printf("        the shell is assuming %dx%d\n", g_rows, g_width);
    }
    return 0;
}

static int cmd_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    fputs(CSI "2J" CSI "1;1H", stdout);  /* erase display, home the cursor */
    fflush(stdout);
    return 0;
}

static int cmd_history(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (!g_hist_count) {
        printf("(no commands yet)\n");
        return 0;
    }
    for (int i = 0; i < g_hist_count; i++) {
        int idx = (g_hist_next - g_hist_count + i + HIST_MAX) % HIST_MAX;
        printf("%4d  %s\n", i + 1, g_hist[idx]);
    }
    return 0;
}

static int cmd_exit(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    g_quit = 1;
    return 0;
}

static int cmd_help(int argc, char** argv)
{
    if (argc > 1) {
        const cmd_t* c = find_cmd(argv[1]);
        if (!c) {
            errf("help: no such command '%s'", argv[1]);
            return 1;
        }
        printf("%s %s\n", c->name, c->usage);
        printf("  %s\n", c->help);
        return 0;
    }

    printf("Commands ('<arg>' required, '[arg]' optional):\n");
    list_commands();
    printf("\nAliases: '?' = help, 'quit' = exit. '^C' abandons the current line.\n");
    return 0;
}

static int cmd_echo(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            putchar(' ');
        }
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

/* -------------------------------------------------------------------------
 * Table & dispatch
 * ---------------------------------------------------------------------- */
static const cmd_t k_cmds[] = {
    { "help",    "[command]",      "list the commands, or describe one",       cmd_help    },
    { "echo",    "<text...>",      "print the arguments back",                 cmd_echo    },
    { "pwd",     "",               "print the guest working directory",        cmd_pwd     },
    { "cd",      "<dir>",          "change the guest working directory",       cmd_cd      },
    { "ls",      "[dir]",          "list a directory in columns",              cmd_ls      },
    { "cat",     "<file...>",      "print files to the console",               cmd_cat     },
    { "stat",    "<path...>",      "show type, mode, size and inode",          cmd_stat    },
    { "mkdir",   "<dir...>",       "create directories",                       cmd_mkdir   },
    { "touch",   "<file...>",      "create empty files",                       cmd_touch   },
    { "rm",      "<path...>",      "remove files or empty directories",        cmd_rm      },
    { "mv",      "<src> <dst>",    "rename a file or directory",                cmd_mv      },
    { "ln",      "<target> <link>", "create a symbolic link",                   cmd_ln      },
    { "readlink", "<path...>",     "print the target of a symbolic link",      cmd_readlink },
    { "truncate", "<file> <size>", "resize a file",                            cmd_truncate },
    { "hold",    "<path...>",      "open files and exit without closing them", cmd_hold    },
    { "asset",   "<name...>",      "read files from the host's asset tree",    cmd_asset   },
    { "id",      "",               "guest uid/gid/pid and cwd",                cmd_id      },
    { "uname",   "",               "guest kernel identity from the host",      cmd_uname   },
    { "date",    "",               "wall clock in UTC and epoch seconds",      cmd_date    },
    { "env",     "[name]",         "list the environment, or one variable",    cmd_env     },
    { "calc",    "<a> <op> <b>",   "integer arithmetic (+ - * / % & | ^ << >>)", cmd_calc  },
    { "rand",    "[bytes]",        "draw entropy from the host (1..64)",       cmd_rand    },
    { "sleep",   "<ms>",           "sleep for a number of milliseconds",       cmd_sleep   },
    { "tty",     "",               "terminal size and isatty() probes",        cmd_tty     },
    { "clear",   "",               "clear the virtual terminal",               cmd_clear   },
    { "history", "",               "the commands entered in this session",     cmd_history },
    { "exit",    "",               "leave the shell (also 'quit')",            cmd_exit    },
};

static void list_commands(void)
{
    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        printf("  %-9s %-15s %s\n",
               k_cmds[i].name, k_cmds[i].usage, k_cmds[i].help);
    }
}

static const cmd_t* find_cmd(const char* name)
{
    if (!strcmp(name, "quit")) {
        name = "exit";
    } else if (!strcmp(name, "?")) {
        name = "help";
    }

    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        if (!strcmp(k_cmds[i].name, name)) {
            return &k_cmds[i];
        }
    }
    return NULL;
}

static int dispatch(int argc, char** argv)
{
    if (!argc) {
        return 0;
    }

    const cmd_t* c = find_cmd(argv[0]);
    if (!c) {
        errf("unknown command '%s' - type 'help'", argv[0]);
        return 1;
    }
    return c->fn(argc, argv);
}

/* -------------------------------------------------------------------------
 * Session
 * ---------------------------------------------------------------------- */
static volatile sig_atomic_t g_signals;

static void on_int(int sig)
{
    (void)sig;
    /* Async-signal-safe: nothing but a flag. The interrupted read(0) returns
     * EINTR, the loop below notices and re-prompts - the run keeps going, the
     * same in-guest delivery test_sigint.c pins down. */
    g_signals++;
}

static void prompt(void)
{
    if (g_color) {
        fputs(CSI "1;36mvp>" CSI "0m ", stdout);
    } else {
        fputs("vp> ", stdout);
    }
    fflush(stdout);
}

/* Non-interactive: the arguments are a command line (or several, separated by
 * ';'), run exactly as the shell would run them. A leading "-c" is dropped, so
 * `test_cli -c "cat /assets/x"` and `test_cli cat /assets/x` are the same thing -
 * the form an `am start --esa argv ...` invocation wants.
 *
 * The process exits with the number of failed commands, which the host logs as
 * the guest's exit code - so a device test needs no console and no eyeballing:
 * one am start, then read the exit code and the log. */
static int run_command_line(int argc, char** argv)
{
    static char  line[CLI_LINE_MAX];
    static char* args[CLI_ARG_MAX];
    size_t o = 0;
    int first = 1;
    int errors = 0;
    char* seg;

    if (!strcmp(argv[1], "-c") || !strcmp(argv[1], "--command")) {
        first = 2;
    }

    line[0] = 0;
    for (int i = first; i < argc; i++) {
        if (i > first && o + 1 < sizeof(line)) {
            line[o++] = ' ';
        }
        for (const char* p = argv[i]; *p && o + 1 < sizeof(line); p++) {
            line[o++] = *p;
        }
    }
    line[o] = 0;

    if (!line[0]) {
        errf("no command given");
        return 2;
    }

    /* ';' separates commands so one host invocation can probe several things,
     * which is what keeps a device test to a single `am start`. */
    seg = line;
    while (seg) {
        char* next = strchr(seg, ';');
        int n;

        if (next) {
            *next = 0;
        }
        n = tokenize(seg, args, CLI_ARG_MAX);
        if (n && dispatch(n, args)) {
            errors++;
        }
        seg = next ? next + 1 : NULL;
    }
    return errors;
}

int main(int argc, char** argv)
{
    static char  line[CLI_LINE_MAX];
    static char* cmd_argv[CLI_ARG_MAX];

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Arguments mean "run this and exit", not "start a session": no prompt, no
     * console input and no ANSI - the output is read by the host, not a
     * terminal. */
    if (argc > 1) {
        return run_command_line(argc, argv);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);

    term_query();

    /* A console without a session (guest output piped into a log) reports
     * ENOTTY, so color is left out and the log stays readable. */
    g_color = isatty(STDOUT_FILENO);

    int interactive = isatty(STDIN_FILENO);

    printf("RVVM VirtPass guest shell (test_cli)\n");
    printf("console %dx%d | interactive input %s\n",
           g_rows, g_width, interactive ? "on" : "off (stdin is not a terminal)");
    rule('=');
    printf("Type 'help' for the command list, 'exit' to leave.\n");

    while (!g_quit) {
        if (interactive) {
            prompt();
        }

        int len = read_line(line, sizeof(line));
        if (len < 0) {
            /* ^C: the handler has run, drop the partial line and re-prompt. */
            printf("%s^C%s\n", g_color ? CSI "1;33m" : "", g_color ? CSI "0m" : "");
            continue;
        }
        if (len == 0 && g_eof) {
            if (interactive) {
                printf("\n");
            }
            printf("(console closed)\n");
            break;
        }
        if (!line[0]) {
            continue;
        }

        /* A script piped in from a Windows editor may start with a UTF-8 BOM;
         * without this the first command reads as an unknown one. */
        char* cmdline = line;
        size_t clen = strlen(cmdline);
        if (clen >= 3 &&
            (unsigned char)cmdline[0] == 0xEF &&
            (unsigned char)cmdline[1] == 0xBB &&
            (unsigned char)cmdline[2] == 0xBF) {
            cmdline += 3;
        }

        hist_push(cmdline);

        int ncmd = tokenize(cmdline, cmd_argv, CLI_ARG_MAX);
        g_commands++;
        if (dispatch(ncmd, cmd_argv)) {
            g_errors++;
        }
    }

    rule('-');
    printf("session: %d command(s), %d error(s), %d signal(s)\n",
           g_commands, g_errors, (int)g_signals);

    fflush(stdout);

    /* Interactive use is not a test: a mistyped command must not fail the
     * process. Replaying a script (stdin redirected) does report failures. */
    return (interactive || !g_errors) ? 0 : 1;
}
