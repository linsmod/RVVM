override NAME    := RVVM
override DESC    := The RVVM Project
override URL     := https://github.com/LekKit/RVVM
override VERSION := v0.7-git

override define LOGO
$(BLUE) 🭥█████🭐 ██  ██ ██  ██🭢█🭌🬿  🭊🭁█🭚
$(BLUE)  ██  🭨█🭬██  ██ ██  ██ ███🭏🭄███
$(ORANGE)  █████🭪 $(BLUE)██  ██ ██  ██$(ORANGE) ██🭥🭒🭝🭚██
$(ORANGE)  ██  🭖█🭀🭕█🭏🭄█🭠 🭕█🭏🭄█🭠 ██ 🭢🭗 ██
$(ORANGE)  ██  🭦█🭛 🭥🭒🭝🭚   🭥🭒🭝🭚  █🭠    ██
$(ORANGE)  █🭠   🭠🭗  🭢🭗     🭢🭗   🭠🭗    🭕█
$(ORANGE)  🭠🭗                         🭢🭕
endef

#
# Platform-dependent default project configuration
#

ifneq (,$(filter linux,$(OS)))
# Enable Wayland on Linux by default
USE_WAYLAND ?= 1
endif
ifneq (,$(filter linux %bsd sunos,$(OS)))
# Enable X11 on Linux, *BSD, Solaris by default
USE_X11 ?= 1
endif
ifneq (,$(filter windows,$(OS)))
USE_WIN32_GUI    ?= 1
# POSIX-on-Win32 compatibility layer: include/mingw_compat headers + src/win/posix_shim.c
USE_WIN32_COMPAT ?= 1
endif
ifneq (,$(filter haiku,$(OS)))
USE_HAIKU_GUI ?= 1
endif
ifneq (,$(filter darwin,$(OS)))
# Enable the native Cocoa GUI backend on macOS by default
USE_COCOA_GUI ?= 1
endif
ifneq (,$(filter serenity,$(OS)))
# Enable SDL2 on Serenity by default
USE_SDL ?= 2
endif
ifneq (,$(filter redox,$(OS)))
# Enable SDL1 and disable networking on Redox by default
USE_SDL ?= 1
USE_NET ?= 0
USE_LIB ?= 0
endif
ifneq (,$(filter emscripten,$(OS)))
# Enable SDL2 on Emscripten by default
USE_SDL ?= 2
endif
ifneq (,$(filter dos,$(OS)))
# Disable JIT/network/sound, emulate threads/atomics in DOS
USE_JIT        ?= 0
USE_NET        ?= 0
USE_SOUND      ?= 0
USE_ATOMIC_EMU ?= 1
USE_THREAD_EMU ?= 1
endif

# Only allow dynamic linking to librvvm in released versions
ifneq (,$(findstring -,$(VERSION)))
USE_LIB_SHARING ?= 0
endif

#
# Default project build configuration
#

# CPU features
USE_RV32 ?= 1 # Support riscv32imacb guests
USE_RV64 ?= 1 # Support riscv64imacb guests
USE_FPU  ?= 1 # Support FPU extensions
USE_RVV  ?= 0 # Support Vector extension

# Usability features
USE_GUI     ?= 1          # Enable guest display GUI
USE_SDL     ?= 0          # Enable SDL as GUI backend - usually picked on per-platform basis
USE_NET     ?= 1          # Enable networking support
USE_SOUND   ?= 0          # Enable sound support
USE_GDBSTUB ?= $(USE_NET) # Support debugging the guest via GDB remote protocol

# Board features
USE_FDT  ?= 1 # Enable Flattened Device Tree automatic generation
USE_VFIO ?= 1 # Support PCIe VFIO pass-through on Linux hosts

# Infrastructure
USE_INFRA_TESTS ?= 0 # Build infrastructure tests
USE_LIBS_PROBE  ?= 0 # Probe libraries in runtime instead of linking to them
USE_LOCK_DEBUG  ?= 1 # Runtime lock debugging & locking debug info
USE_ISOLATION   ?= 1 # Process isolation via seccomp/pledge
USE_JNI         ?= 0 # Enable JNI support in librvvm

# Acceleration
# Enable JIT by default on x86_64, arm64, riscv64
USE_JIT ?= $(if $(filter x86_64 arm64 riscv64 loongarch64,$(ARCH)),1,0)
USE_KVM ?= 0

# Misc toggles for debugging host platform/compiler issues
USE_NO_STACKTRACE ?= 0 # Disable post-mortem crash stacktraces
USE_NO_DLIB       ?= 0 # Disable dynamic library/symbol probing via dlsym()/GetProcAddress()
USE_STDIO         ?= 0 # Use non-threaded stdio fallback IO backend (Instead of Win32/POSIX)
USE_SELECT        ?= 0 # Use select() event interface fallback for networking (Instead of epoll/kqueue)

USE_SOFT_FPU_WRAP ?= 0 # Wrap native floating-point types into bitcasted representation (Fixes 8087 FPU)
USE_SOFT_FPU_FENV ?= 0 # Emulate FPU exceptions (Note this isn't soft-fp, and still fairly fast)

USE_FUTEX_EMU  ?= 0 # Emulate futexes via pthread_cond / Win32 Event / etc
USE_ATOMIC_EMU ?= 0 # Emulate atomics via host mutex
USE_THREAD_EMU ?= 0 # Emulate threads via guest CPU preemption

