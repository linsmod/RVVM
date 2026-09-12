#!/usr/bin/env python3
"""
gen_gl_abi.py - Generate the GL/EGL guest stubs and host-side dispatch tables
for the RVVM Android-userland emulator (Phase 3: EGL+GLES).

Source of truth: the NDK's own EGL/egl.h and GLES2/gl2.h headers (parsed),
so signatures are canonical and zero-maintenance.

Outputs (all marked GENERATED - do not edit by hand):
  include/virtpass/vp_gl.h            shared ABI: types, constants, fn_id
                                      macros, gl_call struct, guest API
                                      prototypes
  src/virtpass/vp_gl_stub.c           guest stubs (142 functions)
  src/virtpass/win32-host/win32_gl_backend.h
                                      host types (w32gl_*), PFN typedefs,
                                      resolved procs
  src/virtpass/win32-host/win32_gl_dispatch_tables.h
                                      host generic dispatch switches

Usage:  python tools/gen_gl_abi.py
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

NDK_SYSROOT = os.environ.get(
    "NDK_SYSROOT",
    r"H:\AndroidSdk\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include",
)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUEST_INC_DIR = os.path.join(REPO_ROOT, "include", "virtpass")
GUEST_SRC_DIR = os.path.join(REPO_ROOT, "src", "virtpass")
HOST_DIR = os.path.join(REPO_ROOT, "src", "virtpass", "win32-host")

GL2_H = os.path.join(NDK_SYSROOT, "GLES2", "gl2.h")
EGL_H = os.path.join(NDK_SYSROOT, "EGL", "egl.h")

# fn_id bases (single source of truth, consumed by guest stub, vp_cmdpost
# and the win32 host dispatch)
GL_FN_BASE = 1
EGL_FN_BASE = 0x100

# Syscall numbers for the two new marshalled hypercalls live in the shared
# ABI header virtpass/vp_syscall.h (SYS_GL_CALL / SYS_EGL_CALL); the generated
# guest header includes it rather than redefining them, so rvvm_user.c and the
# guest stub can never disagree about the numbers.

# EGL core subset exposed to the guest (Phase 3 scope; add names here to
# widen the surface - everything else is generated automatically).
EGL_WHITELIST = [
    "eglGetError",
    "eglGetDisplay",
    "eglInitialize",
    "eglTerminate",
    "eglChooseConfig",
    "eglGetConfigAttrib",
    "eglCreateWindowSurface",
    "eglCreatePbufferSurface",
    "eglDestroySurface",
    "eglCreateContext",
    "eglDestroyContext",
    "eglMakeCurrent",
    "eglSwapBuffers",
    "eglQuerySurface",
    "eglGetProcAddress",
    # Returns a host constant string; the host copies it into the guest
    # scratch buffer offered in args[GL_CALL_RETBUF_SLOT].
    "eglQueryString",
]

# Number of argument slots in gl_call (glCompressedTexSubImage2D needs 9)
GL_CALL_MAX_ARGS = 9

# Scratch buffer the guest offers via args[GL_CALL_RETBUF_SLOT] for functions
# that hand back a host-owned string (glGetString / eglQueryString). It must
# outlive the call, so the guest stub owns it as a file-scope buffer - it
# cannot live inside gl_call, whose `&_c.retbuf` would be a stack address.
GL_CALL_RETBUF_CAP = 8192

# ---------------------------------------------------------------------------
# Pointer translation tables (see win32_gl_dispatch.c for the host helpers)
# ---------------------------------------------------------------------------

# Argument overrides: (function name, param index) -> C expression consuming
# `a` and producing the host argument. Needed where a parameter typed `void*`
# is a guest data pointer rather than an opaque handle.
ARG_OVERRIDES = {
    # glShaderSource passes an array of `count` guest string pointers; the
    # host needs an array of translated host pointers instead.
    ("glShaderSource", 2): "w32gl_translate_shader_srcs(a, (int)a[1])",
    # eglChooseConfig's `configs` is declared void* but points at an array of
    # EGLConfig the implementation writes into - a guest buffer, not a handle.
    ("eglChooseConfig", 2): "(w32gl_EGLConfig*)w32gl_gptr(a[2])",
}

# Pointer arguments that are *either* a guest address (client-side array) or
# a byte offset into the currently bound buffer object. Translating them
# unconditionally breaks the offset form (VBO draws), so the host passes the
# raw value through when it is not a mappable guest address.
OFFSET_PTR_ARGS = {
    ("glVertexAttribPointer", 5),
    ("glDrawElements", 3),
}

# Functions returning a host-owned string: the guest stub offers its static
# glstub_retbuf via args[GL_CALL_RETBUF_SLOT], the host copies the string
# there and answers with that guest address.
STRING_RET_FNS = {"glGetString", "eglQueryString"}

# ---------------------------------------------------------------------------
# Type tables
# ---------------------------------------------------------------------------

# Types that are themselves pointers in the headers
IMPLICIT_PTR = {"EGLDisplay", "EGLSurface", "EGLContext", "EGLConfig",
                "__eglMustCastToProperFunctionPointerType"}

# Platform-native handles -> opaque pointers
NATIVE_TYPES = {
    "EGLNativeDisplayType",
    "EGLNativeWindowType",
    "EGLNativePixmapType",
}

# Guest-side definitions (riscv64 LP64). Widths must match the host side
# after marshalling through the int64_t args[] slots.
GUEST_TYPES = {
    "void": "void",
    "GLenum": "uint32_t",
    "GLuint": "uint32_t",
    "GLbitfield": "uint32_t",
    "GLint": "int32_t",
    "GLsizei": "int32_t",
    "GLchar": "char",
    "GLboolean": "uint8_t",
    "GLbyte": "int8_t",
    "GLubyte": "uint8_t",
    "GLshort": "int16_t",
    "GLushort": "uint16_t",
    "GLfloat": "float",
    "GLclampf": "float",
    "GLintptr": "long",
    "GLsizeiptr": "long",
    "EGLint": "int32_t",
    "EGLBoolean": "uint32_t",
    "EGLenum": "uint32_t",
}

INT32_UNSIGNED = {"GLenum", "GLuint", "GLbitfield", "EGLBoolean", "EGLenum",
                  "GLboolean", "GLubyte", "GLushort"}
INT32_SIGNED = {"GLint", "GLsizei", "EGLint", "GLbyte", "GLshort", "GLchar"}
FLOAT_TYPES = {"GLfloat", "GLclampf"}
INT64_TYPES = {"GLintptr", "GLsizeiptr"}

# ---------------------------------------------------------------------------
# Header parsing
# ---------------------------------------------------------------------------

class Type:
    def __init__(self, base, ptr=0, const=False):
        self.base = base
        self.ptr = ptr
        self.const = const

    def is_pointer(self):
        return self.ptr > 0 or self.base in IMPLICIT_PTR or self.base in NATIVE_TYPES

    def is_float(self):
        return self.base in FLOAT_TYPES and not self.is_pointer()

    def is_int64(self):
        return self.base in INT64_TYPES and not self.is_pointer()

    def __repr__(self):
        return "Type(%s%s%s)" % ("const " if self.const else "", self.base, "*" * self.ptr)


def parse_type(decl):
    """Parse a C declarator fragment like 'const GLchar *const*string' or
    'GLenum texture' into (Type, name-or-None)."""
    decl = decl.strip()
    if not decl or decl == "void":
        return Type("void"), None
    const = False
    if decl.startswith("const "):
        const = True
        decl = decl[6:].strip()
    m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$", decl)
    if not m:
        raise ValueError("cannot parse type: %r" % decl)
    base, rest = m.group(1), m.group(2)
    ptr = rest.count("*")
    # parameter name: last identifier token, unless it is a qualifier
    name = None
    tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", rest)
    for tok in reversed(tokens):
        if tok not in ("const",):
            name = tok
            break
    return Type(base, ptr, const), name


def parse_prototypes(text, api_prefix):
    """Extract (ret: Type, name, [param: (Type, name)]) from declarations of
    the form  <PREFIX> ret <PREFIX2> name (params);"""
    fns = []
    seen = set()
    pat = re.compile(
        r"^%s\s+(.+?)\s+%s\s+(\w+)\s*\(([^)]*)\)\s*;" % (api_prefix[0], api_prefix[1])
    )
    for line in text.splitlines():
        line = line.strip()
        # Khronos headers write pointer-returning declarations as
        # `const char *EGLAPIENTRY eglQueryString(...)` with no space
        # before the APIENTRY macro; normalize so the return type keeps
        # its trailing '*' and the regex still matches.
        line = line.replace("*" + api_prefix[1], "* " + api_prefix[1])
        m = pat.match(line)
        if not m:
            continue
        ret, name, params = m.groups()
        ret_t, _ = parse_type(ret)
        params_l = []
        if params.strip() and params.strip() != "void":
            for p in params.split(","):
                pt, pn = parse_type(p)
                if pt.base == "void" and pt.ptr == 0:
                    continue
                params_l.append((pt, pn))
        if name in seen:
            raise ValueError("duplicate function: %s" % name)
        seen.add(name)
        fns.append({"name": name, "ret": ret_t, "params": params_l})
    return fns


# ---------------------------------------------------------------------------
# Guest-side rendering
# ---------------------------------------------------------------------------

def guest_decl_type(t):
    """Type as written in guest prototypes / casts."""
    stars = t.ptr
    if stars == 0 and (t.base in IMPLICIT_PTR or t.base in NATIVE_TYPES):
        stars = 1  # opaque handles are pointers even when written without *
    if stars:
        if t.base in IMPLICIT_PTR or t.base in NATIVE_TYPES:
            g = "void"
        else:
            g = GUEST_TYPES.get(t.base, t.base)
        return "%s%s%s" % ("const " if t.const else "", g, "*" * stars)
    return GUEST_TYPES.get(t.base, t.base)


def guest_marshal_expr(name, t):
    """Expression storing parameter `name` of guest type t into args[idx]."""
    if t.is_pointer():
        return "(int64_t)(uintptr_t)%s" % name
    if t.is_float():
        return "glstub_packf(%s)" % name
    if t.is_int64():
        return "(int64_t)%s" % name
    if t.base in INT32_UNSIGNED:
        return "(int64_t)(uint32_t)%s" % name
    return "(int64_t)(int32_t)%s" % name


def guest_return_expr(fn):
    """Return statement for the stub (or None for void)."""
    r = fn["ret"]
    if r.base == "void" and r.ptr == 0:
        return None
    if r.is_pointer():
        return "return (%s)(uintptr_t)_c.ret;" % guest_decl_type(r)
    if r.is_float():
        return "return glstub_unpackf(_c.ret);"
    if r.is_int64():
        return "return (%s)_c.ret;" % guest_decl_type(r)
    if r.base in INT32_UNSIGNED:
        return "return (%s)(uint32_t)_c.ret;" % guest_decl_type(r)
    return "return (%s)(int32_t)_c.ret;" % guest_decl_type(r)


# ---------------------------------------------------------------------------
# Host-side rendering
# ---------------------------------------------------------------------------

def host_type(t):
    """Type as written in host-side PFN declarations (w32gl_* typedefs)."""
    stars = t.ptr
    if stars == 0 and (t.base in IMPLICIT_PTR or t.base in NATIVE_TYPES):
        stars = 1  # opaque handles are pointers even when written without *
    if stars:
        if t.base in IMPLICIT_PTR or t.base in NATIVE_TYPES:
            h = "w32gl_void"
        else:
            h = "w32gl_" + t.base
        return "%s%s%s" % ("const " if t.const else "", h, "*" * stars)
    if t.base == "void":
        return "void"
    return "w32gl_" + t.base


def host_pfn(fn):
    return "w32gl_PFN_" + fn["name"]


def host_arg_expr(fn, idx, t):
    override = ARG_OVERRIDES.get((fn["name"], idx))
    if override is not None:
        return override
    if not t.is_pointer():
        if t.is_float():
            return "w32gl_arg_f(a[%d])" % idx
        # 32-bit ints: cast (int64 -> uint32/int32 handled by C conversion)
        return "(%s)a[%d]" % (host_type(t), idx)
    if t.base in IMPLICIT_PTR or t.base in NATIVE_TYPES:
        # Opaque host handle: produced by the host, handed back untouched.
        return "(%s)(uintptr_t)a[%d]" % (host_type(t), idx)
    if (fn["name"], idx) in OFFSET_PTR_ARGS:
        return "(%s)w32gl_gptr_or_off(a[%d])" % (host_type(t), idx)
    # Guest data pointer: translate before the host dereferences it.
    return "(%s)w32gl_gptr(a[%d])" % (host_type(t), idx)


def host_call_expr(fn):
    argexprs = [host_arg_expr(fn, i, t) for i, (t, _) in enumerate(fn["params"])]
    call = "((%s)p_%s)(%s)" % (host_pfn(fn), fn["name"], ", ".join(argexprs))
    r = fn["ret"]
    if r.base == "void" and r.ptr == 0:
        return "%s;" % call
    if r.is_pointer() and r.base not in IMPLICIT_PTR and r.base not in NATIVE_TYPES:
        # Host-owned string: land it in the guest scratch buffer instead of
        # handing a host pointer to the guest.
        if fn["name"] not in STRING_RET_FNS:
            raise ValueError(
                "%s returns a pointer; add it to STRING_RET_FNS or teach the "
                "host dispatch how to translate the result" % fn["name"])
        return "*ret = w32gl_string_out(a, (const char*)%s);" % call
    if r.is_pointer():
        return "*ret = (int64_t)(intptr_t)%s;" % call
    if r.is_float():
        return "*ret = w32gl_pack_f(%s);" % call
    if r.is_int64():
        return "*ret = (int64_t)%s;" % call
    if r.base in INT32_UNSIGNED:
        return "*ret = (int64_t)(uint32_t)%s;" % call
    return "*ret = (int64_t)(int32_t)%s;" % call


# ---------------------------------------------------------------------------
# File emission
# ---------------------------------------------------------------------------

GENERATED_BANNER = """/*
 * GENERATED FILE - produced by tools/gen_gl_abi.py - DO NOT EDIT BY HAND.
 *
 * Source of truth: NDK sysroot headers GLES2/gl2.h + EGL/egl.h (parsed).
 * Regenerate with:  python tools/gen_gl_abi.py
 *
 * Phase 3 ABI notes:
 *  - fn_id macros are the single source of truth shared by the guest stubs,
 *    src/virtpass/vp_cmdpost.c and the win32 host GL dispatch.
 *  - gl_call.args has {nargs} slots (glCompressedTexSubImage2D needs 9; the
 *    original Phase 3 plan said 6 - widened before first deployment, so this
 *    is an internal ABI change with zero consumers).
 *  - Floats travel bit-packed through the int64_t slots; pointers travel as
 *    guest virtual addresses. Guest memory is NOT mapped into the host, so
 *    the host dispatch translates every data pointer argument with
 *    rvvm_user_guest_ptr() and, for calls that hand back a host-owned string
 *    (glGetString/eglQueryString), copies it through the guest scratch
 *    buffer offered in args[GL_CALL_RETBUF_SLOT].
 */
