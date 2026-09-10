/*
 * sys/mman.h - MinGW shim on top of Win32 VirtualAlloc/VirtualFree/VirtualProtect.
 *
 * Only anonymous-private/fixed mappings used by the guest runtime are
 * supported; file-backed maps return -1 (ENOSYS).
 */

#ifndef RVVM_MINGW_SYS_MMAN_H
#define RVVM_MINGW_SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>

#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

#define MAP_SHARED      0x001
#define MAP_PRIVATE     0x002
#define MAP_FIXED       0x010
#define MAP_ANONYMOUS   0x020
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED      ((void*)-1)

/* Additional Linux mmap flags: unimplemented by the shim, but recognized
 * (and ignored) by the guest mmap flag converter */
#ifndef MAP_GROWSDOWN
#define MAP_GROWSDOWN   0x0100
#endif
#ifndef MAP_DENYWRITE
#define MAP_DENYWRITE   0x0800
#endif
#ifndef MAP_EXECUTABLE
#define MAP_EXECUTABLE  0x1000
#endif
#ifndef MAP_LOCKED
#define MAP_LOCKED      0x2000
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE   0x4000
#endif
#ifndef MAP_POPULATE
#define MAP_POPULATE    0x8000
#endif
#ifndef MAP_NONBLOCK
#define MAP_NONBLOCK    0x10000
#endif
#ifndef MAP_STACK
#define MAP_STACK       0x20000
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB     0x40000
#endif

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   madvise(void* addr, size_t length, int advice);

#endif /* RVVM_MINGW_SYS_MMAN_H */