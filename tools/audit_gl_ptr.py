#!/usr/bin/env python3
"""Audit how gen_gl_abi.py translates every pointer argument of the GL/EGL ABI.

Prints, for each function and pointer parameter, the classification the
generator will apply, then flags the ones whose classification does not match
the documented intent:

  opaque  - host handle (EGLDisplay/EGLConfig/EGLContext/EGLSurface and the
            EGLNative* types). Passed through untouched; never dereferenced.
  data    - a guest buffer. Must be translated with rvvm_user_guest_ptr().
  offset  - glVertexAttribPointer/glDrawElements: guest address when drawing
            from client memory, byte offset into the bound VBO otherwise.
  out-str - glGetString/eglQueryString: host-owned string copied into the
            guest scratch buffer.

A `void*`/`void**` parameter is where mistakes hide: the C type says nothing,
so the classification has to be right by hand. Those are listed explicitly.
"""

import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_gl_abi as g  # noqa: E402

# Parameters that must NOT be translated (opaque host handles).
OPAQUE = set(g.IMPLICIT_PTR | g.NATIVE_TYPES)


def classify(fn, idx, t):
    """Mirror host_arg_expr()'s decision, and say why."""
    if not t.is_pointer():
        return "scalar", "-"
    if t.base in OPAQUE:
        return "opaque", "host handle"
    if (fn["name"], idx) in g.ARG_OVERRIDES:
        return "data", "ARG_OVERRIDES"
    if (fn["name"], idx) in g.OFFSET_PTR_ARGS:
        return "offset", "guest VA or VBO byte offset"
    if fn["name"] in g.STRING_RET_FNS:
        return "out-str", "host string -> guest retbuf"
    return "data", "rvvm_user_guest_ptr"


def main():
    gl = open(g.GL2_H, encoding="utf-8", errors="replace").read()
    egl = open(g.EGL_H, encoding="utf-8", errors="replace").read()
    glf = g.parse_prototypes(gl, ("GL_APICALL", "GL_APIENTRY"))
    eglf = [f for f in g.parse_prototypes(egl, ("EGLAPI", "EGLAPIENTRY"))
            if f["name"] in set(g.EGL_WHITELIST)]

    rows = []
    for f in eglf + glf:
        for i, (t, _) in enumerate(f["params"]):
            if not t.is_pointer():
                continue
            kind, why = classify(f, i, t)
            rows.append((f["name"], i, t, kind, why))

    print("%-30s %-3s %-28s %-8s %s" % ("function", "#", "type", "kind", "why"))
    print("-" * 100)
    for name, i, t, kind, why in rows:
        print("%-30s %-3d %-28s %-8s %s" % (name, i, t, kind, why))

    print()
    print("=== every `void*` / `void**` parameter (type tells us nothing) ===")
    for name, i, t, kind, why in rows:
        if t.base == "void":
            print("  %-30s arg%d  -> %-8s (%s)" % (name, i, kind, why))

    print()
    print("=== parameters returned to the guest as a raw host pointer ===")
    for f in eglf + glf:
        if f["ret"].is_pointer() and f["ret"].base not in OPAQUE:
            print("  %-30s -> %s" % (f["name"], f["ret"]))

    print()
    print("counts: %d pointer params over %d functions" %
          (len(rows), len(eglf) + len(glf)))


if __name__ == "__main__":
    main()