"""


def gl2_type_defs():
    return """/* ============================================================
 * GLES2 core types (khronos widths, riscv64 LP64 guest)
 * ============================================================ */
typedef void             GLvoid;
typedef unsigned int     GLenum;
typedef unsigned char    GLboolean;
typedef unsigned int     GLbitfield;
typedef signed char      GLbyte;
typedef short            GLshort;
typedef int              GLint;
typedef unsigned char    GLubyte;
typedef unsigned short   GLushort;
typedef unsigned int     GLuint;
typedef int              GLsizei;
typedef float            GLfloat;
typedef float            GLclampf;
typedef char             GLchar;
typedef long             GLintptr;    /* khronos_intptr_t */
typedef long             GLsizeiptr;  /* khronos_ssize_t  */
"""


def egl_type_defs():
    return """/* ============================================================
 * EGL core types (opaque handles travel as uintptr values)
 * ============================================================ */
typedef void*    EGLDisplay;
typedef void*    EGLSurface;
typedef void*    EGLContext;
typedef void*    EGLConfig;
typedef int32_t  EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_DONT_CARE        ((EGLint)-1)

#define EGL_DEFAULT_DISPLAY  ((EGLDisplay)0)
#define EGL_NO_DISPLAY       ((EGLDisplay)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)

#define EGL_SUCCESS            0x3000
#define EGL_NOT_INITIALIZED    0x3001
#define EGL_BAD_ACCESS         0x3002
#define EGL_BAD_ALLOC          0x3003
#define EGL_BAD_MATCH          0x3004
#define EGL_BAD_ATTRIBUTE      0x3005
#define EGL_BAD_CONFIG         0x3006
#define EGL_BAD_CONTEXT        0x3007
#define EGL_BAD_CURRENT_SURFACE 0x3008
#define EGL_BAD_DISPLAY        0x3009
#define EGL_BAD_MATCH2         0x300A /* reserved */
#define EGL_BAD_NATIVE_WINDOW  0x300B
#define EGL_BAD_SURFACE        0x300D
#define EGL_CONTEXT_LOST       0x300E

