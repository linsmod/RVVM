/*
rvvm_user.c - RVVM Linux binary emulator
Copyright (C) 2024  LekKit <github.com/LekKit>
              2023  nebulka1

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * This thing is hugely WIP, altho it runs static binaries fairly well.
 * With dynamic binaries, it usually either crashes in ld-linux.so, or
 * in random executable locations... Debugging this is a nightmare.
 *
 * Upon debugging this thing, I figured it would be a good idea to remove
 * CPU/syscall emulation out of the equation, so simply test a combination
 * of ELF loader + stack setup thingy. So there is a native x86_64 jump_start()
 * implementation to do exactly that
 *
 * Some helpful resources:
 *   https://jborza.com/post/2021-05-11-riscv-linux-syscalls/
 *   https://gpages.juszkiewicz.com.pl/syscalls-table/syscalls.html
 *
 * Now if we ever get this to work, here are some interesting further goals:
 *
 * - Implement some kind of fake /usr overlay so you can still run RISC-V
 *   binaries without chroot, and without putting RISC-V libs into your system
 *
 * - Allow to run Linux binaries on non-Linux host to some degree. The ELF
 *   loader runs even on Windows (lol), so do the rest of RVVM abstractions.
 *   So maybe for many simple syscalls we can do exactly that, at least on Mac/BSD..
 *
 * - Ask some guys that use qemu-user for build system purposes to try out rvvm-user :D
 *   With some local JIT patches rvvm-user already beats qemu-user on statically
 *   built benches
 */

#include "feature_test.h"

//#define RVVM_USER_TEST
//#define RVVM_USER_TEST_X86
//#define RVVM_USER_TEST_RISCV

// Guard this for now
#if defined(RVVM_USER_TEST)

// Guest fd numbers passed through the syscall dispatch are not host fds,
// GCC static analyzer (-fanalyzer) misinterprets them as leaked descriptors
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#pragma GCC diagnostic ignored "-Wanalyzer-fd-use-after-close"
#pragma GCC diagnostic ignored "-Wanalyzer-fd-double-close"
#pragma GCC diagnostic ignored "-Wanalyzer-fd-access-mode-mismatch"
#endif

#include <stdio.h>

#include <errno.h>
#include <unistd.h>

#include <time.h>     // clock_gettime(), etc
#include <signal.h>   // sigaction(), etc

#include <sched.h>    // sched_getaffinity()

#include <fcntl.h>    // O_RDONLY

#include <sys/types.h>
#include <sys/param.h>

#include <sys/time.h> // setitimer()

// File stuff
#include <dirent.h>    // readdir(), etc
#include <sys/file.h>  // fcntl(), flock(), fallocate(), openat()
#include <sys/uio.h>   // readv(), writev()
#include <sys/stat.h>  // mknodat(), mkdirat(), fchmod(), fchmodat(), fstatat(), fstat(), umask(), statx()
#include <sys/mount.h> // struct statfs

// VMA manipulation
#include <sys/mman.h> // mmap(), munmap(), mprotect()
#include <sys/shm.h>  // shmget(), shmctl(), shmat(), shmdt()

// Sockets
#include <sys/socket.h>
#include <sys/ioctl.h>  // ioctl()
#include <sys/select.h> // select()
#include <poll.h>       // poll()

// Misc
#include <sys/times.h>    // times()
#include <sys/wait.h>     // wait4()
#include <sys/resource.h> // getrusage()
#include <grp.h>          // setgroups()

// Event notification: native epoll on Linux, emulated over poll() on win32
// (see src/win/posix_shim.c)
#if defined(__linux__) || defined(_WIN32)
#include <sys/epoll.h>   // epoll_create1(), epoll_ctl(), epoll_wait()
#endif

// Linux-specific stuff
#ifdef __linux__
#include <sys/eventfd.h> // eventfd()
#include <sys/sysinfo.h> // sysinfo()
#include <sys/fsuid.h>   // setfsuid(), setfsgid()
#include <sys/vfs.h>     // struct statfs

// Put syscall headers here
#include <linux/futex.h> // FUTEX_*
#include <linux/stat.h>  // struct statx
#include <sys/syscall.h> // SYS_*
#include <unistd.h>
#endif

#include "rvvm_user.h" // rvvm_user_io_callback typedef (this file's own public header)
#include "rvvmlib.h"
#include "elf_load.h"
#include "mem_ops.h"
#include "utils.h"
#include "blk_io.h"
#include "threading.h"
#include "vma_ops.h"
#include "spinlock.h"
#include "rvtimer.h"
#include "stacktrace.h"
#include "../cpu/riscv_hart.h" // riscv_hart_queue_pause() for clean userland shutdown

/* Android NDK API Proxy - vp_cmdpost integration (single copy lives in src/virtpass) */
#include "virtpass/vp_cmdpost.h"

#if defined(ANDROID)
#include <android/log.h>

// Android-specific I/O callback using logcat
ssize_t android_io_callback(int fd, const void* buf, size_t count)
{
    if (fd == 1 || fd == 2) { // stdout or stderr
        const char* data = (const char*)buf;
        if (data && count) {
            char sbuf[1024];
            size_t copy = count < sizeof(sbuf) - 1 ? count : sizeof(sbuf) - 1;
            memcpy(sbuf, data, copy);
            sbuf[copy] = '\0';
            __android_log_print(ANDROID_LOG_INFO, "RVVM-GUEST", "%s", sbuf);

            /* Forward the raw bytes to the JNI console bridge, which buffers
             * them into lines for the app UI and its log file. */
            extern void jni_guest_output(const char* data, size_t count);
            jni_guest_output(data, count);
        }
        return count;
    } else {
        // For other file descriptors, use default write
        return write(fd, buf, count);
    }
}
#endif

#define RVVM_USER_RISCV64

#ifdef RVVM_USER_RISCV64
typedef uint64_t uapi_size_t;
typedef uint64_t uapi_ulong_t;
typedef int64_t  uapi_long_t;
#else
typedef uint32_t uapi_size_t;
typedef uint32_t uapi_ulong_t;
typedef int32_t  uapi_long_t;
#endif

#define UAPI_PATH_MAX 4096

/*
 * Guest-visible errno values (riscv64 Linux UAPI).
 *
 * These are deliberately spelled out instead of being taken from <errno.h>:
 * the emulator host may number its errors differently (BSD/macOS puts EAGAIN at
 * 35, win32 puts the whole socket family at 100+), so the two sets must never be
 * mixed. Anything that leaves a host call has to be translated first - see
 * host_to_uapi_errno().
 */
#define UAPI_EPERM           1
#define UAPI_ENOENT          2
#define UAPI_ESRCH           3
#define UAPI_EINTR           4
#define UAPI_EIO             5
#define UAPI_ENXIO           6
#define UAPI_E2BIG           7
#define UAPI_ENOEXEC         8
#define UAPI_EBADF           9
#define UAPI_ECHILD          10
#define UAPI_EAGAIN          11
#define UAPI_EWOULDBLOCK     11 // Alias of EAGAIN
#define UAPI_ENOMEM          12
#define UAPI_EACCESS         13
#define UAPI_EFAULT          14
#define UAPI_ENOTBLK         15
#define UAPI_EBUSY           16
#define UAPI_EEXIST          17
#define UAPI_EXDEV           18
#define UAPI_ENODEV          19
#define UAPI_ENOTDIR         20
#define UAPI_EISDIR          21
#define UAPI_EINVAL          22
#define UAPI_ENFILE          23
#define UAPI_EMFILE          24
#define UAPI_ENOTTY          25
#define UAPI_ETXTBSY         26
#define UAPI_EFBIG           27
#define UAPI_ENOSPC          28
#define UAPI_ESPIPE          29
#define UAPI_EROFS           30
#define UAPI_EMLINK          31
#define UAPI_EPIPE           32
#define UAPI_EDOM            33
#define UAPI_ERANGE          34
#define UAPI_EDEADLK         35
#define UAPI_ENAMETOOLONG    36
#define UAPI_ENOLCK          37
#define UAPI_ENOSYS          38
#define UAPI_ENOTEMPTY       39
#define UAPI_ELOOP           40
#define UAPI_ENOMSG          42
#define UAPI_EIDRM           43
#define UAPI_ENOSTR          60
#define UAPI_ENODATA         61
#define UAPI_ETIME           62
#define UAPI_ENOSR           63
#define UAPI_ENOLINK         67
#define UAPI_EPROTO          71
#define UAPI_EBADMSG         74
#define UAPI_EOVERFLOW       75
#define UAPI_EILSEQ          84
#define UAPI_ERESTART        85
#define UAPI_ESTRPIPE        86
#define UAPI_EUSERS          87
#define UAPI_ENOTSOCK        88
#define UAPI_EDESTADDRREQ    89
#define UAPI_EMSGSIZE        90
#define UAPI_EPROTOTYPE      91
#define UAPI_ENOPROTOOPT     92
#define UAPI_EPROTONOSUPPORT 93
#define UAPI_ESOCKTNOSUPPORT 94
#define UAPI_EOPNOTSUPP      95
#define UAPI_ENOTSUP         95 // Alias of EOPNOTSUPP
#define UAPI_EPFNOSUPPORT    96
#define UAPI_EAFNOSUPPORT    97
#define UAPI_EADDRINUSE      98
#define UAPI_EADDRNOTAVAIL   99
#define UAPI_ENETDOWN        100
#define UAPI_ENETUNREACH     101
#define UAPI_ENETRESET       102
#define UAPI_ECONNABORTED    103
#define UAPI_ECONNRESET      104
#define UAPI_ENOBUFS         105
#define UAPI_EISCONN         106
#define UAPI_ENOTCONN        107
#define UAPI_ESHUTDOWN       108
#define UAPI_ETOOMANYREFS    109
#define UAPI_ETIMEDOUT       110
#define UAPI_ECONNREFUSED    111
#define UAPI_EHOSTDOWN       112
#define UAPI_EHOSTUNREACH    113
#define UAPI_EALREADY        114
#define UAPI_EINPROGRESS     115
#define UAPI_ESTALE          116
#define UAPI_EDQUOT          122
#define UAPI_ENOMEDIUM       123
#define UAPI_EMEDIUMTYPE     124
#define UAPI_ECANCELED       125
#define UAPI_EOWNERDEAD      130
#define UAPI_ENOTRECOVERABLE 131

// RISC-V UAPI struct definitions & conversions
struct uapi_new_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

// Guest iovec: iov_base is a *guest* address. Guest and host are both LP64 so
// the layout matches, but the pointer values do not (see rvvm_iovec_from_guest).
struct uapi_iovec {
    uapi_ulong_t base;
    uapi_ulong_t len;
};

// Guest msghdr: msg_name / msg_iov / msg_control are guest addresses
struct uapi_msghdr {
    uapi_ulong_t name;
    uint32_t     namelen;
    uint32_t     _pad;
    uapi_ulong_t iov;
    uapi_ulong_t iovlen;
    uapi_ulong_t control;
    uapi_ulong_t controllen;
    uint32_t     flags;
    uint32_t     _pad2;
};

struct uapi_stat {
    uapi_ulong_t dev;
    uapi_ulong_t ino;
    uint32_t     mode;
    uint32_t     nlink;
    uint32_t     uid;
    uint32_t     gid;
    uapi_ulong_t rdev;
    uapi_ulong_t pad1;
    uapi_long_t  size;
    int32_t      blksize;
    int32_t      pad2;
    uapi_long_t  blocks;
    uapi_long_t  atime;
    uapi_ulong_t atime_nsec;
    uapi_long_t  mtime;
    uapi_ulong_t mtime_nsec;
    uapi_long_t  ctime;
    uapi_ulong_t ctime_nsec;
    uint32_t     unused4;
    uint32_t     unused5;
};

struct uapi_statfs64 {
    uapi_size_t type;
    uapi_size_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    struct {
        int32_t val[2];
    } fsid;
    uapi_size_t namelen;
    uapi_size_t frsize;
    uapi_size_t flags;
    uapi_size_t spare[4];
};

struct uapi_sigaction {
    uapi_size_t  handler;
    uapi_ulong_t mask;
    uapi_ulong_t flags;
};

struct uapi_sigaltstack {
    uapi_size_t sp;
    int32_t flags;
    uapi_size_t size;
};

struct uapi_sched_param {
    int32_t sched_priority;
};

struct uapi_cap_data_struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

union uapi_epoll_data {
    uapi_size_t ptr;
    int32_t     fd;
    uint32_t    u32;
    uint64_t    u64;
};

struct uapi_epoll_event {
    uint32_t event;
    union uapi_epoll_data data;
};

struct uapi_timeval32 {
    uapi_long_t tv_sec;
    uapi_long_t tv_usec;
};

struct uapi_timespec32 {
    uapi_long_t tv_sec;
    uapi_long_t tv_nsec;
};

struct uapi_timespec {
    uint64_t tv_sec;
    uint64_t tv_nsec;
};

struct uapi_timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

struct uapi_itimerval {
    struct uapi_timeval it_interval;
    struct uapi_timeval it_value;
};

BUILD_ASSERT(sizeof(struct uapi_timespec) == 16);
BUILD_ASSERT(sizeof(struct uapi_timeval) == 16);
BUILD_ASSERT(sizeof(struct uapi_itimerval) == 32);

struct uapi_pollfd {
    int32_t  fd;
    uint16_t events;
    uint16_t revents;
};

struct uapi_linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[0];
};

#ifdef __riscv

BUILD_ASSERT(sizeof(struct uapi_stat) == sizeof(struct stat));
BUILD_ASSERT(sizeof(struct uapi_statfs64) == sizeof(struct statfs));

#endif

#ifdef __linux__
// struct statx is a fixed-width kernel UAPI layout (every field is __u32/__u64),
// so the guest's view of the buffer is byte-identical to the host's and statx()
// needs no field conversion - unlike struct stat, which is architecture-specific.
BUILD_ASSERT(sizeof(struct statx) == 256);
BUILD_ASSERT(sizeof(struct statx_timestamp) == 16);
#endif

static void uapi_stat_convert(struct uapi_stat* dst, const struct stat* src)
{
    dst->dev = src->st_dev;
    dst->ino = src->st_ino;
    dst->mode = src->st_mode;
    dst->nlink = src->st_nlink;
    dst->uid = src->st_uid;
    dst->gid = src->st_gid;
    dst->rdev = src->st_rdev;
    dst->pad1 = 0;
    dst->size = src->st_size;
    dst->blksize = src->st_blksize;
    dst->pad2 = 0;
    dst->blocks = src->st_blocks;
    dst->atime = src->st_atime;
    dst->atime_nsec = 0;
    dst->mtime = src->st_mtime;
    dst->mtime_nsec = 0;
    dst->ctime = src->st_ctime;
    dst->ctime_nsec = 0;
}

static void uapi_statfs64_convert(struct uapi_statfs64* dst, const struct statfs* src)
{
    dst->type = src->f_type;
    dst->bsize = src->f_bsize;
    dst->blocks = src->f_blocks;
    dst->bfree = src->f_bfree;
    dst->bavail = src->f_bavail;
    dst->files = src->f_files;
    dst->ffree = src->f_ffree;
    memcpy(&dst->fsid, &src->f_fsid, sizeof(dst->fsid));
    dst->namelen = 256;
    dst->frsize = src->f_bsize;
    dst->flags = src->f_flags;
}

/*
static void uapi_sigaction_convert(struct uapi_sigaction* dst, const struct sigaction* src)
{
    dst->handler = src->sa_handler;
    dst->flags = src->sa_flags;
    memcpy(&dst->mask, &src->sa_flags, sizeof(dst->mask));
}
*/

/*
 * The machine whose guest memory the calling thread is running in, or NULL when
 * it is not running a guest at all (the RVVM_USER_TEST* native modes, where
 * guest == host).
 *
 * This used to be a file-scope global written by rvvm_user_linux_ex(), and it
 * made the whole module a singleton: with a second guest in the process, one
 * instance's syscalls would translate their pointers through the other
 * instance's memory. Guest memory is a private buffer per machine, so that is
 * not merely the wrong byte range - it is the wrong buffer, and any address
 * that happened to fall inside the other one would read or write that guest's
 * memory instead. The per-thread context already knows its machine (see uctx(),
 * bound on entry to every guest thread), so the translation helpers take it
 * from there.
 *
 * Defined next to uctx() below; declared here because these helpers come first.
 */
static rvvm_machine_t* cur_machine(void);

// Short cast rvvm_addr_t -> void*
static void* to_ptr(rvvm_addr_t addr)
{
    rvvm_machine_t* machine = cur_machine();

    if (!machine) {
        // RVVM_USER_TEST* modes run the guest natively, guest == host
        return (void*)(size_t)addr;
    }
    // Guest RAM is exactly [mem.addr, mem.addr + mem.size). Below it sits the
    // guest's unmapped NULL page (callers test the result for NULL, so a bare
    // offset must not make addr 0 look valid), above it there is nothing at all:
    // rejecting the upper end keeps a bogus guest address from turning into a
    // wild pointer into the host's own address space.
    if (addr < machine->mem.addr || (addr - machine->mem.addr) >= machine->mem.size) {
        return NULL;
    }
    return ((uint8_t*)machine->mem.data) + (addr - machine->mem.addr);
}

// Range-checked variant: the whole [addr, addr + size) window must lie inside
// guest RAM. Use it wherever the host itself touches guest memory (copy, zero,
// struct write) or hands a buffer to host code, so a bogus guest pointer fails
// with -EFAULT instead of corrupting the host address space.
static void* to_ptr_sz(rvvm_addr_t addr, size_t size)
{
    rvvm_machine_t* machine = cur_machine();

    if (!machine) {
        return (void*)(size_t)addr;
    }
    if (addr < machine->mem.addr) {
        return NULL;
    }
    rvvm_addr_t off = addr - machine->mem.addr;
    if (size > machine->mem.size || off > machine->mem.size - size) {
        return NULL;
    }
    return ((uint8_t*)machine->mem.data) + off;
}

// Short cast rvvm_addr_t -> const char*
static const char* to_str(rvvm_addr_t addr)
{
    return (const char*)to_ptr(addr);
}

// Host pointer inside guest memory -> rvvm_addr_t
static rvvm_addr_t to_addr(const void* ptr)
{
    rvvm_machine_t* machine = cur_machine();

    if (!machine) {
        return (rvvm_addr_t)(size_t)ptr;
    }
    return (rvvm_addr_t)(((const uint8_t*)ptr) - ((const uint8_t*)machine->mem.data)) + machine->mem.addr;
}

#include <core/rvvm_user.h>

PUBLIC void* rvvm_user_guest_ptr(uint64_t addr)
{
    return to_ptr((rvvm_addr_t)addr);
}

PUBLIC uint64_t rvvm_user_host_ptr(const void* ptr)
{
    rvvm_machine_t* machine;

    if (!ptr) {
        return 0;
    }
    machine = cur_machine();
    if (!machine) {
        return (uint64_t)(size_t)ptr;
    }
    const uint8_t* data = (const uint8_t*)machine->mem.data;
    const uint8_t* p    = (const uint8_t*)ptr;
    if (p < data || (size_t)(p - data) >= machine->mem.size) {
        return 0;
    }
    return machine->mem.addr + (uint64_t)(p - data);
}

/* ============================================================
 * Guest virtual memory allocator
 *
 * Guest memory is a private buffer owned by the userland machine, so brk() and
 * mmap() carve ranges out of it instead of calling into the host VMA layer:
 * the host address of a guest mapping means nothing to the guest, and the MMU
 * can only serve guest memory from that one contiguous buffer.
 * ============================================================ */

// Past the guest image and its brk heap, where mmap()ed ranges start
#define GUEST_MMAP_BASE  0x11000000UL
#define GUEST_STACK_SIZE 0x4000000UL // 64 MiB
#define GUEST_PAGE_SIZE  0x1000UL

// Guest address a relocatable (ET_DYN) main image is placed at: above the NULL
// page and far below the mmap area, so the brk heap still fits in between (see
// rvvm_user_linux_ex()). A PIE executable or a bare .so launched as the guest
// program lands here; ET_EXEC images carry their own link-time address and
// ignore this.
#define GUEST_DYN_BASE   0x400000UL

typedef struct {
    rvvm_addr_t addr;
    size_t      size;
} guest_range_t;

#define GUEST_FREE_MAX 64

typedef struct {
    rvvm_hart_t* cpu;
    uint32_t* child_settid;
    uint32_t* child_cleartid;
    uint32_t tid;
    uint32_t finished; // Set by userland shutdown to stop this vCPU
} rvvm_user_thread_t;

/* ============================================================
 * Userland instance context
 *
 * This module keeps all of its per-guest state on file-scope statics, which
 * makes it a de-facto singleton: a second guest would fight the first one for
 * the address-space allocator, the thread registry and the callbacks.
 *
 * rvvm_userland_t is where that state belongs instead - one instance per
 * rvvm_machine_t, attached through the machine's userdata slot. The fields
 * mirror the file-scope globals one-for-one, so migrating them is a mechanical
 * rename rather than a redesign.
 *
 * Staged migration: the struct is introduced and attached to the machine here,
 * while the globals it shadows are still the ones in use, so this step changes
 * no behaviour. Later steps move one group at a time into the context.
 * ============================================================ */

// Virtual TTY input ring (cooked bytes waiting for the guest) and the
// canonical line buffer under construction. Both are small: this is an
// interactive console, not a pipe.
#define TTY_IN_RING 4096
#define TTY_IN_LINE 1024

typedef struct rvvm_userland {
    // Machine this context belongs to; identical to rvvm_machine_t::userdata
    rvvm_machine_t* machine;

    // --- Configuration & callbacks (group A) ---
    rvvm_user_io_callback    io_callback;
    rvvm_user_exit_callback  exit_callback;
    const char*              prefix_path;
    // Prefix set through rvvm_user_set_prefix(): the string is owned here and
    // the RVVM_USER_PREFIX environment must not override it
    char*                    prefix_owned;
    bool                     prefix_forced;
    bool                     fake_root;
    int                      fake_uid;
    int                      fake_gid;

    // --- Guest virtual memory allocator (group B) ---
    spinlock_t    guest_lock;
    guest_range_t guest_free[GUEST_FREE_MAX];
    size_t        guest_free_num;
    rvvm_addr_t   guest_bump;
    rvvm_addr_t   guest_mmap_end;
    rvvm_addr_t   guest_stack_base;
    rvvm_addr_t   guest_stack_top;
    rvvm_addr_t   guest_brk_start;
    rvvm_addr_t   guest_brk_end;
    rvvm_addr_t   guest_brk_ptr;
    spinlock_t    brk_lock;

    // --- Guest process image (group C) ---
    struct uapi_sigaction siga[64];
    elf_desc_t            elf;
    elf_desc_t            interp;
    // Host paths of the two images above, for crash symbolization
    // (proc_symbolize() via RVVM_ADDR2LINE). Per instance: an addr2line lookup
    // without the right image resolves frames into the wrong program entirely.
    char                  main_elf_path[UAPI_PATH_MAX];
    char                  interp_elf_path[UAPI_PATH_MAX];

    // --- Thread registry & lifecycle (group D) ---
    spinlock_t                    userland_threads_lock;
    vector_t(rvvm_user_thread_t*) userland_threads;
    rvvm_user_thread_t*           userland_main_thread;
    uint32_t                      userland_exit_reported;
    uint32_t                      userland_suspend;
    uint32_t                      userland_parked;

    // --- Guest virtual TTY (libvterm, optional) ---
    // When tty_cb is set, guest writes to fd 1/2 are parsed by libvterm and the
    // host renders the screen itself instead of writing raw bytes to a tty.
    //
    // The console is a host-owned *session* (rvvm_tty_t, defined below): the
    // session holds the VTerm and the lock that serializes every access to it,
    // and it outlives this machine - that is what lets the host keep rendering
    // (and scrolling through) the last screen after the guest is gone.
    // rvvm_tty_attach() installs the host's session here; when the host attaches
    // none, the first write creates an internal one (tty_owned) so fd 1/2 still
    // parses into a screen for a host that only registered a callback.
    rvvm_tty_t*              tty;
    bool                     tty_owned;     // internally created, free with the machine
    rvvm_user_tty_callback   tty_cb;
    void*                    tty_userdata;

    // --- Guest virtual TTY input (group E) ---
    // The keyboard side of the console. The host hands us the bytes a real
    // terminal would receive (rvvm_user_tty_input); they run through the line
    // discipline the guest's termios advertises (ICRNL, ICANON line assembly
    // with erase, ECHO) and the cooked result is what read(0, ...) returns.
    spinlock_t   tty_in_lock;      // guards the ring + pending line below
    rvvm_event_t tty_in_event;     // wakes a read(0) blocked with nothing to read
    uint8_t      tty_cooked[TTY_IN_RING];
    size_t       tty_cooked_head;  // read cursor into the ring
    size_t       tty_cooked_len;   // bytes waiting for the guest
    uint8_t      tty_line[TTY_IN_LINE];
    size_t       tty_line_len;     // canonical line under construction
    uint32_t     tty_lflag;        // c_lflag from the last TCGETS/TCSETS
    uint32_t     tty_eof_pending;  // Ctrl-D delivered: next read returns 0
    bool         tty_in_eof;       // teardown: unblock reads that are waiting
} rvvm_userland_t;

