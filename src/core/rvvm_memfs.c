/*
rvvm_memfs.c - a memory filesystem the core owns (tmpfs, and an empty root)

See rvvm_memfs.h for the interface and what a caller has to know. This header
comment is the other half: what this is, why it exists, and what it deliberately
does not do.

Why a tmpfs has to be real

A mount table row that says "tmpfs" and answers with a host directory is not a
tmpfs, and the gap is not cosmetic. Three things a guest does with a tmpfs all
mean something the host directory cannot:

  - it is private and dies with the run. A host directory outlives the run, so a
    guest that mounts a tmpfs, writes a socket into it, and re-mounts it finds
    yesterday's file there.
  - size= bounds it. There is no bound on a host directory, so a guest that asks
    for 1M and writes 100M is told it had 1M and is not.
  - MS_RDONLY makes it read-only. This is the one that cannot be faked at all: the
    storage has to belong to whoever is meant to enforce it, and a host directory
    belongs to the host. An MS_RDONLY recorded on a table row with nothing behind
    it is a flag that lies to a guest which then writes anyway.

So the storage is here, and the flags are checked against the bytes.

It is also what an empty root is

With a root mount point served by one of these, "this run has no rootfs" stops
being a special case in the path code and becomes an ordinary empty tmpfs at
"/". That is the whole reason the root mount point was allowed to become a real
mount: prefix == NULL used to mean "the guest's paths ARE the host's paths",
which is a hole shaped like a default, and the alternative to filling it was a
flag every host had to remember to set.

Implementation notes:

  - the path -> dentry map is open addressing with linear probing, keyed by the
    FNV-1a hash of the normalized path, exactly as vp_shadow's is. Slots hold
    index + 1 so 0 means empty.
  - a filesystem is two growable arrays: inodes (identity and bytes) and dentries
    (names and the tree). A file has one inode and at least one dentry, and a hard
    link is a second dentry on the same inode - which is the whole reason the two
    are apart. A returned index is invalidated by the next
    rvvm_memfs_create_file(); the interface returns indices and values rather than
    pointers for that reason, and so does vp_shadow.
  - one lock for the whole filesystem. Every operation here is a handful of
    memory operations, and a finer-grained scheme would be more code to get
    wrong than it saves. The mount table's own lock is a different thing and does
    not cover this: it guards the table, which changes on mount(2), and holding
    it across a file write would serialize every writer in the guest behind the
    mount table's shape.
  - allocation failures are reported (RVVM_MEMFS_ENOMEM) rather than aborting,
    following vp_shadow: this runs on a guest's syscall path, where a clean error
    is what a caller can act on and taking the process down is not.

Deliberate limits, so a reader does not expect more:

  - size= is charged against file contents only, and a hard link charges nothing
    extra: it is a second name for bytes that are already counted. A directory
    and a symlink cost a node, not bytes, and node and name cost are bounded by
    RVVM_MEMFS_MAX_NODES / RVVM_MEMFS_MAX_NAMES. A guest that fills a tmpfs with
    a million empty files is stopped by the node bound rather than by size=, and
    those are not the same limit.
  - hard links share an inode, so writing through one name is visible through the
    other and nlink counts the names that reach it. A directory cannot be
    hard-linked; its nlink is Linux's 2 + its subdirectories.
  - an inode can outlive its last name, but only while a descriptor pins it: the
    unlinked-but-open file of POSIX. Nothing else reaches a nameless inode, since
    unlink is the only removal.
  - no mmap, no O_DIRECT, no fsync, no quotas, no atime/mtime fidelity beyond a
    one-second clock. A guest that mmaps a tmpfs file gets the host's answer for
    a path that does not exist there, which is ENOENT - the same as any file this
    does not hold.
  - permissions are recorded and reported but not enforced, which is the same
    standing decision the rest of the core makes (see rvvm_user.c's
    guest_create_mode()): a guest here runs as one user, and an access check that
    always passes is the same as no check for anything this runs.
*/

#include "core/rvvm_memfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <util/spinlock.h>
#include <util/atomics.h>

/* Entries start here and double; the map rehashes at 70% load, where linear
 * probing is still short. 64 is small enough that an empty root costs one page
 * and large enough that a guest's real tmpfs never rehashes. */
#define MEMFS_MIN_CAPACITY 64
#define MEMFS_MIN_MAP     128

/* How many symlinks one lookup follows before giving up (ELOOP). Linux says 40;
 * a guest that hits that has a cycle, and the number only decides how long the
 * walk takes to notice. */
#define MEMFS_MAX_FOLLOW 8

/* An inode: identity and bytes, with no name. A name is a dentry's, which is
 * what makes a hard link expressible at all - two dentries pointing here are two
 * names for one file, and nothing about the data changes when the second name
 * appears. Splitting the two is also what lets nlink be a real count rather than
 * the constant the merged node reported. */
typedef struct {
    uint8_t* data;      /* file contents; owned, NULL unless kind is REG        */
    char*    target;    /* symlink target; owned, NULL unless kind is LNK      */
    size_t   size;
    size_t   capacity;
    uint64_t ino;       /* synthetic, stable, non-zero */
    uint32_t mode;      /* permission bits only; the type comes from @kind      */
    uint32_t nlink;     /* names pointing here, plus a directory's convention  */
    /* Open handles holding this inode alive. A descriptor is an inode, not a
     * name (see rvvm_memfs_pin()), so removing the last name of a file that is
     * still open must not take the bytes away - which is why the free condition
     * is "no names AND no pins" rather than "no names". */
    uint32_t pins;
    int64_t  mtime;
    uint8_t  kind;      /* rvvm_memfs_kind_t                                    */
    /* The inode free-list link, meaningful only while the slot is free. A field
     * of its own rather than a borrowed one: @nlink is a value a guest reads back
     * through stat(), so reusing it as a link would make a free slot's nlink
     * indistinguishable from a live one's. */
    uint32_t free_next;
} memfs_inode_t;

/* A name in a directory: the tree, and the inode it names. @path is the full
 * path kept here rather than rebuilt by walking @parent, so a lookup is one hash
 * of one string instead of a component walk - the price is that renaming a
 * directory rewrites its descendants' paths (see memfs_reindex_locked()), paid
 * only on a directory rename. @inode is the shared identity: several dentries may
 * name it. */
typedef struct {
    char*    path;      /* absolute within the mount: "/", "/a/b"; owned        */
    uint32_t inode;     /* the inode this name refers to                        */
    uint32_t parent;    /* RVVM_MEMFS_NONE for the root                         */
    uint32_t child;     /* first child, RVVM_MEMFS_NONE ends it                 */
    uint32_t next;      /* next sibling, RVVM_MEMFS_NONE ends it               */
} memfs_dentry_t;

struct rvvm_memfs {
    rvvm_lock_t lock;
    bool        read_only;
    uint64_t    size_limit;
    uint64_t    used;
    /* References. One is the mount that owns the storage; an open descriptor
     * takes its own, which is what lets a descriptor outlive the unmount of its
     * mount point - the descriptor names the filesystem, not the place it was
     * reachable from (see rvvm_memfs_ref). Atomic because the unmount and the
     * descriptor are on different threads. */
    uint32_t    refs;
    memfs_dentry_t* dentries;
    uint32_t*   map;      /* open addressing over dentry indices + 1            */
    size_t      map_size; /* power of two                                       */
    /* Slots handed out, ever. NOT the number of live dentries: a removed slot is
     * kept and reused through free_head rather than returned, because returning
     * it would mean the next allocation lands on index @count - which is only
     * the last live slot when nothing has been removed yet. A filesystem that
     * creates and deletes one file at a time would overwrite a live name on the
     * second create, and the map would keep a key pointing at a dentry whose
     * path had just become someone else's: every later lookup of the clobbered
     * name compares a path against a key that no longer matches it, and answers
     * ENOENT for a file that is still listed in its parent's directory. */
    size_t      count;
    size_t      capacity;   /* slots the array has room for, >= @count          */
    uint32_t    free_head; /* first free dentry slot, chained through ->child    */
    size_t      live;      /* dentries in use, which is not @count               */
    /* The inodes, with the same "hand out an index, keep the slot" discipline as
     * the dentries and a free list of their own. Two lists rather than one: an
     * inode can outlive any single name (a hard link) and a name can outlive no
     * inode, so the two lifetimes are independent and sharing a list would tie
     * them together for nothing. */
    memfs_inode_t* inodes;
    size_t      ino_count;    /* inode slots handed out, ever                  */
    size_t      ino_capacity; /* inode slots the array has room for            */
    uint32_t    ino_free;     /* first free inode slot, chained through free_next */
    size_t      ino_live;     /* inodes in use                                 */
    uint32_t    root;         /* the root dentry index                         */
    uint64_t    next_ino;  /* 0 is never handed out: a zero inode would read as
                            * "unset" to a guest that stats two files and
                            * compares, and 0 is what an uninitialized record
                            * would carry. */
};