#define EGL_BUFFER_SIZE      0x3020
#define EGL_ALPHA_SIZE       0x3021
#define EGL_BLUE_SIZE        0x3022
#define EGL_GREEN_SIZE       0x3023
#define EGL_RED_SIZE         0x3024
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_SAMPLES          0x3031
#define EGL_SAMPLE_BUFFERS   0x3032
#define EGL_SURFACE_TYPE     0x3033
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_NONE             0x3038

#define EGL_PBUFFER_BIT      0x0001
#define EGL_PIXMAP_BIT       0x0002
#define EGL_WINDOW_BIT       0x0004
#define EGL_OPENGL_ES_BIT    0x0001
#define EGL_OPENGL_ES2_BIT   0x0004
#define EGL_OPENGL_ES3_BIT   0x0040

#define EGL_WIDTH            0x3057
#define EGL_HEIGHT           0x3056
#define EGL_LARGEST_PBUFFER  0x3058

#define EGL_VENDOR           0x3053
#define EGL_VERSION          0x3054
#define EGL_EXTENSIONS       0x3055
#define EGL_CLIENT_APIS      0x308D

#define EGL_OPENGL_ES_API    0x30A0
#define EGL_OPENGL_API       0x30A2
#define EGL_OPENVG_API       0x30A1

