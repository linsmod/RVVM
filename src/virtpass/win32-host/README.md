# RVVM Win32 Virtpass Host (`src/virtpass/win32-host`)

Windows port of the Android userland emulator's **host side** (the layer that
`jni_bridge.c` implements on Android). The guest side (`virtpass/vp_android.h`
+ `vp_ndk_stub.c`) and the kernel dispatch (`src/core/rvvm_user.c` ->
`cmdpost_dispatch`) are platform-independent and reused as-is; this skeleton
implements the Win32 callbacks so a riscv64 guest ELF gets a window, input,
lifecycle and sensors.

**Status: working demo.** `test_render` (bouncing square), `test_game_activity`
and `test_sensor_guest` run against the real RVVM core under MinGW.

## Architecture

```
+---------------------------+      ecall 0x10000+n       +--------------------+
| Guest (riscv64, ELF)      | --------------------------> | src/core/rvvm_user.c|
|  vp_ndk_stub.c            |                             |  cmdpost_dispatch() |
|  ANativeWindow_lock ...   | <-------------------------- |  (platform-agnostic)|
+---------------------------+       return value           +--------------------+
                                                                        |
                                            uses (single copy in src/virtpass)
                                                                       v
                                             src/virtpass/vp_cmdpost.c (dispatch,
                                             lifecycle/motion queues, ringbuf)
                                                                       |
                                                   host callbacks (this skeleton)
                                                                       v
                                              win32_cmdpost_bridge.c (Win32: DIB
                                              double buffer, mouse, keys, lifecycle,
                                              stub sensors) + win32_main.c
```

### Screen vs window (important invariant)

The Win32 window is **only a viewport**; the guest bitmap ("virtual screen") is
fully decoupled from it:

- `WINDOW_SET_BUF` resizes the DIB (double buffered) to the guest's geometry;
  the window size never changes and frames are blitted **1:1, never scaled**.
- The guest thread converts pixels into the back buffer and flips; it never
  touches GDI. Presentation happens exclusively on the UI thread in
  `WM_PAINT` via an offscreen DC (`InvalidateRect` coalesces at display rate).
- Host focus/activation noise (`WM_ACTIVATE/WM_SETFOCUS/WM_KILLFOCUS`) is
  deliberately NOT mapped to Android `PAUSE/RESUME/FOCUS` - treating cosmetic
  focus changes as lifecycle events makes the guest tear down its surface and
  the window visibly flash. The lifecycle state machine only sends the
  startup sequence on `WM_CREATE` and teardown on real exit.

## Guest ABI contract (verified against `vp_ndk_stub.c`)

| Syscall (SYS_ANDROID_BASE=0x10000 + n) | n | Win32 implementation here |
|---|---|---|
| `SENSOR_INIT`      | 1  | no-op (stub sensors below) |
| `SENSOR_ENABLE`    | 3  | `on_sensor_enable()`; handle table matches `g_sensors[]` in vp_ndk_stub.c (0=accel, 2=gyro, 3=light) |
| `SENSOR_READ`      | 4  | guest pops the ringbuf itself; host pushes via `cmdpost_push_sensor_event()` on a 100 ms timer while enabled (accel z=9.81, gyro zeros, light zeros) |
| `WINDOW_INIT`      | 5  | window already exists (`WM_CREATE` queues `APP_CMD_INIT_WINDOW`) |
| `LIFECYCLE`        | 7  | host->guest path: `cmdpost_queue_lifecycle_cmd()` consumed by the guest via GAME_POLL_CMD |
| `LOOPER_INIT`      | 9  | vp_cmdpost no-op |
| `WINDOW_LOCK`      | 11 | `on_window_lock()`: fills width/height/stride/format into the guest's buffer struct; **bits stays NULL** - the guest allocates its own pixels (`pixbuf_ensure`) |
| `WINDOW_UNLOCK`    | 12 | `on_window_unlock()`: converts the guest buffer (RGBA/RGBX/565, stride=w px) into the BGRA back DIB, flips front/back, then `InvalidateRect` |
| `WINDOW_GET_SIZE`  | 13 | `on_window_size()` |
| `WINDOW_SET_BUF`   | 14 | `on_window_set_buf()`: updates geometry/format, recreates the DIB pair |
| `GAME_CREATE/DESTROY` | 20/21 | vp_cmdpost logs |
| `GAME_POLL_CMD`    | 22 | vp_cmdpost hands out queued lifecycle cmds (START/RESUME/PAUSE/... + motion events) |
| `GAME_SWAP_INPUT`  | 23 | vp_cmdpost swaps the queued events into the guest input buffer |

