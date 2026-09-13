#!/usr/bin/env python3
"""Generate "not implemented" variants of the VirtPass guest stubs.

The VirtPass guest stubs (src/virtpass/vp_ndk_stub.c, vp_aaudio_stub.c,
vp_gl_stub.c) are the guest-side half of the hypercall ABI. This tool derives a
build variant of one of them whose API functions no longer talk to the host:
each one reports that it was called and returns a zero value, so a guest can be
run against a host with no backend and the console shows exactly which APIs the
guest actually needs.

Transformation rules (nothing is hand-maintained, nothing drifts):

  1.  The prologue (header comments, #includes, typedefs, #defines) is copied
      verbatim.
  2.  Every top-level function definition keeps its signature *verbatim* - per
      AGENTS.md the NDK part must follow the NDK ABI signatures, so the
      signature text is never re-synthesised.
  3.  The body is replaced by: report the call once through stderr, silence
      unused-parameter warnings, return a zero value of the declared type.
  4.  Functions marked __attribute__((constructor)) keep their real body (they
      set up the guest runtime, not a host API), as do names passed to --keep.
  5.  Everything else (static tables, globals, declarations) is copied verbatim
      so the result still links against the same symbols.

Usage:
    python tools/gen_stub_notimpl.py src/virtpass/vp_ndk_stub.c
    python tools/gen_stub_notimpl.py src/virtpass/vp_ndk_stub.c \
        src/virtpass/vp_aaudio_stub.c src/virtpass/vp_gl_stub.c \
        --outdir src/virtpass/vp-sdk
"""

from __future__ import annotations

import argparse
import os
import re
import sys

GENERATED_BANNER = """\
/*
 * GENERATED FILE - produced by tools/gen_stub_notimpl.py - DO NOT EDIT BY HAND.
 *
 * "Not implemented" build variant of %s.
 * Regenerate from the repository root:
 *     python tools/gen_stub_notimpl.py %s%s
 *
 * Every API function below keeps its original signature and reports itself on
 * stderr instead of issuing a hypercall to the host. Link a guest against this
 * file instead of the real stub to see which host APIs it actually asks for.
 */
"""

REPORTER_SECTION = """\
/* ============================================================
 * Generated fallback reporter
 *
 * One line per distinct API is printed the first time it is reached: a guest
 * render loop calls these functions thousands of times per second, so
 * reporting every call would drown the host console in identical lines. The
 * dedup table compares the __func__ literals (stable per function); the race
 * between guest threads at worst repeats a line.
 * ============================================================ */
static void vp_stub_not_implemented(const char* api)
{
    static const char* reported[512];
    static unsigned     count = 0;
    unsigned            i;

    for (i = 0; i < count; i++) {
        if (reported[i] == api) {
            return;
        }
    }
    if (count < sizeof(reported) / sizeof(reported[0])) {
        reported[count++] = api;
    }
    fprintf(stderr, "[virtpass] not implemented: %s()\\n", api);
}
"""

# Kinds of top-level text that look like a function definition but are not.
REJECT_FIRST_WORD = {
    "typedef", "struct", "union", "enum", "extern", "return", "else", "case",
    "switch", "while", "for", "if", "do", "sizeof", "_Static_assert",
    "static_assert", "__attribute__", "__asm__", "__extension__",
}

SCALAR_KEYWORDS = {
    "void", "int", "char", "short", "long", "float", "double", "bool", "signed",
    "unsigned", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t",
}

_IDENT = r"[A-Za-z_]\w*"
_IDENT_RE = re.compile(_IDENT)
_FP_PARAM_RE = re.compile(r"\(\s*\*\s*(%s)\s*\)" % _IDENT)


def detect_newline(text: str) -> str:
    return "\r\n" if "\r\n" in text else "\n"


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" \t"))


def split_head(line: str):
    """Return (return_type, name) when `line` starts a function definition."""
    text = line.rstrip()
    if not text or text[0] in " \t#/":
        return None
    paren = text.find("(")
    if paren < 0:
        return None
    match = re.match(r"^(.*?)(%s)\s*$" % _IDENT, text[:paren])
    if not match:
        return None
    return_type, name = match.group(1).strip(), match.group(2)
    if not return_type:
        # __attribute__((...)), if (...), while (...) - no type in front.
        return None
    if return_type.split()[0] in REJECT_FIRST_WORD:
        return None
    return return_type, name


