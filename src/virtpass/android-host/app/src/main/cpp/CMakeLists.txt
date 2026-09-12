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
add_definitions(
    -DRVVM_USER_TEST
    -DRVVM_USE_RV64
    -DUSE_RV64
    -DANDROID
    -D__linux__
    -DUSE_JIT
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

    # JIT (ARM64 backend, RVJIT_NATIVE_LINKER)
    ${RVVM_SRC}/rvjit/rvjit.c
    ${RVVM_SRC}/rvjit/rvjit_emit.c

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

target_link_libraries(rvvm_core
    android
    log
)

# 16KB page size alignment for Android 15+ compatibility
target_link_options(rvvm_core PRIVATE -Wl,-z,max-page-size=16384)

# ============================================================
# JNI shared library (the final .so loaded by Java)
# ============================================================
add_library(rvvm_jni SHARED
    jni_bridge.c
    vp_aaudio_android.c
    android_gl_host.c
)

target_include_directories(rvvm_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rvvm_jni
    rvvm_core
    android
    log
    m
    aaudio
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