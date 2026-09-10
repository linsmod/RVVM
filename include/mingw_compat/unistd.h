/*
 * unistd.h - MinGW shim additions. MinGW's unistd.h covers read/write/
 * close/lseek/dup/access/getcwd etc.; everything the RVVM syscall layer
 * needs beyond that is declared here and implemented in posix_shim.c.
 */

#ifndef RVVM_MINGW_UNISTD_H
#define RVVM_MINGW_UNISTD_H

#include_next <unistd.h>

#include <sys/types.h>

/* uid_t/gid_t are missing from MinGW entirely (shared guard with
 * sys/stat.h/grp.h shims) */
#ifndef _RVVM_COMPAT_UIDGID
#define _RVVM_COMPAT_UIDGID
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
ssize_t pread(int fd, void* buf, size_t count, long long offset);
ssize_t pwrite(int fd, const void* buf, size_t count, long long offset);
int     fsync(int fd);
int     fdatasync(int fd);
int     fchdir(int fd);
int     fcntl(int fd, int cmd, ...);
int     dup3(int oldfd, int newfd, int flags);
pid_t   fork(void);
int     kill(pid_t pid, int sig);
long    gettid(void);
pid_t   getppid(void);
pid_t   setsid(void);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
ssize_t getrandom(void* buf, size_t len, unsigned flags);
int     memfd_create(const char* name, unsigned flags);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz);
int     symlinkat(const char* target, int dirfd, const char* linkpath);
ssize_t linkat(int fd1, const char* path1, int fd2, const char* path2, int flags);
int     renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int     faccessat(int dirfd, const char* path, int mode, int flags);
int     getpagesize(void);
long    syscall(long number, ...);

#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif

#endif /* RVVM_MINGW_UNISTD_H */
