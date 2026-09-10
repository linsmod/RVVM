/*
 * sys/shm.h - MinGW shim (System V shared memory stubs, return ENOSYS).
 */

#ifndef RVVM_MINGW_SYS_SHM_H
#define RVVM_MINGW_SYS_SHM_H

#include <sys/types.h>

#ifndef key_t_defined_compat
typedef int key_t;
#endif

struct shmid_ds {
    int dummy;
};

int   shmget(key_t key, size_t size, int shmflg);
int   shmctl(int shmid, int cmd, struct shmid_ds* buf);
void* shmat(int shmid, const void* shmaddr, int shmflg);
int   shmdt(const void* shmaddr);

#endif /* RVVM_MINGW_SYS_SHM_H */