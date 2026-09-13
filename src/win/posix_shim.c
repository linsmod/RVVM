/*
 * posix_shim.c - Win32 implementations for the POSIX functions declared
 * in the mingw_compat/ headers. Covers the compile layer for the RVVM
 * core build: everything here either is implemented on top of
 * Win32 (mmap/memory, clocks, file *at() family, poll/select, epoll over
 * poll(), ...) or is an explicit ENOSYS stub for semantics Windows cannot
 * provide without a full emulation layer (fork/eventfd/futex - see README
 * "Known gaps"). Sockets live in win_socket.c; the CRT fd layer below
 * dispatches to it so read()/write()/close()/dup() work on a socket fd.
 *
 * Design notes:
 *  - fds are CRT fds (from _open/_open_osfhandle/_pipe), converted to
 *    HANDLEs via _get_osfhandle() where needed.
 *  - mmap is backed by VirtualAlloc (anonymous) or CreateFileMapping
 *    (file-backed), with a process-local VMA list so munmap() finds the
 *    region base even when Linux semantics hand back interior pointers.
 *  - Linux absolute paths ("/root/...") are passed to the CRT as-is:
 *    Windows maps them onto the current drive. The *at() family resolves
 *    dirfd via GetFinalPathNameByHandle().
 */

/* WIN32_LEAN_AND_MEAN keeps winsock out of windows.h - the BSD socket
 * API comes from the mingw_compat/sys/socket.h shim instead, and winsock's
 * SOCKET-based fd_set/select/socket declarations would conflict with it */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <direct.h>

#include <dirent.h>
#include <grp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/epoll.h>

/* FSCTL_GET_REPARSE_POINT (symbolic link targets) */
#include <winioctl.h>

/* WinSock-backed sockets (and their CRT fd anchors) - see win_socket.c */
#include "win_socket.h"

/* The POSIX spellings of read/write/close/dup are MSVC-deprecated aliases in
 * MinGW's <io.h>; this file provides the real implementations on purpose. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void win_set_errno(void)
{
    DWORD e = GetLastError();
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        errno = ENOENT;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_PRIVILEGE_NOT_HELD:      /* e.g. CreateSymbolicLink without the
                                         * privilege / developer mode */
        errno = EPERM;
        break;
    case ERROR_NOT_A_REPARSE_POINT:
        errno = EINVAL;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    default:
        errno = EIO;
        break;
    }
}

/* Resolve "dirfd + path" to a Windows-openable path, like openat() does.
 * Absolute POSIX paths pass through unchanged (Windows maps them onto
 * the current drive); AT_FDCWD paths stay relative; any other dirfd is
 * resolved to its Windows path and the relative part appended. */
static int at_path(int dirfd, const char* path, char* out, size_t outsz)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        snprintf(out, outsz, "%s", path);
        return 0;
    }
    {
        HANDLE h;
        char dir[MAX_PATH + 16];
        DWORD n;
        if (dirfd < 0) {
            errno = EBADF;
            return -1;
        }
        h = (HANDLE)_get_osfhandle(dirfd);
        if (h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return -1;
        }
        n = GetFinalPathNameByHandleA(h, dir, MAX_PATH, 0);
        if (!n || n >= MAX_PATH) {
            errno = ENOSYS;
            return -1;
        }
        dir[n] = 0;
        if (strncmp(dir, "\\\\?\\UNC\\", 8) == 0) {
            memmove(dir + 2, dir + 8, strlen(dir) - 7);
        } else if (strncmp(dir, "\\\\?\\", 4) == 0) {
            memmove(dir, dir + 4, strlen(dir) - 3);
        }
        snprintf(out, outsz, "%s\\%s", dir, path);
    }
    return 0;
}

/*
 * Symbolic links and directory junctions are reparse points. The buffer
 * FSCTL_GET_REPARSE_POINT fills in has a layout fixed by the OS; MinGW does not
 * declare REPARSE_DATA_BUFFER, so the parts read here are spelled out locally.
 */
typedef struct {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
    } u;
} shim_reparse_buffer;

static bool shim_path_is_reparse(const char* path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

/*
 * Read a symlink / junction target as UTF-8 (host spelling, not yet mapped back
 * to the guest's view). Returns the byte length (NUL excluded), or -1 with
 * EINVAL when the path is not a name-surrogate reparse point.
 */
static ssize_t shim_read_link(const char* path, char* out, size_t outsz)
{
    BYTE raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    shim_reparse_buffer* rdb = (shim_reparse_buffer*)raw;
    const WCHAR* base;
    const WCHAR* name;
    USHORT off, len;
    HANDLE h;
    DWORD bytes = 0;
    int n;

    h = CreateFileA(path, FILE_READ_EA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        win_set_errno();
        return -1;
    }
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0,
                         raw, (DWORD)sizeof(raw), &bytes, NULL)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        errno = (err == ERROR_NOT_A_REPARSE_POINT) ? EINVAL : EIO;
        return -1;
    }
    CloseHandle(h);

    if (!IsReparseTagNameSurrogate(rdb->ReparseTag)) {
        errno = EINVAL;
        return -1;
    }
    if (rdb->ReparseTag == (ULONG)IO_REPARSE_TAG_SYMLINK) {
        off  = rdb->u.SymbolicLinkReparseBuffer.PrintNameOffset;
        len  = rdb->u.SymbolicLinkReparseBuffer.PrintNameLength;
        base = rdb->u.SymbolicLinkReparseBuffer.PathBuffer;
    } else {
        off  = rdb->u.MountPointReparseBuffer.PrintNameOffset;
        len  = rdb->u.MountPointReparseBuffer.PrintNameLength;
        base = rdb->u.MountPointReparseBuffer.PathBuffer;
    }
    if (!len) {
        /* Junctions often only carry the NT form ("\??\C:\path") */
        if (rdb->ReparseTag == (ULONG)IO_REPARSE_TAG_SYMLINK) {
            off  = rdb->u.SymbolicLinkReparseBuffer.SubstituteNameOffset;
            len  = rdb->u.SymbolicLinkReparseBuffer.SubstituteNameLength;
            base = rdb->u.SymbolicLinkReparseBuffer.PathBuffer;
        } else {
            off  = rdb->u.MountPointReparseBuffer.SubstituteNameOffset;
            len  = rdb->u.MountPointReparseBuffer.SubstituteNameLength;
            base = rdb->u.MountPointReparseBuffer.PathBuffer;
        }
        if (len >= 8) {
            const WCHAR* sub = (const WCHAR*)((const BYTE*)base + off);
            if (sub[0] == L'\\' && sub[1] == L'?' && sub[2] == L'?' && sub[3] == L'\\') {
                off = (USHORT)(off + 8);    /* drop the "\??\" prefix */
                len = (USHORT)(len - 8);
            }
        }
    }
    if (!len) {
        errno = EINVAL;
        return -1;
    }
    name = (const WCHAR*)((const BYTE*)base + off);
    n = WideCharToMultiByte(CP_UTF8, 0, name, (int)(len / sizeof(WCHAR)),
                            out, outsz ? (int)outsz - 1 : 0, NULL, NULL);
    if (n < 0) {
        errno = EINVAL;
        return -1;
    }
    if (outsz) {
        out[n] = 0;
    }
    return (ssize_t)n;
}

/* ------------------------------------------------------------------ */
/* Identity / processes                                                */
/* ------------------------------------------------------------------ */

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }
int   setuid(uid_t uid) { (void)uid; return 0; }
int   setgid(gid_t gid) { (void)gid; return 0; }
long  gettid(void) { return (long)GetCurrentThreadId(); }
pid_t getppid(void) { return 0; }
pid_t setsid(void)  { return 0; }

int getgroups(int size, gid_t list[])
{
    (void)list;
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;   /* no supplementary groups */
}

int setgroups(size_t size, const gid_t* list)
{
    (void)size;
    (void)list;
    return 0;
}

pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

int kill(pid_t pid, int sig)
{
    /* No cross-process signal delivery; report success so guest code
     * that signals itself does not bail out early */
    (void)pid;
    (void)sig;
    return 0;
}

pid_t wait4(pid_t pid, int* status, int options, void* rusage)
{
    (void)pid;
    (void)status;
    (void)options;
    (void)rusage;
    errno = ECHILD;
    return -1;
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return wait4(pid, status, options, NULL);
}

