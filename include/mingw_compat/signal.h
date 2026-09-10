/*
 * signal.h - MinGW shim (POSIX signal numbers, sigset_t, siginfo_t,
 * struct sigaction). MinGW's signal.h only provides the C89 signals;
 * rvvm_user.c needs sigaction()/SA_SIGINFO/SIGBUS for its page-fault
 * handler. Implementations live in posix_shim.c (Vectored Exception
 * Handler based).
 */

#ifndef RVVM_MINGW_SIGNAL_H
#define RVVM_MINGW_SIGNAL_H

#include_next <signal.h>
#include <sys/types.h>
#include <errno.h>

/* POSIX signal numbers missing from MinGW (Linux numbering) */
#ifndef SIGHUP
#define SIGHUP      1
#endif
#ifndef SIGQUIT
#define SIGQUIT     3
#endif
#ifndef SIGTRAP
#define SIGTRAP     5
#endif
#ifndef SIGBUS
#define SIGBUS      7
#endif
#ifndef SIGKILL
#define SIGKILL     9
#endif
#ifndef SIGUSR1
#define SIGUSR1     10
#endif
#ifndef SIGUSR2
#define SIGUSR2     12
#endif
#ifndef SIGPIPE
#define SIGPIPE     13
#endif
#ifndef SIGALRM
#define SIGALRM     14
#endif
#ifndef SIGCHLD
#define SIGCHLD     17
#endif
#ifndef SIGCONT
#define SIGCONT     18
#endif
#ifndef SIGSTOP
#define SIGSTOP     19
#endif
#ifndef SIGTSTP
#define SIGTSTP     20
#endif
#define _NSIG       65

/* sigaction flags */
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#endif

/* sigprocmask how */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

#ifndef RVVM_SIGSET_DEFINED
#define RVVM_SIGSET_DEFINED
typedef struct {
    unsigned long long __bits[2];   /* enough for 64 signals */
} sigset_t;
#endif

typedef struct siginfo {
    int   si_signo;
    int   si_errno;
    int   si_code;
    void* si_addr;
    int   si_pad[28];
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t sa_mask;
    int      sa_flags;
};

static inline int sigemptyset(sigset_t* set)
{
    if (!set) { errno = EINVAL; return -1; }
    set->__bits[0] = 0;
    set->__bits[1] = 0;
    return 0;
}

static inline int sigfillset(sigset_t* set)
{
    if (!set) { errno = EINVAL; return -1; }
    set->__bits[0] = ~(unsigned long long)0;
    set->__bits[1] = ~(unsigned long long)0;
    return 0;
}

static inline int sigaddset(sigset_t* set, int sig)
{
    if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    set->__bits[(sig - 1) >> 6] |= 1ULL << ((sig - 1) & 63);
    return 0;
}

static inline int sigdelset(sigset_t* set, int sig)
{
    if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    set->__bits[(sig - 1) >> 6] &= ~(1ULL << ((sig - 1) & 63));
    return 0;
}

static inline int sigismember(const sigset_t* set, int sig)
{
    if (!set || sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    return (int)((set->__bits[(sig - 1) >> 6] >> ((sig - 1) & 63)) & 1);
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old);
int sigprocmask(int how, const sigset_t* set, sigset_t* old);

#endif /* RVVM_MINGW_SIGNAL_H */
