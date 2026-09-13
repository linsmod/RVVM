/*
win_socket.h - WinSock backend behind the mingw_compat BSD socket shim
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

#ifndef RVVM_WIN_SOCKET_H
#define RVVM_WIN_SOCKET_H

#include <stddef.h>

/*
 * Every entry point below takes *guest* (Linux UAPI) numbering, because
 * rvvm_user.c forwards guest syscall arguments untouched. The translation to
 * the WinSock spelling (SO_* / AF_INET6 / MSG_WAITALL / poll bits / ...) stays
 * inside win_socket.c, which is the only translation unit including
 * <winsock2.h>: the mingw_compat headers give the same names the Linux values,
 * so the two must never meet in one file.
 *
 * All functions return -1 and set errno on failure, unless stated otherwise.
 */

/* Idempotent WSAStartup(); called lazily by every creating entry point */
void win_socket_init(void);

/* Non-zero when this CRT fd is a WinSock socket owned by this layer */
int  win_socket_is_fd(int fd);

/* CRT fd anchor: a slot taken from the CRT fd table so that socket descriptors
 * get a numbering that cannot collide with ordinary files (and that read()/
 * close()/dup() can be dispatched on). Used for sockets and epoll instances. */
int  win_socket_alloc_anchor(void);
void win_socket_free_anchor(int fd);

/* Creation */
int  win_socket_create(int domain, int type, int protocol);
int  win_socket_pair(int domain, int type, int protocol, int sv[2]);
int  win_socket_dup(int fd);

/* Connection setup / metadata */
int  win_socket_bind(int fd, const void* addr, int len);
int  win_socket_listen(int fd, int backlog);
int  win_socket_accept(int fd, void* addr, int* len, int flags);
int  win_socket_connect(int fd, const void* addr, int len);
int  win_socket_getsockname(int fd, void* addr, int* len);
int  win_socket_getpeername(int fd, void* addr, int* len);
int  win_socket_shutdown(int fd, int how);
int  win_socket_setsockopt(int fd, int level, int opt, const void* val, int len);
int  win_socket_getsockopt(int fd, int level, int opt, void* val, int* len);
int  win_socket_ioctl(int fd, unsigned long request, void* arg);

/* Data path */
long win_socket_read(int fd, void* buf, size_t len);
long win_socket_write(int fd, const void* buf, size_t len);
long win_socket_sendto(int fd, const void* buf, size_t len, int flags,
                       const void* addr, int alen);
long win_socket_recvfrom(int fd, void* buf, size_t len, int flags,
                         void* addr, int* alen);
long win_socket_send_iov(int fd, const void* iov, int iovcnt, int flags,
                         const void* addr, int alen);
long win_socket_recv_iov(int fd, const void* iov, int iovcnt, int flags,
                         void* addr, int* alen);

/* Complete teardown of a socket fd: closesocket() + releases the CRT slot */
int  win_socket_close(int fd);

/* Non-blocking mode (guest O_NONBLOCK semantics) */
int  win_socket_set_nonblock(int fd, int on);

/* Readiness bits, in the guest (Linux UAPI) numbering that rvvm_user.c's
 * poll()/epoll() paths expect; they match include/mingw_compat/poll.h */
#define WIN_POLLIN   0x001
#define WIN_POLLPRI  0x002
#define WIN_POLLOUT  0x004
#define WIN_POLLERR  0x008
#define WIN_POLLHUP  0x010
#define WIN_POLLNVAL 0x020

/* Readiness of a single socket fd.
 * Returns 0 on timeout, the Linux POLL* bit mask when ready, -1 on error. */
int  win_socket_wait(int fd, short events, int timeout_ms);

/* Wait on a set of socket fds at once (WSAPoll). @ready receives the Linux
 * POLL* bit mask per entry. Returns the number of ready fds, 0 on timeout. */
int  win_socket_wait_many(const int* fds, const short* events, short* ready,
                          int count, int timeout_ms);

#endif