Win32 message mapping:

| Android concept | Win32 message |
|---|---|
| `APP_CMD_START / INIT_WINDOW / RESUME / GAINED_FOCUS` | `WM_CREATE` (queued once, in Android startup order) |
| `APP_CMD_RESUME / PAUSE / GAINED / LOST_FOCUS` | **not mapped** - see "Screen vs window" above |
| `APP_CMD_WINDOW_RESIZED` | `WM_SIZE` |
| `APP_CMD_PAUSE / STOP / DESTROY` | `WM_CLOSE` |
| `APP_CMD_TERM_WINDOW` | `WM_DESTROY` |
| guest exit | `sys_exit`/`sys_exit_group` -> `host_guest_exit_cb()` (rvvm exit callback) records the code; once the guest thread unwinds, `WM_APP_GUEST_EXIT` -> teardown lifecycle + launcher returns to the picker (host mode exits with the guest's code). The callback must stay registered: rvvm_user.c otherwise `_Exit()`s the whole process from the guest thread |
| `AMOTION_EVENT_ACTION_DOWN/MOVE/UP` | `WM_LBUTTONDOWN / WM_MOUSEMOVE(+MK_LBUTTON) / WM_LBUTTONUP`, client coords mapped to surface coords |
| key events | `WM_KEYDOWN / WM_KEYUP` (logged; VK->AKEYCODE mapping + key-event queueing is future work) |

## Build (top-level Makefile)

Requirements: MSYS2 MinGW64 gcc (tested: 15.2.0).

The host is a handful of sources, so it is built directly by the root
`Makefile`/`project.mk` alongside the other binaries - there is no nested CMake
step. On Windows the `rvvm_winhost` bin target is added automatically (it needs
a 64-bit target, and it turns on `USE_VERTPASS` so `src/virtpass/vp_cmdpost.c`
is linked in):

```powershell
mingw32-make bin                      # everything, incl. rvvm_winhost.exe
mingw32-make release.windows.x86_64\rvvm_winhost_x86_64.exe   # just the host

# Thin wrapper that locates the toolchain, runs make and prints the exe path:
pwsh ./build_virtpass.ps1 -Target win32   # -Clean / -Jobs N / -RegenGlAbi available
```

The build compiles the real RVVM core (same macro set as the Android app) and
statically links it into the exe; `win32_main.c` + `win32_cmdpost_bridge.c`
(+ `win32_gl_backend.c` / `win32_gl_dispatch.c`) provide the host side,
`src/win/posix_shim.c` + `include/mingw_compat/` bridge the POSIX layer to
MinGW.

`win32_gl_backend.h` and `win32_gl_dispatch_tables.h` are generated by
`tools/gen_gl_abi.py` (see `src/virtpass/README.md`), not edited by hand.

## Run

Build the guests first (`mingw32-make android-assets`, zig/musl - see
`src/virtpass/README.md`); they land in the APK assets directory, then:

```powershell
.\release.windows.x86_64\rvvm_winhost_x86_64.exe `
    src\virtpass\android-host\app\src\main\assets\test_render.exe

.\release.windows.x86_64\rvvm_winhost_x86_64.exe --help   # options + environment
```

### Launcher (Android-style picker)

Run with **no guest argument** and the host shows a picker instead: a dropdown
listing the `.exe` guests found in the assets directory plus **Run / Stop /
Suspend / Exit** buttons. `--assets <dir>` (or `RVVM_ASSETS`) points at that directory;
the default is `src\virtpass\android-host\app\src\main\assets`. Known sample
names are used as a fallback when the directory is missing or empty.

```powershell
.\release.windows.x86_64\rvvm_winhost_x86_64.exe                # picker
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --assets D:\guests
```

- **Run** boots the selected guest into the existing window (it also sends
  the Android startup lifecycle). When the guest exits, the picker is
  re-shown in the same window so another guest can be run.
- **Stop** is cooperative first: it queues the Android teardown
  (`PAUSE`/`STOP`/`DESTROY`) so a well-behaved guest tears down and exits on
  its own. If the guest is still alive after `STOP_GRACE_MS` (1500 ms) it never
  polls its lifecycle commands (`test_render` renders in a loop and ignores
  `APP_CMD_DESTROY`), so the host takes it down itself via `rvvm_user_stop()`
  from `src/core/rvvm_user.c`. That is not `TerminateThread()`: it pauses every
  guest vCPU and marks every guest thread finished, so the guest still unwinds
  through its normal exit path (`cmdpost_cleanup`, machine free) and the picker
  comes back exactly as on a real guest exit. A forced stop reports exit code
  137 (128 + `SIGKILL`).
- **Suspend** (toggles to **Resume**) parks the guest without tearing it down:
  `rvvm_user_suspend()` / `rvvm_user_resume()` in `src/core/rvvm_user.c`. Every
  guest vCPU is kicked out of the interpreter with a hart pause (which also
  wakes WFI sleepers) and then parks in its wrap loop until resume; on resume it
  re-enters at the same PC, so nothing is lost and no exit callback fires. The
  frame clock is stopped while suspended (a parked guest polls neither frames
  nor lifecycle commands, so a live clock would only pile up vsync ticks).
  `rvvm_user_suspend()` blocks until the vCPUs have actually parked (bounded
  `USERLAND_SUSPEND_BARRIER_MS`, 200 ms - a guest blocked in a host syscall can
  only park once that returns). Pressing **Stop** or closing the window resumes
  a suspended guest first, so the cooperative teardown still has something
  polling it. Measured on `test_render`: ~1.9 s CPU per 1.5 s wall while running,
  **0 ms** while suspended, ~1.8 s again after resume.

Debug switches:

| Env var | Effect |
|---|---|
| `RVVM_VERBOSE=1` | full syscall trace on stderr |
| `RVVM_DUMP_FRAME=1` | dump the first presented frames as `frame_NNN.bmp` |
| `RVVM_USER_NO_THREADS` | force the guest single-threaded (clone -> EAGAIN) |

## Known gaps

1. **Semantic layer is partial.** The full core builds and runs real guests,
   but these syscalls return ENOSYS or take fallback paths in `posix_shim.c`:
   - `epoll`, `eventfd`, `futex`: Linux-only paths (also `__linux__`-guarded
     in `rvvm_user.c` itself)
   - `fork`/`wait4`: no process model on Windows
   - sockets: `sys/socket.h` shims exist, but socket syscalls are stubbed
   - signals: `sigaction` semantics are approximated, no real POSIX signal
     delivery (`tkill` returns ENOSYS, so musl `abort()` ends via
     `exit_group(127)`)
   A guest exercising those paths will fail; CPU-bound or file/graphics
   based guests are the reachable target.
   The Makefile build reuses the regular `USE_WIN32_GUI`/`USE_WIN32_COMPAT`
   host macro set plus `RVVM_USER_TEST` (set globally on Windows for
   `rvvm_user`); the flags `RVVM_STATIC`, `USE_NO_RVJIT` and the
   `core_force_includes.h` force-include that the old CMake build needed are
   no longer required (`RVVM_VERSION` is always defined here, and
   `rvvm_user.c` now includes `rvvm_user.h` itself).

2. **Guest toolchain must be zig/musl, not NDK/bionic.** NDK guests link
   bionic, whose scudo allocator reserves terabyte-scale address ranges at
   startup; that fails on the host mmap layer and the guest exits 127. See
   `src/virtpass/README.md`.

3. **Keyboard input is logged, not delivered.** The guest input buffer swap
   path only carries motion events today; add a key-event queue to vp_cmdpost
   (struct layout already exists: `cmdpost_GameActivityKeyEvent`) plus a
   VK->AKEYCODE table in the bridge.

4. **Sensors are stubs.** Fixed values pushed on a timer; wire up
   `Windows.Devices.Sensors` (WinRT) for real data.

5. **Assets.** `ASSET_OPEN` is unimplemented in vp_cmdpost; guest asset reads
   resolve relative to the current working directory
   (`RVVM_USER_PREFIX` is set to empty, same as jni_bridge.c).

## Suggested next steps

1. Semantic emulation category by category (sockets, epoll/eventfd/futex
   equivalents, real signal delivery) - the main blocker for dynamic-linker
   and networked guests.
2. Add the key-event queue (gap 3), then verify `test_game_activity` input
   end-to-end instead of logging.
3. Drop the leftover `CMakeLists.txt`/`CMakePresets.json`/`mingw_toolchain.cmake`
   harness and any `build/` output now that the root Makefile owns this target.
