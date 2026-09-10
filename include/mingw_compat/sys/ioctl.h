/*
 * sys/ioctl.h - MinGW shim (ioctl stub, returns ENOSYS).
 */

#ifndef RVVM_MINGW_SYS_IOCTL_H
#define RVVM_MINGW_SYS_IOCTL_H

int ioctl(int fd, unsigned long request, ...);

#endif /* RVVM_MINGW_SYS_IOCTL_H */