// Context attached to a userland machine, NULL for a non-userland machine
static inline rvvm_userland_t* rvvm_userland_ctx(rvvm_machine_t* machine)
{
    return machine ? (rvvm_userland_t*)machine->userdata : NULL;
}

/*
 * Guest virtual TTY backed by libvterm.
 *
 * Guest writes to fd 1/2 are fed to a VTerm instance which maintains the screen
 * matrix (handling '\r', ANSI escapes, scrolling, ...). The host renders the
 * screen via the tty callback it registered. The rendering host is responsible
 * for calling vterm_screen_flush_damage() and reading cells; here we just push
 * bytes in and notify the host that new output arrived.
 */
#include <vterm.h>

/* Default grid of a terminal session whose opener did not pick one, and of the
 * internal session created when no host attached one (see user_tty_init). The
 * grid of an attached session belongs to whoever opened it - the Android
 * console, for instance, follows its viewport height - so these two are then
 * only the fallback answers for "no session attached". */
#define VTERM_ROWS 24
#define VTERM_COLS 80

/* ============================================================
 * rvvm_tty - host-owned terminal session
 * ============================================================
 * The screen matrix and the lock that serializes every access to it live here,
 * NOT on the machine, and that split is the point: a machine *is* the guest's
 * lifetime (fd 1/2 parsing, the line discipline, the winsize ioctl all end with
 * the run), while the console is the host's - it keeps rendering the frozen
 * last screen, and scrolling through it, with no machine left to ask.
 *
 * A host opens one session for the lifetime of its console, attaches it before
 * each run and detaches it once the guest thread is gone:
 *
 *     rvvm_tty_t* tty = rvvm_tty_open(24, 80);
 *     rvvm_tty_attach(tty, machine);      // guest fd 1/2 now parses into it
 *     ... rvvm_user_linux_ex() ...
 *     rvvm_tty_detach(tty, machine);      // screen, lock and size stay valid
 *
 * The session also owns everything a renderer draws: the packed cell grid
 * (rvvm_tty_snapshot), the scrollback ring behind it, the view position and the
 * repaint hint - so a host is left with the actual drawing and nothing else.
 * No machine pointer is involved in any of it, which is what lets a console be
 * rendered and scrolled after the guest exited (Ctrl-C included): exactly when
 * reading the output back matters most.
 *
 * Threading: the lock is the same one the core takes while parsing guest output
 * (user_tty_write) and echoing host input, so a host snapshot can never see a
 * half-parsed escape sequence. Attach/detach are not locked: they are the
 * host's start- and end-of-run points, where no guest thread is running.
 * ============================================================ */
/* History rows kept per session. The ring is allocated with the session, which
 * is the same budget the Android host used to keep in a static array of its
 * own. */
#define RVT_TTY_SB_LINES 1000

struct rvvm_tty {
    VTerm*       vt;
    VTermScreen* screen;    /* the screen of vt, for the packing callbacks */
    spinlock_t   lock;

    /* Scrollback: libvterm's sb_pushline is a notification - the row is handed
     * over and then forgotten - so a console that can be scrolled has to store
     * the lines itself. They go in already packed, the very cells a snapshot
     * hands out, which makes a window that reaches into history a copy rather
     * than a second rendering path. Ring of RVT_TTY_SB_LINES rows, each `cols`
     * cells wide; the oldest is at sb_count - 1. */
    rvvm_tty_cell_t* sb;
    int          cols;      /* grid width, fixed for the session's lifetime */
    int          sb_count;
    int          sb_next;

    /* View: where the snapshot window sits. It follows the live screen until a
     * host drags it back, and a view parked in history stays on the lines being
     * read while output keeps arriving (see rvvm_tty_sb_pushline). */
    int          scroll;
    bool         follow;

    /* Cursor visibility: libvterm's DECTCEM state (ESC [ ? 25 h / l), which a
     * full-screen guest toggles to park or hide the cursor. libvterm reports it
     * only through a screen callback, so it is recorded here. */
    bool         cursor_visible;

    /* Repaint hint. Written under the lock, read without it (a poller only ever
     * compares two values), hence the volatile. */
    volatile int serial;
};

/* The cell layout hosts read out of their own arrays (the Android renderer
 * hands its int[] straight to rvvm_tty_snapshot) must stay exactly 4 x 32 bits:
 * this fails to compile if it ever grows padding. */
typedef char rvvm_tty_cell_layout_check[sizeof(rvvm_tty_cell_t) == 16 ? 1 : -1];

/* One VTerm cell -> a packed rvvm_tty_cell_t. Both the live screen and the
 * scrollback rows are packed here, so a line looks the same the moment it
 * scrolls off the top and when it is later read back out of history. The cursor
 * bit belongs to the snapshot and is deliberately left out here. */
static void rvvm_tty_pack_cell(VTermScreen* scr, const VTermScreenCell* cell,
                               rvvm_tty_cell_t* out)
{
    uint32_t cp = cell->chars[0];
    uint32_t flags = 0;
    if (cp == (uint32_t)-1) {
        cp = 0; /* double-width gap: lead char already drawn */
    } else if (cp) {
        if (cell->attrs.bold)      flags |= RVT_TTY_BOLD;
        if (cell->attrs.underline) flags |= RVT_TTY_UNDERLINE;
        /* libvterm's own wcwidth covers CJK *and* emoji (U+1F300+); a
         * hand-rolled codepoint range list misses the latter and renders them
         * overlapping the next cell. */
        if (cell->width == 2)      flags |= RVT_TTY_WIDE;
    }
    /* Colors: resolve defaults and indexed palette to ARGB. */
    VTermColor fg = cell->fg, bg = cell->bg;
    if (VTERM_COLOR_IS_DEFAULT_FG(&fg)) {
        out->fg = 0xFFDCDCDC; /* light gray on black */
    } else {
        vterm_screen_convert_color_to_rgb(scr, &fg);
        out->fg = 0xFF000000u | ((uint32_t)fg.rgb.red << 16) |
                  ((uint32_t)fg.rgb.green << 8) | (uint32_t)fg.rgb.blue;
    }
    if (VTERM_COLOR_IS_DEFAULT_BG(&bg)) {
        out->bg = 0xFF000000; /* black */
    } else {
        vterm_screen_convert_color_to_rgb(scr, &bg);
        out->bg = 0xFF000000u | ((uint32_t)bg.rgb.red << 16) |
                  ((uint32_t)bg.rgb.green << 8) | (uint32_t)bg.rgb.blue;
    }
    if (cell->attrs.reverse) {
        uint32_t t = out->fg; out->fg = out->bg; out->bg = t;
        flags |= RVT_TTY_REVERSE;
    }
    out->cp    = cp;
    out->flags = flags;
}

/* A blank cell of the session's shape: what a scrollback row is padded with
 * when libvterm hands over fewer columns than the session is wide, and what a
 * window reading past the stored history gets. */
static void rvvm_tty_blank_cell(rvvm_tty_cell_t* out)
{
    out->cp    = 0;
    out->fg    = 0xFFDCDCDC;
    out->bg    = 0xFF000000;
    out->flags = 0;
}

/* Reached from the guest thread while it parses its own output, with the
 * session lock held (the core holds it across that write), never outside it. */
static int rvvm_tty_sb_pushline(int cols, const VTermScreenCell* cells, void* user)
{
    rvvm_tty_t* tty = user;
    rvvm_tty_cell_t* row = tty->sb + (size_t)tty->sb_next * tty->cols;
    for (int c = 0; c < tty->cols; c++) {
        if (c < cols) {
            rvvm_tty_pack_cell(tty->screen, &cells[c], row + c);
        } else {
            rvvm_tty_blank_cell(row + c);
        }
    }
    tty->sb_next = (tty->sb_next + 1) % RVT_TTY_SB_LINES;
    if (tty->sb_count < RVT_TTY_SB_LINES) {
        tty->sb_count++;
    }
    /* A view dragged back is anchored on the lines it is showing: one line
     * pushed is one more line between it and the live bottom, otherwise the
     * text would drift up under the reader on every output burst. */
    if (!tty->follow && tty->scroll < tty->sb_count) {
        tty->scroll++;
    }
    tty->serial++;
    return 1;
}

/* The guest asked for the scrollback to go away (CSI 3 J, a reset). It is gone,
 * so a view parked in it has nowhere to be but back on the live screen. */
static int rvvm_tty_sb_clear(void* user)
{
    rvvm_tty_t* tty = user;
    tty->sb_count = 0;
    tty->sb_next  = 0;
    tty->scroll   = 0;
    tty->follow   = true;
    tty->serial++;
    return 1;
}

/* Termprop notifications come from the guest thread (it parses the output that
 * carries them), so this only records the value. */
static int rvvm_tty_settermprop(VTermProp prop, VTermValue* val, void* user)
{
    rvvm_tty_t* tty = user;
    if (prop == VTERM_PROP_CURSORVISIBLE && val) {
        tty->cursor_visible = val->boolean;
    }
    return 1;
}

/* settermprop records cursor visibility; sb_pushline / sb_clear keep the
 * session's own scrollback. The rest must stay NULL: moverect_user() skips
 * damagerect() when a moverect callback answers, so hooking it would lose the
 * damage markings of every scrolled row and the host would stop repainting a
 * scrolling screen - and sb_popline is only asked for when libvterm shrinks a
 * screen, where this session drops the rows rather than storing them back. */
static const VTermScreenCallbacks rvvm_tty_screen_cbs = {
    .damage      = NULL,
    .moverect    = NULL,
    .movecursor  = NULL,
    .settermprop = rvvm_tty_settermprop,
    .bell        = NULL,
    .resize      = NULL,
    .sb_pushline = rvvm_tty_sb_pushline,
    .sb_popline  = NULL,
    .sb_clear    = rvvm_tty_sb_clear,
};

PUBLIC rvvm_tty_t* rvvm_tty_open(int rows, int cols)
{
    if (rows < 1) rows = VTERM_ROWS;
    if (cols < 1) cols = VTERM_COLS;

    rvvm_tty_t* tty = safe_new_obj(rvvm_tty_t);
    tty->vt = vterm_new(rows, cols);
    if (!tty->vt) {
        safe_free(tty);
        return NULL;
    }
    /* UTF-8 is OFF by default in libvterm; without it CJK/emoji get mangled
     * into latin1 before reaching the cells. */
    vterm_set_utf8(tty->vt, 1);

    tty->cols           = cols;
    tty->follow         = true;
    tty->cursor_visible = true;
    tty->sb             = safe_calloc((size_t)RVT_TTY_SB_LINES * cols,
                                      sizeof(rvvm_tty_cell_t));

    /* The screen carries the session as its callback data: sb_pushline and
     * sb_clear keep the scrollback, settermprop the cursor visibility, and the
     * serial is bumped from all of them. Installed before the first reset so
     * the screen starts from a known state. */
    tty->screen = vterm_obtain_screen(tty->vt);
    vterm_screen_set_callbacks(tty->screen, &rvvm_tty_screen_cbs, tty);
    vterm_screen_reset(tty->screen, true);
    return tty;
}

PUBLIC void rvvm_tty_close(rvvm_tty_t* tty)
{
    if (!tty) {
        return;
    }
    /* The lifetime rule belongs to the caller: close only once no guest is
     * attached, since a running guest parses its own output into this VTerm. */
    vterm_free(tty->vt);
    safe_free(tty->sb);
    safe_free(tty);
}

PUBLIC void rvvm_tty_lock(rvvm_tty_t* tty)
{
    if (tty) {
        spin_lock(&tty->lock);
    }
}

PUBLIC void rvvm_tty_unlock(rvvm_tty_t* tty)
{
    if (tty) {
        spin_unlock(&tty->lock);
    }
}

/* The libvterm instance behind the session. Every use has to be inside
 * rvvm_tty_lock() ... rvvm_tty_unlock(), the same lock the guest's own output
 * is parsed under. */
PUBLIC void* rvvm_tty_vterm(rvvm_tty_t* tty)
{
    return tty ? tty->vt : NULL;
}

/* Wipe the screen for a fresh run: the cells, the scrollback, the view and the
 * cursor state all belong to the run that just ended. Takes the lock itself;
 * called between runs, from the host's UI thread. */
PUBLIC void rvvm_tty_reset(rvvm_tty_t* tty)
{
    if (!tty) {
        return;
    }
    rvvm_tty_lock(tty);
    /* libvterm's screen reset does not re-announce the termprop (CURSORVISIBLE
     * is only reported on DECTCEM and DECRC), so a guest that hid its cursor
     * must not leave the next run without one. */
    tty->cursor_visible = true;
    tty->sb_count       = 0;
    tty->sb_next        = 0;
    tty->scroll         = 0;
    tty->follow         = true;
    vterm_screen_reset(tty->screen, true);
    tty->serial++;
    rvvm_tty_unlock(tty);
}

/* Resize the grid - the host viewport owns it. Takes the lock itself (the
 * renderer calls this from its layout path, with no lock held), and the guest
 * observes the new size through TIOCGWINSZ on its next query. */
PUBLIC void rvvm_tty_resize(rvvm_tty_t* tty, int rows, int cols)
{
    if (!tty || rows < 1) {
        return;
    }
    rvvm_tty_lock(tty);
    /* The column count is part of the session: the scrollback is stored that
     * wide, so a resize with a different cols is ignored rather than silently
     * re-striding history. A host that wants another width opens the session
     * with it. */
    if (cols != tty->cols) {
        cols = tty->cols;
    }
    vterm_set_size(tty->vt, rows, cols);
    tty->serial++;
    rvvm_tty_unlock(tty);
}

/* Grid size of the session. Caller holds the lock. */
PUBLIC void rvvm_tty_get_size(rvvm_tty_t* tty, int* rows, int* cols)
{
    int r = VTERM_ROWS, c = VTERM_COLS;
    if (tty) {
        vterm_get_size(tty->vt, &r, &c);
    }
    if (rows) *rows = r;
    if (cols) *cols = c;
}

/* Back to the live screen - anything that makes the lines being written the
 * ones worth showing (typing, a new run). Caller holds the lock. */
static void rvvm_tty_follow_locked(rvvm_tty_t* tty)
{
    if (!tty->follow || tty->scroll) {
        tty->follow = true;
        tty->scroll = 0;
        tty->serial++;
    }
}

PUBLIC void rvvm_tty_scroll(rvvm_tty_t* tty, int lines)
{
    if (!tty || !lines) {
        return;
    }
    rvvm_tty_lock(tty);
    int scroll = (tty->follow ? 0 : tty->scroll) + lines;
    if (scroll <= 0) {
        /* Dragged back past the live bottom: re-pin the view there. */
        tty->follow = true;
        scroll = 0;
    } else {
        if (scroll > tty->sb_count) {
            scroll = tty->sb_count;
        }
        tty->follow = false;
    }
    tty->scroll = scroll;
    tty->serial++;
    rvvm_tty_unlock(tty);
}

PUBLIC int rvvm_tty_serial(rvvm_tty_t* tty)
{
    return tty ? tty->serial : 0;
}

PUBLIC int rvvm_tty_snapshot(rvvm_tty_t* tty, rvvm_tty_cell_t* out, int out_cells,
                             rvvm_tty_view_t* view)
{
    if (!tty || !out || !view) {
        return 0;
    }

    rvvm_tty_lock(tty);
    int rows = 0, cols = 0;
    vterm_get_size(tty->vt, &rows, &cols);
    if (rows < 1 || cols < 1) {
        rvvm_tty_unlock(tty);
        return 0;
    }
    /* The view is filled even when the caller's buffer is too small, so it can
     * size the buffer from it and call again. */
    view->rows        = rows;
    view->cols        = cols;
    view->serial      = tty->serial;
    view->scroll      = 0;
    view->scrollback_lines = tty->sb_count;
    view->cursor_row  = -1;
    view->cursor_col  = -1;
    if (out_cells < rows * cols) {
        rvvm_tty_unlock(tty);
        return 0;
    }

    /* libvterm defers part of its screen state to the damage queue: pending
     * scroll/damage is only folded into the matrix here, and a cell read before
     * that can still be the previous generation of the screen. */
    vterm_screen_flush_damage(tty->screen);

    /* The window: the last `rows` lines of "history + screen", shifted up by the
     * scrollback offset, clamped to what is actually stored so a view parked in
     * history cannot ask for lines that have been recycled. */
    int scroll = tty->follow ? 0 : tty->scroll;
    if (scroll > tty->sb_count) scroll = tty->sb_count;
    if (scroll < 0) scroll = 0;
    /* The scrollback is stored tty->cols cells wide, so a grid of another width
     * cannot read it: only reachable if a host resized the VTerm behind the
     * session's back (rvvm_tty_resize pins the column count). */
    if (cols != tty->cols) {
        scroll = 0;
    }

    /* Where the guest's own cursor sits, read in the same locked pass as the
     * cells. -1/-1 means "no cursor to draw": the guest hid it (DECTCEM), the
     * VTerm has none yet, or the view is scrolled away from the live screen,
     * where the cursor is not on screen at all. */
    VTermPos cursor = { -1, -1 };
    if (!scroll && tty->cursor_visible) {
        vterm_state_get_cursorpos(vterm_obtain_state(tty->vt), &cursor);
    }

    for (int r = 0; r < rows; r++) {
        /* Output row r is line (r - scroll) counted from the live screen's top
         * row; a negative line is that many rows above it, in history. */
        int line = r - scroll;
        rvvm_tty_cell_t* rowout = out + (size_t)r * cols;
        if (line < 0) {
            int idx = tty->sb_count + line; /* -1 = the newest stored row */
            if (idx >= 0) {
                int slot = (tty->sb_next - tty->sb_count + idx + RVT_TTY_SB_LINES)
                           % RVT_TTY_SB_LINES;
                memcpy(rowout, tty->sb + (size_t)slot * tty->cols,
                       (size_t)cols * sizeof(*rowout));
            } else {
                for (int c = 0; c < cols; c++) {
                    rvvm_tty_blank_cell(rowout + c); /* never drawn */
                }
            }
            continue;
        }
        for (int c = 0; c < cols; c++) {
            VTermPos pos = { line, c };
            VTermScreenCell cell;
            memset(&cell, 0, sizeof(cell));
            vterm_screen_get_cell(tty->screen, pos, &cell);
            rvvm_tty_pack_cell(tty->screen, &cell, rowout + c);
            /* The cursor cell keeps its own colours: the renderer inverts them
             * for the block, so it needs the cell as the guest drew it. */
            if (line == cursor.row && c == cursor.col) {
                rowout[c].flags |= RVT_TTY_CURSOR;
            }
        }
    }

    view->scroll     = scroll;
    view->cursor_row = cursor.row;
    view->cursor_col = cursor.col;
    rvvm_tty_unlock(tty);
    return rows * cols;
}

/* Make @tty the console of @machine - the modern spelling of the old
 * rvvm_user_set_tty0(). Called before rvvm_user_linux_ex(). A session already
 * attached is replaced; only an internal one is freed here, since a host
 * session belongs to the host. */
PUBLIC void rvvm_tty_attach(rvvm_tty_t* tty, rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return;
    }
    if (ctx->tty && ctx->tty_owned) {
        rvvm_tty_close(ctx->tty);
    }
    ctx->tty       = tty;
    ctx->tty_owned = false;
}

/* Unhook the console from @machine. Only the pointer goes: the session keeps
 * its screen, its lock and its size, which is what keeps the last screen
 * renderable - and scrollable - after the run, Ctrl-C included. Call once the
 * guest thread has fully unwound; the core must not read the session after
 * this. */
PUBLIC void rvvm_tty_detach(rvvm_tty_t* tty, rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx || ctx->tty != tty) {
        return;
    }
    ctx->tty       = NULL;
    ctx->tty_owned = false;
}

static void user_tty_init(rvvm_userland_t* ctx)
{
    if (ctx->tty) {
        /* A session is attached: it owns the screen, there is nothing to make. */
        return;
    }
    /* No host session attached: build an internal one so fd 1/2 still parses
     * into a screen for a host that only registered a tty callback. */
    rvvm_tty_t* tty = rvvm_tty_open(VTERM_ROWS, VTERM_COLS);
    if (!tty) {
        return;
    }
    ctx->tty       = tty;
    ctx->tty_owned = true;
}

// Feed guest output on fd 1/2 through libvterm. No-op unless a session exists -
// either attached by the host via rvvm_tty_attach() or created on demand when a
// host registered a tty callback. This only mirrors the bytes into the screen
// matrix; the caller still forwards them to the host's io_callback / host fd,
// so both sinks stay live.
static void user_tty_write(rvvm_userland_t* ctx, int fd, const void* buf, size_t count)
{
    if (!ctx->tty && !ctx->tty_cb) {
        return;
    }
    user_tty_init(ctx);
    if (!ctx->tty || !buf || !count) {
        return;
    }
    /* ONLCR emulation: guest stdio emits bare LF, a real tty's line
     * discipline turns it into CRLF before the terminal sees it, and
     * libvterm's LF only moves down without returning the carriage. */
    const uint8_t* p = buf;
    size_t start = 0;
    rvvm_tty_t* tty = ctx->tty;
    /* The VTerm is also touched by host keyboard echo and by the host's screen
     * snapshot, so every access goes through the session's own lock. */
    spin_lock(&tty->lock);
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != '\n') {
            continue;
        }
        vterm_input_write(tty->vt, (const char*)p + start, i - start);
        vterm_input_write(tty->vt, "\r\n", 2);
        start = i + 1;
    }
    if (start < count) {
        vterm_input_write(tty->vt, (const char*)p + start, count - start);
    }
    tty->serial++;  /* there is a newer screen than the last snapshot */
    spin_unlock(&tty->lock);
    // Notify the host; it decides when to flush/render (throttling is its job).
    if (ctx->tty_cb) {
        ctx->tty_cb(ctx->tty_userdata, fd, tty->vt);
    }
}

// Write raw bytes into the VTerm with no line-discipline translation,
// serialized against the guest's own fd 1/2 output, then nudge the host
// renderer. Used for keyboard echo, where the caller already passes the exact
// screen content ("\r\n" for Enter, the back/blank/back sequence of
// user_tty_erase_echo() for erase, ...).
static void user_tty_vt_write(rvvm_userland_t* ctx, const char* buf, size_t len)
{
    if (!ctx->tty || !buf || !len) {
        return;
    }
    rvvm_tty_t* tty = ctx->tty;
    spin_lock(&tty->lock);
    vterm_input_write(tty->vt, buf, len);
    tty->serial++;  /* echo is screen output too */
    spin_unlock(&tty->lock);
    if (ctx->tty_cb) {
        ctx->tty_cb(ctx->tty_userdata, 1, tty->vt);
    }
}

/*
 * Minimal termios/winsize answers for the virtual TTY. Guest libc probes
 * fd 1/2 with ioctl(TCGETS) for isatty(); without an answer stdio fully
 * buffers stdout and nothing shows up until exit. Struct layouts and
 * ioctl numbers follow the asm-generic guest ABI, not the host's.
 */
#define UAPI_TCGETS     0x5401
#define UAPI_TCSETS     0x5402
#define UAPI_TCSETSW    0x5403
#define UAPI_TCSETSF    0x5404
#define UAPI_TIOCGWINSZ 0x5413
#define UAPI_TIOCSWINSZ 0x5414

/*
 * Termios bits the virtual TTY's input path acts on (asm-generic values, the
 * guest ABI's, not the host's). TTY_LFLAG_DEFAULT is the state TCGETS reports
 * before the guest changes anything and therefore what the line discipline
 * starts in - it must stay in sync with the TCGETS answer in user_tty_ioctl().
 */
