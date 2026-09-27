/*
win_socket.c - WinSock backend behind the mingw_compat BSD socket shim
Copyright (C) 2024  RVVM contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * Why this layer exists
 * ---------------------
 * rvvm-user forwards guest syscalls straight to the host libc, so a guest fd
 * *is* a CRT fd: read()/write()/close()/poll() have to work on it. WinSock
 * sockets are not CRT files, and almost every constant differs from the Linux
 * UAPI values the guest passes in (SO_REUSEADDR 2 vs 4, AF_INET6 10 vs 23,
 * MSG_WAITALL 0x100 vs 8, POLLIN 1 vs 0x300, ...).
 *
 * So every socket is anchored onto a CRT fd coming from the CRT table and
 * translated here:
 *
 *      guest fd  --(wsock_fds[])-->  SOCKET
 *
 * The anchor fd itself is backed by a throw-away "NUL" handle, which keeps the
 * CLI's fd allocation (and therefore close()/dup() bookkeeping) authoritative
 * while leaving the socket itself under this layer's control.
 *
 * Only this file includes <winsock2.h>. src/util/networking.c also uses WinSock
 * but links the native names directly, hence the unprefixed BSD names must NOT
 * be defined here - the public surface is win_socket_* (see win_socket.h), and
 * include/mingw_compat/sys/socket.h redirects the POSIX spellings for rvvm_user.c
 * only.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>   // WSAStartup(), WSASocketW(), WSAPoll(), ...
#include <ws2tcpip.h>   // sockaddr_in6, IPPROTO_*, ...
#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "win_socket.h"

#ifndef AF_UNIX
#define AF_UNIX 1       /* WinSock spells it the same, some SDKs omit it */
#endif

/* ------------------------------------------------------------------------ */
/* Guest (Linux UAPI) numbering                                              */
/*                                                                           */
/* These mirror include/mingw_compat/sys/socket.h and the riscv64 UAPI. They  */
/* cannot be taken from the shim headers (they redefine the very names        */
/* <winsock2.h> just provided), so they are spelled out with the LK_ prefix.  */
/* ------------------------------------------------------------------------ */

#define LK_AF_UNIX    1
#define LK_AF_INET    2
#define LK_AF_INET6   10

#define LK_SOCK_TYPE_MASK 0xF
#define LK_SOCK_STREAM    1
#define LK_SOCK_DGRAM     2
#define LK_SOCK_SEQPACKET 5
#define LK_SOCK_NONBLOCK  0x800
#define LK_SOCK_CLOEXEC   0x80000

#define LK_SOL_SOCKET     1
#define LK_SO_DEBUG       1
#define LK_SO_REUSEADDR   2
#define LK_SO_TYPE        3
#define LK_SO_ERROR       4
#define LK_SO_DONTROUTE   5
#define LK_SO_BROADCAST   6
#define LK_SO_SNDBUF      7
#define LK_SO_RCVBUF      8
#define LK_SO_KEEPALIVE   9
#define LK_SO_OOBINLINE   10
#define LK_SO_LINGER      13
#define LK_SO_REUSEPORT   15
#define LK_SO_RCVTIMEO    20
#define LK_SO_SNDTIMEO    21

#define LK_IPPROTO_IP     0
#define LK_IPPROTO_TCP    6
#define LK_IPPROTO_IPV6   41

#define LK_MSG_OOB        1
#define LK_MSG_PEEK       2
#define LK_MSG_DONTROUTE  4
#define LK_MSG_WAITALL    0x100

#define LK_FIONBIO        0x5421
#define LK_FIONREAD       0x541B

/* ------------------------------------------------------------------------ */
/* fd anchors                                                                */
/* ------------------------------------------------------------------------ */

/* CRT fd table is bounded by the UCRT at 8192 entries */
#define WSOCK_MAX_FD 8192

/* WSAPoll() takes an array; cap the batch size and let the caller fall back */
#define WSOCK_POLL_MAX 128

static SRWLOCK wsock_lock = SRWLOCK_INIT;
static SOCKET  wsock_fds[WSOCK_MAX_FD];

/* What each anchor was last bound to, so a stale guest slot can tell "my socket"
 * from "whatever now sits on my number" (see win_socket_anchor_gen). Bumped on
 * every bind; a guest slot remembers the value it saw when it was filed. */
static uint64_t wsock_fds_gen[WSOCK_MAX_FD];
static volatile LONG64 wsock_fds_gen_next;

/* How many guest fd-table slots still name each anchor.
 *
 * The anchor's number comes from the CRT fd table (_open("NUL")), which is
 * process-global: while a socket anchor holds a number, *any* guest process in
 * the run that opens a file can be handed that same number, and once the anchor
 * gives it back the file owns it. A guest slot, on the other hand, keeps the
 * anchor number for as long as it lives - and every guest process of a run
 * shares one fd table and one CRT table, so a slot's number can be recycled out
 * from under it by an unrelated process. That is not theoretical: it is how the
 * sshd master ended up writing its privsep data into a client's connection.
 *
 * So an anchor is only given back to the CRT when the last slot naming it goes
 * away, and a recycled number can no longer be one somebody still holds. The
 * socket object is still closed on the first win_socket_close() - only the number
 * is kept reserved. */
