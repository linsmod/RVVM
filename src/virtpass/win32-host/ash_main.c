/*
ash_main.c - rvvm_ash: the console host, split WSL-style into a core and clients

rvvm_ash is the bash.exe of this project, but the WSL shape needs two roles:

    rvvm_ash --serve        the core: one run (one machine, one rootfs) kept up,
                            booting the session server so clients can attach
    rvvm_ash                a client: attach to the core (start one if none) and
                            be the terminal for a new session
    rvvm_ash --direct       boot the guest shell straight on this console - the
                            old behaviour, kept for the regression tools

A client is a thin terminal over the session server's wire protocol (see
ash_client.c); a core is the run the sessions live in. "--serve is the run
boundary, a client is a session boundary."
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_cmdpost_bridge.h"

/* Implemented in ash_client.c */
int ash_serve(int port, int idle_s);
int ash_client(int port, const char* one_cmd, bool autostart);
int ash_list(void);
int ash_shutdown(int port);

#define ASH_GUEST_ARGS_MAX 32
#define ASH_PORT_DEFAULT   7900

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
            "usage: %s [--serve [--idle S]] [--direct] [--list] [--shutdown]\n"
            "          [--port N] [-c <command>] [args...]\n"
            "\n"
            "  %s                  connect to the run's core (start one if none);"
            " a new session\n"
            "  %s --serve          run the core: one run, kept up for clients\n"
            "  %s --direct         boot the guest shell on this console (old mode)\n"
            "  %s -c \"ls /bin\"      run one command in a session\n"
            "  %s --list           list the cores registered in this release tree\n"
            "  %s --shutdown       ask the core on the port to stop\n"
            "\n"
            "Options: --port N (default %d, RVVM_ASH_PORT); --serve --idle S stops a\n"
            "core after S seconds with no session (0 = never). RVVM_ASH_SHELL overrides\n"
            "the shell: for --direct the guest to boot, for --serve the core program\n"
            "(default /sbin/vpsessiond).\n",
            self, self, self, self, self, self, self, ASH_PORT_DEFAULT);
}

int main(int argc, char** argv)
{
    bool        serve    = false;
    bool        direct   = false;
    bool        list     = false;
    bool        shutdown = false;
    const char* one_cmd  = NULL;
    int         idle     = 0;
    int         port     = ash_port();
    int         i        = 1;
    int         rc;

    /* Host options first; the first argument that is not one of them starts the
     * guest arguments (--direct) or the client's command. */
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(a, "--serve")) {
            serve = true;
        } else if (!strcmp(a, "--direct")) {
            direct = true;
        } else if (!strcmp(a, "--list")) {
            list = true;
        } else if (!strcmp(a, "--shutdown")) {
            shutdown = true;
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
    if (serve) {
        return ash_serve(port, idle);
    }

    /* Backward compatibility: a caller that set RVVM_ASH_SHELL (the regression
     * tools) asked for a direct run of that guest, not for a client. */
    if (direct || getenv("RVVM_ASH_SHELL")) {
        const char* shell = getenv("RVVM_ASH_SHELL");
        char* guest[ASH_GUEST_ARGS_MAX];
        int   n = 0;

        if (!shell || !*shell) {
            shell = "/bin/sh";
        }
        guest[n++] = (char*)shell;
        for (; i < argc && n < ASH_GUEST_ARGS_MAX; i++, n++) {
            guest[n] = argv[i];
        }
        if (i < argc) {
            fprintf(stderr, "%s: too many arguments (max %d)\n",
                    argv[0], ASH_GUEST_ARGS_MAX - 1);
            return 1;
        }

        if (!win32_host_init_console(0, 0, 0)) {
            fprintf(stderr, "%s: could not initialize the host\n", argv[0]);
            return 1;
        }
        if (!win32_host_start_guest(n, guest)) {
            fprintf(stderr, "%s: could not start %s\n", argv[0], shell);
            win32_host_shutdown();
            return 1;
        }
        rc = win32_host_wait_guest();
        win32_host_shutdown();
        return rc;
    }

    /* Client mode. `-c <command>` asks the core for a one-shot session. */
    if (i < argc && !strcmp(argv[i], "-c") && i + 1 < argc) {
        one_cmd = argv[i + 1];
    } else if (i < argc) {
        fprintf(stderr, "%s: unexpected argument '%s' (did you mean --direct?)\n",
                argv[0], argv[i]);
        print_usage(argv[0]);
        return 1;
    }
    return ash_client(port, one_cmd, true);
}