pid_t wait(int* status)
{
    return wait4(-1, status, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Clocks / sleeping / itimers                                         */
/* ------------------------------------------------------------------ */

int setitimer(int which, const struct itimerval* newval, struct itimerval* oldval)
{
    (void)which;
    (void)newval;
    if (oldval) {
        oldval->it_interval.tv_sec  = 0;
        oldval->it_interval.tv_usec = 0;
        oldval->it_value.tv_sec     = 0;
        oldval->it_value.tv_usec    = 0;
    }
    errno = ENOSYS;
    return -1;
}

int getitimer(int which, struct itimerval* cur)
{
    (void)which;
    if (cur) {
        cur->it_interval.tv_sec  = 0;
        cur->it_interval.tv_usec = 0;
        cur->it_value.tv_sec     = 0;
        cur->it_value.tv_usec    = 0;
    }
    return 0;
}

int getpagesize(void)
{
    return 4096;
}

/* ------------------------------------------------------------------ */
/* Process times / rusage / rlimits                                    */
/* ------------------------------------------------------------------ */

clock_t times(struct tms* buf)
{
    FILETIME ct, et, kt, ut;
    unsigned long long k = 0, u = 0;
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
        if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            /* 100ns ticks -> CLK_TCK jiffies */
            k = (((unsigned long long)kt.dwHighDateTime << 32) | kt.dwLowDateTime) / 10;
            u = (((unsigned long long)ut.dwHighDateTime << 32) | ut.dwLowDateTime) / 10;
            buf->tms_utime = (clock_t)(u * CLK_TCK / 1000000);
            buf->tms_stime = (clock_t)(k * CLK_TCK / 1000000);
        }
    }
    return (clock_t)(GetTickCount64() / (1000 / CLK_TCK));
}

int getrusage(int who, struct rusage* usage)
{
    FILETIME ct, et, kt, ut;
    (void)who;
    if (!usage) {
        errno = EFAULT;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
        unsigned long long u = (((unsigned long long)ut.dwHighDateTime << 32) | ut.dwLowDateTime) / 10;
        unsigned long long k = (((unsigned long long)kt.dwHighDateTime << 32) | kt.dwLowDateTime) / 10;
        usage->ru_utime.tv_sec  = (long)(u / 1000000);
        usage->ru_utime.tv_usec = (long)(u % 1000000);
        usage->ru_stime.tv_sec  = (long)(k / 1000000);
        usage->ru_stime.tv_usec = (long)(k % 1000000);
        usage->ru_maxrss = 0;
    }
    return 0;
}

