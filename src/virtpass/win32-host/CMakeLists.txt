# RVVM Windows host
#
# Builds the host bridge (window/input/lifecycle), vp_cmdpost (single copy
# in src/virtpass) and the full RVVM core for real guest execution. The POSIX
# layer is shimmed via include/mingw_compat/ headers + src/win/posix_shim.c
# implementations (semantic gaps like epoll/eventfd/futex/fork/sockets remain
# ENOSYS - see README "Known gaps").

cmake_minimum_required(VERSION 3.15)
project(rvvm_winhost C)

set(RVVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(APP_CPP   "${RVVM_ROOT}/src/virtpass/android-host/app/src/main/cpp")

add_executable(rvvm_winhost
    win32_main.c
    win32_cmdpost_bridge.c
    win32_gl_backend.c
    win32_gl_dispatch.c
    # vp_cmdpost (single copy, shared with the Android app build)
    ${RVVM_ROOT}/src/virtpass/vp_cmdpost.c
    # POSIX shim implementations for MinGW (see include/mingw_compat/)
    ${RVVM_ROOT}/src/win/posix_shim.c
    # Core
    ${RVVM_ROOT}/src/core/rvvm.c
    ${RVVM_ROOT}/src/core/rvvm_user.c
    ${RVVM_ROOT}/src/core/rvvm_irq.c
    ${RVVM_ROOT}/src/core/rvvm_region.c
    ${RVVM_ROOT}/src/core/rvvm_isolation.c
    ${RVVM_ROOT}/src/core/rvvm_fdt.c
    ${RVVM_ROOT}/src/core/gdbstub.c
    # CPU
    ${RVVM_ROOT}/src/cpu/riscv_cpu.c
    ${RVVM_ROOT}/src/cpu/riscv_hart.c
    ${RVVM_ROOT}/src/cpu/riscv_csr.c
    ${RVVM_ROOT}/src/cpu/riscv_mmu.c
    ${RVVM_ROOT}/src/cpu/riscv_priv.c
    ${RVVM_ROOT}/src/cpu/riscv_fpu.c
    ${RVVM_ROOT}/src/cpu/riscv64_interpreter.c
    # Utils
    ${RVVM_ROOT}/src/util/elf_load.c
    ${RVVM_ROOT}/src/util/vma_ops.c
    ${RVVM_ROOT}/src/util/blk_io.c
    ${RVVM_ROOT}/src/util/utils.c
    ${RVVM_ROOT}/src/util/threading.c
    ${RVVM_ROOT}/src/util/atomics.c
    ${RVVM_ROOT}/src/util/locking.c
    ${RVVM_ROOT}/src/util/stacktrace.c
    ${RVVM_ROOT}/src/util/rvtimer.c
    ${RVVM_ROOT}/src/util/vector.c
    ${RVVM_ROOT}/src/util/hashmap.c
    ${RVVM_ROOT}/src/util/ringbuf.c
    ${RVVM_ROOT}/src/util/rcu_lib.c
    ${RVVM_ROOT}/src/util/fpu_lib.c
    ${RVVM_ROOT}/src/util/dlib.c
)

# Same macro set as the Android app build (app/src/main/cpp/CMakeLists.txt)
target_compile_definitions(rvvm_winhost PRIVATE
    RVVM_USER_TEST
    RVVM_USE_RV64
    USE_RV64
    USE_NO_RVJIT
    USE_FPU
    _GNU_SOURCE
    # Core is statically linked into the exe. Without this, rvvm_base.h
    # marks PUBLIC symbols __declspec(dllimport) on Windows, breaking the
    # final link with undefined __imp_* references.
    RVVM_STATIC
)

target_include_directories(rvvm_winhost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${APP_CPP}
    ${RVVM_ROOT}/src/core
    ${RVVM_ROOT}/src
    ${RVVM_ROOT}/src/cpu
    ${RVVM_ROOT}/src/util
    ${RVVM_ROOT}/include
    # POSIX/Linux header shims so rvvm_user.c can compile under MinGW
    ${RVVM_ROOT}/include/mingw_compat
)

# MinGW: make %lld/%llu in printf reliable across CRT flavors
target_compile_definitions(rvvm_winhost PRIVATE __USE_MINGW_ANSI_STDIO=1)

target_link_libraries(rvvm_winhost PRIVATE user32 gdi32)
