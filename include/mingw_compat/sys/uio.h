/*
 * sys/uio.h - MinGW shim (readv/writev are not provided by msvcrt)
 */

#ifndef RVVM_MINGW_SYS_UIO_H
#define RVVM_MINGW_SYS_UIO_H

#include <sys/types.h>

struct iovec {
    void*  iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#endif /* RVVM_MINGW_SYS_UIO_H */