USE_NO_THREAD_LOCAL ?= 0 # Disable and undefine THREAD_LOCAL attribute
USE_NO_BUILD_ASSERT ?= 0 # Disable build-time assertions
USE_NO_RANDSTRUCT   ?= 0 # Disable struct randomization via randomize_layout
USE_NO_ALIGN_TYPE   ?= 0 # Disable type alignment where it's optional
USE_NO_SOURCE_OPT   ?= 0 # Disable per-source manual optimization level
USE_NO_PREFETCH     ?= 0 # Disable use of __builtin_prefetch()
USE_NO_LIKELY       ?= 0 # Disable use of __builtin_expect()
USE_NO_FORCEINLINE  ?= 0 # Disable force inlining
USE_NO_NOINLINE     ?= 0 # Disable force un-inlining
USE_NO_SLOW_PATH    ?= 0 # Disable slow_path attribute
USE_NO_FLATTEN      ?= 0 # Disable flatten_calls attribute

ifneq (,$(call var_use,USE_TAP_LINUX))
$(call log_warn,Linux TAP is deprecated in favor of USE_NET due to checksum issues)
endif

#
# Useflag handling
#

# Useflag conditional sources
override SRC_USE_WIN32_GUI := $(SRCDIR)/gui/win32_window.c
override SRC_USE_HAIKU_GUI := $(SRCDIR)/gui/haiku_window.cpp
override SRC_USE_COCOA_GUI := $(SRCDIR)/gui/cocoa_window.c
override SRC_USE_X11       := $(SRCDIR)/gui/x11_window.c
override SRC_USE_SDL       := $(SRCDIR)/gui/sdl_window.c
override SRC_USE_WAYLAND   := $(SRCDIR)/gui/wayland_window.c

override SRC_USE_TAP_LINUX := $(SRCDIR)/devices/tap_linux.c
override SRC_USE_NET       := $(SRCDIR)/util/networking.c $(SRCDIR)/devices/tap_user.c
override SRC_USE_JIT       := $(SRCDIR)/rvjit/rvjit.c $(SRCDIR)/rvjit/rvjit_emit.c
override SRC_USE_RV32      := $(SRCDIR)/cpu/riscv32_interpreter.c
override SRC_USE_RV64      := $(SRCDIR)/cpu/riscv64_interpreter.c
override SRC_USE_LIBRETRO  := $(SRCDIR)/bindings/libretro/libretro.c
override SRC_USE_VERTPASS  := $(SRCDIR)/virtpass/vp_cmdpost.c $(SRCDIR)/virtpass/vp_sensor.c $(SRCDIR)/virtpass/vp_session.c
override SRC_USE_JNI       := $(SRCDIR)/bindings/jni/rvvm_jni.c
# Win32 implementations of the POSIX API declared in include/mingw_compat
# (win_socket.c is the WinSock 2 backend behind the BSD socket shim)
override SRC_USE_WIN32_COMPAT := $(SRCDIR)/win/posix_shim.c $(SRCDIR)/win/win_socket.c

# Useflag dependencies
override RVJIT_SUPPORTS_ARCH := $(if $(filter i386 x86_64 arm% riscv% loongarch64,$(ARCH)),1)
override DEPS_USE_JIT        := RVJIT_SUPPORTS_ARCH

override DEPS_USE_X11       := USE_GUI
override DEPS_USE_SDL       := USE_GUI
override DEPS_USE_WAYLAND   := USE_GUI
override DEPS_USE_WIN32_GUI := USE_GUI
override DEPS_USE_HAIKU_GUI := USE_GUI
override DEPS_USE_COCOA_GUI := USE_GUI
override DEPS_USE_ALSA      := USE_SOUND
override DEPS_USE_GDBSTUB   := USE_NET
override DEPS_USE_JNI       := USE_LIB USE_NET
override DEPS_USE_LIBRETRO  := USE_LIB USE_NET

# Libraries
override LIBS_USE_SDL     := sdl$(filter-out 1,$(USE_SDL))
override LIBS_USE_X11     := x11 xext
override LIBS_USE_WAYLAND := wayland-client xkbcommon

#
# Additional headers
#

override CPPFLAGS := $(CPPFLAGS) -I$(SRCDIR)/util

# MinGW POSIX compatibility layer: include/mingw_compat provides the POSIX
# headers (sys/uio.h, pipe2/pread/pwrite, ...), src/win/posix_shim.c provides
# the Win32 implementations. These headers shadow the real system headers
# (some via #include_next, some by redefining types like struct iovec), so
# they must only be on the include path when the compat layer is enabled
# (USE_WIN32_COMPAT, on by default for windows targets) - on POSIX hosts the
# native headers must win.
ifneq (,$(call var_use,USE_WIN32_COMPAT))
override CPPFLAGS := $(CPPFLAGS) -I$(INCDIR)/mingw_compat
endif

#
# Prepare build targets
#

override BIN_TARGETS := rvvm
override LIB_TARGETS := rvvm # TODO: rvvm_libretro FTBFS

override bin_src_rvvm          := $(SRCDIR)/main.c
override bin_src_rvvm_user     := $(SRCDIR)/rvvm_user_main.c
override lib_src_rvvm_libretro := $(SRCDIR)/bindings/libretro/libretro.c

# Guest-side stubs under src/virtpass are RISC-V (ecall trampolines),
# they are cross-compiled by the guest builds - never build them for the host
override lib_src_virtpass_guest := $(SRCDIR)/virtpass/vp_ndk_stub.c $(SRCDIR)/virtpass/vp_gl_stub.c $(SRCDIR)/virtpass/vp_aaudio_stub.c