def signature_text(lines, start: int, end: int) -> str:
    """Signature of a definition, from the name up to (excluding) the brace."""
    return " ".join(line.strip() for line in lines[start:end])


def extract_param_names(sig: str, name: str) -> list:
    """Names of the parameters of `sig`, for unused-parameter casts."""
    name_at = sig.find(name)
    paren = sig.find("(", name_at + len(name))
    if paren < 0:
        return []
    depth, end = 1, paren + 1
    while end < len(sig) and depth:
        if sig[end] == "(":
            depth += 1
        elif sig[end] == ")":
            depth -= 1
        end += 1
    params, chunks, depth, cur = [], [], 0, []
    for ch in sig[paren + 1:end - 1]:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            chunks.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    chunks.append("".join(cur))
    for chunk in chunks:
        chunk = chunk.strip()
        if chunk in ("", "void", "..."):
            continue
        fp = _FP_PARAM_RE.search(chunk)
        if fp:
            params.append(fp.group(1))
            continue
        last, depth = None, 0
        for token in re.finditer(r"\(|\)|\[|\]|%s" % _IDENT, chunk):
            text = token.group(0)
            if text in "([":
                depth += 1
            elif text in ")]":
                depth -= 1
            elif depth == 0:
                last = text
        if last:
            params.append(last)
    return params


def default_return(return_type: str) -> str:
    if "*" in return_type:
        return "NULL"
    base = " ".join(w for w in return_type.split() if w not in ("static", "inline", "const"))
    if base in ("float",):
        return "0.0f"
    if base in ("double", "long double"):
        return "0.0"
    return "0"


def is_void(return_type: str) -> bool:
    return return_type.split()[-1] == "void"


def scalar_like(return_type: str) -> bool:
    if "*" in return_type:
        return True
    base = " ".join(w for w in return_type.split() if w not in ("static", "inline", "const"))
    if base in SCALAR_KEYWORDS or base.endswith("_t"):
        return True
    return False


def parse_definition(lines, start: int):
    """If line `start` opens a top-level function body, return (brace, close)."""
    if split_head(lines[start]) is None:
        return None
    brace = None
    for index in range(start, min(start + 25, len(lines))):
        body = lines[index].strip()
        if body.endswith(";"):
            return None  # a declaration, not a definition
        if body == "{" or body.endswith("{"):
            brace = index
            break
    if brace is None:
        return None
    for index in range(brace + 1, len(lines)):
        if lines[index].strip() == "}" and indent_of(lines[index]) == 0:
            return brace, index
    return None


def find_first_definition(lines):
    """Index of the first top-level function definition, if any."""
    for index in range(len(lines)):
        found = parse_definition(lines, index)
        if found:
            return index
    return None


def include_anchor(lines, first_def) -> int:
    """Line index the generated helper block is spliced at.

    Directly after the last #include keeps the pragma/reporter block in front of
    every definition without ever landing inside a declaration (an
    __attribute__((constructor)) line or a signature would otherwise get split).
    """
    if first_def is None:
        return len(lines)
    anchor = first_def
    for index in range(first_def):
        if re.match(r"\s*#\s*include\b", lines[index]):
            anchor = index + 1
    return anchor


def keeps_body(lines, start: int, name: str, keep: set) -> bool:
    if name in keep:
        return True
    for index in range(max(0, start - 3), start):
        if "__attribute__" in lines[index] and "constructor" in lines[index]:
            return True
    return False


def stub_body(return_type: str, params: list, warning_sink: list, api: str, newline: str) -> str:
    out = ["{" + newline, "    vp_stub_not_implemented(__func__);" + newline]
    for param in params:
        out.append("    (void)%s;" % param + newline)
    if not is_void(return_type):
        if not scalar_like(return_type):
            warning_sink.append("%s: unknown return type '%s', emitting 'return 0;'"
                                % (api, return_type))
        out.append("    return %s;" % default_return(return_type) + newline)
    out.append("}" + newline)
    return "".join(out)