#define EGL_DRAW             0x3059
#define EGL_READ             0x305A

#define EGL_CONTEXT_CLIENT_VERSION 0x3098
"""


def gl2_const_defs():
    return """/* ============================================================
 * GLES2 constants (subset used by guests + host smoke tests)
 * ============================================================ */
#define GL_FALSE 0
#define GL_TRUE  1

#define GL_BYTE                0x1400
#define GL_UNSIGNED_BYTE       0x1401
#define GL_SHORT               0x1402
#define GL_UNSIGNED_SHORT      0x1403
#define GL_INT                 0x1404
#define GL_UNSIGNED_INT        0x1405
#define GL_FLOAT               0x1406

#define GL_NEVER               0x0200
#define GL_LESS                0x0201
#define GL_EQUAL               0x0202
#define GL_LEQUAL              0x0203
#define GL_GREATER             0x0204
#define GL_NOTEQUAL            0x0205
#define GL_GEQUAL              0x0206
#define GL_ALWAYS              0x0207

#define GL_POINTS              0x0000
#define GL_LINES               0x0001
#define GL_LINE_LOOP           0x0002
#define GL_LINE_STRIP          0x0003
#define GL_TRIANGLES           0x0004
#define GL_TRIANGLE_STRIP      0x0005
#define GL_TRIANGLE_FAN        0x0006

