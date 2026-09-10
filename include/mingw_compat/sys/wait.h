/*
 * sys/wait.h - MinGW shim (wait4/waitpid stubs, idle loop should
 * yield CPU and report no children).
 */

#ifndef RVVM_MINGW_SYS_WAIT_H
#define RVVM_MINGW_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WIFSIGNALED(status) (!WIFEXITED(status))

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait4(pid_t pid, int* status, int options, void* rusage);

static inline pid_t wait3(int* status, int options, void* rusage) {
    return wait4(-1, status, options, rusage);
}

#endif /* RVVM_MINGW_SYS_WAIT_H */