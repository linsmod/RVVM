/*
 * test_sigint.c - Verifies guest-subscribed SIGINT (Ctrl-C) delivery
 *
 * Registers a SIGINT handler with sigaction() and counts deliveries. Every
 * ^C (the taskbar key, or the host keyboard) runs the handler ON the guest -
 * the run must keep going afterwards, proving the signal was delivered as a
 * real POSIX signal and not the old "host stops the machine" shortcut.
 *
 * Expected console output:
 *   sigint test: press ^C twice
 *   [SIGINT handled]        (per press, from inside the handler)
 *   === 2 SIGINT(s) handled, exit 42 ===
 *
 * Exit code 42: the status line must report it (a ^C that stopped the run
 * instead would end with 130 or nothing at all).
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int hits = 0;

static void on_int(int sig)
{
    (void)sig;
    hits++;
    /* write(), not stdio: async-signal-safe */
    write(1, "[SIGINT handled]\n", 17);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);

    printf("sigint test: press ^C twice\n");
    fflush(stdout);

    while (hits < 2) {
        sleep(1);
    }

    printf("=== %d SIGINT(s) handled, exit 42 ===\n", hits);
    fflush(stdout);
    return 42;
}
