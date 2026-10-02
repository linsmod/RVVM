/*
ash_main.c - rvvm_ash: the core host

A core is one run - one machine, one rootfs - kept up so clients come and go
against it. The core program is the bundle's /sbin/idle, and the host drives a
terminal per client out of the machine's own pty pool (vp_core.c); the guest
forks a shell onto each. "The run is the boundary, a session is the client's."

Clients attach over the guest console, which this host serves as an adb-shaped
byte pipe on a loopback port (vp_console.c). That is `vp`: `vp --local -P N
exec-out "ls"` runs one command, `vp --local -P N shell` is a terminal - the
same client and the same protocol the Android host speaks.

The old `ash` *client* and the guest-side session server it talked to
(/sbin/vpsessiond over AF_UNIX) are retired. init= can still boot that legacy
program, but nothing serves its wire any more, so it is for the curious rather
than for a workflow.

There is no "boot a loose ELF on this console" mode: a core program is a
host-owned system program, and an application is booted as an app package
(rvvm_winhost --app <id>). Debugging is RVVM_TRACE / RVVM_VERBOSE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_cmdpost_bridge.h"
#include "ash_core.h" /* the --serve exit codes, and the prototypes below */
#include "utils.h" /* rvvm_set_loglevel: RVVM_VERBOSE, as the core honours it */

#define ASH_PORT_DEFAULT 7900

static int ash_port(void)
{
    const char* env = getenv("RVVM_ASH_PORT");
    if (env && *env) {
        int p = atoi(env);
        if (p > 0 && p <= 65535) {
            return p;
        }
    }
    return ASH_PORT_DEFAULT;
}

static void print_usage(const char* self)
{
    fprintf(stderr,
            "usage: %s --serve [--idle S] [--dlog <guest-path>] [--memfs]\n"
            "          [--cmdline <linux-args>] [--port N]\n"
            "       %s --list\n"
            "\n"
            "  %s --serve          run the core: one run (one machine, one rootfs)\n"
            "                      kept up, so `vp` clients attach a session each.\n"
            "                      The core program is the bundle's /sbin/idle;\n"
            "                      init= can still boot the legacy guest-side model\n"
            "                      (/sbin/vpsessiond), but nothing serves it.\n"
            "  %s --list           list the cores registered in this release tree\n"
            "\n"
            "Options: --port N (default %d, RVVM_ASH_PORT) is the core's id and the\n"
            "loopback port a `vp` client attaches to. --serve --idle S stops a\n"
            "legacy (init=/sbin/vpsessiond) core after S seconds with no session;\n"
            "the idle core does not honour it. --serve --dlog <path> puts the\n"
            "legacy session server's log at that guest path. --serve --memfs runs\n"
            "the bundle from memory instead of <exe>\\runtime\\rootfs.\n"
            "\n"
            "--cmdline <args> boots the core's machine with those Linux-style boot\n"
            "arguments, and is the only way to change what the run is made of:\n"
            "  init=<guest-path>  the core program; anything but /sbin/idle selects\n"
            "                     the legacy guest-side model\n"
            "  root=<host-path>   the run's filesystem prefix, instead of the bundle\n"
            "  loglevel=<name|n>  none/error/warn/info/debug, or the kernel's 0..4\n"
            "  debug              the same as loglevel=debug\n"
            "Whatever is passed is also what the guest reads back from /proc/cmdline.\n"
            "Arguments this build does not know are kept, not refused.\n"
            "\n"
            "A core serves the guest console as an adb-shaped byte pipe, so it is\n"
            "driven by `vp`:\n"
            "  vp --local -P %d exec-out \"ls\"    one command, its output and status\n"
            "  vp --local -P %d shell             a terminal for a new session\n"
            "\n"
            "Host diagnostics (RVVM_VERBOSE=1) go to stderr, never to this process's\n"
            "stdout. RVVM_LOG_FILE=<path> appends them to a file instead, and\n"
            "RVVM_LOG_RING_DUMP=<path> writes the log ring at exit.\n",
            self, self, self, self, ASH_PORT_DEFAULT,
            ASH_PORT_DEFAULT, ASH_PORT_DEFAULT);
}

