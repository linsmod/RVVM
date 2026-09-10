/*
 * posix_shim.c - Win32 implementations for the POSIX functions declared
 * in the mingw_compat/ headers. Covers the compile layer for the RVVM
 * core build: everything here either is implemented on top of
 * Win32 (mmap/memory, clocks, file *at() family, ...) or is an explicit
 * ENOSYS stub for semantics Windows cannot provide without a full
 * emulation layer (fork/epoll/eventfd/futex/sockets/... - see README
 * "Known gaps").
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
    return _dup2(oldfd, newfd);
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

DIR* fdopendir(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* *at() family                                                        */
/* ------------------------------------------------------------------ */

int openat(int dirfd, const char* path, int flags, ...)
{
    char full[MAX_PATH + 16];
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (at_path(dirfd, path, full, sizeof(full))) {
        return -1;
    }
    return _open(full, flags | _O_BINARY, mode);
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
    return _unlink(full);
}

int symlinkat(const char* target, int dirfd, const char* linkpath)
{
    (void)target;
    (void)dirfd;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

ssize_t linkat(int fd1, const char* path1, int fd2, const char* path2, int flags)
{
    (void)fd1;
    (void)path1;
    (void)fd2;
    (void)path2;
    (void)flags;
    errno = ENOSYS;
    return -1;
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
    char tmp[MAX_PATH + 1];
    DWORD n = 0;
    size_t i;
    if (at_path(dirfd, path, tmp, sizeof(tmp))) {
        return -1;
    }
    if (!strcmp(path, "/proc/self/exe")) {
        n = GetModuleFileNameA(NULL, tmp, MAX_PATH);
    } else if (!strcmp(path, "/proc/self/cwd")) {
        n = GetCurrentDirectoryA(MAX_PATH, tmp);
    }
    if (!n || n >= sizeof(tmp)) {
        errno = EINVAL;
        return -1;
    }
    tmp[n] = 0;
    for (i = 0; i < n && i < bufsiz; i++) {
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

int rvvm_stat(const char* path, struct rvvm_stat* buf)
{
    struct _stat64 st;
    if (_stat64(path, &st)) {
        return -1;
    }
    stat_fill(buf, &st);
    return 0;
}

int rvvm_lstat(const char* path, struct rvvm_stat* buf)
{
    /* No symlink support: same as stat */
    return rvvm_stat(path, buf);
}

int rvvm_fstat(int fd, struct rvvm_stat* buf)
{
    struct _stat64 st;
    if (_fstat64(fd, &st)) {
        return -1;
    }
    stat_fill(buf, &st);
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

int poll(struct pollfd* fds, nfds_t nfds, int timeout)
{
    nfds_t i;
    if (fds) {
        for (i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
    }
    Sleep(timeout > 0 ? (DWORD)timeout : 1);
    return 0;
}

int select(int nfds, fd_set* rset, fd_set* wset, fd_set* eset, struct timeval* timeout)
{
    (void)nfds;
    if (rset) FD_ZERO(rset);
    if (wset) FD_ZERO(wset);
    if (eset) FD_ZERO(eset);
    Sleep(timeout ? (DWORD)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : 1);
    return 0;
}

int ioctl(int fd, unsigned long req, ...)
{
    (void)fd;
    (void)req;
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
/* Sockets - ENOSYS stubs (see README "Known gaps")                    */
/* ------------------------------------------------------------------ */

int socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    errno = ENOSYS;
    return -1;
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)domain; (void)type; (void)protocol; (void)sv;
    errno = ENOSYS;
    return -1;
}

int bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    (void)fd; (void)addr; (void)len;
    errno = ENOSYS;
    return -1;
}

int listen(int fd, int backlog)
{
    (void)fd; (void)backlog;
    errno = ENOSYS;
    return -1;
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    (void)fd; (void)addr; (void)len;
    errno = ENOSYS;
    return -1;
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    errno = ENOSYS;
    return -1;
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* addr, socklen_t addrlen)
{
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    errno = ENOSYS;
    return -1;
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* addr, socklen_t* addrlen)
{
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    errno = ENOSYS;
    return -1;
}

int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    errno = ENOSYS;
    return -1;
}

int shutdown(int fd, int how)
{
    (void)fd; (void)how;
    errno = ENOSYS;
    return -1;
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags)
{
    (void)fd; (void)msg; (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags)
{
    (void)fd; (void)msg; (void)flags;
    errno = ENOSYS;
    return -1;
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
        errno = ENOSYS;
        return -1;
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