#define GL_ZERO                0
#define GL_ONE                 1

#define GL_NO_ERROR            0
#define GL_INVALID_ENUM        0x0500
#define GL_INVALID_VALUE       0x0501
#define GL_INVALID_OPERATION   0x0502
#define GL_OUT_OF_MEMORY       0x0505

#define GL_VENDOR              0x1F00
#define GL_RENDERER            0x1F01
#define GL_VERSION             0x1F02
#define GL_EXTENSIONS          0x1F03

#define GL_DEPTH_BUFFER_BIT    0x00000100
#define GL_STENCIL_BUFFER_BIT  0x00000400
#define GL_COLOR_BUFFER_BIT    0x00004000

#define GL_RGBA                0x1908
#define GL_RGB                 0x1907

#define GL_FRAGMENT_SHADER     0x8B30
#define GL_VERTEX_SHADER       0x8B31
#define GL_COMPILE_STATUS      0x8B81
#define GL_LINK_STATUS         0x8B82
#define GL_ARRAY_BUFFER        0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW         0x88E4
#define GL_STREAM_DRAW         0x88E0
#define GL_FRAMEBUFFER         0x8D40
#define GL_RENDERBUFFER        0x8D41
"""


def fn_id_defs(gl_fns, egl_fns):
    out = []
    out.append("/* ============================================================")
    out.append(" * Function IDs (single source of truth)")
    out.append(" * ============================================================ */")
    out.append("")
    out.append("#define GL_FN_BASE %d" % GL_FN_BASE)
    for i, fn in enumerate(gl_fns):
        out.append("#define GL_FN_%s %d" % (fn["name"][2:].upper(), GL_FN_BASE + i))
    out.append("")
    out.append("#define EGL_FN_BASE 0x%03X" % EGL_FN_BASE)
    for i, fn in enumerate(egl_fns):
        out.append("#define EGL_FN_%s 0x%03X" % (fn["name"][3:].upper(), EGL_FN_BASE + i))
    out.append("")
    out.append("#define GL_CALL_MAX_ARGS %d" % GL_CALL_MAX_ARGS)
    out.append("/* gl_call.args[] slot carrying the guest scratch buffer for the")
    out.append(" * calls returning a host-owned string (glGetString / eglQueryString).")
    out.append(" * The buffer must outlive the call: the stub owns it statically. */")
    out.append("#define GL_CALL_RETBUF_SLOT (GL_CALL_MAX_ARGS - 1)")
    out.append("#define GL_CALL_RETBUF_CAP  %d" % GL_CALL_RETBUF_CAP)
    out.append("")
    out.append("/* The marshalled-call syscall numbers (SYS_GL_CALL / SYS_EGL_CALL) come")
    out.append(" * from the shared ABI header, together with every other hypercall. */")
    out.append('#include "virtpass/vp_syscall.h"')
    return "\n".join(out)


def gl_call_struct():
    return """/* ============================================================
 * Marshalling struct (guest fills, host consumes)
 *
 * Guest allocates gl_call on its stack and passes its address in a0.
 * Pointers inside args[] are GUEST addresses: guest memory is no longer
 * mapped into the host, so the backend must run each data pointer through
 * rvvm_user_guest_ptr() before dereferencing it. Opaque host values
 * (EGLDisplay/EGLConfig/EGLSurface/EGLContext and the EGLNative* types) are
 * only passed back by the guest, never dereferenced, so they pass through.
 *
 * args[GL_CALL_RETBUF_SLOT] holds the address of a guest scratch buffer
 * (GL_CALL_RETBUF_CAP bytes) for calls that hand back a host-owned string
 * (glGetString/eglQueryString): the host copies the string there and answers
 * with that guest address. The stub keeps it in a static, not on its stack -
 * the pointer outlives the call. Only those two single-argument calls use the
 * slot; everywhere else it is just the last parameter (or unused).
 * ============================================================ */
