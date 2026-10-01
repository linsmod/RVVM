/*
rvvm_hostfs.h - a filesystem backed by a host directory

See rvvm_hostfs.c's header comment for what this is and why it exists. It is the
peer of rvvm_memfs: the same shape of interface, the other answer to where the
bytes are. Where a memfs keeps them in the core, this module keeps them in a
directory the host owns - which means the tree outlives the run, is visible to
whoever owns that directory, and can hold anything the host can hold.

Paths are RELATIVE to the mount point, exactly as they are for rvvm_memfs:

    rvvm_hostfs_t* fs = rvvm_hostfs_create("/srv/guest/root", false);
    rvvm_hostfs_mkdir(fs, "/sub", 0755);

"/sub" here means the directory sub inside that host directory, wherever the mount
point happens to be. The mount point's own path is the mount table's business, and
a caller that already holds a host path has the wrong thing: it names a place on
this machine, not a name inside a filesystem.

Result codes are this module's own, and they are deliberately the same numbers as
rvvm_memfs's - ENOENT is -1 in both. They are two enums and not one shared type
because neither module should have to include the other's header to be read; they
agree in value so that a future dispatch layer can pass either through without
translating, which is the whole point of building them to one shape.

What this module does NOT do is decide what a host directory cannot. It reports
that instead - see rvvm_hostfs_can_symlink() and rvvm_hostfs_can_link().
*/

#ifndef RVVM_HOSTFS_H
#define RVVM_HOSTFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rvvm_hostfs rvvm_hostfs_t;

/* The same numbers rvvm_memfs uses, for the reason given above. */
typedef enum {
    RVVM_HOSTFS_OK        = 0,
    RVVM_HOSTFS_ENOENT    = -1,   /* no such file or directory                    */
    RVVM_HOSTFS_EEXIST    = -2,   /* the name is taken (mkdir, link)                */
    RVVM_HOSTFS_ENOTDIR   = -3,   /* a component on the way is not a directory     */
    RVVM_HOSTFS_EISDIR    = -4,   /* the name is a directory and had to be a file  */
    RVVM_HOSTFS_EINVAL    = -5,   /* a bad name, a bad mode, a bad size            */
    RVVM_HOSTFS_ENOSPC    = -6,   /* the host refused the space                    */
    RVVM_HOSTFS_EROFS     = -7,   /* the mount is read-only                        */
    RVVM_HOSTFS_EMFILE    = -8,   /* out of open descriptors                       */
    RVVM_HOSTFS_EXDEV     = -9,   /* rename across a mount point                   */
    RVVM_HOSTFS_ENOTEMPTY = -10,  /* rmdir of a directory with something in it     */
    RVVM_HOSTFS_ELOOP     = -11,  /* too many symlinks on the way                  */
    RVVM_HOSTFS_ENOMEM    = -12,  /* allocation failed                             */
    RVVM_HOSTFS_EPERM     = -13,  /* the host refused: no privilege, or a rule     */
    RVVM_HOSTFS_ENOTSUP   = -14,  /* this host cannot hold one of these at all     */
} rvvm_hostfs_result_t;

typedef enum {
    RVVM_HOSTFS_DIR = 1,  /* a directory                                         */
    RVVM_HOSTFS_REG,      /* a regular file                                      */
    RVVM_HOSTFS_LNK,      /* a symlink                                           */
    RVVM_HOSTFS_OTHER,    /* anything else the host has: a socket, a device      */
} rvvm_hostfs_kind_t;

/* Longest path, and longest single component, inside a mount. The same bounds
 * rvvm_memfs uses, and for the same reason: a filesystem that accepted a
 * 4000-character name would be one no other tool can copy out of. The host may
 * have its own shorter limits, and those come back as the host's error. */
#define RVVM_HOSTFS_PATH_MAX 1024
#define RVVM_HOSTFS_NAME_MAX 256

