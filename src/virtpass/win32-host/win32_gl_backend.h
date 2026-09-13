/*
 * GENERATED FILE - produced by tools/gen_gl_abi.py - DO NOT EDIT BY HAND.
 *
 * Source of truth: NDK sysroot headers GLES2/gl2.h + GLES3/gl3.h + EGL/egl.h
 * (parsed; gl3.h is merged after gl2.h so the GLES2 ids never move).
 * Regenerate with:  python tools/gen_gl_abi.py
 *
 * ABI notes:
 *  - fn_id macros are the single source of truth shared by the guest stubs,
 *    src/virtpass/vp_cmdpost.c and both host GL dispatches.
 *  - gl_call.args has 12 slots. glTexSubImage3D (GLES3) needs 11 and
 *    glCompressedTexSubImage2D (GLES2) 9; the extra slot keeps
 *    GL_CALL_RETBUF_SLOT above every real parameter list. Guest and host are
 *    rebuilt together, so widening it is an internal ABI change only.
 *  - Floats travel bit-packed through the int64_t slots; pointers travel as
 *    guest virtual addresses. Guest memory is NOT mapped into the host, so
 *    the host dispatch translates every data pointer argument with
 *    rvvm_user_guest_ptr() and, for calls that hand back a host-owned string
 *    (glGetString/glGetStringi/eglQueryString), copies it through the guest
 *    scratch buffer offered in args[GL_CALL_RETBUF_SLOT].
 *  - Opaque host values (EGLDisplay/Config/Surface/Context, GLsync) are only
 *    passed back by the guest and never translated.
 *  - The overloaded pointer arguments (glVertexAttribPointer/IPointer,
 *    glDrawElements/Instanced, glDrawRangeElements) travel as their bare
 *    value; the host reads it as a byte offset when a buffer is bound to the
 *    matching target and as a guest address otherwise (vpgl_ptr()).
 */

#ifndef WIN32_GL_BACKEND_H
#define WIN32_GL_BACKEND_H

#include "virtpass/vp_gl_host_types.h"

/* ---- backend lifecycle -------------------------------------------- */

/*
 * Load libEGL.dll + libGLESv2.dll and resolve all entry points.
 * Idempotent and lazily cached: first call does the work, later calls are
 * no-ops returning the cached result. Selection:
 *
 *   RVVM_GL_BACKEND   = angle | swiftshader | off   (default: angle,
 *                     falling back to swiftshader if angle fails)
 *   RVVM_GL_DLL_DIR   = explicit directory containing libEGL.dll and
 *                     libGLESv2.dll (skips backend lookup)
 *
 * Without RVVM_GL_DLL_DIR the Android SDK emulator tree is searched:
 *   <sdk>/emulator/lib64/gles_angle, gles_swiftshader, gles_angle9, ...
 * with <sdk> taken from ANDROID_SDK_ROOT / ANDROID_HOME, then the per-user
 * install under LOCALAPPDATA/Android/Sdk or USERPROFILE/AppData/Local.
 *
 * When no backend can be loaded every GL/EGL dispatch returns 0, which
 * makes guests fall back to the CPU rendering path (WINDOW_LOCK/UNLOCK).
 */
bool win32_gl_backend_load(void);

/* True after a successful win32_gl_backend_load(). */
bool win32_gl_backend_ready(void);

/* Backend name for logs ("angle", "swiftshader", "" when unloaded). */
const char* win32_gl_backend_name(void);

/* Unload the backend (called from win32_host_shutdown). */
void win32_gl_backend_unload(void);

#endif /* WIN32_GL_BACKEND_H */
