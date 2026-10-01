/*
rvvm_fs.h - one shape of filesystem operations, as the two providers have them

There are two filesystems in the core and they were built to the same shape on
purpose: rvvm_memfs (bytes in the core) and rvvm_hostfs (bytes in a host
directory). Same operations, same arguments, same meanings, and - the part that
makes this file small - result codes with the same numbers.

This header is that shape, written down once. It is not a Linux VFS: there is no
dentry cache, no inode cache, no superblock, no mount tree. A filesystem here is
asked about a PATH and answers, which is all the two providers do and all the
syscall layer has ever needed from them.

What a slot is allowed to be
----------------------------

Every slot here is one both providers already implement. Nothing is included
because Linux has it, or because a filesystem might want it one day - a table with
slots nobody fills is a table where every implementation has to decide what to do
about a question it cannot answer, and the usual answer is "return success", which
is the worst one. (The core has been bitten twice by exactly that: utimensat(2)
answered success for every path, and mount(2) answered success for a remount that
changed nothing.)

Paths are relative to the mount point, as they are in both providers. Nothing here
knows what a mount point is; the mount table decides which filesystem a guest path
lands on and hands over the rest.

Result codes
------------

rvvm_fs_result_t is this layer's own enum, and its numbers are the same as both
providers' - ENOENT is -1 in all three. They are separate types so that neither
provider has to include this header to be read, and they agree in value so that
the adapters in rvvm_fs.c are a conversion by the compiler rather than by hand.
rvvm_fs.c pins that agreement with compile-time assertions, because it is a claim
about three files that live apart and nothing else would notice if it drifted.
*/

#ifndef RVVM_FS_H
#define RVVM_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/rvvm_hostfs.h"
#include "core/rvvm_memfs.h"

typedef enum {
    RVVM_FS_OK        = 0,
    RVVM_FS_ENOENT    = -1,   /* no such file or directory                        */
    RVVM_FS_EEXIST    = -2,   /* the name is taken (mkdir, link)                   */
    RVVM_FS_ENOTDIR   = -3,   /* a component on the way is not a directory         */
    RVVM_FS_EISDIR    = -4,   /* the name is a directory and had to be a file      */
    RVVM_FS_EINVAL    = -5,   /* a bad name, a bad mode, a bad size                */
    RVVM_FS_ENOSPC    = -6,   /* out of space, or over the mount's size=           */
    RVVM_FS_EROFS     = -7,   /* the mount is read-only                            */
    RVVM_FS_EMFILE    = -8,   /* out of open descriptors                           */
    RVVM_FS_EXDEV     = -9,   /* across a mount point                              */
    RVVM_FS_ENOTEMPTY = -10,  /* rmdir of a directory with something in it         */
    RVVM_FS_ELOOP     = -11,  /* too many symlinks on the way                      */
    RVVM_FS_ENOMEM    = -12,  /* allocation failed                                 */
    RVVM_FS_EPERM     = -13,  /* refused: no privilege, or a rule                  */
    RVVM_FS_ENOTSUP   = -14,  /* this filesystem cannot do this at all             */
} rvvm_fs_result_t;

typedef enum {
    RVVM_FS_DIR = 1,  /* a directory                                             */
    RVVM_FS_REG,      /* a regular file                                          */
    RVVM_FS_LNK,      /* a symlink                                               */
    RVVM_FS_OTHER,    /* anything else: a socket, a device                       */
} rvvm_fs_kind_t;

#define RVVM_FS_PATH_MAX 1024
#define RVVM_FS_EOF      (-100)

/* What a caller gets back about one name. The same fields both providers report:
 * a value, never a pointer into their internals, because neither can promise a
 * pointer outlives the next call that touches the tree. */
typedef struct {
    char     path[RVVM_FS_PATH_MAX]; /* the name this lookup reached it by       */
    uint8_t  kind;    /* rvvm_fs_kind_t                                          */
    uint32_t mode;    /* permission bits only; the type comes from @kind         */
    uint64_t size;    /* bytes, for a regular file                               */
    uint64_t ino;     /* stable, non-zero; synthetic where the host has none     */
    uint32_t nlink;   /* names reaching it                                       */
    int64_t  mtime;   /* seconds                                                 */
} rvvm_fs_info_t;

/* --- the operations ----------------------------------------------------- */

