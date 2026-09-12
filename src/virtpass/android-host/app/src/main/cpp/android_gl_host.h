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

struct ANativeWindow;

/*
 * dlopen the system EGL/GLES (once) and (re)install the GL dispatch callbacks
 * with vp_cmdpost. Safe and required to call before every guest: cmdpost_cleanup()
 * NULLs the callbacks when a guest exits, so a relaunched guest needs the
 * registration redone - same reason jni_register_cmdpost_callbacks() exists.
 * Returns false when the system libraries cannot be loaded (guests then fall
 * back to CPU rendering, as on win32).
 */
bool android_gl_host_init(void);

/*
 * Hand the current SurfaceView window to the GL backend. Called from the
 * JNI surface lifecycle (nativeSetWindow) with the same ANativeWindow the
 * software-rendering path tracks; NULL when the surface is going away.
 *
 * The pointer is only read at eglCreateWindowSurface time and the resulting
 * EGLSurface keeps the window alive for its own lifetime, so no extra
 * ANativeWindow_acquire is taken here.
 */
void android_gl_set_native_window(struct ANativeWindow* window);

#endif /* ANDROID_GL_HOST_H */
