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
 * Create the host window, register vp_cmdpost callbacks, start plumbing.
 * Call before win32_host_start_guest().
 * Returns false on failure (window creation error).
 */
bool win32_host_init(const char* title, int width, int height);

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

#endif /* WIN32_CMDPOST_BRIDGE_H */
