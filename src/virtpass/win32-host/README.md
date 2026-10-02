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
| key events | `WM_KEYDOWN / WM_KEYUP` reach the guest twice over: the console (fd 0, via `WM_CHAR` / `tty_input()`) and the GameActivity queue (`cmdpost_queue_key_event()`, `AKEYCODE_*` from `akeycode_from_vk()`) |

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

### rvvm_ash: the shell, split WSL-style

`rvvm_ash.exe` is the `bash.exe` of this project, and it plays the two roles a
WSL user expects:

- **the core** - `ash --serve` - one run (one machine, one rootfs) kept up while
  clients come and go. The guest program it boots is the session server
  (`vpsessiond`), so the core itself only relays what each session's pty produces.
- **a client** - `ash` - connect to the core (start one if none is running - the
  "wsl" autostart), ask for a session, and be its terminal. A client is stateless:
  it owns no machine and no guest filesystem.

`--serve` is the run boundary, a client is a session boundary.

```powershell
.\release.windows.x86_64\rvvm_ash_x86_64.exe --serve            # the core; stays up
.\release.windows.x86_64\rvvm_ash_x86_64.exe                    # a client: new session
.\release.windows.x86_64\rvvm_ash_x86_64.exe -c "ls /bin"       # one command, in a session
.\release.windows.x86_64\rvvm_ash_x86_64.exe --list             # cores this tree knows
.\release.windows.x86_64\rvvm_ash_x86_64.exe --shutdown         # ask the core to stop
.\release.windows.x86_64\rvvm_ash_x86_64.exe --serve --idle 300 # stop after 5 min idle
```

`--port N` / `RVVM_ASH_PORT` pick the port (default 7900). The core program is
fixed: `/sbin/vpsessiond`, a *guest* path installed from the bundle's system
layer - there is no shell override. A core registers under
`runtime/cores/` and holds a rootfs lock, so `--list`/`--shutdown` find it and a
second core cannot take the same writable layer.

A client puts the console in raw mode - no echo, no line
assembly, `^C` handed to the guest as a byte so *its* line discipline decides (a
shell gets SIGINT, `vi` a literal `^Z`) - sends the window size as a frame, and
turns a resize into `SIGWINCH`. The console is restored on the way out.

### Job control

The guest shell gets a real terminal, so it turns job control on (`bash.exe`'s
behaviour, not `can't access tty; job control turned off`): the run's shell is
the console's session leader and foreground group, and `^C` / `^Z` are delivered
to the *foreground process group* the shell named with `tcsetpgrp` - which may
live in another address space, since every `fork()` gets one of its own.

```powershell
.\release.windows.x86_64\rvvm_ash_x86_64.exe   # then, at the prompt:
#   sleep 30        ^C                  # the command dies, the shell survives
#   sleep 30        ^Z  jobs  fg  ^C    # stop / list / resume / kill
#   sleep 30 &      jobs  kill -CONT %1
```

What the core implements (`handover.md` §5 Step 7): process groups and sessions,
`setpgid`/`getpgid`/`getpgrp`/`getsid`/`setsid`, `kill(0)`/`kill(-pgid)`, the
console's and a pty's foreground group, `SIGTSTP`/`SIGSTOP`/`SIGTTIN`/`SIGTTOU`
parking a process and `SIGCONT` resuming it, `wait4`'s `WUNTRACED`/`WCONTINUED`
statuses, a cross-address-space `SIGCHLD` so the shell notices its jobs, and a
pty resize (`TIOCSWINSZ` on the master) raising `SIGWINCH` in the foreground
group - the shape a session server needs. `RVVM_TRACE=job` (or `all`) prints a
`job:` line for each of those steps (the trace categories; `RVVM_TRACE_PATH` is
kept as an alias for `path`, see `handover.md` §8).

```powershell
# The interactive timing cannot be reproduced through a pipe - use the driver:
foreach ($s in 'sigint','sigtstp','fg-resume','fg-again','bg','killpg','killpg-cont','wait-bg','wait-int') {
    pwsh ./tools/jobctl_e2e.ps1 -Scenario $s
}

# And the corners a shell cannot be asked about (WCONTINUED, the controlling
# terminal, and the pty line discipline / resize a session server drives) are a
# guest sample, booted as an app package on the console:
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --app test_jobctl   # 49 checks, PASS

# A shell's redirect has to survive fork(): open -> dup2 -> close -> fork, and
# the child (or an execve()d cat) must get a readable descriptor:
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --app test_forkfd   # 17 checks, PASS
```

