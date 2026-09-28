/*
 * test_busybox.c - hand the console to the guest's own busybox
 *
 * What it is for
 * --------------
 * test_cli is a shell with a command table of its own: it reads, writes, lists
 * and stats, and it deliberately never forks or execs. That is the right shape
 * for probing the console path and the wrong shape for anything that has to
 * *run* a program - installing a package, starting a daemon, driving a pipe.
 *
 * The guest rootfs is an Alpine minirootfs, so it already carries busybox, and
 * busybox already carries a shell and every applet the base system uses. This
 * sample is the handover: the console goes to that shell instead of exec being
 * reimplemented on top of a command table. It is deliberately tiny - the work
 * belongs to busybox, and the interesting part is the fork/exec, the PATH
 * lookup and the exit status underneath it.
 *
 *   test_busybox                 an interactive `busybox sh` on the console
 *   test_busybox ls -l /         `busybox ls -l /`, and exit with its status
 *   test_busybox --list          `busybox --list`: every applet, as a self-check
 *   test_busybox -c "<line>"     `busybox sh -c "<line>"` - the form a host drives
 *
 * busybox picks its applet from argv[1] (argv[0] is only the name to report),
 * which is why nothing here has to resolve a program name: `ls` above is an
 * applet, not a path, and PATH is only consulted for what the shell itself
 * starts.
 *
 * The last form is the one a device test wants, because a real sh brings
 * quoting, pipes and redirection with it:
 *
 *   adb shell am start -n com.rvvm.android/.MainActivity \
 *     --es guest test_busybox --esa argv "-c,apk add --no-cache openssh"
 *
 * (test_cli documents the same host-driven shape for its own commands; this
 * exists because `apk` and a daemon need a shell, and a host that has to type
 * a shell command into a console one character at a time is not a test.)
 *
 * PATH is the one thing worth getting right here, and it is inherited
 * unchanged unless it is unusable. The host environ is passed down into the
 * guest, so PATH can arrive as a Windows path ("C:\Program Files\...") or as
 * nothing at all - and then the shell reports "not found" for a program that
 * exists, which reads like a missing applet rather than a missing environment.
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUSYBOX   "/bin/busybox"
#define PATH_MAX  1024
#define LINE_MAX  1024
#define ARG_MAX   64

/* PATH repair.
 *
 * The host environ is passed down into the guest, so PATH arrives naming
 * directories that are not in the guest's rootfs at all - a Windows one
 * ("C:\Program Files\...") on the win32 host, an Android one
 * ("/sbin:/system/bin:/vendor/bin") on a device. Either way the shell then
 * reports "not found" for a program that exists, which reads like a missing
 * applet rather than a missing environment.
 *
 * The guest's own directories go *in front* rather than replacing what came:
 * the inherited entries cannot resolve in this guest, so leaving them first is
 * what loses the lookup, while keeping them behind costs nothing and preserves
 * what the host passed for a test that wants to see it (`env` prints it). */

#define GUEST_PATH "/sbin:/usr/sbin:/bin:/usr/bin"

static void repair_path(void)
{
    const char* inherited = getenv("PATH");
    char        buf[PATH_MAX];
    size_t      used;

    if (inherited && *inherited) {
        int n = snprintf(buf, sizeof(buf), "%s:%s", GUEST_PATH, inherited);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            setenv("PATH", buf, 1);
            return;
        }
    }
    /* Nothing usable came down, or the join did not fit: the guest's own list
     * is a complete answer on its own. */
    setenv("PATH", GUEST_PATH, 1);
}

/* Replace the console with busybox. @applet is the argument vector from index
 * 0 on, already chosen; only an exec failure returns. */
static int exec_busybox(char** applet)
{
    static char* argv[ARG_MAX + 1];
    int n = 0;

    argv[n++] = (char*)"busybox";     /* argv[0]: the name busybox reports */
    for (int i = 0; applet[i] && n < ARG_MAX; i++) {
        argv[n++] = applet[i];
    }
    argv[n] = NULL;

    execv(BUSYBOX, argv);

    /* 127 is what a shell reports for a command it could not run, which is
     * exactly what a failed exec is. */
    fprintf(stderr, "test_busybox: %s: %s\n", BUSYBOX, strerror(errno));
    if (errno == ENOENT) {
        fprintf(stderr, "test_busybox: the guest rootfs has no busybox at %s - "
                        "the bundle's /bin/busybox is missing\n", BUSYBOX);
    }
    return 127;
}

int main(int argc, char** argv)
{
    static char  line[LINE_MAX];
    static char* applet[ARG_MAX];
    int          n = 0;

    /* Everything below hands the console to a program that looks commands up in
     * PATH, so this comes first. */
    repair_path();

    if (argc > 1 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "--command"))) {
        if (argc < 3) {
            fprintf(stderr, "test_busybox: -c needs a command line\n");
            return 2;
        }
        /* Everything after -c is one command line, joined back up: a host hands
         * it over as several argv elements (am start --esa argv), and the shell
         * is what does the splitting - once, properly, with quoting. */
        size_t used = 0;
        line[0] = 0;
        for (int i = 2; i < argc; i++) {
            size_t len = strlen(argv[i]);
            if (used && used + 1 < sizeof(line)) {
                line[used++] = ' ';
            }
            if (used + len + 1 > sizeof(line)) {
                len = sizeof(line) - used - 1;
            }
            memcpy(line + used, argv[i], len);
            used += len;
            line[used] = 0;
        }
        applet[n++] = (char*)"sh";
        applet[n++] = (char*)"-c";
        applet[n++] = line;
    } else if (argc > 1) {
        for (int i = 1; i < argc && n < ARG_MAX; i++) {
            applet[n++] = argv[i];
        }
    } else {
        applet[n++] = (char*)"sh";    /* interactive: the console is a terminal */
    }

    return exec_busybox(applet);
}
