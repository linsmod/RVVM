/*
 * sched.h - MinGW shim (cpu_set_t + sched_getaffinity; winpthreads'
 * sched.h only provides sched_yield()).
 */

#ifndef RVVM_MINGW_SCHED_H
#define RVVM_MINGW_SCHED_H

#include_next <sched.h>

#include <sys/types.h>
#include <stddef.h>
#include <string.h>

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

#ifndef RVVM_CPUSET_DEFINED
#define RVVM_CPUSET_DEFINED
typedef struct {
    unsigned long long __bits[CPU_SETSIZE / 64];
} cpu_set_t;
#endif

#define CPU_ZERO(cpuset)        memset((cpuset), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, cpuset)    ((cpuset)->__bits[(cpu) >> 6] |= 1ULL << ((cpu) & 63))
#define CPU_CLR(cpu, cpuset)    ((cpuset)->__bits[(cpu) >> 6] &= ~(1ULL << ((cpu) & 63)))
#define CPU_ISSET(cpu, cpuset)  (int)(((cpuset)->__bits[(cpu) >> 6] >> ((cpu) & 63)) & 1)

static inline int CPU_COUNT(const cpu_set_t* cpuset)
{
    int count = 0;
    size_t i;
    for (i = 0; i < sizeof(cpuset->__bits) / sizeof(cpuset->__bits[0]); i++) {
        unsigned long long m = cpuset->__bits[i];
        while (m) {
            m &= m - 1;
            count++;
        }
    }
    return count;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask);

#endif /* RVVM_MINGW_SCHED_H */
