# Virtpass
Give you a set of apis to dev applications can runs on Virtpass platform.

VirtPass is an abstract layer conception that can be implemented by emulator to provide bridged apis:
- Android ndk-like abis: GameActivity lifecycle API, Looper API, sensors API, Input API, ANativeWindow API,
User mode
- EGL/GLES apis
- Other apis


# VirtPass implementation

Two hosts implement the same `vp_cmdpost` callback ABI (`vp_cmdpost.h`). A guest
compiled once against the stub headers runs unchanged on either host; only the
backend behind each callback differs.

## Shared layer (`src/virtpass`)

- `vp_cmdpost.c/.h` — the ABI proxy core: the lifecycle, motion and key event
  queues the guest drains through GameActivity, the GL/EGL and AAudio entries,
  and the callback registration points every host fills in.
- `vp_sensor.c/.h` — the host-side sensor subsystem: it caches the device
  descriptors, owns the event queues with their bounded staging FIFO and the
  Looper wake fd, and exposes one `vp_sensor_ops_t` for a platform backend to
  implement. Guest-visible wire format: `virtpass/vp_sensor_abi.h`.
- `vp_core.c/.h` + `vp_console.c/.h` — the console. `vp_core` is the session
  layer (one pty per client out of one machine, shared with `rvvm_ash`);
  `vp_console` is the byte pipe that carries it — an adb-ShellProtocol-shaped
  loopback listener, the framing, and the ring that keeps the guest thread off
  a blocking write. Both hosts build the same `vp_console.c`; only the log sink
  differs (logcat on Android, stderr elsewhere).
- Guest stubs: `vp_ndk_stub.c` (NDK APIs), `vp_gl_stub.c` (EGL/GLES), and
  `vp_aaudio_stub.c` (AAudio). GL calls are marshalled through
  `SYS_GL_CALL`/`SYS_EGL_CALL` with guest addresses in `args[]`.
- GL/EGL symbolic constants are parsed out of the NDK headers instead of being
  typed in by hand. Core `gl2.h`/`gl3.h` tokens go into `virtpass/vp_gl.h`; the
  roughly 1500 registry tokens `gl2ext.h`/`gl3ext.h` add (S3TC/DXT formats,
  `GL_BGRA_EXT`, anisotropy, ...) live in **`virtpass/vp_glext.h`**, which a
  guest includes only when it actually needs them - same split the real NDK
  makes, and it keeps `vp_gl.h` from dwarfing every other header. Constants
  only: the extensions contribute no prototypes, so nothing claims a fn_id.
- Generated GL dispatch (from `tools/gen_gl_abi.py`): `vp_gl_host_types.h`
  (neutral `vpgl_` types + PFN typedefs), `vp_gl_host_entries.h` (the `p_*`
  storage, included by exactly one host TU per host) and
  `vp_gl_dispatch_tables.h` (name tables + generic switch). The argument
  translation rules exist exactly once; each host adds a thin backend on top.
- `glMapBufferRange()` is the one GL entry point whose result cannot cross back
  as a host address, so it is staged: the stub mirrors the range into a guest
  buffer and the host seeds / writes it back with `glBufferSubData()` on
  `glUnmapBuffer()` and `glFlushMappedBufferRange()`
  (`MAP_RANGE_FN` in `tools/gen_gl_abi.py`).

## Windows (`win32-host/`)

- Entry: `rvvm_winhost.exe` (`win32_main.c`) — console application: message
  pump, launcher UI, `--display WxH@PPI`, `--assets DIR` (or `RVVM_ASSETS`), and
  the launch target `--app <id> [args...]` (an app *package* from the bundle's
  apps.tar.gz, booted from its manifest) or `--launcher` (the picker). There is
  no loose-ELF argument: a guest is only ever a package.
- Console keyboard: `WM_CHAR` plus the arrow / Home / End / Delete keys are
  routed to the guest's fd 0 through `rvvm_user_tty_input()`, the same call the
  Android console tab makes. The line discipline stays in the core
  (`rvvm_user.c`), and the VTerm the guest writes to is what the window renders,
  so a guest like `test_cli` runs as an interactive shell in the window. The
  host's own stdin feeds the same discipline (`stdin_pump_thread()`, which waits
  for `rvvm_user_is_started()` before reading), which is what makes a run
  scriptable: `printf 'ls /\nexit\n' | rvvm_winhost.exe --app test_cli`.
