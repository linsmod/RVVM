#!/usr/bin/env python3
"""Audit how gen_gl_abi.py translates every pointer argument of the GL/EGL ABI.

This is a bring-up safety net for the marshalled GL/EGL proxy: a wrongly
classified pointer argument either crashes the host GL implementation (a guest
address dereferenced as host memory) or crashes the guest (a host address used
as guest memory). Both are silent until the offending entry point is first
called, so enumerate every case here instead.

For each pointer parameter the report says which classification the generator
applies and, for the ambiguous ones, why:

  opaque  - host handle that the guest only ever passes back
            (EGLDisplay/EGLConfig/EGLContext/EGLSurface, EGLNative*, and the
            GLES3 GLsync). Passed through untouched, never dereferenced.
  data    - guest buffer; translated with rvvm_user_guest_ptr() before use.
  offset  - guest address for client-side arrays, but a byte offset into the
            bound buffer object for VBO rendering (glVertexAttribPointer,
            glVertexAttribIPointer, glDrawElements, glDrawElementsInstanced,
            glDrawRangeElements). The host resolves it from live GL state with
            vpgl_ptr(): a non-zero binding of GL_ARRAY_BUFFER (resp.
            GL_ELEMENT_ARRAY_BUFFER) means the value is an offset.
  out-str - glGetString/glGetStringi/eglQueryString: host-owned string, copied
            into the guest scratch buffer.

Sections at the end list the parameter shapes the type system cannot vouch
for, because those are where a wrong classification hides:

  * `void*` / `void**` params -- the C type says nothing about intent.
  * pointer-returning functions -- a host pointer handed to the guest is only
    valid if the guest treats it as an opaque handle.

Entry points that gen_gl_abi.EXCLUDED_FNS keeps out of the ABI never reach the
host at all; they are listed at the end of the report rather than audited.

Usage: python tools/audit_gl_ptr.py [--check]

`--check` exits non-zero when a parameter falls outside the classifications
above, so this can gate a rebuild.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_gl_abi as g  # noqa: E402

# Parameters that must NOT be translated: opaque host handles.
OPAQUE = g.IMPLICIT_PTR | g.NATIVE_TYPES


def classify(fn, idx, t):
    """Mirror host_arg_expr()'s decision, and name the reason."""
    if not t.is_pointer():
        return "scalar", "-"
    if fn["name"] == g.PROC_ADDRESS_FN:
        return "guest-only", "never marshalled; guest stub looks the name up"
    if t.base in OPAQUE:
        return "opaque", "host handle"
    if (fn["name"], idx) in g.ARG_OVERRIDES:
        return "data", "ARG_OVERRIDES"
    if (fn["name"], idx) in g.ARRAY_PTR_ARGS:
        return "offset", "guest VA, or VBO offset when GL_ARRAY_BUFFER is bound"
    if (fn["name"], idx) in g.ELEMENT_PTR_ARGS:
        return "offset", "guest VA, or VBO offset when GL_ELEMENT_ARRAY_BUFFER is bound"
    return "data", "rvvm_user_guest_ptr"


def ret_kind(f):
    r = f["ret"]
    if not r.is_pointer():
        return "scalar", "-"
    if f["name"] == g.PROC_ADDRESS_FN:
        return "guest-only", "resolved in the guest stub; never reaches the host"
    if r.base in OPAQUE:
        return "opaque", "host handle"
    if f["name"] in g.STRING_RET_FNS:
        return "out-str", "host string -> guest retbuf"
    return "data?", "NEEDS REVIEW: raw host pointer to guest"


def load():
    egl = open(g.EGL_H, encoding="utf-8", errors="replace").read()
    eglf = [f for f in g.parse_prototypes(egl, ("EGLAPI", "EGLAPIENTRY"))
            if f["name"] in set(g.EGL_WHITELIST)]
    # Exactly the set the generator emits: GL_HEADERS merged, EXCLUDED_FNS
    # dropped. Auditing a different list than the one that ships would be
    # worthless.
    return eglf, g.load_gl_functions()


def main():
    check = "--check" in sys.argv
    eglf, glf = load()

    rows = []
    for f in eglf + glf:
        for i, (t, _) in enumerate(f["params"]):
            if t.is_pointer():
                rows.append((f["name"], i, t) + classify(f, i, t))

    print("%-30s %-3s %-26s %-8s %s" % ("function", "#", "type", "kind", "why"))
    print("-" * 104)
    for name, i, t, kind, why in rows:
        print("%-30s %-3d %-26s %-8s %s" % (name, i, t, kind, why))

    print()
    print("=== `void*` / `void**` params (type does not express intent) ===")
    for name, i, t, kind, why in rows:
        if t.base == "void":
            print("  %-30s arg%d  -> %-8s (%s)" % (name, i, kind, why))

    print()
    print("=== pointer-returning functions ===")
    bad = []
    for f in eglf + glf:
        if f["ret"].is_pointer():
            kind, why = ret_kind(f)
            print("  %-30s -> %-8s %s" % (f["name"], kind, why))
            if kind == "data?":
                bad.append(f["name"])

    print()
    print("=== entry points excluded from the marshalled ABI ===")
    for name in sorted(g.EXCLUDED_FNS):
        print("  %-30s -> out of the ABI (see gen_gl_abi.EXCLUDED_FNS)" % name)

    print()
    print("%d pointer params, %d functions" % (len(rows), len(eglf) + len(glf)))
    if bad:
        print("NEEDS REVIEW: " + ", ".join(bad))
    if check and bad:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
