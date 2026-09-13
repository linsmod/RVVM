/*
 * sys/socket.h - MinGW shim: socket API as seen by the userland syscall bridge
 * (src/core/rvvm_user.c), implemented by src/win/win_socket.c on top of WinSock.
 *
 * Two socket worlds coexist in the win32 host build:
 *
 *  - src/util/networking.c talks to WinSock directly (it includes <winsock2.h>
 *    and never this header); those are the real BSD names exported by ws2_32.
 *  - the userland bridge needs POSIX-looking calls that speak the *guest*
 *    (Linux UAPI) numbering and hand back CRT fds, because a guest fd has to
 *    work with read()/write()/close()/poll().
 *
 * Both cannot own the same symbol, so the bridge lives under rvvm_win_* names
 * and the POSIX spellings are redirected here. The redirects are function-like
 * macros: they only expand on a call, so an unrelated identifier that merely
 * shares the name (a struct member, say) still compiles.
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

/*
 * Linux UAPI values: rvvm_user.c forwards guest syscall arguments untouched,
 * so every constant here has to match the guest's ABI rather than WinSock's
 * (AF_INET6 is 10, not 23; SO_REUSEADDR is 2, not 4; POLLIN is 1, not 0x300).
 */
#define AF_UNSPEC     0
#define AF_UNIX       1
#define AF_LOCAL      1
#define AF_INET       2
#define AF_INET6      10

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

#define SOL_SOCKET    1
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_OOBINLINE  10
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21
#define SO_PROTOCOL   38
#define SO_DOMAIN     39

#define IPPROTO_IP    0
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#define IPPROTO_IPV6  41

#define MSG_OOB       1
#define MSG_PEEK      2
#define MSG_DONTROUTE 4
#define MSG_CTRUNC    8
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define FIONBIO  0x5421
#define FIONREAD 0x541B

int     rvvm_win_socket(int domain, int type, int protocol);
int     rvvm_win_socketpair(int domain, int type, int protocol, int sv[2]);
int     rvvm_win_bind(int fd, const struct sockaddr* addr, socklen_t len);
int     rvvm_win_listen(int fd, int backlog);
int     rvvm_win_accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int     rvvm_win_accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags);
int     rvvm_win_connect(int fd, const struct sockaddr* addr, socklen_t len);
int     rvvm_win_getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen);
int     rvvm_win_getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t rvvm_win_sendto(int fd, const void* buf, size_t len, int flags,
                        const struct sockaddr* addr, socklen_t addrlen);
ssize_t rvvm_win_recvfrom(int fd, void* buf, size_t len, int flags,
                          struct sockaddr* addr, socklen_t* addrlen);
int     rvvm_win_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int     rvvm_win_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int     rvvm_win_shutdown(int fd, int how);
ssize_t rvvm_win_sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t rvvm_win_recvmsg(int fd, struct msghdr* msg, int flags);
int     rvvm_win_dup(int fd);
int     rvvm_win_close(int fd);

#define socket(...)     rvvm_win_socket(__VA_ARGS__)
#define socketpair(...) rvvm_win_socketpair(__VA_ARGS__)
#define bind(...)       rvvm_win_bind(__VA_ARGS__)
#define listen(...)     rvvm_win_listen(__VA_ARGS__)
#define accept(...)     rvvm_win_accept(__VA_ARGS__)
#define accept4(...)    rvvm_win_accept4(__VA_ARGS__)
#define connect(...)    rvvm_win_connect(__VA_ARGS__)
#define getsockname(...)  rvvm_win_getsockname(__VA_ARGS__)
#define getpeername(...)  rvvm_win_getpeername(__VA_ARGS__)
#define sendto(...)     rvvm_win_sendto(__VA_ARGS__)
#define recvfrom(...)   rvvm_win_recvfrom(__VA_ARGS__)
#define setsockopt(...) rvvm_win_setsockopt(__VA_ARGS__)
#define getsockopt(...) rvvm_win_getsockopt(__VA_ARGS__)
#define shutdown(...)   rvvm_win_shutdown(__VA_ARGS__)
#define sendmsg(...)    rvvm_win_sendmsg(__VA_ARGS__)
#define recvmsg(...)    rvvm_win_recvmsg(__VA_ARGS__)

#endif /* winsock guards */

#endif /* RVVM_MINGW_SYS_SOCKET_H */