#define TTY_IFLAG_ICRNL    0x0100  // input CR -> NL
#define TTY_LFLAG_ISIG     0x0001  // Ctrl-C: signal the foreground process
#define TTY_LFLAG_ICANON   0x0002  // canonical: assemble lines, handle erase
#define TTY_LFLAG_ECHO     0x0008  // echo typed characters
#define TTY_LFLAG_ECHOE    0x0010  // erase echoes as "\b \b"
#define TTY_LFLAG_ECHOK    0x0020  // kill echoes the line
#define TTY_LFLAG_IEXTEN   0x8000
#define TTY_LFLAG_DEFAULT  (TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | \
                            TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK | TTY_LFLAG_IEXTEN)

#define TTY_CC_VINTR  0x03  // Ctrl-C, the default VINTR
#define TTY_CC_ERASE  0x7F  // DEL, the default VERASE
#define TTY_CC_VEOF   0x04  // Ctrl-D

// asm-generic struct termios: 4 flag words + c_line + c_cc[32] + 2 speeds
typedef struct __attribute__((packed)) {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed, c_ospeed;
} uapi_termios_t;

typedef struct {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
} uapi_winsize_t;

// Defined further down (it belongs with the TLS it reads); the TTY ioctl and the
// line discipline need it before that.
static inline rvvm_userland_t* uctx(void);

static int64_t user_tty_ioctl(uint64_t cmd, void* arg)
{
    switch (cmd) {
        case UAPI_TCGETS: {
            uapi_termios_t t;
            memset(&t, 0, sizeof(t));
            t.c_iflag = TTY_IFLAG_ICRNL | 0x400; // ICRNL | IXON
            t.c_oflag = 0x1 | 0x4;          // OPOST | ONLCR (matches user_tty_write LF->CRLF)
            t.c_cflag = 0x30 | 0x80 | 0xF;  // CS8 | CREAD | B38400
            t.c_lflag = uctx()->tty_lflag;
            t.c_cc[6] = 1;                  // VMIN
            t.c_ispeed = t.c_ospeed = 0xF;  // B38400
            memcpy(arg, &t, sizeof(t));
            return 0;
        }
        case UAPI_TIOCGWINSZ: {
            /* Report the attached session's real grid rather than the
             * compile-time default: the host may have resized it to match its
             * viewport (the Android console derives its row count from the view
             * height while keeping the column count fixed), and a full-screen
             * guest lays itself out from this value - answering 24x80 while the
             * host shows, say, 60 rows would leave most of them permanently
             * blank. With no session attached this stays the built-in 24x80. */
            rvvm_userland_t* ctx = uctx();
            uapi_winsize_t ws = { VTERM_ROWS, VTERM_COLS, 0, 0 };
            if (ctx && ctx->tty) {
                int rows = VTERM_ROWS, cols = VTERM_COLS;
                rvvm_tty_lock(ctx->tty);
                rvvm_tty_get_size(ctx->tty, &rows, &cols);
                rvvm_tty_unlock(ctx->tty);
                if (rows > 0 && rows <= 0xFFFF) ws.ws_row = (uint16_t)rows;
                if (cols > 0 && cols <= 0xFFFF) ws.ws_col = (uint16_t)cols;
            }
            memcpy(arg, &ws, sizeof(ws));
            return 0;
        }
        case UAPI_TCSETS:
        case UAPI_TCSETSW:
        case UAPI_TCSETSF:
            // The guest's terminal modes are honoured for the flags that shape
            // the input path (ICANON / ECHO / ECHOE): that is what lets a
            // full-screen app turn off canonical mode and receive raw keys.
            // The remaining words stay fixed, matching TCGETS above.
            if (arg) {
                const uapi_termios_t* t = arg;
                uctx()->tty_lflag = t->c_lflag;
            }
            return 0;
        case UAPI_TIOCSWINSZ:
            // Window size changes: accepted and ignored. The grid belongs to
            // the host (it is sized to the host viewport, see TIOCGWINSZ
            // above), so a guest-requested resize does not move it.
            return 0;
    }
    return -UAPI_ENOTTY;
}

/* ============================================================
 * Guest virtual TTY input
 *
 * The keyboard half of the console. The host hands us the byte sequence a
 * real terminal receives (printable UTF-8, '\r' for Enter, 0x7F for
 * Backspace, ESC sequences for the arrows, ...); we run the line discipline
 * the guest's termios advertises and queue the cooked bytes for read(0).
 *
 * This lives here rather than in each host on purpose: the VTerm, the termios
 * answers and the output line discipline are already in this file, so both
 * the Android and the win32 host get identical console behaviour for free.
 * ============================================================ */

// Queue cooked bytes for the guest. Caller holds tty_in_lock. Returns the
// number of bytes actually queued (the ring may be full).
static size_t tty_cooked_push(rvvm_userland_t* ctx, const void* buf, size_t len)
{
    const uint8_t* p = buf;
    size_t space = TTY_IN_RING - ctx->tty_cooked_len;
    if (len > space) {
        len = space;
    }
    for (size_t i = 0; i < len; ++i) {
        size_t idx = (ctx->tty_cooked_head + ctx->tty_cooked_len) % TTY_IN_RING;
        ctx->tty_cooked[idx] = p[i];
        ctx->tty_cooked_len++;
    }
    return len;
}

// Drain up to len bytes out of the ring. Caller holds tty_in_lock.
static size_t tty_cooked_pop(rvvm_userland_t* ctx, void* buf, size_t len)
{
    uint8_t* p = buf;
    if (len > ctx->tty_cooked_len) {
        len = ctx->tty_cooked_len;
    }
    for (size_t i = 0; i < len; ++i) {
        p[i] = ctx->tty_cooked[ctx->tty_cooked_head];
        ctx->tty_cooked_head = (ctx->tty_cooked_head + 1) % TTY_IN_RING;
    }
    ctx->tty_cooked_len -= len;
    return len;
}

/*
 * Columns of screen the character just echoed occupies.
 *
 * Read back from the VTerm rather than from a width table of our own: libvterm
 * decides which cells a character fills (its wcwidth covers CJK and emoji, and
 * is not part of its public API), and the host renderer draws exactly those
 * cells, so a private table could disagree with both and leave half a
 * character behind. The cursor sits one column past the character, where the
 * cell under it answers the question; against the right margin libvterm parks
 * it as a phantom *on* the character instead (see putglyph() in state.c), and
 * there the start column gives the width away, a wide glyph being unable to
 * start at the last column.
 */
static int user_tty_erased_cols(rvvm_userland_t* ctx)
{
    if (!ctx->tty) {
        return 1;
    }
    /* Take the screen from the session's VTerm instead of a cached pointer: the
     * screen travels with the VTerm, and asking for it is what keeps the erase
     * echo honest for a session the host attached moments ago - a NULL screen
     * here would silently degrade every erase to a single column.
     * vterm_obtain_screen() just returns the existing screen for a VTerm that
     * has one. */
    VTerm*       vt  = ctx->tty->vt;
    VTermScreen* scr = vterm_obtain_screen(vt);
    VTermState*  st  = vterm_obtain_state(vt);
    int cols = 1;
    int trows = VTERM_ROWS, tcols = VTERM_COLS;

    spin_lock(&ctx->tty->lock);
    /* The right margin is wherever the VTerm currently ends: the host may have
     * resized it to its viewport, so the compile-time default is only the
     * fallback. */
    vterm_get_size(vt, &trows, &tcols);
    /* Pending scrolls are only folded into the matrix here, and a cell read
     * before that can still be the previous generation of the screen. */
    vterm_screen_flush_damage(scr);
    VTermPos cur;
    vterm_state_get_cursorpos(st, &cur);

    VTermScreenCell cell;
    if (vterm_screen_get_cell(scr, cur, &cell) && cell.chars[0]
        && cur.col + cell.width >= tcols) {
        cols = cell.width;              // right margin: cursor on the character
    } else if (cur.col > 0
        && vterm_screen_get_cell(scr, (VTermPos){ cur.row, cur.col - 1 }, &cell)) {
        /* One column back is either the gap cell of a wide character - the
         * chars[0] == -1 marker the host renderer keys on as well - or the
         * character itself, when it was a narrow one. */
        cols = cell.chars[0] == (uint32_t)-1 ? 2 : 1;
    }
    spin_unlock(&ctx->tty->lock);
    return cols;
}

// Echo an erase: back over the character's cells, blank them, and return the
// cursor to where it was, so the next keystroke lands in the same place. With
// ECHOE clear a real terminal only moves the cursor and leaves the character
// visible.
static void user_tty_erase_echo(rvvm_userland_t* ctx, int cols, bool echoe)
{
    char seq[3 * 8];
    size_t n = 0;
    if (cols < 1) {
        cols = 1;
    } else if (cols > 8) {
        cols = 8;                       // no character is wider than this
    }
    for (int i = 0; i < cols; ++i) {
        seq[n++] = '\b';
    }
    if (echoe) {
        for (int i = 0; i < cols; ++i) {
            seq[n++] = ' ';
        }
        for (int i = 0; i < cols; ++i) {
            seq[n++] = '\b';
        }
    }
    user_tty_vt_write(ctx, seq, n);
}

// How many bytes the lead byte of a UTF-8 sequence claims, 0 if it is not one.
static int tty_utf8_seq_len(uint8_t b)
{
    if (b < 0x80)         return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;                   // a continuation byte, or an invalid lead
}

/*
 * Drop the last character from the pending line, and report how many bytes went
 * away (0 when the line is already empty).
 *
 * A character, not a byte, which is the whole point of this function: the line
 * holds UTF-8, so erasing one byte of a multi-byte character would hand the
 * guest an invalid sequence and would take three Backspaces to remove one CJK
 * character. The tail is walked back over the continuation bytes onto the lead
 * byte, and the whole sequence is taken only when the lead byte claims exactly
 * the bytes that are there: anything malformed (a stray continuation byte, a
 * sequence truncated at the head of the line) is erased a byte at a time, so a
 * broken burst cannot swallow the character before it.
 *
 * Note that this is one *codepoint*, the same unit a Linux tty's VERASE
 * removes: a combining mark that got in as its own codepoint, or one emoji of a
 * ZWJ sequence, still takes a press each.
 */
static size_t tty_line_erase(rvvm_userland_t* ctx)
{
    size_t len = ctx->tty_line_len;
    if (!len) {
        return 0;
    }
    size_t start = len - 1;
    size_t nbytes = 1;
    if (ctx->tty_line[start] & 0x80) {
        while (start > 0 && (ctx->tty_line[start] & 0xC0) == 0x80) {
            start--;
        }
        nbytes = len - start;
        if ((ctx->tty_line[start] & 0xC0) == 0x80
            || tty_utf8_seq_len(ctx->tty_line[start]) != (int)nbytes) {
            start = len - 1;        // not a well-formed sequence: last byte only
            nbytes = 1;
        }
    }
    ctx->tty_line_len = start;
    return nbytes;
}

// Run one host input burst through the line discipline.
static void user_tty_input(rvvm_userland_t* ctx, const void* buf, size_t len)
{
    const uint8_t* p = buf;
    bool wake = false;
    bool interrupt = false;

    spin_lock(&ctx->tty_in_lock);

    uint32_t lflag = ctx->tty_lflag;
    bool canon = (lflag & TTY_LFLAG_ICANON) != 0;
    bool echo  = (lflag & TTY_LFLAG_ECHO)   != 0;
    bool echoe = (lflag & TTY_LFLAG_ECHOE)  != 0;
    bool isig  = (lflag & TTY_LFLAG_ISIG)   != 0;

    for (size_t i = 0; i < len; ++i) {
        uint8_t c = p[i];

        // ICRNL: the keyboard's Enter arrives as CR and reads back as NL.
        if (c == '\r') {
            c = '\n';
        }

        // ISIG: Ctrl-C is not data. A real tty signals the foreground
        // process, drops the pending line and never hands the byte over.
        // The guest is that process; a guest-installed handler cannot be
        // run, so the signal takes its default disposition: stop the run.
        if (isig && c == TTY_CC_VINTR) {
            interrupt = true;
            ctx->tty_line_len = 0;
            if (echo) user_tty_vt_write(ctx, "^C\r\n", 4);
            continue;
        }

        if (!canon) {
            // Raw mode (guest cleared ICANON): hand the byte straight through
            // and echo it verbatim - the guest owns its own line editing.
            tty_cooked_push(ctx, &c, 1);
            if (echo) {
                user_tty_vt_write(ctx, (const char*)&c, 1);
            }
            wake = true;
            continue;
        }

        // --- canonical mode ---
        if (c == TTY_CC_ERASE) {
            // Erase the last character, whole: one Backspace takes one
            // character the user typed, however many bytes of UTF-8 that is,
            // out of the line the guest will read and off the screen.
            if (ctx->tty_line_len) {
                int cols = user_tty_erased_cols(ctx);
                tty_line_erase(ctx);
                if (echo) {
                    user_tty_erase_echo(ctx, cols, echoe);
                }
            }
            continue;
        }

        if (c == TTY_CC_VEOF) {
            // Ctrl-D: deliver the pending line as-is; on an empty line it is
            // the classic end-of-file, which read(0) reports as 0.
            if (ctx->tty_line_len) {
                tty_cooked_push(ctx, ctx->tty_line, ctx->tty_line_len);
                ctx->tty_line_len = 0;
            } else {
                ctx->tty_eof_pending = 1;
            }
            wake = true;
            continue;
        }

        if (c == '\n') {
            // Enter: flush the assembled line, newline included.
            if (echo) {
                user_tty_vt_write(ctx, "\r\n", 2);
            }
            if (ctx->tty_line_len) {
                tty_cooked_push(ctx, ctx->tty_line, ctx->tty_line_len);
                ctx->tty_line_len = 0;
            }
            tty_cooked_push(ctx, "\n", 1);
            wake = true;
            continue;
        }

        if (ctx->tty_line_len < sizeof(ctx->tty_line)) {
            ctx->tty_line[ctx->tty_line_len++] = c;
            if (echo) {
                user_tty_vt_write(ctx, (const char*)&c, 1);
            }
        }
    }

    spin_unlock(&ctx->tty_in_lock);
    if (wake) {
        rvvm_event_wake(&ctx->tty_in_event);
    }
    if (interrupt) {
        // 128 + SIGINT: what a shell reports for a Ctrl-C'd job.
        rvvm_user_stop(ctx->machine, 130);
    }
}

// True while a cooked byte, a pending EOF, or a torn-down TTY is available.
static bool user_tty_readable(rvvm_userland_t* ctx)
{
    return ctx->tty_cooked_len || ctx->tty_eof_pending || ctx->tty_in_eof;
}

/*
 * Serve read(0, ...) from the virtual TTY.
 *
 * @block  when true and nothing is queued, wait for host input instead of
 *         returning early. The first iovec of a readv() blocks, the rest do
 *         not, so a multi-segment read returns as soon as the console drains.
 *
 * Returns the byte count, 0 for EOF, or a negative errno. A single reader is
 * assumed (an interactive console is read by one thread at a time): the wake
 * below rouses one waiter per queued burst.
 */
static int64_t user_tty_read(rvvm_userland_t* ctx, void* buf, size_t count, bool block)
{
    if (!buf) {
        return -UAPI_EFAULT;
    }
    if (!count) {
        return 0;
    }

    for (;;) {
        spin_lock(&ctx->tty_in_lock);
        if (ctx->tty_cooked_len) {
            size_t n = tty_cooked_pop(ctx, buf, count);
            spin_unlock(&ctx->tty_in_lock);
            return (int64_t)n;
        }
        if (ctx->tty_eof_pending) {
            ctx->tty_eof_pending = 0;
            spin_unlock(&ctx->tty_in_lock);
            return 0;
        }
        if (ctx->tty_in_eof) {
            spin_unlock(&ctx->tty_in_lock);
            return 0;
        }
        spin_unlock(&ctx->tty_in_lock);

        if (!block) {
            return 0;
        }
        rvvm_event_wait(&ctx->tty_in_event, RVVM_EVENT_INFINITE);
    }
}

void rvvm_user_set_tty_callback(rvvm_machine_t* machine, rvvm_user_tty_callback callback, void* userdata)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return;
    }
    ctx->tty_cb       = callback;
    ctx->tty_userdata = userdata;
}

/*
 * Push host keyboard input toward the guest's virtual TTY.
 *
 * `buf`/`len` is the byte sequence a real terminal would receive from the
 * keyboard: printable UTF-8, '\r' for Enter, 0x7F for Backspace, "\x1b[A" and
 * friends for the arrow keys, 0x03/0x04 for Ctrl-C/Ctrl-D. The bytes are run
 * through the line discipline the guest's termios advertises (ICRNL, ICANON
 * line assembly, ECHO) and the cooked result is what the guest's read(0, ...)
 * / readv(0, ...) returns. Echo is written into the VTerm, so the host's next
 * snapshot shows the typing.
 *
 * Safe to call from any thread, but the bytes are serialized against the
 * guest's own fd 1/2 output through the session lock. A no-op when no TTY is
 * attached.
 */
void rvvm_user_tty_input(rvvm_machine_t* machine, const void* buf, size_t len)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx || !buf || !len) {
        return;
    }
    if (!ctx->tty && !ctx->tty_cb) {
        return; // no virtual TTY attached: nowhere to echo and nothing to read
    }
    user_tty_init(ctx);
    if (ctx->tty) {
        /* Typing belongs to the live screen: a view parked in history comes
         * home first, or the echo of what is being typed would land off screen
         * and the console would look dead. The view is the session's state, so
         * the reset belongs here rather than in every host. */
        rvvm_tty_lock(ctx->tty);
        rvvm_tty_follow_locked(ctx->tty);
        rvvm_tty_unlock(ctx->tty);
    }
    user_tty_input(ctx, buf, len);
}

/*
 * Context of the guest the calling thread is running, for the deep helpers that
 * have no hart to reach the machine through. Set once per guest thread when it
 * enters the wrap loop - guest threads are 1:1 with host threads and a hart
 * never migrates between them - so a plain TLS variable is enough.
 */
#ifndef THREAD_LOCAL
#define THREAD_LOCAL
#endif
static THREAD_LOCAL rvvm_userland_t* tls_userland = NULL;

static inline rvvm_userland_t* uctx(void)
{
    return tls_userland;
}

/* The machine the calling thread is running a guest in, or NULL when it is not
 * running one. Declared with the translation helpers at the top of the file,
 * which is where the singleton it replaces used to live. */
static rvvm_machine_t* cur_machine(void)
{
    rvvm_userland_t* ctx = uctx();
    return ctx ? ctx->machine : NULL;
}

// Reset the guest address-space allocator for a fresh run (rvvm_user_linux)
static void guest_vm_init(void)
{
    rvvm_userland_t* ctx = uctx();
    ctx->guest_stack_top  = (ctx->machine->mem.addr + ctx->machine->mem.size) & ~(rvvm_addr_t)(GUEST_PAGE_SIZE - 1);
    ctx->guest_stack_base = ctx->guest_stack_top - GUEST_STACK_SIZE;
    ctx->guest_mmap_end   = ctx->guest_stack_base;
    ctx->guest_bump       = GUEST_MMAP_BASE;
    ctx->guest_brk_start  = 0;
    ctx->guest_brk_end    = GUEST_MMAP_BASE;
    ctx->guest_brk_ptr    = 0;
    ctx->guest_free_num   = 0;
}

// Hand out @size bytes of guest address space, zeroed like a fresh mapping
static bool guest_range_alloc(rvvm_addr_t* out, rvvm_addr_t hint, size_t size, bool fixed)
{
    rvvm_userland_t* ctx = uctx();
    size = align_size_up(size, GUEST_PAGE_SIZE);
    if (!size) {
        return false;
    }

    if (fixed) {
        /* A fixed mapping may legitimately land below the mmap area: a dynamic
         * loader maps .bss / guard pages right next to the image, and mremap()
         * moves a mapping wherever it fits. Only the NULL page and the mmap
         * ceiling are off limits - like Linux, such a mapping replaces
         * whatever was there. */
        if (hint < GUEST_PAGE_SIZE || hint + size > ctx->guest_mmap_end) {
            return false;
        }
        memset(to_ptr(hint), 0, size);
        *out = hint;
        return true;
    }

    // First fit in the free list
    for (size_t i = 0; i < ctx->guest_free_num; ++i) {
        if (ctx->guest_free[i].size >= size) {
            rvvm_addr_t addr = ctx->guest_free[i].addr;
            ctx->guest_free[i].addr += size;
            ctx->guest_free[i].size -= size;
            if (!ctx->guest_free[i].size) {
                ctx->guest_free[i] = ctx->guest_free[--ctx->guest_free_num];
            }
            memset(to_ptr(addr), 0, size);
            *out = addr;
            return true;
        }
    }

    // Then a hint above the bump pointer, then the bump pointer itself
    if (hint >= ctx->guest_bump && hint >= GUEST_MMAP_BASE && hint + size <= ctx->guest_mmap_end) {
        memset(to_ptr(hint), 0, size);
        *out = hint;
        return true;
    }
    if (ctx->guest_bump + size > ctx->guest_mmap_end) {
        return false;
    }
    rvvm_addr_t addr = ctx->guest_bump;
    ctx->guest_bump += size;
    if (size >= (1u << 20)) {
        rvvm_warn("DBG alloc: zeroing %llx bytes at %llx (mmap_end %llx)", (long long)size, (long long)addr,
                  (long long)ctx->guest_mmap_end);
    }
    memset(to_ptr(addr), 0, size);
    if (size >= (1u << 20)) {
        rvvm_warn("DBG alloc: zeroed ok");
    }
    *out = addr;
    return true;
}

static void guest_range_free(rvvm_addr_t addr, size_t size)
{
    rvvm_userland_t* ctx = uctx();
    rvvm_addr_t end = align_size_up(addr + size, GUEST_PAGE_SIZE);
    addr            = align_size_down(addr, GUEST_PAGE_SIZE);
    size            = end - addr;
    if (addr < GUEST_MMAP_BASE || addr + size > ctx->guest_mmap_end || !size) {
        return;
    }
    for (size_t i = 0; i < ctx->guest_free_num; ++i) {
        if (ctx->guest_free[i].addr + ctx->guest_free[i].size == addr) {
            ctx->guest_free[i].size += size;
            return;
        }
        if (addr + size == ctx->guest_free[i].addr) {
            ctx->guest_free[i].addr = addr;
            ctx->guest_free[i].size += size;
            return;
        }
    }
    if (ctx->guest_free_num < GUEST_FREE_MAX) {
        ctx->guest_free[ctx->guest_free_num].addr = addr;
        ctx->guest_free[ctx->guest_free_num].size = size;
        ctx->guest_free_num++;
    }
}

/*
 * A host libc that does not define one of these names cannot return it either,
 * so the entry is neutralised with a value errno can never take (errno is
 * always positive) instead of breaking the build. win32 lacks all of them.
 */
#ifndef ENOTBLK
#define ENOTBLK (-1)
#endif
#ifndef ERESTART
#define ERESTART (-1)
#endif
#ifndef ESTRPIPE
#define ESTRPIPE (-1)
#endif
#ifndef EUSERS
#define EUSERS (-1)
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT (-1)
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT (-1)
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN (-1)
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS (-1)
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN (-1)
#endif
#ifndef ESTALE
#define ESTALE (-1)
#endif
#ifndef EDQUOT
#define EDQUOT (-1)
#endif
#ifndef ENOMEDIUM
#define ENOMEDIUM (-1)
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE (-1)
#endif

/*
 * Host -> guest errno translation.
 *
 * The guest was built against the riscv64 Linux UAPI numbers; the host has its
 * own set (BSD/macOS moves EAGAIN to 35, win32 moves the whole socket family to
 * 100+) and the two only happen to agree for the low codes. Letting a host errno
 * through untouched therefore hands the guest a *different* error than the one
 * that happened: host ENOSYS(40) reads as guest ELOOP, host ENAMETOOLONG(38) as
 * guest ENOSYS, host EWOULDBLOCK(140) as nothing at all.
 *
 * Lookups are keyed by the host value, which makes an alias pair such as
 * EOPNOTSUPP/ENOTSUP or EWOULDBLOCK/EAGAIN harmless: both spellings are present
 * and resolve to the same guest code, the first match wins. On a Linux or
 * Android host every entry is an identity mapping, so the table is a no-op there
 * and the platform default is unchanged.
 */