Known gaps (see `handover.md` §6): `jobs` still shows `Stopped` after a
`kill -CONT` (busybox ash's own bookkeeping - the job does resume), `^Z` on a
long `nanosleep` takes effect when that sleep returns rather than instantly, and
`/dev/ttyN` still means the run's console (a *session's* own terminal is
`/dev/tty`, which the core now resolves through `TIOCSCTTY`).

### Sessions

The guest-side half of the WSL-style split is `vpsessiond`
(`src/virtpass/guest-samples/vpsessiond.c`): run it as the run root and the
machine becomes a *core* - one pty-backed shell per TCP connection, each with its
own controlling terminal (`setsid` + `TIOCSCTTY`, so `/dev/tty` inside the
session answers that pty and not the run's console), its own foreground process
group (`^C` / `^Z` written to the socket reach the job), and resize frames
(`ESC ] 999 ; R<rows>;<cols> BEL`) that turn into SIGWINCH.

```powershell
.\release.windows.x86_64\rvvm_ash_x86_64.exe --serve   # the core, kept up
.\release.windows.x86_64\rvvm_ash_x86_64.exe           # a session on it (new shell)
# ...and from a plain TCP client too - netcat, no protocol needed:
#   nc 127.0.0.1 7900     -> an interactive shell at 24x80
```

```powershell
# The end-to-end acceptance (real TCP clients, every assertion read off the socket):
pwsh ./tools/session_e2e.ps1            # single session: PASS (14 checks)
pwsh ./tools/session_e2e.ps1 -Multi     # two clients on one core: PASS (21 checks)
pwsh ./tools/ash_e2e.ps1                # ash --serve + ash clients: PASS (7 checks)
```

Two core bugs that the sessions once reproduced are fixed (see `handover.md`
§5 Step 8 and §6): a **host descriptor number recycled under a still-tracked
guest slot** (now the guest number is chosen by the fd table, the host number is
only the payload), and **`sendfile(2)` losing the bytes of `cat file`** because
it read the file through the host and then wrote it to a pty's synthetic number
(`EBADF`); `rvvm_sys_sendfile` now takes the same dispatch `read(2)/write(2)`
do. A third, older one surfaced with the persistence work: `openat` checked its
result as unsigned, so a **failed open installed a bogus fd** instead of
returning `ENOENT` (the guest got `read: Bad file descriptor`); the check is
signed now. `-Multi` and the persistence runs are what expose these - keep them
as regression drivers.

The core/client split is in place; the remaining distance to a full WSL is not
the session model but the surrounding system: no Windows-drive bridge (`/mnt/c`,
`wslpath`), `/sys` is empty, and the writable layer persists per release
directory rather than per named distro (see `handover.md` §6). `/proc` is
synthesized from the process registry (`rvvm_user.c`'s procfs section), so
`ps`/`top` and `/proc/<pid>/{stat,status,statm,cmdline,comm,fd}` and
`/proc/{self,uptime,stat,meminfo,version,cpuinfo,loadavg,filesystems,cmdline}`
work; `/proc/mounts` stays the bundle's real file. There is no init/login in the
rootfs.

### Persistence

`runtime/rootfs` is not a scratch copy: it is the writable layer the guest keeps
its state in, so a file created or edited in a session survives the next run.
The archive is unpacked once and stamped (`runtime/rootfs/.vp/install`), so a
later run of the same bundle does not walk over those changes; deletes - of a
materialized file, and of archive-only entries like busybox's symlink names -
are recorded (`runtime/rootfs/.vp/hidden`) and restored at the next mount. A
rootfs lock (`runtime/ash-core.lock`) keeps two cores off the same layer. The
`.vp` directory is the one host artifact inside the guest tree (`ls -a /`); the
shadow only knows archive entries, so it is not hidden - a small, known
deviation.

## Run

Build the guests first (`mingw32-make guest-assets`, zig/musl - see
`src/virtpass/README.md`); they land in the build tree, at
`release.<os>.<arch>/guest-assets/`, then:

A release carries no loose ELF at all: `mingw32-make dist` puts the host binary
next to `bundle/{rootfs,system,apps}.tar.gz`, and the host installs all three
into its `runtime/rootfs` (once, persistently): the base `/`, the system
programs (`/sbin/vpsessiond`), and every app under `/data/app/<id>`. The apps
archive is a tar of **`.vapp` packages** - `apps.tar.gz` -> `apps/<id>.vapp`,
each a zip with its own `meta.json` manifest (`vp_app.h`); the tar is the
bundle's delivery container, the zip is the app package (both formats nest). The
layers are flattened/installed at provision time - see `src/virtpass/README.md`.

```powershell
# the controlled way: an app package, by id - its entry and args come from the
# package's manifest, never from the command line. Extra args are appended.
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --app test_fibonacci
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --app test_cli asset fonts/JetBrainsMono-OFL.txt

# the picker (lists the bundle's apps; Run boots one the same way)
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --launcher

.\release.windows.x86_64\rvvm_winhost_x86_64.exe --help   # options + environment
```

A guest is only ever an app package (`--app`); there is no loose-ELF argument, so
a run cannot be pointed at an arbitrary program. `--launcher` shows the picker.

### Scripted runs (non-interactive)

The host is a **console application**: its stdout carries the guest's console
output and its stdin is pumped into the guest's console (see "Interactive
console"), so a run can be driven through either the app's extra argv or its
console. The host exits with the **guest's** exit code, so a check needs nothing
more than to wait on the process:

```powershell
$p = Start-Process -FilePath .\release.windows.x86_64\rvvm_winhost_x86_64.exe `
     -ArgumentList '--app','test_cli','asset','fonts/JetBrainsMono-OFL.txt' `
     -NoNewWindow -PassThru -RedirectStandardOutput guest.log
$p | Wait-Process -Timeout 45
$p.ExitCode        # the guest's own code
```

A guest that reads its console can be scripted the same way through stdin - the
same app, driven by its line protocol instead of its argv:

```powershell
$in = Join-Path $PWD t_in.txt
[IO.File]::WriteAllText($in, "ls /`ncalc 6 * 7`nexit`n", [Text.Encoding]::ASCII)
$p = Start-Process -FilePath .\release.windows.x86_64\rvvm_winhost_x86_64.exe `
     -ArgumentList '--app','test_cli' `
     -NoNewWindow -PassThru -RedirectStandardInput $in -RedirectStandardOutput guest.log
$p | Wait-Process -Timeout 45
$p.ExitCode        # 0: every command in the script succeeded
```

Or straight through a shell pipe:

```powershell
"calc 6 + 7`nexit`n" | .\release.windows.x86_64\rvvm_winhost_x86_64.exe --app test_cli
```

Guest output arrives on the host's stdout unprefixed and as it is written; the
host's own lines carry a `[winhost <ms>]` prefix (`gl_log`/`winhost_log`), and
rvvm's warnings go to stderr. That is why a run can be asserted on directly here:
the equivalent Android check has to pull the output out of logcat by its
`RVVM-GUEST` tag and parse the exit code out of a line, while on this host both
are the process's own stdout and exit status.

The one prerequisite is an interactive desktop session - the host always creates
its window, guest or not.

### Interactive console

A guest that reads its console (`test_cli`) is typed into directly in the
window. The keyboard goes to the guest's fd 0 through `rvvm_user_tty_input()`
- the same entry point the Android host's console tab uses - and the VTerm the
guest writes to is what `WM_PAINT` renders, so output and the echo of typing
share one screen. The window takes focus when a guest is booted, and again
whenever the client area is clicked (the picker's combo box would otherwise
keep it, see below).

```powershell
.\release.windows.x86_64\rvvm_winhost_x86_64.exe --assets release.windows.x86_64\guest-assets
# pick test_cli, Run, then type into the window:
#   vp> ls /
#   vp> calc 6 * 7
```

Key mapping (the bytes a terminal sends, which is what the core's line
discipline expects):

| Key | Bytes |
|---|---|
| printable ASCII | the character, UTF-8 |
| Enter | `CR` (0x0D; ICRNL turns it into NL) |
| Backspace | `DEL` (0x7F) - the window reports ASCII BS (0x08), mapped in `WM_CHAR` |
| Ctrl-C / Ctrl-D / Ctrl-Z | 0x03 / 0x04 / 0x1A |
| Tab | 0x09 |
| ↑ ↓ → ← Home End Delete | `ESC [ A` / `B` / `C` / `D`, `ESC [ H`, `ESC [ F`, `ESC [ 3 ~` |

Three host-side details worth knowing:

- The line discipline lives in `rvvm_user.c`, so the guest's own `termios`
  decides what Enter, Backspace and ^C mean - the host only supplies the bytes.
- Typing is echoed into the VTerm by that same path, which is *not* guest fd 1/2
  output, so `host_tty_cb()` never fires for it: `tty_input()` in
  `win32_cmdpost_bridge.c` flags the layer dirty and repaints itself, otherwise a
  shell sitting in `read(0)` would look dead until the guest printed something.
- The window is not the console's only source: the host's own stdin is pumped
  into the same line discipline by `stdin_pump_thread()`, which is what makes a
  run scriptable (see "Scripted runs"). It is started per run and **waits for
  `rvvm_user_is_started()` before reading**: `jump_start()` wipes the console
  state as it spins the vCPU up ("no type-ahead left in the ring"), so bytes
  delivered earlier are discarded - and a file or pipe only yields its bytes
  once. A console stdin is switched to raw first (no line mode, no echo), so the
  guest's discipline is the only one assembling lines, and it echoes them into
  the window rather than into the console.

In launcher mode the picker's combo and buttons stay visible while a guest runs,
and the console is drawn below them; the guest's own panel is still composited
underneath (the controls are child windows, the parent never paints over them).

### GameActivity keys

The same keystrokes also land in the GameActivity input queue, so a guest that
polls `android_app_swap_input_buffers()` receives them: a shell reads its
console, a game reads these, and neither path disturbs the other.
`akeycode_from_vk()` maps letters, digits, F1-F12, the arrows, the modifiers,
Enter / Escape / Backspace / Tab / space / Home / End / PageUp / PageDown /
Insert / Delete and the punctuation keys onto the `AKEYCODE_*` values VirtPass
carries (`virtpass/vp_android.h`); a key with no AKEYCODE is dropped.
`metaState` reports the Shift/Ctrl/Alt state from `GetKeyState()`, and
`repeatCount` is Win32's repeat count minus one (Android counts the first press
as 0).

`test_game_activity` prints every key event it is handed, which is the quickest
end-to-end check of the path:

```powershell
.\release.windows.x86_64\rvvm_winhost_x86_64.exe `
    release.windows.x86_64\guest-assets\test_game_activity.exe
# focus the window and press a key:
# GameActivity: key event 0 action=0 keyCode=29 metaState=0x0 repeat=0
```

On Android the same queue is fed from Java instead of from VK: the activity's
`dispatchKeyEvent()` hands over whatever its views did not consume
(`nativePostKeyEvent` -> `cmdpost_queue_key_event()`), so the console's
`TtyEditText` keeps its own keys and a game guest gets the rest. Volume, power
and Back stay with the device - Back keeps its "background the task" meaning
rather than becoming a guest key.

### Launcher (Android-style picker)

Run with **no guest argument** and the host shows a picker instead: **Run / Stop /
Suspend / Exit** buttons and a dropdown. What the dropdown lists, in order:

1. the apps in `bundle/apps.tar.gz`, by id - each booted as
   `/data/app/<id>/<entry>`, with its own payload and its own resources;
2. otherwise (no bundle) the `.exe` files in the assets directory -
   `<exe dir>/guest-assets` by default, or `--assets <dir>` / `RVVM_ASSETS`.
   Known sample names are used as a fallback when that is missing or empty.

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
  through its normal exit path (`cmdpost_end_run`, machine free) and the picker
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
| `RVVM_VERBOSE=1` | full syscall trace on stderr (winhost and the ash `--serve` core alike) |
| `RVVM_TRACE=<cats>` | trace categories: `path fd pty job tty signal mmap sys dev wsock` |
| `RVVM_DUMP_FRAME=1` | dump the first presented frames as `frame_NNN.bmp` |
| `RVVM_CONSOLE_PORT=N` | serve the guest console on `127.0.0.1:N` (the same adb-shaped byte pipe the Android host serves; the `vp` client attaches once it knows a local target) |
| `RVVM_USER_NO_THREADS` | force the guest single-threaded (clone -> EAGAIN) |

An ash core is detached, so its stderr disappears with it: set the vars, then
run `rvvm_ash --serve 2> core.log` yourself and connect from another console.

## Known gaps

1. **Semantic layer is partial.** The full core builds and runs real guests,
   but these syscalls return ENOSYS or take fallback paths in `posix_shim.c`:
   - `eventfd`, `futex`: Linux-only paths (also `__linux__`-guarded in
     `rvvm_user.c` itself)
   - `fork`/`wait4`: no process model on Windows
   - `statx`, `mremap`: no syscall case in `rvvm_user.c` for this host
   - signals: `sigaction` semantics are approximated, no real POSIX signal
     delivery (`tkill` returns ENOSYS, so musl `abort()` ends via
     `exit_group(127)`)
   - non-blocking I/O: `O_NONBLOCK` is honoured for reads (a pipe is asked with
     `PeekNamedPipe`, a socket with `FIONBIO`, and the core's own pty//dev
     descriptors answer `EAGAIN` themselves), and `F_SETFL(O_APPEND)` appends by
     hand. What Windows has no primitive for is **writing** to a full pipe
     without blocking, so a non-blocking write that would have to wait still
     does. `fcntl`'s flag word is kept per descriptor on both sides (`test_std`
     checks the read side, `dup` and `F_GETFL`)
   Networking works: `src/win/win_socket.c` backs the BSD socket shim with
   WinSock 2 (AF_INET/AF_INET6 sockets, `socketpair` over a loopback TCP pair,
   and an epoll emulated over `poll()`), translating the guest's Linux UAPI
   constants and anchoring each socket onto a CRT fd so read()/write()/
   close()/poll() keep working. Sockets take their own namespace
   (`rvvm_win_*`) because `src/util/networking.c` links the native WinSock
   names directly. `SCM_RIGHTS` fd passing is the one socket feature still
   refused (`EINVAL`).
   The filesystem layer is usable for real guests: guest `open(2)` flags are
   translated to the CRT ones (`O_CREAT` / `O_APPEND` / `O_EXCL` / `O_TRUNC`),
   directories can be opened and are enumerated through a `getdents64` built on
   `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` (with `.`/`..` and
   `d_type`), and `stat`/`fstat` take the live size, file index and the
   directory bit from the file handle - the CRT path helpers lag behind a just
   written file on some volumes and never report `S_IFDIR` for a directory fd.
   Symbolic and hard links work too: `symlink()`/`link()` go through
  `CreateSymbolicLink`/`CreateHardLink` (the link type is chosen from the target
  that exists at creation time, so a directory link can be followed as a
  directory), `readlink()` and `lstat()` read the reparse point, `readdir()`
  reports `DT_LNK`, and `open()`/`stat()` follow the link. Creating a symlink
  needs the Windows privilege or Developer Mode - a failure surfaces as `EPERM`.
  `mmap` of a file is still a read-only snapshot.
   A guest exercising the remaining gaps will fail; CPU-bound or file/graphics
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

3. **Sensors are stubs.** Fixed values pushed on a timer; wire up
   `Windows.Devices.Sensors` (WinRT) for real data.

4. **Assets.** The host mounts a directory at `/assets`. Which one:

   - when the run booted an app out of the bundle, **that app's own `assets/`**
     (`<rootfs>/data/app/<id>/assets`, from `vp_bundle_app_assets_path()`);
   - otherwise the tree the picker lists guests from (`--assets DIR` /
     `RVVM_ASSETS`, default `<exe dir>/guest-assets`).

   Note the app model is Android's, not per-run: *every* app in the bundle is
   provisioned once, persistently, under `/data/app/<id>`, and a run just points
   the mount at the one it booted. No tree is emptied between runs.

   The guest's `AAssetManager_*` calls are a shell over that mount. Because the
   tree is a real directory here, every mount op is a plain file call: `open()` hands
   out a real seekable descriptor (unlike the Android host, which streams through
   a pipe), `stat()` answers a size, `opendir()` the entries, and a name that
   tries to leave the tree is refused. `AAsset_openFileDescriptor()` therefore
   succeeds here - an asset is a plain uncompressed file, which is exactly the
   case the NDK allows - and reports start 0 with the file's own length, the mount
   opening the asset rather than a container that holds it. (The Android host
   streams its assets, so it cannot address one and answers -1: the NDK's own rule
   for an asset it cannot point at directly.) The guest side reads a whole asset
   into guest RAM at `AAssetManager_open()` - the NDK contract is random access -
   so an asset has to fit there.
   Guest *file system* paths are a separate matter: a relative path resolves
   against the guest's own virtual cwd - it starts at "/" and only `chdir(2)`
   moves it, never the host process's cwd - and is then mapped through the
   prefix; an absolute path is mapped directly. That mapping is a string join, so
   whether a path resolves at all depends on the prefix directory really
   existing: the WinHost points it at the run's materialized rootfs with
   `rvvm_user_set_prefix()` (from the bundle). There is no environment override;
   a host with no bundle asks for passthrough by setting the prefix to NULL.
   `/dev`, `/sys`, `/proc`, `/tmp` and `/var/tmp`
   are deliberate exceptions that pass through unmapped - and `/dev` and
   `/proc` are then answered by the core itself (`userland_dev_*` /
   `userland_proc_*` in `rvvm_user.c`), since the host has no such tree.

## Suggested next steps

1. Semantic emulation category by category (eventfd/futex equivalents, real
   signal delivery) - the main blocker for dynamic-linker guests.
2. Add the key-event queue (gap 3), then verify `test_game_activity` input
   end-to-end instead of logging.
3. Drop the leftover `CMakeLists.txt`/`CMakePresets.json`/`mingw_toolchain.cmake`
   harness and any `build/` output now that the root Makefile owns this target.
