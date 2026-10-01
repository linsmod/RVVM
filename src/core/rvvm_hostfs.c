/*
rvvm_hostfs.c - a filesystem backed by a host directory

See rvvm_hostfs.h for the interface. This is the other half: what this is, why it
is a module at all now, and what it deliberately does not do.

Why "the host" needs a module

There was no hostfs to abstract: it was the else branch of every syscall. A path
that no other provider claimed was mapped to a host path and handed to openat(),
stat(), unlinkat() and the rest, right there in the case, with the errno mapped
inline and whatever else that syscall needed done around it. That code was not
wrong and it was not scattered - it had nowhere else to be, because "the ops are
the host's libc" is not a module, it is the absence of one.

What changed is that there is now a second provider (rvvm_memfs), and a syscall
that serves two providers by asking which one it is, twice, in every case, is a
syscall that has started to describe a filesystem without having one. hostfs is
the first half of that description: the same shape of operations, over bytes the
core does not own.

What is different here, and why it is not a detail

  - the tree outlives the run. A file made by a guest is on the host's disk after
    the run ends, which is the point (it is how a persisted rootfs works) and also
    the hazard: this module has no business assuming it owns what is in there.
  - the host decides what a name can hold. A memfs holds a symlink because it says
    so; this module can only ask the host, and on Windows the answer is often no -
    creating a real symlink needs a privilege a normal process does not have. That
    is why the capability is reported (rvvm_hostfs_can_symlink()) rather than
    swallowed: a caller that needs links has to know whether it must provide them
    some other way, and the shadow layer is that other way.
  - the host's mode bits may not exist. Windows keeps a read-only attribute and
    little else, so the executable bits a guest sees are synthesized by the host's
    own stat shim. This module reports what it is given and does not invent modes
    of its own, because two places guessing would disagree.
  - a file cannot be deleted while it is open, on Windows. POSIX lets an unlinked
    file live until its last descriptor closes; there, the unlink fails. That is a
    difference in what the host offers, not a choice made here, and it is written
    down because it otherwise presents as a puzzling EPERM much later.

Deliberate limits, so a reader does not expect more:

  - no size= and no used count. A host directory is not this module's to bound, and
    counting what is in one means walking it. A mount row reporting this says no
    size at all, which is the honest answer.
  - one lock, held only while this module's own state is touched - the flags, the
    handle table. The host calls happen outside it, because holding a lock across
    a blocking write would serialize the whole guest behind one file. A remount
    racing a write is then possible, exactly as on Linux.
  - listing re-opens the directory and skips @pos entries, the way rvvm_memfs does,
    and a handle remembers the name it was opened by so that listing can be done
    without fdopendir, which this host does not have. A directory renamed between
    the open and the listing is therefore ENOENT rather than a listing of the moved
    directory. Reads through the handle are unaffected.
  - no atime fidelity: touch() sets both times to now, which is what touch(1) is
    for and no more.
*/

#include "core/rvvm_hostfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include <util/spinlock.h>

/* AT_FDCWD is the host's "no directory, use the path as given". The *at() calls
 * this module makes are all of that kind - the paths it holds are absolute host
 * paths - so the value is only ever passed through. Linux's number, and the same
 * one the Windows shim uses. */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

/* MinGW's headers have no S_IFLNK at all - a host where a symlink is not a thing
 * the CRT knows about. The value is Linux's, which is what the Windows stat shim
 * puts in st_mode for a link, so the two agree; without this a link would be
 * reported as an ordinary file the moment the host was asked. */
#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0xF000) == S_IFLNK)
#endif

#if defined(_WIN32)
/* No link(2) on this host, and no declaration of the equivalent either at the
 * WINVER this tree builds with. It is in kernel32 regardless: stdcall, because
 * that is what kernel32 exports, and the A variant because the paths here are
 * bytes. */
extern int __stdcall CreateHardLinkA(const char* link, const char* existing, void* reserved);
#endif

/* Whether a symlink can be made at all is decided at compile time on Windows, and
 * the decision is "not here, not yet".
 *
 * The Windows build has symlink()/readlink()/lstat() - src/win/posix_shim.c
 * implements them - but that file is the whole platform: it reaches sockets,
 * logging, the allocator and the vector, so a module that could only be tested by
 * linking all of it is not independently testable, which is the entire point of
 * this module being a module. The other way to get symlinks here is to call
 * CreateSymbolicLink directly, which is a small amount of Win32 and the obvious
 * thing to add when this is wired into the core.
 *
 * Until one of those is true, HOSTFS_NO_SYMLINK says so: can_symlink() answers
 * false and symlink() answers ENOTSUP. That is not a silent failure - it is the
 * answer a caller needs in order to provide links another way, which is precisely
 * what the existing shadow layer does for a host directory today. Defining
 * RVVM_HOSTFS_HAS_SHIM takes the other path. */
#if defined(_WIN32) && !defined(RVVM_HOSTFS_HAS_SHIM)
#define HOSTFS_NO_SYMLINK 1
#endif

/* Binary, always. This host's open() defaults to text mode, which turns every \n
 * into \r\n on the way out and the pair back into one on the way in - a quiet
 * rewrite of the bytes a caller handed over. A filesystem does not get to do that:
 * a guest's ELF, its tarball and its text file all come back different from how
 * they were written, and the guest has no way to know. Every open() below carries
 * this, including the ones that only read. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* This host's mkdir() takes a path and drops the mode on the floor. */
#if defined(_WIN32)
#include <direct.h>
static int hf_mkdir(const char* path, uint32_t mode)
{
    (void)mode;
    return _mkdir(path);
}
#else
static int hf_mkdir(const char* path, uint32_t mode)
{
    return mkdir(path, (mode_t)mode);
}
#endif

/* stat() and lstat(), as one call. Where a host cannot tell a link from what it
 * points at (Windows without the shim), both are the same question and the answer
 * never mentions links - which is consistent, because that host cannot make one
 * either. */
