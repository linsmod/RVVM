/*
ash_main.c - rvvm_ash: the console host, split WSL-style into a core and clients

rvvm_ash is the bash.exe of this project, and it has the two roles a WSL user
expects:

    rvvm_ash --serve        the core: one run (one machine, one rootfs) kept up,
                            booting the session server (/sbin/vpsessiond, from
                            the bundle's system layer) so clients can attach
    rvvm_ash                a client: attach to the core (start one if none) and
                            be the terminal for a new session

A client is a thin terminal over the session server's wire protocol (see
ash_client.c); a core is the run the sessions live in. "--serve is the run
boundary, a client is a session boundary."

There is no "boot a loose ELF on this console" mode: the core program is the
host-owned system program /sbin/vpsessiond, and an application is booted as an
app package (rvvm_winhost --app <id>). Debugging is RVVM_TRACE / RVVM_VERBOSE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_cmdpost_bridge.h"
#include "ash_core.h" /* the --serve exit codes, and the prototypes below */
#include "utils.h" /* rvvm_set_loglevel: RVVM_VERBOSE, as the client path uses */

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
            "usage: %s [--serve [--idle S] [--dlog <guest-path>]] [--list]\n"
            "          [--shutdown] [--no-autostart]\n"
            "          [--cmdline <linux-args>] [--port N] [-c <command>] [--sock-path]\n"
            "\n"
            "  %s                  connect to the run's core (start one if none);"
            " a new session\n"
            "  %s --serve          run the core: one run, kept up for clients\n"
            "  %s -c \"ls /bin\"      run one command in a session\n"
            "  %s --list           list the cores registered in this release tree\n"
            "  %s --shutdown       ask the core on the port to stop\n"
            "  %s --sock-path      print the core's AF_UNIX endpoint and exit\n"
            "\n"
            "Options: --port N (default %d, RVVM_ASH_PORT) is the core's logical id\n"
            "and names its endpoint; --serve --idle S stops a core after S seconds\n"
            "with no session (0 = never). --serve --dlog <path> puts the session\n"
            "server's log at that guest path instead of the run's /tmp.\n"
            "\n"
            "--cmdline <args> boots the core's machine with those Linux-style boot\n"
            "arguments, and is the only way to change what the run is made of:\n"
            "  init=<guest-path>  the core program, instead of /sbin/vpsessiond\n"
            "  root=<host-path>   the run's filesystem prefix, instead of the bundle\n"
            "  loglevel=<name|n>  none/error/warn/info/debug, or the kernel's 0..4\n"
            "  debug              the same as loglevel=debug\n"
            "Whatever is passed is also what the guest reads back from\n"
            "/proc/cmdline, so a program in the run can see how it was started.\n"
            "Arguments this build does not know are kept, not refused - they may be\n"
            "for the guest program rather than for the host.\n"
            "\n"
            "--no-autostart: fail instead of starting a core when none is listening.\n"
            "A client that starts one by itself is convenient and wrong for a test:\n"
            "the core it starts is a different run, with its own rootfs, and nothing\n"
            "bounds how long it lives, so a driver that lost its core would go on to\n"
            "pass against a run it never set up.\n"
            "\n"
            "Host diagnostics (RVVM_VERBOSE=1) go to stderr, never to this process's\n"
            "stdout, which is the guest's console transcript. RVVM_LOG_FILE=<path>\n"
            "appends them to a file instead, and RVVM_LOG_RING_DUMP=<path> writes the\n"
            "log ring - the last 128 KiB of the run, kept in static storage - at exit.\n",
            self, self, self, self, self, self, self, self, ASH_PORT_DEFAULT);
}

int main(int argc, char** argv)
{
    bool        serve    = false;
    bool        list     = false;
    bool        shutdown = false;
    bool        sockpath = false;
    bool        memfs    = false;
    /* A client starts a core if none is listening, the way wsl does. A test
     * driver does not want that: it wants a dead core to say so. */
    bool        autostart = true;
    const char* one_cmd  = NULL;
    const char* dlog     = NULL;
    const char* cmdline  = NULL;
    int         idle     = 0;
    int         port     = ash_port();
    int         i        = 1;

    /* Host options first; the first argument that is not one of them is the
     * client's command tail. */
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
        } else if (!strcmp(a, "--shutdown")) {
            shutdown = true;
        } else if (!strcmp(a, "--sock-path")) {
            sockpath = true;
        } else if (!strcmp(a, "--no-autostart")) {
            autostart = false;
        } else if (!strcmp(a, "--dlog") && i + 1 < argc) {
            dlog = argv[++i];
        } else if (!strncmp(a, "--dlog=", 7)) {
            dlog = a + 7;
        } else if (!strcmp(a, "--memfs")) {
            /* The bundle goes into the run's own memory filesystem rather than
             * being materialized under <exe>\runtime\rootfs: a run that leaves
             * nothing behind, whose / dies with it. The guest still gets the
             * release's rootfs and system programs - installed into memory - so
             * busybox and the session server are there; what is different is
             * where. */
            memfs = true;
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
        } else {
            break;
        }
    }

    /* Before any mode runs, so every one of them reports a stop from outside the
     * same way (see ASH_EXIT_TERMINATED). A client inside a session is unaffected:
     * that console is in raw mode, so a ^C is a byte to the guest rather than a
     * control event to this process. */
    ash_install_termination_handler();

    /* Before any mode runs: it belongs to the run this process starts, not to one
     * of the modes below, and a client that connects later is talking to a core
     * somebody else started. */
    win32_host_set_volatile_rootfs(memfs);

    if (list) {
        return ash_list();
    }
    if (shutdown) {
        return ash_shutdown(port);
    }
    if (sockpath) {
        return ash_sock_path(port);
    }
    if (serve) {
        /* The same switch the client path honours (see ash_client.c): without
         * it a core stays at LOG_WARN, so the per-syscall lines a session's
         * trouble has to be read from never reach this console - and the core is
         * exactly where they are needed. ash_serve() re-applies it and then
         * applies any loglevel=/debug from --cmdline on top, which has to be
         * the later of the two or a run asked to be loud would not be. */
        rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);
        return ash_serve(port, idle, dlog, cmdline);
    }

    /* Client mode. `-c <command>` asks the core for a new session. */
    if (i < argc && !strcmp(argv[i], "-c") && i + 1 < argc) {
        one_cmd = argv[i + 1];
    } else if (i < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    /* A client boots nothing, so init= and root= have nothing to act on here.
     * Saying so beats taking them and ignoring them: a `--cmdline` that looked
     * like it worked and left the core running last time's rootfs is exactly
     * the kind of silent mismatch a boot argument is supposed to rule out. The
     * logging arguments are still honoured, because they are about this process
     * and not about the core. */
    if (cmdline) {
        const char* init_arg = rvvm_cmdline_get(cmdline, "init");
        const char* root_arg = rvvm_cmdline_get(cmdline, "root");
        if ((init_arg && init_arg[0]) || (root_arg && root_arg[0])) {
            fprintf(stderr,
                    "%s: init= and root= describe the machine, and a client "
                    "starts none.\n"
                    "    They belong to --serve, which is what boots the run:\n"
                    "        %s --serve --cmdline \"%s%s\"\n",
                    argv[0], argv[0], init_arg ? "init=" : "",
                    init_arg ? init_arg : root_arg);
            return 2;
        }
        rvvm_apply_cmdline_logging(cmdline);
    }
    return ash_client(port, one_cmd, autostart);
}