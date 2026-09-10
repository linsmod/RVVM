/*
 * fcntl.h - MinGW shim additions. O_* flag values come from the MinGW
 * header itself (rvvm_user.c converts guest Linux flags to host O_*
 * flags and passes those straight through to openat()). Only the F_*
 * fcntl commands and the *at() constants are missing there.
 */

#ifndef RVVM_MINGW_FCNTL_H
#define RVVM_MINGW_FCNTL_H

#include_next <fcntl.h>

#ifndef F_GETFD
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_DUPFD         0
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1
#endif

#ifndef AT_FDCWD
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000
#endif

int openat(int dirfd, const char* path, int flags, ...);

#endif /* RVVM_MINGW_FCNTL_H */