static LONG wsock_fds_refs[WSOCK_MAX_FD];

/* Release the CRT number of an anchor, once nothing names it any more. */
static void wsock_anchor_release(int fd)
{
    bool last;
    AcquireSRWLockExclusive(&wsock_lock);
    last = (--wsock_fds_refs[fd] <= 0);
    if (last) {
        wsock_fds_refs[fd] = 0;
    }
    ReleaseSRWLockExclusive(&wsock_lock);
    if (last) {
        RVVM_TRC(RVVM_TRC_WSOCK, "free anchor %d (last reference)", fd);
        _close(fd);
    } else {
        RVVM_TRC(RVVM_TRC_WSOCK, "anchor %d kept (still referenced)", fd);
    }
}

/* Defined with the anchors below; needed by the public peer query above. */
static const char* wsock_peer_str(SOCKET s);

/* 0: not started, 1: WSAStartup in flight, 2: ready (see win_socket_init) */
static LONG wsock_init_state;

static SOCKET wsock_fd_get(int fd)
{
    SOCKET s = INVALID_SOCKET;
    if (fd < 0 || fd >= WSOCK_MAX_FD) {
        return INVALID_SOCKET;
    }
    AcquireSRWLockShared(&wsock_lock);
    s = wsock_fds[fd];
    ReleaseSRWLockShared(&wsock_lock);
    return s;
}

static SOCKET wsock_fd_take(int fd)
{
    SOCKET s = INVALID_SOCKET;
    if (fd < 0 || fd >= WSOCK_MAX_FD) {
        return INVALID_SOCKET;
    }
    AcquireSRWLockExclusive(&wsock_lock);
    s = wsock_fds[fd];
    wsock_fds[fd] = INVALID_SOCKET;
    /* Burn the generation as the anchor goes away, so whoever still names this
     * number finds 0 and can tell the binding is gone (win_socket_anchor_gen). */
    wsock_fds_gen[fd] = 0;
    ReleaseSRWLockExclusive(&wsock_lock);
    return s;
}

uint64_t win_socket_anchor_gen(int fd)
{
    uint64_t gen = 0;
    if (fd < 0 || fd >= WSOCK_MAX_FD) {
        return 0;
    }
    if (wsock_init_state != 2) {
        win_socket_init();
    }
    AcquireSRWLockShared(&wsock_lock);
    gen = (wsock_fds[fd] != INVALID_SOCKET) ? wsock_fds_gen[fd] : 0;
    ReleaseSRWLockShared(&wsock_lock);
    return gen;
}

int win_socket_is_fd(int fd)
{
    /* Called for every fd on the read/write/close/poll paths, including before
     * any socket exists */
    if (wsock_init_state != 2) {
        win_socket_init();
    }
    return wsock_fd_get(fd) != INVALID_SOCKET;
}

const char* win_socket_peer_str(int fd)
{
    if (wsock_init_state != 2) {
        win_socket_init();
    }
    return wsock_peer_str(wsock_fd_get(fd));
}

/* ------------------------------------------------------------------------ */
/* Init / error translation                                                  */
/* ------------------------------------------------------------------------ */

void win_socket_init(void)
{
    int i;
    if (wsock_init_state == 2) {
        return;
    }
    if (InterlockedCompareExchange(&wsock_init_state, 1, 0) == 0) {
        WSADATA wsa;
        /* INVALID_SOCKET is ~0, not 0, so the table cannot be left zeroed:
         * win_socket_is_fd() would report every unrelated fd as a socket. */
        for (i = 0; i < WSOCK_MAX_FD; i++) {
            wsock_fds[i] = INVALID_SOCKET;
        }
        WSAStartup(MAKEWORD(2, 2), &wsa);
        InterlockedExchange(&wsock_init_state, 2);
    } else {
        while (wsock_init_state != 2) {
            Sleep(0);
        }
    }
}

/*
 * Translate the pending WinSock error into the *CRT* errno.
 *
 * rvvm_user.c maps host errno values to the guest's UAPI codes by value
 * (host_guest_errno_map), and on the MinGW CRT the socket family already
 * carries the WSA numbering (EWOULDBLOCK 140, EINPROGRESS 112, ...), so
 * copying the WSA code over is exactly what that table expects.
 */