static uint32_t memfs_hash(const char* str)
{
    /* FNV-1a, the same function vp_shadow uses: one hash for the two path-keyed
     * structures in the core, so a reader comparing them sees one shape. */
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

/* The name of the last component of @path, or "" for the root. */
static const char* memfs_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* The parent of @path, or NULL when @path is the root. The caller owns the
 * returned string. */
static char* memfs_dirname(const char* path)
{
    const char* slash;
    char*       out;
    size_t      len;

    if (!strcmp(path, "/")) {
        return NULL;
    }
    slash = strrchr(path, '/');
    len   = (size_t)(slash - path);
    if (len == 0) {
        /* "/x" -> "/" */
        out = malloc(2);
        if (out) {
            strcpy(out, "/");
        }
        return out;
    }
    out = malloc(len + 1);
    if (out) {
        memcpy(out, path, len);
        out[len] = '\0';
    }
    return out;
}

/* Join a parent directory and a name into "/a/b". @name must be a bare
 * component. The caller owns the result. */
static char* memfs_join(const char* dir, const char* name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    size_t need;
    char*  out;

    if (dlen == 1 && dir[0] == '/') {
        dlen = 0;
    }
    need = dlen + 1 + nlen + 1;
    out  = malloc(need);
    if (!out) {
        return NULL;
    }
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return out;
}

/* Normalize @path into an absolute, collapsed form.
 *
 * Accepts what a guest's path code produces and a few things it should not
 * produce, because refusing "a//b" would be a rule a caller has to know and
 * every caller would eventually get it wrong:
 *
 *   "/a/b"   "/a/b"     absolute, as callers pass it
 *   "a/b"    "/a/b"     relative, made absolute against the mount's root
 *   "/a//b"  "/a/b"     empty components collapse
 *   "/a/./b" "/a/b"     "." is dropped
 *   "/a/../b" "/b"      ".." pops, and at the root it is refused rather than
 *                       clamped - a path that walks above the mount is a path
 *                       outside the namespace, and answering it with the root
 *                       would let "/../etc/passwd" name the mount's /etc
 *   "/a/../.." "/"     same refusal, at the root
 *
 * @out is @size bytes and must be at least RVVM_MEMFS_PATH_MAX.
 * RVVM_MEMFS_EINVAL comes back for a path that is empty after normalization, for
 * one with more than RVVM_MEMFS_MAX_SEGS components, or one that tries to leave
 * the mount; RVVM_MEMFS_ENAMETOOLONG is not used, because a path that does not
 * fit is a path that is not usable and EINVAL is the answer that says so.
 */
static rvvm_memfs_result_t memfs_normalize(char* out, size_t size, const char* path)
{
    /* Where each component's separating "/" sits, so ".." pops by truncating to
     * an offset already computed rather than by re-walking what has been built.
     * Sized to the segment cap, which the loop below enforces before the array
     * could overflow. */
    size_t segs[RVVM_MEMFS_MAX_SEGS];
    size_t nsegs = 0;
    size_t olen  = 0;
    const char* p;

    if (!path) {
        return RVVM_MEMFS_EINVAL;
    }
    p = path;
    /* A leading "/" is implied; a run of them is collapsed with the rest. */
    while (*p == '/') {
        p++;
    }
    while (*p) {
        const char* seg = p;
        size_t      seglen;

        while (*p && *p != '/') {
            p++;
        }
        seglen = (size_t)(p - seg);
        while (*p == '/') {
            p++;
        }
        if (seglen == 0) {
            continue;
        }
        if (seglen >= RVVM_MEMFS_NAME_MAX) {
            return RVVM_MEMFS_EINVAL;
        }
        if (seglen == 1 && seg[0] == '.') {
            continue;
        }
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nsegs == 0) {
                /* Above the root. Refused, not clamped: see the header comment.
                 * This is the one place a guest can ask to leave, and the answer
                 * is that the mount is the edge of the namespace. */
                return RVVM_MEMFS_EINVAL;
            }
            /* Truncate to the "/" that introduces the component being popped, so
             * "/a/b" with a trailing ".." becomes "/a" and not "/a/" - the
             * separator belongs to the component that goes, not the one that
             * stays. */
            olen = segs[--nsegs];
            continue;
        }
        if (nsegs >= RVVM_MEMFS_MAX_SEGS) {
            return RVVM_MEMFS_EINVAL;
        }
        /* Check the room before writing rather than after: an overflowing
         * snprintf would have truncated a name and reported success, and a
         * truncated name is a different file. */
        if (olen + 1 + seglen + 1 > size) {
            return RVVM_MEMFS_EINVAL;
        }
        segs[nsegs++] = olen;
        out[olen++]   = '/';
        memcpy(out + olen, seg, seglen);
        olen += seglen;
    }
    if (nsegs == 0) {
        if (size < 2) {
            return RVVM_MEMFS_EINVAL;
        }
        strcpy(out, "/");
        return RVVM_MEMFS_OK;
    }
    out[olen] = '\0';
    return RVVM_MEMFS_OK;
}

/* --- the path -> dentry map ---------------------------------------------- */

static void memfs_map_insert(rvvm_memfs_t* fs, uint32_t idx)
{
    uint32_t slot = memfs_hash(fs->dentries[idx].path) & (uint32_t)(fs->map_size - 1);
    while (fs->map[slot]) {
        slot = (slot + 1) & (uint32_t)(fs->map_size - 1);
    }
    fs->map[slot] = idx + 1;
}

static void memfs_map_remove(rvvm_memfs_t* fs, const char* path)
{
    uint32_t mask = (uint32_t)(fs->map_size - 1);
    uint32_t slot = memfs_hash(path) & mask;
    uint32_t i;
    uint32_t j;

    while (fs->map[slot]) {
        if (!strcmp(fs->dentries[fs->map[slot] - 1].path, path)) {
            break;
        }
        slot = (slot + 1) & mask;
    }
    if (!fs->map[slot]) {
        return; /* not present: removing a key that is not there changes nothing */
    }
    /* Open addressing has no tombstones, so the hole is closed by moving
     * entries, and the direction of the move is the entire subtlety. Sliding
     * everything after the hole one slot back is the version that looks right
     * and is not: an entry whose home IS its own slot would be moved to home-1,
     * and a probe that starts at home then walks forward never reaches it. The
     * name is in the table, in its parent's child list, and unfindable - and
     * since nothing else has gone wrong, the symptom is a plain ENOENT for a
     * file the guest can still see in a directory listing.
     *
     * So an entry moves only when its home lies OUTSIDE the span being closed,
     * (i, j]. One whose home is inside that span probes through it, so it has to
     * stay put and the hole travels to j instead. That is Knuth's Algorithm R,
     * and it is the only version of "close the hole" that leaves every remaining
     * entry reachable from its own home. */
    fs->map[slot] = 0;
    i = slot;
    j = slot;
    for (;;) {
        uint32_t k;
        uint32_t d;
        /* j advances on its own, not from i: on the branch that leaves an entry
         * in place i deliberately does not move, and recomputing j as i+1 there
         * would examine the same slot forever. */
        j = (j + 1) & mask;
        if (!fs->map[j]) {
            break;
        }
        k = memfs_hash(fs->dentries[fs->map[j] - 1].path) & mask;
        d = (k - i) & mask;
        /* Cyclically, is k strictly after i and at or before j? That is
         * 0 < (k - i) <= (j - i), and the "strictly" is the part that is easy to
         * lose: k == i means this entry's home IS the hole, and its probe starts
         * there and stops on the very next slot, so it has to move down even
         * though it is inside the span. */
        if (d != 0 && d <= ((j - i) & mask)) {
            /* Its home is inside the run being closed but after the hole, so a
             * probe reaching it never crosses the hole and it can stay put. The
             * hole does NOT move here: i still names the one empty slot, and
             * advancing it would point the next move at a slot that is occupied,
             * overwriting a live entry with no way to find it again. */
            continue;
        }
        fs->map[i] = fs->map[j];
        fs->map[j] = 0;
        i = j;
    }
}

static bool memfs_map_grow(rvvm_memfs_t* fs)
{
    size_t newsize = fs->map_size * 2;
    uint32_t* newmap;

    if (newsize > (size_t)RVVM_MEMFS_MAX_NAMES * 4) {
        return false; /* the name bound is the real limit; the map needs no more */
    }
    newmap = calloc(newsize, sizeof(uint32_t));
    if (!newmap) {
        return false;
    }
    free(fs->map);
    fs->map      = newmap;
    fs->map_size = newsize;
    {
        size_t i;
        for (i = 0; i < fs->count; ++i) {
            /* Skip the holes. A free slot's path is NULL, and hashing it is not
             * a slow lookup - it is a crash in the strcmp the insert does when
             * the slot it lands on is occupied. */
            if (fs->dentries[i].path) {
                memfs_map_insert(fs, (uint32_t)i);
            }
        }
    }
    return true;
}

/* RVVM_MEMFS_NONE when there is no such name. Caller holds the lock. */
static uint32_t memfs_find_locked(rvvm_memfs_t* fs, const char* path)
{
    uint32_t slot;

    if (!fs->map || !path) {
        return RVVM_MEMFS_NONE;
    }
    slot = memfs_hash(path) & (uint32_t)(fs->map_size - 1);
    while (fs->map[slot]) {
        uint32_t idx = fs->map[slot] - 1;
        if (!strcmp(fs->dentries[idx].path, path)) {
            return idx;
        }
        slot = (slot + 1) & (uint32_t)(fs->map_size - 1);
    }
    return RVVM_MEMFS_NONE;
}

/* --- inodes and dentries ------------------------------------------------ */

static void memfs_inode_release(memfs_inode_t* in)
{
    free(in->target);
    free(in->data);
    in->target = NULL;
    in->data = NULL;
    in->size = in->capacity = 0;
    /* @kind becomes the liveness marker: 0 is not a valid kind (DIR is 1), so a
     * released slot is one whose kind is 0, and the consistency check can tell a
     * free inode from a live one without a flag of its own. The free list link in
     * ->free_next is planted by memfs_inode_slot_free() after this runs. */
    in->kind = 0;
}

static void memfs_dentry_release(memfs_dentry_t* d)
{
    free(d->path);
    d->path = NULL;
}

/* Hand a dentry slot back for reuse. It must already be out of the map and out
 * of its parent's child list; what this adds is the free-list link and the live
 * count, which is what stops the next allocation from landing on it by
 * arithmetic.
 *
 * The link goes in ->child because every other field of a released dentry is
 * either NULL or meaningless, and a field that is only ever read as a free-list
 * link cannot be confused with the child list it used to hold. @path must be
 * NULL by the time this runs - memfs_dentry_release() leaves it so, and both the
 * map grow and the consistency check read "path == NULL" as "free slot". */
static void memfs_dentry_slot_free(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_dentry_t* d = &fs->dentries[idx];
    d->child  = fs->free_head;
    d->parent = RVVM_MEMFS_NONE;
    d->next   = RVVM_MEMFS_NONE;
    fs->free_head = idx;
    if (fs->live) {
        fs->live--;
    }
}

/* Hand an inode slot back for reuse. The link goes in ->free_next, the one field
 * that exists only for this - see memfs_inode_t for why it is not borrowed. */
static void memfs_inode_slot_free(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_inode_t* in = &fs->inodes[idx];
    in->free_next = fs->ino_free;
    fs->ino_free  = idx;
    if (fs->ino_live) {
        fs->ino_live--;
    }
}

/* Grow a file's buffer to hold at least @need bytes. Caller holds the lock.
 * realloc keeps the contents, which is what every caller here assumes: a grow
 * that zeroed the file would be a very quiet way to lose a guest's data.
 *
 * Doubling from 64 rather than to the exact size, so a guest appending a line
 * at a time reallocates a logarithmic number of times rather than once per
 * write. The over-allocation is not charged against size= - a limit on what a
 * guest may store is not a limit on what the allocator may reserve, and
 * charging it would make a 1M tmpfs refuse a 100-byte file. */
