/*
 * win32_cmdpost_bridge.h - Windows host-side bridge for the RVVM
 * Android-userland emulator (skeleton).
 *
 * The guest (riscv64, statically linked against vp_ndk_stub) issues
 * hypercalls that rvvm_user.c forwards to vp_cmdpost. On Android those
 * callbacks are implemented by jni_bridge.c; this file implements the
 * same callback contract on top of Win32 (GDI DIB surface, mouse, keys,
 * lifecycle, stub sensors).
 *
 * Reused from the shared virtpass layer unmodified:
 *   src/virtpass/vp_cmdpost.{c,h}          (dispatch + queues)
 *   include/virtpass/vp_android.h          (guest ABI constants)
 */

#ifndef WIN32_CMDPOST_BRIDGE_H
#define WIN32_CMDPOST_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Create the host window, initialize the layer-1 virtual display, register
 * vp_cmdpost callbacks. Call before win32_host_start_guest().
 *
 * Two independent layers:
 *  - win_w/win_h  : layer 2, the OS window (a pure viewport; the panel is
 *                   blitted 1:1 and leftover area is filled black).
 *  - virt_w/virt_h/virt_ppi : layer 1, the virtual panel the guest renders
 *                   into - the "target machine" of the remote-desktop-like
 *                   model. These are the only display parameters the guest can
 *                   observe, and only through the public NDK ABI
 *                   (ANativeWindow_getWidth/Height, AConfiguration_*).
 * Pass 0 for virt_w/virt_h to make the panel follow the window size; pass 0
 * for any value to take its default.
 *
 * Returns false on failure (window creation error).
 */
bool win32_host_init(const char* title, int win_w, int win_h,
                     int virt_w, int virt_h, int virt_ppi, bool launcher);

/*
 * Enable the Android-style picker (launcher): a dropdown listing the guest
 * programs found in assets_dir plus Run / Stop / Exit buttons. When idle
 * (no guest running) the picker is shown; Run boots the selected guest into
 * the existing window and Stop requests a graceful teardown. When a guest
 * exits for any reason the picker is re-shown instead of closing the window.
 *
 * Call after win32_host_init() and before win32_host_message_loop(). Pass
 * NULL for assets_dir to scan the current directory.
 */
bool win32_host_set_launcher(const char* assets_dir);

/*
 * Launch the guest Linux ELF (argv[0] = ELF path, argv[1..] = guest args)
 * on a dedicated thread. Does not block.
 */
bool win32_host_start_guest(int argc, char** argv);

/* Run the Win32 message pump; returns when the window is closed. */
int win32_host_message_loop(void);

/* Exit code of the guest process (valid after it terminates). */
int win32_host_guest_exit_code(void);

/* Release resources; call after win32_host_message_loop() returns. */
void win32_host_shutdown(void);

/* GL present callbacks — called by win32_gl_dispatch.c on eglSwapBuffers. */
void present_frame(const uint8_t* rows, int32_t w, int32_t h,
                   int32_t src_fmt, bool rows_bottom_up);
void present_gl_frame(void);

/* A pure-EGL guest never calls ANativeWindow_setBuffersGeometry, so the host
 * would present at whatever default it holds. eglCreateWindowSurface reports
 * the surface attributes here instead, which is the size the guest actually
 * renders at (layer 1). cw/ch <= 0 leaves the current value alone. */
void present_gl_set_surface_size(int32_t cw, int32_t ch);

/* Current virtual panel size. For callers that must size an offscreen backing
 * surface themselves (the GL dispatch backs a guest window surface with a
 * panel-sized pbuffer). Both values stay positive: the panel defaults to
 * 1024x768 until the first real window size is known. */
void present_gl_panel_size(int32_t* w, int32_t* h);

#endif /* WIN32_CMDPOST_BRIDGE_H */
