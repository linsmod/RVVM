/*
 * vp_console.h - the guest console as a byte pipe (see vp_console.c)
 *
 * The packet ids and the 1+4 byte header are adb's ShellProtocol, so that a
 * client written against adb's shape works here unchanged: stdin, stdout,
 * stderr, exit, close-stdin and window size, multiplexed on one connection.
 */
#pragma once

#include <stddef.h>

#include "rvvm_user.h"

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

#ifdef __cplusplus
}
#endif
