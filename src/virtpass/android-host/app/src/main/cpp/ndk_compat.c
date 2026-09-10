/* ndk_compat.c - Definitions for syscalls missing below Android API 30 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_memfd_create
#if defined(__aarch64__)
#define __NR_memfd_create 276
#else
#define __NR_memfd_create 279
#endif
#endif

#ifndef __NR_statx
#if defined(__aarch64__)
#define __NR_statx 291
#else
#define __NR_statx 291
#endif
#endif

int memfd_create(const char* name, unsigned int flags)
{
    return (int)syscall(__NR_memfd_create, name, flags);
}

int statx(int dirfd, const char* pathname, int flags, unsigned int mask, void* statxbuf)
{
    return (int)syscall(__NR_statx, dirfd, pathname, flags, mask, statxbuf);
}

/*
 * GDB stub is not built for Android (requires USE_NET). The machine's
 * gdbstub pointer is always NULL in user-mode builds, so this stub is
 * never actually reached. Keep it to satisfy the link of riscv_hart.c.
 */
#include <stdbool.h>
typedef struct gdb_server gdb_server_t;
bool gdbstub_halt(gdb_server_t* server)
{
    (void)server;
    return false;
}