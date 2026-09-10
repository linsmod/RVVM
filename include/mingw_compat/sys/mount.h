/*
 * sys/mount.h - MinGW shim (struct statfs + statfs/fstatfs stubs).
 */

#ifndef RVVM_MINGW_SYS_MOUNT_H
#define RVVM_MINGW_SYS_MOUNT_H

struct statfs {
    long     f_type;
    long     f_bsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    struct { int val[2]; } f_fsid;
    long     f_namelen;
    long     f_frsize;
    long     f_flags;
    long     f_spare[4];
};

int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);

#endif /* RVVM_MINGW_SYS_MOUNT_H */