static bool memfs_inode_reserve(memfs_inode_t* in, size_t need)
{
    uint8_t* fresh;
    size_t   cap;

    if (need <= in->capacity) {
        return true;
    }
    cap = in->capacity ? in->capacity : 64;
    while (cap < need) {
        cap *= 2;
    }
    fresh = realloc(in->data, cap);
    if (!fresh) {
        return false;
    }
    in->data     = fresh;
    in->capacity = cap;
    return true;
}

static int64_t memfs_now(void)
{
    /* One second, which is what a tmpfs this size has for mtime granularity. The
     * alternative - host time - would make two runs of the same program produce
     * different numbers and turn mtime into noise. */
    return (int64_t)time(NULL);
}

/* Allocate an inode of @kind. Caller holds the lock.
 *
 * The inode bound counts live inodes, not slots ever handed out: a filesystem
 * that creates and removes the same file ten thousand times has used one inode,
 * and ENOSPC there would be a lie about how full the filesystem is. */
static rvvm_memfs_result_t memfs_inode_new_locked(rvvm_memfs_t* fs, uint8_t kind,
                                                  uint32_t mode, uint32_t* out_inode)
{
    memfs_inode_t* in;
    uint32_t       idx;

    if (fs->ino_live >= RVVM_MEMFS_MAX_NODES) {
        return RVVM_MEMFS_ENOSPC;
    }
    if (fs->ino_free != RVVM_MEMFS_NONE) {
        /* Reuse a hole. Its ->free_next is the free-list link, planted by
         * memfs_inode_slot_free(); everything else in the record is zeroed. */
        idx = fs->ino_free;
        fs->ino_free = fs->inodes[idx].free_next;
    } else {
        if (fs->ino_count >= fs->ino_capacity) {
            size_t         ncap = fs->ino_capacity ? fs->ino_capacity * 2 : MEMFS_MIN_CAPACITY;
            memfs_inode_t* ninodes;
            if (ncap > RVVM_MEMFS_MAX_NODES) {
                ncap = RVVM_MEMFS_MAX_NODES;
            }
            ninodes = realloc(fs->inodes, ncap * sizeof(memfs_inode_t));
            if (!ninodes) {
                return RVVM_MEMFS_ENOMEM;
            }
            fs->inodes       = ninodes;
            fs->ino_capacity = ncap;
        }
        idx = (uint32_t)fs->ino_count++;
    }
    in = &fs->inodes[idx];
    memset(in, 0, sizeof(*in));
    in->kind  = kind;
    in->mode  = mode & 07777;
    in->ino   = ++fs->next_ino;
    /* A file and a symlink start with one name; a directory with the two a
     * guest's stat() of an empty directory expects (its own entry and the one in
     * its parent), and mkdir adds one more for each subdirectory it gains. */
    in->nlink = (kind == RVVM_MEMFS_DIR) ? 2 : 1;
    in->mtime = memfs_now();
    fs->ino_live++;
    *out_inode = idx;
    return RVVM_MEMFS_OK;
}

/* Give @inode a new name @path under @parent, and file it in the map. Caller
 * holds the lock; @parent and @inode must already exist. The name bound is what
 * makes ENOSPC reachable for a filesystem that is all hard links. */
static rvvm_memfs_result_t memfs_dentry_new_locked(rvvm_memfs_t* fs, const char* path,
                                                   uint32_t parent, uint32_t inode,
                                                   uint32_t* out_idx)
{
    memfs_dentry_t* d;
    char*           copy;
    uint32_t        idx;
    bool            grew = false;

    if (fs->live >= RVVM_MEMFS_MAX_NAMES) {
        return RVVM_MEMFS_ENOSPC;
    }
    /* The name first, so a failed allocation of the slot never consumes one the
     * caller would then have to know to hand back. */
    copy = strdup(path);
    if (!copy) {
        return RVVM_MEMFS_ENOMEM;
    }
    if (fs->free_head != RVVM_MEMFS_NONE) {
        /* Reuse a hole. Its ->child is the free-list link, planted by
         * memfs_dentry_slot_free(); everything else in the record is zeroed. */
        idx = fs->free_head;
        fs->free_head = fs->dentries[idx].child;
    } else {
        if (fs->count >= fs->capacity) {
            size_t          ncap = fs->capacity ? fs->capacity * 2 : MEMFS_MIN_CAPACITY;
            memfs_dentry_t* ndentries;
            if (ncap > RVVM_MEMFS_MAX_NAMES) {
                ncap = RVVM_MEMFS_MAX_NAMES;
            }
            ndentries = realloc(fs->dentries, ncap * sizeof(memfs_dentry_t));
            if (!ndentries) {
                free(copy);
                return RVVM_MEMFS_ENOMEM;
            }
            fs->dentries = ndentries;
            fs->capacity = ncap;
        }
        idx = (uint32_t)fs->count++;
    }
    d = &fs->dentries[idx];
    memset(d, 0, sizeof(*d));
    d->path   = copy;
    d->inode  = inode;
    d->parent = parent;
    d->child  = RVVM_MEMFS_NONE;
    d->next   = RVVM_MEMFS_NONE;
    fs->live++;

    /* Keep the map at least 70% full. Measured against live names, because a
     * free slot is not in the map and must not count towards the load. */
    if (!fs->map || (fs->live * 10) >= (fs->map_size * 7)) {
        /* A successful grow has already re-inserted every live name, this one
         * among them - so the insert below must not run. Doing both files this
         * name under two slots with one key, and the two disagree about which
         * one a removal should drop: the survivor is then an entry no probe
         * reaches, because the hole-closing pass moved it out of its own home's
         * run while the other copy was the one that got cleared. A duplicate is
         * invisible from the outside - the file is findable right up until it is
         * removed, and unfindable afterwards. */
        grew = memfs_map_grow(fs);
        if (!grew && !fs->map) {
            memfs_dentry_release(d);
            memfs_dentry_slot_free(fs, idx);
            return RVVM_MEMFS_ENOMEM;
        }
    }
    if (!grew) {
        memfs_map_insert(fs, idx);
    }
    *out_idx = idx;
    return RVVM_MEMFS_OK;
}

/* Drop one reference to @inode_idx. A file or a symlink lives while any name
 * points at it, so this is a count down to zero; a directory is not hard-linked
 * at all - its nlink is the 2+subdirectories convention, not a reference count -
 * so it is released by the one removal that owns its single name. Caller holds
 * the lock. */
static void memfs_inode_unref_locked(rvvm_memfs_t* fs, uint32_t inode_idx)
{
    memfs_inode_t* in = &fs->inodes[inode_idx];

    if (in->kind != RVVM_MEMFS_DIR) {
        if (in->nlink > 0) {
            in->nlink--;
        }
        if (in->nlink != 0) {
            return; /* another name still reaches it */
        }
    }
    if (in->pins != 0) {
        /* An open descriptor still holds it. A nameless inode that is still open
         * is Linux's unlinked-but-open file: it has no path to find it by, it
         * still answers the descriptor, and the last unpin is what frees it. */
        return;
    }
    if (in->kind == RVVM_MEMFS_REG && fs->size_limit && fs->used >= in->size) {
        /* Only ever subtracting what a write charged, so the counter cannot be
         * driven below zero by a sequence of removes. */
        fs->used -= in->size;
    }
    memfs_inode_release(in);
    memfs_inode_slot_free(fs, inode_idx);
}

/* Unlink a dentry from its parent's child list. Caller holds the lock.
 *
 * The parent's own slot holds the first child, and every later child is reached
 * through the previous sibling's ->next, so the two positions are separate and
 * both have to be handled. Taking the walk from d->next instead - which is what a
 * "is it the last child?" shortcut would do - silently misses the case where
 * @idx IS the first child, and a detach that misses leaves the name linked into
 * its old parent while memfs_attach_locked() links it into the new one. One name
 * in two child lists is a cycle, and the next walk over either of them never
 * ends - which is a hang in a rename, with nothing to point at.
 *
 * Leaving @parent and @next as they were found is deliberate: the caller either
 * frees the name or immediately re-attaches it, and a detach that cleared them
 * would have to be undone in the second case. */
static void memfs_detach_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_dentry_t* d = &fs->dentries[idx];
    uint32_t        p = d->parent;
    uint32_t*       slot;

    if (p == RVVM_MEMFS_NONE) {
        return;
    }
    if (fs->dentries[p].child == idx) {
        fs->dentries[p].child = d->next;
        return;
    }
    slot = &fs->dentries[p].child;
    while (*slot != RVVM_MEMFS_NONE) {
        if (fs->dentries[*slot].next == idx) {
            fs->dentries[*slot].next = d->next;
            return;
        }
        slot = &fs->dentries[*slot].next;
    }
}

static void memfs_attach_locked(rvvm_memfs_t* fs, uint32_t parent, uint32_t idx)
{
    memfs_dentry_t* d = &fs->dentries[idx];
    memfs_dentry_t* p = &fs->dentries[parent];

    d->parent = parent;
    d->next   = p->child;
    p->child  = idx;
    fs->inodes[p->inode].mtime = memfs_now();
}

/* Delete @idx and everything under it, and NOTHING else. Caller holds the lock.
 * Recursive on the depth of the tree, which is bounded by RVVM_MEMFS_MAX_SEGS,
 * so this cannot overflow anything.
 *
 * Not the sibling chain, and that is the whole reason this is its own function
 * rather than the tail of a loop: it walks a child list and recurses once per
 * child, so a version that also advanced to each child's next sibling would
 * delete the entire list on the first call. The parent loop would then be
 * holding the index of a name that is already freed, and the corruption that
 * follows is in the allocator rather than in anything a stack trace would point
 * at. One name in, one subtree out; the caller owns where it goes next.
 *
 * The inode is unrefed here, so a name removed is a name gone however many
 * others still reach the same inode (a hard link survives its siblings). A
 * directory's inode goes with this single removal, and its parent's nlink is the
 * caller's to fix - this function does not know whether the caller is emptying a
 * whole subtree (where no surviving parent is involved) or cutting one name off
 * (where one is). */