static const struct host_guest_errno {
    int host, guest;
} host_guest_errno_map[] = {
    // Base set
    { EPERM, UAPI_EPERM },
    { ENOENT, UAPI_ENOENT },
    { ESRCH, UAPI_ESRCH },
    { EINTR, UAPI_EINTR },
    { EIO, UAPI_EIO },
    { ENXIO, UAPI_ENXIO },
    { E2BIG, UAPI_E2BIG },
    { ENOEXEC, UAPI_ENOEXEC },
    { EBADF, UAPI_EBADF },
    { ECHILD, UAPI_ECHILD },
    { EAGAIN, UAPI_EAGAIN },
    { EWOULDBLOCK, UAPI_EWOULDBLOCK },
    { ENOMEM, UAPI_ENOMEM },
    { EACCES, UAPI_EACCESS },
    { EFAULT, UAPI_EFAULT },
    { ENOTBLK, UAPI_ENOTBLK },
    { EBUSY, UAPI_EBUSY },
    { EEXIST, UAPI_EEXIST },
    { EXDEV, UAPI_EXDEV },
    { ENODEV, UAPI_ENODEV },
    { ENOTDIR, UAPI_ENOTDIR },
    { EISDIR, UAPI_EISDIR },
    { EINVAL, UAPI_EINVAL },
    { ENFILE, UAPI_ENFILE },
    { EMFILE, UAPI_EMFILE },
    { ENOTTY, UAPI_ENOTTY },
    { ETXTBSY, UAPI_ETXTBSY },
    { EFBIG, UAPI_EFBIG },
    { ENOSPC, UAPI_ENOSPC },
    { ESPIPE, UAPI_ESPIPE },
    { EROFS, UAPI_EROFS },
    { EMLINK, UAPI_EMLINK },
    { EPIPE, UAPI_EPIPE },
    { EDOM, UAPI_EDOM },
    { ERANGE, UAPI_ERANGE },

    // File, process and IPC (the point where Linux and the hosts diverge)
    { EDEADLK, UAPI_EDEADLK },
    { ENAMETOOLONG, UAPI_ENAMETOOLONG },
    { ENOLCK, UAPI_ENOLCK },
    { ENOSYS, UAPI_ENOSYS },
    { ENOTEMPTY, UAPI_ENOTEMPTY },
    { ELOOP, UAPI_ELOOP },
    { ENOMSG, UAPI_ENOMSG },
    { EIDRM, UAPI_EIDRM },
    { ENOSTR, UAPI_ENOSTR },
    { ENODATA, UAPI_ENODATA },
    { ETIME, UAPI_ETIME },
    { ENOSR, UAPI_ENOSR },
    { ENOLINK, UAPI_ENOLINK },
    { EPROTO, UAPI_EPROTO },
    { EBADMSG, UAPI_EBADMSG },
    { EOVERFLOW, UAPI_EOVERFLOW },
    { EILSEQ, UAPI_EILSEQ },
    { ERESTART, UAPI_ERESTART },
    { ESTRPIPE, UAPI_ESTRPIPE },
    { EUSERS, UAPI_EUSERS },
    { EDQUOT, UAPI_EDQUOT },
    { ENOMEDIUM, UAPI_ENOMEDIUM },
    { EMEDIUMTYPE, UAPI_EMEDIUMTYPE },
    { ECANCELED, UAPI_ECANCELED },
    { EOWNERDEAD, UAPI_EOWNERDEAD },
    { ENOTRECOVERABLE, UAPI_ENOTRECOVERABLE },

    // Sockets and network: a whole 40-entry block apart on win32
    { ENOTSOCK, UAPI_ENOTSOCK },
    { EDESTADDRREQ, UAPI_EDESTADDRREQ },
    { EMSGSIZE, UAPI_EMSGSIZE },
    { EPROTOTYPE, UAPI_EPROTOTYPE },
    { ENOPROTOOPT, UAPI_ENOPROTOOPT },
    { EPROTONOSUPPORT, UAPI_EPROTONOSUPPORT },
    { ESOCKTNOSUPPORT, UAPI_ESOCKTNOSUPPORT },
    { EOPNOTSUPP, UAPI_EOPNOTSUPP },
    { ENOTSUP, UAPI_ENOTSUP },
    { EPFNOSUPPORT, UAPI_EPFNOSUPPORT },
    { EAFNOSUPPORT, UAPI_EAFNOSUPPORT },
    { EADDRINUSE, UAPI_EADDRINUSE },
    { EADDRNOTAVAIL, UAPI_EADDRNOTAVAIL },
    { ENETDOWN, UAPI_ENETDOWN },
    { ENETUNREACH, UAPI_ENETUNREACH },
    { ENETRESET, UAPI_ENETRESET },
    { ECONNABORTED, UAPI_ECONNABORTED },
    { ECONNRESET, UAPI_ECONNRESET },
    { ENOBUFS, UAPI_ENOBUFS },
    { EISCONN, UAPI_EISCONN },
    { ENOTCONN, UAPI_ENOTCONN },
    { ESHUTDOWN, UAPI_ESHUTDOWN },
    { ETOOMANYREFS, UAPI_ETOOMANYREFS },
    { ETIMEDOUT, UAPI_ETIMEDOUT },
    { ECONNREFUSED, UAPI_ECONNREFUSED },
    { EHOSTDOWN, UAPI_EHOSTDOWN },
    { EHOSTUNREACH, UAPI_EHOSTUNREACH },
    { EALREADY, UAPI_EALREADY },
    { EINPROGRESS, UAPI_EINPROGRESS },
    { ESTALE, UAPI_ESTALE },
};

// Return last errno like a syscall interface
static int last_errno(void)
{
    int host_err = errno;
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(host_guest_errno_map); ++i) {
        if (host_guest_errno_map[i].host == host_err) {
            return -host_guest_errno_map[i].guest;
        }
    }
    /*
     * Not in the table: either a host call that failed without setting errno
     * (0, which is not a real error code) or a host error we have never seen.
     * Hand it through rather than inventing a code, but make a genuinely new one
     * visible once so the table can be extended.
     */
    if (host_err > 0) {
        DO_ONCE({ rvvm_warn("Unmapped host errno %d leaked to the guest", host_err); });
    }
    return -host_err;
}

// Return negative values on -1 error like a syscall interface does
static rvvm_addr_t errno_ret(int64_t val)
{
    if (val == -1) {
        return last_errno();
    } else {
        return val;
    }
}

PUBLIC void rvvm_user_set_io_callback(rvvm_machine_t* machine, rvvm_user_io_callback callback)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (ctx) ctx->io_callback = callback;
}

PUBLIC void rvvm_user_set_exit_callback(rvvm_machine_t* machine, rvvm_user_exit_callback callback)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (ctx) ctx->exit_callback = callback;
}

PUBLIC void rvvm_user_set_prefix(rvvm_machine_t* machine, const char* prefix)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return;
    }
    safe_free(ctx->prefix_owned);
    ctx->prefix_owned = NULL;
    if (prefix && prefix[0]) {
        size_t len = strlen(prefix) + 1;
        char* copy = safe_malloc(len);
        memcpy(copy, prefix, len);
        ctx->prefix_owned = copy;
        ctx->prefix_path = copy;
    } else {
        ctx->prefix_path = NULL;
    }
    ctx->prefix_forced = true;
}

PUBLIC const char* rvvm_user_get_prefix(rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    return ctx ? ctx->prefix_path : NULL;
}

static bool proc_mem_readable(const void* addr, size_t size)
{
    static int fd = 0;
    DO_ONCE({
        fd = vma_anon_memfd(4096);
        if (fd < 0) rvvm_fatal("Failed to create memfd!");
    });
    return write(fd, addr, size) == (ssize_t)size;
}

/*
 * Guest symbolization for crash backtraces. Setting RVVM_ADDR2LINE to an
 * addr2line (or llvm-addr2line, the CLIs are compatible) makes the crash
 * report batch-hand the guest addresses of every frame to that tool together
 * with the ELF image they belong to, which is still on the host disk:
 *
 *   RVVM_ADDR2LINE=C:/msys64/mingw64/bin/addr2line.exe
 *
 * turns a frame into "CL_Init (client/cl_main.c:812)" instead of a bare
 * relocation offset. Off by default (no environment variable, no subprocess).
 *
 * The image paths are per instance, not file-scope: they live in
 * rvvm_userland_t (group C) and are read back through uctx() when a crash is
 * reported - with a second guest in the process, a shared pair of paths would
 * resolve one guest's frames against the other's symbols.
 */

#if defined(_WIN32)
#define proc_popen  _popen
#define proc_pclose _pclose
#else
#define proc_popen  popen
#define proc_pclose pclose
#endif

static void proc_symbolize(const char* label, const char* elf, const rvvm_addr_t* addrs, size_t count)
{
    const char* tool = getenv("RVVM_ADDR2LINE");
    if (!tool || !tool[0] || !elf || !elf[0] || !count) {
        return;
    }
    // One tool invocation per image: addresses as arguments, two output lines
    // (function, location) per address
    char cmd[4096];
#if defined(_WIN32)
    /* The Win32 CRT spawns the tool itself instead of going through a shell,
     * and hands the first word to CreateProcess() verbatim: quoting the
     * program name makes it fail with "The filename, directory name, or volume
     * label syntax is incorrect.", while quoted *arguments* are fine. */
    if (rvvm_strfind(tool, " ")) {
        rvvm_warn("RVVM_ADDR2LINE path must not contain spaces");
        return;
    }
    size_t len = rvvm_snprintf(cmd, sizeof(cmd), "%s -f -C -e \"%s\"", tool, elf);
#else
    // popen() goes through a shell, so quote the commands and paths
    size_t len = rvvm_snprintf(cmd, sizeof(cmd), "\"%s\" -f -C -e \"%s\"", tool, elf);
#endif
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (len >= sizeof(cmd) - 32) {
            break; // Command line limit, symbolize what fits
        }
        len += rvvm_snprintf(cmd + len, sizeof(cmd) - len, " 0x%llx", (unsigned long long)addrs[i]);
        used++;
    }
    if (!used) {
        return;
    }

    FILE* pipe = proc_popen(cmd, "r");
    if (!pipe) {
        rvvm_warn("Failed to run %s", tool);
        return;
    }
    rvvm_warn("Symbols (%s):", label);
    for (size_t i = 0; i < used; ++i) {
        char func[256] = {0};
        char loc[512]  = {0};
        if (!fgets(func, sizeof(func), pipe) || !fgets(loc, sizeof(loc), pipe)) {
            rvvm_warn(" * * * Symbolizer output ended early!");
            break;
        }
        // Trim trailing line endings
        for (size_t j = 0; func[j]; ++j) {
            if (func[j] == '\n' || func[j] == '\r') func[j] = 0;
        }
        for (size_t j = 0; loc[j]; ++j) {
            if (loc[j] == '\n' || loc[j] == '\r') loc[j] = 0;
        }
        rvvm_warn("  %llx: %s (%s)", (unsigned long long)addrs[i], func, loc);
    }
    proc_pclose(pipe);
}

#ifndef __riscv
#define USERLAND_DEFAULT_PREFIX "/home/lekkit/stuff/userland/debian"
#else
#define USERLAND_DEFAULT_PREFIX NULL
#endif

// Defaults applied when a userland context is created (see rvvm_user_linux)
#define USERLAND_DEFAULT_FAKE_ROOT true

static bool path_bypass(const char* path)
{
    const char* prefix = uctx()->prefix_path;
    return prefix == NULL
        || rvvm_strfind(path, "/dev") == path
        || rvvm_strfind(path, "/sys") == path
        || rvvm_strfind(path, "/proc") == path
        || rvvm_strfind(path, "/tmp") == path
        || rvvm_strfind(path, "/var/tmp") == path;
}

static bool path_wrapped(const char* path)
{
    const char* prefix = uctx()->prefix_path;
    return prefix == NULL
        || rvvm_strfind(path, prefix) == path
        || path_bypass(path);
}

static const char* wrap_path(char* buffer, const char* path)
{
    const char* prefix = uctx()->prefix_path;
    if (prefix && path) {
        if (path_bypass(path)) {
            return path;
        }

        if (rvvm_strfind(path, "/") == path) {
            size_t prefix_len = rvvm_strlcpy(buffer, prefix, UAPI_PATH_MAX);
            rvvm_strlcpy(buffer + prefix_len, path, UAPI_PATH_MAX - prefix_len);
            return buffer;
        }
    }
    return path;
}

static size_t unwrap_path(char* buffer, const char* path, size_t size)
{
    const char* prefix = uctx()->prefix_path;
    if (prefix && rvvm_strfind(path, prefix) == path) {
        size_t len = rvvm_strlen(prefix) + 1;
        size_t off = rvvm_strlcpy(buffer, "/", size);
        return rvvm_strlcpy(buffer + off, path + len, size - off);
    }

    return rvvm_strlcpy(buffer, path, UAPI_PATH_MAX);
}

/* Guest open(2) flags -> host open() flags.
 *
 * On a Linux host the two sets are identical, so this passes through. The
 * win32 CRT numbers them differently (O_CREAT 0x40 vs 0x100, O_APPEND 0x400 vs
 * 0x8, O_EXCL 0x80 vs 0x400) and has no equivalent for O_DIRECTORY / O_CLOEXEC
 * / O_NONBLOCK / O_NOFOLLOW / O_PATH, while some guest bits would even alias a
 * *different* CRT flag (the guest's O_NOCTTY 0x100 would be read as _O_CREAT,
 * silently creating files). Everything without an equivalent is dropped; the
 * CRT layer adds _O_BINARY itself. */
static int uapi_open_flags(int flags)
{
#if defined(_WIN32)
    int host = flags & 3;                   /* O_RDONLY / O_WRONLY / O_RDWR agree */
    if (flags & 0x40)   host |= _O_CREAT;   /* guest O_CREAT */
    if (flags & 0x80)   host |= _O_EXCL;    /* guest O_EXCL */
    if (flags & 0x200)  host |= _O_TRUNC;   /* guest O_TRUNC */
    if (flags & 0x400)  host |= _O_APPEND;  /* guest O_APPEND */
    return host;
#else
    return flags;
#endif
}

void sig_handler(int signal)
{
    rvvm_info("Received signal %d", signal);
}

// Debug: current running hart, for host fault diagnostics
// Hart running on the calling thread, for the fault handler (guest threads are
// 1:1 with host threads). Same THREAD_LOCAL guard as fpu_lib.c.
#ifndef THREAD_LOCAL
#define THREAD_LOCAL
#endif
static THREAD_LOCAL rvvm_hart_t* current_user_hart = NULL;

/* Name of the marshalled GL/EGL call currently being serviced, or NULL when
 * the guest is running its own code. The win32 GL dispatch writes it (see
 * win32_gl_dispatch.c) and the fault dump below reads it, so that a host
 * fault can be attributed to the guest call that triggered it. Storage lives
 * here rather than in the dispatch so host binaries that do not link the GL
 * dispatch still resolve this symbol. */
const char* g_gl_inflight = NULL;

static void user_fault_hex(char** p, uint64_t val)
{
    char tmp[17];
    uint32_t i = 0;
    do {
        uint32_t d = val & 0xF;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val >>= 4;
    } while (val);
    while (i) {
        *(*p)++ = tmp[--i];
    }
    *(*p)++ = '\n';
}

static void user_fault_handler(int sig, siginfo_t* info, void* ucontext)
{
    char buf[8192];
    char* p = buf;
    UNUSED(ucontext);
    const char* hdr = "=== HOST FAULT inside guest === sig=";
    while (*hdr) *p++ = *hdr++;
    user_fault_hex(&p, (uint64_t)(size_t)sig);
    const char* addr = "fault addr(si_addr): ";
    while (*addr) *p++ = *addr++;
    user_fault_hex(&p, (uint64_t)(size_t)info->si_addr);
    /* Which marshalled GL/EGL call (if any) was being serviced. Without this
     * the register dump alone cannot say whether the fault came from the
     * guest's own code or from a host GL call on its behalf. */
    {
        const char* inf = "in-flight host call: ";
        while (*inf) *p++ = *inf++;
        const char* name = g_gl_inflight;
        if (name) {
            while (*name) *p++ = *name++;
        } else {
            const char* none = "(none - guest code)";
            while (*none) *p++ = *none++;
        }
        *p++ = '\n';
    }
    rvvm_hart_t* cpu = current_user_hart;
    if (cpu) {
        const char* pch = "guest PC: ";
        while (*pch) *p++ = *pch++;
        user_fault_hex(&p, rvvm_read_cpu_reg(cpu, RVVM_REGID_PC));
        /* Dump the faulting instruction bytes */
        {
            size_t pc = rvvm_read_cpu_reg(cpu, RVVM_REGID_PC);
            uint8_t* ip = (uint8_t*)to_ptr(pc);
            const char* ih = "guest insn bytes: ";
            while (*ih) *p++ = *ih++;
            for (int i = 0; i < 8; ++i) {
                user_fault_hex(&p, ip[i]);
            }
        }
        for (uint32_t i = 0; i < 32; ++i) {
            *p++ = 'x';
            user_fault_hex(&p, i);
            const char* eq = "= ";
            while (*eq) *p++ = *eq++;
            user_fault_hex(&p, rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + i));
        }
    }
#ifndef ANDROID
    (void)!write(2, buf, (size_t)(p - buf));
    stacktrace_print();
#else
    if (p < buf + sizeof(buf)) *p = '\0';
    __android_log_write(ANDROID_LOG_INFO, "RVVM-HOSTFAULT", buf);
#endif
    rvvm_warn("_Exit due to HOST FAULT inside guest");
    _Exit(128 + sig);
}

static void user_fault_handler_install(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = user_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* ============================================================
 * Guest thread registry & userland shutdown
 *
 * Upstream RVVM assumes "guest == process": exit/exit_group either
 * _Exit()s the host process or (with an exit callback installed, as
 * done by embedded hosts like Android) fires the callback and simply
 * keeps running the vCPU, which turns into a zombie thread faulting
 * on freed guest memory or destroyed host resources.
 *
 * We instead track every guest thread and shut the vCPUs down cleanly
 * once the guest process terminates (sys_exit_group, or sys_exit on
 * the main thread, matching Linux semantics).
 * ============================================================ */

/*
 * Host-side suspend/resume (rvvm_user_suspend()/rvvm_user_resume()).
 *
 * ctx->userland_suspend is both the requested state and the futex word the
 * parked vCPUs wait on: while it reads 1 a parked vCPU sits in
 * rvvm_futex_wait(&ctx->userland_suspend, 1, ...), so clearing it wakes them
 * all. ctx->userland_parked counts the vCPUs that are actually parked, which
 * lets rvvm_user_suspend() be a barrier instead of a fire-and-forget request.
 *
 * Bounded wait for the parked vCPUs, so a missed wakeup costs latency but can
 * never turn into a stuck vCPU. The futex wake does the real work.
 */
#define USERLAND_SUSPEND_POLL_NS (100 * 1000000ULL)

/*
 * How long rvvm_user_suspend() waits for the vCPUs to reach the park point. A
 * guest blocked in a host syscall (e.g. a read() waiting on input) cannot park
 * until that returns, so the barrier gives up instead of hanging the caller.
 */
#define USERLAND_SUSPEND_BARRIER_MS 200

/*
 * Park the calling guest thread while the process is suspended. Returns
 * immediately (one relaxed load) when the guest is not suspended, so it costs
 * nothing on the hot path.
 */
static void userland_park_if_suspended(rvvm_user_thread_t* thread)
{
    rvvm_userland_t* ctx = uctx();
    if (likely(atomic_load_uint32(&ctx->userland_suspend) == 0)) {
        return;
    }

    atomic_add_uint32(&ctx->userland_parked, 1);
    rvvm_warn("DBG park: guest thread parking (suspend=%u)", atomic_load_uint32(&ctx->userland_suspend));
    while (atomic_load_uint32(&ctx->userland_suspend) && !atomic_load_uint32(&thread->finished)) {
        rvvm_futex_wait(&ctx->userland_suspend, 1, USERLAND_SUSPEND_POLL_NS);
    }
    rvvm_warn("DBG park: guest thread resumed");
    atomic_sub_uint32(&ctx->userland_parked, 1);
}

/*
 * Host-initiated suspend: stop every guest vCPU at an instruction boundary and
 * keep it stopped until rvvm_user_resume().
 *
 * Unlike rvvm_user_stop() this is fully reversible - nothing is marked
 * finished and no state is torn down. Each vCPU is kicked out of the
 * interpreter with a hart pause (which also wakes WFI sleepers), then parks in
 * its wrap loop; on resume it re-enters exactly where it left off.
 *
 * Safe to call from any thread while rvvm_user_linux() is running, and a no-op
 * when the guest is already suspended. Returns true if every registered vCPU
 * parked within USERLAND_SUSPEND_BARRIER_MS; false means at least one thread
 * is stuck in a blocking host syscall and will park once that returns.
 */
PUBLIC bool rvvm_user_suspend(rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return true; // No guest instance running
    }
    if (atomic_swap_uint32(&ctx->userland_suspend, 1)) {
        return true; // Already suspended
    }

    spin_lock(&ctx->userland_threads_lock);
    vector_foreach(ctx->userland_threads, i) {
        rvvm_user_thread_t* thread = vector_at(ctx->userland_threads, i);
        if (thread->cpu) {
            // Break out of the interpreter at the next instruction boundary
            riscv_hart_queue_pause(thread->cpu);
        }
    }
    spin_unlock(&ctx->userland_threads_lock);

    // Barrier: wait for the vCPUs to actually park. Recomputed every round so a
    // vCPU that registers (clone) or deregisters (exit) concurrently does not
    // make the count impossible to reach.
    for (uint32_t i = 0; i < USERLAND_SUSPEND_BARRIER_MS; ++i) {
        uint32_t total = 0;
        uint32_t parked = atomic_load_uint32(&ctx->userland_parked);
        spin_lock(&ctx->userland_threads_lock);
        total = vector_size(ctx->userland_threads);
        spin_unlock(&ctx->userland_threads_lock);
        if (parked >= total) {
            return true;
        }
        sleep_ms(1);
    }
    return false;
}

/*
 * Resume a guest suspended with rvvm_user_suspend(). Wakes every parked vCPU;
 * no-op when the guest is not suspended.
 */
PUBLIC void rvvm_user_resume(rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return; // No guest instance running
    }
    if (atomic_swap_uint32(&ctx->userland_suspend, 0) == 0) {
        return; // Was not suspended
    }
    rvvm_futex_wake(&ctx->userland_suspend, UINT32_MAX);
}

// True while the guest is suspended (requested; see rvvm_user_suspend())
PUBLIC bool rvvm_user_is_suspended(rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    return ctx && atomic_load_uint32(&ctx->userland_suspend) != 0;
}

// Push/pop the calling guest thread onto its instance's registry. Called from
// the guest threads themselves, so the context comes from the TLS binding.
static void userland_thread_register(rvvm_user_thread_t* thread)
{
    rvvm_userland_t* ctx = uctx();
    spin_lock(&ctx->userland_threads_lock);
    vector_push_back(ctx->userland_threads, thread);
    spin_unlock(&ctx->userland_threads_lock);
}

static void userland_thread_unregister(rvvm_user_thread_t* thread)
{
    rvvm_userland_t* ctx = uctx();
    spin_lock(&ctx->userland_threads_lock);
    vector_foreach_back(ctx->userland_threads, i) {
        if (vector_at(ctx->userland_threads, i) == thread) {
            vector_erase(ctx->userland_threads, i);
            break;
        }
    }
    spin_unlock(&ctx->userland_threads_lock);
}

/*
 * Process-wide guest termination: fire the exit callback once, then
 * stop every guest vCPU. Threads other than @self get kicked out of
 * the interpreter loop via a hart pause and unwind in their own wrap
 * loop; @self unwinds via its syscall handling path.
 */
