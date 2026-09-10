/*
 * dirent.h - MinGW shim additions (fdopendir; MinGW already provides
 * opendir/readdir/closedir). Implementation lives in posix_shim.c.
 */

#ifndef RVVM_MINGW_DIRENT_H
#define RVVM_MINGW_DIRENT_H

#include_next <dirent.h>

DIR* fdopendir(int fd);

#endif /* RVVM_MINGW_DIRENT_H */
