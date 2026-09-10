/* compat_fix.h - Workarounds for NDK clang issues */
#ifndef COMPAT_FIX_H
#define COMPAT_FIX_H

/* memfd_create declaration (not in NDK headers) */
#include <sys/types.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
extern int memfd_create(const char *__name, unsigned int __flags) __attribute__((__weak__));

/* statx declaration (not in NDK headers) */
#if !defined(__linux__)
struct statx;
#endif
#ifndef STATX_TYPE
#define STATX_TYPE 0x0001U
#endif
#ifndef AT_STATX_SYNC_TYPE
#define AT_STATX_SYNC_TYPE 0x6000
#endif
extern int statx(int __dirfd, const char* __path, int __flags,
                 unsigned int __mask, void* __buf) __attribute__((__weak__));

#endif /* COMPAT_FIX_H */