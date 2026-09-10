# Building Userland Host Components

This document covers all build systems for the RVVM **userland** (guest
emulation) components. The userland stack lets riscv64 guest ELF programs
call Android NDK-style APIs (sensors, window, input, GameActivity) through
custom hypercalls, with platform-specific host bridges.

## Quick Reference

| Target | Toolchain | Output | Use Case |
|--------|-----------|--------|----------|
| Guest ELFs | zig cc / riscv64 NDK clang | `src/virtpass/guest-samples/*.c` | Run on host or Android |
| Win32 host | MinGW gcc + Makefile | `release.windows.x86_64/rvvm_winhost_x86_64.exe` | Run guests on Windows |
| Android app | Android NDK + Gradle | APK containing `librvvm_jni.so` | Install on Android device |
| GL ABI headers | Python 3 | `include/virtpass/vp_gl.h`, `src/virtpass/vp_gl_stub.c`, `win32_gl_backend.h`, `win32_gl_dispatch_tables.h` | Auto-generated from NDK headers |

## Prerequisites

All tools must be on `PATH` or set via env vars:

| Tool | Version | How to get | Env var |
|------|---------|------------|---------|
| **zig cc** | 0.14+ | `winget install zig.zig` | `zig` on PATH |
| **MSYS2 MinGW64** | gcc 15+ | [msys2.org](https://www.msys2.org/) | `C:\msys64\mingw64\bin` on PATH |
| **CMake** | >= 3.15 | `winget install cmake` | `cmake` on PATH |
| **Android NDK** | 27.x | Android SDK | `NDK_ROOT` (default: `H:\AndroidSdk\Sdk\ndk\27.0.12077973`) |
| **Python 3** | 3.10+ | `winget install python` | `python` on PATH |
| **git** | any | `winget install git` | — |

### Verify all tools

```powershell
# From repo root (H:\github_repos\RVVM)
zig version
cmake --version
python --version
# NDK check:
ls H:\AndroidSdk\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin\riscv64-linux-android35-clang
```

---

## 1. Building Guest ELF Programs (riscv64)

Guest programs are riscv64 static ELFs. They call Android NDK APIs through
`vp_ndk_stub` and optionally `vp_gles_stub` for EGL/GLES.

### 1A. Using the PowerShell build scripts (recommended)

Both scripts are thin wrappers around the repository `Makefile` — they only
locate the toolchain, run `mingw32-make` and report the artifacts.

```powershell
# From repo root
pwsh ./build_virtpass-android.ps1 -Target assets  # zig/musl riscv64 guest ELFs -> APK assets
pwsh ./build_virtpass-android.ps1                 # guest assets + librvvm_jni.so + APK
pwsh ./build_virtpass-win32.ps1 -RegenGlAbi       # regenerate GL ABI headers, then build

# Equivalent raw make invocations
mingw32-make android-assets        # guest ELFs only
mingw32-make android               # guest ELFs + librvvm_jni.so + APK
```

### 1B. Manual: NDK riscv64 clang

```powershell
# From repo root
$NDK = "H:\AndroidSdk\Sdk\ndk\27.0.12077973"
$CC = "$NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\riscv64-linux-android35-clang"
$AR = "$NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-ar"
$NDK_INC = "$NDK\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include"
$VP = "src\virtpass"

# vp_android_stub.a
& $CC -c -I$NDK_INC -Iinclude -o "$VP\vp_ndk_stub.o" "$VP\vp_ndk_stub.c"
& $AR rcs "$VP\vp_android_stub.a" "$VP\vp_ndk_stub.o"

# vp_gles_stub.a (Phase 3 GL/EGL marshaling)
& $CC -c -I$NDK_INC -Iinclude -o "$VP\vp_gl_stub.o" "$VP\vp_gl_stub.c"
& $AR rcs "$VP\vp_gles_stub.a" "$VP\vp_gl_stub.o"

# Test programs (all link against both stub libs)
New-Item -ItemType Directory -Force build | Out-Null
foreach ($t in @('test_sensor_guest', 'test_render', 'test_render_gles', 'test_game_activity')) {
    & $CC -static -O2 -Iinclude `
        -o "build\$t" "$VP\guest-samples\$t.c" `
        "$VP\vp_android_stub.a" "$VP\vp_gles_stub.a"
}
```

### 1C. Manual: zig / musl (host build)

**zig is preferred for host builds** because NDK guests link bionic whose
scudo allocator reserves terabyte-scale address ranges that fail on the
Win32 host mmap layer.

```powershell
# From repo root
$VP = "src\virtpass"

# vp_android_stub.a
zig cc -target riscv64-linux-musl -c -fno-sanitize=undefined `
    -Iinclude -o "$VP\vp_ndk_stub.o" "$VP\vp_ndk_stub.c"
zig ar rcs "$VP\vp_android_stub.a" "$VP\vp_ndk_stub.o"

# vp_gles_stub.a
zig cc -target riscv64-linux-musl -c -fno-sanitize=undefined `
    -Iinclude -o "$VP\vp_gl_stub.o" "$VP\vp_gl_stub.c"
zig ar rcs "$VP\vp_gles_stub.a" "$VP\vp_gl_stub.o"

# Test programs
New-Item -ItemType Directory -Force build | Out-Null
foreach ($t in @('test_sensor_guest', 'test_render', 'test_render_gles', 'test_game_activity')) {
    zig cc -target riscv64-linux-musl -static -O1 -fno-sanitize=undefined `
        -Iinclude `
        -o "build\$t" "$VP\guest-samples\$t.c" `
        "$VP\vp_android_stub.a" "$VP\vp_gles_stub.a"
}
```

The `android-assets` make target automates the zig build shown above and drops
the resulting ELFs into `src\virtpass\android-host\app\src\main\assets\`:

```powershell
mingw32-make android-assets
```

Every `guest-samples/*.c` is bundled by default (`ANDROID_GUEST_SAMPLES` is
derived from the directory listing), so adding a new sample needs no Makefile
edit — override the variable to build a subset.

### Why `-fno-sanitize=undefined` for zig?

Without this flag, zig may implicitly enable UBSan which produces
undefined symbols (`__ubsan_handle_*`) that the static musl build
doesn't provide. The NDK path doesn't have this issue.

### Why NDK vs zig?

| | NDK riscv64 | zig / musl |
|---|---|---|
| libc | bionic | musl |
| Host runs? | ❌ bionic scudo fails on Win32 mmap | ✅ plain static ELF |
| Android device? | ✅ | ✅ (musl is compatible) |
| Use case | Android device / NDK riscv64 | Host testing / Android device |

---

## 2. Building the Win32 Host

The Win32 host lets you run riscv64 guest ELFs on Windows. It provides
the same callbacks that `jni_bridge.c` provides on Android.

### Using the build script

```powershell
# From repo root - produces release.windows.x86_64\rvvm_winhost_x86_64.exe
pwsh ./build_virtpass-win32.ps1
pwsh ./build_virtpass-win32.ps1 -Clean -Jobs 8
pwsh ./build_virtpass-win32.ps1 -RegenGlAbi

# With GL backend selection:
$env:RVVM_GL_BACKEND = "swiftshader"
pwsh ./build_virtpass-win32.ps1

$env:RVVM_GL_DLL_DIR = "H:\AndroidSdk\Sdk\emulator\lib64\gles_swiftshader"
pwsh ./build_virtpass-win32.ps1
```

### Direct Makefile build

The host is a plain `bin` target declared in `project.mk` (it enables
`USE_VERTPASS` on 64-bit Windows) — no CMake involved:

```powershell
# From repo root (this also builds rvvm.exe / rvvm_user.exe)
mingw32-make bin

# Or just the host, by explicit target file
mingw32-make release.windows.x86_64\rvvm_winhost_x86_64.exe
```

### Environment variables for Win32 host

| Env var | Default | Description |
|---------|---------|-------------|
| `RVVM_GL_BACKEND` | `angle` | GL backend: `angle`, `swiftshader`, `off` |
| `RVVM_GL_DLL_DIR` | guessed from `ANDROID_SDK_ROOT` | Directory containing `libEGL.dll` / `libGLESv2.dll` |
| `RVVM_VERBOSE` | — | Enable syscall trace on stderr |
| `RVVM_DUMP_FRAME` | — | Dump presented frames as `frame_NNN.bmp` |
| `RVVM_GL_PRESENT` | `readback` | Present mode: `readback` or `direct` (reserved) |

### GL backend DLL locations

| Backend | Default path |
|---------|-------------|
| angle | `H:\AndroidSdk\Sdk\emulator\lib64\gles_angle` |
| swiftshader | `H:\AndroidSdk\Sdk\emulator\lib64\gles_swiftshader` |
| off | N/A — disables GL, falls back to CPU pixel path |

### Running the Win32 host

```powershell
.\release.windows.x86_64\rvvm_winhost_x86_64.exe src\virtpass\android-host\app\src\main\assets\test_render.exe
```

---

## 3. Building the Android App

The Android app builds `rvvm_jni.so` which loads the RVVM core and
bridges to Android NDK APIs.

### Using Android Studio

1. Open `src\virtpass\android-host\` in Android Studio
2. Build → Make Project (or `.\gradlew.bat assembleDebug`)
3. Install the APK on a device

### Using command line

```powershell
# From repo root - guest assets + librvvm_jni.so + APK
mingw32-make android ANDROID_VARIANT=debug
pwsh ./build_virtpass-android.ps1 -Target apk
```

### CMakeLists.txt notes

The Android CMakeLists (`src/virtpass/android-host/app/src/main/cpp/CMakeLists.txt`) builds:
- `cmdpost` — single-copy vp_cmdpost from `src/virtpass/vp_cmdpost.c`
- `rvvm_core` — RVVM emulator core
- `rvvm_jni` — JNI bridge (`jni_bridge.c`)

The riscv64 guest stubs (`vp_ndk_stub.c` / `vp_gl_stub.c`) are **not** built by
this CMake project: the NDK targets arm64/x86_64 only, so guests are
cross-compiled separately with `zig cc` (`mingw32-make android-assets`).

The include directory `${RVVM_ROOT}/include` is added so that `jni_bridge.c`
can reference `virtpass/vp_gl.h` for fn_id macros.

---

## 4. Regenerating GL ABI Headers

The GL guest stubs and host dispatch tables are auto-generated from NDK
headers by `tools/gen_gl_abi.py`. This ensures the fn_id numbering,
`gl_call` struct layout, and function signatures are identical on both
sides.

```powershell
# From repo root
python tools\gen_gl_abi.py
```

This regenerates:
- `include/virtpass/vp_gl.h` — guest ABI types + fn_id macros
- `src/virtpass/vp_gl_stub.c` — 142 GLES + 16 EGL marshaling stubs
- `src/virtpass/win32-host/win32_gl_backend.h` — host types + ABI block
- `src/virtpass/win32-host/win32_gl_dispatch_tables.h` — generic dispatch switches

### NDK header dependency

The script reads:
- `GLES2/gl2.h` from NDK sysroot
- `EGL/egl.h` from NDK sysroot

Set `NDK_SYSROOT` env var if NDK is at a non-default path:
```powershell
$env:NDK_SYSROOT = "H:\AndroidSdk\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include"
python tools\gen_gl_abi.py
```

### Recompile after regenerating

After `gen_gl_abi.py`, run either build script with `-RegenGlAbi` (it
regenerates, then rebuilds), or recompile manually:
1. `mingw32-make android-assets` — rebuilds the zig guest stubs + samples
2. `mingw32-make bin` — rebuilds the Win32 host against the new headers
3. `mingw32-make android` — rebuilds the APK

---

## 5. Build Architecture Summary

```
repo root (H:\github_repos\RVVM)
│
├── src/
│   ├── core/                           # RVVM core (single copy, shared)
│   │   └── rvvm_user.c                 # User-mode runner (Android proxy syscall cases)
│   ├── virtpass/                       # VirtPass shared layer
│   │   ├── vp_cmdpost.{c,h}            # Host-side syscall dispatch + callback typedefs
│   │   ├── vp_ndk_stub.c               # Guest-side NDK proxy (riscv64)
│   │   ├── vp_gl_stub.c                # Guest-side GL/EGL proxy (riscv64)
│   │   ├── guest-samples/              # Test guest programs (sources)
│   │   │   ├── test_sensor_guest.c
│   │   │   ├── test_render.c
│   │   │   ├── test_render_gles.c
│   │   │   └── test_game_activity.c
│   │   └── android-host/               # Android app (gradle + CMake)
│   │       └── app/src/main/
│   │           ├── cpp/
│   │           │   ├── CMakeLists.txt  # rvvm_core + cmdpost + rvvm_jni
│   │           │   ├── jni_bridge.c    # JNI callbacks for Android
│   │           │   └── ndk_compat.c    # memfd_create/statx for API < 30
│   │           ├── java/com/rvvm/android/
│   │           └── assets/             # guest ELFs pushed into the APK
│   └── win/
│       └── posix_shim.c                # POSIX→Win32 shim implementations
│
├── include/
│   ├── virtpass/                       # Public virtpass headers
│   │   ├── vp_android.h
│   │   ├── vp_gl.h
│   │   └── vp_sensor_ringbuf.h
│   └── mingw_compat/                   # POSIX header shims for MinGW (sys/uio.h, ...)
│
├── src/virtpass/win32-host/            # Win32 virtpass host (rvvm_winhost bin target)
│   ├── win32_gl_backend.c             # GL DLL loading + p_* resolution
│   ├── win32_gl_backend.h             # generated: host types + ABI block
│   ├── win32_gl_dispatch.c            # GL dispatch callbacks
│   ├── win32_gl_dispatch_tables.h     # generated: generic dispatch switches
│   ├── win32_cmdpost_bridge.c         # Host callbacks (present_frame, etc.)
│   └── win32_main.c                   # Win32 UI entry point
│
├── Makefile / project.mk               # Build system (bin, android*, test, clean)
├── build_virtpass-win32.ps1            # Win32 host wrapper (make bin)
├── build_virtpass-android.ps1          # Guest assets / JNI / APK wrapper
└── tools/
    └── gen_gl_abi.py                  # GL ABI header generator from NDK headers
```

### Code sharing rules

- `src/virtpass/vp_cmdpost.{c,h}` — **single copy**, referenced by all platforms
- `src/core/rvvm_user.c` — **single copy**, contains platform-agnostic dispatch
- `src/virtpass/vp_ndk_stub.c`, `src/virtpass/vp_gl_stub.c` — **guest-side** riscv64 stubs
- `include/virtpass/` — public headers shared by host and guest sides
- `src/virtpass/guest-samples/` — guest test programs (sources)
- `src/virtpass/android-host/` — Android app (gradle project + NDK CMake)
- `src/win/posix_shim.c` + `include/mingw_compat/` — POSIX layer for the Win32/MinGW host

---

## 6. Troubleshooting

### `make: command not found`

Use `mingw32-make` from MSYS2 MinGW64 (the build scripts locate it for you):
```powershell
mingw32-make android-assets
```

Or use a PowerShell build script: `pwsh ./build_virtpass-android.ps1 -Target assets`

### `riscv64-linux-android35-clang: not found`

NDK path is wrong or NDK not installed:
```powershell
$env:NDK_ROOT = "H:\AndroidSdk\Sdk\ndk\27.0.12077973"
```

### `zig: command not found`

```powershell
winget install zig.zig
# Restart terminal after install
```

### UBSan undefined symbols when building with zig

Always use `-fno-sanitize=undefined` with zig:
```bash
zig cc -target riscv64-linux-musl -c -fno-sanitize=undefined ...
```

### `bionic: scudo internal map failure` when running guests on host

You're using NDK-built guests on the Win32 host. Switch to zig/musl:
```powershell
pwsh ./build_virtpass-android.ps1 -Target assets
```

### `mingw32-make: command not found`

Install MSYS2 and add the MinGW64 toolchain to `PATH`:
```powershell
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
```

### GL not working (black/garbled screen)

1. Verify `RVVM_GL_BACKEND` points to a valid DLL directory
2. Check `RVVM_GL_DLL_DIR` contains `libEGL.dll` and `libGLESv2.dll`
3. Try `RVVM_GL_BACKEND=off` to verify the CPU fallback path works
4. If using `swiftshader`, ensure the DLL directory has all dependencies

### Guest ELF link errors (`vp_gl_stub` / `vp_ndk_stub` missing)

Don't link the stubs by hand — let the Makefile build them together with the
samples (the `android-assets` target compiles `vp_ndk_stub.c` / `vp_gl_stub.c`
for riscv64 and links each guest program against them):

```powershell
mingw32-make android-assets
```
