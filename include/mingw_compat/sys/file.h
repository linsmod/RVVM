/*
 * sys/file.h - MinGW shim (flock). MinGW does not ship this header at all.
 * Implementation lives in posix_shim.c.
 */

#ifndef RVVM_MINGW_SYS_FILE_H
#define RVVM_MINGW_SYS_FILE_H

#include <sys/types.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif /* RVVM_MINGW_SYS_FILE_H */
