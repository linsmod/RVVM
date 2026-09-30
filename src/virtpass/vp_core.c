/*
 * vp_core.c - the host side of a core (see vp_core.h)
 *
 * Everything platform-specific is already below this line: the terminal comes
 * from rvvm_session_pty_* and the bytes move through the same pair, and the core
 * knows on its own whether the host has a kernel pty or has to simulate one.
 * What is left here is the part that is a host's business on either platform -
 * holding the control terminal, asking for a shell, and getting the bytes to and
 * from one client.
 */

#include <stdlib.h>
#include <string.h>

#include "virtpass/vp_core.h"

/* How many sessions a host may hold at once.
 *
 * A bound rather than a pool: a session's terminal belongs to a live client and
 * goes when it does, so this is the most that can exist at the same time. It sits
 * under the core's own pty pool so the refusal is "no sessions" rather than a
 * terminal that cannot be had. */
#define VP_CORE_MAX_SESSIONS 32

struct vp_core_session {
    vp_core_t*          core;
    rvvm_host_pty_t*    pty;
    char                path[64];
};

struct vp_core {
    rvvm_machine_t*     machine;
    rvvm_host_pty_t*    control;
    char                control_path[64];
    vp_core_session_t*  sessions[VP_CORE_MAX_SESSIONS];
    int                 count;
};

vp_core_t* vp_core_new(rvvm_machine_t* machine)
{
    if (!machine) {
        return NULL;
    }
    vp_core_t* c = (vp_core_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->machine = machine;
    /* Made here and kept: the guest is told this terminal's path and opens it as
     * its first act, so it has to exist before the guest starts - which is why
     * a host creates the core between making the machine and starting the guest,
     * and not after. */
    c->control = rvvm_session_pty_new(machine);
    if (!c->control) {
        free(c);
        return NULL;
    }
    rvvm_session_pty_name(c->control, c->control_path, sizeof(c->control_path));
    rvvm_session_pty_resize(c->control, VP_CORE_ROWS, VP_CORE_COLS);
    return c;
}

void vp_core_free(vp_core_t* core)
{
    if (!core) {
        return;
    }
    /* The sessions first, in order: their terminals belonged to this machine,
     * and a machine that is going away cannot honour a terminal - so each shell
     * gets its hangup before the control channel does, rather than all of them
     * discovering it at once. */
    for (int i = 0; i < VP_CORE_MAX_SESSIONS; i++) {
        if (core->sessions[i]) {
            vp_core_session_release(core->sessions[i]);
        }
    }
    if (core->control) {
        rvvm_session_pty_free(core->control);
        core->control = NULL;
    }
    free(core);
}

rvvm_host_pty_t* vp_core_control(vp_core_t* core)
{
    return core ? core->control : NULL;
}

const char* vp_core_control_path(vp_core_t* core)
{
    return (core && core->control_path[0]) ? core->control_path : NULL;
}

vp_core_session_t* vp_core_session_open(vp_core_t* core, int rows, int cols)
{
    if (!core || !core->control) {
        return NULL;
    }
    if (core->count >= VP_CORE_MAX_SESSIONS) {
        return NULL;
    }
    if (rows <= 0) {
        rows = VP_CORE_ROWS;
    }
    if (cols <= 0) {
        cols = VP_CORE_COLS;
    }

    rvvm_host_pty_t* pty = rvvm_session_pty_new(core->machine);
    if (!pty) {
        return NULL;
    }
    char path[64] = { 0 };
    if (rvvm_session_pty_name(pty, path, sizeof(path)) < 0 || !path[0]) {
        rvvm_session_pty_free(pty);
        return NULL;
    }
    /* Sized before the request, not after: the shell's first prompt is laid out
     * against this, and a terminal that gets its size once the shell is already
     * running costs a full-screen program a wrong frame before anything tells it
     * to redraw. */
    rvvm_session_pty_resize(pty, rows, cols);

    vp_core_session_t* s = (vp_core_session_t*)calloc(1, sizeof(*s));
    if (!s) {
        rvvm_session_pty_free(pty);
        return NULL;
    }
    s->core = core;
    s->pty  = pty;
    snprintf(s->path, sizeof(s->path), "%s", path);

    for (int i = 0; i < VP_CORE_MAX_SESSIONS; i++) {
        if (!core->sessions[i]) {
            core->sessions[i] = s;
            core->count++;
            break;
        }
    }

    /* The request, as one line into the control terminal. The size rides along
     * for the same reason it is applied above: so the shell is laid out for the
     * client from its first prompt.
     *
     * Written after the session is registered, so that a shell which comes up
     * before this returns already has a terminal this core is holding - the
     * reverse order leaves a window in which the guest has a terminal nobody
     * owns, which is a session whose output goes nowhere. */
    char req[128];
    snprintf(req, sizeof(req), "spawn %s %d %d\n", path, cols, rows);
    if (rvvm_session_pty_input(core->control, req, strlen(req)) <= 0) {
        vp_core_session_release(s);
        return NULL;
    }
    return s;
}

rvvm_host_pty_t* vp_core_session_pty(vp_core_session_t* s)
{
    return s ? s->pty : NULL;
}

const char* vp_core_session_path(vp_core_session_t* s)
{
    return s ? s->path : NULL;
}

void vp_core_session_resize(vp_core_session_t* s, int rows, int cols)
{
    if (s && s->pty) {
        rvvm_session_pty_resize(s->pty, rows, cols);
    }
}

int64_t vp_core_session_input(vp_core_session_t* s, const void* buf, size_t len)
{
    if (!s || !s->pty) {
        return 0;
    }
    return rvvm_session_pty_input(s->pty, buf, len);
}

int64_t vp_core_session_output(vp_core_session_t* s, void* buf, size_t len)
{
    if (!s || !s->pty) {
        return 0;
    }
    return rvvm_session_pty_output(s->pty, buf, len);
}

int vp_core_session_alive(vp_core_session_t* s)
{
    return (s && s->pty) ? rvvm_session_pty_alive(s->pty) : 0;
}

void vp_core_session_release(vp_core_session_t* s)
{
    if (!s) {
        return;
    }
    vp_core_t* core = s->core;
    if (core) {
        for (int i = 0; i < VP_CORE_MAX_SESSIONS; i++) {
            if (core->sessions[i] == s) {
                core->sessions[i] = NULL;
                core->count--;
                break;
            }
        }
    }
    if (s->pty) {
        rvvm_session_pty_free(s->pty);
        s->pty = NULL;
    }
    free(s);
}