static int hf_stat_path(const char* path, struct stat* st, bool follow)
{
#if defined(HOSTFS_NO_SYMLINK)
    (void)follow;
    return stat(path, st);
#else
    return follow ? stat(path, st) : lstat(path, st);
#endif
}

/* --- symlinks this module keeps itself ----------------------------------- */

/* Where the host is not an option.
 *
 * A symlink is a name that holds a string, and a host directory can hold that
 * only if the host has them. On Windows making one needs a privilege this process
 * does not have (SeCreateSymbolicLinkPrivilege - asked directly, it is win32 1314,
 * ERROR_PRIVILEGE_NOT_HELD), so there are hosts where the answer is simply no.
 * Refusing is not useful: a guest rootfs without symlinks is a rootfs with no
 * /bin/sh, and the shadow layer exists precisely to provide them another way.
 *
 * So this module keeps the link itself: a regular file whose contents are the
 * magic line below, followed by the target verbatim.
 *
 * A FILE, and not a table beside the tree, because the tree outlives the run and
 * an index of what is in it would have to as well. A sidecar index is a second
 * source of truth that can disagree with the directory it describes - an entry
 * whose file was deleted by something outside this module, a tree copied away
 * without the index beside it. A link that IS a file cannot drift: deleting the
 * file deletes the link, copying the directory copies the links, backing it up
 * backs them up, and nothing has to be locked, rewritten or repaired.
 *
 * Cygwin and MSYS2 do the same thing on a filesystem with no symlinks, so the
 * representation is not invented here.
 *
 * Two consequences, stated rather than left to be discovered:
 *   - something that reads the directory without going through this module - the
 *     host's own tools, or a core path that still maps straight to a host path -
 *     sees a small ordinary file and not a link. That is what "the host cannot
 *     hold one" means; the alternative is having no links at all.
 *   - a real file whose first bytes happen to be the magic line reads back as a
 *     link. The magic is long enough that this does not happen by accident, and
 *     it is the same exposure Cygwin has.
 */
#define HOSTFS_LINK_MAGIC     "!<symlink>\n"
#define HOSTFS_LINK_MAGIC_LEN 11

/* Used by the link representation below, defined further down the file. The link
 * representation sits above the struct it belongs to because it is the answer to a
 * question about the host, and it takes the root as a string for that reason -
 * resolution needs no more of the filesystem than where it is rooted. */
static rvvm_hostfs_result_t hf_read_all(int fd, uint64_t off, void* buf,
                                        size_t count, size_t* done);
static rvvm_hostfs_result_t hf_write_all(int fd, uint64_t off, const void* buf,
                                         size_t count, size_t* done);
static rvvm_hostfs_result_t hf_errno(void);
static rvvm_hostfs_result_t hf_normalize(const char* rel, char* out, size_t size);
static rvvm_hostfs_result_t hf_host_path(rvvm_hostfs_t* fs, const char* rel,
                                         char* out, size_t size);

/* The target of a link this module keeps, or false when @host is not one.
 * @out is terminated; @len, when given, is the target's length without it. */
static bool hf_marker_read(const char* host, char* out, size_t size, size_t* len)
{
    char head[HOSTFS_LINK_MAGIC_LEN];
    bool ok = false;
    int  fd;

    if (size < 2) {
        return false;
    }
    fd = open(host, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return false;
    }
    size_t got = 0;
    /* The whole magic has to have been READ, not just compared. A file shorter
     * than the magic - an empty one, most often - leaves @head holding whatever
     * was in that stack last, which after a nearby call is the magic itself: an
     * empty file then reads back as a link pointing at the previous one's target.
     * The length is the fact; the comparison is only meaningful once it is there. */
    if (hf_read_all(fd, 0, head, sizeof(head), &got) == RVVM_HOSTFS_OK &&
        got == sizeof(head) && !memcmp(head, HOSTFS_LINK_MAGIC, sizeof(head))) {
        /* Truncated rather than refused when the caller's buffer is small, which
         * is what readlink(2) does with a target that will not fit. */
        if (hf_read_all(fd, sizeof(head), out, size - 1, &got) == RVVM_HOSTFS_OK) {
            out[got] = '\0';
            if (len) {
                *len = got;
            }
            ok = true;
        }
    }
    close(fd);
    return ok;
}

static rvvm_hostfs_result_t hf_marker_write(const char* host, const char* target)
{
    int                  fd;
    rvvm_hostfs_result_t rc = RVVM_HOSTFS_OK;
    int                  err = 0;

    fd = open(host, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) {
        return hf_errno();
    }
    if (hf_write_all(fd, 0, HOSTFS_LINK_MAGIC, HOSTFS_LINK_MAGIC_LEN, NULL) != RVVM_HOSTFS_OK) {
        err = errno;
        rc  = hf_errno();
    } else if (!target[0] ||
               hf_write_all(fd, HOSTFS_LINK_MAGIC_LEN, target, strlen(target), NULL) != RVVM_HOSTFS_OK) {
        err = errno;
        rc  = hf_errno();
    }
    close(fd);
    if (rc != RVVM_HOSTFS_OK) {
        errno = err;
    }
    return rc;
}

/* Follow the links on the way to @rel, both kinds - a real one by asking the host,
 * one of ours by reading it - and give back the name that is really being named.
 *
 * @depth is what stops a cycle: two links pointing at each other would otherwise
 * walk until the path ran out of room, and ELOOP is what Linux answers for that.
 * A relative target is resolved against the directory the link sits in, as on
 * Linux, and not against the mount root - a link in /d that says "x" means /d/x. */
