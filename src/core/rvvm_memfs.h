/*
rvvm_memfs.h - a memory filesystem the core owns (tmpfs, and an empty root)

See rvvm_memfs.c's header comment for what this is and why it exists. The
interface is deliberately a plain data structure: the core owns the lookups and
the mount table decides which paths land here, and nothing in this module knows
about a guest, a mount table, or Linux errno numbering.

Paths are RELATIVE to the mount point, and that is the one convention a caller
has to know:

    rvvm_memfs_t* fs = rvvm_memfs_create(false, 0);
    rvvm_memfs_mkdir(fs, "/sub", 0755);
    rvvm_memfs_create_file(fs, "/sub/f", 0644);

"/sub/f" here means the file f inside the directory sub, wherever the mount point
happens to be. The mount point's own path is the mount table's business and is
not repeated in every key, which is what lets a mount be renamed, covered, or
unmounted without rewriting what is inside it - and what lets a descriptor opened
before the unmount keep working, since its contents never depended on where it
was reachable from.

Result codes are this module's own (RVVM_MEMFS_ENOENT, not -ENOENT). Translating
them into guest errno numbers is the caller's job, and keeping the two apart is
what lets this file be read without knowing what a guest is.
*/

#ifndef RVVM_MEMFS_H
#define RVVM_MEMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rvvm_memfs rvvm_memfs_t;

/* "No such node" in the uint32 index space, and the same value the index space
 * uses for a missing child or sibling. Distinct from any real index, which is
 * why it can be compared against without a separate "has one" flag. */
#define RVVM_MEMFS_NONE ((uint32_t)0xFFFFFFFFu)

/* The end of a getdents64 walk. Not an error: the caller reports it as a short
 * read, which is what a directory at its last entry is. Its own code so it can
 * never be confused with a failure - the alternative, returning 0, is also what
 * "no room in the buffer" would mean, and the two must not be told apart. */
#define RVVM_MEMFS_EOF (-100)

typedef enum {
    RVVM_MEMFS_OK        = 0,
    RVVM_MEMFS_ENOENT    = -1,   /* no such file or directory                     */
    RVVM_MEMFS_EEXIST    = -2,   /* the name is taken (mkdir, link)                 */
    RVVM_MEMFS_ENOTDIR   = -3,   /* a component on the way is not a directory      */
    RVVM_MEMFS_EISDIR    = -4,   /* the name is a directory and had to be a file   */
    RVVM_MEMFS_EINVAL    = -5,   /* a bad name, a bad mode, a bad size             */
    RVVM_MEMFS_ENOSPC    = -6,   /* out of nodes or names, or over size=           */
    RVVM_MEMFS_EROFS     = -7,   /* the mount is read-only                         */
    RVVM_MEMFS_EMFILE    = -8,   /* out of open descriptors                        */
    RVVM_MEMFS_EXDEV     = -9,   /* rename across a mount point                    */
    RVVM_MEMFS_ENOTEMPTY = -10,  /* rmdir of a directory with something in it      */
    RVVM_MEMFS_ELOOP     = -11,  /* too many symlinks on the way                   */
    RVVM_MEMFS_ENOMEM    = -12,  /* allocation failed                              */
    RVVM_MEMFS_EPERM     = -13,  /* link() of a directory - directories are a tree */
} rvvm_memfs_result_t;

typedef enum {
    RVVM_MEMFS_DIR = 1,   /* a directory                                        */
    RVVM_MEMFS_REG,       /* a regular file: the bytes are here, not on a host */
    RVVM_MEMFS_LNK,       /* a symlink: @target is where it points              */
} rvvm_memfs_kind_t;

/* How deep a path may be, in components. Refused rather than truncated: a guest
 * that builds a 200-component path has a bug, and answering it with the first
 * 32 components would hand it a file it did not name. */
#define RVVM_MEMFS_MAX_SEGS 32

/* Longest path, and longest single component. Both absolute, both with room for
 * the terminator. The component bound is what NAME_MAX means on Linux (255);
 * a tmpfs that accepted a 4000-character name would be a filesystem no other
 * tool can copy out of. */
