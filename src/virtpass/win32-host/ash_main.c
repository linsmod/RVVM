/*
ash_main.c - rvvm_ash: a console-only host that boots the bundle's shell

The WinHost is a windowed host that also owns a console; this is the other half
of the same bridge: no window, no GL, no audio, no sensors - just the guest's
terminal, wired to this console. It is the bash.exe to the WinHost's wsl.exe:

    rvvm_ash.exe                 interactive shell on the bundle's rootfs
    rvvm_ash.exe -c "ls /bin"    one command; the exit code is the guest's
    rvvm_ash.exe script.sh       arguments are handed to the shell

While the guest runs the console is in raw mode - no echo, no line assembly, and
^C arrives as a byte so the guest's own line discipline decides what it means
(a shell gets its SIGINT, vi gets a literal ^Z) - and it is put back the way it
was found on the way out. The guest sees this console's real geometry, and a
resize of it as SIGWINCH.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_cmdpost_bridge.h"

#define ASH_GUEST_ARGS_MAX 32

static void print_usage(const char* self)
{
    fprintf(stderr,
            "usage: %s [-c <command>] [args...]\n"
            "\n"
            "A shell on the bundle's rootfs (busybox ash), on this console:\n"
            "  %s                  interactive shell\n"
            "  %s -c \"ls /bin\"      one command, exit with the guest's code\n"
            "\n"
            "It runs on the bundle next to this binary\n"
            "(bundle/{rootfs,apps}.tar.gz, see `make dist`). RVVM_ASH_SHELL\n"
            "overrides the shell to run (default /bin/sh).\n",
            self, self, self);
}

int main(int argc, char** argv)
{
    const char* shell = getenv("RVVM_ASH_SHELL");
    char* guest[ASH_GUEST_ARGS_MAX];
    int n = 0;
    int i = 1;
    int rc;

    if (!shell || !*shell) {
        shell = "/bin/sh";
    }
    if (i < argc && (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))) {
        print_usage(argv[0]);
        return 0;
    }

    /* argv[0] is the guest program; everything else is handed to the shell
     * unchanged - "-c <command>" included, which is the shell's own spelling. */
    guest[n++] = (char*)shell;
    for (; i < argc && n < ASH_GUEST_ARGS_MAX; i++, n++) {
        guest[n] = argv[i];
    }
    if (i < argc) {
        fprintf(stderr, "%s: too many arguments (max %d)\n", argv[0], ASH_GUEST_ARGS_MAX - 1);
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