static void memfs_remove_node_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_dentry_t* d = &fs->dentries[idx];
    uint32_t        inode = d->inode;
    uint32_t        child = d->child;

    while (child != RVVM_MEMFS_NONE) {
        /* Read before the recursive call: it frees @child, and @kid is the
         * only way back to the rest of the list. It stays valid because the
         * array is not compacted on removal - only its count moves, so a freed
         * index is a hole rather than a shift. */
        uint32_t kid = fs->dentries[child].next;
        memfs_remove_node_locked(fs, child);
        child = kid;
    }
    memfs_detach_locked(fs, idx);
    /* The map is keyed by this name's path and looks it up by value against
     * dentries[].path, so the entry has to leave the map BEFORE the string is
     * freed. Copied first because dentry_release() frees the dentry's own
     * pointer, and the copy is what outlives the call. */
    {
        char* key = strdup(d->path);
        if (key) {
            memfs_map_remove(fs, key);
            free(key);
        }
    }
    memfs_dentry_release(d);
    memfs_dentry_slot_free(fs, idx);
    memfs_inode_unref_locked(fs, inode);
}

/* --- resolution --------------------------------------------------------- */

/* Resolve @path to a dentry index, following symlinks when @follow. Caller holds
 * the lock. The index names the *name* that was reached, which is what stat() and
 * readdir() then report; a caller that wants the identity behind it reads
 * dentries[idx].inode.
 *
 * The symlink loop is a string walk rather than a recursive call: a link target
 * can point at another link that points back, and a recursive resolver would
 * need a visited set to notice. Bounding the number of hops and answering ELOOP
 * is the whole of the cycle handling Linux does here too. */
