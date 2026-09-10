/*
 * sys/socket.h - MinGW shim (BSD socket API stubs, return ENOSYS).
 *
 * Guarded against winsock so a later <windows.h>/winsock2.h inclusion does
 * not produce conflicting declarations.
 */

#ifndef RVVM_MINGW_SYS_SOCKET_H
#define RVVM_MINGW_SYS_SOCKET_H

#if !defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_) && !defined(_WS2TCPIP_H_)

#include <sys/types.h>

#ifndef socklen_t_compat
typedef int socklen_t;
#endif

struct sockaddr {
    unsigned short sa_family;
    char           sa_data[14];
};

struct sockaddr_storage {
    unsigned short ss_family;
    char           __ss_pad[126];
};

struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    size_t        msg_iovlen;
    void*         msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

int     socket(int domain, int type, int protocol);
int     socketpair(int domain, int type, int protocol, int sv[2]);
int     bind(int fd, const struct sockaddr* addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int     connect(int fd, const struct sockaddr* addr, socklen_t len);
int     getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen);
int     getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* addr, socklen_t* addrlen);
int     setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int     getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int     shutdown(int fd, int how);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);

#endif /* winsock guards */

#endif /* RVVM_MINGW_SYS_SOCKET_H */