static int wsock_errno_of(int wsa)
{
    switch (wsa) {
    case WSAEWOULDBLOCK:      return EWOULDBLOCK;      /* -> guest EAGAIN */
    case WSAEINPROGRESS:      return EINPROGRESS;
    case WSAEALREADY:         return EALREADY;
    case WSAENOTSOCK:         return ENOTSOCK;
    case WSAEDESTADDRREQ:     return EDESTADDRREQ;
    case WSAEMSGSIZE:         return EMSGSIZE;
    case WSAEPROTOTYPE:       return EPROTOTYPE;
    case WSAENOPROTOOPT:      return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT:  return EPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT:  return EPROTONOSUPPORT;  /* MinGW has no ESOCKTNOSUPPORT */
    case WSAEOPNOTSUPP:       return EOPNOTSUPP;
    case WSAEAFNOSUPPORT:     return EAFNOSUPPORT;
    case WSAEADDRINUSE:       return EADDRINUSE;
    case WSAEADDRNOTAVAIL:    return EADDRNOTAVAIL;
    case WSAENETDOWN:         return ENETDOWN;
    case WSAENETUNREACH:      return ENETUNREACH;
    case WSAENETRESET:        return ENETRESET;
    case WSAECONNABORTED:     return ECONNABORTED;
    case WSAECONNRESET:       return ECONNRESET;
    case WSAENOBUFS:          return ENOBUFS;
    case WSAEISCONN:          return EISCONN;
    case WSAENOTCONN:         return ENOTCONN;
    case WSAESHUTDOWN:        return ENOTCONN;         /* MinGW has no ESHUTDOWN */
    case WSAETOOMANYREFS:     return EMFILE;           /* MinGW has no ETOOMANYREFS */
    case WSAETIMEDOUT:        return ETIMEDOUT;
    case WSAECONNREFUSED:     return ECONNREFUSED;
    case WSAEHOSTDOWN:        return EHOSTUNREACH;     /* MinGW has no EHOSTDOWN */
    case WSAEHOSTUNREACH:     return EHOSTUNREACH;
    case WSAEINTR:            return EINTR;
    case WSAEINVAL:           return EINVAL;
    case WSAEACCES:           return EACCES;
    case WSAEFAULT:           return EFAULT;
    case WSAEMFILE:           return EMFILE;
    default:                  return EIO;
    }
}

static void wsock_set_errno(void)
{
    errno = wsock_errno_of(WSAGetLastError());
}

/* ------------------------------------------------------------------------ */
/* Anchors                                                                   */
/* ------------------------------------------------------------------------ */

int win_socket_alloc_anchor(void)
{
    /* "NUL" is a plain CRT device: the fd can be closed or duplicated by the
     * CRT without ever touching a socket object. */
    int fd = _open("NUL", _O_RDWR | _O_BINARY);
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    AcquireSRWLockExclusive(&wsock_lock);
    /* Zero: the reference belongs to whichever guest slot gets filed onto this
     * anchor, and takes it in userland_fd_install() / the inherit paths. */
    wsock_fds_refs[fd] = 0;
    ReleaseSRWLockExclusive(&wsock_lock);
    RVVM_TRC(RVVM_TRC_WSOCK, "alloc anchor %d", fd);
    return fd;
}

/* One more guest slot now names @fd. The CRT number stays reserved until the
 * matching unrefs bring the count back to zero (see wsock_fds_refs). */
void win_socket_anchor_ref(int fd)
{
    if (fd < 0 || fd >= WSOCK_MAX_FD) {
        return;
    }
    AcquireSRWLockExclusive(&wsock_lock);
    InterlockedIncrement(&wsock_fds_refs[fd]);
    ReleaseSRWLockExclusive(&wsock_lock);
}

void win_socket_free_anchor(int fd)
{
    /* Not a guest-held slot (a failed alloc, a dup that could not be bound):
     * give the number straight back. */
    RVVM_TRC(RVVM_TRC_WSOCK,  "free anchor %d", fd);
    _close(fd);
}

/* A guest slot that named @fd is going away without closing a socket (an epoll
 * instance, say): drop its reference so the number can be reused. */
void win_socket_anchor_unref(int fd)
{
    if (fd < 0 || fd >= WSOCK_MAX_FD) {
        return;
    }
    wsock_anchor_release(fd);
}

/* The far end of @s as "host:port", or "?" when it has none. Every address space
 * of a run shares this one anchor table, so a bare anchor number does not say
 * *which* connection a descriptor is - this is what makes a trace readable
 * when a guest forks and both sides hold ends of the same connection. */
static const char* wsock_peer_str(SOCKET s)
{
    static char buf[64];
    struct sockaddr_in6 a6;
    struct sockaddr_in  a4;
    int len;

    if (s == INVALID_SOCKET) {
        return "?";
    }
    len = sizeof(a6);
    if (getpeername(s, (struct sockaddr*)&a6, &len) == 0) {
        if (a6.sin6_family == AF_INET6) {
            char ip[64];
            if (inet_ntop(AF_INET6, &a6.sin6_addr, ip, sizeof(ip))) {
                rvvm_snprintf(buf, sizeof(buf), "[%s]:%u", ip, ntohs(a6.sin6_port));
                return buf;
            }
        }
    }
    len = sizeof(a4);
    if (getpeername(s, (struct sockaddr*)&a4, &len) == 0 &&
        a4.sin_family == AF_INET) {
        char ip[32];
        if (inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip))) {
            rvvm_snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(a4.sin_port));
            return buf;
        }
    }
    return "?";
}

