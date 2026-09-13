# VirtPass SDK stubs (vpsdk)

Guest libraries whose APIs **report themselves instead of talking to the host**.

Guest programs normally link `src/virtpass/vp_*_stub.c`, which turn every NDK /
EGL / AAudio call into a hypercall serviced by the host bridge (`rvvm_winhost`,
`librvvm_jni.so`, the passthrough). Those stubs fail silently when the host has
no backend for an API: the call returns a zeroed value and nothing tells you
that a piece of the guest never worked.

Link a guest against `vpsdk` instead and every API the guest actually reaches
prints one line on stderr:

```
[virtpass] not implemented: ANativeWindow_acquire()
[virtpass] not implemented: AChoreographer_postFrameCallback()
[virtpass] not implemented: AAudioStreamBuilder_openStream()
```

That log is the list of host APIs a guest really depends on - useful when
porting a guest to a new host, or when trimming a host backend.

## Contents

| File | Generated from | Stubbed functions |
|------|----------------|-------------------|
| `vp_ndk_stub_notimpl.c`   | `src/virtpass/vp_ndk_stub.c`    | 97  |
| `vp_aaudio_stub_notimpl.c`| `src/virtpass/vp_aaudio_stub.c` | 61  |
| `vp_gl_stub_notimpl.c`    | `src/virtpass/vp_gl_stub.c`     | 160 |

Together they cover the whole guest API surface, so a guest can be linked
against `vpsdk` **instead of** `libandroid_stubs.a` + `libgles_stubs.a`.

Any further `*.c` dropped into this directory is compiled into both artifacts
automatically (the Makefile matches the directory, not a file list). The directory
is also part of `lib_src_virtpass_nonhost`, so these sources can never be picked
up by the host build - RISC-V guest code must not end up inside `librvvm`.

## These files are generated - do not edit them by hand

The generator is `tools/gen_stub_notimpl.py`; each file carries the exact
command that reproduces it in its header comment.

```sh
make vp-sdk-gen      # regenerate all three from the real guest stubs
```

```sh
python tools/gen_stub_notimpl.py src/virtpass/vp_ndk_stub.c \
    src/virtpass/vp_aaudio_stub.c src/virtpass/vp_gl_stub.c \
    --outdir src/virtpass/vp-sdk
```

Transformation rules (see the module docstring for the details):

1. Prologue (`#include`, typedefs, `#define`s), static tables and globals are
   copied verbatim, so the result compiles and links with the same headers.
2. Every function **signature** is kept verbatim - per `AGENTS.md` the NDK part
   of the ABI must follow the NDK, and a generated signature would be a second
   source of truth.
3. The body becomes `vp_stub_not_implemented(__func__)`, `(void)`-casts for the
   parameters, and a zero return (`NULL` for pointers, `0.0f` / `0.0` for
   floats, `0` otherwise).
4. `__attribute__((constructor))` functions keep their real body (they set up
   the guest runtime, not a host API); `--keep <name>` preserves any other
   function.
5. File-local helpers (`static`, `static inline`) are stubbed as well - notably
   the `virtpass_syscall()` `ecall` trampoline, which is the point: a guest
   linked against `vpsdk` must not reach the host at all.

Re-running the generator on its own output is a no-op for the reporter (the
reporting helper is never stubbed into a recursive call).

## Building

```sh
make vp-sdk
```

produces, from one `-fPIC` object set:

| Artifact | Description |
|----------|-------------|
| `lib/libvpsdk.a`  | static archive (use this for guests) |
| `lib/libvpsdk.so` | shared object, riscv64-linux-musl |

Both land in the repository root's `lib/` (`VP_SDK_OUT`, relative to the repo
root) instead of the build directory: that is the single directory a guest build
has to point `-L` at. Only the object files stay in `$(BUILDDIR)/vp-sdk/`, which
defaults to `release.<os>.<arch>` (e.g. `release.windows.x86_64`). Like every
other `src/virtpass` stub these are riscv64 **guest** objects: they are
cross-compiled with `zig cc`, never with the host compiler, and `lib/` is
gitignored - `make clean` (or `make vp-sdk-clean`) removes them. Override the
flags with `VP_SDK_CFLAGS=...` (they are tracked in the build directory's
`sdk_flags.stamp`, so changing them rebuilds).

From the wrapper script:

```sh
pwsh ./build_virtpass.ps1 -Target sdk     # == make vp-sdk
```

`make vp-sdk` compiles the checked-in sources as-is; it never rewrites this
directory. Run `make vp-sdk-gen` after touching `vp_*_stub.c`, `include/virtpass`
or the generator itself.

## Using it

Link a guest against the archive instead of the real stubs:

```sh
zig cc -target riscv64-linux-musl -O0 -g -I include -static \
    src/virtpass/guest-samples/test_render.c \
    -L lib -lvpsdk -o test_render.exe
```

Both artifacts carry the `lib` prefix (`libvpsdk.a` / `libvpsdk.so`), so the usual
`-L <dir> -lvpsdk` lookup works; passing the archive path directly
(`lib/libvpsdk.a`) works just as well. `lib/` is a plain path with no dots, so
`-Llib` is safe on PowerShell too - unlike the old
`release.windows.x86_64/vp-sdk`, which PowerShell splits at the first dot.

The guest runs against any host, including one with no virtpass backend: it just
does nothing and prints what it wanted. Each distinct API is reported once (a
512-entry table keyed on the `__func__` literal, so a render loop does not flood
the log); regenerate with `--no-dedupe` for one line per call.

## Limitations

- **Nothing is emulated.** Handles come back `NULL`, sizes `0`, enums `0`. The
  guest may take an early-out path, spin, or crash on a `NULL` dereference
  before it has asked for everything it needs. The log is the deliverable, not
  the behaviour. `eglGetProcAddress()` returning `NULL` is the usual first
  casualty: only the (copied verbatim) `glstub_procs[]` table survives, and a GL
  loader that does not check the pointer will fault immediately.
- **Guest-side machinery is stubbed too.** The looper pump, the Choreographer
  vsync fallback and the AAudio callback thread are host-backed in the real
  stubs, so a guest may never get past its first calls (`ALooper_pollOnce()`
  returns `0` forever, callbacks never fire). Regenerate with `--keep` for the
  ones that are really guest-side plumbing for your case, e.g.:

  ```sh
  python tools/gen_stub_notimpl.py src/virtpass/vp_ndk_stub.c \
      --outdir src/virtpass/vp-sdk \
      --keep vp_choreographer_arm --keep vp_looper_add
  ```

  The banner records `--keep`, so a regenerated file says how it was made.
- **`libvpsdk.so` exports every non-static symbol** (no version script); the static
  archive is the artifact guest builds should use. The `.so` is only usable with
  a dynamic loader inside the guest.
- **Do not link `vpsdk` together with the real stubs** - the API symbols would
  be defined twice.

## Adding a new API group

1. Add the stub next to the others, e.g. `src/virtpass/vp_foo_stub.c`.
2. Generate its `notimpl` variant into this directory:

   ```sh
   python tools/gen_stub_notimpl.py src/virtpass/vp_foo_stub.c --outdir src/virtpass/vp-sdk
   ```

3. `make vp-sdk` - the new `*.c` is picked up from the directory listing.
