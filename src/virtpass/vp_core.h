/*
 * vp_core.h - the host side of a core: one machine, one session per client
 *
 * A host that serves several clients out of one machine needs a terminal per
 * client, not one shared console. The byte pipe a console is - one keyboard, one
 * foreground process - can only ever be one session, and two clients on it do not
 * queue behind each other, they interleave: the second reads the first one's
 * prompt and its output, and both sets of bytes arrive at both.
 *
 * Giving each client its own machine does not fix that either, and this is the
 * part worth being sure about: separate machines are separate /proc, so no
 * session's `ps` would ever name another session's processes. Several shells that
 * cannot see each other are not the thing anyone wants from "several sessions".
 *
 * So the machine is shared and the *terminal* is not. A session is a terminal, and
 * a session is a terminal in one piece:
 *
 *   - it holds a pty out of the machine's own pool (rvvm_session_pty_*), which is
 *     what makes its line discipline, echo, window size and ^C its own;
 *   - the host drives the master end and moves bytes;
 *   - the guest forks a shell onto the slave end, and that shell is an ordinary
 *     process in the machine - which is why sessions see each other's processes.
 *
 * The guest side of the fork is idle (guest-samples/idle.c), booted as the run
 * root so the machine outlives every client. It is told which terminal carries
 * the session requests, and this module writes one short line per session into
 * it. A line of text rather than a socket, because these two ends already have a
 * protocol and this is deliberately not a second one.
 *
 * Why this is a shared module and not host code: the Android host and rvvm_ash
 * are two implementations of exactly this, and vp_session.c exists for the same
 * reason - one state machine written twice is two that drift. Nothing here knows
 * about a platform: a terminal comes from the core (which knows whether the host
 * has a kernel pty or has to simulate one), and the bytes move through the core
 * too. What a host supplies is a callback for each client and the machine to run
 * on.
 *
 * Threading: the host owns concurrency. The registry below takes a lock because
 * a session can be opened from a connection thread while another connection is
 * arriving, and those are genuinely concurrent; everything else is the host's
 * own ordering, as it already was.
 *
 * Logging: this module does not log. What is worth a line differs per host - the
 * Android bridge has __android_log_print and rvvm_ash has stderr - and the calls
 * that change something say so in their return value.
 */

#ifndef VP_CORE_H
#define VP_CORE_H

#include <stddef.h>

#include <core/rvvm_user.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A core, and a session on it. Both opaque: a host holds the handles and passes
 * them back, and neither is a struct it should be reading fields out of - the
 * terminal behind a session belongs to the core, not to the client that has it. */
typedef struct vp_core         vp_core_t;
typedef struct vp_core_session vp_core_session_t;

/* The terminal size a session starts at, and what a client that has said nothing
 * gets. A real terminal's own default, so a shell is laid out for a real screen
 * before anyone has said otherwise. */
#define VP_CORE_ROWS 24
#define VP_CORE_COLS 80

/*
 * A core, on @machine.
 *
 * Made once per machine and kept for its life: the control terminal is booted into
 * idle as its --control argument, so it has to exist before the guest starts, and
 * every session request goes into it.
 *
 * NULL when no terminal could be had. rvvm_session_pty_errno() then says why,
 * which is the only way a host can tell "no /dev/ptmx" from "this process may not
 * open one" - the same symptom, fixed in entirely different places.
 */
vp_core_t* vp_core_new(rvvm_machine_t* machine);

/* Give the terminal back and end the core. The sessions still attached are
 * released with it: their terminals belonged to this machine, and a machine that
 * has gone cannot honour a terminal. */
void vp_core_free(vp_core_t* core);

/* The core's control terminal, for a host that has to name it itself - which is
 * the one thing a host must do before the guest starts, because idle opens this
 * terminal as its first act. NULL when there is none. */
rvvm_host_pty_t* vp_core_control(vp_core_t* core);
const char*      vp_core_control_path(vp_core_t* core);

/*
 * A new session, sized @rows x @cols, with a shell already asked for.
 *
 * NULL when there is no terminal left, or when the core did not take the
 * request. "Asked for" is the honest word: the shell is forked by the guest when
 * it reads the line, so this returning a session does not mean a shell is
 * running yet - it means one has been requested and will be.
 *
 * Passing 0 for either size uses VP_CORE_ROWS / VP_CORE_COLS.
 */
vp_core_session_t* vp_core_session_open(vp_core_t* core, int rows, int cols);

/* The session's terminal, for a host that needs it (a pty path, a resize, a
 * native fd). NULL once released. */
rvvm_host_pty_t* vp_core_session_pty(vp_core_session_t* s);

/* The terminal's path, which is how the guest names it. Copied into @out. */
const char* vp_core_session_path(vp_core_session_t* s);

/* Tell the session's terminal it has been resized. */
void vp_core_session_resize(vp_core_session_t* s, int rows, int cols);

/* Host -> session: the bytes a client typed. Non-blocking; the full count, a
 * partial count, or negative. */
int64_t vp_core_session_input(vp_core_session_t* s, const void* buf, size_t len);

/* Session -> host: what its shell wrote. Non-blocking - 0 means nothing yet, and
 * negative is a hangup (the shell is gone), which is how a host learns a session
 * ended without being told. */
int64_t vp_core_session_output(vp_core_session_t* s, void* buf, size_t len);

/* Whether the session's guest end is still there. 0 once it has hung up. */
int vp_core_session_alive(vp_core_session_t* s);

/* Close the session's terminal. The hangup is what ends the shell on it - the
 * same thing closing a terminal window does. After this the handle is spent. */
void vp_core_session_release(vp_core_session_t* s);

#ifdef __cplusplus
}
#endif

#endif /* VP_CORE_H */