int main(int argc, char** argv)
{
    bool        serve   = false;
    bool        list    = false;
    bool        memfs   = false;
    const char* dlog    = NULL;
    const char* cmdline = NULL;
    int         idle    = 0;
    int         port    = ash_port();
    int         i       = 1;

    for (; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(a, "--serve")) {
            serve = true;
        } else if (!strcmp(a, "--list")) {
            list = true;
        } else if (!strcmp(a, "--memfs")) {
            /* The bundle goes into the run's own memory filesystem rather than
             * being materialized under <exe>\runtime\rootfs: a run that leaves
             * nothing behind, whose / dies with it. The guest still gets the
             * release's rootfs and system programs - installed into memory - so
             * busybox and the core programs are there; what is different is
             * where. */
            memfs = true;
        } else if (!strcmp(a, "--dlog") && i + 1 < argc) {
            dlog = argv[++i];
        } else if (!strncmp(a, "--dlog=", 7)) {
            dlog = a + 7;
        } else if (!strcmp(a, "--idle") && i + 1 < argc) {
            idle = atoi(argv[++i]);
        } else if (!strncmp(a, "--idle=", 7)) {
            idle = atoi(a + 7);
        } else if (!strcmp(a, "--cmdline") && i + 1 < argc) {
            cmdline = argv[++i];
        } else if (!strncmp(a, "--cmdline=", 10)) {
            cmdline = a + 10;
        } else if (!strcmp(a, "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strncmp(a, "--port=", 7)) {
            port = atoi(a + 7);
        } else if (!strcmp(a, "--shutdown") || !strcmp(a, "--sock-path") ||
                   !strcmp(a, "--no-autostart")) {
            /* Verbs (and a flag) of the client era. The AF_UNIX endpoint and
             * the client that spoke to it went with the guest-side session
             * model: a core is stopped the way any console process is - Ctrl+C,
             * or ending it - and a machine is used through `vp`. */
            fprintf(stderr,
                    "%s: '%s' belonged to the ash client, which is gone. A core is\n"
                    "    served over the `vp` byte pipe now: drive it with `vp --local`,\n"
                    "    and stop it with Ctrl+C (or by ending this process).\n",
                    argv[0], a);
            return ASH_EXIT_USAGE;
        } else {
            fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], a);
            print_usage(argv[0]);
            return ASH_EXIT_USAGE;
        }
    }

    /* Before any mode runs, so a stop from outside is reported the same way
     * (see ASH_EXIT_TERMINATED) - the console going away, or Ctrl+C. */
    ash_install_termination_handler();

    /* Before any mode runs: it belongs to the run this process starts. */
    win32_host_set_volatile_rootfs(memfs);

    if (list) {
        return ash_list();
    }
    if (!serve) {
        /* There is no client mode any more: the machine a client would have
         * attached to is served by `vp` now, over the same console the Android
         * host exposes. Saying where to go beats starting a core nobody asked
         * for. */
        fprintf(stderr,
                "%s: nothing to do. This is the core host: run `%s --serve`.\n"
                "    To attach, use `vp --local -P <port> shell` (or exec-out);\n"
                "    `vp --local devices` lists a running core.\n",
                argv[0], argv[0]);
        return ASH_EXIT_USAGE;
    }

    /* The same switch the core honours (see ash_serve): without it a core stays
     * at LOG_WARN, so the per-syscall lines a session's trouble has to be read
     * from never reach this console - and the core is exactly where they are
     * needed. ash_serve() re-applies it and then applies any loglevel=/debug
     * from --cmdline on top, which has to be the later of the two or a run
     * asked to be loud would not be. */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);
    return ash_serve(port, idle, dlog, cmdline);
}
