/*
 * sys/epoll.h - MinGW shim: epoll as seen by the userland syscall bridge
 * (src/core/rvvm_user.c), emulated over poll() in src/win/posix_shim.c.
 *
 * Event bits and control opcodes use the Linux UAPI numbering, because
 * rvvm_user.c translates only the struct layout and forwards the values.
 */

#ifndef RVVM_MINGW_SYS_EPOLL_H
#define RVVM_MINGW_SYS_EPOLL_H

#include <stdint.h>

#define EPOLLIN        0x00000001
#define EPOLLPRI       0x00000002
#define EPOLLOUT       0x00000004
#define EPOLLERR       0x00000008
#define EPOLLHUP       0x00000010
#define EPOLLNVAL      0x00000020
#define EPOLLRDNORM    0x00000040
#define EPOLLRDBAND    0x00000080
#define EPOLLWRNORM    0x00000100
#define EPOLLWRBAND    0x00000200
#define EPOLLMSG       0x00000400
#define EPOLLRDHUP     0x00002000
#define EPOLLEXCLUSIVE 0x10000000
#define EPOLLWAKEUP    0x20000000
#define EPOLLONESHOT   0x40000000
#define EPOLLET        0x80000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 0x80000

typedef union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);

#endif /* RVVM_MINGW_SYS_EPOLL_H */