typedef struct {
    /* Which, for a mount row that wants to say. */
    const char* name;

    /* The mount's own read-only flag: MS_RDONLY, and what a remount changes.
     * Consulted by every mutating operation rather than stamped onto anything. */
    bool (*read_only)(void* priv);
    void (*set_read_only)(void* priv, bool read_only);

    /* Names. @rel is absolute within the mount, and normalized by the provider. */
    rvvm_fs_result_t (*stat)(void* priv, const char* rel, bool follow, rvvm_fs_info_t* out);
    rvvm_fs_result_t (*isdir)(void* priv, const char* rel);
    rvvm_fs_result_t (*mkdir)(void* priv, const char* rel, uint32_t mode);
    rvvm_fs_result_t (*create_file)(void* priv, const char* rel, uint32_t mode);
    rvvm_fs_result_t (*unlink)(void* priv, const char* rel, bool dir);
    rvvm_fs_result_t (*rename)(void* priv, const char* from, const char* to);
    rvvm_fs_result_t (*link)(void* priv, const char* from, const char* to);
    rvvm_fs_result_t (*symlink)(void* priv, const char* target, const char* rel);
    rvvm_fs_result_t (*readlink)(void* priv, const char* rel, char* buf, size_t size);
    rvvm_fs_result_t (*truncate)(void* priv, const char* rel, uint64_t size);
    rvvm_fs_result_t (*chmod)(void* priv, const char* rel, uint32_t mode);
    rvvm_fs_result_t (*touch)(void* priv, const char* rel);

    /* Contents, by path. @off is absolute: two readers of one file through two
     * descriptors must not share a cursor by accident. */
    rvvm_fs_result_t (*read)(void* priv, const char* rel, uint64_t off,
                             void* buf, size_t count, size_t* done);
    rvvm_fs_result_t (*write)(void* priv, const char* rel, uint64_t off,
                              const void* buf, size_t count, size_t* done);

    /* Open descriptors. A handle names the file and not the name, so a rename or
     * an unlink between two calls does not change what it answers. Both providers
     * keep no cursor in the handle: @off is explicit here too. */
    rvvm_fs_result_t (*pin)(void* priv, const char* rel, bool writable, uint32_t* out_ino);
    void              (*unpin)(void* priv, uint32_t ino);
    rvvm_fs_result_t (*read_at)(void* priv, uint32_t ino, uint64_t off,
                                void* buf, size_t count, size_t* done);
    rvvm_fs_result_t (*write_at)(void* priv, uint32_t ino, uint64_t off,
                                 const void* buf, size_t count, size_t* done);
    rvvm_fs_result_t (*truncate_ino)(void* priv, uint32_t ino, uint64_t size);
    rvvm_fs_result_t (*stat_ino)(void* priv, uint32_t ino, rvvm_fs_info_t* out);

    /* Listing. One entry per call; @pos counts entries already handed out, so a
     * child removed between two calls is simply not visited. */
    rvvm_fs_result_t (*getdents)(void* priv, const char* rel, uint32_t* pos,
                                 char* out_name, size_t size,
                                 uint8_t* out_kind, uint64_t* out_ino);
    rvvm_fs_result_t (*getdents_ino)(void* priv, uint32_t ino, uint32_t* pos,
                                     char* out_name, size_t size,
                                     uint8_t* out_kind, uint64_t* out_ino);
} rvvm_fs_ops_t;

/* --- a mounted filesystem ----------------------------------------------- */

/* Operations plus what they operate on. Built on the stack at the point of use
 * and not owned: the mount row owns the provider, and a caller that needs the
 * filesystem to outlive a mount takes a reference on the provider itself. */
typedef struct {
    const rvvm_fs_ops_t* ops;
    void*                priv;
} rvvm_fs_t;

/* A view of an existing provider. Two functions and not a constructor: neither
 * allocates, neither takes a reference, and the provider is untouched by being
 * looked at this way. */
rvvm_fs_t rvvm_fs_view_memfs(rvvm_memfs_t* fs);
rvvm_fs_t rvvm_fs_view_hostfs(rvvm_hostfs_t* fs);

static inline bool rvvm_fs_read_only(rvvm_fs_t* fs)
{
    return fs->ops->read_only(fs->priv);
}

static inline void rvvm_fs_set_read_only(rvvm_fs_t* fs, bool read_only)
{
    fs->ops->set_read_only(fs->priv, read_only);
}

static inline rvvm_fs_result_t rvvm_fs_stat(rvvm_fs_t* fs, const char* rel,
                                            bool follow, rvvm_fs_info_t* out)
{
    return fs->ops->stat(fs->priv, rel, follow, out);
}

static inline rvvm_fs_result_t rvvm_fs_isdir(rvvm_fs_t* fs, const char* rel)
{
    return fs->ops->isdir(fs->priv, rel);
}

static inline rvvm_fs_result_t rvvm_fs_mkdir(rvvm_fs_t* fs, const char* rel, uint32_t mode)
{
    return fs->ops->mkdir(fs->priv, rel, mode);
}

