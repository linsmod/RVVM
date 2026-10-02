/*
 * netinet/in.h - MinGW shim: the IPv4 socket address, in the Linux (guest) ABI
 * that the BSD socket shim (src/win/win_socket.c) expects.
 *
 * sys/socket.h has already fixed the numbering (AF_INET is 2, not WinSock's
 * 2-byte AF_INET coincidence, ...); what is missing is the address type and the
 * byte-order helpers the shared socket code needs. The layout here is the
 * guest's - a 16-bit family, a 16-bit big-endian port, a 4-byte big-endian
 * address - because that is exactly what win_socket_bind()/connect() translate
 * from. One consequence worth stating: the helpers live in posix_shim.c rather
 * than here, since ws2_32 already exports them and the two must not meet in one
 * translation unit.
 *
 * Guarded the same way sys/socket.h is: if <winsock2.h> has been seen, its
 * struct in_addr already exists and a second definition would not compile.
 */

#ifndef RVVM_MINGW_NETINET_IN_H
#define RVVM_MINGW_NETINET_IN_H

#if !defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_) && !defined(_WS2TCPIP_H_)

#include <stdint.h>
#include <sys/socket.h> /* AF_INET, IPPROTO_*, socklen_t */

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr; /* network byte order */
};

struct sockaddr_in {
    uint16_t       sin_family;
    in_port_t      sin_port; /* network byte order */
    struct in_addr sin_addr; /* network byte order */
    unsigned char  sin_zero[8];
};

#define INADDR_ANY       ((in_addr_t)0x00000000u)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001u) /* host order, as POSIX has it */
#define INADDR_BROADCAST ((in_addr_t)0xffffffffu)
#define INADDR_NONE      ((in_addr_t)0xffffffffu)

/* Implemented in src/win/posix_shim.c (see the note at the top). */
uint16_t  htons(uint16_t v);
uint16_t  ntohs(uint16_t v);
uint32_t  htonl(uint32_t v);
uint32_t  ntohl(uint32_t v);
in_addr_t inet_addr(const char* cp);

#endif /* winsock guards */

#endif /* RVVM_MINGW_NETINET_IN_H */