/* Open descriptors this module hands out. A handle is one open host descriptor,
 * so this is a bound on the host descriptors a filesystem holds, not on the files
 * in it - and it is the number EMFILE refers to. */
#define RVVM_HOSTFS_MAX_HANDLES 64

/* What a caller gets back about one name.
 *
 * @mode is the permission bits only; the type is @kind. On a host that does not
 * keep modes (Windows keeps a read-only attribute and little else) the executable
 * bits are synthesized from the file's contents, because a guest whose binaries
 * are not executable is a guest that cannot run anything - the mode a guest sees
 * is a report on this filesystem, not a fact about the host's ACLs.
 *
 * @ino is the host's inode number where the host has one. It is reported, not
 * relied upon: nothing here pins anything by number, because on a host a number
 * is not a reference - see rvvm_hostfs_pin(). */
typedef struct {
    char     path[RVVM_HOSTFS_PATH_MAX]; /* the name this lookup reached it by  */
    uint8_t  kind;    /* rvvm_hostfs_kind_t                                     */
    uint32_t mode;    /* permission bits only; the type comes from @kind        */
    uint64_t size;    /* bytes, for a regular file                              */
    uint64_t ino;     /* the host's, where it has one; 0 otherwise              */
    uint32_t nlink;   /* names reaching it                                      */
    int64_t  mtime;   /* seconds                                                */
} rvvm_hostfs_info_t;

/* --- lifetime ----------------------------------------------------------- */

/* A filesystem whose root is the host directory @root.
 *
 * The directory has to exist: this module does not create it, because whether a
 * mount's backing directory should be made is the caller's question - a bundle
 * installer is allowed to make one, a mount of an existing tree is not allowed to
 * invent it. ENOENT when it is not there, ENOTDIR when it is not a directory.
 *
 * @read_only is MS_RDONLY, enforced here the same way rvvm_memfs enforces it:
 * every mutating operation consults it before touching the host, so a flag that
 * lives on a mount-table row and one that lives here cannot disagree.
 *
 * @root is copied, so the caller's string is theirs to free. */
rvvm_hostfs_t* rvvm_hostfs_create(const char* root, bool read_only);

/* As rvvm_memfs_ref()/rvvm_memfs_free(): a reference taken on the filesystem so
 * it outlives an unmount of the mount point it was reachable from. */
rvvm_hostfs_t* rvvm_hostfs_ref(rvvm_hostfs_t* fs);
void           rvvm_hostfs_free(rvvm_hostfs_t* fs);

/* --- what this host directory can hold ---------------------------------- */

/* Whether a symlink can be created here at all.
 *
 * This is a question about the host, not about the filesystem: on Windows making
 * a real symlink needs a privilege (or Developer Mode) that a normal process does
 * not have, so an otherwise working host directory cannot hold one. That is the
 * fact the shadow layer exists to paper over, and it is why the answer is
 * reported here rather than swallowed: a caller that needs symlinks knows whether
 * it has to provide them some other way, and symlink() below can then say what
 * went wrong instead of returning a bare failure nobody can act on.
 *
 * Probed once, on first use, by actually making a link in the root and reading it
 * back - the only way to know, since the alternative is guessing from the host's
 * name and version. Returns false on a read-only mount, where the probe cannot
 * run and the answer would not be usable anyway. */
bool rvvm_hostfs_can_symlink(rvvm_hostfs_t* fs);

/* Whether a hard link can be created here. True wherever link(2) exists; on
 * Windows it is CreateHardLink, which needs no privilege but does need the two
 * names to be on one volume. Probed the same way. */
bool rvvm_hostfs_can_link(rvvm_hostfs_t* fs);

/* --- properties --------------------------------------------------------- */

bool     rvvm_hostfs_read_only(rvvm_hostfs_t* fs);
void     rvvm_hostfs_set_read_only(rvvm_hostfs_t* fs, bool read_only);