typedef struct {
    uint32_t fn_id;                    /* GL_FN_* / EGL_FN_*            */
    uint32_t nargs;                    /* number of real args[] slots   */
    int64_t  ret;                      /* host writes the return value  */
    int64_t  args[GL_CALL_MAX_ARGS];   /* 32-bit ints are zero/sign
                                        * extended; floats bit-packed;
                                        * data pointers as guest VA;
                                        * handles as opaque host values;
                                        * last slot = string scratch     */
} gl_call;
"""


def guest_prototypes(gl_fns, egl_fns):
    out = ["/* ============================================================",
           " * Guest-facing API (same signatures as the NDK headers)",
           " * ============================================================ */",
           ""]
    for fn in egl_fns:
        params = ", ".join("%s %s" % (guest_decl_type(t), n or "p%d" % i)
                           for i, (t, n) in enumerate(fn["params"])) or "void"
        out.append("%s %s(%s);" % (guest_decl_type(fn["ret"]), fn["name"], params))
    out.append("")
    for fn in gl_fns:
        params = ", ".join("%s %s" % (guest_decl_type(t), n or "p%d" % i)
                           for i, (t, n) in enumerate(fn["params"])) or "void"
        out.append("%s %s(%s);" % (guest_decl_type(fn["ret"]), fn["name"], params))
    return "\n".join(out)


def gen_guest_header(gl_fns, egl_fns):
    parts = [GENERATED_BANNER.format(nargs=GL_CALL_MAX_ARGS)]
    parts.append("#ifndef VIRTPASS_GL")
    parts.append("#define VIRTPASS_GL")
    parts.append("")
    parts.append("#include <stdint.h>")
    parts.append("")
    parts.append(gl2_type_defs())
    parts.append(gl2_const_defs())
    parts.append(egl_type_defs())
    parts.append(fn_id_defs(gl_fns, egl_fns))
    parts.append(gl_call_struct())
    parts.append(guest_prototypes(gl_fns, egl_fns))
    parts.append("")
    parts.append("#endif /* VIRTPASS_GL */")
    parts.append("")
    return "\n".join(parts)


def gen_guest_source(gl_fns, egl_fns):
    out = []
    out.append(GENERATED_BANNER.format(nargs=GL_CALL_MAX_ARGS))
    out.append("""/*
 * vp_gl_stub.c - Guest-side EGL/GLES stubs for statically-linked riscv64
 * ELFs. Every call is marshalled through one hypercall:
 *
 *   SYS_GL_CALL   -> gl_call* in a0 (GLES functions)
 *   SYS_EGL_CALL  -> gl_call* in a0 (EGL functions)
 *
 * The host (win32_gl_dispatch.c on Windows, jni_bridge on Android) executes
 * the real EGL/GLES call and writes the return value back into _c.ret.
 */
#include <string.h>
#include <stdint.h>
#include "virtpass/vp_gl.h"

/* Same ecall trampoline as vp_ndk_stub.c */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2,
                                   long a3, long a4, long a5)
{
    register long t0 __asm__("a7") = nr;
    register long t1 __asm__("a0") = a0;
    register long t2 __asm__("a1") = a1;
    register long t3 __asm__("a2") = a2;
    register long t4 __asm__("a3") = a3;
    register long t5 __asm__("a4") = a4;
    register long t6 __asm__("a5") = a5;

    __asm__ __volatile__(
        "ecall"
        : "+r"(t1)
        : "r"(t2), "r"(t3), "r"(t4), "r"(t5), "r"(t6), "r"(t0)
        : "memory"
    );
    return t1;
}

/* Float <-> int64 bit packing (GLES2 has no double params).
 * Only packing is needed guest-side: no GLES2 core function returns a float,
 * unpacking happens host-side via w32gl_arg_f(). */
static inline int64_t glstub_packf(float v)
{
    union { float f; uint32_t u; } cvt;
    cvt.f = v;
    return (int64_t)cvt.u;
}

#define GLSTUB_CALL(id, n)                        \\
    gl_call _c;                                   \\
    memset(&_c, 0, sizeof(_c));                   \\
    _c.fn_id = (uint32_t)(id);                    \\
    _c.nargs = (uint32_t)(n)

#define GLSTUB_DO(nr)                             \\
    virtpass_syscall((nr), (long)(uintptr_t)&_c, 0, 0, 0, 0, 0)

/* Scratch buffer handed to the host by calls returning a host-owned string
 * (glGetString/eglQueryString). File scope, not a gl_call member: the host
 * answers with this guest address and the caller reads it after the call, so
 * it must outlive the stub's own stack frame. */
