/*
 * poll.h - MinGW shim (pollfd + poll). Implemented over Win32 kernel objects
 * and WSAPoll() in src/win/posix_shim.c; the event bits use the Linux UAPI
 * numbering because that is what the guest passes in.
 */

#ifndef RVVM_MINGW_POLL_H
#define RVVM_MINGW_POLL_H

#include <sys/types.h>

#define POLLIN  0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

typedef int nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#endif /* RVVM_MINGW_POLL_H */