# Other non-host subtrees under src/virtpass: the guest programs and the
# Android JNI pass are cross-built for RISC-V / Android, win32-host provides
# its own binary, vp-sdk is cross-built by `make vp-sdk` - none of them may end
# up in librvvm
#
# vp_rootfs.c reads the bundle archives (gzip + tar) and needs zlib, so it is a
# host-side module too: it is built into rvvm_winhost (and the Android JNI pass)
# instead of librvvm, which stays free of the dependency. vp_shadow.c is the
# plain index the core queries and has no such dependency, so it does live in
# librvvm.
override lib_src_virtpass_nonhost := $(SRCDIR)/virtpass/guest-samples/% $(SRCDIR)/virtpass/android-host/% $(SRCDIR)/virtpass/win32-host/% $(SRCDIR)/virtpass/vp-sdk/% $(SRCDIR)/virtpass/vp_rootfs.c $(SRCDIR)/virtpass/vp_zip.c $(SRCDIR)/virtpass/vp_app.c $(SRCDIR)/virtpass/vp_bundle.c

# virtpass_stub bundles those guest-side stubs so guest programs can link them
# against the virtpass passthrough. It is only buildable on a native riscv64
# Linux target, where the guest ISA/ABI matches the host one
ifeq ($(OS),linux)
ifeq ($(ARCH),riscv64)
override LIB_TARGETS           := $(LIB_TARGETS) virtpass_stub
override lib_src_virtpass_stub := $(lib_src_virtpass_guest)
endif
endif

#
# VirtPass SDK: guest libraries whose APIs report instead of talking to the host
#
# src/virtpass/vp-sdk/*.c are generated from the real guest stubs by
# tools/gen_stub_notimpl.py and checked in (see src/virtpass/vp-sdk/README.md).
# They keep the NDK / EGL / AAudio signatures but return early, printing the API
# name on stderr - a guest linked against vpsdk runs on a host with no backend
# at all, and the log lists exactly which host APIs the guest asks for.
#
# Like every other src/virtpass stub these are riscv64 guest objects, so they
# are cross-compiled with zig (the same toolchain which builds the Android guest
# assets below) and never with the host compiler. Both the static and the shared
# library are produced from one -fPIC object set: the objects stay inside
# $(VP_SDK_DIR) under $(BUILDDIR), while the finished artifacts are written
# straight out to $(VP_SDK_OUT) - the repo root's lib/ - so a guest build only
# ever points -L at that one directory.
#
#   make vp-sdk       -> lib/libvpsdk.a + lib/libvpsdk.so
#   make vp-sdk-gen   -> regenerate those .c files from the real guest stubs
#

override VP_SDK_DIR   := $(BUILDDIR)/vp-sdk
# Where the linkable artifacts land, relative to the repository root (the CWD of
# the build): lib/, i.e. TOP/lib. Object files stay in the build tree, and
# `make clean` (see vp-sdk-clean below) removes these along with everything else.
VP_SDK_OUT            ?= lib
override VP_SDK_SRC   := $(filter %.c,$(call ls_dir,$(SRCDIR)/virtpass/vp-sdk))
override VP_SDK_OBJ   := $(addprefix $(VP_SDK_DIR)/,$(addsuffix .o,$(notdir $(basename $(VP_SDK_SRC)))))
override VP_SDK_ZIG   := zig cc
override VP_SDK_AR    := zig ar
# -fPIC so the very same objects serve both the archive and the .so;
# -fno-sanitize=undefined is a zig cc requirement
VP_SDK_CFLAGS ?= -target riscv64-linux-musl -O2 -g -fPIC -I$(INCDIR) -fno-sanitize=undefined
# Whole guest ABI header directory, so an edit to any vp_*.h rebuilds the SDK
override VP_SDK_HEADS := $(filter %.h,$(call ls_dir,$(INCDIR)/virtpass))
# Both artifacts carry the lib prefix, so `-L$(VP_SDK_OUT) -lvpsdk` resolves them
override VP_SDK_A     := $(VP_SDK_OUT)/libvpsdk.a
override VP_SDK_SO    := $(VP_SDK_OUT)/libvpsdk.so
# A DT_SONAME is not cosmetic here. lld (what zig cc links with) records the
# *path it resolved* a soname-less -l input to, so a guest doing
# `-L<sysroot>/lib -lvpsdk` ends up with
#   DT_NEEDED /home/you/riscv64-sysroot/lib/libvpsdk.so
# baked into its own shared objects - a path that only exists on the build
# machine, so the library fails to load and every symbol it provides stays
# unresolved. Naming the soname after the file keeps DT_NEEDED a bare
# "libvpsdk.so", which any -L dir, sysroot or guest /lib can satisfy.
override VP_SDK_SONAME := -Wl$(COMMA)-soname$(COMMA)$(notdir $(VP_SDK_SO))

# Cross-compile flags are data, not a file, so they are not prerequisites of the
# objects they affect. Keep them in a stamp and depend on it: it is rewritten
# (and thus made newer than every object) only when the flags change, so an
# unchanged build stays up to date.
override VP_SDK_STAMP := $(VP_SDK_DIR)/sdk_flags.stamp
ifneq ($(if $(wildcard $(VP_SDK_STAMP)),$(file <$(VP_SDK_STAMP)),),$(VP_SDK_CFLAGS))
$(call create_dirs,$(VP_SDK_DIR))
$(file >$(VP_SDK_STAMP),$(VP_SDK_CFLAGS))
endif

