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
#include "utils.h" /* rvvm_set_loglevel: RVVM_VERBOSE, as the client path uses */

/* Implemented in ash_client.c */
int ash_serve(int port, int idle_s, const char* dlog);
int ash_client(int port, const char* one_cmd, bool autostart);
int ash_list(void);
int ash_shutdown(int port);
int ash_sock_path(int port);

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
            "          [--port N] [-c <command>] [--sock-path]\n"
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
            "server's log at that guest path instead of the run's /tmp. The core\n"
            "program is /sbin/vpsessiond, from the bundle.\n"
            "\n"
            "--no-autostart: fail instead of starting a core when none is listening.\n"
            "A client that starts one by itself is convenient and wrong for a test:\n"
            "the core it starts is a different run, with its own rootfs, and nothing\n"
            "bounds how long it lives, so a driver that lost its core would go on to\n"
            "pass against a run it never set up.\n",
            self, self, self, self, self, self, self, ASH_PORT_DEFAULT);
}

int main(int argc, char** argv)
{
    bool        serve    = false;
    bool        list     = false;
    bool        shutdown = false;
    bool        sockpath = false;
    /* A client starts a core if none is listening, the way wsl does. A test
     * driver does not want that: it wants a dead core to say so. */
    bool        autostart = true;
    const char* one_cmd  = NULL;
    const char* dlog     = NULL;
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
        } else if (!strcmp(a, "--idle") && i + 1 < argc) {
            idle = atoi(argv[++i]);
        } else if (!strncmp(a, "--idle=", 7)) {
            idle = atoi(a + 7);
        } else if (!strcmp(a, "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strncmp(a, "--port=", 7)) {
            port = atoi(a + 7);
        } else {
            break;
        }
    }

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
         * exactly where they are needed. */
        rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);
        return ash_serve(port, idle, dlog);
    }

    /* Client mode. `-c <command>` asks the core for a one-shot session. */
    if (i < argc && !strcmp(argv[i], "-c") && i + 1 < argc) {
        one_cmd = argv[i + 1];
    } else if (i < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[i]);
        print_usage(argv[0]);
        return 1;
    }
    return ash_client(port, one_cmd, autostart);
}