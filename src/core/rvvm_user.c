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

// Linux-specific stuff
#ifdef __linux__
#include <sys/eventfd.h> // eventfd()
#include <sys/epoll.h>   // epoll_create1(), etc
#include <sys/sysinfo.h> // sysinfo()
#include <sys/fsuid.h>   // setfsuid(), setfsgid()
#include <sys/vfs.h>     // struct statfs

// Put syscall headers here
#include <linux/futex.h> // FUTEX_*
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

#define UAPI_EPERM   1
#define UAPI_ENOENT  2
#define UAPI_EINTR   4
#define UAPI_EIO     5
#define UAPI_EBADF   9
#define UAPI_EAGAIN  11
#define UAPI_ENOMEM  12
#define UAPI_EACCESS 13
#define UAPI_EFAULT  14
#define UAPI_EBUSY   16
#define UAPI_EEXIST  17
#define UAPI_EINVAL  22
#define UAPI_ENOSYS  38

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

static rvvm_machine_t* userland; // Emulated RVVM process context


// Short cast rvvm_addr_t -> void*
static void* to_ptr(rvvm_addr_t addr)
{
    if (!userland) {
        // RVVM_USER_TEST* modes run the guest natively, guest == host
        return (void*)(size_t)addr;
    }
    // Preserve NULL: the guest's NULL page is unmapped, and callers test the
    // result for NULL (a bare offset would make addr 0 look like valid memory)
    if (addr < userland->mem.addr) {
        return NULL;
    }
    return ((uint8_t*)userland->mem.data) + (addr - userland->mem.addr);
}

// Short cast rvvm_addr_t -> const char*
static const char* to_str(rvvm_addr_t addr)
{
    return (const char*)to_ptr(addr);
}

// Host pointer inside guest memory -> rvvm_addr_t
static rvvm_addr_t to_addr(const void* ptr)
{
    if (!userland) {
        return (rvvm_addr_t)(size_t)ptr;
    }
    return (rvvm_addr_t)(((const uint8_t*)ptr) - ((const uint8_t*)userland->mem.data)) + userland->mem.addr;
}

#include <core/rvvm_user.h>

PUBLIC void* rvvm_user_guest_ptr(uint64_t addr)
{
    if (!userland) {
        // RVVM_USER_TEST* modes run the guest natively, guest == host
        return addr ? (void*)(size_t)addr : NULL;
    }
    if (addr < userland->mem.addr || (addr - userland->mem.addr) >= userland->mem.size) {
        return NULL;
    }
    return ((uint8_t*)userland->mem.data) + (addr - userland->mem.addr);
}