$(VP_SDK_DIR)/%.o: $(SRCDIR)/virtpass/vp-sdk/%.c $(VP_SDK_HEADS) $(VP_SDK_STAMP)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(VP_SDK_ZIG) $(VP_SDK_CFLAGS) -c -o $@ $<)

$(VP_SDK_A): $(VP_SDK_OBJ)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(VP_SDK_AR) rcs $@ $(VP_SDK_OBJ))

$(VP_SDK_SO): $(VP_SDK_OBJ)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)LD$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(VP_SDK_ZIG) $(VP_SDK_CFLAGS) -shared $(VP_SDK_SONAME) -o $@ $(VP_SDK_OBJ))

# Regenerate the sources from the real guest stubs. Kept out of the artifact
# rules on purpose: it rewrites files inside the source tree, and it would make
# every build non-incremental. Sources are listed explicitly because the shell
# on Windows does not expand wildcards.
VP_SDK_GEN_SRC ?= $(SRCDIR)/virtpass/vp_ndk_stub.c $(SRCDIR)/virtpass/vp_aaudio_stub.c $(SRCDIR)/virtpass/vp_gl_stub.c
VP_SDK_GEN     ?= python tools/gen_stub_notimpl.py
VP_SDK_GEN_OPTS ?= --outdir $(SRCDIR)/virtpass/vp-sdk

.PHONY: vp-sdk       # Build the VirtPass "not implemented" guest SDK into $(VP_SDK_OUT)/ (TOP/lib)
vp-sdk: $(VP_SDK_A) $(VP_SDK_SO)

.PHONY: vp-sdk-gen   # Regenerate src/virtpass/vp-sdk/*.c from the real guest stubs
vp-sdk-gen:
	$(call log_info,Regenerating the VirtPass SDK stubs)
	@$(call shell_esc,$(VP_SDK_GEN) $(VP_SDK_GEN_SRC) $(VP_SDK_GEN_OPTS))

# The artifacts live outside $(BUILDDIR), so `make clean` would leave them behind
# and a later `make vp-sdk` would then consider them up to date. Hook them into
# clean; the trailing rmdir only ever succeeds while $(VP_SDK_OUT) is empty.
.PHONY: vp-sdk-clean # Remove the $(VP_SDK_OUT)/ artifacts built by vp-sdk
vp-sdk-clean:
	$(call log_info,Removing the VirtPass SDK artifacts)
	$(call shell_ex,$(if $(HOST_POSIX),rm -f $(call path_shell,$(VP_SDK_A) $(VP_SDK_SO)),del $(call path_shell,$(subst /,\,$(VP_SDK_A) $(VP_SDK_SO))) 2>&1))
	$(call shell_ex,$(if $(HOST_POSIX),rmdir $(call path_shell,$(VP_SDK_OUT)),rd $(call path_shell,$(VP_SDK_OUT))) 2>&1)

clean: vp-sdk-clean

# The userland emulator assumes a 64-bit host address space,
# build it solely on non-i386 targets
ifneq (,$(filter linux windows mingw mingw32 msys cygwin,$(OS)))
ifeq (,$(filter i386,$(ARCH)))
override BIN_TARGETS        := $(BIN_TARGETS) rvvm_user
override CPPFLAGS           := $(CPPFLAGS) -DRVVM_USER_TEST
override bin_libs_rvvm_user := rvvm
# libvterm: a headless terminal state machine used to parse guest tty output
# (fd 1/2) so that CR / ANSI escape sequences are handled correctly. It is
# built on the fly from source into a static archive and linked here. The
# pre-generated *.inc files are already present, so no perl step is needed.
#
# NOTE: BUILDDIR is not yet defined when project.mk is parsed (it is set later in
# Makefile), so we place the archive at a fixed path inside the source tree and
# reference it by a path that is known at parse time (relative to $(CURDIR)).
LIBVTERM_DIR  := libvterm-0.3.3
LIBVTERM_A    := $(LIBVTERM_DIR)/libvterm.a
$(LIBVTERM_A):
	$(MAKE) -f $(LIBVTERM_DIR)/Makefile.cmake \
		CC="$(CC)" AR="$(AR)" LIBVTERM_A="$@"
# rvvm_user.c is compiled into the shared librvvm, so every binary that links it
# needs the libvterm archive. Make all binaries depend on it, and wire it into
# the link via a path relative to the repo root (where linking runs).
$(BIN_TARGETS): $(LIBVTERM_A)
override LDFLAGS            := $(LDFLAGS) -L$(LIBVTERM_DIR) -lvterm
override CPPFLAGS           := $(CPPFLAGS) -I$(CURDIR)/$(LIBVTERM_DIR)/include
endif
endif

# Win32 virtpass host: a windowed host which boots a Linux guest ELF through
# core/rvvm_user.c and services the virtpass hypercalls with GDI + OpenGL.
# Only a handful of sources, so it is built straight from here - no nested CMake
ifneq (,$(filter windows mingw mingw32 msys cygwin,$(OS)))
ifeq (,$(filter i386,$(ARCH)))
# vp_cmdpost.c bridges the guest syscalls to the host window, so it is required
USE_VERTPASS                   ?= 1
override BIN_TARGETS           := $(BIN_TARGETS) rvvm_winhost rvvm_ash
override bin_src_rvvm_winhost  := $(SRCDIR)/virtpass/win32-host/win32_main.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_cmdpost_bridge.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_gl_backend.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_gl_dispatch.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_aaudio_wasapi.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_sensor_stub.c \
                                  $(SRCDIR)/virtpass/vp_rootfs.c \
                                  $(SRCDIR)/virtpass/vp_zip.c \
                                  $(SRCDIR)/virtpass/vp_app.c \
                                  $(SRCDIR)/virtpass/vp_bundle.c