static rvvm_memfs_result_t memfs_resolve_locked(rvvm_memfs_t* fs, const char* path,
                                                bool follow, uint32_t* out_idx)
{
    char           work[RVVM_MEMFS_PATH_MAX];
    char           joined[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    unsigned       hops = 0;

    rc = memfs_normalize(work, sizeof(work), path);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    for (;;) {
        uint32_t idx = memfs_find_locked(fs, work);
        if (idx == RVVM_MEMFS_NONE) {
            return RVVM_MEMFS_ENOENT;
        }
        if (!follow || fs->inodes[fs->dentries[idx].inode].kind != RVVM_MEMFS_LNK) {
            *out_idx = idx;
            return RVVM_MEMFS_OK;
        }
        if (++hops > MEMFS_MAX_FOLLOW) {
            return RVVM_MEMFS_ELOOP;
        }
        {
            /* A link's target is relative to the directory holding the link, and
             * an absolute target is absolute in the mount. Concatenating
             * blindly would turn "b" inside /a into /b, which is a different
             * file - and the difference is what makes a relative link work at
             * all. */
            const char* target = fs->inodes[fs->dentries[idx].inode].target;
            if (!target) {
                return RVVM_MEMFS_EINVAL;
            }
            if (target[0] == '/') {
                rc = memfs_normalize(joined, sizeof(joined), target);
            } else {
                char* dir = memfs_dirname(work);
                char* full;
                if (!dir) {
                    /* The link is the root, so a relative target is the mount's
                     * own root: "/". */
                    rc = memfs_normalize(joined, sizeof(joined), "/");
                } else {
                    full = memfs_join(dir, target);
                    free(dir);
                    if (!full) {
                        return RVVM_MEMFS_ENOMEM;
                    }
                    rc = memfs_normalize(joined, sizeof(joined), full);
                    free(full);
                }
            }
        }
        if (rc != RVVM_MEMFS_OK) {
            return rc;
        }
        memcpy(work, joined, strlen(joined) + 1);
    }
}

/* The parent directory of @path, resolved, as a dentry index (its final
 * component may be a link, which a lookup of the parent has to follow to get
 * somewhere real). Caller holds the lock. */
static rvvm_memfs_result_t memfs_parent_locked(rvvm_memfs_t* fs, const char* path,
                                               uint32_t* out_parent)
{
    char* dir = memfs_dirname(path);
    rvvm_memfs_result_t rc;

    if (!dir) {
        return RVVM_MEMFS_EINVAL; /* the root has no parent to create inside */
    }
    rc = memfs_resolve_locked(fs, dir, true, out_parent);
    free(dir);
    if (rc != RVVM_MEMFS_OK) {
        /* A path whose directory is missing is ENOENT, and a path whose
         * directory is a file is ENOTDIR. resolve() already says which, except
         * for the case where the missing thing is the whole path - which is the
         * same answer, so nothing is added here. */
        return rc;
    }
    if (fs->inodes[fs->dentries[*out_parent].inode].kind != RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_ENOTDIR;
    }
    return RVVM_MEMFS_OK;
}

static void memfs_fill_info(const rvvm_memfs_t* fs, uint32_t dentry_idx, rvvm_memfs_info_t* out)
{
    const memfs_dentry_t* d  = &fs->dentries[dentry_idx];
    const memfs_inode_t*  in = &fs->inodes[d->inode];

    /* The identity is the inode, not the name: two hard links to one file report
     * the same @index and the same @ino, and differ only in @path - which is the
     * name this lookup reached it by. */
    out->index = d->inode;
    memcpy(out->path, d->path, strlen(d->path) + 1);
    out->kind  = in->kind;
    out->mode  = in->mode;
    out->size  = (in->kind == RVVM_MEMFS_REG) ? (uint64_t)in->size : 0;
    out->ino   = in->ino;
    /* The real count: a file is the number of names that reach it, a directory
     * is Linux's 2 + its subdirectories. */
    out->nlink = in->nlink;
    out->mtime = in->mtime;
}

/* --- consistency check -------------------------------------------------- */

/* Walk the map, the dentries and the inodes against each other and report the
 * first disagreement. Returns 0 when they agree.
 *
 * This exists because the three structures are updated by hand in a dozen places
 * and the failure mode when they drift is not a crash but a quiet wrong answer: a
 * lookup that compares a name's path against a key belonging to a different name
 * concludes the file is not there. Every way that can happen - a removal leaving
 * a stale slot, a rename forgetting to re-key the root, a reused slot keeping an
 * old identity, a hard link that forgot to move nlink - produces the same symptom
 * from the outside, and none of them points at the line that caused it. Checking
 * the invariants directly is the difference between a test that says "the map is
 * wrong" and one that says "entry 37 is keyed as /clu/02 but its name says
 * /clu/03".
 *
 * The split added two whole families of invariant to check, so this grew: a name
 * must point at a live inode, an inode's nlink must equal the number of names (or
 * subdirectories) that reach it, and every name's stored path must be the one its
 * parent chain implies - the last of which is also what catches a cycle in the
 * parent links, since a cycle can never reach the root.
 *
 * Caller holds the lock. */
static size_t memfs_check_locked(rvvm_memfs_t* fs, char* why, size_t whysize)
{
    size_t   i;
    size_t   j;
    size_t   occupied = 0;
    uint32_t live_inodes = 0;

    /* --- the map against the names --- */
    for (i = 0; i < fs->count; ++i) {
        if (!fs->dentries[i].path) {
            continue; /* a free slot */
        }
        if (memfs_find_locked(fs, fs->dentries[i].path) != (uint32_t)i) {
            snprintf(why, whysize, "dentry %zu (%s) is not findable by its own path", i,
                     fs->dentries[i].path);
            return i;
        }
    }
    /* Every occupied slot must be reachable by a probe that starts at the
     * element's own home. Checked by walking the run rather than by comparing
     * slot numbers: "is i before or after h" has no answer without a wraparound
     * convention, and getting that convention wrong turns the check into a
     * reporter of ordinary probe collisions. Walking home -> i and finding an
     * empty slot on the way is the same condition a lookup would hit. */
    for (i = 0; i < fs->map_size; ++i) {
        uint32_t idx;
        uint32_t h;
        uint32_t t;
        uint32_t steps = 0;
        if (!fs->map[i]) {
            continue;
        }
        occupied++;
        idx = fs->map[i] - 1;
        if (idx >= fs->count || !fs->dentries[idx].path) {
            snprintf(why, whysize, "slot %zu names dentry %u, which is free or past the end",
                     i, idx);
            return i;
        }
        h = memfs_hash(fs->dentries[idx].path) & (uint32_t)(fs->map_size - 1);
        for (t = h; t != i; t = (t + 1) & (uint32_t)(fs->map_size - 1)) {
            if (!fs->map[t] || ++steps > fs->map_size) {
                snprintf(why, whysize,
                         "slot %zu holds dentry %u (%s), whose home is %u: a gap in between",
                         i, idx, fs->dentries[idx].path, h);
                return i;
            }
        }
    }
    for (i = 0; i < fs->map_size; ++i) {
        if (!fs->map[i]) {
            continue;
        }
        for (j = i + 1; j < fs->map_size; ++j) {
            if (fs->map[j] && fs->map[j] == fs->map[i]) {
                snprintf(why, whysize, "dentry %u is filed in two slots (%zu and %zu)",
                         fs->map[i] - 1, i, j);
                return i;
            }
        }
    }
    if (occupied != fs->live) {
        snprintf(why, whysize, "%zu slots occupied but %zu names live", occupied, fs->live);
        return fs->map_size;
    }

    /* --- the inodes: a live marker, and the count of live ones --- */
    for (i = 0; i < fs->ino_count; ++i) {
        if (fs->inodes[i].kind != 0) {
            live_inodes++;
        }
    }
    if (live_inodes != fs->ino_live) {
        snprintf(why, whysize, "%u inodes marked live but %zu counted", live_inodes,
                 fs->ino_live);
        return fs->ino_count;
    }

    /* --- every name reaches a live inode --- */
    for (i = 0; i < fs->count; ++i) {
        uint32_t inode;
        if (!fs->dentries[i].path) {
            continue;
        }
        inode = fs->dentries[i].inode;
        if (inode >= fs->ino_count || fs->inodes[inode].kind == 0) {
            snprintf(why, whysize, "dentry %zu (%s) names inode %u, which is free or past the end",
                     i, fs->dentries[i].path, inode);
            return i;
        }
    }

    /* --- the root, and every name's parent chain --- */
    if (fs->root >= fs->count || !fs->dentries[fs->root].path ||
        strcmp(fs->dentries[fs->root].path, "/") ||
        fs->dentries[fs->root].parent != RVVM_MEMFS_NONE) {
        snprintf(why, whysize, "the root is not a parentless \"/\"");
        return fs->root;
    }
    for (i = 0; i < fs->count; ++i) {
        uint32_t    parent;
        uint32_t    walk;
        uint32_t    depth = 0;
        char        expect[RVVM_MEMFS_PATH_MAX];
        const char* base;
        const char* dir;
        int         n;

        if (!fs->dentries[i].path || (uint32_t)i == fs->root) {
            continue;
        }
        parent = fs->dentries[i].parent;
        if (parent >= fs->count || !fs->dentries[parent].path) {
            snprintf(why, whysize, "dentry %zu (%s) has no live parent", i, fs->dentries[i].path);
            return i;
        }
        if (fs->inodes[fs->dentries[parent].inode].kind != RVVM_MEMFS_DIR) {
            snprintf(why, whysize, "dentry %zu (%s) sits under something that is not a directory",
                     i, fs->dentries[i].path);
            return i;
        }
        /* The stored path must be exactly the one the parent chain implies. A
         * name whose path disagreed with where it hangs is a name a lookup would
         * find at one place and a readdir would list at another. */
        base = memfs_basename(fs->dentries[i].path);
        dir  = fs->dentries[parent].path;
        n = !strcmp(dir, "/") ? snprintf(expect, sizeof(expect), "/%s", base)
                              : snprintf(expect, sizeof(expect), "%s/%s", dir, base);
        if (n <= 0 || (size_t)n >= sizeof(expect) || strcmp(expect, fs->dentries[i].path)) {
            snprintf(why, whysize, "dentry %zu is %s but its parent chain says %s", i,
                     fs->dentries[i].path, expect);
            return i;
        }
        /* And the chain has to reach the root. A cycle among parent links never
         * does, which is the one shape the path check above cannot see. */
        for (walk = parent; walk != fs->root; walk = fs->dentries[walk].parent) {
            if (walk == RVVM_MEMFS_NONE || walk >= fs->count ||
                !fs->dentries[walk].path || ++depth > fs->count) {
                snprintf(why, whysize, "dentry %zu (%s) does not reach the root", i,
                         fs->dentries[i].path);
                return i;
            }
        }
    }

    /* --- nlink, recomputed rather than trusted --- */
    for (i = 0; i < fs->ino_count; ++i) {
        memfs_inode_t* in = &fs->inodes[i];
        uint32_t       expect;

        if (in->kind == 0) {
            continue;
        }
        if (in->kind == RVVM_MEMFS_DIR) {
            uint32_t self = RVVM_MEMFS_NONE;
            uint32_t subdirs = 0;
            for (j = 0; j < fs->count; ++j) {
                if (fs->dentries[j].path && fs->dentries[j].inode == (uint32_t)i) {
                    if (self != RVVM_MEMFS_NONE) {
                        snprintf(why, whysize, "directory inode %zu has more than one name", i);
                        return j;
                    }
                    self = (uint32_t)j;
                }
            }
            if (self == RVVM_MEMFS_NONE) {
                /* A directory removed while a descriptor still holds it: nameless,
                 * kept alive by its pin, with its nlink frozen at the convention
                 * value there is nothing left to compare it against. Any other
                 * nameless directory is a leak. */
                if (in->pins > 0) {
                    continue;
                }
                snprintf(why, whysize, "live directory inode %zu has no name and no pin", i);
                return i;
            }
            for (j = 0; j < fs->count; ++j) {
                if (fs->dentries[j].path && fs->dentries[j].parent == self &&
                    fs->inodes[fs->dentries[j].inode].kind == RVVM_MEMFS_DIR) {
                    subdirs++;
                }
            }
            expect = 2 + subdirs;
        } else {
            uint32_t names = 0;
            for (j = 0; j < fs->count; ++j) {
                if (fs->dentries[j].path && fs->dentries[j].inode == (uint32_t)i) {
                    names++;
                }
            }
            expect = names;
            /* A file whose last name is gone has to be held open, or unref would
             * have freed it: a live nameless file with no pin is a leak. */
            if (expect == 0 && in->pins == 0) {
                snprintf(why, whysize, "nameless file inode %zu is live with no pin", i);
                return i;
            }
        }
        if (in->nlink != expect) {
            snprintf(why, whysize, "inode %zu (kind %u) reports nlink %u but %u reach it", i,
                     in->kind, in->nlink, expect);
            return i;
        }
    }
    return 0;
}

/* Public wrapper for a test: 0 when the map and the nodes agree. */
int rvvm_memfs_selftest_consistency(rvvm_memfs_t* fs, char* why, size_t whysize)
{
    int bad;
    if (!fs || !why || !whysize) {
        return -1;
    }
    rvvm_lock(&fs->lock);
    bad = memfs_check_locked(fs, why, whysize) != 0;
    rvvm_unlock(&fs->lock);
    return bad;
}

/* --- lifetime ----------------------------------------------------------- */

rvvm_memfs_t* rvvm_memfs_create(bool read_only, uint64_t size_limit)
{
    rvvm_memfs_t* fs = calloc(1, sizeof(*fs));

    if (!fs) {
        return NULL;
    }
    rvvm_lock_init(&fs->lock);
    fs->read_only  = read_only;
    fs->size_limit = size_limit;
    fs->refs       = 1; /* the mount that asked for it holds the first reference */
    fs->next_ino   = 0;
    fs->root       = RVVM_MEMFS_NONE;
    /* Not 0, which is what calloc left: index 0 is the root's dentry and inode 0
     * is the root's inode, so a free list that starts at 0 hands the root itself
     * back out as the next free slot. The filesystem then has two claims on slot
     * 0 and the second one to be written wins. */
    fs->free_head  = RVVM_MEMFS_NONE;
    fs->ino_free   = RVVM_MEMFS_NONE;
    fs->map_size   = MEMFS_MIN_MAP;
    fs->map        = calloc(fs->map_size, sizeof(uint32_t));
    if (!fs->map) {
        free(fs);
        return NULL;
    }
    {
        uint32_t root_inode  = RVVM_MEMFS_NONE;
        uint32_t root_dentry = RVVM_MEMFS_NONE;
        /* The root is created through the same two calls as everything else, so
         * it has the same mode and the same mtime and a directory the guest makes
         * is indistinguishable from the one it started with. Not read-only
         * checked: the root exists whether or not the mount can be written to,
         * which is what makes a read-only mount still have a readable root. */
        if (memfs_inode_new_locked(fs, RVVM_MEMFS_DIR, 0755, &root_inode) != RVVM_MEMFS_OK ||
            memfs_dentry_new_locked(fs, "/", RVVM_MEMFS_NONE, root_inode, &root_dentry) !=
                RVVM_MEMFS_OK) {
            /* Either call may have been the one that failed, so both arrays are
             * freed rather than the one the failing call happened to touch. */
            free(fs->dentries);
            free(fs->inodes);
            free(fs->map);
            free(fs);
            return NULL;
        }
        fs->root = root_dentry;
    }
    return fs;
}

rvvm_memfs_t* rvvm_memfs_ref(rvvm_memfs_t* fs)
{
    if (fs) {
        atomic_add_uint32(&fs->refs, 1);
    }
    return fs;
}

void rvvm_memfs_free(rvvm_memfs_t* fs)
{
    size_t i;

    if (!fs) {
        return;
    }
    /* Drop one reference, and free only on the last. The storage outlives its
     * mount point for as long as a descriptor still names it, which is what the
     * interface promises: an open descriptor keeps working across an unmount,
     * because what it reads never depended on where it was reachable from. */
    if (atomic_sub_uint32(&fs->refs, 1) != 1) {
        return;
    }
    for (i = 0; i < fs->count; ++i) {
        memfs_dentry_release(&fs->dentries[i]);
    }
    /* Through the whole inode array, not only the live part: release() is a
     * no-op on a slot that was already released, so this needs no liveness test
     * and cannot miss one that grew past ino_live. */
    for (i = 0; i < fs->ino_count; ++i) {
        memfs_inode_release(&fs->inodes[i]);
    }
    free(fs->dentries);
    free(fs->inodes);
    free(fs->map);
    free(fs);
}

bool     rvvm_memfs_read_only(const rvvm_memfs_t* fs)   { return fs ? fs->read_only : false; }
uint64_t rvvm_memfs_size_limit(const rvvm_memfs_t* fs) { return fs ? fs->size_limit : 0; }

uint64_t rvvm_memfs_used(rvvm_memfs_t* fs)
{
    uint64_t used;
    if (!fs) {
        return 0;
    }
    rvvm_lock(&fs->lock);
    used = fs->used;
    rvvm_unlock(&fs->lock);
    return used;
}

/* Live inodes, not dentries and not slots ever handed out: a hard link adds a
 * name without adding a node, and a file deleted and recreated used one. */
uint32_t rvvm_memfs_count(rvvm_memfs_t* fs)
{
    uint32_t count;
    if (!fs) {
        return 0;
    }
    rvvm_lock(&fs->lock);
    count = (uint32_t)fs->ino_live;
    rvvm_unlock(&fs->lock);
    return count;
}

/* --- lookups ------------------------------------------------------------ */

rvvm_memfs_result_t rvvm_memfs_stat(rvvm_memfs_t* fs, const char* path,
                                    bool follow, rvvm_memfs_info_t* out)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, follow, &idx);
        if (rc == RVVM_MEMFS_OK && out) {
            memfs_fill_info(fs, idx, out);
        }
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_isdir(rvvm_memfs_t* fs, const char* path)
{
    rvvm_memfs_info_t info;
    rvvm_memfs_result_t rc = rvvm_memfs_stat(fs, path, true, &info);

    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    return (info.kind == RVVM_MEMFS_DIR) ? RVVM_MEMFS_OK : RVVM_MEMFS_ENOTDIR;
}

/* The inode a name refers to. Caller holds the lock. */
static memfs_inode_t* memfs_inode_of(rvvm_memfs_t* fs, uint32_t dentry_idx)
{
    return &fs->inodes[fs->dentries[dentry_idx].inode];
}

/* Free an inode that was allocated but never got its first name - the dentry
 * that was to carry it could not be filed. Both halves are undone here so a
 * caller that only has the inode index does not have to know the pair. Caller
 * holds the lock. */
static void memfs_inode_abandon_locked(rvvm_memfs_t* fs, uint32_t inode_idx)
{
    memfs_inode_release(&fs->inodes[inode_idx]);
    memfs_inode_slot_free(fs, inode_idx);
}

/* Shared prologue for the name-changing operations: normalize, refuse a
 * read-only mount, and find the parent. Everything that would change a name
 * goes through here so the read-only check cannot be forgotten in one of them -
 * it is the one check that has no second line of defence, because the bytes being
 * protected are right here. */