/* The host directory this filesystem is rooted at, for a caller that has to name
 * it to the host - a mount table row reporting a device name, say. Borrowed: it
 * belongs to the filesystem and stops being valid when the last reference goes. */
const char* rvvm_hostfs_root(const rvvm_hostfs_t* fs);

/* --- lookups ------------------------------------------------------------ */

/* The name at @path, or RVVM_HOSTFS_ENOENT. @path is absolute within the mount
 * ("/", "/a/b") and normalized by this module, so a caller may pass an
 * unnormalized one.
 *
 * A path that would leave the mount - "/..", or a ".." that climbs past the root
 * - is EINVAL rather than a walk into the host's parent directories. A filesystem
 * is not allowed to reach outside itself, and refusing is what makes @root a root
 * rather than a prefix.
 *
 * @follow resolves a final symlink: false is lstat(), true is stat(). */
rvvm_hostfs_result_t rvvm_hostfs_stat(rvvm_hostfs_t* fs, const char* path,
                                      bool follow, rvvm_hostfs_info_t* out);

rvvm_hostfs_result_t rvvm_hostfs_isdir(rvvm_hostfs_t* fs, const char* path);

/* --- names -------------------------------------------------------------- */

/* All of these refuse on a read-only mount, before anything is changed. */
rvvm_hostfs_result_t rvvm_hostfs_mkdir(rvvm_hostfs_t* fs, const char* path, uint32_t mode);

/* Create or truncate a regular file, as O_CREAT does. */
rvvm_hostfs_result_t rvvm_hostfs_create_file(rvvm_hostfs_t* fs, const char* path, uint32_t mode);

/* @dir says which: true removes an empty directory, false a regular file or a
 * symlink. */
rvvm_hostfs_result_t rvvm_hostfs_unlink(rvvm_hostfs_t* fs, const char* path, bool dir);

/* Rename within this filesystem. As with rvvm_memfs, whether @to is on another
 * mount is the caller's question and is asked of the mount table before calling
 * here. */
rvvm_hostfs_result_t rvvm_hostfs_rename(rvvm_hostfs_t* fs, const char* from, const char* to);

/* link(2): a second name for @from's inode. ENOTSUP on a host that cannot make
 * them - no privilege is involved, the operation is simply missing - and EPERM
 * when the host refuses for a reason of its own, such as the two names not being
 * on one volume. */
rvvm_hostfs_result_t rvvm_hostfs_link(rvvm_hostfs_t* fs, const char* from, const char* to);

/* symlink(2): @target is stored verbatim and is not resolved here - not against
 * this mount, and not against the host. ENOTSUP when this host directory cannot
 * hold a link at all, which is the Windows case and the answer a caller needs in
 * order to provide them another way. */
rvvm_hostfs_result_t rvvm_hostfs_symlink(rvvm_hostfs_t* fs, const char* target, const char* path);

/* The target of a symlink. ENOENT when the name is not there, EINVAL when it is
 * there and is not a link - which is what readlink(2) says, and is how a caller
 * tells "no such file" from "that is not a link". */
rvvm_hostfs_result_t rvvm_hostfs_readlink(rvvm_hostfs_t* fs, const char* path,
                                          char* buf, size_t size);

rvvm_hostfs_result_t rvvm_hostfs_truncate(rvvm_hostfs_t* fs, const char* path, uint64_t size);

rvvm_hostfs_result_t rvvm_hostfs_chmod(rvvm_hostfs_t* fs, const char* path, uint32_t mode);

/* Give an existing name a fresh mtime, which is what touch(1) asks for. Its most
 * important answer is the negative one - ENOENT for a name that is not there -
 * because touch(1) asks this before it creates anything and only creates when it
 * hears ENOENT; a stub that answered success left touch reporting success with no
 * file behind it. */
rvvm_hostfs_result_t rvvm_hostfs_touch(rvvm_hostfs_t* fs, const char* path);

/* --- reading and writing ------------------------------------------------ */