# zlib: vp_rootfs.c inflates the bundle archives (pkg-config module name is
# "zlib", not "z")
override bin_libs_rvvm_winhost := rvvm zlib

# rvvm_ash: the same host without a window. It boots the bundle's shell on the
# console it was started from (bash.exe to the WinHost's wsl.exe), so it shares
# every source but the entry point - the bridge keeps its window, GL, audio and
# sensor code behind win32_host_init(), which this one never calls.
override bin_src_rvvm_ash     := $(SRCDIR)/virtpass/win32-host/ash_main.c \
                                 $(SRCDIR)/virtpass/win32-host/ash_client.c \
                                 $(filter-out $(SRCDIR)/virtpass/win32-host/win32_main.c,$(bin_src_rvvm_winhost))
override bin_libs_rvvm_ash    := rvvm zlib

# vp: a client for the *Android* host's guest console, shaped like adb. It is
# the counterpart of rvvm_ash - where that one is a console app for this
# host's own guest, this is the thing that drives the other one over USB, and
# it borrows adb's verbs so that anyone used to adb is already right about how
# to spell it.
#
# It links nothing of RVVM: it is a socket client, and everything it knows
# about the other end is five bytes of header and six packet ids. It compiles
# the WinSock backend in directly rather than linking `rvvm` for it - that
# library is the emulator, and a tool that never runs a guest should not drag
# one in behind it.
override BIN_TARGETS          := $(BIN_TARGETS) vp
override bin_src_vp            := $(SRCDIR)/virtpass/win32-host/vp_client.c
override bin_libs_vp           :=
endif
endif

override lib_src_rvvm          := $(filter-out $(bin_src_rvvm) $(bin_src_rvvm_user) $(bin_src_rvvm_winhost) $(lib_src_rvvm_libretro) $(lib_src_virtpass_guest) $(lib_src_virtpass_nonhost),$(call recursive_match,$(SRCDIR),*.c *.cpp *.cc *.cxx))

override bin_libs_rvvm := rvvm
override lib_libs_rvvm := $(if $(call var_use,USE_LIBS_PROBE),,$(LIBS_USE_SDL) $(LIBS_USE_X11) $(LIBS_USE_WAYLAND))

#
# Tests
#

override RVVM := $(call bin_target,rvvm)

override TEST_DATA_TAR_LINK := https://github.com/LekKit/riscv-tests/releases/download/rvvm-tests/riscv-tests.tar.gz
override TEST_DATA_TAR_FILE := $(lastword $(subst /,$(SPACE),$(TEST_DATA_TAR_LINK)))

override test_result = $(call println,$(TEXT)[$(if $(filter 0,$1),$(GREEN)PASS,$(RED)FAIL: $1)$(TEXT)] $2)$(if $(filter 0,$1),,fail)
override invoke_rvvm = $(call test_result,$(lastword $(call shell_ex,$(RVVM) $1 -nonet -nogui -nosound $(NULL_STDERR))),$(firstword $1))
override filter_test = $(filter-out $(foreach isa,$2,$(BUILDDIR)/riscv-tests/$(isa)%),$(call recursive_wildcard,$(BUILDDIR)/riscv-tests/$1*))

test:
	$(if $(call paths_exist,$(BUILDDIR)/riscv-tests),,$(call shell_ex,cd $(BUILDDIR) && curl -LO $(TEST_DATA_TAR_LINK) && tar xzf $(TEST_DATA_TAR_FILE)))
ifneq (,$(call var_use,USE_RV32))
	$(call println,)
	$(call log_info,Running RISC-V Tests (riscv32))
	$(call println,)
	@$(if $(strip $(foreach test,$(call filter_test,rv32,$(if $(call var_use,USE_FPU),,rv32uf rv32ud rv32uzfh)),$(call invoke_rvvm,$(test) -rv32))),exit 1)
endif
ifneq (,$(call var_use,USE_RV64))
	$(call println,)
	$(call log_info,Running RISC-V Tests (riscv64))
	$(call println,)
	@$(if $(strip $(foreach test,$(call filter_test,rv64,$(if $(call var_use,USE_FPU),,rv64uf rv64ud rv64uzfh)),$(call invoke_rvvm,$(test) -rv64))),exit 1)
endif
	@:

#
# Android host application (Gradle + NDK CMake)
#
# The Android app is a Gradle project: Gradle drives the NDK CMake pass that
# produces librvvm_jni.so (app/src/main/cpp/CMakeLists.txt) and then packages
# it together with the Java UI into an APK. It cannot be expressed with this
# Makefile's compile/link rules, so it is cascaded through the Gradle wrapper.
#

override ANDROID_HOST := $(CURDIR)/src/virtpass/android-host

ANDROID_VARIANT     ?= debug
ANDROID_GRADLE_OPTS ?=

# HOST_POSIX is set for POSIX-like shells (incl. MSYS), empty for stock Windows CMD
override ANDROID_GRADLE := $(if $(HOST_POSIX),./gradlew,gradlew.bat)

# Gradle capitalizes build type names (debug -> Debug)
override android_build_type := $(call capitalize,$(ANDROID_VARIANT))