#define RVVM_MEMFS_PATH_MAX 1024
#define RVVM_MEMFS_NAME_MAX 256

/* Nodes one filesystem holds. A tmpfs in a guest holds what its programs make -
 * a pid file, a socket, a log - and this is four orders of magnitude more than
 * that. The bound is what makes ENOSPC reachable and testable instead of
 * theoretical, and it is also what lets a node be a fixed-size record. */
#define RVVM_MEMFS_MAX_NODES 4096

/* Names one filesystem holds. A node is an inode - one file, however many names
 * reach it - and this bounds the names, which is what makes a hard link cost a
 * name and nothing else. Twice the node bound, so a filesystem that is partly
 * hard links is possible at all: a thousand files with two names each is a
 * thousand nodes and two thousand names. */
#define RVVM_MEMFS_MAX_NAMES (RVVM_MEMFS_MAX_NODES * 2)

/* What a caller gets back about one node. A value, not a pointer: the node lives
 * in an inode array that grows, and a pointer into it would not survive the next
 * rvvm_memfs_create_file() - which is the same reason vp_shadow hands out indices
 * and has callers look up again. */
typedef struct {
    uint32_t index;   /* the inode: stable, and shared by every hard link to it */
    char     path[RVVM_MEMFS_PATH_MAX]; /* the name this lookup reached it by */
    uint8_t  kind;    /* rvvm_memfs_kind_t                                    */
    uint32_t mode;    /* permission bits only; the type comes from @kind       */
    uint64_t size;    /* bytes, for a regular file; 0 otherwise                */
    uint64_t ino;     /* synthetic, stable, non-zero                          */
    uint32_t nlink;   /* names reaching it; a directory is 2 + its subdirs */
    int64_t  mtime;   /* seconds, from the last change that touched the node */
} rvvm_memfs_info_t;

/* --- lifetime ----------------------------------------------------------- */

/* A filesystem with a root directory and nothing else.
 *
 * @read_only is the mount's MS_RDONLY, enforced here rather than at the mount
 * table: this is where the storage is, so this is the only place a read-only
 * flag has anything behind it. A flag recorded on a table row and not checked
 * against the bytes is a flag that lies to a guest which then writes anyway.
 *
 * @size_limit is the tmpfs size= in bytes; 0 means no limit, which is what
 * Linux's default is. Charged against file contents only - a directory and a
 * symlink cost nodes, not bytes, and their node cost is bounded by
 * RVVM_MEMFS_MAX_NODES. That is a deliberate simplification and it is stated
 * here because a guest that fills a tmpfs with a million empty files will be
 * stopped by the node bound rather than by size=, and the two are not the same
 * limit. */
/* 0 when the path map and the node array still agree, non-zero with the reason
 * in @why. Exported so a test can assert the invariant directly rather than
 * inferring it from a lookup that quietly failed; see the implementation for why
 * the two drifting apart produces a wrong answer rather than a crash. */
int rvvm_memfs_selftest_consistency(rvvm_memfs_t* fs, char* why, size_t whysize);

rvvm_memfs_t* rvvm_memfs_create(bool read_only, uint64_t size_limit);

void          rvvm_memfs_free(rvvm_memfs_t* fs);

/* The mount's own properties, for a mount table row to report.
 *
 * read_only and size_limit are const and lock-free because nothing changes them
 * after rvvm_memfs_create(): a read-only mount cannot be made writable and a
 * size= cannot be changed, so there is no state here for a lock to guard. used
 * and count are not, and take a non-const pointer for the ordinary reason that
 * taking a lock writes to it. */
bool     rvvm_memfs_read_only(const rvvm_memfs_t* fs);
uint64_t rvvm_memfs_size_limit(const rvvm_memfs_t* fs);
uint64_t rvvm_memfs_used(rvvm_memfs_t* fs);
uint32_t rvvm_memfs_count(rvvm_memfs_t* fs);

/* --- lookups ------------------------------------------------------------ */

