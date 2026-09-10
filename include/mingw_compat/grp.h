/*
 * grp.h - MinGW shim (getgroups/setgroups stubs).
 */

#ifndef RVVM_MINGW_GRP_H
#define RVVM_MINGW_GRP_H

#include <sys/types.h>

/* uid_t/gid_t are missing from MinGW entirely (shared guard with
 * unistd.h/sys/stat.h shims) */
#ifndef _RVVM_COMPAT_UIDGID
#define _RVVM_COMPAT_UIDGID
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t* list);

#endif /* RVVM_MINGW_GRP_H */