#
# Guest programs (shared by both hosts)
#
# These ELFs execute inside the emulated RISC-V machine, not on the host running
# it, so they are cross-compiled for riscv64-linux-musl and linked against the
# virtpass guest stubs. musl is used in place of the NDK's bionic because scudo
# reserves address space in a way the Win32 mmap shim does not support yet.
#
# They land in the build tree (GUEST_ASSETS_DIR), not in any one host's tree: the
# APK boots apps out of bundle/apps.tar.gz - which is packed from here - and ships
# no loose ELF at all, while the WinHost looks for that same directory next to its
# own binary. Neither host has to know where the other keeps its files.
#

# Samples to build: every guest program found in guest-samples/, so dropping a new
# <name>.c there is enough to get it built and packed into the apps archive.
override guest_samples := $(notdir $(basename $(filter %.c,$(call ls_dir,$(SRCDIR)/virtpass/guest-samples))))
GUEST_SAMPLES ?= $(guest_samples)

# The APK's assets/ directory carries exactly two things: the tree an app's own
# resources are packed from (fonts/) and the staged bundle/ directory. Not the
# guest ELFs: they are build output, and build output does not belong in a source
# tree that is also the APK's package.
override ANDROID_ASSETS_DIR := $(ANDROID_HOST)/app/src/main/assets
override GUEST_ASSETS_DIR   := $(BUILDDIR)/guest-assets
override GUEST_OBJ_DIR      := $(BUILDDIR)/guest-obj
override GUEST_ZIG          := zig cc
override GUEST_AR           := zig ar
# Debug build of the guests: -O0 -g keeps line tables for the in-host
# userland debugger; -fno-sanitize=undefined is a zig cc requirement
override GUEST_CFLAGS := -target riscv64-linux-musl -O0 -g -I$(INCDIR) -fno-sanitize=undefined
# Whole guest ABI header directory: listing the headers by hand silently missed
# vp_aaudio.h / vp_sensor_abi.h, so edits to those never rebuilt a guest.
override GUEST_HEADS := $(filter %.h,$(call ls_dir,$(INCDIR)/virtpass))
override GUEST_LIBS  := $(GUEST_OBJ_DIR)/libandroid_stubs.a $(GUEST_OBJ_DIR)/libgles_stubs.a
override guest_assets := $(addprefix $(GUEST_ASSETS_DIR)/,$(addsuffix .exe,$(GUEST_SAMPLES)))

# Guest ELFs left behind by samples that no longer exist. Both launchers can
# enumerate a directory of them, and a stale file stays listed forever and then
# fails to launch. Computed before the build so it only ever names orphans.
override guest_assets_stale := $(filter-out $(guest_assets),$(filter %.exe,$(call ls_dir,$(GUEST_ASSETS_DIR))))

# Cross-compile flags are data, not a file, so they are not prerequisites of
# the objects they affect. Keep them in a stamp and depend on it: the stamp is
# rewritten (and thus made newer than every guest) only when the flags change,
# so an unchanged build stays up to date.
override GUEST_STAMP := $(GUEST_OBJ_DIR)/guest_flags.stamp
ifneq ($(if $(wildcard $(GUEST_STAMP)),$(file <$(GUEST_STAMP)),),$(GUEST_CFLAGS))
$(call create_dirs,$(GUEST_OBJ_DIR))
$(file >$(GUEST_STAMP),$(GUEST_CFLAGS))
endif

# Guest-side syscall stubs, shared by every sample
$(GUEST_OBJ_DIR)/vp_ndk_stub.o: $(SRCDIR)/virtpass/vp_ndk_stub.c $(GUEST_HEADS) $(GUEST_STAMP)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_ZIG) $(GUEST_CFLAGS) -c -o $@ $<)

$(GUEST_OBJ_DIR)/vp_gl_stub.o: $(SRCDIR)/virtpass/vp_gl_stub.c $(GUEST_HEADS) $(GUEST_STAMP)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_ZIG) $(GUEST_CFLAGS) -c -o $@ $<)

$(GUEST_OBJ_DIR)/vp_aaudio_stub.o: $(SRCDIR)/virtpass/vp_aaudio_stub.c $(GUEST_HEADS) $(GUEST_STAMP)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_ZIG) $(GUEST_CFLAGS) -c -o $@ $<)

$(GUEST_OBJ_DIR)/libandroid_stubs.a: $(GUEST_OBJ_DIR)/vp_ndk_stub.o $(GUEST_OBJ_DIR)/vp_aaudio_stub.o
	$(call println,$(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_AR) rcs $@ $(GUEST_OBJ_DIR)/vp_ndk_stub.o $(GUEST_OBJ_DIR)/vp_aaudio_stub.o)

$(GUEST_OBJ_DIR)/libgles_stubs.a: $(GUEST_OBJ_DIR)/vp_gl_stub.o
	$(call println,$(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_AR) rcs $@ $<)

# Each sample is linked into the build tree's guest assets
$(GUEST_ASSETS_DIR)/%.exe: $(SRCDIR)/virtpass/guest-samples/%.c $(GUEST_LIBS) $(GUEST_HEADS) $(GUEST_STAMP)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)LD$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(GUEST_ZIG) $(GUEST_CFLAGS) -static -L$(GUEST_OBJ_DIR) $< -landroid_stubs -lgles_stubs -o $@)

