/*
 * sys/stat.h - MinGW shim. MinGW's struct stat lacks st_blksize/st_blocks,
 * which uapi_stat_convert() needs. Provide a POSIX-shaped struct stat plus
 * the *at() family, and redirect stat/fstat/lstat to implementations in
 * posix_shim.c (which fill st_blksize/st_blocks on top of _stat64).
 */

#ifndef RVVM_MINGW_SYS_STAT_H
#define RVVM_MINGW_SYS_STAT_H

#include_next <sys/stat.h>

#include <sys/types.h>

/* uid_t/gid_t are missing from MinGW entirely (shared guard with
 * unistd.h/grp.h shims) */
#ifndef _RVVM_COMPAT_UIDGID
#define _RVVM_COMPAT_UIDGID
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

/* Avoid clashes with MinGW's stat macros/typedefs */
#undef stat
#undef fstat
#undef lstat

struct rvvm_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    long long          st_size;
    long long          st_blksize;
    long long          st_blocks;
    long long          st_atime;
    long long          st_mtime;
    long long          st_ctime;
};

int rvvm_stat(const char* path, struct rvvm_stat* buf);
int rvvm_lstat(const char* path, struct rvvm_stat* buf);
int rvvm_fstat(int fd, struct rvvm_stat* buf);

int fstatat(int dirfd, const char* path, struct rvvm_stat* buf, int flags);
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char* path, mode_t mode, int flags);
int fchown(int fd, uid_t owner, gid_t group);
int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags);
int mkdirat(int dirfd, const char* path, mode_t mode);
int mknodat(int dirfd, const char* path, mode_t mode, unsigned long long dev);
int unlinkat(int dirfd, const char* path, int flags);
int umask(int mask);

#define stat  rvvm_stat
#define fstat rvvm_fstat
#define lstat rvvm_lstat

#endif /* RVVM_MINGW_SYS_STAT_H */