/* The node at @path, or RVVM_MEMFS_ENOENT. @path is absolute within the mount
 * ("/", "/a/b") and normalized by this module, so a caller may pass an
 * unnormalized one - the same rule the guest's own path code follows.
 *
 * @follow resolves a final symlink. A lookup that does not resolve it is lstat();
 * one that does is stat(). The distinction is not cosmetic: /proc/self and every
 * /dev/pts/N is a link whose target has to be what gets stat()ed. */
rvvm_memfs_result_t rvvm_memfs_stat(rvvm_memfs_t* fs, const char* path,
                                    bool follow, rvvm_memfs_info_t* out);

/* Whether @path names a directory, without filling in an info block. */
rvvm_memfs_result_t rvvm_memfs_isdir(rvvm_memfs_t* fs, const char* path);

/* --- names -------------------------------------------------------------- */

/* All of these refuse on a read-only mount, before anything is changed. */
rvvm_memfs_result_t rvvm_memfs_mkdir(rvvm_memfs_t* fs, const char* path, uint32_t mode);

/* Create or truncate a regular file, as O_CREAT does. @mode is masked to 0777
 * plus the set-id bits; the type bits in a mode are not a thing a caller gets
 * to choose here, since the kind is an argument here and not a mode word. */
rvvm_memfs_result_t rvvm_memfs_create_file(rvvm_memfs_t* fs, const char* path, uint32_t mode);

/* @dir says which: true removes an empty directory, false a regular file or a
 * symlink. Getting it wrong is EITHER/ENOTDIR rather than a wrong removal, and
 * the two are told apart because a caller that unlinks a directory by mistake
 * should find out that it did not do it. */
rvvm_memfs_result_t rvvm_memfs_unlink(rvvm_memfs_t* fs, const char* path, bool dir);

/* Rename within this filesystem.
 *
 * This module does not know where a mount point is, so it cannot answer EXDEV
 * itself: whether @to names something on another mount is the caller's question,
 * and rvvm_user.c asks it of the mount table before calling here. Both paths
 * arrive as mount-relative, which is what keeps that question outside this file. */
rvvm_memfs_result_t rvvm_memfs_rename(rvvm_memfs_t* fs, const char* from, const char* to);

/* link(2): give @from's inode a second name, @to. The two share contents, inode
 * number and nlink, and a write through either is seen through both - the one
 * thing a copy would not do and the reason this is not one. @from is not
 * followed: Linux's link() links the symlink itself, not what it points at.
 * EEXIST when @to is taken, EPERM for a directory (a directory with two parents
 * is not a tree). */
rvvm_memfs_result_t rvvm_memfs_link(rvvm_memfs_t* fs, const char* from, const char* to);

rvvm_memfs_result_t rvvm_memfs_symlink(rvvm_memfs_t* fs, const char* target, const char* path);

/* The target of a symlink, into @buf. ENOENT when the name is not there,
 * EINVAL when it is there and is not a link - which is what readlink(2) says,
 * and is the answer a caller that passed the wrong path needs in order to tell
 * "no such file" from "that is not a link". */
rvvm_memfs_result_t rvvm_memfs_readlink(rvvm_memfs_t* fs, const char* path,
                                        char* buf, size_t size);

/* Resize a regular file. Growing zero-fills; shrinking keeps the first @size
 * bytes. ENOSPC when growing past the mount's limit, EROFS on a read-only mount,
 * EINVAL on a directory. */
rvvm_memfs_result_t rvvm_memfs_truncate(rvvm_memfs_t* fs, const char* path, uint64_t size);

rvvm_memfs_result_t rvvm_memfs_chmod(rvvm_memfs_t* fs, const char* path, uint32_t mode);

/* --- reading and writing ------------------------------------------------ */

/* Copy out of a regular file. @off is absolute, so a caller holding a read
 * cursor does not have to track a base as well - and two readers of one file
 * through two descriptors must not share a cursor, which a single stored
 * position per file would make them. */
rvvm_memfs_result_t rvvm_memfs_read(rvvm_memfs_t* fs, const char* path,
                                    uint64_t off, void* buf, size_t count, size_t* done);