static inline rvvm_fs_result_t rvvm_fs_create_file(rvvm_fs_t* fs, const char* rel, uint32_t mode)
{
    return fs->ops->create_file(fs->priv, rel, mode);
}

static inline rvvm_fs_result_t rvvm_fs_unlink(rvvm_fs_t* fs, const char* rel, bool dir)
{
    return fs->ops->unlink(fs->priv, rel, dir);
}

static inline rvvm_fs_result_t rvvm_fs_rename(rvvm_fs_t* fs, const char* from, const char* to)
{
    return fs->ops->rename(fs->priv, from, to);
}

static inline rvvm_fs_result_t rvvm_fs_link(rvvm_fs_t* fs, const char* from, const char* to)
{
    return fs->ops->link(fs->priv, from, to);
}

static inline rvvm_fs_result_t rvvm_fs_symlink(rvvm_fs_t* fs, const char* target, const char* rel)
{
    return fs->ops->symlink(fs->priv, target, rel);
}

static inline rvvm_fs_result_t rvvm_fs_readlink(rvvm_fs_t* fs, const char* rel,
                                                char* buf, size_t size)
{
    return fs->ops->readlink(fs->priv, rel, buf, size);
}

static inline rvvm_fs_result_t rvvm_fs_truncate(rvvm_fs_t* fs, const char* rel, uint64_t size)
{
    return fs->ops->truncate(fs->priv, rel, size);
}

static inline rvvm_fs_result_t rvvm_fs_chmod(rvvm_fs_t* fs, const char* rel, uint32_t mode)
{
    return fs->ops->chmod(fs->priv, rel, mode);
}

static inline rvvm_fs_result_t rvvm_fs_touch(rvvm_fs_t* fs, const char* rel)
{
    return fs->ops->touch(fs->priv, rel);
}

static inline rvvm_fs_result_t rvvm_fs_read(rvvm_fs_t* fs, const char* rel, uint64_t off,
                                            void* buf, size_t count, size_t* done)
{
    return fs->ops->read(fs->priv, rel, off, buf, count, done);
}

static inline rvvm_fs_result_t rvvm_fs_write(rvvm_fs_t* fs, const char* rel, uint64_t off,
                                             const void* buf, size_t count, size_t* done)
{
    return fs->ops->write(fs->priv, rel, off, buf, count, done);
}

static inline rvvm_fs_result_t rvvm_fs_pin(rvvm_fs_t* fs, const char* rel,
                                           bool writable, uint32_t* out_ino)
{
    return fs->ops->pin(fs->priv, rel, writable, out_ino);
}

static inline void rvvm_fs_unpin(rvvm_fs_t* fs, uint32_t ino)
{
    fs->ops->unpin(fs->priv, ino);
}

static inline rvvm_fs_result_t rvvm_fs_read_at(rvvm_fs_t* fs, uint32_t ino, uint64_t off,
                                               void* buf, size_t count, size_t* done)
{
    return fs->ops->read_at(fs->priv, ino, off, buf, count, done);
}

static inline rvvm_fs_result_t rvvm_fs_write_at(rvvm_fs_t* fs, uint32_t ino, uint64_t off,
                                                const void* buf, size_t count, size_t* done)
{
    return fs->ops->write_at(fs->priv, ino, off, buf, count, done);
}

static inline rvvm_fs_result_t rvvm_fs_truncate_ino(rvvm_fs_t* fs, uint32_t ino, uint64_t size)
{
    return fs->ops->truncate_ino(fs->priv, ino, size);
}

static inline rvvm_fs_result_t rvvm_fs_stat_ino(rvvm_fs_t* fs, uint32_t ino, rvvm_fs_info_t* out)
{
    return fs->ops->stat_ino(fs->priv, ino, out);
}

static inline rvvm_fs_result_t rvvm_fs_getdents(rvvm_fs_t* fs, const char* rel, uint32_t* pos,
                                                char* out_name, size_t size,
                                                uint8_t* out_kind, uint64_t* out_ino)
{
    return fs->ops->getdents(fs->priv, rel, pos, out_name, size, out_kind, out_ino);
}

static inline rvvm_fs_result_t rvvm_fs_getdents_ino(rvvm_fs_t* fs, uint32_t ino, uint32_t* pos,
                                                    char* out_name, size_t size,
                                                    uint8_t* out_kind, uint64_t* out_ino)
{
    return fs->ops->getdents_ino(fs->priv, ino, pos, out_name, size, out_kind, out_ino);
}

#endif /* RVVM_FS_H */
