/*
 * sys/time.h - MinGW shim additions (itimerval/setitimer, nanosleep,
 * clock_gettime/clock_nanosleep, CLOCK_* constants). struct timeval and
 * gettimeofday come from the MinGW header via include_next.
 * Implementations live in posix_shim.c.
 */

#ifndef RVVM_MINGW_SYS_TIME_H
#define RVVM_MINGW_SYS_TIME_H

#include_next <sys/time.h>
#include <time.h>
#include <sys/types.h>

#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    long long tv_sec;
    long      tv_nsec;
};
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME              0
#define CLOCK_MONOTONIC             1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID     3
#define CLOCK_MONOTONIC_RAW         4
#define CLOCK_REALTIME_COARSE       5
#define CLOCK_MONOTONIC_COARSE      6
#define CLOCK_BOOTTIME              7
#endif

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

#ifndef ITIMER_REAL
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
#endif

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

int setitimer(int which, const struct itimerval* newval, struct itimerval* oldval);
int getitimer(int which, struct itimerval* cur);

/* Note: clock_gettime(), nanosleep() and clock_nanosleep() are NOT declared
 * here - winpthreads' pthread_time.h already defines them as static inlines
 * (pulled in via <time.h>), and redeclaring or reimplementing them here
 * causes redefinition errors. */

#endif /* RVVM_MINGW_SYS_TIME_H */