.PHONY: guest-assets # Cross-compile the guest samples into the build tree
guest-assets: $(guest_assets)
	$(if $(guest_assets_stale),$(call log_info,Removing stale guests: $(notdir $(guest_assets_stale))))
	$(if $(guest_assets_stale),$(if $(HOST_POSIX),$(call shell_ex,rm -f $(call path_shell,$(guest_assets_stale))),$(call shell_ex,del /F /Q $(call path_shell,$(subst /,\,$(guest_assets_stale))))))

.PHONY: android-assets # Old name of guest-assets, kept for existing scripts
android-assets: guest-assets

# Stacktraces: libbacktrace is loaded by name at runtime (src/util/stacktrace.c)
# rather than linked, so a build without it still runs - it just cannot say where
# it went when it faults. That only holds if the DLL is somewhere LoadLibrary
# looks, and the exe's own directory is the one place that needs nothing set up
# on the caller's side. A mingw-w64 toolchain installs it as libbacktrace-0.dll,
# which dlib.c's probe now also looks for.
#
# Skipped with a note when it is not there. An optional debug aid is never a
# reason for a build to fail.
#
# Override TOOLCHAIN_BIN if your toolchain lives elsewhere:
#   make bin TOOLCHAIN_BIN=/path/to/mingw64/bin
override TOOLCHAIN_BIN ?= $(firstword $(wildcard C:/msys64/mingw64/bin) $(wildcard /mingw64/bin) /usr/bin)

.PHONY: debug-deps       # Stage the optional libraries the host loads by name
debug-deps:
	$(if $(wildcard $(TOOLCHAIN_BIN)/libbacktrace-0.dll),\
	  $(call install_file,$(TOOLCHAIN_BIN)/libbacktrace-0.dll,$(BUILDDIR)/libbacktrace-0.dll,0644),\
	  $(call log_info,libbacktrace-0.dll not under $(TOOLCHAIN_BIN) - stacktraces stay unavailable))

# The APK ships the same two archives a release bundle carries. The host unpacks
# them into the app's own storage on first use and materializes the guest's
# rootfs from there, exactly as the WinHost does it - the code is the same
# (vp_bundle.c). Staged out of the release bundle/ directory, so an APK always
# carries what the host binary of that release boots from.
override ANDROID_BUNDLE_DIR := $(ANDROID_ASSETS_DIR)/bundle

.PHONY: android-bundle # Stage the release bundle (rootfs + system + apps) into the APK assets
android-bundle: pack-apps pack-system fetch-rootfs
	$(call println,$(TEXT)[$(GREEN)STAGE$(TEXT)] $(ANDROID_BUNDLE_DIR) $(RESET))
	$(call install_file,$(ROOTFS_TAR),$(ANDROID_BUNDLE_DIR)/$(notdir $(ROOTFS_TAR)),0644)
	$(call install_file,$(SYSTEM_TAR),$(ANDROID_BUNDLE_DIR)/$(notdir $(SYSTEM_TAR)),0644)
	$(call install_file,$(APPS_TAR),$(ANDROID_BUNDLE_DIR)/$(notdir $(APPS_TAR)),0644)

.PHONY: android       # Build the Android APK (Java + librvvm_jni.so + the bundle)
android: guest-assets android-bundle
	$(call log_info,Building Android $(ANDROID_VARIANT) APK)
	$(call shell_esc,cd $(ANDROID_HOST) && $(ANDROID_GRADLE) $(ANDROID_GRADLE_OPTS) :app:assemble$(android_build_type))

.PHONY: android-jni   # Build only librvvm_jni.so (native pass of the Gradle build)
android-jni:
	$(call log_info,Building Android $(ANDROID_VARIANT) JNI library)
	$(call shell_esc,cd $(ANDROID_HOST) && $(ANDROID_GRADLE) $(ANDROID_GRADLE_OPTS) :app:externalNativeBuild$(android_build_type))

.PHONY: android-clean # Clean the Android build outputs
android-clean:
	$(call log_info,Cleaning Android builds)
	$(call shell_esc,cd $(ANDROID_HOST) && $(ANDROID_GRADLE) $(ANDROID_GRADLE_OPTS) clean)

#
# Bundle: the three archives a released host reads at runtime
#
# They sit next to the host binary (win32: the exe's own directory, resolved
# with GetModuleFileNameA because the guest chdir()s away; Android: the APK's
# assets/), under one directory, and none is ever visible to the guest:
#
#   <bundle>/rootfs.tar.gz   the guest's `/` (Alpine minirootfs)
#   <bundle>/system.tar.gz   system programs, laid out at their guest paths
#   <bundle>/apps.tar.gz     the pre-deployed apps: apps/<id>.vapp members
#
# All are gitignored (the repo ignores the whole root), so a release - and any
# end-to-end run - has to fetch or pack them first. These variables mirror the C
# literals in src/virtpass/vp_rootfs.h; change them together.
#
override VP_BUNDLE_DIR    := bundle
override VP_ROOTFS_TAR_GZ := rootfs.tar.gz
override VP_SYSTEM_TAR_GZ := system.tar.gz
override VP_APPS_TAR_GZ   := apps.tar.gz

# Recursive on purpose: BUILDDIR is not defined yet while project.mk is parsed.
override BUNDLE_DIR = $(BUILDDIR)/$(VP_BUNDLE_DIR)
override ROOTFS_TAR = $(BUNDLE_DIR)/$(VP_ROOTFS_TAR_GZ)
override SYSTEM_TAR = $(BUNDLE_DIR)/$(VP_SYSTEM_TAR_GZ)
override APPS_TAR   = $(BUNDLE_DIR)/$(VP_APPS_TAR_GZ)

