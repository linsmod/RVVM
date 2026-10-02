/*
 * vp_console.h - the guest console as a byte pipe (see vp_console.c)
 *
 * The packet ids and the 1+4 byte header are adb's ShellProtocol, so that a
 * client written against adb's shape works here unchanged: stdin, stdout,
 * stderr, exit, close-stdin and window size, multiplexed on one connection.
 */
#pragma once

#include <stddef.h>

#include <core/rvvm_user.h>
#include <virtpass/vp_core.h>

struct console_conn;

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VP_CONSOLE_STDIN        = 0,
    VP_CONSOLE_STDOUT       = 1,
    VP_CONSOLE_STDERR       = 2,
    VP_CONSOLE_EXIT         = 3,
    VP_CONSOLE_CLOSESTDIN   = 4,
    VP_CONSOLE_WINDOWSIZE   = 5,
    /* Client -> guest. The first thing a console says, sent before anything
     * else on the connection: the client waits for it, so nothing the driver
     * types can arrive before the console is there to receive it. */
    VP_CONSOLE_READY        = 6,
};

/* The machine the console types into. Set once, at the point the run is
 * known; NULL until then, which is a no-op rather than a crash because the
 * tap can fire from a guest that is on its way out. */
void vp_console_set_machine(rvvm_machine_t* machine);
rvvm_machine_t* vp_console_machine(void);

/* Bytes the guest wrote to fd 1 or 2. Called on the guest's own thread, from
 * the io callback, and must not block: it copies into a ring and returns. */
void vp_console_tap(int fd, const void* data, size_t len);

/* A run's exit status, sent as its own packet after its output. */
void vp_console_exit(int code);

/* Loopback listener. 0 on success. */
int  vp_console_start(int port);
void vp_console_stop(void);

/* --- Driving a core: one machine, one session per client ---
 *
 * A console with one keyboard is one session: every client typed into the same
 * line discipline, and two clients' bytes interleaved in one shell. Giving each
 * client its own machine does not fix it either, because separate machines are
 * separate /proc and no session's ps would name another's processes.
 *
 * A session is therefore a terminal - one per client, all in one machine - and
 * the whole of that is shared with the other host in vp_core.c, because it is the
 * same code there and a second copy is a second thing to keep right. What this
 * file owns is the console's own half: which connections have a session, when
 * they are given one, and moving bytes between a client and its terminal.
 *
 * The protocol is untouched. A session is the same five-byte framing as before;
 * what changed is how many pipes there are and which terminal each is attached
 * to. */

/* Make a core on the machine that has just appeared, and give every waiting
 * client a session on it. Does nothing unless the host armed a core, so a run
 * that is a command keeps the plain console it always had.
 *
 * Called from vp_console_set_machine(), because that is the only moment a machine
 * exists - and a core cannot have one before its client arrives, the run being
 * held until a client is there. That is also why a session cannot be opened at
 * accept: a client on a core always connects before there is anything to be a
 * session in. */
void vp_console_set_core(rvvm_machine_t* machine);

/* Whether the host has said the next run to start is a core. Set before the guest
 * starts, because the control terminal has to exist before the guest thread does
 * - the guest opens it as its first act. */
void vp_console_arm_core(void);
int  vp_console_core_armed(void);

/* The core's control terminal path, for the --control argument the guest is
 * booted with. Valid once vp_console_set_core() has run; NULL before, because
 * there is no machine to own a terminal yet. */
const char* vp_console_control_path(void);

/* A session for @c, on the core. NULL when there is no core or no terminal left,
 * in which case the connection keeps the run's own console - the pre-core
 * behaviour, and still right for a host that launched a single named guest. */
struct vp_core_session* vp_console_session_open(struct console_conn* c,
                                               int rows, int cols);

/* Give this connection a session, sized by whatever it has said about its
 * terminal so far. Does nothing on a second call. */
void vp_console_session_start(struct console_conn* c, int rows, int cols);

/* Called on the accept thread once a client is actually attached - not when
 * the listener binds. The hook runs on that thread and must not block; it is
 * how a host that was launched with a guest named waits for a driver to arrive
 * before starting the run. NULL clears it. */
void vp_console_set_connect_hook(void (*hook)(void*), void* ud);

#ifdef __cplusplus
}
#endif
