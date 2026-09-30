/*
rvvm_user_main.c - RVVM Linux userland emulator entry point
Thin wrapper around rvvm_user_linux_ex() from core/rvvm_user.c

Usage: rvvm_user [-prefix <dir>] [-cmdline <args>] [-append <args>] <guest_elf> [args...]

`-cmdline` and `-append` are src/main.c's own spellings, reused rather than
invented: the full-system binary (rvvm_x86_64) has always taken boot arguments
that way, and a command line with two spellings is worse than one with none. The
full-system binary can act on them because it hands the machine to a kernel
through a device tree; here there is no kernel, so the same string is also what
the guest reads back from /proc/cmdline.

`-prefix` is NOT a boot argument and is not one anywhere: the hostfs base is a
host path (see rvvm_user_set_prefix), and a command line that named one would
mean a different run on every machine. It is here because this binary *is* the
host - with no bundle to install, the only thing that can put a rootfs under the
guest is the thing that started it. A relative path is resolved against the
working directory; the setter does that, not this file.
*/

#include <stdio.h>
#include <string.h>

#include "core/rvvm_user.h"
#include "rvvm/rvvm.h"   /* rvvm_set_cmdline / rvvm_append_cmdline */

/*
 * Why this does not just call rvvm_user_linux().
 *
 * rvvm_user_linux() creates the machine itself and hands it straight to
 * rvvm_user_linux_ex(), so its caller never holds a handle and there is nowhere
 * to set a command line before the image is loaded - which is the whole of what
 * a boot argument is. The seam is already public: rvvm_user_create() and
 * rvvm_user_linux_ex() are both PUBLIC, so the three steps are done here
 * instead, in the order a boot does them.
 *
 * Doing it this way rather than adding a setter to the core is deliberate: a
 * "pending cmdline" global would have to be consumed by rvvm_user_linux_ex() and
 * would then be a second way for every host to configure a machine - the thing
 * the win32 bridge and the Android host already have (win32_host_set_cmdline /
 * nativeSetCmdline) and the reason rvvm_user_linux_ex(machine, ...) exists as a
 * separate entry point at all.
 */
int main(int argc, char** argv, char** envp)
{
    rvvm_machine_t* machine;
    const char*     prefix  = NULL;
    const char*     cmdline = NULL;
    const char*     append  = NULL;
    int             guest_argc;
    char**          guest_argv;

    int i = 1;
    for (; i < argc; ++i) {
        if (!strcmp(argv[i], "-prefix") || !strcmp(argv[i], "--prefix")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            prefix = argv[++i];
        } else if (!strncmp(argv[i], "-prefix=", 8)) {
            prefix = argv[i] + 8;
        } else if (!strncmp(argv[i], "--prefix=", 9)) {
            prefix = argv[i] + 9;
        } else if (!strcmp(argv[i], "-cmdline") || !strcmp(argv[i], "--cmdline")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            cmdline = argv[++i];
        } else if (!strncmp(argv[i], "-cmdline=", 9)) {
            cmdline = argv[i] + 9;
        } else if (!strncmp(argv[i], "--cmdline=", 10)) {
            cmdline = argv[i] + 10;
        } else if (!strcmp(argv[i], "-append") || !strcmp(argv[i], "--append")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], argv[i]);
                return 1;
            }
            append = argv[++i];
        } else if (!strncmp(argv[i], "-append=", 8)) {
            append = argv[i] + 8;
        } else if (!strncmp(argv[i], "--append=", 9)) {
            append = argv[i] + 9;
        } else {
            break;   /* the first thing that is not an option is the ELF */
        }
    }

    if (i >= argc) {
        fprintf(stderr,
                "Usage: %s [-prefix <dir>] [-cmdline <args>] [-append <args>]"
                " <guest_elf> [args...]\n"
                "\n"
                "  -prefix <dir>    the hostfs base: where the guest's / lives.\n"
                "                  A HOST path, and not a boot argument - root= on\n"
                "                  -cmdline is the guest-facing spelling of the same\n"
                "                  idea. Relative paths resolve against the working\n"
                "                  directory.\n"
                "  -cmdline <args>  boot arguments, in Linux's spelling\n"
                "  -append <args>   more of them, keeping what -cmdline set\n"
                "\n"
                "The arguments are what the guest reads back from /proc/cmdline.\n"
                "root= is applied (it names the guest directory that becomes /);\n"
                "init= is not, because this wrapper is already told which ELF to\n"
                "run and overriding that behind its back would boot a program\n"
                "nobody asked for.\n",
                argv[0]);
        return 1;
    }

    machine = rvvm_user_create();
    if (!machine) {
        return -1;
    }

    /* Before the image is opened: both of these have to be in effect before the
     * first path the guest resolves, and rvvm_user_linux_ex() loads the ELF
     * immediately after this. */
    if (prefix) {
        rvvm_user_set_prefix(machine, prefix);
        /* Echoed resolved, because that is the string every guest path is built
         * from, and "it found my directory" is worth seeing before the guest runs
         * rather than inferred from a run that happened to work. */
        fprintf(stderr, "rvvm_user: prefix: %s\n", rvvm_user_get_prefix(machine));
    }

    /* Two calls rather than one, and the same two src/main.c makes (lines
     * 379-383): the appending setter is what puts the separator between the
     * arguments and bounds the result, so building a merged string here would be
     * a second set of rules for something the machine already knows how to do. */
    if (cmdline) {
        rvvm_user_set_cmdline(machine, cmdline);
    }
    if (append) {
        rvvm_append_cmdline(machine, append);
    }

    /* What the guest will see, printed rather than assumed: /proc/cmdline is
     * generated from this string and a run whose arguments silently did not
     * land is a run that looks like the guest ignored them. */
    if (cmdline || append) {
        fprintf(stderr, "rvvm_user: cmdline: %s\n", rvvm_user_get_cmdline(machine));
    }

    guest_argc = argc - i;
    guest_argv = argv + i;
    return rvvm_user_linux_ex(machine, guest_argc, guest_argv, envp);
}