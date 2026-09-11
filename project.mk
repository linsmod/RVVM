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
override SRC_USE_VERTPASS  := $(SRCDIR)/virtpass/vp_cmdpost.c
override SRC_USE_JNI       := $(SRCDIR)/bindings/jni/rvvm_jni.c
# Win32 implementations of the POSIX API declared in include/mingw_compat
override SRC_USE_WIN32_COMPAT := $(SRCDIR)/win/posix_shim.c

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
# its own binary - none of them may end up in librvvm
override lib_src_virtpass_nonhost := $(SRCDIR)/virtpass/guest-samples/% $(SRCDIR)/virtpass/android-host/% $(SRCDIR)/virtpass/win32-host/%

# virtpass_stub bundles those guest-side stubs so guest programs can link them
# against the virtpass passthrough. It is only buildable on a native riscv64
# Linux target, where the guest ISA/ABI matches the host one
ifeq ($(OS),linux)
ifeq ($(ARCH),riscv64)
override LIB_TARGETS           := $(LIB_TARGETS) virtpass_stub
override lib_src_virtpass_stub := $(lib_src_virtpass_guest)
endif
endif

# The userland emulator assumes a 64-bit host address space,
# build it solely on non-i386 targets
ifneq (,$(filter linux windows mingw mingw32 msys cygwin,$(OS)))
ifeq (,$(filter i386,$(ARCH)))
override BIN_TARGETS        := $(BIN_TARGETS) rvvm_user
override CPPFLAGS           := $(CPPFLAGS) -DRVVM_USER_TEST
override bin_libs_rvvm_user := rvvm
endif
endif

# Win32 virtpass host: a windowed host which boots a Linux guest ELF through
# core/rvvm_user.c and services the virtpass hypercalls with GDI + OpenGL.
# Only a handful of sources, so it is built straight from here - no nested CMake
ifneq (,$(filter windows mingw mingw32 msys cygwin,$(OS)))
ifeq (,$(filter i386,$(ARCH)))
# vp_cmdpost.c bridges the guest syscalls to the host window, so it is required
USE_VERTPASS                   ?= 1
override BIN_TARGETS           := $(BIN_TARGETS) rvvm_winhost
override bin_src_rvvm_winhost  := $(SRCDIR)/virtpass/win32-host/win32_main.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_cmdpost_bridge.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_gl_backend.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_gl_dispatch.c \
                                  $(SRCDIR)/virtpass/win32-host/win32_aaudio_wasapi.c
override bin_libs_rvvm_winhost := rvvm
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
# Guest programs bundled into the APK assets
#
# These ELFs execute inside the emulated RISC-V machine, not on the Android
# host, so they are cross-compiled for riscv64-linux-musl and linked against the
# virtpass guest stubs. musl is used in place of the NDK's bionic because scudo
# reserves address space in a way the Win32 mmap shim does not support yet.
#

# Samples to bundle: every guest program found in guest-samples/, so dropping a
# new <name>.c there is enough to get it built and copied into the assets tree
override android_guest_samples := $(notdir $(basename $(filter %.c,$(call ls_dir,$(SRCDIR)/virtpass/guest-samples))))
ANDROID_GUEST_SAMPLES ?= $(android_guest_samples)

override ANDROID_ASSETS_DIR  := $(ANDROID_HOST)/app/src/main/assets
override ANDROID_GUEST_DIR   := $(BUILDDIR)/android-guest
override ANDROID_GUEST_ZIG   := zig cc
override ANDROID_GUEST_AR    := zig ar
# Debug build of the guests: -O0 -g keeps line tables for the in-host
# userland debugger; -fno-sanitize=undefined is a zig cc requirement
override ANDROID_GUEST_FLAGS := -target riscv64-linux-musl -O0 -g -I$(INCDIR) -fno-sanitize=undefined
override ANDROID_GUEST_HEADS := $(INCDIR)/virtpass/vp_android.h $(INCDIR)/virtpass/vp_gl.h $(INCDIR)/virtpass/vp_audio_ringbuf.h
override ANDROID_GUEST_LIBS  := $(ANDROID_GUEST_DIR)/libandroid_stubs.a $(ANDROID_GUEST_DIR)/libgles_stubs.a
override android_guest_assets := $(addprefix $(ANDROID_ASSETS_DIR)/,$(addsuffix .exe,$(ANDROID_GUEST_SAMPLES)))

# Guest-side syscall stubs, shared by every sample
$(ANDROID_GUEST_DIR)/vp_ndk_stub.o: $(SRCDIR)/virtpass/vp_ndk_stub.c $(ANDROID_GUEST_HEADS)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_ZIG) $(ANDROID_GUEST_FLAGS) -c -o $@ $<)

$(ANDROID_GUEST_DIR)/vp_gl_stub.o: $(SRCDIR)/virtpass/vp_gl_stub.c $(ANDROID_GUEST_HEADS)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_ZIG) $(ANDROID_GUEST_FLAGS) -c -o $@ $<)

$(ANDROID_GUEST_DIR)/vp_aaudio_stub.o: $(SRCDIR)/virtpass/vp_aaudio_stub.c $(ANDROID_GUEST_HEADS)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)CC$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_ZIG) $(ANDROID_GUEST_FLAGS) -c -o $@ $<)

$(ANDROID_GUEST_DIR)/libandroid_stubs.a: $(ANDROID_GUEST_DIR)/vp_ndk_stub.o $(ANDROID_GUEST_DIR)/vp_aaudio_stub.o
	$(call println,$(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_AR) rcs $@ $(ANDROID_GUEST_DIR)/vp_ndk_stub.o $(ANDROID_GUEST_DIR)/vp_aaudio_stub.o)

$(ANDROID_GUEST_DIR)/libgles_stubs.a: $(ANDROID_GUEST_DIR)/vp_gl_stub.o
	$(call println,$(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_AR) rcs $@ $<)

# Each sample is linked straight into the APK assets tree
$(ANDROID_ASSETS_DIR)/%.exe: $(SRCDIR)/virtpass/guest-samples/%.c $(ANDROID_GUEST_LIBS) $(ANDROID_GUEST_HEADS)
	$(call create_dirs,$(dir $@))
	$(call println,$(TEXT)[$(GREEN)LD$(TEXT)] $@ $(RESET))
	@$(call shell_esc,$(ANDROID_GUEST_ZIG) $(ANDROID_GUEST_FLAGS) -static -L$(ANDROID_GUEST_DIR) $< -landroid_stubs -lgles_stubs -o $@)

.PHONY: android-assets # Cross-compile the guest samples into the APK assets
android-assets: $(android_guest_assets)

.PHONY: android       # Build the Android APK (Java + librvvm_jni.so + guest assets)
android: android-assets
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
