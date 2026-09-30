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
 * The program a run with nothing to say about it starts.
 *
 * "/bin/busybox sh", NOT "/bin/sh". Both name a shell and the second is what a
 * person would type, but they are not the same thing to exec:
 *
 *   /bin/sh is a symlink. Symlinks are deliberately never materialized on the
 *   host - that is what lets one busybox answer for its three hundred names
 *   (vp_rootfs.h) - so /bin/sh exists only in the archive's shadow. A run with a
 *   shadow installed resolves it; this binary has no shadow, because installing
 *   one is a host's job (rvvm_user_set_shadow()) and rvvm_user is the host with
 *   no bundle. Opening it therefore fails with ENOENT, which is a poor thing for
 *   a default to do on its first run.
 *
 *   /bin/busybox is a real file, and "busybox sh" is busybox's own documented
 *   dispatch - which is how Alpine invokes a shell anyway. So the default is the
 *   same shell, reached the way that works without a shadow.
 *
 * Not /sbin/init, which is also present: busybox init reads /etc/inittab and
 * claims a console, and there is no init here to supervise anything, so it would
 * come up printing complaints instead of a prompt.
 */
#define RVVM_USER_DEFAULT_INIT "/bin/busybox"
#define RVVM_USER_DEFAULT_VERB "sh"

