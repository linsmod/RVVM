/*
 * android_gl_host.h - system EGL/GLES backend for the Android host
 *
 * The win32 host has to hunt for ANGLE DLLs because Windows ships no GLES
 * implementation. The Android host IS Android: the system libEGL.so and
 * libGLESv2.so are right there, and the host owns a real ANativeWindow (the
 * SurfaceView). So unlike the win32 path there is no pbuffer fallback, no DIB
 * composition and no present_gl_frame(): eglCreateWindowSurface binds the
 * real SurfaceView and eglSwapBuffers presents through SurfaceFlinger.
 *
 * Everything else - the marshalled gl_call ABI, the fn_id space, the pointer
 * translation rules - is shared with the win32 host via the generated
 * virtpass/vp_gl_host_types.h + virtpass/vp_gl_dispatch_tables.h.
 */

#ifndef ANDROID_GL_HOST_H
#define ANDROID_GL_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "virtpass/vp_cmdpost.h"  /* vp_cmdpost_t: the instance the callbacks go into */

struct ANativeWindow;

/*
 * Bind the host's vp_cmdpost instance. The GL dispatch callbacks are registered
 * into it, so this has to be called before the first android_gl_host_init().
 */
void android_gl_set_cmdpost(vp_cmdpost_t* inst);

/*
 * dlopen the system EGL/GLES (once) and (re)install the GL dispatch callbacks
 * with vp_cmdpost. Safe and required to call before every guest: it re-installs
 * the callbacks into the host's instance, the same thing
 * jni_register_cmdpost_callbacks() does for the rest of the bridge.
 * Returns false when the system libraries cannot be loaded (guests then fall
 * back to CPU rendering, as on win32).
 */
bool android_gl_host_init(void);

/*
 * Tell the GL backend that the SurfaceView window was attached or detached.
 * Called from the JNI surface lifecycle (nativeSetWindow) with the same
 * ANativeWindow the software-rendering path tracks; NULL when the surface is
 * going away.
 *
 * The pointer is NOT kept. The host releases the previous wrapper the moment a
 * new one arrives (the card is resized on every run), so a window cached here
 * could be freed before the next guest call; the window is instead taken with a
 * reference held across the platform call that needs it, through
 * jni_wait_surface() in jni_bridge.c (which also gives a window that is on its
 * way a moment to appear). This notification is what keeps the attach/detach
 * visible in the log.
 */
void android_gl_set_native_window(struct ANativeWindow* window);

#endif /* ANDROID_GL_HOST_H */