int getrlimit(int resource, struct rlimit* rlim)
{
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    switch (resource) {
    case RLIMIT_CORE:
    case RLIMIT_NOFILE:
    case RLIMIT_STACK:
    case RLIMIT_AS:
    case RLIMIT_DATA:
    case RLIMIT_CPU:
        rlim->rlim_cur = RLIM_INFINITY;
        rlim->rlim_max = RLIM_INFINITY;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int setrlimit(int resource, const struct rlimit* rlim)
{
    (void)resource;
    (void)rlim;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mmap family                                                         */
/* ------------------------------------------------------------------ */

struct vma_node {
    void* base;
    struct vma_node* next;
};

static SRWLOCK vma_lock = SRWLOCK_INIT;
static struct vma_node* vma_nodes;

static void vma_track(void* base)
{
    struct vma_node* n = (struct vma_node*)malloc(sizeof(*n));
    if (!n) {
        return;
    }
    n->base = base;
    AcquireSRWLockExclusive(&vma_lock);
    n->next = vma_nodes;
    vma_nodes = n;
    ReleaseSRWLockExclusive(&vma_lock);
}

static int vma_untrack(void* base)
{
    struct vma_node** pp;
    int found = 0;
    AcquireSRWLockExclusive(&vma_lock);
    for (pp = &vma_nodes; *pp; pp = &(*pp)->next) {
        if ((*pp)->base == base) {
            struct vma_node* dead = *pp;
            *pp = dead->next;
            free(dead);
            found = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&vma_lock);
    return found;
}

static DWORD prot_to_win32(int prot)
{
    if (prot & PROT_EXEC) {
        if (prot & PROT_WRITE) return PAGE_EXECUTE_READWRITE;
        if (prot & PROT_READ)  return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ)  return PAGE_READONLY;
    return PAGE_NOACCESS;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset)
{
    size_t len = (length + 0xFFF) & ~((size_t)0xFFF);
    void* ret;

    if (!len) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    if (!(flags & MAP_ANONYMOUS) && fd != -1) {
        /* File-backed: CreateFileMapping + MapViewOfFileEx. Windows
         * requires 64K-aligned file offsets; emulate Linux by mapping
         * from (offset - delta) and handing back base + delta. */
        HANDLE h, fmh;
        DWORD delta = (DWORD)((unsigned long long)offset & 0xFFFF);
        unsigned long long map_off = (unsigned long long)offset - delta;
        DWORD map_len = (DWORD)(len + delta);
        DWORD win_prot, access;
        void* hint;
        void* base;

        h = (HANDLE)_get_osfhandle(fd);
        if (h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return MAP_FAILED;
        }
        if (delta && (flags & MAP_FIXED)) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (flags & MAP_PRIVATE) {
            win_prot = (prot & PROT_WRITE) ? PAGE_WRITECOPY : PAGE_READONLY;
        } else {
            win_prot = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        }
        access = FILE_MAP_READ;
        if (prot & PROT_WRITE) access = (flags & MAP_PRIVATE) ? FILE_MAP_COPY : FILE_MAP_WRITE;
        if (prot & PROT_EXEC)  access |= FILE_MAP_EXECUTE;

        fmh = CreateFileMappingA(h, NULL, win_prot, (DWORD)(((unsigned long long)map_len) >> 32), (DWORD)map_len, NULL);
        if (!fmh) {
            win_set_errno();
            return MAP_FAILED;
        }
        hint = (addr && !(flags & MAP_FIXED)) ? (void*)((char*)addr - delta) :
               (flags & MAP_FIXED)            ? (void*)((char*)addr - delta) : NULL;
        base = MapViewOfFileEx(fmh, access, (DWORD)(map_off >> 32), (DWORD)map_off, map_len, hint);
        CloseHandle(fmh);
        if (!base) {
            win_set_errno();
            return MAP_FAILED;
        }
        if ((flags & MAP_FIXED) && base != (void*)((char*)addr - delta)) {
            UnmapViewOfFile(base);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return (char*)base + delta;
    }

    /* Anonymous mapping via VirtualAlloc */
    if (flags & MAP_FIXED) {
        /* Best effort: free whatever occupies the range. Only works if
         * addr is an allocation base we previously created. */
        if (!vma_untrack(addr) || !VirtualFree(addr, 0, MEM_RELEASE)) {
            fprintf(stderr, "[shim-mmap] MAP_FIXED|ANON addr=%p len=%zu prot=0x%x -> untrack failed, falling back to plain VirtualAlloc\n",
                    addr, length, prot);
            SetLastError(ERROR_INVALID_ADDRESS);
            /* win_set_errno() sets EIO for unknown errors (scudo sees I/O error) */
            /* fall through to plain VirtualAlloc at the requested hint */
        }
    }
    ret = VirtualAlloc((flags & MAP_FIXED) ? addr : NULL, len,
                       MEM_RESERVE | MEM_COMMIT, prot_to_win32(prot));
    if (!ret) {
        fprintf(stderr, "[shim-mmap] VirtualAlloc failed addr=%p len=%zu err=%u\n", addr, len, GetLastError());
        win_set_errno();
        return MAP_FAILED;
    }
    vma_track(ret);
    return ret;
}

int munmap(void* addr, size_t length)
{
    MEMORY_BASIC_INFORMATION mbi;
    (void)length;
    if (!addr || addr == MAP_FAILED) {
        errno = EINVAL;
        return -1;
    }
    if (vma_untrack(addr)) {
        if (VirtualFree(addr, 0, MEM_RELEASE)) {
            return 0;
        }
        win_set_errno();
        return -1;
    }
    /* File-backed view: find its allocation base and unmap that */
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_MAPPED && mbi.AllocationBase) {
        if (UnmapViewOfFile(mbi.AllocationBase)) {
            return 0;
        }
        win_set_errno();
        return -1;
    }
    if (VirtualFree(addr, 0, MEM_RELEASE)) {
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int mprotect(void* addr, size_t length, int prot)
{
    DWORD old;
    (void)length;
    if (!VirtualProtect(addr, length ? length : 1, prot_to_win32(prot), &old)) {
        win_set_errno();
        return -1;
    }
    return 0;
}

int madvise(void* addr, size_t length, int advice)
{
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Vector / positioned I/O                                             */
/* ------------------------------------------------------------------ */

ssize_t readv(int fd, const struct iovec* iov, int iovcnt)
{
    ssize_t total = 0;
    int i;
    if (iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < iovcnt; i++) {
        ssize_t r;
        if (!iov[i].iov_len) {
            continue;
        }
        r = _read(fd, iov[i].iov_base, (unsigned)iov[i].iov_len);
        if (r < 0) {
            return total ? total : -1;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt)
{
    ssize_t total = 0;
    int i;
    if (iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < iovcnt; i++) {
        ssize_t r;
        if (!iov[i].iov_len) {
            continue;
        }
        r = _write(fd, iov[i].iov_base, (unsigned)iov[i].iov_len);
        if (r < 0) {
            return total ? total : -1;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t pread(int fd, void* buf, size_t count, long long offset)
{
    long long saved = _lseeki64(fd, 0, SEEK_CUR);
    ssize_t r;
    if (saved < 0) {
        return -1;
    }
    if (_lseeki64(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    r = _read(fd, buf, (unsigned)count);
    _lseeki64(fd, saved, SEEK_SET);
    return r;
}

ssize_t pwrite(int fd, const void* buf, size_t count, long long offset)
{
    long long saved = _lseeki64(fd, 0, SEEK_CUR);
    ssize_t r;
    if (saved < 0) {
        return -1;
    }
    if (_lseeki64(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    r = _write(fd, buf, (unsigned)count);
    _lseeki64(fd, saved, SEEK_SET);
    return r;
}

/* ------------------------------------------------------------------ */
/* fd operations                                                       */
/* ------------------------------------------------------------------ */

/* Sockets are anchored onto CRT fds (see win_socket.c), so the descriptor
 * data path has to dispatch them before falling back to the CRT. The POSIX
 * signatures of read/write/close/dup are the MinGW <io.h> ones on purpose:
 * this is exactly the symbol those callers resolve against. */

static int shim_epoll_anchor_release(int fd);
static int shim_dir_forget(int fd);

int read(int fd, void* buf, unsigned int count)
{
    if (win_socket_is_fd(fd)) {
        return (int)win_socket_read(fd, buf, count);
    }
    return _read(fd, buf, count);
}

int write(int fd, const void* buf, unsigned int count)
{
    if (win_socket_is_fd(fd)) {
        return (int)win_socket_write(fd, buf, count);
    }
    return _write(fd, buf, count);
}

int close(int fd)
{
    if (win_socket_is_fd(fd)) {
        return win_socket_close(fd);
    }
    if (shim_epoll_anchor_release(fd)) {
        return 0;
    }
    shim_dir_forget(fd);
    return _close(fd);
}

int dup(int fd)
{
    if (win_socket_is_fd(fd)) {
        return win_socket_dup(fd);
    }
    return _dup(fd);
}

int dup2(int oldfd, int newfd)
{
    if (oldfd == newfd) {
        return newfd;
    }
    if (win_socket_is_fd(oldfd) || win_socket_is_fd(newfd)) {
        /* Moving a socket to a fixed descriptor would mean relocating its CRT
         * anchor; the guests that rely on dup2-on-socket (fork/exec plumbing)
         * have no Windows equivalent anyway. */
        errno = ENOSYS;
        return -1;
    }
    return _dup2(oldfd, newfd);
}

int pipe(int fds[2])
{
    return _pipe(fds, 65536, _O_BINARY);
}

int pipe2(int fds[2], int flags)
{
    /* O_NONBLOCK/O_CLOEXEC not supported on CRT pipes */
    (void)flags;
    return _pipe(fds, 65536, _O_BINARY);
}

int fsync(int fd)
{
    if (_commit(fd) < 0) {
        return -1;
    }
    return 0;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

int dup3(int oldfd, int newfd, int flags)
{
    (void)flags;
    return dup2(oldfd, newfd);
}

int fcntl(int fd, int cmd, ...)
{
    static int fd_flags[4096];
    long arg = 0;
    if (cmd == F_SETFL || cmd == F_SETFD || cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, long);
        va_end(ap);
    }
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return _dup(fd);
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    case F_GETFL:
        if (fd < 0 || fd >= 4096) {
            errno = EBADF;
            return -1;
        }
        return fd_flags[fd];
    case F_SETFL:
        if (fd < 0 || fd >= 4096) {
            errno = EBADF;
            return -1;
        }
        if (win_socket_is_fd(fd)) {
            /* The guest passes the Linux UAPI O_NONBLOCK (0x800), not MinGW's */
            if (win_socket_set_nonblock(fd, !!(arg & 0x800)) < 0) {
                return -1;
            }
        }
        fd_flags[fd] = (int)arg;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int flock(int fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

int fchdir(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    char path[MAX_PATH + 16];
    DWORD n;
    const char* p;
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    n = GetFinalPathNameByHandleA(h, path, MAX_PATH, 0);
    if (!n || n >= MAX_PATH) {
        errno = ENOSYS;
        return -1;
    }
    path[n] = 0;
    p = path;
    if (strncmp(p, "\\\\?\\UNC\\", 8) == 0)       p += 6;
    else if (strncmp(p, "\\\\?\\", 4) == 0)       p += 4;
    if (!SetCurrentDirectoryA(p)) {
        win_set_errno();
        return -1;
    }
    return 0;
}

int umask(int mask)
{
    return _umask(mask);
}

/* ------------------------------------------------------------------ */
/* Directory enumeration                                               */
/*                                                                     */
/* A guest lists a directory through getdents64: opendir() opens it    */
/* (see openat(), which wraps the directory handle the CRT refuses) and */
/* every readdir() is a getdents64 syscall. The guest layout is        */
/* mirrored here because the Win32 walk is the only producer of it.    */
/* ------------------------------------------------------------------ */

/* Mirror of rvvm_user.c's struct uapi_linux_dirent64 */
struct shim_linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];
};

#define SHIM_DIR_MAX 64
#define SHIM_DIR_BUF 8192

#define SHIM_DT_DIR 4
#define SHIM_DT_REG 8
#define SHIM_DT_LNK 10

typedef struct {
    int     used;
    int     fd;
    HANDLE  handle;
    bool    eof;
    bool    drained;                 /* no unparsed record left in buf */
    int64_t cookie;                  /* handed out as d_off */
    DWORD   off;                     /* consumed bytes in buf */
    uint64_t buf[SHIM_DIR_BUF / 8];  /* FILE_ID_BOTH_DIR_INFO records */
} shim_dir_t;

static SRWLOCK    shim_dir_lock = SRWLOCK_INIT;
static shim_dir_t shim_dirs[SHIM_DIR_MAX];

static int shim_dir_forget(int fd)
{
    int i, found = 0;
    AcquireSRWLockExclusive(&shim_dir_lock);
    for (i = 0; i < SHIM_DIR_MAX; i++) {
        if (shim_dirs[i].used && shim_dirs[i].fd == fd) {
            shim_dirs[i].used = 0;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&shim_dir_lock);
    return found;
}

/* Look up (or start) the walk for @fd; the caller holds shim_dir_lock */
static shim_dir_t* shim_dir_acquire(int fd)
{
    HANDLE h;
    int i;
    for (i = 0; i < SHIM_DIR_MAX; i++) {
        if (shim_dirs[i].used && shim_dirs[i].fd == fd) {
            return &shim_dirs[i];
        }
    }
    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        errno = EBADF;
        return NULL;
    }
    for (i = 0; i < SHIM_DIR_MAX; i++) {
        if (!shim_dirs[i].used) {
            memset(&shim_dirs[i], 0, sizeof(shim_dirs[i]));
            shim_dirs[i].used = 1;
            shim_dirs[i].fd = fd;
            shim_dirs[i].handle = h;
            shim_dirs[i].drained = true;    /* nothing buffered yet */
            return &shim_dirs[i];
        }
    }
    errno = EMFILE;
    return NULL;
}

/* Pull the next batch of entries; the caller holds shim_dir_lock */
static int shim_dir_refill(shim_dir_t* d)
{
    /* MinGW's GetFileInformationByHandleEx() has no out-length parameter, so
     * the batch is walked through NextEntryOffset instead (a zero offset marks
     * its last record). */
    if (!GetFileInformationByHandleEx(d->handle, FileIdBothDirectoryInfo,
                                      (void*)d->buf, (DWORD)sizeof(d->buf))) {
        DWORD err = GetLastError();
        if (err == ERROR_NO_MORE_FILES) {
            d->eof = true;
            return 0;
        }
        if (err == ERROR_INVALID_PARAMETER || err == ERROR_DIRECTORY) {
            /* A plain file descriptor, not a directory */
            errno = ENOTDIR;
            return -1;
        }
        win_set_errno();
        return -1;
    }
    d->off = 0;
    d->drained = false;
    return 0;
}

/*
 * getdents64 over a directory handle. The walk state lives per fd, so a
 * multi-call listing (a guest buffer smaller than the directory) resumes where
 * it stopped instead of restarting.
 */
static ssize_t shim_getdents64(int fd, void* out, size_t size)
{
    shim_dir_t* d;
    size_t written = 0;

    if (!out && size) {
        errno = EFAULT;
        return -1;
    }

    AcquireSRWLockExclusive(&shim_dir_lock);
    d = shim_dir_acquire(fd);
    if (!d) {
        goto fail;
    }

    for (;;) {
        while (!d->drained) {
            const FILE_ID_BOTH_DIR_INFO* ent =
                (const FILE_ID_BOTH_DIR_INFO*)((const BYTE*)d->buf + d->off);
            DWORD next = ent->NextEntryOffset;
            char name[1024];
            int name_len;
            size_t reclen;
            struct shim_linux_dirent64* de;

            name_len = WideCharToMultiByte(CP_UTF8, 0, ent->FileName,
                                           (int)(ent->FileNameLength / sizeof(WCHAR)),
                                           name, sizeof(name) - 1, NULL, NULL);
            if (name_len < 0) {
                name_len = 0;
            }
            name[name_len] = 0;

            /* linux_dirent64: 8+8+2+1+name+NUL, 8-byte aligned */
            reclen = (sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + 1 +
                      (size_t)name_len + 1 + 7) & ~(size_t)7;
            if (reclen > size - written) {
                /* Does not fit: leave the entry for the next call */
                ReleaseSRWLockExclusive(&shim_dir_lock);
                return (ssize_t)written;
            }

            de = (struct shim_linux_dirent64*)((BYTE*)out + written);
            memset(de, 0, reclen);
            de->d_ino = (uint64_t)ent->FileId.QuadPart;
            if (!de->d_ino) {
                de->d_ino = 1;
            }
            de->d_off = ++d->cookie;
            de->d_reclen = (uint16_t)reclen;
            if (ent->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                /* A symlink or a directory junction */
                de->d_type = SHIM_DT_LNK;
            } else {
                de->d_type = (ent->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                             ? SHIM_DT_DIR : SHIM_DT_REG;
            }
            memcpy(de->d_name, name, (size_t)name_len);

            written += reclen;
            if (next) {
                d->off += next;
            } else {
                d->drained = true;
            }
        }

        if (d->eof) {
            break;
        }
        if (shim_dir_refill(d) < 0) {
            goto fail;
        }
        if (d->eof) {
            break;
        }
    }

    ReleaseSRWLockExclusive(&shim_dir_lock);
    return (ssize_t)written;

fail:
    ReleaseSRWLockExclusive(&shim_dir_lock);
    return -1;
}

/*
 * The CRT cannot open a directory, so fdopendir() has no fd to work from:
 * resolve the handle back to a path and let opendir() (FindFirstFile-backed)
 * do the walk. The DIR* therefore starts from the beginning whatever the fd's
 * own position is - good enough here, and far better than ENOSYS.
 */
DIR* fdopendir(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    char path[MAX_PATH + 16];
    DWORD n;
    const char* p;
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        errno = EBADF;
        return NULL;
    }
    n = GetFinalPathNameByHandleA(h, path, MAX_PATH, 0);
    if (!n || n >= MAX_PATH) {
        errno = ENOSYS;
        return NULL;
    }
    path[n] = 0;
    p = path;
    if (strncmp(p, "\\\\?\\UNC\\", 8) == 0)      p += 6;
    else if (strncmp(p, "\\\\?\\", 4) == 0)      p += 4;
    return opendir(p);
}

/* ------------------------------------------------------------------ */
/* *at() family                                                        */
/* ------------------------------------------------------------------ */

int openat(int dirfd, const char* path, int flags, ...)
{
    char full[MAX_PATH + 16];
    mode_t mode = 0;
    DWORD attrs;
    int fd;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    fd = _open(full, flags | _O_BINARY, mode);
    if (fd >= 0) {
        return fd;
    }

    /* UCRT refuses to open directories, but guests do open them (opendir(),
     * O_DIRECTORY) and then enumerate with getdents64, so hand out a descriptor
     * wrapping the directory handle. FILE_FLAG_BACKUP_SEMANTICS is what allows
     * CreateFile to touch a directory at all. */
    attrs = GetFileAttributesA(full);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        HANDLE h = CreateFileA(full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            int dfd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
            if (dfd >= 0) {
                return dfd;
            }
            CloseHandle(h);
        }
        win_set_errno();
    }
    return -1;
}

int mkdirat(int dirfd, const char* path, mode_t mode)
{
    char full[MAX_PATH + 16];
    (void)mode;
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    return _mkdir(full);
}

int mknodat(int dirfd, const char* path, mode_t mode, unsigned long long dev)
{
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)dev;
    errno = ENOSYS;
    return -1;
}

int unlinkat(int dirfd, const char* path, int flags)
{
    char full[MAX_PATH + 16];
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    if (flags & AT_REMOVEDIR) {
        return _rmdir(full);
    }
    if (_unlink(full) == 0) {
        return 0;
    }
    /* A symlink to a directory (or a junction) is a directory object: POSIX
     * unlink(2) removes the link in either case, so retry accordingly. */
    if (shim_path_is_reparse(full) && _rmdir(full) == 0) {
        return 0;
    }
    return -1;
}

/*
 * A failed CreateSymbolicLinkA() reports the reason through GetLastError(). An
 * already existing name is final: retrying with another flag combination would
 * overwrite the errno with a different failure (typically EPERM from the
 * privileged attempt). Returns true when the caller must stop and fail.
 */
static bool win_symlink_errno_pending(void)
{
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
        errno = EEXIST;
        return true;
    }
    return false;
}

/*
 * Windows stores the link *type* in the reparse point, so it must be chosen at
 * creation time; a file link to a directory cannot be followed as a directory.
 * Resolve a relative target against the directory holding the link (which is
 * how both POSIX and Win32 interpret it) and ask the object that exists there.
 */
static bool win_symlink_target_is_dir(const char* target, const char* linkpath)
{
    char resolved[MAX_PATH + 16];
    const char* base = target;
    DWORD attrs;

    if (target[0] != '\\' && target[0] != '/' &&
        !(((target[0] >= 'A' && target[0] <= 'Z') || (target[0] >= 'a' && target[0] <= 'z')) &&
          target[1] == ':')) {
        const char* slash = strrchr(linkpath, '\\');
        const char* slash2 = strrchr(linkpath, '/');
        if (slash2 && (!slash || slash2 > slash)) {
            slash = slash2;
        }
        if (slash) {
            size_t dirlen = (size_t)(slash - linkpath) + 1;
            if (dirlen + strlen(target) >= sizeof(resolved)) {
                return false;
            }
            memcpy(resolved, linkpath, dirlen);
            snprintf(resolved + dirlen, sizeof(resolved) - dirlen, "%s", target);
            base = resolved;
        }
    }
    attrs = GetFileAttributesA(base);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

int symlinkat(const char* target, int dirfd, const char* linkpath)
{
    char full[MAX_PATH + 16];
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    DWORD dirflag;

    if (!target || !linkpath) {
        errno = EFAULT;
        return -1;
    }
    if (at_path(dirfd, linkpath, full, sizeof(full))) {
        return -1;
    }
    dirflag = win_symlink_target_is_dir(target, full) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;

    if (CreateSymbolicLinkA(full, target, flags | dirflag)) {
        return 0;
    }
    if (win_symlink_errno_pending()) {
        return -1;
    }
    /* The target may be a dangling name, or the guess above may be wrong */
    if (CreateSymbolicLinkA(full, target, flags | (dirflag ^ SYMBOLIC_LINK_FLAG_DIRECTORY))) {
        return 0;
    }
    if (win_symlink_errno_pending()) {
        return -1;
    }
    /* Windows older than 1703 rejects the unprivileged flag outright */
    if (CreateSymbolicLinkA(full, target, dirflag) ||
        CreateSymbolicLinkA(full, target, dirflag ^ SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        return 0;
    }
    win_set_errno();
    return -1;
}

ssize_t linkat(int fd1, const char* path1, int fd2, const char* path2, int flags)
{
    char from[MAX_PATH + 16], to[MAX_PATH + 16];
    (void)flags;    /* no way to express AT_SYMLINK_FOLLOW differently */

    if (!path1 || !path2) {
        errno = EFAULT;
        return -1;
    }
    if (at_path(fd1, path1, from, sizeof(from)) ||
        at_path(fd2, path2, to, sizeof(to))) {
        return -1;
    }
    if (!CreateHardLinkA(to, from, NULL)) {
        win_set_errno();
        return -1;
    }
    return 0;
}

int renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath)
{
    char from[MAX_PATH + 16], to[MAX_PATH + 16];
    if (at_path(olddirfd, oldpath, from, sizeof(from)) ||
        at_path(newdirfd, newpath, to, sizeof(to))) {
        return -1;
    }
    if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
        win_set_errno();
        return -1;
    }
    return 0;
}

int faccessat(int dirfd, const char* path, int mode, int flags)
{
    char full[MAX_PATH + 16];
    (void)flags;
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    if (mode & X_OK) {
        /* No exec bit concept; only refuse if the file is missing */
        struct rvvm_stat st;
        if (rvvm_stat(full, &st)) {
            return -1;
        }
        mode &= ~X_OK;
    }
    return _access(full, mode);
}

ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz)
{
    char full[MAX_PATH + 16];
    char tmp[MAX_PATH + 16];
    ssize_t n;
    size_t i;

    if (!buf && bufsiz) {
        errno = EFAULT;
        return -1;
    }
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    if (!strcmp(path, "/proc/self/exe")) {
        DWORD len = GetModuleFileNameA(NULL, tmp, MAX_PATH);
        if (!len || len >= MAX_PATH) {
            errno = EINVAL;
            return -1;
        }
        n = (ssize_t)len;
    } else if (!strcmp(path, "/proc/self/cwd")) {
        DWORD len = GetCurrentDirectoryA(MAX_PATH, tmp);
        if (!len || len >= MAX_PATH) {
            errno = EINVAL;
            return -1;
        }
        n = (ssize_t)len;
    } else {
        /* A symbolic link (or directory junction): its target is stored as a
         * reparse point. Anything else is EINVAL, as on POSIX. */
        n = shim_read_link(full, tmp, sizeof(tmp));
        if (n < 0) {
            return -1;
        }
    }
    /* The guest never sees Windows separators */
    for (i = 0; i < (size_t)n && i < bufsiz; i++) {
        buf[i] = (tmp[i] == '\\') ? '/' : tmp[i];
    }
    return (ssize_t)i;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int fchmodat(int dirfd, const char* path, mode_t mode, int flags)
{
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)flags;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags)
{
    (void)dirfd;
    (void)path;
    (void)owner;
    (void)group;
    (void)flags;
    return 0;
}

/* ------------------------------------------------------------------ */
/* stat / statfs                                                       */
/* ------------------------------------------------------------------ */

static void stat_fill(struct rvvm_stat* out, const struct _stat64* in)
{
    memset(out, 0, sizeof(*out));
    out->st_dev    = in->st_dev;
    out->st_ino    = in->st_ino;
    out->st_mode   = in->st_mode;
    out->st_nlink  = in->st_nlink ? in->st_nlink : 1;
    out->st_uid    = 0;
    out->st_gid    = 0;
    out->st_rdev   = in->st_rdev;
    out->st_size   = in->st_size;
    out->st_blksize = 4096;
    out->st_blocks = (in->st_size + 511) / 512;
    out->st_atime  = (long long)in->st_atime;
    out->st_mtime  = (long long)in->st_mtime;
    out->st_ctime  = (long long)in->st_ctime;
}

static long long filetime_to_unix(const FILETIME* ft)
{
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (!t) {
        return 0;
    }
    return (long long)((t / 10000000ULL) - 11644473600ULL);
}

/*
 * Refine a stat result from the file's handle.
 *
 * The CRT path helpers are not a reliable source here: _stat64() reads the
 * directory metadata, which on some volumes (network/virtual drives) still
 * reports the *old* size right after a write, and _fstat64() never sets
 * S_IFDIR for a directory. Querying BY_HANDLE_FILE_INFORMATION gives the live
 * size, the real file index and the directory attribute - and, because both
 * stat() and fstat() go through it, they agree with each other.
 */
static void stat_refine(struct rvvm_stat* out, HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (h == INVALID_HANDLE_VALUE || h == NULL || !GetFileInformationByHandle(h, &info)) {
        return;
    }
    out->st_dev    = info.dwVolumeSerialNumber;
    out->st_ino    = ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    out->st_nlink  = info.nNumberOfLinks ? info.nNumberOfLinks : 1;
    out->st_size   = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    out->st_blocks = (out->st_size + 511) / 512;
    out->st_mode   = (out->st_mode & ~(unsigned)_S_IFMT) |
                     ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR : _S_IFREG);
    out->st_atime  = filetime_to_unix(&info.ftLastAccessTime);
    out->st_mtime  = filetime_to_unix(&info.ftLastWriteTime);
    out->st_ctime  = filetime_to_unix(&info.ftCreationTime);
}

/* FILE_READ_ATTRIBUTES + FILE_FLAG_BACKUP_SEMANTICS also opens directories */
static void stat_refine_path(struct rvvm_stat* out, const char* path)
{
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        stat_refine(out, h);
        CloseHandle(h);
    }
}

int rvvm_stat(const char* path, struct rvvm_stat* buf)
{
    struct _stat64 st;
    if (_stat64(path, &st)) {
        return -1;
    }
    stat_fill(buf, &st);
    stat_refine_path(buf, path);
    return 0;
}

#ifndef _S_IFLNK
#define _S_IFLNK 0xA000     /* Linux S_IFLNK; some CRT headers omit it */
#endif

int rvvm_lstat(const char* path, struct rvvm_stat* buf)
{
    DWORD attrs = GetFileAttributesA(path);
    char target[MAX_PATH + 16];
    HANDLE h;

    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* Not a link: stat() is both the answer and the errno source */
        return rvvm_stat(path, buf);
    }
    if (shim_read_link(path, target, sizeof(target)) < 0) {
        /* A reparse point that is not a name surrogate (a cloud placeholder,
         * say) stands for a real object: describe that instead */
        return rvvm_stat(path, buf);
    }

    /* lstat(2) describes the link itself: S_IFLNK plus the target length */
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = _S_IFLNK | 0777;
    buf->st_nlink = 1;
    buf->st_blksize = 4096;
    buf->st_size = (long long)strlen(target);

    h = CreateFileA(path, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        /* dev/ino/times of the link object, then the link's own mode/size back
         * on top (stat_refine() reports the attributes it sees) */
        stat_refine(buf, h);
        buf->st_mode = (buf->st_mode & ~(unsigned)_S_IFMT) | _S_IFLNK;
        buf->st_size = (long long)strlen(target);
        CloseHandle(h);
    }
    return 0;
}

int rvvm_fstat(int fd, struct rvvm_stat* buf)
{
    struct _stat64 st;
    if (_fstat64(fd, &st)) {
        return -1;
    }
    stat_fill(buf, &st);
    stat_refine(buf, (HANDLE)_get_osfhandle(fd));
    return 0;
}

int fstatat(int dirfd, const char* path, struct rvvm_stat* buf, int flags)
{
    char full[MAX_PATH + 16];
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    if (flags & AT_SYMLINK_NOFOLLOW) {
        return rvvm_lstat(full, buf);
    }
    return rvvm_stat(full, buf);
}

int statfs(const char* path, struct statfs* buf)
{
    ULARGE_INTEGER total, avail;
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    total.QuadPart = 0;
    avail.QuadPart = 0;
    if (path && GetDiskFreeSpaceExA(path, &avail, &total, NULL)) {
        buf->f_blocks = total.QuadPart / 4096;
        buf->f_bfree  = avail.QuadPart / 4096;
        buf->f_bavail = buf->f_bfree;
        buf->f_files  = buf->f_blocks;
        buf->f_ffree  = buf->f_bfree;
    }
    buf->f_type    = 0xEF53;    /* pretend ext2 */
    buf->f_bsize   = 4096;
    buf->f_namelen = 255;
    buf->f_frsize  = 4096;
    return 0;
}

int fstatfs(int fd, struct statfs* buf)
{
    (void)fd;
    return statfs(NULL, buf);
}

/* ------------------------------------------------------------------ */
/* poll / select / ioctl / shm - stubs                                 */
/* ------------------------------------------------------------------ */

/* Upper bound WaitForMultipleObjects() accepts in one call */
#define SHIM_POLL_MAX_WAIT MAXIMUM_WAIT_OBJECTS

/* Translate the *current* readiness of one fd into revents bits. Only called
 * after WaitForMultipleObjects() reported the underlying handle as signaled
 * (or for the immediate, non-blocking probe path). */
static short shim_poll_revents(int fd, short events)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    short  revents = 0;

    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return POLLNVAL;
    }

    if (GetFileType(h) == FILE_TYPE_PIPE) {
        DWORD avail = 0;
        if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            if (avail && (events & POLLIN)) {
                revents |= POLLIN;
            }
            /* Room in the pipe buffer: a write of at most PIPE_BUF bytes
             * completes without blocking */
            if (events & POLLOUT) {
                revents |= POLLOUT;
            }
        }
        else if (GetLastError() == ERROR_BROKEN_PIPE) {
            /* Every write end is gone: reads report EOF and we are hung up */
            revents |= POLLHUP;
            if (events & POLLIN) {
                revents |= POLLIN;
            }
        }
    }
    else {
        /* Disk files and consoles: a signaled handle means the transfer is
         * ready to proceed */
        if (events & POLLIN) {
            revents |= POLLIN;
        }
        if (events & POLLOUT) {
            revents |= POLLOUT;
        }
    }

    return revents;
}

/*
 * poll() over Win32 kernel objects - the no-socket case.
 *
 * Unlike a real Linux guest, rvvm-user passes syscalls straight through, so
 * the guest's pipe()/read()/poll() share our CRT fd table and the underlying
 * HANDLEs are waitable: an anonymous pipe's read handle is signaled exactly
 * when data is available and its write handle when there is buffer space.
 *
 * This is what makes the virtpass vsync-fd path (ALooper_addFd) work: the
 * guest parks in poll() on the pipe it owns and the host's frame clock wakes
 * it with a plain write(). The old Sleep()-and-return-0 stub could not wake
 * anyone, so the guest would have had to spin.
 *
 * Called by poll() for fd sets without WinSock sockets: those are not waitable
 * objects and take the WSAPoll() path there instead.
 */
static int shim_poll_objects(struct pollfd* fds, nfds_t nfds, int timeout)
{
    HANDLE    handles[SHIM_POLL_MAX_WAIT];
    int       map[SHIM_POLL_MAX_WAIT];   /* fds[i] -> index into handles, or -1 */
    unsigned  nhandles = 0;
    int       ready = 0;
    ULONGLONG deadline = 0;
    nfds_t    i;
    unsigned  k;

    if (!fds || nfds <= 0) {
        /* Nothing to wait on: only the timeout is meaningful */
        Sleep(timeout > 0 ? (DWORD)timeout : 1);
        return 0;
    }

    if (nfds > SHIM_POLL_MAX_WAIT) {
        /* More fds than WaitForMultipleObjects() can take at once. Keep the
         * old pacing behaviour rather than blocking on a partial set. */
        for (i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        Sleep(timeout > 0 ? (DWORD)timeout : 1);
        return 0;
    }

    if (timeout >= 0) {
        deadline = GetTickCount64() + (ULONGLONG)timeout;
    }

    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        map[i] = -1;
    }

    /* One waitable handle per distinct fd; flag the invalid ones right away */
    for (i = 0; i < nfds; i++) {
        HANDLE h;
        if (fds[i].fd < 0) {
            continue;
        }
        h = (HANDLE)_get_osfhandle(fds[i].fd);
        if (h == INVALID_HANDLE_VALUE || h == NULL) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        for (k = 0; k < nhandles; k++) {
            if (handles[k] == h) {
                break;
            }
        }
        if (k == nhandles) {
            handles[nhandles] = h;
            nhandles++;
        }
        map[i] = (int)k;
    }

    if (ready) {
        return ready;
    }
    if (!nhandles) {
        /* Only negative fds: mimic a plain sleep */
        if (timeout > 0) {
            Sleep((DWORD)timeout);
        }
        else if (timeout < 0) {
            Sleep(1);
        }
        return 0;
    }

    for (;;) {
        DWORD wait_ms;
        DWORD r;
        int   woken = 0;

        if (timeout < 0) {
            wait_ms = INFINITE;
        }
        else {
            ULONGLONG now = GetTickCount64();
            wait_ms = (now >= deadline) ? 0 : (DWORD)(deadline - now);
        }

        r = WaitForMultipleObjects(nhandles, handles, FALSE, wait_ms);
        if (r == WAIT_FAILED) {
            errno = EINVAL;
            return -1;
        }
        if (r == WAIT_TIMEOUT) {
            return 0;
        }

        /* A handle signaled: report every fd that maps onto it. Duplicated fds
         * share a handle slot, so walk all of them. */
        for (i = 0; i < nfds; i++) {
            if (fds[i].fd < 0 || map[i] < 0) {
                continue;
            }
            if ((unsigned)map[i] != r - WAIT_OBJECT_0) {
                continue;
            }
            fds[i].revents |= shim_poll_revents(fds[i].fd, fds[i].events);
            if (fds[i].revents) {
                woken++;
            }
        }
        if (woken) {
            return woken;
        }
        if (timeout == 0) {
            /* Non-blocking probe with nothing we care about ready */
            return 0;
        }
        if (wait_ms == 0) {
            /* The deadline passed while we were checking */
            return 0;
        }
        /* Handle woke up but none of the requested events are ready: avoid a
         * hot spin on an uninteresting signal. */
        Sleep(1);
    }
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout)
{
    int       sidx[SHIM_POLL_MAX_WAIT];  /* socket subset: fds[] index */
    short     sev[SHIM_POLL_MAX_WAIT];
    short     srev[SHIM_POLL_MAX_WAIT];
    unsigned  nsockets = 0;
    unsigned  nvalid = 0;
    int       ready = 0;
    ULONGLONG deadline = 0;
    nfds_t    i;
    unsigned  k;

    if (!fds || nfds <= 0) {
        /* Nothing to wait on: only the timeout is meaningful */
        Sleep(timeout > 0 ? (DWORD)timeout : 1);
        return 0;
    }

    if (nfds > SHIM_POLL_MAX_WAIT) {
        /* More fds than WaitForMultipleObjects() can take at once. Keep the
         * old pacing behaviour rather than blocking on a partial set. */
        for (i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        Sleep(timeout > 0 ? (DWORD)timeout : 1);
        return 0;
    }

    if (timeout >= 0) {
        deadline = GetTickCount64() + (ULONGLONG)timeout;
    }

    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;
        }
        nvalid++;
        if (win_socket_is_fd(fds[i].fd)) {
            nsockets++;
        }
    }

    /*
     * A WinSock socket is not a waitable kernel object, so sockets can only be
     * waited on with WSAPoll(). Three cases:
     *   - sockets only       -> one WSAPoll() call, timeout exactly honoured
     *   - kernel objects only-> WaitForMultipleObjects() as before (this is the
     *                           vsync pipe path the virtpass hosts rely on)
     *   - mixed              -> 1 ms polls; the rare case that cannot be served
     *                           by a single host wait
     */
    if (nsockets && nsockets == nvalid) {
        int sfd[SHIM_POLL_MAX_WAIT];
        unsigned n = 0;
        int r;
        for (i = 0; i < nfds; i++) {
            if (fds[i].fd < 0) {
                continue;
            }
            sidx[n] = (int)i;
            sev[n] = fds[i].events;
            srev[n] = 0;
            n++;
        }
        for (k = 0; k < n; k++) {
            sfd[k] = fds[sidx[k]].fd;
        }
        r = win_socket_wait_many(sfd, sev, srev, (int)n, timeout);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        for (k = 0; k < n; k++) {
            if (srev[k]) {
                fds[sidx[k]].revents = srev[k];
                ready++;
            }
        }
        return ready;
    }

    if (nsockets) {
        /* Mixed set: probe everything without blocking */
        for (;;) {
            int done = 0;
            for (i = 0; i < nfds; i++) {
                if (fds[i].fd < 0 || fds[i].revents) {
                    continue;
                }
                if (win_socket_is_fd(fds[i].fd)) {
                    int r = win_socket_wait(fds[i].fd, fds[i].events, 0);
                    if (r < 0) {
                        fds[i].revents = POLLNVAL;
                        done++;
                    } else if (r) {
                        fds[i].revents = (short)r;
                        done++;
                    }
                } else {
                    HANDLE h = (HANDLE)_get_osfhandle(fds[i].fd);
                    if (h == INVALID_HANDLE_VALUE || h == NULL) {
                        fds[i].revents = POLLNVAL;
                        done++;
                    } else if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
                        fds[i].revents = shim_poll_revents(fds[i].fd, fds[i].events);
                        if (fds[i].revents) {
                            done++;
                        }
                    }
                }
            }
            if (done) {
                return done;
            }
            if (timeout == 0) {
                return 0;
            }
            if (timeout > 0 && GetTickCount64() >= deadline) {
                return 0;
            }
            Sleep(1);
        }
    }

    /* No sockets: the plain Win32 object wait handles the whole set */
    return shim_poll_objects(fds, nfds, timeout);
}

/*
 * select() over poll(), so that sockets, pipes and regular files share one
 * readiness path (there is no CRT select() on Windows, and WinSock's version
 * only knows WinSock sockets - the guest's fd space is a CRT one here).
 */
int rvvm_win_select(int nfds, fd_set* rset, fd_set* wset, fd_set* eset,
                    struct timeval* timeout)
{
    struct pollfd pfds[FD_SETSIZE];
    unsigned char which[FD_SETSIZE];    /* bit 1: read, 2: write, 4: except */
    fd_set ro, wo, eo;
    nfds_t n = 0;
    int timeout_ms = -1;
    int ready, i;
    unsigned k;

    (void)nfds;

    if (timeout) {
        timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }
    }

#define SHIM_SELECT_ADD(set, bit, ev)                                     \
    do {                                                                  \
        const fd_set* s_ = (set);                                         \
        if (s_) {                                                         \
            for (k = 0; k < s_->fd_count; k++) {                          \
                int fd_ = s_->fd_array[k];                                \
                nfds_t j_;                                                \
                for (j_ = 0; j_ < n; j_++) {                              \
                    if (pfds[j_].fd == fd_) {                             \
                        pfds[j_].events |= (ev);                          \
                        which[j_] |= (bit);                               \
                        break;                                            \
                    }                                                     \
                }                                                         \
                if (j_ == n && n < FD_SETSIZE) {                          \
                    pfds[n].fd = fd_;                                     \
                    pfds[n].events = (ev);                                \
                    pfds[n].revents = 0;                                  \
                    which[n] = (bit);                                     \
                    n++;                                                  \
                }                                                         \
            }                                                             \
        }                                                                 \
    } while (0)

    SHIM_SELECT_ADD(rset, 1, POLLIN);
    SHIM_SELECT_ADD(wset, 2, POLLOUT);
    SHIM_SELECT_ADD(eset, 4, POLLPRI);
#undef SHIM_SELECT_ADD

    ready = poll(pfds, n, timeout_ms);
    if (ready < 0) {
        return -1;
    }

    FD_ZERO(&ro);
    FD_ZERO(&wo);
    FD_ZERO(&eo);
    ready = 0;
    for (i = 0; i < (int)n; i++) {
        short rev = pfds[i].revents;
        int hit = 0;
        if (!rev) {
            continue;
        }
        if ((which[i] & 1) && (rev & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(pfds[i].fd, &ro);
            hit = 1;
        }
        if ((which[i] & 2) && (rev & (POLLOUT | POLLERR))) {
            FD_SET(pfds[i].fd, &wo);
            hit = 1;
        }
        if ((which[i] & 4) && (rev & POLLPRI)) {
            FD_SET(pfds[i].fd, &eo);
            hit = 1;
        }
        if (hit) {
            ready++;
        }
    }

    if (rset) *rset = ro;
    if (wset) *wset = wo;
    if (eset) *eset = eo;
    return ready;
}

/* ------------------------------------------------------------------ */
/* epoll - emulated over poll()                                        */
/*                                                                     */
/* Windows has neither epoll nor kqueue, and the guest's event loop is  */
/* the one place where a plain poll() is not enough on its own: epoll   */
/* keeps a persistent interest set. The instance is anchored to a CRT   */
/* fd (same trick as sockets) so close()/dup() keep working.            */
/* ------------------------------------------------------------------ */

#define SHIM_EPOLL_MAX_FDS       64
#define SHIM_EPOLL_MAX_INSTANCES 32

typedef struct {
    int      fd;
    uint32_t events;
    uint64_t data;
} shim_epoll_item_t;

typedef struct {
    int               used;
    size_t            count;
    int               anchor;
    shim_epoll_item_t items[SHIM_EPOLL_MAX_FDS];
} shim_epoll_t;

static SRWLOCK      shim_epoll_lock = SRWLOCK_INIT;
static shim_epoll_t shim_epolls[SHIM_EPOLL_MAX_INSTANCES];

static int shim_epoll_lookup(int epfd)
{
    int i;
    if (epfd < 0) {
        return -1;
    }
    for (i = 0; i < SHIM_EPOLL_MAX_INSTANCES; i++) {
        if (shim_epolls[i].used && shim_epolls[i].anchor == epfd) {
            return i;
        }
    }
    return -1;
}

static int shim_epoll_anchor_release(int fd)
{
    int i;
    int found = 0;
    AcquireSRWLockExclusive(&shim_epoll_lock);
    i = shim_epoll_lookup(fd);
    if (i >= 0) {
        shim_epolls[i].used = 0;
        shim_epolls[i].count = 0;
        shim_epolls[i].anchor = -1;
        found = 1;
    }
    ReleaseSRWLockExclusive(&shim_epoll_lock);
    if (found) {
        win_socket_free_anchor(fd);
    }
    return found;
}

static uint32_t shim_epoll_to_poll(uint32_t events)
{
    uint32_t p = 0;
    if (events & (EPOLLIN | EPOLLRDNORM))  p |= POLLIN;
    if (events & (EPOLLOUT | EPOLLWRNORM)) p |= POLLOUT;
    if (events & EPOLLPRI)                 p |= POLLPRI;
    return p;
}

static uint32_t shim_poll_to_epoll(short revents)
{
    uint32_t e = 0;
    if (revents & POLLIN)  e |= EPOLLIN;
    if (revents & POLLOUT) e |= EPOLLOUT;
    if (revents & POLLPRI) e |= EPOLLPRI;
    if (revents & POLLERR)                 e |= EPOLLERR;
    if (revents & POLLHUP)                 e |= EPOLLHUP;
    if (revents & POLLNVAL)                e |= EPOLLERR;
    return e;
}

int epoll_create1(int flags)
{
    int i, anchor, slot = -1;

    (void)flags;
    anchor = win_socket_alloc_anchor();
    if (anchor < 0) {
        return -1;
    }
    AcquireSRWLockExclusive(&shim_epoll_lock);
    for (i = 0; i < SHIM_EPOLL_MAX_INSTANCES; i++) {
        if (!shim_epolls[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        shim_epolls[slot].used = 1;
        shim_epolls[slot].count = 0;
        shim_epolls[slot].anchor = anchor;
    }
    ReleaseSRWLockExclusive(&shim_epoll_lock);
    if (slot < 0) {
        win_socket_free_anchor(anchor);
        errno = EMFILE;
        return -1;
    }
    return anchor;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)
{
    int slot = shim_epoll_lookup(epfd);
    int ret = 0;
    size_t i;

    if (slot < 0) {
        errno = EBADF;
        return -1;
    }
    if (op != EPOLL_CTL_DEL && !event) {
        errno = EFAULT;
        return -1;
    }

    AcquireSRWLockExclusive(&shim_epoll_lock);
    switch (op) {
    case EPOLL_CTL_ADD: {
        shim_epoll_t* ep = &shim_epolls[slot];
        for (i = 0; i < ep->count; i++) {
            if (ep->items[i].fd == fd) {
                ret = -1;
                errno = EEXIST;
                goto done;
            }
        }
        if (ep->count >= SHIM_EPOLL_MAX_FDS) {
            ret = -1;
            errno = ENOSPC;
            goto done;
        }
        ep->items[ep->count].fd = fd;
        ep->items[ep->count].events = event->events;
        ep->items[ep->count].data = event->data.u64;
        ep->count++;
        break;
    }
    case EPOLL_CTL_MOD: {
        shim_epoll_t* ep = &shim_epolls[slot];
        for (i = 0; i < ep->count; i++) {
            if (ep->items[i].fd == fd) {
                ep->items[i].events = event->events;
                ep->items[i].data = event->data.u64;
                break;
            }
        }
        if (i == ep->count) {
            ret = -1;
            errno = ENOENT;
        }
        break;
    }
    case EPOLL_CTL_DEL: {
        shim_epoll_t* ep = &shim_epolls[slot];
        for (i = 0; i < ep->count; i++) {
            if (ep->items[i].fd == fd) {
                ep->items[i] = ep->items[ep->count - 1];
                ep->count--;
                break;
            }
        }
        if (i == ep->count) {
            ret = -1;
            errno = ENOENT;
        }
        break;
    }
    default:
        ret = -1;
        errno = EINVAL;
        break;
    }

done:
    ReleaseSRWLockExclusive(&shim_epoll_lock);
    return ret;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
    struct pollfd pfds[SHIM_EPOLL_MAX_FDS];
    shim_epoll_item_t items[SHIM_EPOLL_MAX_FDS];
    int slot = shim_epoll_lookup(epfd);
    size_t count, i;
    int n = 0, ready, out = 0;

    if (slot < 0) {
        errno = EBADF;
        return -1;
    }
    if (!events || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }

    AcquireSRWLockExclusive(&shim_epoll_lock);
    count = shim_epolls[slot].count;
    for (i = 0; i < count; i++) {
        items[i] = shim_epolls[slot].items[i];
    }
    ReleaseSRWLockExclusive(&shim_epoll_lock);

    for (i = 0; i < count; i++) {
        pfds[n].fd = items[i].fd;
        pfds[n].events = (short)shim_epoll_to_poll(items[i].events);
        pfds[n].revents = 0;
        n++;
    }

    if (!n) {
        /* Nothing registered: only the timeout is meaningful */
        if (timeout > 0) {
            Sleep((DWORD)timeout);
        }
        return 0;
    }

    ready = poll(pfds, (nfds_t)n, timeout);
    if (ready < 0) {
        return -1;
    }
    if (ready == 0) {
        return 0;
    }

    for (i = 0; i < count && out < maxevents; i++) {
        uint32_t ev;
        if (!pfds[i].revents) {
            continue;
        }
        ev = shim_poll_to_epoll(pfds[i].revents);
        /* Only report what was subscribed, plus the always-reported errors */
        events[out].events = ev & (items[i].events | EPOLLERR | EPOLLHUP);
        events[out].data.u64 = items[i].data;
        out++;
    }
    return out;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void* arg = NULL;
    va_start(ap, req);
    arg = va_arg(ap, void*);
    va_end(ap);
    if (win_socket_is_fd(fd)) {
        return win_socket_ioctl(fd, req, arg);
    }
    (void)arg;
    errno = ENOTTY;
    return -1;
}

int shmget(key_t key, size_t size, int shmflg)
{
    (void)key;
    (void)size;
    (void)shmflg;
    errno = ENOSYS;
    return -1;
}

int shmctl(int shmid, int cmd, struct shmid_ds* buf)
{
    (void)shmid;
    (void)cmd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

void* shmat(int shmid, const void* shmaddr, int shmflg)
{
    (void)shmid;
    (void)shmaddr;
    (void)shmflg;
    errno = ENOSYS;
    return (void*)-1;
}

int shmdt(const void* shmaddr)
{
    (void)shmaddr;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Sockets - WinSock-backed (see win_socket.c)                         */
/*                                                                     */
/* The POSIX spellings are redirected to rvvm_win_* by the socket shim  */
/* header, so what is written below is the bridge's own namespace - the */
/* unprefixed names belong to ws2_32 (src/util/networking.c).          */
/* ------------------------------------------------------------------ */

int socket(int domain, int type, int protocol)
{
    return win_socket_create(domain, type, protocol);
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    if (!sv) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_pair(domain, type, protocol, sv);
}

int bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    if (!addr && len) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_bind(fd, addr, (int)len);
}

int listen(int fd, int backlog)
{
    return win_socket_listen(fd, backlog);
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    if ((addr && !addrlen) || (addrlen && !addr)) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_accept(fd, addr, (int*)addrlen, 0);
}

int accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags)
{
    if ((addr && !addrlen) || (addrlen && !addr)) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_accept(fd, addr, (int*)addrlen, flags);
}

int connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    if (!addr) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_connect(fd, addr, (int)len);
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    if (!addr || !addrlen) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_getsockname(fd, addr, (int*)addrlen);
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    if (!addr || !addrlen) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_getpeername(fd, addr, (int*)addrlen);
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* addr, socklen_t addrlen)
{
    if (!buf && len) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)win_socket_sendto(fd, buf, len, flags, addr, (int)addrlen);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* addr, socklen_t* addrlen)
{
    if (!buf && len) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)win_socket_recvfrom(fd, buf, len, flags, addr, (int*)addrlen);
}

int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
    return win_socket_setsockopt(fd, level, optname, optval, (int)optlen);
}

int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
    if (!optlen) {
        errno = EFAULT;
        return -1;
    }
    return win_socket_getsockopt(fd, level, optname, optval, (int*)optlen);
}

int shutdown(int fd, int how)
{
    return win_socket_shutdown(fd, how);
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags)
{
    if (!msg) {
        errno = EFAULT;
        return -1;
    }
    if (msg->msg_control && msg->msg_controllen) {
        /* Ancillary data (SCM_RIGHTS fd passing) has no Windows bridge */
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)win_socket_send_iov(fd, msg->msg_iov, (int)msg->msg_iovlen,
                                        flags, msg->msg_name, (int)msg->msg_namelen);
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags)
{
    int namelen;
    ssize_t ret;
    if (!msg) {
        errno = EFAULT;
        return -1;
    }
    if (msg->msg_control && msg->msg_controllen) {
        /* No control messages are ever delivered: report an empty cmsg list
         * instead of leaving the guest parsing stale bytes. */
        msg->msg_controllen = 0;
    }
    namelen = (int)msg->msg_namelen;
    ret = (ssize_t)win_socket_recv_iov(fd, msg->msg_iov, (int)msg->msg_iovlen, flags,
                                       msg->msg_name ? msg->msg_name : NULL,
                                       msg->msg_name ? &namelen : NULL);
    if (ret >= 0 && msg->msg_name) {
        msg->msg_namelen = (socklen_t)namelen;
    }
    msg->msg_flags = 0;
    return ret;
}

/* ------------------------------------------------------------------ */
/* syscall() dispatcher                                                */
/* ------------------------------------------------------------------ */

long syscall(long number, ...)
{
    va_list ap;
    long long a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;

    va_start(ap, number);
    a1 = va_arg(ap, long long);
    a2 = va_arg(ap, long long);
    a3 = va_arg(ap, long long);
    a4 = va_arg(ap, long long);
    a5 = va_arg(ap, long long);
    va_end(ap);

    switch (number) {
    case SYS_getdents64:    /* 217 */
        return (long)shim_getdents64((int)a1, (void*)(uintptr_t)a2, (size_t)a3);
    case 276:               /* SYS_renameat2 */
        {
            char from[MAX_PATH + 16], to[MAX_PATH + 16];
            unsigned flags = (unsigned)a5;
            DWORD mflags = 0;
            if (flags & 1 /* RENAME_NOREPLACE */) {
                struct rvvm_stat st;
                if (rvvm_stat((const char*)(uintptr_t)a2, &st) == 0) {
                    errno = EEXIST;
                    return -1;
                }
            } else {
                mflags = MOVEFILE_REPLACE_EXISTING;
            }
            if (at_path((int)a1, (const char*)(uintptr_t)a2, from, sizeof(from)) ||
                at_path((int)a3, (const char*)(uintptr_t)a4, to, sizeof(to))) {
                return -1;
            }
            if (!MoveFileExA(from, to, mflags)) {
                win_set_errno();
                return -1;
            }
            return 0;
        }
    default:
        errno = ENOSYS;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Random / memfd                                                      */
/* ------------------------------------------------------------------ */

typedef BOOLEAN (WINAPI* rtl_gen_random_fn)(void*, ULONG);

ssize_t getrandom(void* buf, size_t len, unsigned flags)
{
    static rtl_gen_random_fn fn;
    static int looked;
    (void)flags;
    if (!looked) {
        fn = (rtl_gen_random_fn)(void*)GetProcAddress(GetModuleHandleA("advapi32.dll"),
                                                      "SystemFunction036");
        looked = 1;
    }
    if (fn && fn(buf, (ULONG)len)) {
        return (ssize_t)len;
    }
    errno = ENOSYS;
    return -1;
}

int memfd_create(const char* name, unsigned flags)
{
    static volatile LONG memfd_counter = 0;
    char tmp[MAX_PATH];
    char path[MAX_PATH + 64];
    HANDLE h;
    int fd;
    (void)name;
    (void)flags;
    if (!GetTempPathA(MAX_PATH, tmp)) {
        errno = ENOSYS;
        return -1;
    }
    snprintf(path, sizeof(path), "%srvvm_memfd_%u_%ld.tmp",
             tmp, (unsigned)GetCurrentProcessId(),
             (long)InterlockedIncrement(&memfd_counter));
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        win_set_errno();
        return -1;
    }
    fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Signals - sigaction() over a Vectored Exception Handler             */
/* ------------------------------------------------------------------ */

static struct sigaction shim_sigact[_NSIG];
static sigset_t shim_sigmask;
static int shim_veh_installed;

static LONG CALLBACK shim_veh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    int sig = 0;
    void* addr = NULL;
    struct sigaction* sa;

    switch (rec->ExceptionCode) {
    case STATUS_ACCESS_VIOLATION:
    case STATUS_GUARD_PAGE_VIOLATION:
        sig = SIGSEGV;
        if (rec->NumberParameters >= 2) {
            addr = (void*)(uintptr_t)rec->ExceptionInformation[1];
        }
        break;
    case STATUS_IN_PAGE_ERROR:
        sig = SIGBUS;
        if (rec->NumberParameters >= 2) {
            addr = (void*)(uintptr_t)rec->ExceptionInformation[1];
        }
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    sa = &shim_sigact[sig];
    if (sa->sa_flags & SA_SIGINFO) {
        siginfo_t info;
        if (!sa->sa_sigaction) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code  = 128;    /* SI_KERNEL */
        info.si_addr  = addr;
        sa->sa_sigaction(sig, &info, ep);
    } else {
        if (!sa->sa_handler) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        sa->sa_handler(sig);
    }
    /* A Linux handler would resume at the faulting instruction here; the
     * VMA fault path in rvvm_user.c longjmps out instead. Otherwise let
     * the exception propagate (crash). */
    return EXCEPTION_CONTINUE_SEARCH;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old)
{
    if (sig < 1 || sig >= _NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (old) {
        *old = shim_sigact[sig];
    }
    if (act) {
        shim_sigact[sig] = *act;
        if (!shim_veh_installed) {
            AddVectoredExceptionHandler(1, shim_veh);
            shim_veh_installed = 1;
        }
    }
    return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    (void)how;
    if (old) {
        *old = shim_sigmask;
    }
    if (set) {
        shim_sigmask = *set;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CPU affinity                                                        */
/* ------------------------------------------------------------------ */

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask)
{
    SYSTEM_INFO si;
    DWORD count, i;
    (void)pid;
    if (!mask || cpusetsize < sizeof(cpu_set_t)) {
        errno = EINVAL;
        return -1;
    }
    GetSystemInfo(&si);
    count = si.dwNumberOfProcessors;
    if (count > CPU_SETSIZE) {
        count = CPU_SETSIZE;
    }
    CPU_ZERO(mask);
    for (i = 0; i < count; i++) {
        CPU_SET(i, mask);
    }
    return (int)sizeof(cpu_set_t);
}
