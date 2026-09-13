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

- `vp_cmdpost.c/.h` — the ABI proxy core: lifecycle/input queues, the GL/EGL
  and AAudio entries, and the callback registration points every host fills in.
- `vp_sensor.c/.h` — the host-side sensor subsystem: it caches the device
  descriptors, owns the event queues with their bounded staging FIFO and the
  Looper wake fd, and exposes one `vp_sensor_ops_t` for a platform backend to
  implement. Guest-visible wire format: `virtpass/vp_sensor_abi.h`.
- Guest stubs: `vp_ndk_stub.c` (NDK APIs), `vp_gl_stub.c` (EGL/GLES), and
  `vp_aaudio_stub.c` (AAudio). GL calls are marshalled through
  `SYS_GL_CALL`/`SYS_EGL_CALL` with guest addresses in `args[]`.
- Generated GL dispatch (from `tools/gen_gl_abi.py`): `vp_gl_host_types.h`
  (neutral `vpgl_` types + PFN typedefs), `vp_gl_host_entries.h` (the `p_*`
  storage, included by exactly one host TU per host) and
  `vp_gl_dispatch_tables.h` (name tables + generic switch). The argument
  translation rules exist exactly once; each host adds a thin backend on top.

## Windows (`win32-host/`)

- Entry: `rvvm_winhost.exe` (`win32_main.c`) — message pump, launcher UI,
  `--display WxH@PPI`, `--assets DIR` (or `RVVM_ASSETS`).
- `win32_cmdpost_bridge.c` — window/config/lifecycle/input callbacks and the
  two-layer display model: layer 1 is the host-owned virtual panel the guest
  observes, layer 2 is the OS window (a pure viewport). CPU rendering is
  presented by copying the guest frame into a BGRA DIB on
  `ANativeWindow_unlockAndPost`.
- GL (`win32_gl_backend.c` + `win32_gl_dispatch.c`): Windows has no native
  GLES, so ANGLE is loaded from the Android SDK emulator installation. A guest
  window surface is downgraded to a panel-sized pbuffer (EGL_WIDTH/HEIGHT
  injected when the guest's attribute list has none) and every
  `eglSwapBuffers` blits the pbuffer through the DIB. All 157 entry points go
  through the shared generated dispatch.
- Audio: `win32_aaudio_wasapi.c` implements the AAudio ops over WASAPI.
- Sensors: `win32_sensor_stub.c` provides three virtual sensors, driven from
  the WM_TIMER on the UI thread and fed into `vp_sensor_ingest()`.
- Vsync: a 60 Hz clock driven by DwmFlush.

## Android (`android-host/`)

- Gradle app; native side lives in `app/src/main/cpp/`. `jni_bridge.c` is the
  JNI surface (`RvvmNative.java`, `MainActivity.java`): guest thread
  management, sensor manager, lifecycle/motion queues, and reinstalling every
  cmdpost callback before each guest run (`cmdpost_cleanup` NULLs them on
  guest exit).
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
- Diagnostics: guest stdout/stderr (write and writev) land in logcat under the
  `RVVM-GUEST` tag; GL calls trace under `RVVM-GL` with `RVVM_GL_TRACE`.
  A guest can be launched directly with
  `am start -n com.rvvm.android/.MainActivity --es guest <name>.exe`.

## Test knobs (guest side, `guest-samples/test_render_gles.c`)

- default: window-surface mode — the path a real app takes (native present).
- `RVVM_GL_TEST_OFFSCREEN=1`: pbuffer mode, the pixel-exact regression gate.
- `RVVM_GL_TEST_SIZE=WxH|full`: content size in offscreen mode, including the
  oversized case that exercises host-side cropping.

## Build

- Windows: `pwsh ./build_virtpass.ps1 -Target win32` (make target `bin`;
  `-RegenGlAbi` regenerates the GL dispatch from `tools/gen_gl_abi.py`).
- Android: `pwsh ./build_virtpass.ps1 -Target apk` (make target `android`, which
  first cross-compiles every guest in `guest-samples/` for
  riscv64-linux-musl via zig cc into the APK assets).
- `pwsh ./build_virtpass.ps1` with no `-Target` builds both, plus the guest SDK.

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