static int wsock_fd_alloc(SOCKET s)
{
    int fd = win_socket_alloc_anchor();
    if (fd < 0) {
        return -1;
    }
    if (fd >= WSOCK_MAX_FD) {
        win_socket_free_anchor(fd);
        errno = EMFILE;
        return -1;
    }
    AcquireSRWLockExclusive(&wsock_lock);
    wsock_fds[fd] = s;
    /* A fresh generation, never 0 (0 reads as "not a live anchor"): this is what
     * a guest slot files alongside the anchor number, so that a slot outliving
     * its anchor can recognise the number has been handed to someone else. */
    wsock_fds_gen[fd] = (uint64_t)InterlockedIncrement64(&wsock_fds_gen_next);
    ReleaseSRWLockExclusive(&wsock_lock);
    RVVM_TRC(RVVM_TRC_WSOCK,  "fd %d <- socket %p (gen %llu)", fd, (void*)s,
             (unsigned long long)wsock_fds_gen[fd]);
    return fd;
}

/* ------------------------------------------------------------------------ */
/* Enumeration translation                                                   */
/* ------------------------------------------------------------------------ */

static int lk_domain_to_win(int domain)
{
    switch (domain) {
    case LK_AF_UNIX:  return AF_UNIX;
    case LK_AF_INET:  return AF_INET;
    case LK_AF_INET6: return AF_INET6;
    default:          return -1;
    }
}

static int win_domain_to_lk(int domain)
{
    switch (domain) {
    case AF_UNIX:  return LK_AF_UNIX;
    case AF_INET:  return LK_AF_INET;
    case AF_INET6: return LK_AF_INET6;
    default:       return domain;
    }
}

static int lk_type_to_win(int type)
{
    /* The SOCK_* base values agree; only the Linux-only flags have to go */
    switch (type & LK_SOCK_TYPE_MASK) {
    case LK_SOCK_STREAM:    return SOCK_STREAM;
    case LK_SOCK_DGRAM:     return SOCK_DGRAM;
    case LK_SOCK_SEQPACKET: return SOCK_SEQPACKET;
    default:                return -1;
    }
}

static DWORD lk_msg_to_win(int flags)
{
    DWORD win = 0;
    if (flags & LK_MSG_OOB)       win |= MSG_OOB;
    if (flags & LK_MSG_PEEK)      win |= MSG_PEEK;
    if (flags & LK_MSG_DONTROUTE) win |= MSG_DONTROUTE;
    if (flags & LK_MSG_WAITALL)   win |= MSG_WAITALL;
    /* MSG_DONTWAIT / MSG_NOSIGNAL have no WinSock equivalent: the former is
     * covered by setting the socket non-blocking, the latter is meaningless
     * without SIGPIPE. */
    return win;
}

static int lk_level_to_win(int level)
{
    /* Only SOL_SOCKET differs (Linux 1, WinSock 0xffff); the IPPROTO_* levels
     * keep their values */
    return (level == LK_SOL_SOCKET) ? SOL_SOCKET : level;
}

static int lk_sockopt_to_win(int level, int opt)
{
    if (level != LK_SOL_SOCKET) {
        /* IPPROTO_TCP / IPPROTO_IP / IPPROTO_IPV6 option values agree for the
         * ones in use (TCP_NODELAY 1, IPV6_V6ONLY 27, ...) */
        return (level == LK_IPPROTO_IP || level == LK_IPPROTO_TCP ||
                level == LK_IPPROTO_IPV6) ? opt : -1;
    }
    switch (opt) {
    case LK_SO_DEBUG:     return SO_DEBUG;
    case LK_SO_REUSEADDR: return SO_REUSEADDR;
    case LK_SO_TYPE:      return SO_TYPE;
    case LK_SO_ERROR:     return SO_ERROR;
    case LK_SO_DONTROUTE: return SO_DONTROUTE;
    case LK_SO_BROADCAST: return SO_BROADCAST;
    case LK_SO_SNDBUF:    return SO_SNDBUF;
    case LK_SO_RCVBUF:    return SO_RCVBUF;
    case LK_SO_KEEPALIVE: return SO_KEEPALIVE;
    case LK_SO_OOBINLINE: return SO_OOBINLINE;
    case LK_SO_LINGER:    return SO_LINGER;
    case LK_SO_RCVTIMEO:  return SO_RCVTIMEO;
    case LK_SO_SNDTIMEO:  return SO_SNDTIMEO;
    case LK_SO_REUSEPORT: return SO_REUSEADDR;  /* no WinSock equivalent */
    default:              return -1;
    }
}

