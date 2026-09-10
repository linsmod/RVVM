/* Compile smoke test for the generated host-side GL headers.
 * Simulates the future win32_gl_dispatch.c: include the backend header,
 * define the dispatch helpers it owes, include the dispatch tables, and
 * reference everything so unused warnings fire if any. Build:
 *   gcc -c -Wall -Wextra -Werror -I rvvm-on-windows/win32-host tools/gl_dispatch_smoke.c -o /tmp/smoke.o
 */
#include "win32_gl_backend.h"

/* Helpers win32_gl_dispatch.c is expected to provide (float unpack). */
static inline float w32gl_arg_f(int64_t v)
{
    union { float f; uint32_t u; } cvt;
    cvt.u = (uint32_t)v;
    return cvt.f;
}

#include "win32_gl_dispatch_tables.h"

/* Non-static wrapper so the static dispatch functions are referenced. */
void w32gl_dispatch_smoke(uint32_t egl_id, uint32_t gl_id,
                          const int64_t* a, int64_t* ret)
{
    w32gl_dispatch_egl_generic(egl_id, a, ret);
    w32gl_dispatch_gl_generic(gl_id, a, ret);
}

const char* w32gl_backend_smoke(void)
{
    return win32_gl_backend_name();
}