static rvvm_memfs_result_t memfs_prepare_write(rvvm_memfs_t* fs, const char* path,
                                                char* norm, size_t norm_size,
                                                uint32_t* out_parent)
{
    rvvm_memfs_result_t rc;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(norm, norm_size, path);
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    if (!strcmp(norm, "/")) {
        /* Creating or removing the mount's own root is not a name change this
         * can make: the root is what the mount point IS, and a mount with no
         * root is not a filesystem. Linux answers EBUSY for an unlink of a busy
         * mount point, and this is the same situation. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    rc = memfs_parent_locked(fs, norm, out_parent);
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    return RVVM_MEMFS_OK; /* lock still held */
}

rvvm_memfs_result_t rvvm_memfs_mkdir(rvvm_memfs_t* fs, const char* path, uint32_t mode)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    uint32_t            inode;
    uint32_t            idx;
    rvvm_memfs_result_t rc;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    if (memfs_find_locked(fs, norm) != RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EEXIST;
    }
    rc = memfs_inode_new_locked(fs, RVVM_MEMFS_DIR, mode, &inode);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_dentry_new_locked(fs, norm, parent, inode, &idx);
        if (rc != RVVM_MEMFS_OK) {
            memfs_inode_abandon_locked(fs, inode);
        }
    }
    if (rc == RVVM_MEMFS_OK) {
        memfs_attach_locked(fs, parent, idx);
        /* A new subdirectory is one more link to its parent: that is what makes a
         * directory's nlink 2 + its subdirectories rather than a constant. */
        memfs_inode_of(fs, parent)->nlink++;
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_create_file(rvvm_memfs_t* fs, const char* path, uint32_t mode)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    uint32_t            inode;
    uint32_t            idx;
    rvvm_memfs_result_t rc;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    idx = memfs_find_locked(fs, norm);
    if (idx != RVVM_MEMFS_NONE) {
        memfs_inode_t* in = memfs_inode_of(fs, idx);
        if (in->kind == RVVM_MEMFS_DIR) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EISDIR;
        }
        if (in->kind == RVVM_MEMFS_LNK) {
            /* O_CREAT on a symlink is EEXIST, not "follow it and create the
             * target". Creating through a link is what open() without O_EXCL
             * does, and open() is the caller's decision to make, not this one. */
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EEXIST;
        }
        /* O_CREAT on an existing file truncates it, which is what the flag says
         * and what a caller opening with O_TRUNC|O_CREAT expects. Truncating
         * through one name of a hard-linked file truncates the one inode every
         * name sees, which is exactly what a hard link means. The bytes it held
         * go back to the size budget as they are released: a guest that fills a
         * tmpfs to its limit, truncates the file and rewrites it has to be able
         * to, and keeping the old contents charged would fail the rewrite with
         * ENOSPC on a filesystem that is now empty. */
        if (fs->size_limit && fs->used >= in->size) {
            fs->used -= in->size;
        }
        in->size  = 0;
        in->mtime = memfs_now();
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_OK;
    }
    rc = memfs_inode_new_locked(fs, RVVM_MEMFS_REG, mode, &inode);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_dentry_new_locked(fs, norm, parent, inode, &idx);
        if (rc != RVVM_MEMFS_OK) {
            memfs_inode_abandon_locked(fs, inode);
        }
    }
    if (rc == RVVM_MEMFS_OK) {
        memfs_attach_locked(fs, parent, idx);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_unlink(rvvm_memfs_t* fs, const char* path, bool dir)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    uint32_t            idx;
    uint8_t             kind;
    rvvm_memfs_result_t rc;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    idx = memfs_find_locked(fs, norm);
    if (idx == RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    kind = memfs_inode_of(fs, idx)->kind;
    if (kind == RVVM_MEMFS_DIR) {
        if (!dir) {
            /* rmdir() asked to remove a file. */
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EISDIR;
        }
        if (fs->dentries[idx].child != RVVM_MEMFS_NONE) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOTEMPTY;
        }
    } else if (dir) {
        /* unlink() asked to remove a directory. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOTDIR;
    }
    /* One name, not the sibling chain: unlinking /a must leave /b alone, and the
     * two are the same call site one character apart. A hard link is the same
     * idea: this drops the one name, and the inode goes only if it was the last -
     * which memfs_remove_node_locked() decides from its nlink. */
    memfs_remove_node_locked(fs, idx);
    /* The parent moved: a name came out of it, and if that name was a directory,
     * one more of its links. */
    {
        memfs_inode_t* p = memfs_inode_of(fs, parent);
        p->mtime = memfs_now();
        if (kind == RVVM_MEMFS_DIR && p->nlink > 0) {
            p->nlink--;
        }
    }
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

/* link(2): give @from's inode a second name, @to. Whether the two are on the
 * same mount is the caller's question - this module does not know where its
 * mount point is - and so is EXDEV.
 *
 * The source is NOT followed: Linux's link() links the symlink itself, not what
 * it points at, and following here would quietly make a link to a link into a
 * second name for the target instead.
 *
 * Directories cannot be hard-linked (Linux answers EPERM): a directory with two
 * parents is not a tree, and every walk from the root would have to survive the
 * loop. */
rvvm_memfs_result_t rvvm_memfs_link(rvvm_memfs_t* fs, const char* from, const char* to)
{
    char                src[RVVM_MEMFS_PATH_MAX];
    char                dst[RVVM_MEMFS_PATH_MAX];
    uint32_t            dstparent;
    uint32_t            idx;
    uint32_t            inode;
    uint32_t            newidx;
    rvvm_memfs_result_t rc;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(src, sizeof(src), from);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_normalize(dst, sizeof(dst), to);
    }
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    if (!strcmp(dst, "/")) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    /* find, not resolve: @from names the link itself, never its target. */
    idx = memfs_find_locked(fs, src);
    if (idx == RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    inode = fs->dentries[idx].inode;
    if (fs->inodes[inode].kind == RVVM_MEMFS_DIR) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EPERM;
    }
    if (memfs_find_locked(fs, dst) != RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EEXIST;
    }
    rc = memfs_parent_locked(fs, dst, &dstparent);
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    rc = memfs_dentry_new_locked(fs, dst, dstparent, inode, &newidx);
    if (rc == RVVM_MEMFS_OK) {
        memfs_inode_t* in = &fs->inodes[inode];
        memfs_attach_locked(fs, dstparent, newidx);
        in->nlink++;
        in->mtime = memfs_now();
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* The longest path any name in @idx's subtree would have if @idx were moved to a
 * path of @prefix_len bytes. Measured before the move rather than discovered
 * while doing it, because a subtree whose deepest path no longer fits cannot be
 * half-renamed: the names already rewritten would be under the new prefix and the
 * rest under the old one, and neither key would be findable. Caller holds the
 * lock. */
static size_t memfs_subtree_extent(const rvvm_memfs_t* fs, uint32_t idx,
                                   size_t prefix_len)
{
    const memfs_dentry_t* d = &fs->dentries[idx];
    size_t                deepest = prefix_len;
    uint32_t              kid;

    for (kid = d->child; kid != RVVM_MEMFS_NONE; kid = fs->dentries[kid].next) {
        size_t here = prefix_len + 1 + strlen(memfs_basename(fs->dentries[kid].path));
        size_t down;
        if (here > deepest) {
            deepest = here;
        }
        down = memfs_subtree_extent(fs, kid, here);
        if (down > deepest) {
            deepest = down;
        }
    }
    return deepest;
}

/* Rewrite the keys of @idx's subtree to sit under its current path. Recursive on
 * the depth of the tree, which memfs_normalize() bounds at
 * RVVM_MEMFS_MAX_SEGS components - so the call depth is bounded and cannot
 * overflow.
 *
 * An explicit stack was the first shape and it had to be sized for the name
 * count rather than the depth: the walk is over child lists, so a directory with
 * a thousand subdirectories is legal and would have put a thousand entries on
 * the stack at once, and the cap would have silently abandoned the rest of the
 * subtree mid-rename. Depth-recursion has no such failure mode, because the
 * thing it recurses on is exactly the thing that is bounded.
 *
 * The inode split did not make this O(1), and it is worth being exact about why:
 * the identity is now the inode, but the *path* is still cached on each dentry so
 * a lookup stays one hash. Renaming a directory therefore still rewrites every
 * descendant's cached path. Only the data moved to the inode; the name did not. */
static void memfs_reindex_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    uint32_t kid = fs->dentries[idx].child;

    while (kid != RVVM_MEMFS_NONE) {
        /* Safe to build without a length check: memfs_subtree_extent() measured
         * this exact prefix before the move was committed, and refused the rename
         * if any of these would not fit. */
        const char* dir  = fs->dentries[idx].path;
        const char* base = memfs_basename(fs->dentries[kid].path);
        size_t      need = strlen(dir) + 1 + strlen(base) + 1;
        char*       fresh = malloc(need);
        char*       oldpath;

        if (!fresh) {
            /* Out of memory halfway through. The root of the move is already
             * renamed and this child is not, which is a filesystem in a state no
             * caller can describe - so the whole filesystem is abandoned rather
             * than left half-moved. There is no good answer here; refusing to
             * start would mean pre-allocating every rewritten string up front, and
             * a rename is not worth that. The allocation is the one thing in this
             * module that is not allowed to fail partway, which is why the
             * comments say so. */
            abort();
        }
        memcpy(fresh, dir, strlen(dir));
        fresh[strlen(dir)] = '/';
        memcpy(fresh + strlen(dir) + 1, base, strlen(base) + 1);

        /* Remove by the OLD key before inserting the new one. The map has no
         * tombstones, and changing a key in place would leave the old spelling
         * findable and the new one absent - a lookup of either name answering
         * about the wrong file. */
        oldpath = strdup(fs->dentries[kid].path);
        if (oldpath) {
            memfs_map_remove(fs, oldpath);
            free(oldpath);
        }
        free(fs->dentries[kid].path);
        fs->dentries[kid].path = fresh;
        memfs_map_insert(fs, kid);
        memfs_reindex_locked(fs, kid);
        kid = fs->dentries[kid].next;
    }
}

rvvm_memfs_result_t rvvm_memfs_rename(rvvm_memfs_t* fs, const char* from, const char* to)
{
    char                src[RVVM_MEMFS_PATH_MAX];
    char                dst[RVVM_MEMFS_PATH_MAX];
    uint32_t            srcparent;
    uint32_t            dstparent;
    rvvm_memfs_result_t rc;
    uint32_t            idx;
    uint32_t            dstidx;
    uint32_t            walk;
    uint8_t             src_kind;
    uint8_t             dst_kind = 0;
    char*               newpath;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(src, sizeof(src), from);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_normalize(dst, sizeof(dst), to);
    }
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    if (!strcmp(src, "/") || !strcmp(dst, "/")) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    rc = memfs_parent_locked(fs, src, &srcparent);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_parent_locked(fs, dst, &dstparent);
    }
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    idx = memfs_find_locked(fs, src);
    if (idx == RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    src_kind = memfs_inode_of(fs, idx)->kind;
    /* A directory cannot be moved inside itself. Checked by walking the
     * destination's parent chain: /a/b/a exists iff /a/b is being moved into a
     * directory under itself, and this is the only way to ask without copying
     * the tree. */
    if (src_kind == RVVM_MEMFS_DIR) {
        for (walk = dstparent; walk != RVVM_MEMFS_NONE; walk = fs->dentries[walk].parent) {
            if (walk == idx) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_EINVAL;
            }
        }
    }
    dstidx = memfs_find_locked(fs, dst);
    if (dstidx != RVVM_MEMFS_NONE) {
        dst_kind = memfs_inode_of(fs, dstidx)->kind;
        if (dst_kind == RVVM_MEMFS_DIR) {
            if (src_kind != RVVM_MEMFS_DIR) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_EISDIR;
            }
            if (fs->dentries[dstidx].child != RVVM_MEMFS_NONE) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_ENOTEMPTY;
            }
        } else if (src_kind == RVVM_MEMFS_DIR) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOTDIR;
        }
    }
    /* Every path under the moved name is prefixed with the new one, so the
     * subtree's keys have to be rewritten. That is what makes a rename O(size of
     * subtree) here rather than O(1): the inode is the identity now, but the
     * cached path is still per dentry. The measurement is what makes the rewrite
     * below unable to fail on a path that does not fit. */
    if (memfs_subtree_extent(fs, idx, strlen(dst)) >= RVVM_MEMFS_PATH_MAX) {
        /* ENAMETOOLONG is the honest answer and EINVAL would name the wrong
         * problem, but the whole tree cannot be renamed atomically, so this is
         * reported as "no" rather than "partly": the destination is too deep for
         * the paths underneath it, which is a statement about @to and not about
         * a syscall argument. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    newpath = strdup(dst);
    if (!newpath) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOMEM;
    }
    /* The name being replaced goes FIRST, before this one is filed under its new
     * name. Order matters here in a way that is not obvious from either step:
     * once this name is inserted under @dst, the map holds two entries whose key
     * is the same string - the replaced name's and this one's - and
     * memfs_map_remove() finds a key, not a slot. Removing the replaced one
     * afterwards would therefore take whichever slot the probe reached first,
     * which is the one just inserted, and leave the other behind pointing at a
     * dentry whose path string is about to be freed. The map would then hold a
     * dangling key, and the next lookup that walks past that slot reads freed
     * memory to compare a path with - which is a use-after-free that looks like
     * an ordinary ENOENT until it does not.
     *
     * A replaced directory takes one link off its parent here; the count goes
     * back on below if the source was a directory too. */
    if (dstidx != RVVM_MEMFS_NONE) {
        memfs_remove_node_locked(fs, dstidx);
        if (dst_kind == RVVM_MEMFS_DIR) {
            memfs_inode_of(fs, dstparent)->nlink--;
        }
    }
    /* This name's own key moves too, not only its descendants'. Doing that for
     * the subtree only - which is what the reindex below does, and it is easy to
     * assume it covers the root - leaves the map holding this name's OLD path as
     * its key while dentries[idx].path is the new one. Every later lookup then
     * compares the two and finds neither: the old name hashes to a slot whose
     * entry answers with a different path, and the new name finds nothing. */
    {
        char* oldkey = strdup(src);
        if (oldkey) {
            memfs_map_remove(fs, oldkey);
            free(oldkey);
        }
    }
    free(fs->dentries[idx].path);
    fs->dentries[idx].path = newpath;
    memfs_map_insert(fs, idx);
    memfs_detach_locked(fs, idx);
    memfs_attach_locked(fs, dstparent, idx);
    /* A directory moved between two parents takes a link off the one it left and
     * gives one to the one it entered. Moving it within one parent changes
     * neither: the dentry never left, it was only renamed. */
    if (src_kind == RVVM_MEMFS_DIR && srcparent != dstparent) {
        memfs_inode_of(fs, srcparent)->nlink--;
        memfs_inode_of(fs, dstparent)->nlink++;
    }
    /* Reindex the moved subtree, and only now that the move is committed. */
    memfs_reindex_locked(fs, idx);
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

rvvm_memfs_result_t rvvm_memfs_symlink(rvvm_memfs_t* fs, const char* target, const char* path)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    uint32_t            inode;
    uint32_t            idx;
    char*               tcopy;
    rvvm_memfs_result_t rc;

    if (!target || !target[0]) {
        return RVVM_MEMFS_EINVAL;
    }
    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    if (memfs_find_locked(fs, norm) != RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EEXIST;
    }
    tcopy = strdup(target);
    if (!tcopy) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOMEM;
    }
    rc = memfs_inode_new_locked(fs, RVVM_MEMFS_LNK, 0777, &inode);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_dentry_new_locked(fs, norm, parent, inode, &idx);
        if (rc != RVVM_MEMFS_OK) {
            memfs_inode_abandon_locked(fs, inode);
        }
    }
    if (rc != RVVM_MEMFS_OK) {
        free(tcopy);
        rvvm_unlock(&fs->lock);
        return rc;
    }
    /* The target lives on the inode: it is a property of the link, and a link has
     * exactly one name (hard-linking a symlink would share this). */
    fs->inodes[inode].target = tcopy;
    memfs_attach_locked(fs, parent, idx);
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

