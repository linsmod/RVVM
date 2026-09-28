cmake_minimum_required(VERSION 3.22.1)
project("rvvm-android")

# ============================================================
# RVVM source root (relative to this CMakeLists.txt)
# ============================================================
set(RVVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../..")
set(RVVM_SRC  "${RVVM_ROOT}/src")
set(RVVM_INC  "${RVVM_ROOT}/include")

# ============================================================
# Global compile definitions
# ============================================================
#
# USE_JIT is decided per ABI rather than globally. The JIT emits *host* code, and
# its 32-bit ARM backend (src/rvjit/rvjit_arm.h) defines neither
# RVJIT_NATIVE_LINKER nor RVJIT_NATIVE_64BIT: no direct block-to-block linking,
# and 64-bit guest registers moved through helpers instead of natively. That is a
# configuration the JIT recognises, but it is not the one this app is built
# against - project.mk gates USE_JIT on the same host list (x86_64, arm64,
# riscv64, loongarch64) and leaves every other host on the interpreter. So a
# 32-bit ABI gets an interpreter-only build, which is the whole of what
# "supports compiling" needs to mean here.
option(RVVM_JIT_32BIT "Build the 32-bit JIT backend instead of the interpreter" OFF)
set(RVVM_JIT_ABIS arm64-v8a x86_64 riscv64)
set(RVVM_JIT_SRCS)
if(RVVM_JIT_32BIT OR CMAKE_ANDROID_ARCH_ABI IN_LIST RVVM_JIT_ABIS)
    add_definitions(-DUSE_JIT)
    set(RVVM_JIT_SRCS
        ${RVVM_SRC}/rvjit/rvjit.c
        ${RVVM_SRC}/rvjit/rvjit_emit.c
    )
endif()

add_definitions(
    -DRVVM_USER_TEST
    -DRVVM_USE_RV64
    -DUSE_RV64
    -DANDROID
    -D__linux__
    -D_GNU_SOURCE
    -DUSE_FPU
)

# Force ZERO_INIT to {0} for NDK clang compatibility
# Must override compiler.h's GNU_EXTS logic
add_definitions(-UGNU_EXTS)

# Declare missing NDK functions (memfd_create, statx) for all TUs
add_compile_options(-include "${CMAKE_CURRENT_SOURCE_DIR}/compat_fix.h")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RVVM_SRC}
    ${RVVM_SRC}/core
    ${RVVM_SRC}/cpu
    ${RVVM_SRC}/util
    ${RVVM_SRC}/virtpass
    ${RVVM_INC}
)

# ============================================================
# vp_cmdpost (single copy in src/virtpass)
# ============================================================
add_library(cmdpost STATIC
    ${RVVM_SRC}/virtpass/vp_cmdpost.c
)

# ============================================================
# RVVM core static library
# ============================================================
add_library(rvvm_core STATIC
    # NDK compat (syscalls missing below API 30)
    ${CMAKE_CURRENT_SOURCE_DIR}/ndk_compat.c

    # Core
    ${RVVM_SRC}/core/rvvm.c
    ${RVVM_SRC}/core/rvvm_user.c
    ${RVVM_SRC}/core/rvvm_irq.c
    ${RVVM_SRC}/core/rvvm_region.c
    ${RVVM_SRC}/core/rvvm_isolation.c
    
    # CPU
    ${RVVM_SRC}/cpu/riscv_cpu.c
    ${RVVM_SRC}/cpu/riscv_hart.c
    ${RVVM_SRC}/cpu/riscv_csr.c
    ${RVVM_SRC}/cpu/riscv_mmu.c
    ${RVVM_SRC}/cpu/riscv_priv.c
    ${RVVM_SRC}/cpu/riscv_fpu.c
    ${RVVM_SRC}/cpu/riscv64_interpreter.c

    # JIT (RVVM_JIT_SRCS: the host backend, or nothing on a 32-bit ABI)
    ${RVVM_JIT_SRCS}

    # Utils
    ${RVVM_SRC}/util/elf_load.c
    ${RVVM_SRC}/util/vma_ops.c
    ${RVVM_SRC}/util/blk_io.c
    ${RVVM_SRC}/util/utils.c
    ${RVVM_SRC}/util/threading.c
    ${RVVM_SRC}/util/atomics.c
    ${RVVM_SRC}/util/locking.c
    ${RVVM_SRC}/util/stacktrace.c
    ${RVVM_SRC}/util/rvtimer.c
    ${RVVM_SRC}/util/vector.c
    ${RVVM_SRC}/util/hashmap.c
    ${RVVM_SRC}/util/ringbuf.c
    ${RVVM_SRC}/util/rcu_lib.c
    ${RVVM_SRC}/util/fpu_lib.c
    ${RVVM_SRC}/util/dlib.c

    # VirtPass host side
    ${RVVM_SRC}/virtpass/vp_cmdpost.c
    ${RVVM_SRC}/virtpass/vp_sensor.c
    ${RVVM_SRC}/virtpass/vp_session.c

    # The guest rootfs "shape" index (plain data, no zlib)
    ${RVVM_SRC}/virtpass/vp_shadow.c
)