static void userland_process_exit(rvvm_userland_t* ctx, int code, rvvm_user_thread_t* self)
{
    if (atomic_swap_uint32(&ctx->userland_exit_reported, 1) == 0 && ctx->exit_callback) {
        ctx->exit_callback(code);
    }

    spin_lock(&ctx->userland_threads_lock);
    vector_foreach(ctx->userland_threads, i) {
        rvvm_user_thread_t* thread = vector_at(ctx->userland_threads, i);
        atomic_store_uint32(&thread->finished, 1);
        if (thread != self && thread->cpu) {
            // Make the vCPU return from the interpreter loop promptly
            riscv_hart_queue_pause(thread->cpu);
        }
    }
    spin_unlock(&ctx->userland_threads_lock);

    // Release vCPUs parked by rvvm_user_suspend(): they were told to finish
    // above but cannot see it while suspended, and must unwind like the rest.
    if (atomic_swap_uint32(&ctx->userland_suspend, 0)) {
        rvvm_futex_wake(&ctx->userland_suspend, UINT32_MAX);
    }

    // Unblock any thread parked in read(0) on the virtual TTY. Its wait is on a
    // host event, not on the interpreter, so the hart pause above cannot reach
    // it; it returns EOF and unwinds like every other guest thread.
    spin_lock(&ctx->tty_in_lock);
    ctx->tty_in_eof = true;
    spin_unlock(&ctx->tty_in_lock);
    rvvm_event_wake(&ctx->tty_in_event);
}

/*
 * Host-initiated stop: the win32 launcher's Stop falls back to this when a
 * guest ignores the Android lifecycle teardown (a guest that never polls
 * APP_CMD_DESTROY, or one wedged in its own loop).
 *
 * It goes through the guest-exit path with self == NULL. The caller is not a
 * guest thread, so no thread is exempt: every guest thread - the guest main
 * thread included - is paused and marked finished, then breaks out of its wrap
 * loop and unwinds through the normal cleanup. That makes this a clean stop
 * rather than a TerminateThread() on live interpreter state.
 */
PUBLIC void rvvm_user_stop(rvvm_machine_t* machine, int exit_code)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return; // No guest instance running
    }
    userland_process_exit(ctx, exit_code, NULL);
}

/*
 * Wait (bounded) for all guest threads to deregister themselves.
 * Returns true once the registry is empty.
 */
static bool userland_threads_gone(rvvm_userland_t* ctx, uint32_t timeout_ms)
{
    while (timeout_ms--) {
        bool empty = false;
        spin_lock(&ctx->userland_threads_lock);
        empty = vector_size(ctx->userland_threads) == 0;
        spin_unlock(&ctx->userland_threads_lock);
        if (empty) return true;
        sleep_ms(1);
    }
    return false;
}

/* ============================================================
 * Memory read/write helpers for Guest↔Host data transfer
 * ============================================================ */

/*
 * Read data from Guest memory into Host buffer.
 * @param cpu      Hart context (for guest memory access)
 * @param guest_addr  Guest virtual address to read from
 * @param host_buf Host buffer to write into
 * @param size     Number of bytes to read
 * @return         0 on success, -1 on failure
 */
static int guest_read_mem(rvvm_hart_t* cpu, uint64_t guest_addr, void* host_buf, size_t size)
{
    (void)cpu;
    const void* src = to_ptr_sz(guest_addr, size);
    if (!src) {
        return -1;
    }
    memcpy(host_buf, src, size);
    return 0;
}

/*
 * Write data from Host buffer into Guest memory.
 * @param cpu      Hart context (for guest memory access)
 * @param guest_addr  Guest virtual address to write to
 * @param host_buf Host buffer to read from
 * @param size     Number of bytes to write
 * @return         0 on success, -1 on failure
 */
static int guest_write_mem(rvvm_hart_t* cpu, uint64_t guest_addr, const void* host_buf, size_t size)
{
    (void)cpu;
    void* dst = to_ptr_sz(guest_addr, size);
    if (!dst) {
        return -1;
    }
    memcpy(dst, host_buf, size);
    return 0;
}

/*
 * Read a NUL-terminated string from Guest memory.
 * @param cpu      Hart context
 * @param guest_addr  Guest virtual address of string
 * @param host_buf Host buffer to store string
 * @param max_len  Maximum length of string (including NUL)
 * @return         Number of bytes copied, or -1 on failure
 */
static int guest_read_str(rvvm_hart_t* cpu, uint64_t guest_addr, char* host_buf, size_t max_len)
{
    (void)cpu;
    if (max_len == 0) {
        return -1;
    }
    const char* src = (const char*)to_ptr_sz(guest_addr, max_len);
    if (!src) {
        return -1;
    }
    size_t i;
    for (i = 0; i < max_len - 1; i++) {
        host_buf[i] = src[i];
        if (src[i] == 0) break;
    }
    host_buf[i] = 0;
    return (int)i;
}

/* Main execution loop (Run the user CPU, handle syscalls) */
static void* rvvm_user_thread_wrap(void* arg);

// We can't touch the native brk heap since it would likely blow up the process
static rvvm_addr_t rvvm_sys_brk(rvvm_addr_t brk_new)
{
    rvvm_userland_t* ctx = uctx();
    spin_lock(&ctx->brk_lock);
    if (ctx->guest_brk_start && brk_new >= ctx->guest_brk_start && brk_new <= ctx->guest_brk_end) {
        if (brk_new > ctx->guest_brk_ptr) {
            // Newly allocated brk memory should be zeroed
            memset(to_ptr(ctx->guest_brk_ptr), 0, brk_new - ctx->guest_brk_ptr);
        }
        ctx->guest_brk_ptr = brk_new;
    } else if (brk_new) {
        rvvm_warn("invalid brk %llx, heap %llx..%llx!", (unsigned long long)brk_new,
                  (unsigned long long)ctx->guest_brk_start, (unsigned long long)ctx->guest_brk_end);
    }
    rvvm_addr_t brk_ret = ctx->guest_brk_ptr;
    spin_unlock(&ctx->brk_lock);
    return brk_ret;
}

#define UAPI_CLONE_VM             0x00000100
#define UAPI_CLONE_VFORK          0x00004000
#define UAPI_CLONE_SETTLS         0x00080000
#define UAPI_CLONE_PARENT_SETTID  0x00100000
#define UAPI_CLONE_CHILD_CLEARTID 0x00200000
#define UAPI_CLONE_CHILD_SETTID   0x01000000

#define UAPI_CLONE_INVALID_THREAD_FLAGS 0x7E02F000

// long sys_clone(unsigned long flags, void *stack, int *parent_tid, unsigned long tls, int *child_tid);
static int rvvm_sys_clone(rvvm_hart_t* cpu, uint32_t flags, size_t stack, uint32_t* parent_tid, size_t tls, uint32_t* child_tid)
{
    if ((flags & UAPI_CLONE_VM) && !(flags & UAPI_CLONE_VFORK)) {
        if (flags & UAPI_CLONE_INVALID_THREAD_FLAGS) {
            rvvm_warn("sys_clone(): Invalid flags %x", flags);
            return -UAPI_EINVAL;
        }

        rvvm_user_thread_t* thread = safe_new_obj(rvvm_user_thread_t);
        thread->cpu = rvvm_create_user_thread(cpu->machine);

        // Clone all CPU state
        for (size_t i=1; i<32; ++i) {
            rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_X0 + i, rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + i));
        }
        for (size_t i=0; i<32; ++i) {
            rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_F0 + i, rvvm_read_cpu_reg(cpu, RVVM_REGID_F0 + i));
        }

        // Land after syscall entry
        rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_PC, rvvm_read_cpu_reg(cpu, RVVM_REGID_PC) + 4);

        // Set guest stack pointer
        rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_X0 + 2, stack);

        if (flags & UAPI_CLONE_SETTLS) {
            // Set guest TLS register
            rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_X0 + 4, tls);
        }

        // Return 0 in cloned thread
        rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_X0 + 10, 0); // a0

        // Spawn the thread using portable RVVM thread facilities
        thread_detach(rvvm_thread_create_ex(rvvm_user_thread_wrap, thread, 0));

        uint32_t tid = atomic_load_uint32(&thread->tid);
        while (!tid) {
            sleep_ms(0);
            tid = atomic_load_uint32(&thread->tid);
        }

        if ((flags & UAPI_CLONE_PARENT_SETTID) && parent_tid) {
            atomic_store_uint32(parent_tid, tid);
        }

        if (flags & UAPI_CLONE_CHILD_SETTID) {
            thread->child_settid = child_tid;
        }

        if (flags & UAPI_CLONE_CHILD_CLEARTID) {
            thread->child_cleartid = child_tid;
        }

        return tid;
    } else {
        // Emulate vfork via fork too
        return fork();
    }
}

#define UAPI_FUTEX_CMD_MASK    0x3F
#define UAPI_FUTEX_WAIT        0x0
#define UAPI_FUTEX_WAKE        0x1
#define UAPI_FUTEX_WAIT_BITSET 0x9
#define UAPI_FUTEX_WAKE_BITSET 0xA

static int uapi_ts_to_host(struct timespec* dst, const struct uapi_timespec* src);

// futex(uaddr, op, val, timeout, uaddr2, val3): the timeout is a guest struct
// timespec sitting in guest memory, so it must be converted into a host-side
// temporary before the host kernel dereferences it (FUTEX_WAIT treats it as a
// relative duration, FUTEX_WAIT_BITSET / the PI ops as an absolute instant -
// both are the same two 64-bit fields, only the interpretation differs).
static int rvvm_sys_futex(uint32_t* addr, int futex_op, uint32_t val,
                          const struct uapi_timespec* timeout, uint32_t* uaddr2, uint32_t val3)
{
    if (!addr) {
        return -UAPI_EFAULT;
    }
#if defined(__linux__)
    struct timespec ts;
    const struct timespec* hts = uapi_ts_to_host(&ts, timeout) ? &ts : NULL;
    return errno_ret(syscall(SYS_futex, addr, futex_op, val, hts, uaddr2, val3));
#else
    UNUSED(uaddr2); UNUSED(val3);
    // No host futex: poll the guest word and honour the timeout ourselves.
    bool absolute = (futex_op & UAPI_FUTEX_CMD_MASK) == UAPI_FUTEX_WAIT_BITSET;
    uint64_t wait_ms = (uint64_t)-1; // No timeout: wait forever
    if (timeout) {
        uint64_t deadline_ms = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
        if (absolute) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now)) {
                return -UAPI_EINVAL;
            }
            uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
            wait_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0;
        } else {
            wait_ms = deadline_ms;
        }
    }
    switch (futex_op & UAPI_FUTEX_CMD_MASK) {
        case UAPI_FUTEX_WAIT:
        case UAPI_FUTEX_WAIT_BITSET: {
            /*
             * Linux fails a mismatching wait on the spot instead of sleeping:
             * the caller relies on that answer to tell "the word is still what
             * I expect, I really have to be woken" from "somebody changed it
             * under me, retry". The polling loop below can only ever produce
             * the first outcome, so a wait that never matched would be reported
             * as a success.
             */
            if (atomic_load_uint32(addr) != val) {
                return -UAPI_EAGAIN;
            }
            uint64_t waited = 0;
            while (atomic_load_uint32(addr) == val) {
                if (waited >= wait_ms) {
                    return -UAPI_ETIMEDOUT;
                }
                sleep_ms(1);
                waited++;
            }
            return 0;
        }
        case UAPI_FUTEX_WAKE:
        case UAPI_FUTEX_WAKE_BITSET:
            return 0;
    }
    rvvm_warn("Unimplemented futex op %x", futex_op);
    return -UAPI_EINVAL;
#endif
}

static struct timeval* uapi_ts32_to_timeval(struct timeval* tv, const struct uapi_timespec32* ts32)
{
    if (ts32) {
        tv->tv_sec = ts32->tv_sec;
        tv->tv_usec = ts32->tv_nsec / 1000;
        return tv;
    }
    return NULL;
}

// Guest <-> host time structure conversion. Guest memory is only ever
// interpreted as a struct uapi_*, the host libc only ever writes into a
// host-side temporary: the two layouts are not the same type (the guest's
// fields are 8 bytes wide whatever the host's long is), so passing to_ptr()
// straight to the host would leave half of the guest's tv_nsec untouched.
static int uapi_ts_to_host(struct timespec* dst, const struct uapi_timespec* src)
{
    if (!src) {
        return 0; // Absent structure: the syscall decides what that means
    }
    dst->tv_sec = (long long)src->tv_sec;
    dst->tv_nsec = (long)src->tv_nsec; // 0 <= tv_nsec < 1e9, fits any long
    return 1;
}

static void uapi_ts_from_host(struct uapi_timespec* dst, const struct timespec* src)
{
    dst->tv_sec = (uint64_t)src->tv_sec;
    dst->tv_nsec = (uint64_t)src->tv_nsec;
}

static int uapi_timeval_to_host(struct timeval* dst, const struct uapi_timeval* src)
{
    if (!src) {
        return 0;
    }
    dst->tv_sec = (long long)src->tv_sec;
    dst->tv_usec = (long)src->tv_usec;
    return 1;
}

static void uapi_timeval_from_host(struct uapi_timeval* dst, const struct timeval* src)
{
    dst->tv_sec = (uint64_t)src->tv_sec;
    dst->tv_usec = (uint64_t)src->tv_usec;
}

// The host gettimeofday() is not usable as-is here: MinGW's struct timeval has
// a 32-bit tv_sec (LLP64), so routing the guest clock through it would truncate
// the wall clock. clock_gettime() keeps full 64-bit seconds on every host we
// build for, and the vDSO makes it just as cheap on Linux.
static rvvm_addr_t rvvm_sys_gettimeofday(struct uapi_timeval* tv)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) {
        return last_errno();
    }
    tv->tv_sec  = (uint64_t)ts.tv_sec;
    tv->tv_usec = (uint64_t)(ts.tv_nsec / 1000);
    return 0;
}

static void uapi_itimerval_to_host(struct itimerval* dst, const struct uapi_itimerval* src)
{
    memset(dst, 0, sizeof(*dst));
    uapi_timeval_to_host(&dst->it_interval, src ? &src->it_interval : NULL);
    uapi_timeval_to_host(&dst->it_value, src ? &src->it_value : NULL);
}

static void uapi_itimerval_from_host(struct uapi_itimerval* dst, const struct itimerval* src)
{
    uapi_timeval_from_host(&dst->it_interval, &src->it_interval);
    uapi_timeval_from_host(&dst->it_value, &src->it_value);
}

static int rvvm_sys_select_time32(int nfds, void* rfds, void* wfds, void* efds, const struct uapi_timespec32* ts32)
{
    // TODO: fd_set conversion
    struct timeval tv = {0};
    return errno_ret(select(nfds, rfds, wfds, efds, uapi_ts32_to_timeval(&tv, ts32)));
}

static int rvvm_sys_poll_time32(void* pfds, size_t npfds, const struct uapi_timespec32* ts32)
{
    // TODO: struct pollfd conversion
    int timeout = -1;
    if (ts32) {
        timeout = (ts32->tv_sec * 1000) + (ts32->tv_nsec / 1000000);
    }
    return errno_ret(poll(pfds, npfds, timeout));
}

static int64_t rvvm_sys_getdents64(int fd, void* dirp, size_t size)
{
    /* SYS_getdents64 is served by the host layer: the native syscall on Linux,
     * the Win32 directory walk in posix_shim.c elsewhere. The guest structure
     * (struct uapi_linux_dirent64 above) is filled by that layer. */
    int64_t ret = errno_ret(syscall(SYS_getdents64, fd, dirp, size));
    //rvvm_warn("getdents64(%d, %p, %ld) -> %ld (%s)", fd, dirp, size, ret, (ret < 0) ? strerror(errno) : "Success");
    return ret;
}

#define UAPI_PROT_READ  0x1
#define UAPI_PROT_WRITE 0x2
#define UAPI_PROT_EXEC  0x4

#define UAPI_MAP_SHARED          0x000001
#define UAPI_MAP_PRIVATE         0x000002
#define UAPI_MAP_FIXED           0x000010
#define UAPI_MAP_ANON            0x000020
#define UAPI_MAP_FIXED_NOREPLACE 0x100000

#define UAPI_MAP_ILLEGAL         0xE00000

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

/* Guest structs that embed pointers need conversion before being handed to the
 * host libc: writev() dereferences iov_base, and it holds a guest address.
 * This used to work because guest memory was identity-mapped onto the host. */

#define IOV_STACK_MAX 16
#define IOV_HARD_MAX  1024

// Translate a guest iovec array into a host one; may return @stack_buf
static struct iovec* rvvm_iovec_from_guest(const struct uapi_iovec* giov, size_t count, struct iovec* stack_buf)
{
    struct iovec* hiov = stack_buf;
    if (count > IOV_STACK_MAX) {
        hiov = safe_new_arr(struct iovec, count);
    }
    for (size_t i = 0; i < count; ++i) {
        hiov[i].iov_base = to_ptr_sz(giov[i].base, giov[i].len);
        hiov[i].iov_len  = giov[i].len;
        if (giov[i].len && !hiov[i].iov_base) {
            // A segment outside guest RAM: fail the whole call instead of
            // letting the host (or the host kernel) touch a wild pointer.
            if (hiov != stack_buf) {
                free(hiov);
            }
            return NULL;
        }
    }
    return hiov;
}

static void rvvm_iovec_release(struct iovec* hiov, struct iovec* stack_buf)
{
    if (hiov != stack_buf) {
        free(hiov);
    }
}

// Translate a guest msghdr (and the iovec array it points to) into host form
static void rvvm_msghdr_from_guest(struct msghdr* host, const struct uapi_msghdr* guest,
                                   struct iovec* iov_buf, size_t iov_count)
{
    memset(host, 0, sizeof(*host));
    host->msg_name       = guest->name ? to_ptr_sz(guest->name, guest->namelen) : NULL;
    host->msg_namelen    = guest->namelen;
    host->msg_iov        = iov_buf;
    host->msg_iovlen     = EVAL_MIN(guest->iovlen, iov_count);
    host->msg_control    = guest->control ? to_ptr_sz(guest->control, guest->controllen) : NULL;
    host->msg_controllen = guest->controllen;
    host->msg_flags      = guest->flags;
}

#define UAPI_MREMAP_MAYMOVE 1

static rvvm_addr_t rvvm_sys_mmap(rvvm_addr_t addr, size_t size, int prot, int flags, int fd, uint64_t offset)
{
    /* NOTE: %llx, not %lx - on Windows host `long` is 32-bit, so %lx silently
     * prints only the low half of a 64-bit address and makes hints look bogus. */
    rvvm_info("sys_mmap(addr=%llx size=%llx prot=%x flags=%x fd=%d off=%llx)",
              (long long)addr, (long long)size, prot, flags, fd, (long long)offset);
    if (flags & UAPI_MAP_ILLEGAL) {
        return -UAPI_EINVAL;
    }
    if (!size) {
        return -UAPI_EINVAL;
    }

    spin_lock(&uctx()->guest_lock);
    rvvm_addr_t ret = 0;
    if (!guest_range_alloc(&ret, addr, size, !!(flags & (UAPI_MAP_FIXED | UAPI_MAP_FIXED_NOREPLACE)))) {
        spin_unlock(&uctx()->guest_lock);
        /* Surface the exact parameters of a failed mapping: a guest-side
         * malloc() failure only reports ENOMEM, the interesting part (hint
         * address / size / flags) lives here. */
        rvvm_warn("sys_mmap: guest map failed -> ENOMEM (addr=%llx size=%llx prot=%x flags=%x fixed=%d)",
                  (long long)addr, (long long)size, prot, flags,
                  !!(flags & UAPI_MAP_FIXED));
        return -UAPI_ENOMEM;
    }
    spin_unlock(&uctx()->guest_lock);

    if (!(flags & UAPI_MAP_ANON) && fd >= 0) {
        /* File-backed mapping: the guest can only see its own buffer, so read
         * the requested window in. Write-back to the file is not emulated. */
        if (size >= (1u << 20)) {
            rvvm_warn("DBG mmap: pread %llx bytes fd=%d off=%llx -> guest %llx",
                      (long long)size, fd, (long long)offset, (long long)ret);
        }
        ssize_t rd = pread(fd, to_ptr(ret), size, (off_t)offset);
        if (size >= (1u << 20)) {
            rvvm_warn("DBG mmap: pread returned %lld (short read = past EOF)", (long long)rd);
        }
        if (rd < 0) {
            rvvm_warn("sys_mmap: pread failed (fd=%d off=%llx size=%llx)",
                      fd, (long long)offset, (long long)size);
            int err = last_errno();
            spin_lock(&uctx()->guest_lock);
            guest_range_free(ret, size);
            spin_unlock(&uctx()->guest_lock);
            return err;
        }
    }

    rvvm_info("sys_mmap: -> %llx (hint=%llx size=%llx prot=%x flags=%x fixed=%d)",
              (long long)ret, (long long)addr, (long long)size, prot, flags,
              !!(flags & UAPI_MAP_FIXED));
    return ret;
}

static int rvvm_sys_munmap(rvvm_addr_t addr, size_t size)
{
    spin_lock(&uctx()->guest_lock);
    guest_range_free(addr, size);
    spin_unlock(&uctx()->guest_lock);
    return 0;
}

extern uint64_t __thread_selfid(void);

static int rvvm_sys_gettid(void)
{
#if defined(__APPLE__)
    return __thread_selfid();
#else
    return gettid();
#endif
}

static int rvvm_sys_getuid(void)
{
    rvvm_userland_t* ctx = uctx();
    if (ctx->fake_root) return ctx->fake_uid;
    return errno_ret(getuid());
}

static int rvvm_sys_getgid(void)
{
    rvvm_userland_t* ctx = uctx();
    if (ctx->fake_root) return ctx->fake_gid;
    return errno_ret(getgid());
}

static int rvvm_sys_setuid(int uid)
{
    rvvm_userland_t* ctx = uctx();
    if (ctx->fake_root) {
        ctx->fake_uid = uid;
        return 0;
    }
    return errno_ret(setuid(uid));
}

static int rvvm_sys_setgid(int gid)
{
    rvvm_userland_t* ctx = uctx();
    if (ctx->fake_root) {
        ctx->fake_gid = gid;
        return 0;
    }
    return errno_ret(setgid(gid));
}

static int rvvm_sys_getresuid(int* ruid, int* euid, int* suid)
{
    if (ruid) *ruid = rvvm_sys_getuid();
    if (euid) *euid = rvvm_sys_getuid();
    if (suid) *suid = rvvm_sys_getuid();
    return 0;
}

static int rvvm_sys_getresgid(int* rgid, int* egid, int* sgid)
{
    if (rgid) *rgid = rvvm_sys_getgid();
    if (egid) *egid = rvvm_sys_getgid();
    if (sgid) *sgid = rvvm_sys_getgid();
    return 0;
}

static rvvm_addr_t rvvm_sys_getcwd(char* buffer, size_t size)
{
    char tmp[UAPI_PATH_MAX] = {0};
    if (!getcwd(tmp, size)) {
        return last_errno();
    }
    /* The kernel returns the string length (NUL excluded) and libc getcwd()
     * treats a 0 as ENOENT, so report it explicitly instead of forwarding
     * whatever the copy helper happens to return. */
    unwrap_path(buffer, tmp, size);
    return rvvm_strlen(buffer);
}

static rvvm_addr_t rvvm_sys_readlinkat(int dirfd, const char* pathname, char* buffer, size_t size)
{
    char tmp[UAPI_PATH_MAX] = {0};
    if (readlinkat(dirfd, wrap_path(tmp, pathname), tmp, size) < 0) {
        return last_errno();
    }
    return unwrap_path(buffer, tmp, size);
}

/* Syscall traces below use rvvm_info(), which is hidden at the default
 * LOG_WARN level and shown only when verbose logging is enabled. */