PUBLIC uint64_t rvvm_user_host_ptr(const void* ptr)
{
    if (!ptr) {
        return 0;
    }
    if (!userland) {
        return (uint64_t)(size_t)ptr;
    }
    const uint8_t* data = (const uint8_t*)userland->mem.data;
    const uint8_t* p    = (const uint8_t*)ptr;
    if (p < data || (size_t)(p - data) >= userland->mem.size) {
        return 0;
    }
    return userland->mem.addr + (uint64_t)(p - data);
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

typedef struct rvvm_userland {
    // Machine this context belongs to; identical to rvvm_machine_t::userdata
    rvvm_machine_t* machine;

    // --- Configuration & callbacks (group A) ---
    rvvm_user_io_callback    io_callback;
    rvvm_user_exit_callback  exit_callback;
    const char*              prefix_path;
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
    void*                    tty_vt;        // VTerm* (opaque here)
    void*                    tty_screen;    // VTermScreen* (opaque here)
    rvvm_user_tty_callback   tty_cb;
    void*                    tty_userdata;
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

#define VTERM_ROWS 24
#define VTERM_COLS 80

static void user_tty_init(rvvm_userland_t* ctx)
{
    if (ctx->tty_vt) {
        return;
    }
    VTerm* vt = vterm_new(VTERM_ROWS, VTERM_COLS);
    if (!vt) {
        return;
    }
    VTermScreen* screen = vterm_obtain_screen(vt);
    // No screen callbacks needed: the host renders by calling
    // vterm_screen_flush_damage() + vterm_screen_get_chars() on demand.
    vterm_screen_reset(screen, true);
    ctx->tty_vt     = vt;
    ctx->tty_screen = screen;
}

// Feed guest output on fd 1/2 through libvterm. No-op unless a host registered
// a tty callback (so platforms that simply write to a real tty are unaffected).
static void user_tty_write(rvvm_userland_t* ctx, int fd, const void* buf, size_t count)
{
    if (!ctx->tty_cb || !buf || !count) {
        return;
    }
    user_tty_init(ctx);
    if (!ctx->tty_vt) {
        return;
    }
    vterm_input_write((VTerm*)ctx->tty_vt, buf, count);
    // Notify the host; it decides when to flush/render (throttling is its job).
    ctx->tty_cb(ctx->tty_userdata, fd, ctx->tty_vt);
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

// Reset the guest address-space allocator for a fresh run (rvvm_user_linux)
static void guest_vm_init(void)
{
    rvvm_userland_t* ctx = uctx();
    ctx->guest_stack_top  = (userland->mem.addr + userland->mem.size) & ~(rvvm_addr_t)(GUEST_PAGE_SIZE - 1);
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
        if (hint < GUEST_MMAP_BASE || hint + size > ctx->guest_mmap_end) {
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
    memset(to_ptr(addr), 0, size);
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

// Return last errno like a syscall interface
static int last_errno(void)
{
    // TODO: Host->Guest errno conversion
    return -errno;
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

static bool proc_mem_readable(const void* addr, size_t size)
{
    static int fd = 0;
    DO_ONCE({
        fd = vma_anon_memfd(4096);
        if (fd < 0) rvvm_fatal("Failed to create memfd!");
    });
    return write(fd, addr, size) == (ssize_t)size;
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
    while (atomic_load_uint32(&ctx->userland_suspend) && !atomic_load_uint32(&thread->finished)) {
        rvvm_futex_wait(&ctx->userland_suspend, 1, USERLAND_SUSPEND_POLL_NS);
    }
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
    memcpy(host_buf, to_ptr(guest_addr), size);
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
    memcpy(to_ptr(guest_addr), host_buf, size);
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
    const char* src = (const char*)to_ptr(guest_addr);
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
        thread->cpu = rvvm_create_user_thread(userland);

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

static int rvvm_sys_futex(uint32_t* addr, int futex_op, uint32_t val, size_t val2, uint32_t* uaddr2, uint32_t val3)
{
#if defined(__linux__)
    return errno_ret(syscall(SYS_futex, addr, futex_op, val, val2, uaddr2, val3));
#else
    UNUSED(val2); UNUSED(uaddr2); UNUSED(val3);
    switch (futex_op & UAPI_FUTEX_CMD_MASK) {
        case UAPI_FUTEX_WAIT:
        case UAPI_FUTEX_WAIT_BITSET:
            if (atomic_load_uint32(addr) == val) {
                sleep_ms(1);
            }
            return 0;
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
    int64_t ret = 0;
    ret = errno_ret(syscall(SYS_getdents64, fd, dirp, size));
    //rvvm_warn("getdents64(%d, %p, %ld) -> %ld (%s)", fd, dirp, size, ret, (ret < 0) ? strerror(errno) : "Success");
    return ret;
    DIR* dir = fdopendir(dup(fd));
    if (dir) {
        struct dirent* dent = NULL;
        while ((dent = readdir(dir))) {
            size_t name_len = rvvm_strlen(dent->d_name);
            size_t dirent_size = sizeof(struct uapi_linux_dirent64) + name_len + 1;
            if (dirent_size <= size) {
                struct uapi_linux_dirent64* dirent = dirp;
                dirent->d_ino = dent->d_ino;
                dirent->d_off = dirent_size;
                dirent->d_reclen = dirent_size;
                // TODO: Figure d_type somehow?
                dirent->d_type = 0;
                memcpy(dirent->d_name, dent->d_name, name_len);
                dirent->d_name[name_len] = 0;

                ret += dirent_size;
                size -= dirent_size;
                dirp = ((uint8_t*)dirp) + dirent_size;
            } else {
                closedir(dir);

                return ret;
            }
        }
        closedir(dir);
        return ret;
    } else {
        return -UAPI_ENOENT;
    }
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
        hiov[i].iov_base = to_ptr(giov[i].base);
        hiov[i].iov_len  = giov[i].len;
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
    host->msg_name       = guest->name ? to_ptr(guest->name) : NULL;
    host->msg_namelen    = guest->namelen;
    host->msg_iov        = iov_buf;
    host->msg_iovlen     = EVAL_MIN(guest->iovlen, iov_count);
    host->msg_control    = guest->control ? to_ptr(guest->control) : NULL;
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
        ssize_t rd = pread(fd, to_ptr(ret), size, (off_t)offset);
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
    return unwrap_path(buffer, tmp, size);
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
                    a0 = rvvm_sys_getcwd(to_ptr(a0), a1);
                    break;
                }
#ifdef __linux__
                case 19: // eventfd2
                    rvvm_info("sys_eventfd2(%lx, %lx)", a0, a1);
                    a0 = errno_ret(eventfd(a0, a1));
                    break;
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
                        struct uapi_epoll_event guest_ev;
                        memcpy(&guest_ev, to_ptr(a3), sizeof(guest_ev));
                        host_ev.events = guest_ev.event;
                        host_ev.data.u64 = guest_ev.data.u64;
                        host_ev_ptr = &host_ev;
                    }
                    a0 = errno_ret(epoll_ctl(a0, a1, a2, host_ev_ptr));
                    break;
                }
                case 22: { // epoll_pwait (sigmask ignored)
                    rvvm_info("sys_epoll_pwait(%lx, %lx, %lx, %lx, %lx, %lx)", a0, a1, a2, a3, a4, a5);
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
                        struct uapi_epoll_event* guest_evs = to_ptr(a1);
                        for (size_t i = 0; i < (size_t)a0; i++) {
                            guest_evs[i].event = host_evs[i].events;
                            guest_evs[i].data.u64 = host_evs[i].data.u64;
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
                    rvvm_info("sys_statfs64(%s, %lx, %lx)", to_str(a0), a1, a2);
                    a0 = errno_ret(statfs(wrap_path(path_buf, to_str(a0)), &stfs));
                    uapi_statfs64_convert(to_ptr(a1), &stfs);
                    break;
                }
                case 44: { // fstatfs64
                    struct statfs stfs = {0};
                    rvvm_info("sys_fstatfs64(%ld, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(fstatfs(a0, &stfs));
                    uapi_statfs64_convert(to_ptr(a1), &stfs);
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
                        a0 = errno_ret(openat(a0, path ? wrap_path(path_buf, path) : path, a2, a3));
                    }
                    break;
                }
                case 57: // close
                    a0 = errno_ret(close(a0));
                    break;
                case 59: // pipe2
                    rvvm_info("sys_pipe2(%lx, %lx)", a0, a1);
                    a0 = errno_ret(pipe(to_ptr(a0)));
                    break;
                case 61: // getdents64
                    a0 = rvvm_sys_getdents64(a0, to_ptr(a1), a2);
                    break;
                case 62: // lseek
                    a0 = errno_ret(lseek(a0, a1, a2));
                    break;
                case 63: // read
                    a0 = errno_ret(read(a0, to_ptr(a1), a2));
                    break;
                case 64: { // write
                    void* wbuf = to_ptr(a1);
                    if (a0 == 1 || a0 == 2) {
                        // fd 1/2: feed the virtual TTY parser (no-op unless a host
                        // registered a tty callback). When it did, the host renders
                        // the screen itself, so don't also write raw bytes to the
                        // host fd (that would double the output).
                        user_tty_write(uctx(), a0, wbuf, a2);
                        if (uctx()->tty_cb) {
                            a0 = (ssize_t)a2;
                            break;
                        }
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
                    struct iovec* hiov = rvvm_iovec_from_guest(to_ptr(a1), a2, stack_iov);
                    if (a7 == 65) {
                        a0 = errno_ret(readv(a0, hiov, a2));
                    } else if (a0 == 1 || a0 == 2) {
                        /* stdout/stderr */
                        ssize_t total = 0;
                        for (int i = 0; i < (int)a2; i++) {
                            // Feed each segment to the virtual TTY parser.
                            user_tty_write(uctx(), a0, hiov[i].iov_base, hiov[i].iov_len);
                            if (uctx()->tty_cb) {
                                total += (ssize_t)hiov[i].iov_len;
                                continue;
                            }
                            // No tty host: fall back to the raw write path.
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
                case 67: // pread64
                    a0 = errno_ret(pread(a0, to_ptr(a1), a2, a3));
                    break;
                case 68: // pwrite64
                    a0 = errno_ret(pwrite(a0, to_ptr(a1), a2, a3));
                    break;
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
                    int ret;
                    rvvm_info("sys_newfstatat(%ld, %s, %lx, %lx)", a0, path, a2, a3);
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
                    uapi_stat_convert(to_ptr(a2), &st);
                    break;
                }
                case 80: { // newfstat
                    struct stat st = {0};
                    rvvm_info("sys_newfstat(%ld, %lx)", a0, a1);
                    a0 = errno_ret(fstat(a0, &st));
                    uapi_stat_convert(to_ptr(a1), &st);
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
                    a0 = rvvm_sys_futex(to_ptr(a0), a1, a2, a3, to_ptr(a4), a5);
                    break;
                case 99: // set_robust_list
                    // TODO: Implement this
                    rvvm_info("sys_set_robust_list(%lx, %lx)", a0, a1);
                    a0 = 0;
                    break;
                case 101: // nanosleep
                    // TODO: Struct conversion
                    a0 = errno_ret(nanosleep(to_ptr(a0), to_ptr(a1)));
                    break;
                case 103: // setitimer
                    rvvm_info("sys_setitimer(%lx, %lx, %lx)", a0, a1, a2);
                    a0 = errno_ret(setitimer(a0, to_ptr(a1), to_ptr(a2)));
                    break;
                case 113: // clock_gettime
                    // TODO: Struct conversion!
                    rvvm_info("sys_clock_gettime(%lx, %lx)", a0, a1);
                    a0 = errno_ret(clock_gettime(a0, to_ptr(a1)));
                    break;
                case 114: // clock_getres
                    rvvm_info("sys_clock_getres(%lx, %lx)", a0, a1);
                    a0 = errno_ret(clock_getres(a0, to_ptr(a1)));
                    break;
#ifdef __linux__
                case 115: // clock_nanosleep
                    // TODO: struct conversion?
                    rvvm_info("sys_clock_nanosleep(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(clock_nanosleep(a0, a1, to_ptr(a2), to_ptr(a3)));
                    break;
#endif
                case 118: // sched_setparam - ignore
                case 119: // sched_setscheduler - ignore
                case 120: // sched_getscheduler - ignore
                    a0 = 0;
                    break;
                case 121: // sched_getparam - stub
                    if (a1) {
                        struct uapi_sched_param* param = to_ptr(a1);
                        memset(param, 0, sizeof(*param));
                    }
                    a0 = 0;
                    break;
                case 122: // sched_setaffinity - ignore
                    a0 = 0;
                    break;
                case 123: { // sched_getaffinity - pass through the host affinity mask,
                    // guest allocators (f.e. Zig SmpAllocator) size per-CPU arenas by it
                    if (a2 && a1) {
                        memset(to_ptr(a2), 0, a1);
                        cpu_set_t host_mask;
                        CPU_ZERO(&host_mask);
                        if (!sched_getaffinity(0, sizeof(host_mask), &host_mask)) {
                            size_t copy_len = sizeof(host_mask) < a1 ? sizeof(host_mask) : a1;
                            memcpy(to_ptr(a2), &host_mask, copy_len);
                        } else {
                            *(uint8_t*)to_ptr(a2) = 1;
                        }
                    }
                    // Syscall ABI: return the amount of bytes written into the mask
                    a0 = (a1 >= sizeof(unsigned long)) ? sizeof(unsigned long) : a1;
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
                        if (a2) memcpy(to_ptr(a2), &ctx->siga[a0], a3);
                        if (a1) {
                            memcpy(&ctx->siga[a0], to_ptr(a1), a3);

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
                case 148: // getresuid - semi stub
                    a0 = rvvm_sys_getresuid(to_ptr(a0), to_ptr(a1), to_ptr(a2));
                    break;
                case 149: // setresgid - semi stub
                    a0 = rvvm_sys_setgid(a0);
                    break;
                case 150: // getresgid - semi stub
                    a0 = rvvm_sys_getresgid(to_ptr(a0), to_ptr(a1), to_ptr(a2));
                    break;
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
                case 160: // newuname
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
                        memcpy(to_ptr(a0), &name, sizeof(name));
                        a0 = 0;
                    }
                    break;
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
                    const struct uapi_msghdr* gmsg = to_ptr(a1);
                    if (gmsg->iovlen > IOV_HARD_MAX) {
                        a0 = -UAPI_EINVAL;
                        break;
                    }
                    struct iovec  stack_iov[IOV_STACK_MAX] = {0};
                    struct iovec* hiov = rvvm_iovec_from_guest(to_ptr(gmsg->iov), gmsg->iovlen, stack_iov);
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
                    rvvm_addr_t new_addr = 0;
                    spin_lock(&uctx()->guest_lock);
                    bool ok = guest_range_alloc(&new_addr, 0, a2, false);
                    spin_unlock(&uctx()->guest_lock);
                    if (!ok) {
                        a0 = -UAPI_ENOMEM;
                        break;
                    }
                    memcpy(to_ptr(new_addr), to_ptr(a0), EVAL_MIN(a1, a2));
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
#ifdef __linux__
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
                    rvvm_flush_icache(userland, a0, a1 - a0);
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
                case 260: // wait4
                    // TODO: Struct conversion
                    rvvm_info("sys_wait4(%lx, %lx, %lx, %lx)", a0, a1, a2, a3);
                    a0 = errno_ret(wait4(a0, to_ptr(a1), a2, to_ptr(a3)));
                    break;
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
                case 278: // getrandom
                    rvvm_randombytes(to_ptr(a0), a1);
                    a0 = a1;
                    break;
#ifdef __linux__
                case 279: // memfd_create
                    rvvm_info("sys_memfd_create(%s, %lx)", to_str(a0), a1);
                    a0 = errno_ret(memfd_create(to_str(a0), a1));
                    break;
                case 291: // statx
                    // TODO: Struct conversion!
                    rvvm_info("sys_statx(%ld, %s, %lx, %lx, %lx)", a0, to_str(a1), a2, a3, a4);
                    a0 = errno_ret(statx(a0, wrap_path(path_buf, to_str(a1)), a2, a3, to_ptr(a4)));
                    break;
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

            rvvm_warn("Backtrace:");
            void **fp = NULL;
            void** next_fp = (void*)rvvm_read_cpu_reg(cpu, RVVM_REGID_X0 + 8);
            do {
                rvvm_warn(" PC %lx", pc);
                if (pc >= (size_t)uctx()->elf.base && pc < (size_t)uctx()->elf.base + uctx()->elf.buf_size) {
                    rvvm_warn("  @ Main binary, reloc: %lx", pc - (size_t)uctx()->elf.base);
                }
                if (pc >= (size_t)uctx()->interp.base && pc < (size_t)uctx()->interp.base + uctx()->interp.buf_size) {
                    rvvm_warn("  @ Interpreter, reloc: %lx)", pc - (size_t)uctx()->interp.base);
                }
                if (next_fp <= fp) break;
                if (!proc_mem_readable(fp, 8)) {
                    rvvm_warn(" * * * Frame pointer points to inaccessible memory!");
                    break;
                }
                next_fp = fp[-2];
                rvvm_warn("Next FP: %p", next_fp);
                pc = (size_t)fp[-1];
                fp = next_fp;
            } while (true);

            if (proc_mem_readable((void*)pc_al, 32)) {
                rvvm_warn("Instruction bytes around PC:");
                for (size_t i=0; i<32; ++i) {
                    printf("%02x", *(uint8_t*)(pc_al + i));
                }
                printf("\n");
                for (size_t i=0; i<32; ++i) {
                    printf("%s", (pc_al + i == pc) ? "^ " : "  ");
                }
                printf("\n");
            } else {
                rvvm_warn(" * * * PC points to inaccessible memory!");
            }

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

    rvvm_user_thread_t* thread = safe_new_obj(rvvm_user_thread_t);
    thread->cpu = rvvm_create_user_thread(userland);
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
    vector_free(ctx->userland_threads);
    if (ctx->tty_vt) {
        vterm_free((VTerm*)ctx->tty_vt);
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
    if (userland == machine) {
        userland = NULL;
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
    userland = machine;

    // Bind the calling (main) thread too, for the native paths that do not go
    // through the wrap loop below
    tls_userland = rvvm_userland_ctx(userland);

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
    rvfile_t* file = rvopen(wrap_path(path_buf, argv[0]), 0);
    if (!file) {
        // drvfs (WSL) sometimes fails opening big files right after host-side
        // writes, retry a couple of times before giving up
        for (int retry = 0; !file && retry < 10; retry++) {
            sleep_ms(100);
            file = rvopen(wrap_path(path_buf, argv[0]), 0);
        }
    }
    if (!file) {
        rvvm_error("Failed to open ELF file %s (errno %d)", argv[0], errno);
        rvvm_user_free(machine);
        return -1;
    }
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
        file = rvopen(wrap_path(path_buf, uctx()->elf.interp_path), 0);
        if (file) {
            // A relocatable interpreter needs a guest address picked upfront
            spin_lock(&uctx()->guest_lock);
            bool placed = guest_range_alloc(&uctx()->interp.load_addr, 0, rvfilesize(file), false);
            spin_unlock(&uctx()->guest_lock);
            if (!placed) {
                uctx()->interp.load_addr = 0;
            }
        }
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
        userland = NULL;
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