target_include_directories(rvvm_core PUBLIC
    ${RVVM_SRC}
    ${RVVM_SRC}/core
    ${RVVM_SRC}/cpu
    ${RVVM_SRC}/util
    ${RVVM_SRC}/virtpass
    ${RVVM_INC}
)

# Suppress warnings for third-party code
target_compile_options(rvvm_core PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-format
    -Wno-incompatible-pointer-types
)

target_link_libraries(rvvm_core PUBLIC
    android
    log
    vterm
)

# 16KB page size alignment for Android 15+ compatibility
target_link_options(rvvm_core PRIVATE -Wl,-z,max-page-size=16384)

# ============================================================
# JNI shared library (the final .so loaded by Java)
# ============================================================
add_library(rvvm_jni SHARED
    jni_bridge.c
    vp_aaudio_android.c
    vp_sensor_android.c
    android_gl_host.c
    # Bundle archive reader (gzip + tar): host side, hence linked against zlib
    # here rather than into rvvm_core. vp_bundle.c is the other half of it - the
    # mount and the /assets ops; vp_app.c / vp_zip.c are the app packages (a
    # `.vapp` is a zip nested in apps.tar.gz), shared with the WinHost.
    ${RVVM_SRC}/virtpass/vp_rootfs.c
    ${RVVM_SRC}/virtpass/vp_zip.c
    ${RVVM_SRC}/virtpass/vp_app.c
    ${RVVM_SRC}/virtpass/vp_bundle.c
)

target_include_directories(rvvm_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- libvterm (headless terminal parser, built from source) ----
# Compiled directly by CMake (no external make/ninja invocation) so it works
# under the NDK toolchain regardless of whether `make` is on PATH. The *.inc
# files are pre-generated, so no perl/codegen step is needed. rvvm_user.c lives
# in rvvm_core and consumes the libvterm API, so core is linked PUBLIC to expose
# both the headers and the symbols.
set(LIBVTERM_DIR "${RVVM_ROOT}/libvterm-0.3.3")
file(GLOB LIBVTERM_SRCS "${LIBVTERM_DIR}/src/*.c")
add_library(vterm STATIC ${LIBVTERM_SRCS})
set_target_properties(vterm PROPERTIES C_STANDARD 99)
target_include_directories(vterm PUBLIC "${LIBVTERM_DIR}/include" "${LIBVTERM_DIR}/src")
# Third-party C99 code: relax the host warning flags so it compiles cleanly.
target_compile_options(vterm PRIVATE
    -Wno-error -Wno-unused -Wno-uninitialized -Wno-unused-parameter)

target_link_libraries(rvvm_jni
    rvvm_core
    android
    log
    m
    aaudio
    vterm
    z
)

# 16KB page size alignment for Android 15+ compatibility
target_link_options(rvvm_jni PRIVATE -Wl,-z,max-page-size=16384)

# ============================================================
# Guest-side API stub (riscv64 ecall trampolines)
#
# vp_gl_stub.c / vp_ndk_stub.c are *guest* code: they use RISC-V inline asm
# and pass guest calls to the host. They must be built by a
# riscv64-linux-android*-clang toolchain, NOT by the host-side NDK toolchain
# (arm64-v8a / x86_64) that builds librvvm_jni.so - "ecall" is not an ARM
# instruction, so enabling this here breaks the regular app build.
# Enable it only for a guest-toolchain build:
#   cmake ... -DRVVM_BUILD_VIRTPASS_GUEST=ON \
#     -DCMAKE_C_COMPILER=<ndk>/toolchains/llvm/prebuilt/<host>/bin/riscv64-linux-android28-clang
# ============================================================
option(RVVM_BUILD_VIRTPASS_GUEST "Build the riscv64 virtpass guest stubs" OFF)

if(RVVM_BUILD_VIRTPASS_GUEST)
    add_library(virtpass_stub STATIC
        ${RVVM_SRC}/virtpass/vp_gl_stub.c
        ${RVVM_SRC}/virtpass/vp_ndk_stub.c
    )
    target_include_directories(virtpass_stub PUBLIC
        ${RVVM_INC}/virtpass
    )
    target_compile_options(virtpass_stub PRIVATE -DRVVM_USE_RV64)

    target_link_libraries(rvvm_jni virtpass_stub)
endif()