/* Copy out of / into a regular file. @off is absolute, as it is for rvvm_memfs,
 * so two readers through two descriptors cannot share a cursor by accident. The
 * whole @count is moved or nothing is. */
rvvm_hostfs_result_t rvvm_hostfs_read(rvvm_hostfs_t* fs, const char* path,
                                      uint64_t off, void* buf, size_t count, size_t* done);
rvvm_hostfs_result_t rvvm_hostfs_write(rvvm_hostfs_t* fs, const char* path,
                                       uint64_t off, const void* buf, size_t count, size_t* done);

/* --- open handles ------------------------------------------------------- */

/* Open @path and hand back a handle, which is this module's notion of an inode.
 *
 * It is a descriptor and not a number because on a host a number is not a
 * reference: an inode number stays valid after the last name goes only if
 * something is holding the file open, and the thing that holds it open is this.
 * So a handle survives a rename and - on a POSIX host - an unlink of the name it
 * was opened through, which is Linux's rule for a file unlinked while open.
 *
 * On Windows it does not survive the unlink: a file that is open cannot be
 * deleted there, so the unlink fails instead. That is a difference in what the
 * host offers, not a choice this module made, and it is stated here because it is
 * the kind of thing that presents as a puzzling EPERM much later.
 *
 * @writable asks for write access; a handle opened readable cannot write. */
rvvm_hostfs_result_t rvvm_hostfs_pin(rvvm_hostfs_t* fs, const char* path,
                                     bool writable, uint32_t* out_ino);
void                 rvvm_hostfs_unpin(rvvm_hostfs_t* fs, uint32_t ino);

/* The same read, write, truncate, stat and listing, named by a handle instead of
 * by a path. A handle is a descriptor, so a rename between two calls must not
 * change what it answers. @off is absolute and the handle keeps no cursor: the
 * question of who owns the offset is left where rvvm_memfs already left it. */
rvvm_hostfs_result_t rvvm_hostfs_read_at(rvvm_hostfs_t* fs, uint32_t ino,
                                         uint64_t off, void* buf, size_t count, size_t* done);
rvvm_hostfs_result_t rvvm_hostfs_write_at(rvvm_hostfs_t* fs, uint32_t ino,
                                          uint64_t off, const void* buf, size_t count, size_t* done);
rvvm_hostfs_result_t rvvm_hostfs_truncate_ino(rvvm_hostfs_t* fs, uint32_t ino, uint64_t size);
rvvm_hostfs_result_t rvvm_hostfs_stat_ino(rvvm_hostfs_t* fs, uint32_t ino, rvvm_hostfs_info_t* out);

/* --- listing ------------------------------------------------------------ */

/* One entry per call, as rvvm_memfs_getdents(): @pos is the caller's cursor, in
 * as how many entries have been handed out and out as the new count; start at 0
 * and stop at RVVM_HOSTFS_EOF. A count and not a list position, for the same
 * reason - a child removed between two calls is then simply not visited, rather
 * than a cursor naming a node that is gone.
 *
 * @out_name receives the name only, never a path. */
rvvm_hostfs_result_t rvvm_hostfs_getdents(rvvm_hostfs_t* fs, const char* path,
                                          uint32_t* pos, char* out_name, size_t size,
                                          uint8_t* out_kind, uint64_t* out_ino);
rvvm_hostfs_result_t rvvm_hostfs_getdents_ino(rvvm_hostfs_t* fs, uint32_t ino,
                                              uint32_t* pos, char* out_name, size_t size,
                                              uint8_t* out_kind, uint64_t* out_ino);

/* The end of a walk. Not an error: the caller reports it as a short read. Its own
 * code so it can never be confused with a failure - the alternative, returning 0,
 * is also what "no room in the buffer" would mean. The same value rvvm_memfs
 * uses. */
#define RVVM_HOSTFS_EOF (-100)

#endif /* RVVM_HOSTFS_H */
