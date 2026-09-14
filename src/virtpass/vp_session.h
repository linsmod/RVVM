/*
 * vp_session.h - host-side session state of one guest run
 *
 * What the two hosts (win32_main.c and the Android JNI bridge) were each
 * keeping as their own file-scope globals: the display geometry the guest
 * observes, the geometry last pushed to the presentation surface, and the
 * console line assembly that turns the guest's byte stream into whole lines.
 *
 * Nothing here knows about a platform. The panel is a pair of integers, the
 * surface geometry is a cache of what the host last pushed, and a console line
 * leaves through a callback. That is what lets the same code serve the win32
 * host (DIB + GDI window), the Android host (ANativeWindow) and, eventually,
 * more than one guest at a time - which is the other reason the state is an
 * object and not a global: one session per guest is exactly what a second
 * instance needs, and it is the shape the core already uses for the same job
 * (rvvm_userland_t, one per rvvm_machine_t).
 *
 * Threading: this module takes no locks. The host owns concurrency, as it
 * already did around the globals these fields came from: the Android bridge
 * holds its surface mutex around the geometry calls and its console mutex
 * around the console ones.
 *
 * Logging: this module does not log. The calls that change something say so in
 * their return value, because what is worth a log line differs per host - the
 * Android host logs through __android_log_print at a level of its own choosing,
 * while rvvm_info() below LOG_WARN is invisible there.
 */

#ifndef VP_SESSION_H
#define VP_SESSION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provisional panel, used until a real one is known. */
#define VP_PANEL_DEFAULT_W 640
#define VP_PANEL_DEFAULT_H 480

/* Longest console line assembled before a partial one is emitted as-is. */
#define VP_SESSION_LINE_MAX 2048

/* Pixel formats, spelled out with the values the guest's NDK uses
 * (android/native_window.h): they are part of the guest-facing ABI, not
 * something this module should reach into a platform header for. */
#define VP_FORMAT_RGBA_8888 1
#define VP_FORMAT_RGBX_8888 2
#define VP_FORMAT_RGB_565   4

/* A completed console line: NUL-terminated, without its newline. */
typedef void (*vp_session_line_fn)(void* user, const char* line);

typedef struct vp_session {
    /* ---- Display geometry ----
     * The panel is the guest-visible display: what ANativeWindow_getWidth()
     * answers and what input coordinates are expressed in. The guest's surface
     * geometry is latched from it on first use and then only moved by the guest
     * itself (ANativeWindow_setBuffersGeometry). Keeping the two apart is what
     * lets the host resize the *view* the guest is shown in without the guest
     * ever seeing its buffer move. */
    int32_t panel_w, panel_h;       /* the panel (0 = not known yet)          */
    int32_t panel_ppi;              /* host-owned density                     */
    int32_t init_w, init_h;         /* panel snapshot taken when it latched   */
    int32_t gfx_w, gfx_h, gfx_fmt;  /* the buffer geometry the guest draws in */

    /* What was last pushed onto the presentation surface. Kept so the host can
     * skip a redundant setBuffersGeometry() on every frame; the surface's own
     * geometry is the viewport's, not the guest's. */
    int32_t applied_w, applied_h, applied_fmt;

    /* ---- Console line assembly ---- */
    char   line[VP_SESSION_LINE_MAX];
    size_t line_len;
    int    first_frame;             /* one per run, see note_first_frame()    */

    /* ---- Host hooks ---- */
    vp_session_line_fn on_line;
    void*              on_line_user;
} vp_session_t;

/* Zero everything and take the panel defaults. */
void vp_session_init(vp_session_t* s);

/* ------------------------------------------------------------------
 * Display geometry
 * ------------------------------------------------------------------ */

/*
 * Latch the panel from a real window size, the first time a host has one.
 * Does nothing once the guest has seen a geometry (moving the panel afterwards
 * would desynchronise the guest's buffer from the bytes the host copies out of
 * it), and nothing when the panel is already known.
 *
 * Returns 1 when it latched, 0 when it left the session alone.
 */
