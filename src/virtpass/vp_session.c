/*
 * vp_session.c - see vp_session.h for what this is and why it is an object.
 *
 * The geometry half is a transcription of what the Android host kept in
 * jni_bridge.c (g_virt_*, g_init_*, g_surf_*, g_applied_*) and the win32 host
 * kept in win32_cmdpost_bridge.c; the console half likewise (g_console_buf,
 * g_console_len). Both were the same state machine written twice, and the two
 * copies are what this module exists to end: from here on the hosts differ only
 * in how they get a window and how they show the bytes.
 */

#include <string.h>

#include "virtpass/vp_session.h"

void vp_session_init(vp_session_t* s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->panel_ppi = 0;
    s->gfx_fmt   = VP_FORMAT_RGBA_8888;
}

/* ==================================================================
 * Display geometry
 * ================================================================== */

int vp_session_latch_panel(vp_session_t* s, int32_t win_w, int32_t win_h)
{
    if (s->gfx_w > 0 && s->gfx_h > 0) {
        return 0;   /* the guest already saw a geometry */
    }
    if (s->panel_w > 0 && s->panel_h > 0) {
        return 0;   /* already known */
    }
    if (win_w <= 0 || win_h <= 0) {
        return 0;
    }

    s->panel_w = win_w;
    s->panel_h = win_h;
    s->init_w  = win_w;
    s->init_h  = win_h;
    return 1;
}

int vp_session_set_panel(vp_session_t* s, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (s->gfx_w > 0 || s->gfx_h > 0) {
        return 0;   /* the guest is rendering into a geometry already */
    }

    s->panel_w = w;
    s->panel_h = h;
    s->init_w  = w;
    s->init_h  = h;
    return 1;
}

void vp_session_set_density(vp_session_t* s, int32_t ppi)
{
    if (s && ppi > 0) {
        s->panel_ppi = ppi;
    }
}

void vp_session_panel_size(const vp_session_t* s, int32_t* w, int32_t* h)
{
    if (w) {
        *w = (s->panel_w > 0) ? s->panel_w
            : (s->init_w  > 0) ? s->init_w : VP_PANEL_DEFAULT_W;
    }
    if (h) {
        *h = (s->panel_h > 0) ? s->panel_h
            : (s->init_h  > 0) ? s->init_h : VP_PANEL_DEFAULT_H;
    }
}

/* Formats a host may hold. RGBA and RGBX are the same 4-byte layout with the
 * fourth byte unused, and both hosts' present paths treat them alike (4 bytes
 * per pixel, red first); RGB_565 is the narrow one. Anything else is not a
 * format either host could present, so it never becomes the surface's. */
static int format_known(int32_t fmt)
{
    return fmt == VP_FORMAT_RGBA_8888
        || fmt == VP_FORMAT_RGBX_8888
        || fmt == VP_FORMAT_RGB_565;
}

int vp_session_guest_geometry(vp_session_t* s, int32_t* w, int32_t* h, int32_t* fmt)
{
    int latched = 0;

    if (s->gfx_w <= 0 || s->gfx_h <= 0) {
        vp_session_panel_size(s, &s->gfx_w, &s->gfx_h);
        latched = 1;
    }

    if (!format_known(s->gfx_fmt)) {
        s->gfx_fmt = VP_FORMAT_RGBA_8888;
    }

    if (w)   *w   = s->gfx_w;
    if (h)   *h   = s->gfx_h;
    if (fmt) *fmt = s->gfx_fmt;
    return latched;
}

int vp_session_set_guest_geometry(vp_session_t* s, int32_t w, int32_t h, int32_t fmt)
{
    int changed = 0;

    if (w > 0 && h > 0) {
        if (s->gfx_w != w || s->gfx_h != h) {
            changed = 1;
        }
        s->gfx_w = w;
        s->gfx_h = h;
        if (s->panel_w <= 0 || s->panel_h <= 0) {
            s->panel_w = w;
            s->panel_h = h;
        }
    }
    if (format_known(fmt)) {
        if (s->gfx_fmt != fmt) {
            changed = 1;
        }
        s->gfx_fmt = fmt;
    }

    /* The real surface has to be told again at the next lock, whatever the
     * guest asked for. */
    vp_session_forget_geometry(s);
    return changed;
}

int vp_session_geometry_dirty(const vp_session_t* s, int32_t w, int32_t h, int32_t fmt)
{
    return s->applied_w != w || s->applied_h != h || s->applied_fmt != fmt;
}

void vp_session_geometry_pushed(vp_session_t* s, int32_t w, int32_t h, int32_t fmt)
{
    s->applied_w   = w;
    s->applied_h   = h;
    s->applied_fmt = fmt;
}

void vp_session_forget_geometry(vp_session_t* s)
{
    s->applied_w   = 0;
    s->applied_h   = 0;
    s->applied_fmt = 0;
}

void vp_session_reset_surface(vp_session_t* s)
{
    /* Zeroing the geometry is what makes the next query re-latch from the
     * panel; the format goes back to the 4-byte default with it. */
    s->gfx_w = 0;
    s->gfx_h = 0;
    s->gfx_fmt = 0;
    vp_session_forget_geometry(s);
}

/* ==================================================================
 * Console
 * ================================================================== */

/* The guest's byte stream is not all printable: a control byte would either
 * break the UTF-8 conversion on the way to the UI or move the cursor of
 * whatever renders the line. Tabs keep their meaning, the rest becomes '?'. */
static char console_sanitise(char c)
{
    unsigned char u = (unsigned char)c;
    if (u == '\t') return c;
    if (u < 0x20 || u > 0x7e) return '?';
    return c;
}

void vp_session_console_flush(vp_session_t* s)
{
    if (s->line_len == 0) {
        return;
    }
    s->line[s->line_len] = '\0';
    if (s->on_line) {
        s->on_line(s->on_line_user, s->line);
    }
    s->line_len = 0;
}

void vp_session_console_output(vp_session_t* s, const char* data, size_t len)
{
    if (!s || !data) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            vp_session_console_flush(s);
        } else if (c == '\r') {
            /* CR alone never ends a guest line; LF does. */
        } else if (s->line_len >= sizeof(s->line) - 1) {
            vp_session_console_flush(s);   /* oversized line: emit as-is */
            s->line[s->line_len++] = console_sanitise(c);
        } else {
            s->line[s->line_len++] = console_sanitise(c);
        }
    }
}

int vp_session_note_first_frame(vp_session_t* s)
{
    if (s->first_frame) {
        return 0;
    }
    s->first_frame = 1;
    return 1;
}

void vp_session_reset_run(vp_session_t* s)
{
    vp_session_console_flush(s);
    s->first_frame = 0;
}