static void* rvvm_user_thread_wrap(void* arg)
{
    rvvm_user_thread_t* thread = arg;
    rvvm_hart_t* cpu = thread->cpu;
    bool running = true;

    current_user_hart = cpu;
    // Bind this guest thread to its instance context (see uctx())
    tls_userland = rvvm_userland_ctx(cpu->machine);

    userland_thread_register(thread);

    char path_buf[UAPI_PATH_MAX] = {0};
    char path_buf1[UAPI_PATH_MAX] = {0};

    // Set up thread tid, child_tid
    atomic_store_uint32(&thread->tid, rvvm_sys_gettid());
    if (thread->child_settid) {
        atomic_store_uint32(thread->child_settid, atomic_load_uint32(&thread->tid));
    }

    while (running) {
        if (atomic_load_uint32(&thread->finished)) {
            // Userland is shutting down - leave the run loop cleanly
            break;
        }
        // Suspended by the host (rvvm_user_suspend): park here until resume
        userland_park_if_suspended(thread);
        if (atomic_load_uint32(&thread->finished)) {
            // Shutdown raced the suspend - do not re-enter the guest to unwind
            break;
        }
        rvvm_addr_t cause = rvvm_run_user_thread(cpu);
        rvvm_warn("DBG loop: interpreter returned cause=%llx pc=%llx a7=%llx finished=%u",
                  (long long)cause, (long long)rvvm_read_cpu_reg(cpu, RVVM_REGID_PC),
                  (long long)rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 17),
                  atomic_load_uint32(&thread->finished));
        if (atomic_load_uint32(&thread->finished)) {
            /*
             * A stop/exit request kicked this vCPU out of the interpreter (see
             * userland_process_exit(): every thread is marked finished and
             * queued for a hart pause). As with the suspend case below, the
             * cause we got back describes whatever state the hart was in when
             * it was interrupted, not a fresh trap - and the guest registers
             * (a0 in particular, which SYS_ANDROID_CALL aliases as both its
             * sub-command input and its return value) may hold leftovers from
             * the previous dispatch. Dispatching that would execute a phantom
             * syscall with a garbage number. Nothing observable is lost by
             * dropping it: the process is being torn down, so no guest can
             * read the result. Unwind instead of handling the trap.
             */
            break;
        }
        if (atomic_load_uint32(&uctx()->userland_suspend)) {
            /*
             * A suspend request kicked this vCPU out of the interpreter. The
             * value we got back describes whatever trapped last, not a fresh
             * trap, so it must not be interpreted as a syscall. Dropping it
             * loses nothing: the vCPU was not advanced, so on resume it
             * re-enters at the same PC and the trap (if any) reoccurs.
             */
            continue;
        }
        if (cause == 8) {
            // Handle syscall trap
            rvvm_addr_t a0 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 10);
            rvvm_addr_t a1 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 11);
            rvvm_addr_t a2 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 12);
            rvvm_addr_t a3 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 13);
            rvvm_addr_t a4 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 14);
            rvvm_addr_t a5 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 15);
            rvvm_addr_t a7 = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 17);
            switch (a7) {
                case 17: { // getcwd
                    rvvm_info("sys_getcwd(%lx, %lx)", a0, a1);
                    char* buf = to_ptr_sz(a0, a1);
                    a0 = buf ? rvvm_sys_getcwd(buf, a1) : (rvvm_addr_t)-UAPI_EFAULT;
                    break;
                }
#ifdef __linux__
                case 19: // eventfd2
                    rvvm_info("sys_eventfd2(%lx, %lx)", a0, a1);
                    a0 = errno_ret(eventfd(a0, a1));
                    break;
#endif
#if defined(__linux__) || defined(_WIN32)
                case 20: // epoll_create1
                    rvvm_info("sys_epoll_create1(%lx)", a0);
                    a0 = errno_ret(epoll_create1(a0));
                    break;
                case 21: { // epoll_ctl
                    // Host (x86-64) epoll_event is packed (12 bytes) while
                    // RISC-V UAPI one is naturally aligned (16 bytes) - convert
                    rvvm_info("sys_epoll_ctl(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    struct epoll_event host_ev;
                    struct epoll_event* host_ev_ptr = NULL;
                    if (a3) {
                        const struct uapi_epoll_event* guest_ev = to_ptr_sz(a3, sizeof(*guest_ev));
                        if (!guest_ev) {
                            a0 = -UAPI_EFAULT;
                            break;
                        }
                        host_ev.events = guest_ev->event;
                        host_ev.data.u64 = guest_ev->data.u64;
                        host_ev_ptr = &host_ev;
                    }
                    a0 = errno_ret(epoll_ctl(a0, a1, a2, host_ev_ptr));
                    break;
                }
                case 22: { // epoll_pwait (sigmask ignored)
                    rvvm_info("sys_epoll_pwait(%lx, %lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4, a5);
                    if (!a1 && a2) {
                        // A NULL event buffer is refused up front, like the kernel
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    struct epoll_event stack_evs[128];
                    struct epoll_event* host_evs = stack_evs;
                    size_t maxev = a2 > 128 ? 128 : a2;
                    if (a2 > 128) {
                        host_evs = malloc(a2 * sizeof(struct epoll_event));
                        if (!host_evs) {
                            a0 = -UAPI_ENOMEM;
                            break;
                        }
                        maxev = a2;
                    }
                    a0 = errno_ret(epoll_wait(a0, host_evs, maxev, a3));
                    if ((ssize_t)a0 > 0 && a1) {
                        struct uapi_epoll_event* guest_evs =
                            to_ptr_sz(a1, (size_t)a0 * sizeof(struct uapi_epoll_event));
                        if (guest_evs) {
                            for (size_t i = 0; i < (size_t)a0; i++) {
                                guest_evs[i].event = host_evs[i].events;
                                guest_evs[i].data.u64 = host_evs[i].data.u64;
                            }
                        } else {
                            a0 = -UAPI_EFAULT;
                        }
                    }
                    if (host_evs != stack_evs) free(host_evs);
                    break;
                }
#endif
                case 23: // dup
                    rvvm_info("sys_dup(%ld)", a0);
                    a0 = errno_ret(dup(a0));
                    break;
                case 24: // dup3
                    rvvm_info("sys_dup3(%ld, %ld, %lx)", a0, a1, a2);
                    a0 = errno_ret(dup2(a0, a1));
                    break;
                case 25: // fcntl64
                    rvvm_info("sys_fcntl64(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(fcntl(a0, a1, a2));
                    break;
                case 29: // ioctl
                    // TODO: I sure hope not many ioctl() interfaces need struct conversion...
                    rvvm_info("sys_ioctl(%ld, %lx, %lx)", a0, a1, a2);
                    if ((a0 == 0 || a0 == 1 || a0 == 2) && uctx()->tty) {
                        // Virtual TTY rendered by the host: answer the termios
                        // probes guest libc makes for isatty() itself instead
                        // of forwarding them to the host fd. fd 0 is included
                        // because it is the same console, only the input half.
                        a0 = user_tty_ioctl(a1, a2 ? to_ptr(a2) : NULL);
                        break;
                    }
                    a0 = errno_ret(ioctl(a0, a1, a2));
                    break;
                case 32: // flock
                    rvvm_info("sys_flock(%ld, %lx)", a0, a1);
                    a0 = errno_ret(flock(a0, a1));
                    break;
                case 33: // mknodat
                    rvvm_info("sys_mknodat(%ld, %s, %lx, %lx)", a0, to_str(a1), a2, a3);
                    a0 = errno_ret(mknodat(a0, wrap_path(path_buf, to_str(a1)), a2, a3));
                    break;
                case 34: // mkdirat
                    rvvm_info("sys_mkdirat(%ld, %s, %lx)", a0, to_str(a1), a2);
                    a0 = errno_ret(mkdirat(a0, wrap_path(path_buf, to_str(a1)), a2));
                    break;
                case 35: // unlinkat
                    rvvm_info("sys_unlinkat(%ld, %s, %lx)", a0, to_str(a1), a2);
                    a0 = errno_ret(unlinkat(a0, wrap_path(path_buf, to_str(a1)), a2));
                    break;
                case 36: // symlinkat
                    rvvm_info("sys_symlinkat(%s, %ld, %s)", to_str(a0), a1, to_str(a2));
                    a0 = errno_ret(symlinkat(wrap_path(path_buf, to_str(a0)), a1,
                                             wrap_path(path_buf1, to_str(a2))));
                    break;
                case 37: // linkat
                    rvvm_info("sys_linkat(%ld, %s, %ld, %s, %lx)", a0, to_str(a1), a2, to_str(a3), a4);
                    a0 = errno_ret(linkat(a0, wrap_path(path_buf, to_str(a1)),
                                          a2, wrap_path(path_buf1, to_str(a3)), a4));
                    break;
                case 43: { // statfs64
                    struct statfs stfs = {0};
                    struct uapi_statfs64* out = to_ptr_sz(a1, sizeof(*out));
                    rvvm_info("sys_statfs64(%s, %lx, %lx)", to_str(a0), a1, a2);
                    if (!out) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(statfs(wrap_path(path_buf, to_str(a0)), &stfs));
                    uapi_statfs64_convert(out, &stfs);
                    break;
                }
                case 44: { // fstatfs64
                    struct statfs stfs = {0};
                    struct uapi_statfs64* out = to_ptr_sz(a1, sizeof(*out));
                    rvvm_info("sys_fstatfs64(%ld, %lx, %lx)", a0, a1, a2);
                    if (!out) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(fstatfs(a0, &stfs));
                    uapi_statfs64_convert(out, &stfs);
                    break;
                }
                case 45: // truncate64
                    rvvm_info("sys_truncate64(%s, %lx)", to_str(a0), a1);
                    a0 = errno_ret(truncate(wrap_path(path_buf, to_str(a0)), a1));
                    break;
                case 46: // ftruncate64
                    rvvm_info("sys_ftruncate64(%ld, %lx)", a0, a1);
                    a0 = errno_ret(ftruncate(a0, a1));
                    break;
#ifdef __linux__
                case 47: // fallocate
                    rvvm_info("sys_fallocate(%ld, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(fallocate(a0, a1, a2, a3));
                    break;
#endif
                case 48: // faccessat
                    rvvm_info("sys_faccessat(%ld, %s, %lx)", a0, to_str(a1), a2);
                    a0 = errno_ret(faccessat(a0, wrap_path(path_buf, to_str(a1)), a2, 0));
                    break;
                case 49: // chdir
                    rvvm_info("sys_chdir(%s)", to_str(a0));
                    a0 = errno_ret(chdir(wrap_path(path_buf, to_str(a0))));
                    break;
                case 50: // fchdir
                    rvvm_info("sys_fchdir(%ld)", a0);
                    a0 = errno_ret(fchdir(a0));
                    break;
                case 52: // fchmod
                    rvvm_info("sys_fchmodat(%ld, %lx)", a0, a1);
                    a0 = errno_ret(fchmod(a0, a1));
                    break;
                case 53: // fchmodat
                    rvvm_info("sys_fchmodat(%ld, %s, %lx)", a0, to_str(a1), a2);
                    a0 = errno_ret(fchmodat(a0, wrap_path(path_buf, to_str(a1)), a2, 0));
                    break;
                case 54: // fchownat
                    if (uctx()->fake_root) {
                        a0 = 0;
                    } else {
                        rvvm_info("sys_fchownat(%ld, %s, %lx, %lx, %lx)", a0, to_str(a1), a2, a3, a4);
                        a0 = errno_ret(fchownat(a0, wrap_path(path_buf, to_str(a1)), a2, a3, a4));
                    }
                    break;
                case 55: // fchown
                    if (uctx()->fake_root) {
                        a0 = 0;
                    } else {
                        rvvm_info("sys_fchownat(%ld, %lx, %lx)", a0, a1, a2);
                        a0 = errno_ret(fchown(a0, a1, a2));
                    }
                    break;
                case 56: { // openat
                    const char* path = to_str(a1);
                    rvvm_info("sys_openat(%ld, %s, %lx, %lx)", a0, path, a2, a3);
                    if (!path && !(a2 & AT_EMPTY_PATH)) {
                        /* openat would dereference the NULL path */
                        a0 = -EFAULT;
                    } else {
                        /* NULL path with AT_EMPTY_PATH refers to the dirfd */
                        const char* host_path = path ? wrap_path(path_buf, path) : path;
                        a0 = errno_ret(openat(a0, host_path, uapi_open_flags(a2), a3));
                        if (host_path && a0 < 0x1000) {
                            rvvm_warn("DBG openat %s -> fd %ld", host_path, (long)a0);
                        }
                    }
                    break;
                }
                case 57: // close
                    a0 = errno_ret(close(a0));
                    break;
                case 59: { // pipe2
                    rvvm_info("sys_pipe2(%lx, %lx)", a0, a1);
                    int* fds = to_ptr_sz(a0, sizeof(int) * 2);
                    a0 = fds ? errno_ret(pipe(fds)) : (rvvm_addr_t)-UAPI_EFAULT;
                    break;
                }
                case 61: { // getdents64
                    void* dirp = a2 ? to_ptr_sz(a1, a2) : NULL;
                    a0 = (a2 && !dirp) ? (rvvm_addr_t)-UAPI_EFAULT : (rvvm_addr_t)rvvm_sys_getdents64(a0, dirp, a2);
                    break;
                }
                case 62: // lseek
                    a0 = errno_ret(lseek(a0, a1, a2));
                    break;
                case 63: { // read
                    void* buf = a2 ? to_ptr_sz(a1, a2) : NULL;
                    if (a2 && !buf) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (a0 == 0 && uctx()->tty) {
                        // fd 0 is the virtual TTY: the guest's stdin comes from
                        // the host keyboard (rvvm_user_tty_input), not from the
                        // host process's own stdin. Blocks until a line has been
                        // assembled by the line discipline.
                        a0 = (rvvm_addr_t)user_tty_read(uctx(), buf, a2, true);
                        break;
                    }
                    a0 = errno_ret(read(a0, buf, a2));
                    break;
                }
                case 64: { // write
                    void* wbuf = a2 ? to_ptr_sz(a1, a2) : NULL;
                    if (a2 && !wbuf) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (a0 == 1 || a0 == 2) {
                        // fd 1/2: feed the virtual TTY parser first (no-op unless a
                        // host injected a VTerm or registered a tty callback). The
                        // bytes then continue to the host's io_callback / the host
                        // fd as usual, so the guest console also reaches whatever
                        // sink the host installed - logcat + the Java console on
                        // Android, stdout on win32. A host that wants the virtual
                        // TTY to be the only sink consumes the bytes in its own
                        // io_callback instead of leaning on this path.
                        user_tty_write(uctx(), a0, wbuf, a2);
                    }
                    if (uctx()->io_callback) {
                        ssize_t ret = uctx()->io_callback(a0, wbuf, a2);
                        a0 = errno_ret(ret);
                    } else {
                        a0 = errno_ret(write(a0, wbuf, a2));
                    }
                    break;
                }
                case 65: // readv
                case 66: { // writev
                    // The iovec array is converted: iov_base is a guest address
                    if (a2 > IOV_HARD_MAX) {
                        a0 = -UAPI_EINVAL;
                        break;
                    }
                    struct iovec  stack_iov[IOV_STACK_MAX] = {0};
                    const struct uapi_iovec* giov = to_ptr_sz(a1, a2 * sizeof(*giov));
                    struct iovec* hiov = giov ? rvvm_iovec_from_guest(giov, a2, stack_iov) : NULL;
                    if (!hiov) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (a7 == 65) {
                        if (a0 == 0 && uctx()->tty) {
                            // fd 0 is the virtual TTY: serve readv() from the
                            // keyboard queue. The first non-empty segment blocks
                            // until input arrives, the rest drain what is
                            // already queued so the call returns as soon as the
                            // console is empty instead of blocking twice.
                            ssize_t total = 0;
                            for (int i = 0; i < (int)a2; i++) {
                                if (!hiov[i].iov_len) {
                                    continue;
                                }
                                ssize_t r = user_tty_read(uctx(), hiov[i].iov_base,
                                                          hiov[i].iov_len, total == 0);
                                if (r < 0) {
                                    total = total ? total : r;
                                    break;
                                }
                                if (r == 0) {
                                    break; // EOF, or the console drained
                                }
                                total += r;
                                if (!user_tty_readable(uctx())) {
                                    break;
                                }
                            }
                            a0 = errno_ret(total);
                        } else {
                            a0 = errno_ret(readv(a0, hiov, a2));
                        }
                    } else if (a0 == 1 || a0 == 2) {
                        /* stdout/stderr: mirror every segment into the virtual
                         * TTY parser (when one exists) and forward the same
                         * bytes to the host's io_callback / host fd, exactly
                         * like the write() path above - the TTY must not
                         * swallow the guest console. */
                        ssize_t total = 0;
                        for (int i = 0; i < (int)a2; i++) {
                            user_tty_write(uctx(), a0, hiov[i].iov_base, hiov[i].iov_len);
                            ssize_t r = uctx()->io_callback
                                        ? uctx()->io_callback(a0, hiov[i].iov_base, hiov[i].iov_len)
                                        : writev(a0, &hiov[i], 1);
                            if (r < 0) { total = r; break; }
                            total += r;
                        }
                        a0 = errno_ret(total);
                    } else {
                        a0 = errno_ret(writev(a0, hiov, a2));
                    }
                    rvvm_iovec_release(hiov, stack_iov);
                    break;
                }
                case 67: { // pread64
                    void* buf = a2 ? to_ptr_sz(a1, a2) : NULL;
                    a0 = (a2 && !buf) ? (rvvm_addr_t)-UAPI_EFAULT : errno_ret(pread(a0, buf, a2, a3));
                    break;
                }
                case 68: { // pwrite64
                    const void* buf = a2 ? to_ptr_sz(a1, a2) : NULL;
                    a0 = (a2 && !buf) ? (rvvm_addr_t)-UAPI_EFAULT : errno_ret(pwrite(a0, buf, a2, a3));
                    break;
                }
                case 72: // pselect6_time32
                    a0 = rvvm_sys_select_time32(a0, to_ptr(a1), to_ptr(a2), to_ptr(a3), to_ptr(a4));
                    break;
                case 73: // ppoll_time32
                    a0 = rvvm_sys_poll_time32(to_ptr(a0), a1, to_ptr(a2));
                    break;
                case 78: // readlinkat
                    rvvm_info("sys_readlinkat(%ld, %s, %lx, %lx)", a0, to_str(a1), a2, a3);
                    a0 = rvvm_sys_readlinkat(a0, to_str(a1), to_ptr(a2), a3);
                    break;
                case 79: { // newfstatat
                    struct stat st = {0};
                    const char* path = to_str(a1);
                    struct uapi_stat* out = to_ptr_sz(a2, sizeof(*out));
                    int ret;
                    rvvm_info("sys_newfstatat(%ld, %s, %lx, %lx)", a0, path, a2, a3);
                    if (!out) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (!path && !(a3 & AT_EMPTY_PATH)) {
                        /* fstatat would dereference the NULL path */
                        ret = -1;
                        errno = EFAULT;
                    } else {
                        /* NULL path with AT_EMPTY_PATH means "fstat the fd";
                         * fstatat() accepts it but wrap_path() would not. */
                        ret = fstatat(a0, path ? wrap_path(path_buf, path) : path, &st, a3);
                    }
                    a0 = errno_ret(ret);
                    uapi_stat_convert(out, &st);
                    break;
                }
                case 80: { // newfstat
                    struct stat st = {0};
                    struct uapi_stat* out = to_ptr_sz(a1, sizeof(*out));
                    const long fd = (long)a0;
                    rvvm_info("sys_newfstat(%ld, %lx)", a0, a1);
                    if (!out) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(fstat(fd, &st));
                    rvvm_warn("DBG newfstat fd=%ld ret=%ld host_size=%lld", fd, (long)a0, (long long)st.st_size);
                    uapi_stat_convert(out, &st);
                    break;
                }
                case 82: // fsync
                    a0 = errno_ret(fsync(a0));
                    break;
                case 83: // fdatasync
                    a0 = errno_ret(fsync(a0));
                    break;
                case 88: // utimensat - ignore
                    a0 = 0;
                    break;
                case 90: // capget - stub
                    if (a1) {
                        struct uapi_cap_data_struct* cap = to_ptr(a1);
                        memset(cap, 0, sizeof(*cap));
                    }
                    a0 = 0;
                    break;
                case 91: // capset - ignore
                    a0 = 0;
                    break;
                case 93: // exit
                    rvvm_warn("sys_exit(%ld) @ PC %lx", (long)a0, rvvm_read_cpu_reg(cpu, RVVM_REGID_PC));
                    if (uctx()->exit_callback) {
                        if (thread == uctx()->userland_main_thread) {
                            // Linux semantics: main thread exit terminates the process
                            userland_process_exit(uctx(), (int)a0, thread);
                        } else {
                            // Secondary thread exit: stop this vCPU only
                            atomic_store_uint32(&thread->finished, 1);
                        }
                    } else {
                        _Exit(a0);
                    }
                    break;
                case 94: // exit_group
                    rvvm_warn("sys_exit_group(%ld)", (long)a0);
                    if (uctx()->exit_callback) {
                        userland_process_exit(uctx(), (int)a0, thread);
                    } else {
                        _Exit(a0);
                    }
                    break;
                case 96: // set_tid_address
                    thread->child_cleartid = to_ptr(a0);
                    a0 = atomic_load_uint32(&thread->tid);
                    break;
                case 98: // futex
                {
                    uint32_t* faddr = to_ptr(a0);
                    rvvm_warn("DBG futex: uaddr=%llx op=%llx val=%llx timeout=%llx uaddr2=%llx val3=%llx word=%x ra=%llx sp=%llx",
                              (long long)a0, (long long)a1, (long long)a2, (long long)a3, (long long)a4, (long long)a5,
                              faddr ? *faddr : 0xdeadbeef,
                              (long long)rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 1),
                              (long long)rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 2));
                    a0 = rvvm_sys_futex(faddr, a1, a2, (const struct uapi_timespec*)to_ptr(a3), to_ptr(a4), a5);
                    break;
                }
                case 99: // set_robust_list
                    // TODO: Implement this
                    rvvm_info("sys_set_robust_list(%lx, %lx)", a0, a1);
                    a0 = 0;
                    break;
                case 101: // nanosleep
                {
                    struct timespec req, rem = { 0, 0 };
                    struct uapi_timespec* grem = to_ptr(a1);
                    if (!uapi_ts_to_host(&req, to_ptr(a0))) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(nanosleep(&req, grem ? &rem : NULL));
                    // rem is only filled in when the sleep is interrupted, but
                    // the guest gets it either way
                    if (grem) {
                        uapi_ts_from_host(grem, &rem);
                    }
                    break;
                }
                case 103: // setitimer
                {
                    struct itimerval newval = { 0 }, oldval = { 0 };
                    struct uapi_itimerval* gnew = to_ptr(a1);
                    struct uapi_itimerval* gold = to_ptr(a2);
                    rvvm_info("sys_setitimer(%lx, %lx, %lx)", a0, a1, a2);
                    uapi_itimerval_to_host(&newval, gnew);
                    a0 = errno_ret(setitimer(a0, gnew ? &newval : NULL, gold ? &oldval : NULL));
                    if (gold) {
                        uapi_itimerval_from_host(gold, &oldval);
                    }
                    break;
                }
                case 113: // clock_gettime
                {
                    struct timespec ts;
                    rvvm_info("sys_clock_gettime(%lx, %lx)", a0, a1);
                    int ret = clock_gettime(a0, &ts);
                    if (ret == 0) {
                        // A NULL target has nothing to write back to; that is
                        // not worth faulting on, POSIX allows it for getres/clock_gettime alike
                        struct uapi_timespec* out = to_ptr(a1);
                        if (out) {
                            uapi_ts_from_host(out, &ts);
                        }
                    }
                    a0 = errno_ret(ret);
                    break;
                }
                case 114: // clock_getres
                {
                    struct timespec ts;
                    int ret;
                    rvvm_info("sys_clock_getres(%lx, %lx)", a0, a1);
                    ret = clock_getres(a0, &ts);
                    if (ret == 0) {
                        struct uapi_timespec* out = to_ptr(a1);
                        if (out) {
                            uapi_ts_from_host(out, &ts);
                        }
                    }
                    a0 = errno_ret(ret);
                    break;
                }
#ifdef __linux__
                case 115: // clock_nanosleep
                {
                    struct timespec req, rem = { 0, 0 };
                    struct uapi_timespec* grem = to_ptr(a3);
                    rvvm_info("sys_clock_nanosleep(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    if (!uapi_ts_to_host(&req, to_ptr(a2))) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(clock_nanosleep(a0, a1, &req, grem ? &rem : NULL));
                    if (grem) {
                        uapi_ts_from_host(grem, &rem);
                    }
                    break;
                }
#endif
                case 118: // sched_setparam - ignore
                case 119: // sched_setscheduler - ignore
                case 120: // sched_getscheduler - ignore
                    a0 = 0;
                    break;
                case 121: { // sched_getparam - stub
                    if (a1) {
                        struct uapi_sched_param* param = to_ptr_sz(a1, sizeof(*param));
                        if (!param) {
                            a0 = -UAPI_EFAULT;
                            break;
                        }
                        memset(param, 0, sizeof(*param));
                    }
                    a0 = 0;
                    break;
                }
                case 122: // sched_setaffinity - ignore
                    a0 = 0;
                    break;
                case 123: { // sched_getaffinity - pass through the host affinity mask,
                    // guest allocators (f.e. Zig SmpAllocator) size per-CPU arenas by it
                    // A mask shorter than one word cannot hold a single CPU; the
                    // kernel rejects the size before it ever looks at the pointer
                    if (a1 < sizeof(unsigned long)) {
                        a0 = -UAPI_EINVAL;
                        break;
                    }
                    // The mask is not optional (there is nothing to report into
                    // and no way to answer the question without writing it)
                    void* mask = to_ptr_sz(a2, a1);
                    if (!mask) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    memset(mask, 0, a1);
                    cpu_set_t host_mask;
                    CPU_ZERO(&host_mask);
                    if (!sched_getaffinity(0, sizeof(host_mask), &host_mask)) {
                        size_t copy_len = sizeof(host_mask) < a1 ? sizeof(host_mask) : a1;
                        memcpy(mask, &host_mask, copy_len);
                    } else {
                        *(uint8_t*)mask = 1;
                    }
                    // Syscall ABI: return the amount of bytes written into the mask
                    a0 = sizeof(unsigned long);
                    break;
                }
                case 124: // sched_yield
                    sleep_ms(0);
                    a0 = 0;
                    break;
                case 125: // sched_get_priority_max - ignore
                case 126: // sched_get_priority_min - ignore
                    a0 = 0;
                    break;
                case 129: // kill
                    rvvm_warn("sys_kill(%lx, %lx)", a0, a1);
                    a0 = errno_ret(kill(a0, a1));
                    break;
#ifdef __linux__
                case 130: // tkill
                    rvvm_warn("sys_tkill(%lx, %lx)", a0, a1);
                    a0 = errno_ret(tgkill(getpid(), a0, a1));
                    break;
                case 131: // tgkill
                    rvvm_warn("sys_tgkill(%lx, %lx, %ld)", a0, a1, a2);
                    a0 = errno_ret(tgkill(a0, a1, a2));
                    break;
#endif
                case 134: { // rt_sigaction
                    rvvm_userland_t* ctx = uctx();
                    struct sigaction sa = {0};
                    rvvm_info("sys_rt_sigaction(%ld, %lx, %lx, %lx)", a0, a1, a2, a3);
                    if (a0 < STATIC_ARRAY_SIZE(ctx->siga)) {
                        // a3 is the guest sigsetsize; the whole struct sigaction
                        // must never spill past one siga[] slot
                        size_t copy_len = a3 < sizeof(ctx->siga[0]) ? a3 : sizeof(ctx->siga[0]);
                        void* old = a2 ? to_ptr_sz(a2, copy_len) : NULL;
                        const void* act = a1 ? to_ptr_sz(a1, copy_len) : NULL;
                        if ((a2 && !old) || (a1 && !act)) {
                            a0 = -UAPI_EFAULT;
                            break;
                        }
                        if (old) memcpy(old, &ctx->siga[a0], copy_len);
                        if (act) {
                            memcpy(&ctx->siga[a0], act, copy_len);

                            // Register a shim signal handler
                            if (a0 != 11) {
                                memcpy(&sa.sa_mask, &ctx->siga[a0].mask, 8);
                                sa.sa_flags = ctx->siga[a0].flags & ~SA_SIGINFO;
                                sa.sa_handler = to_ptr(ctx->siga[a0].handler);
                                if (sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN) {
                                    sa.sa_handler = sig_handler;
                                }
                                sigaction(a0, &sa, NULL);
                            }
                        }
                        a0 = 0;
                    } else {
                        a0 = -UAPI_EINVAL;
                    }
                    break;
                }
                case 135: // rt_sigprocmask
                    rvvm_info("sys_rt_sigprocmask(%ld, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(sigprocmask(a0, to_ptr(a1), to_ptr(a2)));
                    break;
                case 137: // rt_sigtimedwait_time32
                    // TODO: Signal handling
                    sleep_ms(-1);
                    a0 = 0;
                    break;
                case 140: // setpriority - ignore
                    a0 = 0;
                    break;
                case 144: // setgid
                    a0 = rvvm_sys_setgid(a0);
                    break;
                case 146: // setuid
                    a0 = rvvm_sys_setuid(a0);
                    break;
                case 147: // setresuid - semi stub
                    a0 = rvvm_sys_setuid(a0);
                    break;
                case 148: { // getresuid - semi stub
                    // Linux has no optional out-parameters here - all three must
                    // be writable, a NULL is EFAULT rather than "don't report it"
                    int* ruid = to_ptr_sz(a0, sizeof(int));
                    int* euid = to_ptr_sz(a1, sizeof(int));
                    int* suid = to_ptr_sz(a2, sizeof(int));
                    if (!ruid || !euid || !suid) {
                        a0 = -UAPI_EFAULT;
                    } else {
                        a0 = rvvm_sys_getresuid(ruid, euid, suid);
                    }
                    break;
                }
                case 149: // setresgid - semi stub
                    a0 = rvvm_sys_setgid(a0);
                    break;
                case 150: { // getresgid - semi stub
                    // Same as getresuid: every pointer is required
                    int* rgid = to_ptr_sz(a0, sizeof(int));
                    int* egid = to_ptr_sz(a1, sizeof(int));
                    int* sgid = to_ptr_sz(a2, sizeof(int));
                    if (!rgid || !egid || !sgid) {
                        a0 = -UAPI_EFAULT;
                    } else {
                        a0 = rvvm_sys_getresgid(rgid, egid, sgid);
                    }
                    break;
                }
                case 151: // setfsuid - ignore
                    a0 = rvvm_sys_getuid();
                    break;
                case 152: // setfsgid - ignore
                    a0 = rvvm_sys_getgid();
                    break;
                case 153: // times
                    // TODO: Struct conversion!
                    rvvm_info("sys_times(%lx)", a0);
                    a0 = errno_ret(times(to_ptr(a0)));
                    break;
#ifdef __linux__
                case 154: // setpgid
                    rvvm_info("sys_setpgid(%lx, %lx)", a0, a1);
                    a0 = errno_ret(setpgid(a0, a1));
                    break;
                case 155: // getpgid
                    rvvm_info("sys_getpgid(%lx)", a0);
                    a0 = errno_ret(getpgid(a0));
                    break;
#endif
                case 157: // setsid
                    rvvm_info("sys_setsid()");
                    a0 = errno_ret(setsid());
                    break;
                case 158: // getgroups
                    rvvm_warn("sys_getgroups(%lx, %lx)", a0, a1);
                    a0 = errno_ret(getgroups(a0, to_ptr(a1)));
                    break;
                case 159: // setgroups
                    if (uctx()->fake_root) {
                        a0 = 0;
                    } else {
                        a0 = errno_ret(setgroups(a0, to_ptr(a1)));
                    }
                    break;
                case 160: { // newuname
                    rvvm_info("sys_newuname(%lx)", a0);
                    if (a0) {
                        // Just lie about the host details
                        struct uapi_new_utsname name = {
                            .sysname = "Linux",
                            .nodename = "rvvm-user",
                            .release = "6.6.6",
                            .version = "RVVM " RVVM_VERSION,
                            .machine = "riscv64",
                        };
                        struct uapi_new_utsname* out = to_ptr_sz(a0, sizeof(*out));
                        if (!out) {
                            a0 = -UAPI_EFAULT;
                            break;
                        }
                        memcpy(out, &name, sizeof(name));
                        a0 = 0;
                    }
                    break;
                }
                case 165: // getrusage
                    rvvm_info("sys_getrusage(%lx, %lx)", a0, a1);
                    a0 = errno_ret(getrusage(a0, to_ptr(a1)));
                    break;
                case 166: // umask
                    rvvm_info("sys_umask(%lx)", a0);
                    a0 = errno_ret(umask(a0));
                    break;
                case 167: // prctl
                    rvvm_info("sys_prctl(%lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4);
                    //a0 = errno_ret(prctl(a0, a1, a2, a3, a4));
                    a0 = 0;
                    break;
                case 169: { // gettimeofday
                    // a1 is struct timezone* - obsolete: the kernel ignores it
                    // and every libc passes NULL, so accept it and drop it.
                    struct uapi_timeval* tv = a0 ? to_ptr_sz(a0, sizeof(*tv)) : NULL;
                    if (a0 && !tv) {
                        a0 = -UAPI_EFAULT;
                    } else {
                        a0 = tv ? rvvm_sys_gettimeofday(tv) : 0;
                    }
                    break;
                }
                case 172: // getpid
                    a0 = errno_ret(getpid());
                    break;
                case 173: // getppid
                    a0 = errno_ret(getppid());
                    break;
                case 174: // getuid
                    a0 = rvvm_sys_getuid();
                    break;
                case 175: // geteuid - semi stub
                    a0 = rvvm_sys_getuid();
                    break;
                case 176: // getgid
                    a0 = rvvm_sys_getgid();
                    break;
                case 177: // getegid - semi stub
                    a0 = rvvm_sys_getgid();
                    break;
                case 178: // gettid
                    a0 = thread->tid;
                    break;
#ifdef __linux__
                case 179: // sysinfo
                    // TODO: struct conversion(?)
                    rvvm_info("sys_sysinfo(%lx)", a0);
                    a0 = errno_ret(sysinfo(to_ptr(a0)));
                    break;
#endif
                case 194: // shmget
                    rvvm_info("sys_shmget(%lx, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(shmget(a0, a1, a2));
                    break;
                case 195: // shmctl
                    // TODO: struct conversion?
                    rvvm_info("sys_shmctl(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(shmctl(a0, a1, to_ptr(a2)));
                    break;
                case 196: // shmat
                    rvvm_info("sys_shmat(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret((size_t)shmat(a0, to_ptr(a1), a2));
                    break;
                case 197: // shmdt
                    rvvm_info("sys_shmdt(%lx)", a0);
                    a0 = errno_ret(shmdt(to_ptr(a0)));
                    break;
                case 198: // socket
                    rvvm_info("sys_socket(%lx, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(socket(a0, a1, a2));
                    break;
                case 199: // socketpair
                    rvvm_info("sys_socketpair(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(socketpair(a0, a1, a2, to_ptr(a3)));
                    break;
                case 200: // bind
                    // TODO struct conversion
                    rvvm_info("sys_bind(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(bind(a0, to_ptr(a1), a2));
                    break;
                case 201: // listen
                    rvvm_info("sys_listen(%ld, %lx)", a0, a1);
                    a0 = errno_ret(listen(a0, a1));
                    break;
                case 202: // accept
                    // TODO: struct conversion(?)
                    rvvm_info("sys_accept(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(accept(a0, to_ptr(a1), to_ptr(a2)));
                    break;
                case 203: // connect
                    // TODO: struct conversion(?)
                    rvvm_info("sys_connect(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(connect(a0, to_ptr(a1), a2));
                    break;
                case 204: // getsockname
                    // TODO: struct conversion(?)
                    rvvm_info("sys_getsockname(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(getsockname(a0, to_ptr(a1), to_ptr(a2)));
                    break;
                case 205: // getpeername
                    // TODO: struct conversion(?)
                    rvvm_info("sys_getpeername(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(getpeername(a0, to_ptr(a1), to_ptr(a2)));
                    break;
                case 206: // sendto
                    // TODO: struct conversion(?)
                    rvvm_info("sys_sendto(%ld, %lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4, a5);
                    a0 = errno_ret(sendto(a0, to_ptr(a1), a2, a3, to_ptr(a4), a5));
                    break;
                case 207: // recvfrom
                    // TODO: struct conversion(?)
                    rvvm_info("sys_recvfrom(%ld, %lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4, a5);
                    a0 = errno_ret(recvfrom(a0, to_ptr(a1), a2, a3, to_ptr(a4), to_ptr(a5)));
                    break;
                case 208: // setsockopt
                    rvvm_info("sys_setsockopt(%ld, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4);
                    a0 = errno_ret(setsockopt(a0, a1, a2, to_ptr(a3), a4));
                    break;
                case 209: // getsockopt
                    rvvm_info("sys_getsockopt(%ld, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4);
                    a0 = errno_ret(getsockopt(a0, a1, a2, to_ptr(a3), to_ptr(a4)));
                    break;
                case 210: // shutdown
                    rvvm_info("sys_shutdown(%ld, %lx)", a0, a1);
                    a0 = errno_ret(shutdown(a0, a1));
                    break;
                case 211: // sendmsg
                case 212: { // recvmsg
                    // struct msghdr embeds three guest pointers plus an iovec array
                    rvvm_info("sys_%smsg(%ld, %lx, %lx)", a7 == 211 ? "send" : "recv", a0, a1, a2);
                    const struct uapi_msghdr* gmsg = to_ptr_sz(a1, sizeof(*gmsg));
                    if (!gmsg) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (gmsg->iovlen > IOV_HARD_MAX) {
                        a0 = -UAPI_EINVAL;
                        break;
                    }
                    struct iovec  stack_iov[IOV_STACK_MAX] = {0};
                    const struct uapi_iovec* giov = to_ptr_sz(gmsg->iov, gmsg->iovlen * sizeof(*giov));
                    struct iovec* hiov = giov ? rvvm_iovec_from_guest(giov, gmsg->iovlen, stack_iov) : NULL;
                    if (!hiov && gmsg->iovlen) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    struct msghdr hmsg = {0};
                    rvvm_msghdr_from_guest(&hmsg, gmsg, hiov, gmsg->iovlen);
                    if (a7 == 211) {
                        a0 = errno_ret(sendmsg(a0, &hmsg, a2));
                    } else {
                        a0 = errno_ret(recvmsg(a0, &hmsg, a2));
                    }
                    rvvm_iovec_release(hiov, stack_iov);
                    break;
                }
                case 214: // brk
                    rvvm_info("sys_brk(%lx)", a0);
                    a0 = (size_t)rvvm_sys_brk(a0);
                    break;
                case 215: // munmap
                    rvvm_info("sys_munmap(%lx, %lx)", a0, a1);
                    a0 = rvvm_sys_munmap(a0, a1);
                    break;
#ifdef __linux__
                case 216: { // mremap
                    rvvm_info("sys_mremap(%lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4);
                    /* Growing in place is only valid while nothing is mapped
                     * right after the old range, which this allocator does not
                     * track - only honour the relocating form. */
                    if (!(a3 & UAPI_MREMAP_MAYMOVE)) {
                        a0 = -UAPI_ENOMEM;
                        break;
                    }
                    size_t copy_len = EVAL_MIN(a1, a2);
                    const void* old = to_ptr_sz(a0, copy_len);
                    if (!old) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    rvvm_addr_t new_addr = 0;
                    spin_lock(&uctx()->guest_lock);
                    bool ok = guest_range_alloc(&new_addr, 0, a2, false);
                    spin_unlock(&uctx()->guest_lock);
                    if (!ok) {
                        a0 = -UAPI_ENOMEM;
                        break;
                    }
                    memcpy(to_ptr(new_addr), old, copy_len);
                    if (a2 > a1) {
                        memset(to_ptr(new_addr + a1), 0, a2 - a1);
                    }
                    spin_lock(&uctx()->guest_lock);
                    guest_range_free(a0, a1);
                    spin_unlock(&uctx()->guest_lock);
                    a0 = new_addr;
                    break;
                }
#endif
                case 220: // clone
                    rvvm_info("sys_clone(%lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4);
                    if (getenv("RVVM_USER_NO_THREADS")) {
                        // Experimental knob: force the guest single-threaded
                        a0 = -UAPI_EAGAIN;
                        break;
                    }
                    a0 = rvvm_sys_clone(cpu, a0, a1, to_ptr(a2), a3, to_ptr(a4));
                    break;
                case 221: { // execve
                    rvvm_info("sys_execve(%lx, %lx, %lx)", a0, a1, a2);
                    if (access(wrap_path(path_buf, to_str(a0)), F_OK)) {
                        a0 = -ENOENT;
                        break;
                    }
                    char** orig_argv = to_ptr(a1);
                    char* new_argv[256] = {"/proc/self/exe", "-user", 0};
                    for (size_t i=2; i<255 && orig_argv[i - 2]; ++i) new_argv[i] = orig_argv[i - 2];
                    new_argv[2] = to_ptr(a0);
                    a0 = errno_ret(execve("/proc/self/exe", new_argv, to_ptr(a2)));
                    break;
                }
                case 222: // mmap
                    a0 = rvvm_sys_mmap(a0, a1, a2, a3, a4, a5);
                    break;
                case 223: // fadvise64_64 - ignore
                    a0 = 0;
                    break;
                case 226: // mprotect
                    rvvm_info("sys_mprotect(%lx, %lx, %x)", a0, a1, a2);
                    /* Guest code is interpreted rather than executed natively,
                     * so guest page protections are not enforced. Changing the
                     * host protection of the shared guest buffer would only
                     * break the emulator's own access to it. */
                    a0 = 0;
                    break;
#if defined(__linux__) || defined(_WIN32)
                case 233: // madvise
                    rvvm_info("sys_madvise(%lx, %lx, %lx)", a0, a1, a2);
                    a0 = 0;
                    break;
                case 242: // accept4
                    // TODO: struct conversion(?)
                    rvvm_info("sys_accept4(%ld, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(accept4(a0, to_ptr(a1), to_ptr(a2), a3));
                    break;
#endif
                case 258: // riscv_hwprobe
                    a0 = -UAPI_ENOSYS;
                    break;
                case 259: // riscv_flush_icache
                    //rvvm_warn("riscv_flush_icache(%lx, %lx, %lx)", a0, a1, a2);
                    rvvm_flush_icache(cpu->machine, a0, a1 - a0);
                    if (getenv("RVVM_JITDUMP") && a1 > a0) {
                        static int jitdump_seq = 0;
                        char dump_name[128];
                        snprintf(dump_name, sizeof(dump_name), "jitdump_%03d_0x%lx.bin", jitdump_seq++, (unsigned long)a0);
                        int dump_fd = open(dump_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (dump_fd >= 0) {
                            const char* dump_src = to_ptr(a0);
                            size_t dump_left = a1 - a0;
                            while (dump_left) {
                                ssize_t dw = write(dump_fd, dump_src, dump_left);
                                if (dw <= 0) break;
                                dump_src += dw; dump_left -= dw;
                            }
                            close(dump_fd);
                        }
                    }
                    a0 = 0;
                    break;
                case 260: { // wait4
                    // TODO: Struct conversion (the rusage argument follows the
                    // guest's 64-bit timeval layout, not the host's)
                    rvvm_info("sys_wait4(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    int* status = a1 ? to_ptr_sz(a1, sizeof(int)) : NULL;
                    if (a1 && !status) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    a0 = errno_ret(wait4(a0, status, a2, to_ptr(a3)));
                    break;
                }
                case 261: // prlimit64 - stub
                    rvvm_info("sys_prlimit64(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    //a0 = errno_ret(prlimit(a0, a1, to_ptr(a2), to_ptr(a3)));
                    a0 = -UAPI_EINVAL;
                    break;
#ifdef __linux__
                case 269: // sendmmsg
                    // TODO: Struct conversion
                    rvvm_info("sys_sendmmsg(%ld, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(sendmmsg(a0, to_ptr(a1), a2, a3));
                    break;
#endif
                case 276: // renameat2
                    rvvm_info("sys_renameat2(%ld, %s, %ld, %s, %lx)", a0, to_str(a1), a2, to_str(a3), a4);
                    a0 = errno_ret(renameat(a0, wrap_path(path_buf, to_str(a1)),
                                            a2, wrap_path(path_buf1, to_str(a3))));
                    break;
                case 277: // seccomp - stub
                    // Hitler SHOT HIMSELF after seeing this...
                    a0 = 0;
                    break;
                case 278: { // getrandom
                    void* buf = a1 ? to_ptr_sz(a0, a1) : NULL;
                    if (a1 && !buf) {
                        a0 = -UAPI_EFAULT;
                        break;
                    }
                    if (buf) {
                        rvvm_randombytes(buf, a1);
                    }
                    a0 = a1;
                    break;
                }
#ifdef __linux__
                case 279: // memfd_create
                    rvvm_info("sys_memfd_create(%s, %lx)", to_str(a0), a1);
                    a0 = errno_ret(memfd_create(to_str(a0), a1));
                    break;
                case 291: { // statx
                    // No field conversion needed: struct statx is a fixed-width
                    // kernel UAPI type, guest and host layouts are identical
                    // (asserted above), unlike struct stat.
                    rvvm_info("sys_statx(%ld, %s, %lx, %lx, %lx)", a0, to_str(a1), a2, a3, a4);
                    struct statx* stx = to_ptr_sz(a4, sizeof(*stx));
                    if (!stx) {
                        a0 = -UAPI_EFAULT;
                    } else {
                        a0 = errno_ret(statx(a0, wrap_path(path_buf, to_str(a1)), a2, a3, stx));
                    }
                    break;
                }
#endif
                case 425: // io_uring_setup - guest event loop falls back to poll on ENOSYS
                    a0 = -UAPI_ENOSYS;
                    break;
                case 435: // clone3
                    // FUCK THIS FUCKING SYSCALL FOR NOW
                    a0 = -UAPI_ENOSYS;
                    break;
                case 436: // close_range - ignore
                    a0 = -UAPI_ENOSYS;
                    break;
                case 439: // faccessat2
                    rvvm_info("sys_faccessat2(%ld, %s, %lx, %lx)", a0, to_str(a1), a2, a3);
                    a0 = errno_ret(faccessat(a0, wrap_path(path_buf, to_str(a1)), a2, a3));
                    break;
                /*
                 * Android NDK API Proxy syscalls (0x10000+)
                 * These are custom syscalls used by vp_ndk_stub to forward
                 * NDK API calls from the Guest to the Host side.
                 *
                 * Dispatch to vp_cmdpost which handles the actual Android API calls.
                 */
                case SYS_ANDROID_CALL: {
                    rvvm_info("cmdpost_dispatch a0=%lx a1=%lx a2=%lx", a0, a1, a2);
                    a0 = cmdpost_dispatch(SYS_ANDROID_CALL, a0, a1, a2, a3, a4, a5, NULL);
                    break;
                }
                case SYS_GL_CALL:
                case SYS_EGL_CALL:
                    rvvm_info("cmdpost_dispatch nr=%lx a0=%lx a1=%lx", a7, a0, a1);
                    a0 = cmdpost_dispatch(a7, a0, a1, a2, a3, a4, a5, NULL);
                    break;
                default:
#ifndef __riscv
                    rvvm_error("Unknown syscall %ld!", a7);
                    a0 = -UAPI_ENOSYS;
#else
                    a0 = errno_ret(syscall(a7, a0, a1, a2, a3, a4, a5));
#endif
                    break;
            }
            if ((int64_t)a0 < 0) {
                rvvm_warn("Syscall %ld failed: %ld", a7, a0);
            }
            rvvm_info("  nr=%ld -> %lx", a7, a0);
            rvvm_write_cpu_reg(cpu, RVVM_REGID_X0 + 10, a0);
            rvvm_write_cpu_reg(cpu, RVVM_REGID_PC, rvvm_read_cpu_reg(cpu, RVVM_REGID_PC) + 4);
        } else if (atomic_load_uint32(&thread->finished)) {
            // Hart was paused by userland shutdown - exit quietly
            break;
        } else {
            // Boom!
            rvvm_warn("Exception %lx (tval %lx) at PC %lx, SP %lx",
                      rvvm_read_cpu_reg(cpu, RVVM_REGID_CAUSE),
                      rvvm_read_cpu_reg(cpu, RVVM_REGID_TVAL),
                      rvvm_read_cpu_reg(cpu, RVVM_REGID_PC),
                      rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 2));
            for (uint32_t i=0; i<32; ++i) {
                rvvm_warn("X%d: %016lx", i, rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + i));
            }

            rvvm_addr_t pc = rvvm_read_cpu_reg(cpu, RVVM_REGID_PC);
            rvvm_addr_t pc_al = EVAL_MAX(pc - 16, pc & ~0xFFF);
            rvvm_addr_t pc_fault = pc; // The backtrace walk below moves pc

            rvvm_warn("Backtrace:");
            void** fp = NULL;
            rvvm_addr_t next_fp = rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 8);
            // Guest load addresses: elf->base is a host pointer
            rvvm_addr_t elf_base = uctx()->elf.base ? to_addr(uctx()->elf.base) : 0;
            rvvm_addr_t interp_base = uctx()->interp.base ? to_addr(uctx()->interp.base) : 0;
            // Frames falling into a known image, symbolized below
            rvvm_addr_t elf_frames[64];
            rvvm_addr_t interp_frames[32];
            size_t elf_frames_count = 0;
            size_t interp_frames_count = 0;
            do {
                rvvm_warn(" PC %lx", pc);
                if (pc >= elf_base && pc < elf_base + uctx()->elf.buf_size) {
                    rvvm_warn("  @ Main binary, reloc: %lx", pc - elf_base);
                    if (elf_frames_count < STATIC_ARRAY_SIZE(elf_frames)) {
                        elf_frames[elf_frames_count++] = pc;
                    }
                }
                if (pc >= interp_base && pc < interp_base + uctx()->interp.buf_size) {
                    rvvm_warn("  @ Interpreter, reloc: %lx", pc - interp_base);
                    if (interp_frames_count < STATIC_ARRAY_SIZE(interp_frames)) {
                        interp_frames[interp_frames_count++] = pc;
                    }
                }
                if (next_fp <= (rvvm_addr_t)(size_t)fp) break;
                void** frame = to_ptr_sz(next_fp, sizeof(void*));
                if (!frame || !proc_mem_readable(frame, sizeof(void*))) {
                    rvvm_warn(" * * * Frame pointer points to inaccessible memory!");
                    break;
                }
                fp = (void**)(size_t)next_fp;
                next_fp = (rvvm_addr_t)(size_t)frame[-2];
                rvvm_warn(" Next FP: %lx", next_fp);
                pc = (rvvm_addr_t)(size_t)frame[-1];
            } while (true);

            uint8_t* pc_host = to_ptr_sz(pc_al, 32);
            if (pc_host && proc_mem_readable(pc_host, 32)) {
                rvvm_warn("Instruction bytes around PC:");
                for (size_t i=0; i<32; ++i) {
                    printf("%02x", pc_host[i]);
                }
                printf("\n");
                for (size_t i=0; i<32; ++i) {
                    printf("%s", (pc_al + i == pc_fault) ? "^ " : "  ");
                }
                printf("\n");
            } else {
                rvvm_warn(" * * * PC points to inaccessible memory!");
            }

            // Resolve the frames through the ELF images on the host disk
            proc_symbolize("Main binary", uctx()->main_elf_path, elf_frames, elf_frames_count);
            proc_symbolize("Interpreter", uctx()->interp_elf_path, interp_frames, interp_frames_count);

            break;
        }
    }

    if (thread->child_cleartid) {
        atomic_store_uint32(thread->child_cleartid, 0);
        rvvm_sys_futex(thread->child_cleartid, UAPI_FUTEX_WAKE, 1, 0, NULL, 0);
    }

    userland_thread_unregister(thread);

    rvvm_free_user_thread(cpu);
    free(thread);
    return NULL;
}

// Jump into _start after setting up the context
// Both @entry and @stack_top are guest addresses
static void jump_start(size_t entry, size_t stack_top)
{
#ifdef RVVM_USER_TEST_RISCV
    register size_t a0 __asm__("a0") = (size_t) entry;
    register size_t sp __asm__("sp") = (size_t) stack_top;

    __asm__ __volatile__(
        "jr a0;"
        :
        : "r" (a0), "r" (sp)
        :
    );
#elif defined(RVVM_USER_TEST_X86)
    register size_t rax __asm__("rax") = (size_t) entry;
    register size_t rsp __asm__("rsp") = (size_t) stack_top;
    register size_t rdx __asm__("rdx") = (size_t) &exit; // Why do we even need to pass this?

    __asm__ __volatile__(
        "jmp *%0;"
        :
        : "r" (rax), "r" (rsp), "r" (rdx)
        :
    );
#else
    rvvm_userland_t* ctx = uctx();
    atomic_store_uint32(&ctx->userland_exit_reported, 0);
    // A new guest starts running: the launcher boots several in one process,
    // so a suspend left over from the previous one must not carry over.
    atomic_store_uint32(&ctx->userland_suspend, 0);
    atomic_store_uint32(&ctx->userland_parked, 0);

    // ...and neither must the previous guest's console state: no type-ahead
    // left in the ring, no half-typed line, and the terminal back to the
    // default modes it reports through TCGETS.
    spin_lock(&ctx->tty_in_lock);
    ctx->tty_cooked_head = 0;
    ctx->tty_cooked_len  = 0;
    ctx->tty_line_len    = 0;
    ctx->tty_eof_pending = 0;
    ctx->tty_in_eof      = false;
    ctx->tty_lflag       = TTY_LFLAG_DEFAULT;
    spin_unlock(&ctx->tty_in_lock);

    rvvm_user_thread_t* thread = safe_new_obj(rvvm_user_thread_t);
    thread->cpu = rvvm_create_user_thread(ctx->machine);
    ctx->userland_main_thread = thread;

    rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_X0 + 2, (size_t)stack_top);
    rvvm_write_cpu_reg(thread->cpu, RVVM_REGID_PC,     (size_t)entry);

    rvvm_user_thread_wrap(thread);
    ctx->userland_main_thread = NULL;
#endif
}

// Describes the executable to be ran
typedef struct {
    // Self explanatory
    size_t argc;
    char** argv;
    char** envp;

    size_t base;         // Main ELF base address (relocation)
    size_t entry;        // Main ELF entry point
    size_t interp_base;  // ELF interpreter (aka linker usually) base address
    size_t interp_entry; // ELF interpreter entry point
    size_t phdr;         // Address of ELF PHDR section
    size_t phnum;        // Number of PHDRs
} exec_desc_t;

/*
 * Guest process stack setup routines
 */

static void* stack_put_mem(uint8_t* stack, const void* mem, size_t len)
{
    stack -= len;
    memcpy(stack, mem, len);
    return stack;
}

static void* stack_put_size(void* stack, uapi_size_t val)
{
    return stack_put_mem(stack, &val, sizeof(val));
}

static void* stack_put_str(void* stack, const char* str)
{
    return stack_put_mem(stack, str, rvvm_strlen(str) + 1);
}

#define UAPI_AT_NULL           0
#define UAPI_AT_IGNORE         1
#define UAPI_AT_EXECFD         2
#define UAPI_AT_PHDR           3
#define UAPI_AT_PHENT          4
#define UAPI_AT_PHNUM          5
#define UAPI_AT_PAGESZ         6
#define UAPI_AT_BASE           7
#define UAPI_AT_FLAGS          8
#define UAPI_AT_ENTRY          9
#define UAPI_AT_NOTELF        10
#define UAPI_AT_UID           11
#define UAPI_AT_EUID          12
#define UAPI_AT_GID           13
#define UAPI_AT_EGID          14
#define UAPI_AT_PLATFORM      15
#define UAPI_AT_HWCAP         16
#define UAPI_AT_CLKTCK        17
#define UAPI_AT_SECURE        23
#define UAPI_AT_BASE_PLATFORM 24
#define UAPI_AT_RANDOM        25
#define UAPI_AT_EXECFN        31
#define UAPI_AT_SYSINFO_EHDR  33 // vDSO location; RISC-V specific!

static rvvm_addr_t rvvm_user_init_stack(void* stack, exec_desc_t* desc)
{
    /*
     * Stack layout (upside down):
     * 1. argc (guest size_t)
     * 2. string pointers: argv, 0, envp, 0
     * 3. auxv
     * 4. padding
     * 5. random bytes (16)
     * 6. string data: argv, envp
     * 7. string data: execfn
     * 8. null (guest size_t)
     */

    // 8. null
    stack = stack_put_size(stack, 0);

    // 7. string data: execfn
    stack = stack_put_str(stack, desc->argv[0]);
    char* execfn = stack;

    // 6. string data: argv, envp
    size_t envc = 0;
    while (desc->envp[envc]) envc++;

    size_t string_num = desc->argc + envc + 2;
    uapi_size_t* string_ptrs = safe_new_arr(uapi_size_t, string_num);

    for (size_t i = envc; i--;) {
        stack = stack_put_str(stack, desc->envp[i]);
        string_ptrs[desc->argc + 1 + i] = (uapi_size_t)to_addr(stack);
    }

    for (size_t i = desc->argc; i--;) {
        stack = stack_put_str(stack, desc->argv[i]);
        string_ptrs[i] = (uapi_size_t)to_addr(stack);
    }

    // 5. random bytes
    char rng_buf[16] = {0};
    rvvm_randombytes(rng_buf, sizeof(rng_buf));
    stack = stack_put_mem(stack, rng_buf, sizeof(rng_buf));
    char* random_bytes = stack;

    // 4. align to 16 bytes
    stack = (char*)align_size_down((size_t)stack, 16);

    // 3. auxv, then null
    uapi_size_t auxv[] = {
        // TODO: UAPI_AT_EXECFD
        UAPI_AT_PHDR,          desc->phdr,
        UAPI_AT_PHENT,         56,
        UAPI_AT_PHNUM,         desc->phnum,
        UAPI_AT_PAGESZ,        0x1000,
        UAPI_AT_BASE,          desc->interp_base,
        UAPI_AT_FLAGS,         0,
        UAPI_AT_ENTRY,         desc->entry,
        UAPI_AT_UID,           getuid(),
        UAPI_AT_EUID,          getuid(),
        UAPI_AT_GID,           getgid(),
        UAPI_AT_EGID,          getgid(),
        UAPI_AT_PLATFORM,      0,
        UAPI_AT_HWCAP,         0x112d,
        UAPI_AT_CLKTCK,        100,
        UAPI_AT_SECURE,        0,
        UAPI_AT_BASE_PLATFORM, 0,
        UAPI_AT_RANDOM,        (uapi_size_t)to_addr(random_bytes),
        UAPI_AT_EXECFN,        (uapi_size_t)to_addr(execfn),
        UAPI_AT_NULL,
    };
    stack = stack_put_mem(stack, auxv, sizeof(auxv));

    // 2. string pointers
    stack = stack_put_mem(stack, string_ptrs, sizeof(uapi_size_t) * string_num);
    free(string_ptrs);

    // 1. argc
    stack = stack_put_size(stack, desc->argc);

    return to_addr(stack);
}

extern char** environ;

/*
static char* default_envp[] = {
    "LANG=en_US.UTF-8",
    "TERM=xterm-256color",
    "DISPLAY=:0",
    "XDG_RUNTIME_DIR=/run/user/1000",
    NULL,
};
*/

/*
 * Create a userland instance (machine + its per-instance context) without
 * running it, so a host can configure it before the guest starts:
 *
 *     rvvm_machine_t* m = rvvm_user_create();
 *     rvvm_user_set_exit_callback(m, on_exit);
 *     rvvm_user_linux_ex(m, argc, argv, envp);
 *
 * The instance owns its whole state (allocator, image, signal table, thread
 * registry, callbacks), so several instances can run independently in one
 * process. Returns NULL on failure.
 */
PUBLIC rvvm_machine_t* rvvm_user_create(void)
{
    rvvm_machine_t* machine = rvvm_create_userland("rv64");
    if (!machine) {
        rvvm_error("Failed to create the userland machine");
        return NULL;
    }

    rvvm_userland_t* ctx = safe_new_obj(rvvm_userland_t);
    ctx->machine      = machine;
    ctx->prefix_path  = USERLAND_DEFAULT_PREFIX;
    ctx->fake_root    = USERLAND_DEFAULT_FAKE_ROOT;
    // Terminal state before the guest changes it - must match the TCGETS
    // answer in user_tty_ioctl(): canonical, echoing, line editing.
    ctx->tty_lflag    = TTY_LFLAG_DEFAULT;
    machine->userdata = ctx;
    return machine;
}

// Tear down a userland instance: context, thread registry and machine
static void userland_destroy(rvvm_machine_t* machine)
{
    rvvm_userland_t* ctx = rvvm_userland_ctx(machine);
    if (!ctx) {
        return;
    }
    machine->userdata = NULL;
    safe_free(ctx->prefix_owned);
    vector_free(ctx->userland_threads);
    /* Only an internally created session is ours to free: a host-attached one
     * outlives the machine by design (see rvvm_tty_detach). */
    if (ctx->tty && ctx->tty_owned) {
        rvvm_tty_close(ctx->tty);
    }
    rvvm_free_machine(machine);
    safe_free(ctx);
}

/*
 * Free an instance created with rvvm_user_create() without running it, or
 * discard one whose rvvm_user_linux_ex() was declined. Safe on NULL; do not
 * call it on an instance that rvvm_user_linux_ex() already returned from.
 */
PUBLIC void rvvm_user_free(rvvm_machine_t* machine)
{
    if (!machine) {
        return;
    }
    /* If the calling thread is the one bound to this instance (the failure
     * paths inside rvvm_user_linux_ex() run on the guest thread), drop the
     * binding with it: the context it points at is about to be freed. A host
     * thread freeing an instance it never ran holds no such binding. */
    if (tls_userland == rvvm_userland_ctx(machine)) {
        tls_userland = NULL;
    }
    userland_destroy(machine);
}

/*
 * Run @machine (from rvvm_user_create()) until the guest exits. Blocks the
 * calling thread. The machine and its context are freed before returning.
 */
PUBLIC int rvvm_user_linux_ex(rvvm_machine_t* machine, int argc, char** argv, char** envp)
{
    char path_buf[UAPI_PATH_MAX] = {0};

    if (!machine || !rvvm_userland_ctx(machine)) {
        rvvm_error("Invalid userland machine, use rvvm_user_create()");
        return -1;
    }

    // Bind the calling (main) thread too, for the native paths that do not go
    // through the wrap loop below. This binding is the whole of "which guest is
    // this thread in": every translation helper reads the machine from it, and a
    // second instance in the process binds its own threads to its own context.
    tls_userland = rvvm_userland_ctx(machine);

    /* Reset this instance's per-launch state (a previous guest may have run on
     * the same machine):
     *  - a stale elf/interp .base makes elf_load_file() take the objcopy
     *    path (no fresh mapping, entry relocated wrongly -> jump into the
     *    NULL page), and the old image blocks the next fixed VMA mapping;
     *  - siga[] carries the previous guest's signal dispositions.
     * The brk heap is reset by guest_vm_init() below. */
    rvvm_userland_t* ctx = uctx();
    elf_unload_file(&ctx->elf);
    elf_unload_file(&ctx->interp);
    memset(ctx->siga, 0, sizeof(ctx->siga));

    /* Path prefix override: an empty RVVM_USER_PREFIX passes host paths through
     * unchanged, unset keeps the build-time default. Resolved here, on the
     * guest thread, rather than in rvvm_user_create(): hosts putenv() right
     * before launching this thread, so create() would have read a stale value. */
    const char* env_prefix = getenv("RVVM_USER_PREFIX");
    if (env_prefix) {
        ctx->prefix_path = env_prefix[0] ? env_prefix : NULL;
    }

    guest_vm_init();
    /* Host pointer standing for guest address 0. Deliberately computed with
     * integer math: the result points below the allocation and must never be
     * dereferenced on its own, only as (window + guest_addr). */
    uint8_t* window = (uint8_t*)((size_t)machine->mem.data - (size_t)machine->mem.addr);
    uctx()->elf.guest_window    = window;
    uctx()->interp.guest_window = window;

    stacktrace_init();
    user_fault_handler_install();
    
#if defined(ANDROID)
    /* Set Android I/O callback before initializing cmdpost */
    extern ssize_t android_io_callback(int fd, const void* buf, size_t count);
    rvvm_user_set_io_callback(machine, android_io_callback);
#endif
    
    /* Initialize Android NDK API proxy */
    cmdpost_init();
    // Remember the ELF images for crash symbolization (see proc_symbolize())
    uctx()->main_elf_path[0] = 0;
    uctx()->interp_elf_path[0] = 0;
    const char* host_elf = wrap_path(path_buf, argv[0]);
    rvvm_strlcpy(uctx()->main_elf_path, host_elf, sizeof(uctx()->main_elf_path));
    rvfile_t* file = rvopen(host_elf, 0);
    if (!file) {
        // drvfs (WSL) sometimes fails opening big files right after host-side
        // writes, retry a couple of times before giving up
        for (int retry = 0; !file && retry < 10; retry++) {
            sleep_ms(100);
            file = rvopen(host_elf, 0);
        }
    }
    if (!file) {
        rvvm_error("Failed to open ELF file %s (errno %d)", argv[0], errno);
        rvvm_user_free(machine);
        return -1;
    }
    /* A relocatable (ET_DYN) main image - a PIE executable, or a .so launched
     * as the guest program - has no link-time address, so hand the loader one:
     * elf_load_file() places the image there and ignores this for ET_EXEC.
     * Must be set on every launch: elf_unload_file() zeroes the descriptor. */
    uctx()->elf.load_addr = GUEST_DYN_BASE;
    /* Shared objects carry no entry point, so name the symbol to start from.
     * RVVM_USER_ENTRY overrides it (android_main, ANativeActivity_onCreate...). */
    const char* entry_sym = getenv("RVVM_USER_ENTRY");
    uctx()->elf.entry_symbol = (entry_sym && entry_sym[0]) ? entry_sym : "main";
    bool success = elf_load_file(file, &uctx()->elf);
    rvclose(file);
    if (!success) {
        rvvm_error("Failed to load ELF %s (fixed VMA collision?)", argv[0]);
        // Dump the host memory map to find what occupies the guest region
        int maps_fd = open("/proc/self/maps", O_RDONLY);
        if (maps_fd != -1) {
            char chunk[4096];
            ssize_t rd;
            while ((rd = read(maps_fd, chunk, sizeof(chunk))) > 0) {
                if (write(STDERR_FILENO, chunk, rd) != rd) break;
            }
            close(maps_fd);
        }
        rvvm_user_free(machine);
        return -1;
    }
    rvvm_info("Loaded ELF %s at base %lx, entry %lx,\n%ld PHDRs at %lx",
              argv[0], (size_t)uctx()->elf.base, uctx()->elf.entry, uctx()->elf.phnum, uctx()->elf.phdr);

    /* The brk heap starts right past the image and runs up to where mmap()ed
     * ranges begin - exactly what ELF_USERLAND_HEAP_MARGIN reserved. */
    uctx()->guest_brk_start = align_size_up(to_addr(uctx()->elf.base) + uctx()->elf.buf_size, GUEST_PAGE_SIZE);
    uctx()->guest_brk_ptr   = uctx()->guest_brk_start;

    if (uctx()->elf.interp_path) {
        rvvm_info("ELF interpreter at %s", uctx()->elf.interp_path);
        // wrap_path() may pass the path through untouched - keep its result
        const char* host_interp = wrap_path(path_buf, uctx()->elf.interp_path);
        rvvm_strlcpy(uctx()->interp_elf_path, host_interp, sizeof(uctx()->interp_elf_path));
        file = rvopen(host_interp, 0);
        if (file) {
            /* A relocatable interpreter needs a guest address picked upfront.
             * Reserve its full memory extent, not the file size: .bss counts
             * too, and musl's ldso keeps its internal locks there. An
             * undersized reservation leaves that tail outside the interpreter,
             * so the next guest mmap() (the first shared library) lands on top
             * of it - the loader then blocks forever on a corrupted lock. */
            spin_lock(&uctx()->guest_lock);
            bool placed = guest_range_alloc(&uctx()->interp.load_addr, 0, elf_image_extent(file), false);
            spin_unlock(&uctx()->guest_lock);
            if (!placed) {
                uctx()->interp.load_addr = 0;
            }
        }
        /* The interpreter is itself a shared object (musl's ld-musl / glibc's
         * ld.so are linked as .so), so it carries no usable e_entry: the kernel
         * jumps to its loader entry symbol instead. */
        uctx()->interp.entry_symbol = "_dlstart";
        success = file && elf_load_file(file, &uctx()->interp);
        rvclose(file);
        if (!success) {
            rvvm_error("Failed to load interpreter %s", uctx()->elf.interp_path);
            rvvm_user_free(machine);
            return -1;
        }
        rvvm_info("Loaded interpreter %s at base %lx, entry %lx,\n%ld PHDRs at %lx",
                  uctx()->elf.interp_path, (size_t)uctx()->interp.base, uctx()->interp.entry, uctx()->interp.phnum, uctx()->interp.phdr);
    }

    if (envp == NULL) {
        envp = environ;
    }

    const char* prefix_path = uctx()->prefix_path;
    if (prefix_path && (!getcwd(path_buf, sizeof(path_buf)) || !path_wrapped(path_buf))) {
        if (chdir(prefix_path)) {
            rvvm_error("Failed to chdir to userland prefix %s", prefix_path);
        }
    }

    //rvvm_set_loglevel(LOG_INFO);

    exec_desc_t desc = {
        .argc = argc,
        .argv = argv,
        .envp = envp,
        .base = uctx()->elf.base ? to_addr(uctx()->elf.base) : 0,
        .entry = uctx()->elf.entry,
        .interp_base = uctx()->interp.base ? to_addr(uctx()->interp.base) : 0,
        .interp_entry = uctx()->interp.entry,
        .phdr = uctx()->elf.phdr,
        .phnum = uctx()->elf.phnum,
    };

    // The stack lives in guest memory too, so the guest can name pointers to it
    uint8_t* stack_buffer = to_ptr(uctx()->guest_stack_base);
    memset(stack_buffer, 0, GUEST_STACK_SIZE);
    size_t stack_top = rvvm_user_init_stack(stack_buffer + GUEST_STACK_SIZE, &desc);

    rvvm_info("Stack top at %lx", (size_t)stack_top);

    if (uctx()->elf.interp_path) {
        jump_start(uctx()->interp.entry, stack_top);
    } else {
        jump_start(uctx()->elf.entry, stack_top);
    }

    /* Cleanup Android NDK API proxy */
    cmdpost_cleanup();

    /* Guest threads wind down asynchronously: wait for them to leave the
     * machine before freeing it. On timeout, leak the machine instead of
     * leaving a running vCPU on freed memory. */
    if (userland_threads_gone(ctx, 1000)) {
        tls_userland = NULL;
        userland_destroy(machine);
    } else {
        rvvm_warn("Guest threads linger after exit, leaking userland machine");
    }
    return 0;
}

/* Single-instance convenience entry: creates its own userland instance, runs it
 * and frees it. Equivalent to rvvm_user_create() + rvvm_user_linux_ex(). */
int rvvm_user_linux(int argc, char** argv, char** envp)
{
    rvvm_machine_t* machine = rvvm_user_create();
    if (!machine) {
        return -1;
    }
    return rvvm_user_linux_ex(machine, argc, argv, envp);
}

#else

#include "utils.h"

rvvm_machine_t* rvvm_user_create(void)
{
    rvvm_warn("Userland emulation not available, define RVVM_USER_TEST");
    return NULL;
}

int rvvm_user_linux_ex(rvvm_machine_t* machine, int argc, char** argv, char** envp)
{
    UNUSED(machine); UNUSED(argc); UNUSED(argv); UNUSED(envp);
    rvvm_warn("Userland emulation not available, define RVVM_USER_TEST");
    return -1;
}

int rvvm_user_linux(int argc, char** argv, char** envp)
{
    UNUSED(argc); UNUSED(argv); UNUSED(envp);
    rvvm_warn("Userland emulation not available, define RVVM_USER_TEST");
    return -1;
}

void rvvm_user_free(rvvm_machine_t* machine)
{
    UNUSED(machine);
}

void rvvm_user_set_io_callback(rvvm_machine_t* machine, rvvm_user_io_callback callback)
{
    UNUSED(machine); UNUSED(callback);
}

void rvvm_user_set_exit_callback(rvvm_machine_t* machine, rvvm_user_exit_callback callback)
{
    UNUSED(machine); UNUSED(callback);
}

void rvvm_user_stop(rvvm_machine_t* machine, int exit_code)
{
    UNUSED(machine); UNUSED(exit_code);
}

bool rvvm_user_suspend(rvvm_machine_t* machine)
{
    UNUSED(machine);
    return true;
}

void rvvm_user_resume(rvvm_machine_t* machine)
{
    UNUSED(machine);
}

bool rvvm_user_is_suspended(rvvm_machine_t* machine)
{
    UNUSED(machine);
    return false;
}

#endif
