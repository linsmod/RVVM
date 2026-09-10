/*
 * sys/select.h - MinGW shim (fd_set + select, pselect not provided).
 */

#ifndef RVVM_MINGW_SYS_SELECT_H
#define RVVM_MINGW_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct fd_set {
    unsigned int fd_count;
    int fd_array[FD_SETSIZE];
} fd_set;

#define FD_ZERO(s)                        do { (s)->fd_count = 0; } while (0)
#define FD_SET(fd, s)                     do { if ((s)->fd_count < FD_SETSIZE && fd >= 0) (s)->fd_array[(s)->fd_count++] = (fd); } while (0)
#define FD_CLR(fd, s)                     { unsigned int _i; for (_i = 0; _i < (s)->fd_count; _i++) if ((s)->fd_array[_i] == (fd)) { (s)->fd_count--; (s)->fd_array[_i] = (s)->fd_array[(s)->fd_count]; break; } }
#define FD_ISSET(fd, s)                   _fd_isset(fd, s)

static inline int _fd_isset(int fd, const fd_set* s) {
    unsigned int _i;
    for (_i = 0; _i < s->fd_count; _i++)
        if (s->fd_array[_i] == fd)
            return 1;
    return 0;
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);

#endif /* RVVM_MINGW_SYS_SELECT_H */