static rvvm_hostfs_result_t hf_resolve(const char* root, const char* rel,
                                       char* out, size_t size, int depth)
{
    char         norm[RVVM_HOSTFS_PATH_MAX];
    char         host[RVVM_HOSTFS_PATH_MAX];
    char         target[RVVM_HOSTFS_PATH_MAX];
    struct stat  st;
    rvvm_hostfs_result_t rc;
    int          want;

    rc = hf_normalize(rel, norm, sizeof(norm));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (depth > 8) {
        return RVVM_HOSTFS_ELOOP;
    }
    want = snprintf(host, sizeof(host), "%s%s", root, norm);
    if (want < 0 || (size_t)want >= sizeof(host)) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* The host follows a real symlink here; one of ours is an ordinary file to it,
     * and that is the case handled next. A name that is not there at all stops
     * here with the host's ENOENT. */
    if (hf_stat_path(host, &st, true)) {
        return hf_errno();
    }
    if (hf_marker_read(host, target, sizeof(target), NULL)) {
        char        next[RVVM_HOSTFS_PATH_MAX];
        const char* slash  = strrchr(norm, '/');
        size_t      parent = slash ? (size_t)(slash - norm) : 0;

        if (target[0] == '/') {
            snprintf(next, sizeof(next), "%s", target);
        } else {
            snprintf(next, sizeof(next), "%.*s/%s", (int)parent, norm, target);
        }
        return hf_resolve(root, next, out, size, depth + 1);
    }
    snprintf(out, size, "%s", norm[0] ? norm : "/");
    return RVVM_HOSTFS_OK;
}

/* The name @rel really names, for the operations that follow a link. Not for the
 * ones that act on the link itself - unlink and rename must not. */