static short lk_poll_to_win(short events)
{
    short win = 0;
    if (events & WIN_POLLIN)  win |= POLLRDNORM;
    if (events & WIN_POLLPRI) win |= POLLRDBAND;
    if (events & WIN_POLLOUT) win |= POLLWRNORM;
    return win;
}

static short win_poll_to_lk(short events)
{
    short lk = 0;
    if (events & (POLLRDNORM | POLLRDBAND | POLLIN)) lk |= WIN_POLLIN;
    if (events & POLLWRNORM) lk |= WIN_POLLOUT;
    if (events & POLLERR)    lk |= WIN_POLLERR;
    if (events & POLLHUP)    lk |= WIN_POLLHUP;
    if (events & POLLNVAL)   lk |= WIN_POLLNVAL;
    return lk;
}

/*
 * sockaddr conversion: the layouts are identical (the guest is LP64 as well),
 * only the address-family *values* differ for IPv6. Unknown families are
 * rejected instead of leaking a raw number WinSock would misinterpret.
 */
static int addr_to_win(const void* src, int len, struct sockaddr_storage* dst, int* out_len)
{
    if (!src || len <= 0) {
        *out_len = 0;
        return 0;
    }
    if (len > (int)sizeof(*dst)) {
        len = (int)sizeof(*dst);
    }
    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, (size_t)len);
    switch (dst->ss_family) {
    case LK_AF_UNIX:
    case LK_AF_INET:
        break;
    case LK_AF_INET6:
        dst->ss_family = AF_INET6;
        break;
    default:
        errno = EAFNOSUPPORT;
        return -1;
    }
    *out_len = len;
    return 1;
}

static void addr_from_win(void* dst, int* len, const struct sockaddr_storage* src, int src_len)
{
    struct sockaddr_storage tmp;
    int n = src_len;
    if (!dst || !len || n <= 0) {
        return;
    }
    if (n > (int)sizeof(tmp)) n = (int)sizeof(tmp);
    memcpy(&tmp, src, (size_t)n);
    tmp.ss_family = (ADDRESS_FAMILY)win_domain_to_lk(tmp.ss_family);
    if (*len < n) {
        n = *len;
    }
    memcpy(dst, &tmp, (size_t)n);
    *len = n;
}

/* ------------------------------------------------------------------------ */
/* Data path helpers                                                         */
/* ------------------------------------------------------------------------ */

/* Layout of the POSIX iovec the shim hands over (win_socket.c does not include
 * the shim headers, so it must match include/mingw_compat/sys/uio.h) */
struct wsock_iov {
    void*  base;
    size_t len;
};

static int iov_to_wsabuf(const void* iov, int count, WSABUF* out)
{
    const struct wsock_iov* v = (const struct wsock_iov*)iov;
    int i;
    if (count < 0 || count > WSOCK_POLL_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (v[i].len > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        out[i].buf = (char*)v[i].base;
        out[i].len = (ULONG)v[i].len;
    }
    return count;
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                  */
/* ------------------------------------------------------------------------ */

int win_socket_create(int domain, int type, int protocol)
{
    int wd, wt, fd;
    int nonblock = (type & LK_SOCK_NONBLOCK) ? 1 : 0;
    SOCKET s;

    win_socket_init();
    wd = lk_domain_to_win(domain);
    if (wd < 0) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    wt = lk_type_to_win(type);
    if (wt < 0) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    s = WSASocketW(wd, wt, protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        wsock_set_errno();
        return -1;
    }
    if (wd == AF_INET6) {
        /* Windows defaults an IPv6 socket to v6-only, Linux to dual-stack: a
         * guest that binds "::" (busybox nc, many servers) expects the latter,
         * and with the Windows default its listener silently refuses every
         * IPv4 connection. Best effort - a socket that will not take the
         * option is still usable. */
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
    }
    if (nonblock) {
        u_long nb = 1;
        if (ioctlsocket(s, FIONBIO, &nb) == SOCKET_ERROR) {
            wsock_set_errno();
            closesocket(s);
            return -1;
        }
    }
    fd = wsock_fd_alloc(s);
    if (fd < 0) {
        closesocket(s);
    }
    return fd;
}

int win_socket_pair(int domain, int type, int protocol, int sv[2])
{
    SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET, server = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    int nonblock = (type & LK_SOCK_NONBLOCK) ? 1 : 0;
    int fd0, fd1;

    (void)protocol;
    if (!sv) {
        errno = EFAULT;
        return -1;
    }
    if (domain != LK_AF_UNIX && domain != LK_AF_INET && domain != LK_AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if ((type & LK_SOCK_TYPE_MASK) != LK_SOCK_STREAM) {
        /* Only stream pairs are emulated (over a loopback TCP connection) */
        errno = EOPNOTSUPP;
        return -1;
    }

    win_socket_init();

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    listener = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET) {
        goto fail;
    }
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        goto fail;
    }
    if (getsockname(listener, (struct sockaddr*)&addr, &addrlen) == SOCKET_ERROR) {
        goto fail;
    }
    if (listen(listener, 1) == SOCKET_ERROR) {
        goto fail;
    }

    client = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (client == INVALID_SOCKET) {
        goto fail;
    }
    if (connect(client, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        goto fail;
    }
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) {
        goto fail;
    }
    {
        struct sockaddr_in la = {}; int ll = sizeof(la);
        getsockname(listener, (struct sockaddr*)&la, &ll);
        struct sockaddr_in ca = {}; int cl = sizeof(ca);
        getpeername(server, (struct sockaddr*)&ca, &cl);
        RVVM_TRC(RVVM_TRC_WSOCK,  "pair: listener port %d, server peer 127.0.0.1:%d",
                    ntohs(la.sin_port), ntohs(ca.sin_port));
    }
    closesocket(listener);
    listener = INVALID_SOCKET;

    if (nonblock) {
        u_long nb = 1;
        ioctlsocket(client, FIONBIO, &nb);
        ioctlsocket(server, FIONBIO, &nb);
    }

    fd0 = wsock_fd_alloc(client);
    if (fd0 < 0) {
        goto fail;
    }
    fd1 = wsock_fd_alloc(server);
    if (fd1 < 0) {
        win_socket_close(fd0);
        goto fail;
    }
    sv[0] = fd0;
    sv[1] = fd1;
    return 0;

fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client != INVALID_SOCKET)   closesocket(client);
    if (server != INVALID_SOCKET)   closesocket(server);
    wsock_set_errno();
    return -1;
}