static char glstub_retbuf[GL_CALL_RETBUF_CAP];
""")

    def emit(fn, syscall_nr):
        names = [n or "p%d" % i for i, (t, n) in enumerate(fn["params"])]
        params = ", ".join("%s %s" % (guest_decl_type(t), nm)
                           for (t, _), nm in zip(fn["params"], names)) or "void"
        out.append("%s %s(%s)" % (guest_decl_type(fn["ret"]), fn["name"], params))
        out.append("{")
        out.append("    GLSTUB_CALL(%s, %d);"
                   % ("GL_FN_" + fn["name"][2:].upper() if fn["name"].startswith("gl")
                      else "EGL_FN_" + fn["name"][3:].upper(), len(fn["params"])))
        for i, (t, _) in enumerate(fn["params"]):
            out.append("    _c.args[%d] = %s;" % (i, guest_marshal_expr(names[i], t)))
        if fn["name"] in STRING_RET_FNS:
            out.append("    _c.args[GL_CALL_RETBUF_SLOT] = "
                       "(int64_t)(uintptr_t)glstub_retbuf;")
        out.append("    GLSTUB_DO(%s);" % syscall_nr)
        if fn["name"] in STRING_RET_FNS:
            # The host landed the string in glstub_retbuf and answered with its
            # guest address, which is what _c.ret already holds - the raw host
            # pointer never crosses back. Return the stub's own buffer so the
            # value stays valid for the caller.
            out.append("    return (%s)(uintptr_t)glstub_retbuf;"
                       % guest_decl_type(fn["ret"]))
        else:
            r = guest_return_expr(fn)
            if r:
                out.append("    %s" % r)
        out.append("}")
        out.append("")

    for fn in egl_fns:
        emit(fn, "SYS_EGL_CALL")
    for fn in gl_fns:
        emit(fn, "SYS_GL_CALL")
    return "\n".join(out)


def gen_host_backend_header(gl_fns, egl_fns):
    out = []
    out.append(GENERATED_BANNER.format(nargs=GL_CALL_MAX_ARGS))
    out.append("#ifndef WIN32_GL_BACKEND_H")
    out.append("#define WIN32_GL_BACKEND_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("#include <stddef.h>")
    out.append("#include <stdbool.h>")
    out.append("")
    out.append("/*")
    out.append(" * Host-side GL/EGL function types matching the real EGL/GLES DLLs")
    out.append(" * (SwiftShader / ANGLE from the Android SDK emulator directory).")
    out.append(" * Export names were verified undecorated -> plain cdecl, no")
    out.append(" * WINAPI. All types are w32gl_-prefixed so this header never")
    out.append(" * collides with real GL/EGL headers.")
    out.append(" */")
    out.append("")
    out.append("#define w32gl_APIENTRY /* cdecl */")
    out.append("")
    out.append("/* ---- type definitions ---- */")
    out.append("typedef void          w32gl_void;")
    out.append("typedef char          w32gl_char;")
    out.append("typedef unsigned int  w32gl_GLenum;")
    out.append("typedef unsigned int  w32gl_GLuint;")
    out.append("typedef unsigned int  w32gl_GLbitfield;")
    out.append("typedef int           w32gl_GLint;")
    out.append("typedef int           w32gl_GLsizei;")
    out.append("typedef char          w32gl_GLchar;")
    out.append("typedef unsigned char w32gl_GLboolean;")
    out.append("typedef signed char   w32gl_GLbyte;")
    out.append("typedef unsigned char w32gl_GLubyte;")
    out.append("typedef short         w32gl_GLshort;")
    out.append("typedef unsigned short w32gl_GLushort;")
    out.append("typedef float         w32gl_GLfloat;")
    out.append("typedef float         w32gl_GLclampf;")
    out.append("typedef ptrdiff_t     w32gl_GLintptr;")
    out.append("typedef ptrdiff_t     w32gl_GLsizeiptr;")
    out.append("typedef void*         w32gl_EGLDisplay;")
    out.append("typedef void*         w32gl_EGLSurface;")
    out.append("typedef void*         w32gl_EGLContext;")
    out.append("typedef void*         w32gl_EGLConfig;")
    out.append("typedef int           w32gl_EGLint;")
    out.append("typedef unsigned int  w32gl_EGLBoolean;")
    out.append("typedef unsigned int  w32gl_EGLenum;")
    out.append("")

    # Shared ABI block: same fn_id macros / syscall numbers / gl_call struct
    # as the guest header (project convention duplicates ABI constants on
    # both sides; the generator keeps them in sync).
    out.append(fn_id_defs(gl_fns, egl_fns))
    out.append("")
    out.append(gl_call_struct())

    def pfn(fn):
        params = ", ".join("%s %s" % (host_type(t), n or "p%d" % i)
                           for i, (t, n) in enumerate(fn["params"])) or "void"
        return "typedef %s (w32gl_APIENTRY *%s)(%s);" % (
            host_type(fn["ret"]), host_pfn(fn), params)

    out.append("/* ---- EGL function pointer types ---- */")
    for fn in egl_fns:
        out.append(pfn(fn))
    out.append("")
    out.append("/* ---- GLES2 function pointer types ---- */")
    for fn in gl_fns:
        out.append(pfn(fn))
    out.append("")
    out.append("/* ---- resolved entry points (NULL when missing) ---- */")
    for fn in egl_fns:
        out.append("extern %s p_%s;" % (host_pfn(fn), fn["name"]))
    out.append("")
    for fn in gl_fns:
        out.append("extern %s p_%s;" % (host_pfn(fn), fn["name"]))
    out.append("")
    out.append("""/* ---- backend lifecycle -------------------------------------------- */

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
""")
    out.append("#endif /* WIN32_GL_BACKEND_H */")
    out.append("")
    return "\n".join(out)


def gen_host_dispatch_tables(gl_fns, egl_fns):
    out = []
    out.append(GENERATED_BANNER.format(nargs=GL_CALL_MAX_ARGS))
    out.append("/*")
    out.append(" * Generic dispatch switches, included from win32_gl_dispatch.c.")
    out.append(" * Do not include from anywhere else (defines static functions).")
    out.append(" * `a` is the const int64_t* args array, `ret` the int64_t* out.")
    out.append(" * Guest data pointers are translated by the w32gl_gptr*() helpers")
    out.append(" * defined in win32_gl_dispatch.c above this include.")
    out.append(" */")
    out.append("")

    out.append("/* fn_id -> name, for the RVVM_GL_TRACE call log */")
    out.append("static const char* w32gl_egl_name(uint32_t fn_id)")
    out.append("{")
    out.append("    switch (fn_id) {")
    for fn in egl_fns:
        fid = "EGL_FN_" + fn["name"][3:].upper()
        out.append('    case %s: return "%s";' % (fid, fn["name"]))
    out.append("    default: return NULL;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static const char* w32gl_gl_name(uint32_t fn_id)")
    out.append("{")
    out.append("    switch (fn_id) {")
    for fn in gl_fns:
        fid = "GL_FN_" + fn["name"][2:].upper()
        out.append('    case %s: return "%s";' % (fid, fn["name"]))
    out.append("    default: return NULL;")
    out.append("    }")
    out.append("}")
    out.append("")

    out.append("static void w32gl_dispatch_egl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)")
    out.append("{")
    out.append("    switch (fn_id) {")
    for fn in egl_fns:
        fid = "EGL_FN_" + fn["name"][3:].upper()
        out.append("    case %s:" % fid)
        out.append("        if (p_%s) { %s }" % (fn["name"], host_call_expr(fn)))
        out.append("        break;")
    out.append("    default:")
    out.append("        break;")
    out.append("    }")
    out.append("}")
    out.append("")

    out.append("static void w32gl_dispatch_gl_generic(uint32_t fn_id, const int64_t* a, int64_t* ret)")
    out.append("{")
    out.append("    switch (fn_id) {")
    for fn in gl_fns:
        fid = "GL_FN_" + fn["name"][2:].upper()
        out.append("    case %s:" % fid)
        out.append("        if (p_%s) { %s }" % (fn["name"], host_call_expr(fn)))
        out.append("        break;")
    out.append("    default:")
    out.append("        break;")
    out.append("    }")
    out.append("}")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if not os.path.isfile(GL2_H) or not os.path.isfile(EGL_H):
        sys.exit("NDK headers not found (set NDK_SYSROOT env var):\n  %s\n  %s" % (GL2_H, EGL_H))

    with open(GL2_H, "r", encoding="utf-8", errors="replace") as f:
        gl_fns = parse_prototypes(f.read(), ("GL_APICALL", "GL_APIENTRY"))
    with open(EGL_H, "r", encoding="utf-8", errors="replace") as f:
        all_egl = parse_prototypes(f.read(), ("EGLAPI", "EGLAPIENTRY"))

    egl_names = set(EGL_WHITELIST)
    egl_fns = [fn for fn in all_egl if fn["name"] in egl_names]
    missing = egl_names - {fn["name"] for fn in egl_fns}
    if missing:
        sys.exit("EGL whitelist entries not found in egl.h: %s" % sorted(missing))

    # basic sanity: every param/ret type must be known on the guest side
    known = set(GUEST_TYPES) | IMPLICIT_PTR | NATIVE_TYPES
    for fn in gl_fns + egl_fns:
        for t in [fn["ret"]] + [pt for pt, _ in fn["params"]]:
            if t.base not in known and not t.is_pointer() and t.ptr == 0:
                # unknown value type: fail loudly so the mapping gets updated
                sys.exit("unknown value type %r in %s" % (t, fn["name"]))

    os.makedirs(GUEST_INC_DIR, exist_ok=True)
    os.makedirs(GUEST_SRC_DIR, exist_ok=True)
    os.makedirs(HOST_DIR, exist_ok=True)

    outs = [
        (os.path.join(GUEST_INC_DIR, "vp_gl.h"), gen_guest_header(gl_fns, egl_fns)),
        (os.path.join(GUEST_SRC_DIR, "vp_gl_stub.c"), gen_guest_source(gl_fns, egl_fns)),
        (os.path.join(HOST_DIR, "win32_gl_backend.h"), gen_host_backend_header(gl_fns, egl_fns)),
        (os.path.join(HOST_DIR, "win32_gl_dispatch_tables.h"), gen_host_dispatch_tables(gl_fns, egl_fns)),
    ]
    for path, content in outs:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("wrote %s" % os.path.relpath(path, REPO_ROOT))

    print("\nGLES2 core functions: %d   EGL functions: %d" % (len(gl_fns), len(egl_fns)))
    print("GL fn_id range:  [%d, %d)   EGL fn_id range: [0x%03X, 0x%03X)"
          % (GL_FN_BASE, GL_FN_BASE + len(gl_fns), EGL_FN_BASE, EGL_FN_BASE + len(egl_fns)))


if __name__ == "__main__":
    main()
