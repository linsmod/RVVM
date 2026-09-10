#include "win32_gl_backend.h"

#include <stdio.h>
#include <string.h>

static float w32gl_arg_f(int64_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

#include "win32_gl_dispatch_tables.h"
#include "win32_cmdpost_bridge.h" /* present_gl_frame */

bool g_gl_active = false;

/* ============================================================
 * EGL dispatch: 3 special cases before the generic table
 * ============================================================ */

void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    switch (fn_id) {
    case EGL_FN_GETDISPLAY:
        *ret = (int64_t)(intptr_t)p_eglGetDisplay((w32gl_void*)0);
        break;
    case EGL_FN_CREATEWINDOWSURFACE: {
        if (!g_gl_active) { g_gl_active = true; printf("[winhost] GL mode active\n"); }
        *ret = (int64_t)(intptr_t)p_eglCreatePbufferSurface(
            (w32gl_EGLDisplay)(uintptr_t)args[0],
            (w32gl_EGLConfig)(uintptr_t)args[1],
            (const w32gl_EGLint*)args[2]);
        break;
    }
    case EGL_FN_SWAPBUFFERS:
        present_gl_frame();
        *ret = 1;
        break;
    default:
        w32gl_dispatch_egl_generic(fn_id, args, ret);
        break;
    }
}

/* ============================================================
 * GLES dispatch: all fn_id 1-142 handled by generic table.
 * Keep hook points for future specialization.
 * ============================================================ */

void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret)
{
    switch (fn_id) {
    default:
        w32gl_dispatch_gl_generic(fn_id, args, ret);
        break;
    }
}