int win_socket_dup(int fd)
{
    SOCKET s = wsock_fd_get(fd);
    WSAPROTOCOL_INFOW info;
    SOCKET dup;
    int nfd;

    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (WSADuplicateSocketW(s, GetCurrentProcessId(), &info) != 0) {
        wsock_set_errno();
        return -1;
    }
    dup = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                     &info, 0, WSA_FLAG_OVERLAPPED);
    if (dup == INVALID_SOCKET) {
        wsock_set_errno();
        return -1;
    }
    nfd = wsock_fd_alloc(dup);
    if (nfd < 0) {
        closesocket(dup);
    }
    return nfd;
}

int win_socket_close(int fd)
{
    SOCKET s = wsock_fd_take(fd);
    RVVM_TRC(RVVM_TRC_WSOCK,  "close anchor %d (socket %p)", fd, (void*)s);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    /* The socket goes first: its handle value may be recycled immediately, and
     * the anchor (a plain CRT "NUL" file) is what _close() actually releases. */
    {
        struct sockaddr_storage peer;
        int plen = (int)sizeof(peer);
        if (getpeername(s, (struct sockaddr*)&peer, &plen) == 0) {
            struct sockaddr_in* in = (struct sockaddr_in*)&peer;
            unsigned char* ip = (unsigned char*)&in->sin_addr;
            RVVM_TRC(RVVM_TRC_WSOCK,  "close: peer=%d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], ntohs(in->sin_port));
        } else {
            RVVM_TRC(RVVM_TRC_WSOCK,  "close: getpeername err=%d (not connected?)", WSAGetLastError());
        }
    }
    if (closesocket(s) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        RVVM_TRC(RVVM_TRC_WSOCK,  "closesocket FAILED on fd %d (socket %p) err=%d", fd, (void*)s, err);
        wsock_anchor_release(fd);
        errno = wsock_errno_of(err);
        return -1;
    }
    RVVM_TRC(RVVM_TRC_WSOCK,  "closesocket OK on fd %d (socket %p)", fd, (void*)s);
    /* The socket is gone with this call, but the CRT *number* only goes back to
     * the fd table when the last guest slot naming it is gone too - otherwise a
     * still-live slot would find its number handed to an unrelated file (see
     * wsock_fds_refs). This call drops the closing slot's own reference. */
    wsock_anchor_release(fd);
    return 0;
}

int win_socket_set_nonblock(int fd, int on)
{
    SOCKET s = wsock_fd_get(fd);
    u_long nb = on ? 1 : 0;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (ioctlsocket(s, FIONBIO, &nb) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Connection setup                                                          */
/* ------------------------------------------------------------------------ */

int win_socket_bind(int fd, const void* addr, int len)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int wlen = 0;
    int have;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    have = addr_to_win(addr, len, &st, &wlen);
    if (have < 0) {
        return -1;
    }
    if (bind(s, (struct sockaddr*)&st, wlen) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_listen(int fd, int backlog)
{
    SOCKET s = wsock_fd_get(fd);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (listen(s, backlog) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_accept(int fd, void* addr, int* len, int flags)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int stlen = sizeof(st);
    SOCKET n;
    int nfd;
    int nonblock;

    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    /* SOCK_NONBLOCK arrives in the Linux flag position; SOCK_CLOEXEC has no
     * Windows equivalent (handles are not inherited by exec here). */
    nonblock = (flags & LK_SOCK_NONBLOCK) ? 1 : 0;

    n = accept(s, addr ? (struct sockaddr*)&st : NULL, addr ? &stlen : NULL);
    if (n == INVALID_SOCKET) {
        wsock_set_errno();
        return -1;
    }
    if (addr && len) {
        addr_from_win(addr, len, &st, stlen);
    }
    if (nonblock) {
        u_long nb = 1;
        ioctlsocket(n, FIONBIO, &nb);
    }
    nfd = wsock_fd_alloc(n);
    if (nfd < 0) {
        closesocket(n);
    }
    RVVM_TRC(RVVM_TRC_WSOCK,  "accept -> anchor %d (socket %p)", nfd, (void*)n);
    return nfd;
}

int win_socket_connect(int fd, const void* addr, int len)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int wlen = 0;
    int have;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    have = addr_to_win(addr, len, &st, &wlen);
    if (have < 0) {
        return -1;
    }
    if (connect(s, (struct sockaddr*)&st, wlen) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        /* A non-blocking connect that is *in progress* is not a failure: Winsock
         * reports WSAEWOULDBLOCK where POSIX says EINPROGRESS, and a guest that
         * distinguishes the two - which is what makes connect() + poll() work -
         * would take the EAGAIN for "try again later" and drop the attempt. */
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            errno = EINPROGRESS;
            return -1;
        }
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_getsockname(int fd, void* addr, int* len)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int stlen = sizeof(st);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (getsockname(s, (struct sockaddr*)&st, &stlen) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    addr_from_win(addr, len, &st, stlen);
    return 0;
}

int win_socket_getpeername(int fd, void* addr, int* len)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int stlen = sizeof(st);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (getpeername(s, (struct sockaddr*)&st, &stlen) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    addr_from_win(addr, len, &st, stlen);
    return 0;
}

int win_socket_shutdown(int fd, int how)
{
    SOCKET s = wsock_fd_get(fd);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (shutdown(s, how) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_setsockopt(int fd, int level, int opt, const void* val, int len)
{
    SOCKET s = wsock_fd_get(fd);
    int wopt;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    wopt = lk_sockopt_to_win(level, opt);
    if (wopt < 0) {
        errno = ENOPROTOOPT;
        return -1;
    }
    if (setsockopt(s, lk_level_to_win(level), wopt, (const char*)val, len) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_getsockopt(int fd, int level, int opt, void* val, int* len)
{
    SOCKET s = wsock_fd_get(fd);
    int wopt;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    wopt = lk_sockopt_to_win(level, opt);
    if (wopt < 0) {
        errno = ENOPROTOOPT;
        return -1;
    }
    if (getsockopt(s, lk_level_to_win(level), wopt, (char*)val, len) == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return 0;
}

int win_socket_ioctl(int fd, unsigned long request, void* arg)
{
    SOCKET s = wsock_fd_get(fd);
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (request == LK_FIONBIO) {
        u_long nb = (arg && *(int*)arg) ? 1 : 0;
        if (ioctlsocket(s, FIONBIO, &nb) == SOCKET_ERROR) {
            wsock_set_errno();
            return -1;
        }
        return 0;
    }
    if (request == LK_FIONREAD) {
        u_long avail = 0;
        if (!arg) {
            errno = EFAULT;
            return -1;
        }
        if (ioctlsocket(s, FIONREAD, &avail) == SOCKET_ERROR) {
            wsock_set_errno();
            return -1;
        }
        *(int*)arg = (int)avail;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Data path                                                                 */
/* ------------------------------------------------------------------------ */

long win_socket_read(int fd, void* buf, size_t len)
{
    SOCKET s = wsock_fd_get(fd);
    int n;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    n = recv(s, (char*)buf, (int)len, 0);
    if (n == 0) {
        RVVM_TRC(RVVM_TRC_WSOCK,  "recv EOF on fd %d (socket %p)", fd, (void*)s);
    } else if (n > 0) {
        RVVM_TRC(RVVM_TRC_WSOCK, "recv %ld byte(s) on fd %d (peer %s)", n, fd, wsock_peer_str(s));
    }
    if (n == SOCKET_ERROR) {
        int werr = WSAGetLastError();
        wsock_set_errno();
        if (werr != WSAEWOULDBLOCK) {
            RVVM_TRC(RVVM_TRC_WSOCK,  "recv err %d on fd %d (socket %p) errno=%d", werr, fd, (void*)s, errno);
        }
        return -1;
    }
    return n;
}

long win_socket_write(int fd, const void* buf, size_t len)
{
    SOCKET s = wsock_fd_get(fd);
    int n;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    n = send(s, (const char*)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    RVVM_TRC(RVVM_TRC_WSOCK, "sent %ld byte(s) on fd %d (peer %s)", n, fd, wsock_peer_str(s));
    return n;
}

long win_socket_sendto(int fd, const void* buf, size_t len, int flags,
                       const void* addr, int alen)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int wlen = 0;
    int have;
    int n;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    have = addr_to_win(addr, alen, &st, &wlen);
    if (have < 0) {
        return -1;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    n = sendto(s, (const char*)buf, (int)len, lk_msg_to_win(flags),
               have ? (struct sockaddr*)&st : NULL, wlen);
    if (n == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return n;
}

long win_socket_recvfrom(int fd, void* buf, size_t len, int flags,
                         void* addr, int* alen)
{
    SOCKET s = wsock_fd_get(fd);
    struct sockaddr_storage st;
    int stlen = sizeof(st);
    int n;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    n = recvfrom(s, (char*)buf, (int)len, lk_msg_to_win(flags),
                 addr ? (struct sockaddr*)&st : NULL, addr ? &stlen : NULL);
    if (n == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    if (addr && alen) {
        addr_from_win(addr, alen, &st, stlen);
    }
    return n;
}

long win_socket_send_iov(int fd, const void* iov, int iovcnt, int flags,
                         const void* addr, int alen)
{
    SOCKET s = wsock_fd_get(fd);
    WSABUF bufs[WSOCK_POLL_MAX];
    DWORD sent = 0;
    int r;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (iov_to_wsabuf(iov, iovcnt, bufs) < 0) {
        return -1;
    }
    if (addr) {
        struct sockaddr_storage st;
        int wlen = 0;
        int have = addr_to_win(addr, alen, &st, &wlen);
        if (have < 0) {
            return -1;
        }
        r = WSASendTo(s, bufs, (DWORD)iovcnt, &sent, lk_msg_to_win(flags),
                      (struct sockaddr*)&st, wlen, NULL, NULL);
    } else {
        r = WSASend(s, bufs, (DWORD)iovcnt, &sent, lk_msg_to_win(flags), NULL, NULL);
    }
    if (r == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    return (long)sent;
}

long win_socket_recv_iov(int fd, const void* iov, int iovcnt, int flags,
                         void* addr, int* alen)
{
    SOCKET s = wsock_fd_get(fd);
    WSABUF bufs[WSOCK_POLL_MAX];
    struct sockaddr_storage st;
    int stlen = sizeof(st);
    DWORD recvd = 0;
    DWORD wflags = lk_msg_to_win(flags);
    int r;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    if (iov_to_wsabuf(iov, iovcnt, bufs) < 0) {
        return -1;
    }
    if (addr) {
        r = WSARecvFrom(s, bufs, (DWORD)iovcnt, &recvd, &wflags,
                        (struct sockaddr*)&st, &stlen, NULL, NULL);
    } else {
        r = WSARecv(s, bufs, (DWORD)iovcnt, &recvd, &wflags, NULL, NULL);
    }
    if (r == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    if (addr && alen) {
        addr_from_win(addr, alen, &st, stlen);
    }
    return (long)recvd;
}

/* ------------------------------------------------------------------------ */
/* Readiness                                                                 */
/* ------------------------------------------------------------------------ */

int win_socket_wait(int fd, short events, int timeout_ms)
{
    SOCKET s = wsock_fd_get(fd);
    WSAPOLLFD p;
    int r;
    if (s == INVALID_SOCKET) {
        errno = ENOTSOCK;
        return -1;
    }
    p.fd = s;
    p.events = lk_poll_to_win(events);
    p.revents = 0;
    r = WSAPoll(&p, 1, timeout_ms);
    if (r == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    return win_poll_to_lk(p.revents);
}

int win_socket_wait_many(const int* fds, const short* events, short* ready,
                         int count, int timeout_ms)
{
    WSAPOLLFD p[WSOCK_POLL_MAX];
    int i, r, n = 0;
    if (count < 0 || count > WSOCK_POLL_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; i++) {
        SOCKET s = wsock_fd_get(fds[i]);
        ready[i] = 0;
        if (s == INVALID_SOCKET) {
            continue;
        }
        p[n].fd = s;
        p[n].events = lk_poll_to_win(events[i]);
        p[n].revents = 0;
        n++;
    }
    if (!n) {
        return 0;
    }
    r = WSAPoll(p, (ULONG)n, timeout_ms);
    if (r == SOCKET_ERROR) {
        wsock_set_errno();
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    {
        int k = 0;
        for (i = 0; i < count; i++) {
            if (wsock_fd_get(fds[i]) == INVALID_SOCKET) {
                continue;
            }
            if (p[k].revents) {
                ready[i] = win_poll_to_lk(p[k].revents);
            }
            k++;
        }
    }
    return r;
}