static void print_usage(const char* self)
{
    fprintf(stderr,
            "usage: %s [-prefix <dir>] [-cmdline <args>] [-append <args>]"
            " [guest_elf [args...]]\n"
            "\n"
            "With no guest_elf, boots %s in the default prefix"
            " (./runtime/rootfs).\n"
            "\n"
            "  -prefix <dir>    the hostfs base: where the guest's / lives.\n"
            "                  A HOST path and NOT a boot argument - it would\n"
            "                  mean a different run on every machine, since the\n"
            "                  rootfs sits under the APK's private directory on\n"
            "                  Android and under the release tree on win32.\n"
            "                  Relative paths resolve against the working"
            " directory.\n"
            "  -cmdline <args>  boot arguments, in Linux's spelling. The guest\n"
            "                  reads them back from /proc/cmdline.\n"
            "  -append <args>   more of them, keeping what -cmdline set.\n"
            "\n"
            "The program, in order of precedence: the ELF named here, else\n"
            "init= from -cmdline, else %s.\n"
            "\n"
            "loglevel=<name|0..4> and a bare debug set this process's logging.\n"
            "root= is recorded and reported but not applied - the guest's\n"
            "filesystem is answered from the archive index, which knows nothing\n"
            "of a sub-root; see rvvm_user_set_cmdline() for what that would take.\n",
            self, RVVM_USER_DEFAULT_INIT, RVVM_USER_DEFAULT_INIT);
}

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
    /* Hoisted out of the default-program branch below so the debug line can see
     * them; the branch is the only writer. */
    char          chosen[512];
    char          verb[64];
    char*         args[3];

    /* stderr unbuffered, before anything is printed.
     *
     * This binary is the one people run with its output redirected to a file or
     * a pipe - that is how a driver reads a guest's transcript - and two things
     * go wrong when it is not. A guest that dies takes the process with it
     * without flushing, so the last lines written are the ones lost, and those
     * are the ones naming the crash. Worse, stacktrace_print() writes its frames
     * straight to the stderr FILE* (stacktrace.c, via libbacktrace) rather than
     * through rvvm_warn, so a crash *inside* a stdio operation on stderr leaves
     * that stream's lock held and the frames never appear at all - the output
     * stops at "Stacktrace:" with nothing under it, which is the one case where
     * the trace was wanted.
     *
     * Unbuffered costs nothing here: a handful of lines per run, and rvvm_warn's
     * own sink (utils.c) is where the per-syscall volume goes regardless. */
    setvbuf(stderr, NULL, _IONBF, 0);

    int i = 1;
    for (; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            /* Handled here rather than in the no-arguments branch below, so it
             * works in any position: `rvvm_user --help` and
             * `rvvm_user -cmdline "..." --help` are both a person asking for
             * help, and a branch that only looked at argc would answer the
             * second one by trying to boot "--help". */
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-prefix") || !strcmp(argv[i], "--prefix")) {
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

    if (argc < 2) {
        /* No arguments at all is a run, not a usage error. Both defaults already
         * exist - the prefix is USERLAND_DEFAULT_PREFIX (./runtime/rootfs, see
         * rvvm_user_create) and the program is RVVM_USER_DEFAULT_INIT below - so
         * the shortest useful command is the one with nothing in it. A userland
         * emulator whose no-argument case prints help has a userland emulator
         * that cannot be run by a person who has just installed it.
         *
         * Nothing to do here: control falls through to the defaults. -h is
         * handled in the option loop above, which cannot run in this case. */
    } else if (i >= argc && !(cmdline || append)) {
        /* Options given, no ELF, and nothing that could name one: that IS a
         * mistake, and saying so beats quietly booting a shell the caller did not
         * ask for. With -cmdline/-append it is not a mistake, because init= in
         * there is the program (see the precedence below) - so this has to
         * look at what those were set to rather than only at i. */
        fprintf(stderr, "%s: options given but no guest program\n\n", argv[0]);
        print_usage(argv[0]);
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

    /* Which program runs, in Linux's order of precedence:
     *
     *   1. an ELF named on the command line. Explicit beats everything, and it is
     *      the only form that can also pass the program arguments.
     *   2. init= from the boot arguments. This is where init= finally means
     *      something: this binary *is* the host, so it is the one that has to
     *      honour it, which is exactly why the core refuses to (see
     *      rvvm_user_set_cmdline - the core cannot know what programs exist).
     *   3. /bin/sh.
     *
     * The default is a shell rather than an init because that is what this is: a
     * userland emulator with no init to run. The rootfs does ship /sbin/init (a
     * busybox link, in the archive's shadow - symlinks are never materialized on
     * the host), and it would be a worse default: busybox init reads /etc/inittab
     * and wants a console it does not have here, so the run would come up as a
     * process printing complaints instead of a prompt.
     */
    if (i < argc) {
        guest_argc = argc - i;
        guest_argv = argv + i;
    } else {
        const char* init = rvvm_user_cmdline_arg(machine, "init");
        /* argv[argc] is NULL by the C standard, and guest_setup_stack() walks
         * the array to that terminator, hence three slots for two entries. */
        if (init && init[0]) {
            fprintf(stderr, "rvvm_user: init=%s\n", init);
            snprintf(chosen, sizeof(chosen), "%s", init);
        } else {
            snprintf(chosen, sizeof(chosen), "%s", RVVM_USER_DEFAULT_INIT);
        }
        snprintf(verb, sizeof(verb), "%s", RVVM_USER_DEFAULT_VERB);
        /* A char* pointing at the buffers, not a cast of them: argv is an array
         * of pointers, and (char**)&chosen would make guest_argv[0] read the
         * first eight bytes of the path as an address. */
        args[0]    = chosen;
        args[1]    = verb;
        args[2]    = NULL;
        guest_argc = 2;
        guest_argv = args;
    }

    /* One line naming the program and the prefix it will see: both are decided
     * above from three sources, and "which shell did that actually start" is not
     * a question the output can always answer. */
    /* One line naming the program and the prefix it will see: both are decided
     * above from three sources, and "which shell did that actually start" is not
     * a question the output can always answer. */
    fprintf(stderr, "rvvm_user: boot %s\n", guest_argv[0]);
    fprintf(stderr, "rvvm_user: prefix %s\n",
            rvvm_user_get_prefix(machine) ? rvvm_user_get_prefix(machine)
                                          : "(passthrough)");
    fprintf(stderr, "rvvm_user: guest root %s\n",
            rvvm_user_get_guest_root(machine) ? rvvm_user_get_guest_root(machine)
                                              : "(none)");
    return rvvm_user_linux_ex(machine, guest_argc, guest_argv, envp);
}