/* Write into a regular file, extending it as needed. The whole @count is
 * written or nothing is: a short write here would have to be reported to the
 * guest as a short write, and a guest that retries would corrupt nothing but
 * would be told a lie about how much of its write landed. */
rvvm_memfs_result_t rvvm_memfs_write(rvvm_memfs_t* fs, const char* path,
                                     uint64_t off, const void* buf, size_t count, size_t* done);

/* --- open handles (a descriptor names an inode, not a name) ------------- */

/* Take a reference on @ino so that removing its last name does not take it away
 * while a descriptor is still reading it - Linux's rule for a file unlinked
 * while open. Every open of a descriptor pins once and every close unpins once;
 * the inode is freed when its last name AND its last pin are both gone. A caller
 * that pins must unpin exactly once, or the inode is a leak the consistency
 * check reports.
 *
 * The index to pin is the @index rvvm_memfs_info_t reports, which is also the
 * number every hard link to the file reports - and that is what lets a
 * descriptor survive a rename or an unlink of the name it was opened through. */
rvvm_memfs_result_t rvvm_memfs_pin(rvvm_memfs_t* fs, uint32_t ino);
void                rvvm_memfs_unpin(rvvm_memfs_t* fs, uint32_t ino);

/* The same read, write and truncate the path-keyed calls above do, named by the
 * inode a descriptor already resolved. A descriptor is an inode, so a rename or
 * an unlink between two calls must not change what it answers. */
rvvm_memfs_result_t rvvm_memfs_read_at(rvvm_memfs_t* fs, uint32_t ino,
                                       uint64_t off, void* buf, size_t count, size_t* done);
rvvm_memfs_result_t rvvm_memfs_write_at(rvvm_memfs_t* fs, uint32_t ino,
                                        uint64_t off, const void* buf, size_t count, size_t* done);
rvvm_memfs_result_t rvvm_memfs_truncate_ino(rvvm_memfs_t* fs, uint32_t ino, uint64_t size);

/* What fstat() reports for a descriptor. @out->path is left empty on purpose: an
 * inode reached by number has no one name. */
rvvm_memfs_result_t rvvm_memfs_stat_ino(rvvm_memfs_t* fs, uint32_t ino, rvvm_memfs_info_t* out);

/* The listing of a directory named by inode, for a readdir descriptor. Same
 * contract as rvvm_memfs_getdents(): one entry per call, @pos the caller's
 * cursor, RVVM_MEMFS_EOF at the end. */
rvvm_memfs_result_t rvvm_memfs_getdents_ino(rvvm_memfs_t* fs, uint32_t ino,
                                            uint32_t* pos, char* out_name, size_t size,
                                            uint8_t* out_kind, uint64_t* out_ino);

/* --- listing ------------------------------------------------------------ */

/* One entry per call, so the guest's readdir loop drives it and nothing has to
 * be remembered between calls on this side.
 *
 * @pos is the caller's cursor: in on entry, how many entries have been handed
 * out; out on RVVM_MEMFS_OK, the new count. Start it at 0 and stop at
 * RVVM_MEMFS_EOF.
 *
 * A count of entries already given out, not a position in the child list. That
 * costs a walk from the first child on every call and buys a cursor that cannot
 * be invalidated: a child removed between two calls is then simply not visited.
 * A list-position cursor would name a node that is gone, and repairing it means
 * noticing that the index it held has since been reused - which puts a file in
 * a directory it was never created in, partway through a listing.
 *
 * Neither choice promises anything about entries created during the walk. A
 * directory modified while it is being read is the same undefined thing it is on
 * Linux, and no snapshot is taken to pretend otherwise.
 *
 * @out_name receives the name only, never a path: a directory's children are
 * named, not addressed, and handing back full paths would invite a caller to
 * treat the name as one. */
rvvm_memfs_result_t rvvm_memfs_getdents(rvvm_memfs_t* fs, const char* path,
                                        uint32_t* pos, char* out_name, size_t size,
                                        uint8_t* out_kind, uint64_t* out_ino);

#endif /* RVVM_MEMFS_H */