int vp_session_latch_panel(vp_session_t* s, int32_t win_w, int32_t win_h);

/*
 * Pin the panel explicitly, before any guest has observed one. Wins over a
 * panel that was inferred from a window size: that inference can only have come
 * from a viewport nobody has rendered into yet.
 *
 * Returns 1 when it was set, 0 when it was refused because the guest already
 * has a geometry.
 */
int vp_session_set_panel(vp_session_t* s, int32_t w, int32_t h);

/* Host-owned panel density (dpi), reported to the guest through
 * AConfiguration. Values <= 0 are ignored. */
void vp_session_set_density(vp_session_t* s, int32_t ppi);

/*
 * The panel size to compose against: the pinned panel, else the snapshot taken
 * when it latched, else the last-resort default. Always positive, which is what
 * makes it usable as the size of a host-owned composition surface without the
 * caller repeating the fallback chain.
 *
 * Either output may be NULL.
 */
void vp_session_panel_size(const vp_session_t* s, int32_t* w, int32_t* h);

/*
 * The geometry the guest should be told about, and should be rendering in.
 * Latches the panel into the guest surface on first use; from then on the value
 * is stable, so the guest allocates its pixel buffer once instead of chasing a
 * moving target. Any of the three outputs may be NULL.
 *
 * Returns 1 when it latched (the caller's cue that the guest will now be
 * rendering at that size), 0 otherwise.
 */
int vp_session_guest_geometry(vp_session_t* s, int32_t* w, int32_t* h, int32_t* fmt);

/*
 * The guest set its own buffer geometry (ANativeWindow_setBuffersGeometry).
 * A concrete width/height redefines the guest surface - the only path by which
 * the guest moves its geometry - while the usual (0, 0, format) call only
 * selects the pixel format and leaves the size alone.
 *
 * Returns 1 when anything changed, 0 when the call was a no-op.
 */
int vp_session_set_guest_geometry(vp_session_t* s, int32_t w, int32_t h, int32_t fmt);

/* Does the presentation surface still need (w, h, fmt) pushed onto it? */
int vp_session_geometry_dirty(const vp_session_t* s, int32_t w, int32_t h, int32_t fmt);

/* Record that (w, h, fmt) is now the surface's geometry. */
void vp_session_geometry_pushed(vp_session_t* s, int32_t w, int32_t h, int32_t fmt);

/*
 * A brand new presentation surface starts with the platform's default geometry,
 * so whatever was pushed onto the old one no longer applies.
 */
void vp_session_forget_geometry(vp_session_t* s);

/*
 * Drop the surface geometry a guest negotiated and go back to the panel: until
 * the next guest asks or sets one, it gets the host default. Hosts with a
 * launcher call this between guests (one guest's SET_BUF must not leak into the
 * next); a host that boots one guest per process never needs it.
 */
void vp_session_reset_surface(vp_session_t* s);

/* ------------------------------------------------------------------
 * Console
 * ------------------------------------------------------------------ */

/*
 * Feed raw guest output (fd 1/2). Whole lines go to on_line as they are
 * completed; a carriage return alone never ends one, and an over-long line is
 * emitted in pieces rather than growing the buffer without bound.
 */
void vp_session_console_output(vp_session_t* s, const char* data, size_t len);

/* Emit a trailing partial line, if any. */
void vp_session_console_flush(vp_session_t* s);

/*
 * Note that a frame reached the surface. Returns 1 the first time in a run and
 * 0 afterwards, so the host can fire its one-shot work (flushing the console
 * line that precedes the frame, telling the UI) exactly once.
 */
int vp_session_note_first_frame(vp_session_t* s);

/*
 * Start a run: flush the previous guest's trailing partial line and re-arm the
 * one-shot first-frame note.
 */
void vp_session_reset_run(vp_session_t* s);

#ifdef __cplusplus
}
#endif

#endif /* VP_SESSION_H */