rvvm_memfs_result_t rvvm_memfs_readlink(rvvm_memfs_t* fs, const char* path,
                                        char* buf, size_t size)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (!fs || !buf || size == 0) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        /* Not followed: readlink(2) is about the link itself, and following it
         * here would make "readlink /some/link" answer with the target of the
         * target. */
        idx = memfs_find_locked(fs, norm);
        if (idx == RVVM_MEMFS_NONE) {
            rc = RVVM_MEMFS_ENOENT;
        } else if (memfs_inode_of(fs, idx)->kind != RVVM_MEMFS_LNK) {
            rc = RVVM_MEMFS_EINVAL;
        } else {
            const char* t = memfs_inode_of(fs, idx)->target ? memfs_inode_of(fs, idx)->target : "";
            size_t      n = strlen(t);
            if (n >= size) {
                n = size - 1;
            }
            memcpy(buf, t, n);
            buf[n] = '\0';
        }
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* The truncate itself is defined with the other inode-keyed helpers below, since
 * the descriptor-keyed call shares it; declared here because this path-keyed one
 * comes first. */
static rvvm_memfs_result_t memfs_truncate_inode_locked(rvvm_memfs_t* fs, memfs_inode_t* in,
                                                       uint64_t size);

rvvm_memfs_result_t rvvm_memfs_truncate(rvvm_memfs_t* fs, const char* path, uint64_t size)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, true, &idx);
    }
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_truncate_inode_locked(fs, memfs_inode_of(fs, idx), size);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_chmod(rvvm_memfs_t* fs, const char* path, uint32_t mode)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        /* Follows: chmod on a symlink is a different, privileged operation
         * (it changes the link, not what it points at) and this is the ordinary
         * one. */
        rc = memfs_resolve_locked(fs, norm, true, &idx);
        if (rc == RVVM_MEMFS_OK) {
            memfs_inode_of(fs, idx)->mode = mode & 07777;
        }
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* --- reading and writing ------------------------------------------------ */

/* The read itself, on an inode the caller has already resolved. Split out so the
 * path-keyed call and the descriptor-keyed one (rvvm_memfs_read_at) share one
 * implementation: a descriptor names an inode, not a name, and the only
 * difference is how the inode was found. Caller holds the lock. */
static rvvm_memfs_result_t memfs_read_inode_locked(memfs_inode_t* in,
                                                   uint64_t off, void* buf, size_t count,
                                                   size_t* done)
{
    size_t avail;

    if (in->kind == RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_EISDIR;
    }
    if (in->kind != RVVM_MEMFS_REG) {
        /* Resolved with follow, so a link that survived is a link with no
         * target - which is ENOENT to a caller, not a readable file. */
        return RVVM_MEMFS_EINVAL;
    }
    if (off >= in->size) {
        /* Past the end is a short read, not an error: read(2) at or beyond EOF
         * returns 0, and a guest looping until it sees 0 is how a file copy
         * terminates. */
        return RVVM_MEMFS_OK;
    }
    avail = in->size - (size_t)off;
    if (avail > count) {
        avail = count;
    }
    memcpy(buf, in->data + off, avail);
    if (done) {
        *done = avail;
    }
    return RVVM_MEMFS_OK;
}

/* The write itself. Caller holds the lock, and the read-only check has already
 * been made by whoever found the inode. */
static rvvm_memfs_result_t memfs_write_inode_locked(rvvm_memfs_t* fs, memfs_inode_t* in,
                                                    uint64_t off, const void* buf, size_t count,
                                                    size_t* done)
{
    uint64_t need;

    if (in->kind == RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_EISDIR;
    }
    if (in->kind != RVVM_MEMFS_REG) {
        return RVVM_MEMFS_EINVAL;
    }
    need = off + count;
    if (need > in->size) {
        uint64_t delta = need - in->size;
        if (fs->size_limit && (fs->used + delta) > fs->size_limit) {
            /* ENOSPC and not a short write: the guest asked for @count bytes at
             * @off and either all of it is stored or the call fails, so a
             * partial write cannot leave a file the guest believes it filled. */
            return RVVM_MEMFS_ENOSPC;
        }
        if (!memfs_inode_reserve(in, (size_t)need)) {
            return RVVM_MEMFS_ENOMEM;
        }
        /* The gap a sparse write leaves has to read back as zeros, or a guest
         * that seeks past the end and writes would hand its own reader whatever
         * was in the reused buffer. */
        if (off > in->size) {
            memset(in->data + in->size, 0, (size_t)(off - in->size));
        }
        fs->used += delta;
        in->size = (size_t)need;
    }
    memcpy(in->data + off, buf, count);
    in->mtime = memfs_now();
    if (done) {
        *done = count;
    }
    return RVVM_MEMFS_OK;
}

