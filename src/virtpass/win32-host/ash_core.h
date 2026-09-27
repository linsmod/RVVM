/* ash_core.h - what `ash --serve` exits with, and the one signature behind it.
 *
 * The exit code is the core's own vocabulary and nothing else. It used to be the
 * guest program's status handed straight back (ash_serve returned whatever
 * win32_host_wait_guest() gave), which made every value ambiguous: 1 was both
 * "another core owns this port" and "the guest exited 1", and no client or test
 * driver could tell a refused start from a finished run.
 *
 * So the guest's status is not reported as ours any more. It is still recorded,
 * on the core's stderr, next to the log that already explains the run - but the
 * code says what happened to the *core*, which is the only question a caller can
 * act on: a different port, a different release, a different remedy.
 *
 * These are a closed set. A value not listed here is a bug in ash, not a case.
 */

#ifndef VIRT_PASS_WIN32_HOST_ASH_CORE_H
#define VIRT_PASS_WIN32_HOST_ASH_CORE_H

#include <stdbool.h>

/* The core stopped: the guest program left, or a client asked it to. */
#define ASH_EXIT_OK           0

/* Another live core already owns this port, so this one refused to start. */
#define ASH_EXIT_PORT_BUSY   64
/* Another live core holds this release tree's rootfs, so this one cannot run
 * beside it - a different problem from a busy port, with a different remedy. */
#define ASH_EXIT_ROOTFS_BUSY 65
/* The host could not be initialized at all. */
#define ASH_EXIT_HOST_INIT   66
/* The host came up but the guest program would not start. */
#define ASH_EXIT_NO_GUEST    67
/* The arguments do not name a run this binary can perform. */
#define ASH_EXIT_USAGE       68
/* The guest program left with a non-zero status, so it died rather than stopped.
 *
 * A classification, not the guest's value: a number from inside the guest never
 * becomes one of ours, which is what the codes above are for. The distinction
 * itself has to survive, though, because it is the one a crash regression
 * watches - the guest's fault handler exits 128+signal, and a core that called
 * that "stopped cleanly" would tell the driver looking for exactly that death
 * that nothing had happened. The status itself is on the core's stderr. */
#define ASH_EXIT_GUEST_NONZERO 69

/* Implemented in ash_client.c */
int ash_serve(int port, int idle_s, const char* dlog);
int ash_client(int port, const char* one_cmd, bool autostart);
int ash_list(void);
int ash_shutdown(int port);
int ash_sock_path(int port);

#endif