- GameActivity keys: the same keystrokes are also queued for
  `android_app_swap_input_buffers()` - `cmdpost_queue_key_event()` with the VK
  mapped to an `AKEYCODE_*` value. The queue lives in the shared
  `vp_cmdpost.c`; the guest stub already drained it.
- `win32_cmdpost_bridge.c` — window/config/lifecycle/input callbacks and the
  two-layer display model: layer 1 is the host-owned virtual panel the guest
  observes, layer 2 is the OS window (a pure viewport). CPU rendering is
  presented by copying the guest frame into a BGRA DIB on
  `ANativeWindow_unlockAndPost`.
- GL (`win32_gl_backend.c` + `win32_gl_dispatch.c`): Windows has no native
  GLES, so ANGLE is loaded from the Android SDK emulator installation. A guest
  window surface is downgraded to a panel-sized pbuffer (EGL_WIDTH/HEIGHT
  injected when the guest's attribute list has none) and every
  `eglSwapBuffers` blits the pbuffer through the DIB. All 268 entry points
  (246 GLES2/GLES3 + 22 EGL) go through the shared generated dispatch.
- Audio: `win32_aaudio_wasapi.c` implements the AAudio ops over WASAPI.
- Sensors: `win32_sensor_stub.c` provides three virtual sensors, driven from
  the WM_TIMER on the UI thread and fed into `vp_sensor_ingest()`.
- Vsync: a 60 Hz clock driven by DwmFlush.

## Android (`android-host/`)

- Gradle app; native side lives in `app/src/main/cpp/`. `jni_bridge.c` is the
  JNI surface (`RvvmNative.java`, `MainActivity.java`): guest thread
  management, sensor manager, lifecycle/motion queues, and reinstalling every
  cmdpost callback before each guest run (idempotent: a guest's exit ends its
  run - `cmdpost_end_run()` - and leaves the host's registrations alone; only
  the host's own teardown calls `cmdpost_cleanup()`).
- Display: the same two-layer model as win32, but layer 2 is a real
  SurfaceView. The CPU path locks the ANativeWindow and copies the guest frame
  on unlock, with the surface geometry re-applied lazily (panel size, not
  viewport size, so resizes never move the guest's buffer).
- GL (`android_gl_host.c`): the host IS Android — `libEGL.so`/`libGLESv2.so`
  are dlopened from the system. `eglCreateWindowSurface` binds the real
  ANativeWindow and `eglSwapBuffers` presents through SurfaceFlinger: no
  pbuffer downgrade, no DIB, no blit. A window-less call fails like real EGL
  (EGL_NO_SURFACE) instead of silently diverting to an invisible surface.
- Audio: `vp_aaudio_android.c` pumps the guest's SPSC ring into an
  AAudioStream.
- Sensors: `vp_sensor_android.c` implements the sensor ops directly on the
  platform `ASensorManager` / `ASensorEventQueue`, which is drained on its own
  looper thread; no sensor data crosses Java.
- Vsync: a dedicated thread owns the per-thread AChoreographer and publishes
  frame times; guests consume them via Looper fd wakeup or a blocking wait.
- Keyboard: one keystroke stream, two destinations. The console's
  `TtyEditText` consumes what it needs (Enter, Backspace, Tab, arrows, Ctrl
  combinations) and feeds `nativeTtyInput`; the activity's `dispatchKeyEvent()`
  hands everything the views did not consume to the GameActivity queue
  (`nativePostKeyEvent` -> `cmdpost_queue_key_event()`), which a game guest
  reads through `android_app_swap_input_buffers()`. So the same key never lands
  in both places. Volume, power and Back stay with the device.
- Diagnostics: guest stdout/stderr (write and writev) land in logcat under the
  `RVVM-GUEST` tag; GL calls trace under `RVVM-GL` with `RVVM_GL_TRACE`.
  The guest console is also parsed into the virtual TTY layer (the console
  tab), and keeps flowing to logcat / the Java console log file - the TTY and
  the console sink are complementary sinks, not alternatives. A guest can be
  launched directly with
  `am start -n com.rvvm.android/.MainActivity --es guest <name>.exe`.
- Assets: the host mounts its asset tree at `/assets` (`rvvm_user_set_assets()`),
  and that mount is the only transport - the guest's `AAssetManager_*` calls are a
  shell over it in `vp_ndk_stub.c` (`stat()` for the length, `open()`/`read()` for
  the bytes), so there is no asset-specific syscall or cmdpost callback at all.
  On Android the mount hands back a *stream*: a pipe fed by a thread pulling
  through the platform `AAssetManager`, so nothing is resident on the host and the
  copy back-pressures on the guest's own reads. On win32 the tree is a real
  directory, so the descriptors are plain seekable files. `lseek()` on a stream
  reports `ESPIPE`, which is the honest answer for one; see
  `include/virtpass/vp_asset.h`.
  The core reaps those descriptors when the run ends (a guest that exits mid-read
  is the ordinary case, and the emulator runs no process teardown), which is what
  lets a feeder thread unwind instead of being stranded; the same sweep closes
  directories left open.
  `stat`/`access` are answered from `android_asset_size()` (a stat never copies
  the asset), a directory reports `S_IFDIR`, and writes inside the mount fail
  with `EROFS` before the host is ever asked.
  `opendir()`/`readdir()` enumerate through `AAssetManager_openDir()` on a
  synthetic fd, because an asset directory has no host fd to hand out. That
  enumeration is the NDK's, so it reports the *files* at that level and does not
  return subdirectory names: `ls /assets` lists the guest ELFs but not `fonts/`,
  which stays reachable by name (`ls /assets/fonts`).
- Scripted runs: the console is a byte pipe on loopback, framed exactly as adb
  frames a shell (`ShellProtocol` — a 5-byte header of one id plus a 4-byte
  little-endian length, then a payload). One id per logical stream:

  | id | stream | direction |
  |----|--------|-----------|
  | 0 | stdin | client → guest, raw keystrokes |
  | 1 | stdout | guest → client |
  | 2 | stderr | guest → client |
  | 3 | exit | guest → client, the status as text |
  | 4 | close stdin | client → guest, the guest sees EOF |
  | 5 | window size | client → guest, ASCII winsize |

  Everything blocks: a client waits on a read, and a driver writes keystrokes
  without a round trip. There is no poll loop and no sleep to tune.

  Two of those ids are the reason it is worth having. **stdout and stderr are
  separate**, and they are not separable further in: the virtual TTY shows
  both on one screen, so this is the only place the distinction survives — and
  it reaches whoever writes the client, who can then match on stdout without an
  error message satisfying it. (Busybox ash writes its prompt to stderr and the
  echoed command line to stdout; they arrive as different ids.) And **exit
  carries the result in band**, after the output: a batch guest's answer *is*
  its status, and it arrives once the guest's thread has handed over every byte
  it wrote, so a client that stops reading on `exit` has the whole transcript.

  ```powershell
  # a guest that prints and exits 7 — the driver's exit code is 7
  pwsh ./tools/android_console.ps1 -Serial <adb-serial> \
       -App test_busybox -Argv '-c|exit 7' -ExpectExit

  # a shell to type in (the default guest is an interactive busybox)
  pwsh ./tools/android_console.ps1 -Serial <adb-serial> -Interactive

  # or drive it directly
  . ./tools/android_console.ps1
  $s = New-ShellSession -Serial <adb-serial>
  Send-Shell $s -Keys CtrlC           # the core's line discipline runs it
  Wait-ShellText $s -Pattern 'localhost' -Stream stdout
  Close-ShellSession $s

  pwsh ./tools/android_console.ps1 -SelfTest     # no device needed
  ```

  Keystrokes go in raw; the core's own line discipline (ICRNL, ISIG, erase,
  echo) runs them, so a client types and does not emulate a terminal.

  Starting one is two steps, in that order, and the order is the point: the
  console is live and does not replay, so a driver that launched the guest and
  *then* connected was racing a run that may be over already —
  `busybox sh -c "..."` finishes in milliseconds. Host first, guest second:

  ```sh
  adb shell "am start -n com.rvvm.android/.SimpleLauncherActivity --ez console true"
  adb forward tcp:7979 tcp:7979
  adb shell "am start -n com.rvvm.android/.GuestActivity \
      --es guest_app_name test_busybox --es argv_b64 <base64 of NUL-joined argv>"
  ```

  The picker is the first step because it and the guest live in different
  tasks. Note `argv_b64` rather than `--esa argv`: `--esa` is a multi-value
  option, and multi-value options are where shells disagree — on a ColorOS
  build `--esa argv a b c` puts `[a]` in the intent and `--esa argv -c "ls"`
  throws `Argument expected after "-c"` out of `Intent.parseCommandArgs`.

  Binding is loopback only and the switch is per-launch: a debug channel
  reachable from the network is a remote shell into every guest on the device.
  `adb shell setprop debug.rvvm.console 7979` turns it on for every launch
  instead, the same property the trace switch uses. A client that stops
  reading is dropped rather than waited for — the writer is a guest thread, and
  a wedged client would take the emulator with it.

  Without any of this, the older path still works and is one command plus
  logcat: `am start` with `--es guest test_cli.exe --esa argv '...'`.
  `test_cli` treats its arguments as a command line (`;` separates commands)
  and exits with the number of failures, so the result is the guest's exit code
  in logcat plus the `RVVM-GUEST` output. It just cannot be *talked to*.


## Test knobs (guest side, `guest-samples/test_render_gles.c`)

- default: window-surface mode — the path a real app takes (native present).
- `RVVM_GL_TEST_OFFSCREEN=1`: pbuffer mode, the pixel-exact regression gate.
- `RVVM_GL_TEST_SIZE=WxH|full`: content size in offscreen mode, including the
  oversized case that exercises host-side cropping.

## Build

- Windows: `pwsh ./build_virtpass.ps1 -Target win32` (make target `bin`;
  `-RegenGlAbi` regenerates the GL dispatch from `tools/gen_gl_abi.py` first).
- Android: `pwsh ./build_virtpass.ps1 -Target apk` (make target `android`, which
  first cross-compiles every guest in `guest-samples/` for
  riscv64-linux-musl via zig cc into the APK assets).
- `pwsh ./build_virtpass.ps1` with no `-Target` builds both, plus the guest SDK.
- `pwsh ./build_virtpass.ps1 -?` lists every target; it only locates the
  toolchain and drives `mingw32-make`, so the raw Makefile goals work as well.

### Toolchain

| Tool | Version | Used for |
|---|---|---|
| MSYS2 MinGW64 gcc | 15+ | `mingw32-make`, the host binaries |
| zig cc | 0.14+ | every riscv64 guest artifact (guest stubs + `guest-samples/`) |
| Android SDK + NDK | 27.x | the Gradle/NDK-CMake build of `android-host/` (found via `local.properties`, `ANDROID_HOME` or `ANDROID_SDK_ROOT`) |
| Python 3 | 3.10+ | `tools/gen_gl_abi.py`, i.e. only with `-RegenGlAbi` |

Guests are built for **riscv64-linux-musl with zig cc, never the NDK/bionic**:
bionic links scudo, which reserves terabyte-scale address ranges at startup that
the Win32 host mmap layer cannot satisfy (the guest then exits 127). Those zig
flags carry `-fno-sanitize=undefined` because zig otherwise implies UBSan and
emits `__ubsan_handle_*` references that a static musl link does not provide.

`tools/gen_gl_abi.py` reads `GLES2/gl2.h` + `EGL/egl.h` from the NDK sysroot
(`NDK_SYSROOT`, else the NDK 27.x path hardcoded in the script) and rewrites
`include/virtpass/vp_gl.h`, `src/virtpass/vp_gl_stub.c`,
`src/virtpass/vp_gl_host_types.h`, `src/virtpass/vp_gl_host_entries.h`,
`src/virtpass/vp_gl_dispatch_tables.h` and
`src/virtpass/win32-host/win32_gl_backend.h`; guest and host must then be
rebuilt together (`-RegenGlAbi` regenerates, audits the pointer params and
rebuilds in one go).

## Sensor conformance test

`guest-samples/test_sensor_guest.c` is the regression gate for the sensor
path: it enumerates the sensors, runs a Looper-driven queue and asserts that
every event carries the guest-visible handle and type it enabled. See the file
header for what it pins down.

headers [text](include/virtpass)
win host [text](src/virtpass/win32-host)

android host [text](src/virtpass/android-host)


## 扩充应遵循
VirtPass ABI 中NDK部分，在进化时必须严格跟随 Android NDK 的 ABI 签名，可以根据情况仅使用某类的子集。