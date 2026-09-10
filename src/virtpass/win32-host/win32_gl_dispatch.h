#ifndef WIN32_GL_DISPATCH_H
#define WIN32_GL_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

/* GLES2 enum constants (avoid including gl2.h on Win32) */
#define GL_RGBA         0x1908
#define GL_UNSIGNED_BYTE 0x1401

/* GL active state shared between dispatch and bridge. */
extern bool g_gl_active;

/* Dispatch callbacks — registered with vp_cmdpost in win32_host_init. */
void on_egl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret);
void on_gl_dispatch(uint32_t fn_id, const int64_t* args, int64_t* ret);

#endif /* WIN32_GL_DISPATCH_H */