/* The truncate itself. Caller holds the lock. */
static rvvm_memfs_result_t memfs_truncate_inode_locked(rvvm_memfs_t* fs, memfs_inode_t* in,
                                                       uint64_t size)
{
    if (in->kind == RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_EISDIR;
    }
    if (in->kind == RVVM_MEMFS_LNK) {
        /* truncate() follows the link, so reaching a link here means its target
         * is not a regular file. */
        return RVVM_MEMFS_EINVAL;
    }
    if (size > in->size) {
        uint64_t delta = size - in->size;
        if (fs->size_limit && (fs->used + delta) > fs->size_limit) {
            return RVVM_MEMFS_ENOSPC;
        }
        if (!memfs_inode_reserve(in, (size_t)size)) {
            return RVVM_MEMFS_ENOMEM;
        }
        memset(in->data + in->size, 0, (size_t)delta);
        fs->used += delta;
    } else if (fs->size_limit && fs->used >= in->size) {
        /* Shrinking releases, and the budget has to follow for the same reason
         * truncation does: a guest that cuts a file to fit inside its limit and
         * then writes to it again would otherwise be refused by a filesystem
         * that is holding fewer bytes than it did before. Truncating one name of
         * a hard-linked file truncates the single inode every name shares. */
        fs->used -= in->size - (size_t)size;
    }
    in->size = (size_t)size;
    in->mtime = memfs_now();
    return RVVM_MEMFS_OK;
}

rvvm_memfs_result_t rvvm_memfs_read(rvvm_memfs_t* fs, const char* path,
                                    uint64_t off, void* buf, size_t count, size_t* done)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (done) {
        *done = 0;
    }
    if (!fs || !buf) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, true, &idx);
    }
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_read_inode_locked(memfs_inode_of(fs, idx), off, buf, count, done);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_write(rvvm_memfs_t* fs, const char* path,
                                     uint64_t off, const void* buf, size_t count, size_t* done)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    if (done) {
        *done = 0;
    }
    if (!fs || !buf) {
        return RVVM_MEMFS_EINVAL;
    }
    if (count == 0) {
        return RVVM_MEMFS_OK;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        /* The one place MS_RDONLY means something. Checked here rather than at
         * the mount table because here is where the bytes are - and a
         * read-only mount that accepted a write would be telling the guest it
         * had been told no. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, true, &idx);
    }
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_write_inode_locked(fs, memfs_inode_of(fs, idx), off, buf, count, done);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* --- open handles: an inode a descriptor keeps alive -------------------- */

/* Whether @ino names a live inode. Caller holds the lock. */
static bool memfs_inode_live(const rvvm_memfs_t* fs, uint32_t ino)
{
    return ino < fs->ino_count && fs->inodes[ino].kind != 0;
}

/* The one dentry naming @ino, or RVVM_MEMFS_NONE. A directory has exactly one
 * (a directory cannot be hard-linked), so this is how a directory inode is
 * reached from a descriptor that holds only the inode. Caller holds the lock. */
static uint32_t memfs_dentry_of_inode(const rvvm_memfs_t* fs, uint32_t ino)
{
    size_t i;
    for (i = 0; i < fs->count; ++i) {
        if (fs->dentries[i].path && fs->dentries[i].inode == ino) {
            return (uint32_t)i;
        }
    }
    return RVVM_MEMFS_NONE;
}

rvvm_memfs_result_t rvvm_memfs_pin(rvvm_memfs_t* fs, uint32_t ino)
{
    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (!memfs_inode_live(fs, ino)) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    fs->inodes[ino].pins++;
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

void rvvm_memfs_unpin(rvvm_memfs_t* fs, uint32_t ino)
{
    memfs_inode_t* in;

    if (!fs) {
        return;
    }
    rvvm_lock(&fs->lock);
    if (!memfs_inode_live(fs, ino)) {
        rvvm_unlock(&fs->lock);
        return;
    }
    in = &fs->inodes[ino];
    if (in->pins == 0) {
        rvvm_unlock(&fs->lock);
        return;
    }
    in->pins--;
    /* The last name may have gone while this pin held the inode, and dropping
     * the last pin is what finally frees it. A file whose names are all gone has
     * nlink 0; a directory, whose nlink is the 2+subdirectories convention
     * rather than a count, is asked whether any name still reaches it. */
    if (in->pins == 0 &&
        ((in->kind != RVVM_MEMFS_DIR) ? (in->nlink == 0)
                                      : (memfs_dentry_of_inode(fs, ino) == RVVM_MEMFS_NONE))) {
        memfs_inode_release(in);
        memfs_inode_slot_free(fs, ino);
    }
    rvvm_unlock(&fs->lock);
}

rvvm_memfs_result_t rvvm_memfs_read_at(rvvm_memfs_t* fs, uint32_t ino,
                                       uint64_t off, void* buf, size_t count, size_t* done)
{
    rvvm_memfs_result_t rc;

    if (done) {
        *done = 0;
    }
    if (!fs || !buf) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    if (!memfs_inode_live(fs, ino)) {
        rc = RVVM_MEMFS_ENOENT;
    } else {
        rc = memfs_read_inode_locked(&fs->inodes[ino], off, buf, count, done);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_write_at(rvvm_memfs_t* fs, uint32_t ino,
                                        uint64_t off, const void* buf, size_t count, size_t* done)
{
    rvvm_memfs_result_t rc;

    if (done) {
        *done = 0;
    }
    if (!fs || !buf) {
        return RVVM_MEMFS_EINVAL;
    }
    if (count == 0) {
        return RVVM_MEMFS_OK;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    if (!memfs_inode_live(fs, ino)) {
        rc = RVVM_MEMFS_ENOENT;
    } else {
        rc = memfs_write_inode_locked(fs, &fs->inodes[ino], off, buf, count, done);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_truncate_ino(rvvm_memfs_t* fs, uint32_t ino, uint64_t size)
{
    rvvm_memfs_result_t rc;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    if (!memfs_inode_live(fs, ino)) {
        rc = RVVM_MEMFS_ENOENT;
    } else {
        rc = memfs_truncate_inode_locked(fs, &fs->inodes[ino], size);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* The stat of an inode, for fstat(). @out->path is left empty on purpose: an
 * inode reached by number has no one name, and inventing one - the first name
 * that happens to reach it - would be a path the caller never asked about. Every
 * other field is the inode's own. */
rvvm_memfs_result_t rvvm_memfs_stat_ino(rvvm_memfs_t* fs, uint32_t ino, rvvm_memfs_info_t* out)
{
    memfs_inode_t* in;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (!memfs_inode_live(fs, ino)) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    if (out) {
        in = &fs->inodes[ino];
        out->index = ino;
        out->path[0] = '\0';
        out->kind  = in->kind;
        out->mode  = in->mode;
        out->size  = (in->kind == RVVM_MEMFS_REG) ? (uint64_t)in->size : 0;
        out->ino   = in->ino;
        out->nlink = in->nlink;
        out->mtime = in->mtime;
    }
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

/* --- listing ------------------------------------------------------------ */

/* The basename of a name, for a listing. Read off the dentry's own stored path
 * rather than recomputed, so it cannot disagree with the key it is filed under. */
static const char* memfs_dentry_name(const memfs_dentry_t* d)
{
    return memfs_basename(d->path);
}

/* One entry of the listing of the directory @dir (a dentry index). Caller holds
 * the lock and has checked @pos and the buffers. Split out so the path-keyed and
 * the descriptor-keyed calls share it, the same way read and write do. */
static rvvm_memfs_result_t memfs_getdents_dentry_locked(rvvm_memfs_t* fs, uint32_t dir,
                                                        uint32_t* pos, char* out_name, size_t size,
                                                        uint8_t* out_kind, uint64_t* out_ino)
{
    uint32_t seen = 0;
    uint32_t kid;

    if (memfs_inode_of(fs, dir)->kind != RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_ENOTDIR;
    }
    /* The cursor is a count of entries already handed out, not an index into the
     * child list. That costs a walk from the first child per call and buys a
     * cursor that cannot be invalidated: a child removed between two calls is
     * then simply not visited, where a list-position cursor would name a name
     * that is gone and have to be repaired against whatever index was reused in
     * its place - a file appearing in a directory it was never created in. */
    for (kid = fs->dentries[dir].child; kid != RVVM_MEMFS_NONE; kid = fs->dentries[kid].next) {
        if (seen++ < *pos) {
            continue;
        }
        {
            const char* name = memfs_dentry_name(&fs->dentries[kid]);
            size_t      nlen = strlen(name);
            if (nlen >= size) {
                /* A name this module stored cannot exceed RVVM_MEMFS_NAME_MAX,
                 * so this is the caller's buffer being too small rather than a
                 * corrupt entry. Refusing is better than truncating a name,
                 * which would be a different name. */
                return RVVM_MEMFS_EINVAL;
            }
            memcpy(out_name, name, nlen + 1);
        }
        if (out_kind) {
            *out_kind = fs->inodes[fs->dentries[kid].inode].kind;
        }
        if (out_ino) {
            *out_ino = fs->inodes[fs->dentries[kid].inode].ino;
        }
        *pos = seen;
        return RVVM_MEMFS_OK;
    }
    return RVVM_MEMFS_EOF;
}

rvvm_memfs_result_t rvvm_memfs_getdents(rvvm_memfs_t* fs, const char* path,
                                        uint32_t* pos, char* out_name, size_t size,
                                        uint8_t* out_kind, uint64_t* out_ino)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            dir;

    if (!fs || !pos || !out_name || size == 0) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, true, &dir);
    }
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_getdents_dentry_locked(fs, dir, pos, out_name, size, out_kind, out_ino);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* The listing of a directory named by inode, for a descriptor that already
 * resolved it. A directory removed while still open has no dentry left to walk,
 * so its listing is empty (EOF) rather than an error - which is also true of the
 * directory itself: rmdir only removes an empty one. */
rvvm_memfs_result_t rvvm_memfs_getdents_ino(rvvm_memfs_t* fs, uint32_t ino,
                                            uint32_t* pos, char* out_name, size_t size,
                                            uint8_t* out_kind, uint64_t* out_ino)
{
    rvvm_memfs_result_t rc;
    uint32_t            dir;

    if (!fs || !pos || !out_name || size == 0) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    if (!memfs_inode_live(fs, ino)) {
        rc = RVVM_MEMFS_ENOENT;
    } else {
        dir = memfs_dentry_of_inode(fs, ino);
        rc = (dir == RVVM_MEMFS_NONE)
           ? RVVM_MEMFS_EOF
           : memfs_getdents_dentry_locked(fs, dir, pos, out_name, size, out_kind, out_ino);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}