ALPINE_MIRROR  ?= https://dl-cdn.alpinelinux.org/alpine
ALPINE_VERSION ?= 3.24.2
ALPINE_ARCH    ?= riscv64
override ALPINE_BRANCH := $(word 1,$(subst ., ,$(ALPINE_VERSION))).$(word 2,$(subst ., ,$(ALPINE_VERSION)))
override ROOTFS_URL := $(ALPINE_MIRROR)/v$(ALPINE_BRANCH)/releases/$(ALPINE_ARCH)/alpine-minirootfs-$(ALPINE_VERSION)-$(ALPINE_ARCH).tar.gz

.PHONY: fetch-rootfs # Download the Alpine minirootfs archive into the bundle
fetch-rootfs:
	$(call create_dirs,$(BUNDLE_DIR))
	$(if $(call paths_exist,$(ROOTFS_TAR)),\
		$(call log_info,Rootfs archive already present: $(ROOTFS_TAR)),\
		$(call log_info,Fetching $(ROOTFS_URL))$(call shell_ex,curl -fL -o $(call path_shell,$(ROOTFS_TAR)) $(ROOTFS_URL)))
	@:

#
# The system layer and the apps archive
#
# A system program is not an app: pack_system.py declares the list (source name
# -> guest path) and lays each one out at its guest path in system.tar.gz
# (sbin/vpsessiond), so the host extracts the archive as a layer and the guest
# names it like any other program - no manifest, no per-id directory. pack-apps
# reads that same archive back (--exclude-from), so a system program is never
# also packed as an app: the archive is the single source of truth.
#
# An app is packed as apps/<id>/{app.json,bin/<id>.exe,assets/...} and installed
# under <guest>/data/app/<id>. Nothing in an app is host-specific, so this one
# archive serves both hosts: the ELFs come from the build tree, the shared
# resources from the source tree they live in.
#
override SYSTEM_PACKER    ?= python $(CURDIR)/tools/pack_system.py

override APPS_PACKER     ?= python $(CURDIR)/tools/pack_apps.py
override APPS_SRC_DIR    ?= $(GUEST_ASSETS_DIR)
override APPS_ASSET_ROOT ?= $(ANDROID_ASSETS_DIR)
override APPS_ASSET_DIRS ?= fonts
override APPS_ONLY       ?=

.PHONY: pack-system # Pack the system programs into the bundle's system.tar.gz
pack-system: guest-assets
	$(call create_dirs,$(BUNDLE_DIR))
	$(call println,$(TEXT)[$(GREEN)PACK$(TEXT)] $(SYSTEM_TAR) $(RESET))
	@$(call shell_esc,$(SYSTEM_PACKER) --src $(call path_shell,$(GUEST_ASSETS_DIR))\
		--out $(call path_shell,$(SYSTEM_TAR)))

.PHONY: pack-apps # Pack the guest programs into the bundle's apps.tar.gz
pack-apps: guest-assets pack-system
	$(call create_dirs,$(BUNDLE_DIR))
	$(call println,$(TEXT)[$(GREEN)PACK$(TEXT)] $(APPS_TAR) $(RESET))
	@$(call shell_esc,$(APPS_PACKER) --src $(call path_shell,$(APPS_SRC_DIR))\
		$(foreach name,$(APPS_ASSET_DIRS),$(if $(wildcard $(APPS_ASSET_ROOT)/$(name)),--asset $(name)=$(call path_shell,$(APPS_ASSET_ROOT)/$(name))))\
		$(if $(APPS_ONLY),--only $(APPS_ONLY))\
		--exclude-from $(call path_shell,$(SYSTEM_TAR))\
		--out $(call path_shell,$(APPS_TAR)))

#
# dist: the release layout, which is also the layout the host reads at runtime
#
#   $(DIST_DIR)/
#     rvvm_winhost_<arch>.exe        (and every other host binary this build made)
#     bundle/{rootfs.tar.gz,system.tar.gz,apps.tar.gz}
#
# The host resolves bundle/ next to its own binary (GetModuleFileNameA, never the
# cwd: the guest chdir()s away the moment it starts), so this is not just a
# packaging convention - it is what the binary looks for. The guest ELFs are
# deliberately absent: apps.tar.gz and system.tar.gz are the only things a host
# installs them from.
#
override DIST_DIR ?= $(BUILDDIR)/dist

.PHONY: dist # Assemble a release: the host binaries and the bundle they boot from
dist: bin pack-apps pack-system fetch-rootfs
	$(call create_dirs,$(DIST_DIR)/$(VP_BUNDLE_DIR))
	$(foreach bin,$(BIN_TARGETS),$(call install_file,$(bin),$(DIST_DIR)/$(notdir $(bin)),0755))
	$(call install_file,$(ROOTFS_TAR),$(DIST_DIR)/$(VP_BUNDLE_DIR)/$(notdir $(ROOTFS_TAR)),0644)
	$(call install_file,$(SYSTEM_TAR),$(DIST_DIR)/$(VP_BUNDLE_DIR)/$(notdir $(SYSTEM_TAR)),0644)
	$(call install_file,$(APPS_TAR),$(DIST_DIR)/$(VP_BUNDLE_DIR)/$(notdir $(APPS_TAR)),0644)
	$(call println,$(TEXT)[$(GREEN)DIST$(TEXT)] $(DIST_DIR) $(RESET))