def generate(path: str, keep: set, dedupe: bool):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        raw = handle.read()
    newline = detect_newline(raw)
    lines = raw.replace("\r\n", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    lines = [line + "\n" for line in lines]

    first_def = find_first_definition(lines)
    anchor = include_anchor(lines, first_def)
    # Re-running the tool on its own output must not stub the reporter (it would
    # call itself) nor duplicate the helper block.
    already_generated = any("vp_stub_not_implemented" in line for line in lines)
    out, index = [], 0
    stubbed, kept = [], []
    warnings = []
    extras_emitted = False

    def emit_extras():
        nonlocal extras_emitted
        if extras_emitted:
            return
        extras_emitted = True
        if already_generated:
            return
        text = []
        if not any(re.match(r"\s*#\s*include\s*<\s*stdio\.h\s*>", l) for l in out):
            text.append("#include <stdio.h>       /* generator: stubs report via stderr */")
        if not any(re.match(r"\s*#\s*include\s*<\s*stddef\.h\s*>", l) for l in out):
            text.append("#include <stddef.h>      /* generator: NULL / size_t */")
        if text:
            text = [line + newline for line in text]
            text.append(newline)
        text.append("#if defined(__GNUC__)" + newline)
        text.append("#pragma GCC diagnostic ignored \"-Wunused-function\"" + newline)
        text.append("#pragma GCC diagnostic ignored \"-Wunused-variable\"" + newline)
        text.append("#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"" + newline)
        text.append("#endif" + newline + newline)
        if dedupe:
            text.append(REPORTER_SECTION.replace("\n", newline))
            text.append(newline)
        out.extend(text)

    while index < len(lines):
        if index == anchor:
            emit_extras()
        found = parse_definition(lines, index)
        if found is None:
            # Prologue, static tables, globals: copied verbatim.
            out.append(lines[index])
            index += 1
            continue

        brace, close = found
        return_type, name = split_head(lines[index])
        if keeps_body(lines, index, name, keep):
            out.extend(lines[index:close + 1])
            kept.append(name)
        else:
            out.extend(lines[index:brace])
            if lines[brace].strip() != "{":
                # '{' shares the last signature line: keep the signature part.
                sig_part = lines[brace].rstrip().rstrip("\n")
                sig_part = sig_part[:sig_part.rfind("{")].rstrip()
                out.append(sig_part + newline)
            sig = signature_text(lines, index, brace)
            params = extract_param_names(sig, name)
            out.append(stub_body(return_type, params, warnings, name, newline))
            stubbed.append(name)
        index = close + 1

    if not extras_emitted:
        emit_extras()

    return newline, "".join(out), stubbed, kept, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sources", nargs="+", help="stub .c files to transform")
    parser.add_argument("--outdir", default=None,
                        help="output directory (default: next to each source)")
    parser.add_argument("--suffix", default="_notimpl",
                        help="output name suffix, before '.c' (default: _notimpl)")
    parser.add_argument("--keep", action="append", default=[],
                        help="function name whose real body must be preserved "
                             "(repeatable); constructors are always kept")
    parser.add_argument("--no-dedupe", action="store_true",
                        help="report every call instead of once per API")
    parser.add_argument("--quiet", action="store_true", help="no per-file report")
    args = parser.parse_args()

    failed = False
    for source in args.sources:
        keep = set(args.keep) | {"vp_stub_not_implemented"}
        newline, body, stubbed, kept, warnings = generate(source, keep, not args.no_dedupe)
        directory = args.outdir or os.path.dirname(source) or "."
        os.makedirs(directory, exist_ok=True)
        stem = os.path.splitext(os.path.basename(source))[0]
        target = os.path.join(directory, "%s%s.c" % (stem, args.suffix))
        # The banner carries the exact command that reproduces this file: the
        # generated variants are checked in, so the command must include the
        # output directory.
        extra = ""
        if args.outdir:
            extra += " --outdir " + args.outdir.replace(os.sep, "/")
        if args.suffix != "_notimpl":
            extra += " --suffix " + args.suffix
        if args.keep:
            extra += "".join(" --keep " + name for name in args.keep)
        if args.no_dedupe:
            extra += " --no-dedupe"
        text = (GENERATED_BANNER % (source.replace(os.sep, "/"), source.replace(os.sep, "/"), extra)
                ).replace("\n", newline) + newline + body
        with open(target, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        print("%s -> %s" % (source, target))
        if not args.quiet:
            print("    stubbed %d function(s), kept %d real body/bodies%s"
                  % (len(stubbed), len(kept), (" (" + ", ".join(kept) + ")") if kept else ""))
            for warning in warnings:
                print("    warning: %s" % warning)
        if not stubbed:
            print("    warning: no function definition was rewritten, is this a stub?")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