static rvvm_hostfs_result_t hf_follow(const char* root, const char* rel,
                                      char* host, size_t size)
{
    char                resolved[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_resolve(root, rel, resolved, sizeof(resolved), 0);
    int                  want;

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* @resolved is mount-relative, and the root is "" for the mount's own root, so
     * this is the same concatenation hf_host_path() does. */
    want = snprintf(host, size, "%s%s", root, resolved);
    if (want < 0 || (size_t)want >= size) {
        return RVVM_HOSTFS_EINVAL;
    }
    return RVVM_HOSTFS_OK;
}

/* A host directory with nothing in it still has two entries, and they are not the
 * filesystem's: a guest listing "." and ".." back would be told about entries
 * that exist in every directory rather than about this one. */
#define HOSTFS_SKIP_DOTS 2

struct rvvm_hostfs {
    char       root[RVVM_HOSTFS_PATH_MAX]; /* the host directory, no trailing / */
    bool       read_only;
    spinlock_t lock;
    uint32_t   refs;

    /* Open descriptors. A handle is an index here and the host fd is the value;
     * -1 means free. The name is kept so that a listing can be done without
     * fdopendir() - see the header comment. */
    int    fd[RVVM_HOSTFS_MAX_HANDLES];
    bool   writable[RVVM_HOSTFS_MAX_HANDLES];
    char   name[RVVM_HOSTFS_MAX_HANDLES][RVVM_HOSTFS_PATH_MAX];

    /* -1 unknown, 0 no, 1 yes. Probed once, on first use; see can_symlink(). */
    int    can_symlink;
    int    can_link;
};

/* --- host errno -> this module's result ---------------------------------- */

/* The host's error, in this module's words. Translated rather than passed through
 * because a caller here is a filesystem dispatch and not a libc: it needs to know
 * that a name is taken, not that a host said 17.
 *
 * An errno this does not recognise becomes EPERM, which is "the host refused" -
 * the one answer that is true of every unmapped failure and that does not claim
 * to know why. */
static rvvm_hostfs_result_t hf_errno(void)
{
    switch (errno) {
        case 0:            return RVVM_HOSTFS_OK;
        case ENOENT:       return RVVM_HOSTFS_ENOENT;
        case EEXIST:       return RVVM_HOSTFS_EEXIST;
        case ENOTDIR:      return RVVM_HOSTFS_ENOTDIR;
        case EISDIR:       return RVVM_HOSTFS_EISDIR;
        case EINVAL:       return RVVM_HOSTFS_EINVAL;
        case ENOSPC:
#ifdef EDQUOT
        case EDQUOT:
#endif
#ifdef EFBIG
        case EFBIG:
#endif
                           return RVVM_HOSTFS_ENOSPC;
#ifdef EROFS
        case EROFS:        return RVVM_HOSTFS_EROFS;
#endif
        case EMFILE:
        case ENFILE:       return RVVM_HOSTFS_EMFILE;
#ifdef EXDEV
        case EXDEV:        return RVVM_HOSTFS_EXDEV;
#endif
#ifdef ENOTEMPTY
        case ENOTEMPTY:    return RVVM_HOSTFS_ENOTEMPTY;
#endif
#ifdef ELOOP
        case ELOOP:        return RVVM_HOSTFS_ELOOP;
#endif
        case ENOMEM:       return RVVM_HOSTFS_ENOMEM;
        case EPERM:
        case EACCES:       return RVVM_HOSTFS_EPERM;
        case ENAMETOOLONG: return RVVM_HOSTFS_EINVAL;
        case EBUSY:        return RVVM_HOSTFS_EPERM;
        default:           return RVVM_HOSTFS_EPERM;
    }
}

/* --- paths --------------------------------------------------------------- */

/* Normalize a mount-relative path: collapse "/" runs and ".", resolve ".."
 * lexically, and refuse one that leaves the mount.
 *
 * Lexical, and deliberately so: ".." is resolved against the spelling of the path
 * and not against what is there. /x/../a means /a whether or not /x exists, which
 * is what the guest's own paths mean and what rvvm_memfs does. A ".." that would
 * climb above the root is EINVAL rather than a walk into the host's parent
 * directories: a filesystem may not reach outside itself, and that refusal is what
 * makes @root a root instead of a prefix.
 *
 * Writes into @out a path with a leading slash and no trailing one; the root is
 * the empty string, which is what makes joining it onto the host root give the
 * root itself rather than the root with a slash after it - and that in turn is
 * what lets a caller tell "this is the mount's own root" by comparing strings. */
static rvvm_hostfs_result_t hf_normalize(const char* rel, char* out, size_t size)
{
    const char* p = rel;
    size_t      n = 0;

    if (!rel || rel[0] != '/' || size < 2) {
        return RVVM_HOSTFS_EINVAL;
    }
    out[0] = '\0';
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t      len   = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0 || (len == 1 && p[0] == '.')) {
            /* A doubled slash, or "." - both name nothing. */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (n == 0) {
                /* Already at the root, so this would leave the mount. */
                return RVVM_HOSTFS_EINVAL;
            }
            size_t i = n;
            while (i > 1 && out[i - 1] != '/') {
                i--;
            }
            n      = (i == 1) ? 0 : i - 1;
            out[n] = '\0';
        } else {
            if (len > RVVM_HOSTFS_NAME_MAX - 1) {
                return RVVM_HOSTFS_EINVAL;
            }
            if (n + 1 + len + 1 > size) {
                return RVVM_HOSTFS_EINVAL;
            }
            out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
            out[n] = '\0';
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return RVVM_HOSTFS_OK;
}

/* The host path for a mount-relative one. The root is prefixed and nothing else
 * is done to it: no realpath, no symlink resolution - the host resolves them when
 * the call is made, which is the only time the answer is true. */
static rvvm_hostfs_result_t hf_host_path(rvvm_hostfs_t* fs, const char* rel,
                                         char* out, size_t size)
{
    char                norm[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_normalize(rel, norm, sizeof(norm));
    int                  want;

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* The root is stored without a trailing slash and @norm is "" for the root
     * itself, so this is a plain concatenation - and "/" + "" is the root. */
    want = snprintf(out, size, "%s%s", fs->root, norm);
    if (want < 0 || (size_t)want >= size) {
        return RVVM_HOSTFS_EINVAL;
    }
    return RVVM_HOSTFS_OK;
}

/* --- stat ---------------------------------------------------------------- */

static void hf_info(const char* rel, const struct stat* st, rvvm_hostfs_info_t* out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", rel);

    if (S_ISDIR(st->st_mode)) {
        out->kind = RVVM_HOSTFS_DIR;
    } else if (S_ISREG(st->st_mode)) {
        out->kind = RVVM_HOSTFS_REG;
    } else if (S_ISLNK(st->st_mode)) {
        out->kind = RVVM_HOSTFS_LNK;
    } else {
        /* A socket, a fifo, a device - something the host has and this module does
         * not model. Reported as itself rather than as a regular file, because a
         * guest that asks is entitled to know that it is not one. */
        out->kind = RVVM_HOSTFS_OTHER;
    }
    out->mode  = (uint32_t)(st->st_mode & 07777);
    out->size  = (uint64_t)st->st_size;
    out->ino   = (uint64_t)st->st_ino;
    out->nlink = (uint32_t)st->st_nlink;
    out->mtime = (int64_t)st->st_mtime;
}

/* --- lifetime ------------------------------------------------------------ */

rvvm_hostfs_t* rvvm_hostfs_create(const char* root, bool read_only)
{
    rvvm_hostfs_t* fs;
    struct stat    st;
    unsigned       i;

    if (!root || !root[0]) {
        return NULL;
    }
    /* The directory has to be there. This module does not make it: whether a
     * mount's backing directory should be created is the caller's question - a
     * bundle installer may, a mount of an existing tree may not invent one. */
    if (stat(root, &st) || !S_ISDIR(st.st_mode)) {
        return NULL;
    }
    if (strlen(root) >= RVVM_HOSTFS_PATH_MAX) {
        return NULL;
    }
    fs = calloc(1, sizeof(*fs));
    if (!fs) {
        return NULL;
    }
    snprintf(fs->root, sizeof(fs->root), "%s", root);
    /* A trailing slash would double up with the leading one of every relative
     * path, and "//bin" is not the same name to every host. */
    {
        size_t n = strlen(fs->root);
        while (n > 1 && (fs->root[n - 1] == '/' || fs->root[n - 1] == '\\')) {
            fs->root[--n] = '\0';
        }
    }
    fs->read_only   = read_only;
    fs->refs        = 1;
    fs->can_symlink = -1;
    fs->can_link    = -1;
    spin_init(&fs->lock);
    for (i = 0; i < RVVM_HOSTFS_MAX_HANDLES; ++i) {
        fs->fd[i] = -1;
    }
    return fs;
}

rvvm_hostfs_t* rvvm_hostfs_ref(rvvm_hostfs_t* fs)
{
    if (fs) {
        spin_lock(&fs->lock);
        fs->refs++;
        spin_unlock(&fs->lock);
    }
    return fs;
}

void rvvm_hostfs_free(rvvm_hostfs_t* fs)
{
    unsigned i;
    bool     last;

    if (!fs) {
        return;
    }
    spin_lock(&fs->lock);
    fs->refs--;
    last = (fs->refs == 0);
    spin_unlock(&fs->lock);
    if (!last) {
        return;
    }
    /* A handle still open at the last reference is a caller's leak, and closing it
     * here is the only thing that can be done with it: the alternative is a
     * descriptor the host keeps for the life of the process. */
    for (i = 0; i < RVVM_HOSTFS_MAX_HANDLES; ++i) {
        if (fs->fd[i] >= 0) {
            close(fs->fd[i]);
        }
    }
    free(fs);
}

const char* rvvm_hostfs_root(const rvvm_hostfs_t* fs)
{
    return fs ? fs->root : NULL;
}

bool rvvm_hostfs_read_only(rvvm_hostfs_t* fs)
{
    bool ro;
    if (!fs) {
        return false;
    }
    spin_lock(&fs->lock);
    ro = fs->read_only;
    spin_unlock(&fs->lock);
    return ro;
}

void rvvm_hostfs_set_read_only(rvvm_hostfs_t* fs, bool read_only)
{
    if (!fs) {
        return;
    }
    spin_lock(&fs->lock);
    fs->read_only = read_only;
    spin_unlock(&fs->lock);
}

/* --- capabilities -------------------------------------------------------- */

/* Try to make one, in the root, and read it back. There is no cheaper honest way:
 * the privilege involved is a property of the process and the volume, not of a
 * name or a version, so asking is the only test. The link is removed again, and a
 * failure to remove it is not reported - the answer was already obtained, and a
 * stray zero-length probe link is not something a caller can act on. */
#if !defined(HOSTFS_NO_SYMLINK)
static bool hf_probe_symlink(rvvm_hostfs_t* fs)
{
    char link[RVVM_HOSTFS_PATH_MAX + 32];
    char buf[64];
    bool ok = false;
    int  want;

    want = snprintf(link, sizeof(link), "%s/.rvvm_hostfs_symlink_probe", fs->root);
    if (want < 0 || (size_t)want >= sizeof(link)) {
        return false;
    }
    if (!symlinkat("probe", AT_FDCWD, link)) {
        ssize_t got = readlinkat(AT_FDCWD, link, buf, sizeof(buf));
        ok = (got == 5 && !memcmp(buf, "probe", 5));
        unlink(link);
    }
    return ok;
}
#endif

static bool hf_probe_link(rvvm_hostfs_t* fs)
{
    char from[RVVM_HOSTFS_PATH_MAX + 32];
    char to[RVVM_HOSTFS_PATH_MAX + 32];
    bool ok = false;
    int  fd;

    if (snprintf(from, sizeof(from), "%s/.rvvm_hostfs_link_probe", fs->root) <= 0 ||
        snprintf(to, sizeof(to), "%s/.rvvm_hostfs_link_probe2", fs->root) <= 0) {
        return false;
    }
    fd = open(from, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (fd >= 0) {
        close(fd);
#if defined(_WIN32)
        /* No link(2) on this host; CreateHardLink is the equivalent and needs no
         * privilege, only that both names are on one volume. */
        ok = !!CreateHardLinkA(to, from, NULL);
#else
        ok = !link(from, to);
#endif
        if (ok) {
            unlink(to);
        }
        unlink(from);
    }
    return ok;
}

/* Both are answers about the host, cached because they are asked on a path a
 * guest can walk repeatedly and each one costs four host calls. A read-only mount
 * answers false without probing: the probe cannot run, and it could not be acted
 * on if it could. */
static bool hf_capability(rvvm_hostfs_t* fs, int* slot, bool (*probe)(rvvm_hostfs_t*))
{
    bool ro;
    bool val;

    if (!fs) {
        return false;
    }
    spin_lock(&fs->lock);
    if (*slot >= 0) {
        val = (*slot == 1);
        spin_unlock(&fs->lock);
        return val;
    }
    ro = fs->read_only;
    spin_unlock(&fs->lock);
    if (ro) {
        return false;
    }
    val = probe(fs);
    spin_lock(&fs->lock);
    *slot = val ? 1 : 0;
    spin_unlock(&fs->lock);
    return val;
}

bool rvvm_hostfs_can_symlink(rvvm_hostfs_t* fs)
{
#if defined(HOSTFS_NO_SYMLINK)
    /* No probe: this host cannot hold one, and pretending to check would cost four
     * host calls to reach an answer that is already known. */
    (void)fs;
    return false;
#else
    return fs ? hf_capability(fs, &fs->can_symlink, hf_probe_symlink) : false;
#endif
}

bool rvvm_hostfs_can_link(rvvm_hostfs_t* fs)
{
    return fs ? hf_capability(fs, &fs->can_link, hf_probe_link) : false;
}

/* --- lookups ------------------------------------------------------------- */

rvvm_hostfs_result_t rvvm_hostfs_stat(rvvm_hostfs_t* fs, const char* path,
                                      bool follow, rvvm_hostfs_info_t* out)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    char                target[RVVM_HOSTFS_PATH_MAX];
    size_t              tlen = 0;
    struct stat         st;
    rvvm_hostfs_result_t rc;

    if (!fs || !out) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* Following is resolution, and resolution is this module's job even where the
     * host has no symlinks: a link kept as a file is an ordinary file to the host,
     * so asking the host to follow it would answer about the file. */
    rc = follow ? hf_follow(fs->root, path, host, sizeof(host))
                : hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (hf_stat_path(host, &st, follow)) {
        return hf_errno();
    }
    /* Asked without following, a link this module keeps has to be reported as
     * one: to the host it is a small ordinary file, and a caller that asked for
     * lstat() is asking precisely what the name is. */
    if (!follow && hf_marker_read(host, target, sizeof(target), &tlen)) {
        hf_info(host, &st, out);
        out->kind = RVVM_HOSTFS_LNK;
        out->size = tlen;
        out->mode = 0777;
    } else {
        hf_info(host, &st, out);
    }
    /* The name a caller recognizes, not the host's spelling of it. */
    if (hf_normalize(path, out->path, sizeof(out->path)) != RVVM_HOSTFS_OK) {
        out->path[0] = '\0';
    }
    if (out->path[0] == '\0') {
        /* The root is "" internally, so that joining it onto the host root gives
         * the host root; to a caller it is "/". */
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_isdir(rvvm_hostfs_t* fs, const char* path)
{
    rvvm_hostfs_info_t   info;
    rvvm_hostfs_result_t rc = rvvm_hostfs_stat(fs, path, true, &info);

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    return (info.kind == RVVM_HOSTFS_DIR) ? RVVM_HOSTFS_OK : RVVM_HOSTFS_ENOTDIR;
}

/* --- names --------------------------------------------------------------- */

/* Every mutating operation asks this first, under the lock, so that a flag turned
 * on by a remount is seen before anything reaches the host - and so that the check
 * and the flag it checks cannot be reordered around each other. */
static rvvm_hostfs_result_t hf_writable(rvvm_hostfs_t* fs)
{
    bool ro;
    if (!fs) {
        return RVVM_HOSTFS_EINVAL;
    }
    spin_lock(&fs->lock);
    ro = fs->read_only;
    spin_unlock(&fs->lock);
    return ro ? RVVM_HOSTFS_EROFS : RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_mkdir(rvvm_hostfs_t* fs, const char* path, uint32_t mode)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* The mode is passed to the host and may come back different - a host that has
     * no mode bits cannot be made to keep one. This module does not record what it
     * asked for: the mode a guest sees is whatever the host reports, and a second
     * copy kept here would be the one that drifts. */
    return hf_mkdir(host, mode) ? hf_errno() : RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_create_file(rvvm_hostfs_t* fs, const char* path, uint32_t mode)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    int                  fd;

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    fd = open(host, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, (mode_t)(mode & 0777));
    if (fd < 0) {
        return hf_errno();
    }
    close(fd);
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_unlink(rvvm_hostfs_t* fs, const char* path, bool dir)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* The root cannot be removed, whatever the host would say about its own
     * directory: a filesystem deleting its own root has nothing left to be.
     * Compared as strings, which is why the root normalizes to "" - the host path
     * for it is the root itself, with no slash added. */
    if (!strcmp(host, fs->root)) {
        return RVVM_HOSTFS_EINVAL;
    }
    if (dir ? rmdir(host) : unlink(host)) {
        return hf_errno();
    }
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_rename(rvvm_hostfs_t* fs, const char* from, const char* to)
{
    char                hfrom[RVVM_HOSTFS_PATH_MAX];
    char                hto[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, from, hfrom, sizeof(hfrom));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, to, hto, sizeof(hto));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* Both sides are inside this mount, so this cannot be the EXDEV a rename
     * across mounts is - the caller asks that of the mount table beforehand. */
    return rename(hfrom, hto) ? hf_errno() : RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_link(rvvm_hostfs_t* fs, const char* from, const char* to)
{
    char                hfrom[RVVM_HOSTFS_PATH_MAX];
    char                hto[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (!rvvm_hostfs_can_link(fs)) {
        /* Not "the host refused" but "this host cannot": on Windows there is no
         * link(2) at all, and ENOTSUP is what lets a caller say so to a guest
         * rather than inventing a copy and calling it a link. */
        return RVVM_HOSTFS_ENOTSUP;
    }
    rc = hf_host_path(fs, from, hfrom, sizeof(hfrom));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_host_path(fs, to, hto, sizeof(hto));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
#if defined(_WIN32)
    if (!CreateHardLinkA(hto, hfrom, NULL)) {
        return hf_errno();
    }
#else
    if (link(hfrom, hto)) {
        return hf_errno();
    }
#endif
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_symlink(rvvm_hostfs_t* fs, const char* target, const char* path)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (!target) {
        return RVVM_HOSTFS_EINVAL;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* The target is stored verbatim: a symlink is the string, and resolving it is
     * the lookup's job, not this one's.
     *
     * Where the host can hold a real link, that is what is made - a rootfs built
     * here stays readable by the host's own tools. Where it cannot (no privilege
     * on Windows), this module keeps the link as a file instead, and symlink()
     * succeeds on every host. */
#if !defined(HOSTFS_NO_SYMLINK)
    if (rvvm_hostfs_can_symlink(fs)) {
        return symlinkat(target, AT_FDCWD, host) ? hf_errno() : RVVM_HOSTFS_OK;
    }
#endif
    return hf_marker_write(host, target);
}

rvvm_hostfs_result_t rvvm_hostfs_readlink(rvvm_hostfs_t* fs, const char* path,
                                          char* buf, size_t size)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    struct stat         st;
    rvvm_hostfs_result_t rc;
    ssize_t              got;

    if (!fs || !buf) {
        return RVVM_HOSTFS_EINVAL;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* Asked first, because readlink() on something that is not a link and on
     * something that is not there both fail, and only this tells the two apart. */
    if (hf_stat_path(host, &st, false)) {
        return hf_errno();
    }
    if (size == 0) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* One of ours first: on a host that cannot hold a real link, S_ISLNK is never
     * true and the call below does not exist. */
    if (hf_marker_read(host, buf, size, NULL)) {
        return RVVM_HOSTFS_OK;
    }
    if (!S_ISLNK(st.st_mode)) {
        return RVVM_HOSTFS_EINVAL;
    }
#if defined(HOSTFS_NO_SYMLINK)
    /* Unreachable: nothing on this host can be a link, so the lstat above said so
     * and returned. Spelled out because the alternative is a call that does not
     * exist here. */
    (void)got;
    return RVVM_HOSTFS_EINVAL;
#else
    got = readlinkat(AT_FDCWD, host, buf, size - 1);
    if (got < 0) {
        return hf_errno();
    }
    /* Terminated here and reported without the terminator, which is what
     * readlink(2)'s return value means - a length, not a string. A buffer too
     * small for the target is truncated rather than refused: the alternative is an
     * error that tells a caller nothing about the target it asked for. */
    buf[got] = '\0';
    return RVVM_HOSTFS_OK;
#endif
}

rvvm_hostfs_result_t rvvm_hostfs_truncate(rvvm_hostfs_t* fs, const char* path, uint64_t size)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    int                  fd;

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* open() + ftruncate() rather than truncate(), which this host's CRT declares
     * but does not always implement. Opening for write is also what makes the
     * permission question the same one: a file that cannot be written cannot be
     * resized, and a directory is EISDIR either way. */
    fd = open(host, O_WRONLY | O_BINARY);
    if (fd < 0) {
        return hf_errno();
    }
    if (ftruncate(fd, (off_t)size)) {
        int e = errno;
        close(fd);
        errno = e;
        return hf_errno();
    }
    close(fd);
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_chmod(rvvm_hostfs_t* fs, const char* path, uint32_t mode)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (chmod(host, (mode_t)(mode & 07777))) {
        return hf_errno();
    }
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_touch(rvvm_hostfs_t* fs, const char* path)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* Both times to now, which is what touch(1) means and the whole of what this
     * host is asked to do with a timestamp. utime() with NULL is the one spelling
     * of that both kinds of host have. */
    if (utime(host, NULL)) {
        return hf_errno();
    }
    return RVVM_HOSTFS_OK;
}

/* --- reading and writing ------------------------------------------------- */

/* lseek() rather than pread()/pwrite(), which this host does not have. The
 * descriptor is this call's own, so moving its offset is visible to nobody - and
 * that is the only reason doing it this way is safe. */
static bool hf_seek(int fd, uint64_t off)
{
    return lseek(fd, (off_t)off, SEEK_SET) != (off_t)-1;
}

/* Read @count bytes at @off. Short at EOF, which is correct: a read that reaches
 * the end of a file is not a failure, and @done says how much was there. */
static rvvm_hostfs_result_t hf_read_all(int fd, uint64_t off, void* buf,
                                        size_t count, size_t* done)
{
    size_t moved = 0;

    if (done) {
        *done = 0;
    }
    if (!hf_seek(fd, off)) {
        return hf_errno();
    }
    while (moved < count) {
        ssize_t n = read(fd, (char*)buf + moved, count - moved);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return hf_errno();
        }
        if (n == 0) {
            break;
        }
        moved += (size_t)n;
    }
    if (done) {
        *done = moved;
    }
    return RVVM_HOSTFS_OK;
}

/* Write @count bytes at @off. A whole @count or nothing: a short write would have
 * to be reported to a guest as a short write, and a guest that retried would be
 * told a lie about how much of its write had landed. @buf is const because this
 * does not change the caller's bytes, which is why it is a separate function from
 * the read above rather than one with a flag. */
static rvvm_hostfs_result_t hf_write_all(int fd, uint64_t off, const void* buf,
                                         size_t count, size_t* done)
{
    size_t moved = 0;

    if (done) {
        *done = 0;
    }
    if (!hf_seek(fd, off)) {
        return hf_errno();
    }
    while (moved < count) {
        ssize_t n = write(fd, (const char*)buf + moved, count - moved);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return hf_errno();
        }
        if (n == 0) {
            /* The host took none of it, which is ENOSPC's usual shape. */
            return RVVM_HOSTFS_ENOSPC;
        }
        moved += (size_t)n;
    }
    if (done) {
        *done = moved;
    }
    return RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_read(rvvm_hostfs_t* fs, const char* path,
                                      uint64_t off, void* buf, size_t count, size_t* done)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc;
    int                  fd;

    if (!fs || !buf) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* Follows: reading a link reads what it points at, on either representation. */
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    fd = open(host, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return hf_errno();
    }
    rc = hf_read_all(fd, off, buf, count, done);
    close(fd);
    return rc;
}

rvvm_hostfs_result_t rvvm_hostfs_write(rvvm_hostfs_t* fs, const char* path,
                                       uint64_t off, const void* buf, size_t count, size_t* done)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc = hf_writable(fs);
    int                  fd;

    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    if (!buf) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* Follows, as a write through a link does. */
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    /* Writable, not created: a write to a name that is not there is ENOENT, as it
     * is in a filesystem where creating and writing are two operations. */
    fd = open(host, O_WRONLY | O_BINARY);
    if (fd < 0) {
        return hf_errno();
    }
    rc = hf_write_all(fd, off, buf, count, done);
    close(fd);
    return rc;
}

/* --- handles ------------------------------------------------------------- */

rvvm_hostfs_result_t rvvm_hostfs_pin(rvvm_hostfs_t* fs, const char* path,
                                     bool writable, uint32_t* out_ino)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc;
    int                  fd;
    unsigned             i;

    if (!fs || !out_ino) {
        return RVVM_HOSTFS_EINVAL;
    }
    /* Follows: opening a link opens what it points at. The handle then names the
     * file itself, so a rename of the link afterwards does not change what it
     * answers. */
    rc = hf_follow(fs->root, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    fd = open(host, (writable ? O_RDWR : O_RDONLY) | O_BINARY);
    if (fd < 0) {
        return hf_errno();
    }
    spin_lock(&fs->lock);
    for (i = 0; i < RVVM_HOSTFS_MAX_HANDLES; ++i) {
        if (fs->fd[i] < 0) {
            fs->fd[i]       = fd;
            fs->writable[i] = writable;
            snprintf(fs->name[i], sizeof(fs->name[i]), "%s", host);
            break;
        }
    }
    spin_unlock(&fs->lock);
    if (i == RVVM_HOSTFS_MAX_HANDLES) {
        close(fd);
        return RVVM_HOSTFS_EMFILE;
    }
    *out_ino = (uint32_t)i;
    return RVVM_HOSTFS_OK;
}

void rvvm_hostfs_unpin(rvvm_hostfs_t* fs, uint32_t ino)
{
    int fd = -1;

    if (!fs || ino >= RVVM_HOSTFS_MAX_HANDLES) {
        return;
    }
    spin_lock(&fs->lock);
    fd         = fs->fd[ino];
    fs->fd[ino] = -1;
    fs->name[ino][0] = '\0';
    spin_unlock(&fs->lock);
    if (fd >= 0) {
        close(fd);
    }
}

/* The fd behind a handle, or -1. Checked on every call because a handle is a
 * number a caller holds, and a number that was unpinned is not an error this
 * module can report to anybody - it is a bug in the caller. */
static int hf_fd(rvvm_hostfs_t* fs, uint32_t ino, bool writing)
{
    int fd;

    if (!fs || ino >= RVVM_HOSTFS_MAX_HANDLES) {
        return -1;
    }
    spin_lock(&fs->lock);
    fd = fs->fd[ino];
    if (fd >= 0 && writing && !fs->writable[ino]) {
        fd = -1;
        errno = EBADF;
    }
    spin_unlock(&fs->lock);
    return fd;
}

rvvm_hostfs_result_t rvvm_hostfs_read_at(rvvm_hostfs_t* fs, uint32_t ino,
                                         uint64_t off, void* buf, size_t count, size_t* done)
{
    int fd = hf_fd(fs, ino, false);

    if (fd < 0) {
        return hf_errno();
    }
    return hf_read_all(fd, off, buf, count, done);
}

rvvm_hostfs_result_t rvvm_hostfs_write_at(rvvm_hostfs_t* fs, uint32_t ino,
                                          uint64_t off, const void* buf, size_t count, size_t* done)
{
    int fd = hf_fd(fs, ino, true);

    if (fd < 0) {
        return hf_errno();
    }
    return hf_write_all(fd, off, buf, count, done);
}

rvvm_hostfs_result_t rvvm_hostfs_truncate_ino(rvvm_hostfs_t* fs, uint32_t ino, uint64_t size)
{
    int fd;

    if (hf_writable(fs) != RVVM_HOSTFS_OK) {
        return RVVM_HOSTFS_EROFS;
    }
    fd = hf_fd(fs, ino, true);
    if (fd < 0) {
        return hf_errno();
    }
    return ftruncate(fd, (off_t)size) ? hf_errno() : RVVM_HOSTFS_OK;
}

rvvm_hostfs_result_t rvvm_hostfs_stat_ino(rvvm_hostfs_t* fs, uint32_t ino, rvvm_hostfs_info_t* out)
{
    struct stat st;
    int         fd = hf_fd(fs, ino, false);
    char        name[RVVM_HOSTFS_PATH_MAX];

    if (!out) {
        return RVVM_HOSTFS_EINVAL;
    }
    if (fd < 0) {
        return hf_errno();
    }
    if (fstat(fd, &st)) {
        return hf_errno();
    }
    spin_lock(&fs->lock);
    snprintf(name, sizeof(name), "%s", fs->name[ino]);
    spin_unlock(&fs->lock);
    hf_info(name, &st, out);
    return RVVM_HOSTFS_OK;
}

/* --- listing ------------------------------------------------------------- */

/* One entry per call. @pos counts entries already handed out, so the directory is
 * re-opened and the first @pos are skipped: a cursor that cannot be invalidated,
 * since a child removed between two calls is then simply not visited rather than
 * named by a position that has moved. */
static rvvm_hostfs_result_t hf_getdents(const char* host,
                                        uint32_t* pos, char* out_name, size_t size,
                                        uint8_t* out_kind, uint64_t* out_ino)
{
    DIR*           d;
    struct dirent* e;
    uint32_t       seen = 0;

    if (!pos || !out_name || size < 2) {
        return RVVM_HOSTFS_EINVAL;
    }
    d = opendir(host);
    if (!d) {
        return hf_errno();
    }
    while ((e = readdir(d)) != NULL) {
        const char* name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        if (seen++ < *pos) {
            continue;
        }
        /* One past this entry, so the next call resumes here rather than at a
         * number that has since shifted. */
        *pos = seen;
        snprintf(out_name, size, "%s", name);
        if (out_kind || out_ino) {
            /* The host's readdir gives a name and, on a POSIX host, a type; the
             * type is asked for here rather than taken from d_type because this
             * host's dirent has no d_type, and because the same answer has to come
             * back from both. A child removed between the readdir and this stat is
             * ENOENT, and that is correct: it is not there any more. */
            char                child[RVVM_HOSTFS_PATH_MAX];
            char                ltarget[RVVM_HOSTFS_PATH_MAX];
            struct stat         st;
            rvvm_hostfs_result_t rc;
            int                  want = snprintf(child, sizeof(child), "%s/%s", host, name);

            if (want < 0 || (size_t)want >= sizeof(child)) {
                closedir(d);
                return RVVM_HOSTFS_EINVAL;
            }
            if (hf_stat_path(child, &st, false)) {
                closedir(d);
                return hf_errno();
            }
            rc = RVVM_HOSTFS_OK;
            if (out_kind) {
                /* A link this module keeps is a file to the host, so it is asked
                 * for here rather than read off the mode - otherwise a listing
                 * would show an ordinary small file where the guest made a link. */
                if (S_ISREG(st.st_mode) &&
                    hf_marker_read(child, ltarget, sizeof(ltarget), NULL)) {
                    *out_kind = RVVM_HOSTFS_LNK;
                } else {
                    *out_kind = S_ISDIR(st.st_mode) ? RVVM_HOSTFS_DIR
                              : S_ISREG(st.st_mode) ? RVVM_HOSTFS_REG
                              : S_ISLNK(st.st_mode) ? RVVM_HOSTFS_LNK
                                                    : RVVM_HOSTFS_OTHER;
                }
            }
            if (out_ino) {
                *out_ino = (uint64_t)st.st_ino;
            }
            closedir(d);
            return rc;
        }
        closedir(d);
        return RVVM_HOSTFS_OK;
    }
    closedir(d);
    return RVVM_HOSTFS_EOF;
}

rvvm_hostfs_result_t rvvm_hostfs_getdents(rvvm_hostfs_t* fs, const char* path,
                                          uint32_t* pos, char* out_name, size_t size,
                                          uint8_t* out_kind, uint64_t* out_ino)
{
    char                host[RVVM_HOSTFS_PATH_MAX];
    rvvm_hostfs_result_t rc;

    if (!fs) {
        return RVVM_HOSTFS_EINVAL;
    }
    rc = hf_host_path(fs, path, host, sizeof(host));
    if (rc != RVVM_HOSTFS_OK) {
        return rc;
    }
    return hf_getdents(host, pos, out_name, size, out_kind, out_ino);
}

rvvm_hostfs_result_t rvvm_hostfs_getdents_ino(rvvm_hostfs_t* fs, uint32_t ino,
                                              uint32_t* pos, char* out_name, size_t size,
                                              uint8_t* out_kind, uint64_t* out_ino)
{
    char host[RVVM_HOSTFS_PATH_MAX];

    if (!fs || ino >= RVVM_HOSTFS_MAX_HANDLES) {
        return RVVM_HOSTFS_EINVAL;
    }
    spin_lock(&fs->lock);
    snprintf(host, sizeof(host), "%s", fs->name[ino]);
    spin_unlock(&fs->lock);
    if (host[0] == '\0') {
        return RVVM_HOSTFS_ENOENT;
    }
    return hf_getdents(host, pos, out_name, size, out_kind, out_ino);
}
