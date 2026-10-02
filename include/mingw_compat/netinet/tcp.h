/*
 * netinet/tcp.h - MinGW shim: the TCP-level socket option names, in the Linux
 * numbering win_socket.c translates. Only the option a TCP socket is worth
 * tuning with is here; the rest would be names nothing calls for.
 */

#ifndef RVVM_MINGW_NETINET_TCP_H
#define RVVM_MINGW_NETINET_TCP_H

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#ifndef TCP_MAXSEG
#define TCP_MAXSEG 2
#endif

#endif /* RVVM_MINGW_NETINET_TCP_H */
