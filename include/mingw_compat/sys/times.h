/*
 * sys/times.h - MinGW shim (times, uses wall clock).
 */

#ifndef RVVM_MINGW_SYS_TIMES_H
#define RVVM_MINGW_SYS_TIMES_H

#include <sys/types.h>

#define CLK_TCK 100

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

clock_t times(struct tms* buf);

#endif /* RVVM_MINGW_SYS_TIMES_H */