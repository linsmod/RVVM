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

  - the path -> node map is open addressing with linear probing, keyed by the
    FNV-1a hash of the normalized path, exactly as vp_shadow's is. Slots hold
    index + 1 so 0 means empty.
  - nodes live in one growable array; a returned index is invalidated by the next
    rvvm_memfs_create_file(). The interface returns indices and values rather
    than pointers for that reason, and so does vp_shadow.
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

  - size= is charged against file contents only. A directory and a symlink cost
    a node, not bytes, and their cost is bounded by RVVM_MEMFS_MAX_NODES. A guest
    that fills a tmpfs with a million empty files is stopped by the node bound
    rather than by size=, and those are not the same limit.
  - no hard links. A link is a copy here, so writing through one name is not
    visible through the other. nlink is reported as 1 for a file and 2 for a
    directory, which is what the shape of the data supports.
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

/* Entries start here and double; the map rehashes at 70% load, where linear
 * probing is still short. 64 is small enough that an empty root costs one page
 * and large enough that a guest's real tmpfs never rehashes. */
#define MEMFS_MIN_CAPACITY 64
#define MEMFS_MIN_MAP     128

/* How many symlinks one lookup follows before giving up (ELOOP). Linux says 40;
 * a guest that hits that has a cycle, and the number only decides how long the
 * walk takes to notice. */
#define MEMFS_MAX_FOLLOW 8

typedef struct {
    char*    path;      /* absolute within the mount: "/", "/a/b"; owned        */
    char*    target;    /* symlink target; owned, NULL unless kind is LNK      */
    uint8_t* data;      /* file contents; owned, NULL unless kind is REG        */
    size_t   size;
    size_t   capacity;
    uint64_t ino;       /* synthetic, stable, non-zero                         */
    uint32_t mode;      /* permission bits only; the type comes from @kind      */
    uint32_t parent;    /* RVVM_MEMFS_NONE for the root                         */
    uint32_t child;     /* first child, RVVM_MEMFS_NONE ends it                 */
    uint32_t next;      /* next sibling, RVVM_MEMFS_NONE ends it               */
    uint8_t  kind;      /* rvvm_memfs_kind_t                                    */
    uint32_t nlink;
    int64_t  mtime;
} memfs_node_t;

struct rvvm_memfs {
    rvvm_lock_t lock;
    bool        read_only;
    uint64_t    size_limit;
    uint64_t    used;
    memfs_node_t* entries;
    uint32_t*   map;      /* open addressing over node indices + 1              */
    size_t      map_size; /* power of two                                       */
    /* Slots handed out, ever. NOT the number of live nodes: a removed slot is
     * kept and reused through free_head rather than returned, because returning
     * it would mean the next allocation lands on index @count - which is only
     * the last live node when nothing has been removed yet. A filesystem that
     * creates and deletes one file at a time would overwrite a live node on the
     * second create, and the map would keep a key pointing at a node whose path
     * had just become someone else's: every later lookup of the clobbered name
     * compares a path against a key that no longer matches it, and answers
     * ENOENT for a file that is still listed in its parent's directory. */
    size_t      count;
    size_t      capacity;   /* slots the array has room for, >= @count          */
    uint32_t    free_head; /* first free slot, chained through ->child            */
    size_t      live;      /* nodes in use, which is not @count                   */
    uint32_t    root;
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

/* --- the path -> node map ------------------------------------------------ */

static void memfs_map_insert(rvvm_memfs_t* fs, uint32_t idx)
{
    uint32_t slot = memfs_hash(fs->entries[idx].path) & (uint32_t)(fs->map_size - 1);
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
        if (!strcmp(fs->entries[fs->map[slot] - 1].path, path)) {
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
     * file is in the table, in its parent's child list, and unfindable - and
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
        k = memfs_hash(fs->entries[fs->map[j] - 1].path) & mask;
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

    if (newsize > (size_t)RVVM_MEMFS_MAX_NODES * 4) {
        return false; /* the node bound is the real limit; the map needs no more */
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
            if (fs->entries[i].path) {
                memfs_map_insert(fs, (uint32_t)i);
            }
        }
    }
    return true;
}

/* RVVM_MEMFS_NONE when there is no such node. Caller holds the lock. */
static uint32_t memfs_find_locked(rvvm_memfs_t* fs, const char* path)
{
    uint32_t slot;

    if (!fs->map || !path) {
        return RVVM_MEMFS_NONE;
    }
    slot = memfs_hash(path) & (uint32_t)(fs->map_size - 1);
    while (fs->map[slot]) {
        uint32_t idx = fs->map[slot] - 1;
        if (!strcmp(fs->entries[idx].path, path)) {
            return idx;
        }
        slot = (slot + 1) & (uint32_t)(fs->map_size - 1);
    }
    return RVVM_MEMFS_NONE;
}

/* --- nodes -------------------------------------------------------------- */

static void memfs_node_release(memfs_node_t* n)
{
    free(n->path);
    free(n->target);
    free(n->data);
    n->path = n->target = NULL;
    n->data = NULL;
    n->size = n->capacity = 0;
}

/* Hand a slot back for reuse. It must already be out of the map and out of its
 * parent's child list; what this adds is the free-list link and the live count,
 * which is what stops the next allocation from landing on it by arithmetic.
 *
 * The link goes in ->child because every other field of a released node is
 * either NULL or meaningless, and a field that is only ever read as a free-list
 * link cannot be confused with the child list it used to hold. */
static void memfs_slot_free(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_node_t* n = &fs->entries[idx];
    n->child  = fs->free_head;
    n->parent = RVVM_MEMFS_NONE;
    n->next   = RVVM_MEMFS_NONE;
    fs->free_head = idx;
    if (fs->live) {
        fs->live--;
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
static bool memfs_node_reserve(memfs_node_t* n, size_t need)
{
    uint8_t* fresh;
    size_t   cap;

    if (need <= n->capacity) {
        return true;
    }
    cap = n->capacity ? n->capacity : 64;
    while (cap < need) {
        cap *= 2;
    }
    fresh = realloc(n->data, cap);
    if (!fresh) {
        return false;
    }
    n->data     = fresh;
    n->capacity = cap;
    return true;
}

static int64_t memfs_now(void)
{
    /* One second, which is what a tmpfs this size has for mtime granularity. The
     * alternative - host time - would make two runs of the same program produce
     * different numbers and turn mtime into noise. */
    return (int64_t)time(NULL);
}

/* Add a node. @parent must already exist. Caller holds the lock and has checked
 * the read-only flag and the node bound. */
static rvvm_memfs_result_t memfs_add_locked(rvvm_memfs_t* fs, const char* path,
                                            uint32_t parent, uint8_t kind,
                                            uint32_t mode, uint32_t* out_idx)
{
    memfs_node_t* n;
    char*         copy;
    uint32_t      idx;
    bool          grew = false;

    /* The node bound counts live nodes, not slots ever handed out: a filesystem
     * that creates and removes the same file ten thousand times has used one
     * node, and ENOSPC there would be a lie about how full the filesystem is. */
    if (fs->live >= RVVM_MEMFS_MAX_NODES) {
        return RVVM_MEMFS_ENOSPC;
    }
    if (fs->free_head != RVVM_MEMFS_NONE) {
        /* Reuse a hole. Its ->child is the free-list link, planted by
         * memfs_remove_node_locked(); everything else in the record is zeroed. */
        idx = fs->free_head;
        fs->free_head = fs->entries[idx].child;
    } else {
        if (fs->count >= fs->capacity) {
            size_t        ncap = fs->capacity ? fs->capacity * 2 : MEMFS_MIN_CAPACITY;
            memfs_node_t* nentries;
            if (ncap > RVVM_MEMFS_MAX_NODES) {
                ncap = RVVM_MEMFS_MAX_NODES;
            }
            nentries = realloc(fs->entries, ncap * sizeof(memfs_node_t));
            if (!nentries) {
                return RVVM_MEMFS_ENOMEM;
            }
            fs->entries  = nentries;
            fs->capacity = ncap;
        }
        idx = (uint32_t)fs->count++;
    }
    copy = strdup(path);
    if (!copy) {
        return RVVM_MEMFS_ENOMEM;
    }
    n          = &fs->entries[idx];
    memset(n, 0, sizeof(*n));
    n->path     = copy;
    n->kind     = kind;
    n->mode     = mode & 07777;
    n->ino      = ++fs->next_ino;
    n->parent   = parent;
    n->child    = RVVM_MEMFS_NONE;
    n->next     = RVVM_MEMFS_NONE;
    n->nlink    = 1;
    n->mtime    = memfs_now();
    fs->live++;

    /* Keep the map at least 70% full. Measured against live nodes, because a
     * free slot is not in the map and must not count towards the load. */
    if (!fs->map || (fs->live * 10) >= (fs->map_size * 7)) {
        /* A successful grow has already re-inserted every live node, this one
         * among them - so the insert below must not run. Doing both files this
         * node under two slots with one key, and the two disagree about which
         * one a removal should drop: the survivor is then an entry no probe
         * reaches, because the hole-closing pass moved it out of its own home's
         * run while the other copy was the one that got cleared. A duplicate is
         * invisible from the outside - the file is findable right up until it is
         * removed, and unfindable afterwards. */
        grew = memfs_map_grow(fs);
        if (!grew && !fs->map) {
            memfs_slot_free(fs, idx);
            return RVVM_MEMFS_ENOMEM;
        }
    }
    if (!grew) {
        memfs_map_insert(fs, idx);
    }
    *out_idx = idx;
    return RVVM_MEMFS_OK;
}

/* Unlink a node from its parent's child list. Caller holds the lock.
 *
 * The parent's own slot holds the first child, and every later child is reached
 * through the previous sibling's ->next, so the two positions are separate and
 * both have to be handled. Taking the walk from n->next instead - which is what a
 * "is it the last child?" shortcut would do - silently misses the case where
 * @idx IS the first child, and a detach that misses leaves the node linked into
 * its old parent while memfs_attach_locked() links it into the new one. One node
 * in two child lists is a cycle, and the next walk over either of them never
 * ends - which is a hang in a rename, with nothing to point at.
 *
 * Leaving @parent and @next as they were found is deliberate: the caller either
 * frees the node or immediately re-attaches it, and a detach that cleared them
 * would have to be undone in the second case. */
static void memfs_detach_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    memfs_node_t* n = &fs->entries[idx];
    uint32_t      p = n->parent;
    uint32_t*     slot;

    if (p == RVVM_MEMFS_NONE) {
        return;
    }
    if (fs->entries[p].child == idx) {
        fs->entries[p].child = n->next;
        return;
    }
    slot = &fs->entries[p].child;
    while (*slot != RVVM_MEMFS_NONE) {
        if (fs->entries[*slot].next == idx) {
            fs->entries[*slot].next = n->next;
            return;
        }
        slot = &fs->entries[*slot].next;
    }
}

static void memfs_attach_locked(rvvm_memfs_t* fs, uint32_t parent, uint32_t idx)
{
    memfs_node_t* n = &fs->entries[idx];
    memfs_node_t* p = &fs->entries[parent];

    n->parent = parent;
    n->next   = p->child;
    p->child  = idx;
    p->mtime  = memfs_now();
}

/* Remove a node, its whole subtree, and its entry. Caller holds the lock.
 * Recursive on the depth of the tree, which is bounded by
 * RVVM_MEMFS_MAX_SEGS, so this cannot overflow anything. */
/* Delete @idx and everything under it, and NOTHING else.
 *
 * Not the sibling chain, and that is the whole reason this is its own function
 * rather than the tail of a loop: memfs_remove_subtree_locked() below walks a
 * child list and recurses once per child, so a version that also advanced to
 * each child's next sibling would delete the entire list on the first call. The
 * parent loop would then be holding the index of a node that is already freed,
 * and the corruption that follows is in the allocator rather than in anything a
 * stack trace would point at. One node in, one subtree out; the caller owns
 * where it goes next. */
static void memfs_remove_node_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    while (idx != RVVM_MEMFS_NONE) {
        memfs_node_t* n = &fs->entries[idx];
        uint32_t      child = n->child;

        while (child != RVVM_MEMFS_NONE) {
            /* Read before the recursive call: it frees @child, and @kid is the
             * only way back to the rest of the list. It stays valid because the
             * node array is not compacted on removal - only its count moves, so
             * a freed index is a hole rather than a shift. */
            uint32_t kid = fs->entries[child].next;
            memfs_remove_node_locked(fs, child);
            child = kid;
        }
        if (n->kind == RVVM_MEMFS_REG && fs->size_limit && fs->used >= n->size) {
            /* Only ever subtracting what a write charged, so the counter cannot
             * be driven below zero by a sequence of removes. */
            fs->used -= n->size;
        }
        memfs_detach_locked(fs, idx);
        /* The map is keyed by this node's path and looks it up by value against
         * entries[].path, so the entry has to leave the map BEFORE the string is
         * freed. Copied first because node_release() frees the node's own
         * pointer, and the copy is what outlives the call. */
        {
            char* key = strdup(n->path);
            if (key) {
                memfs_map_remove(fs, key);
                free(key);
            }
        }
        memfs_node_release(n);
        memfs_slot_free(fs, idx);
        idx = RVVM_MEMFS_NONE;
    }
}

/* Remove a node and its whole subtree, then every sibling of it, so a caller
 * with a child list can empty it in one call. Only used where that is the
 * intent; memfs_remove_node_locked() is what the recursive cases want. Caller
 * holds the lock. */

/* --- resolution --------------------------------------------------------- */

/* Resolve @path to a node index, following symlinks when @follow. Caller holds
 * the lock.
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
        if (!follow || fs->entries[idx].kind != RVVM_MEMFS_LNK) {
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
            const char* target = fs->entries[idx].target;
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

/* The parent directory of @path, resolved (its final component may be a link,
 * which a lookup of the parent has to follow to get somewhere real). Caller
 * holds the lock. */
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
    if (fs->entries[*out_parent].kind != RVVM_MEMFS_DIR) {
        return RVVM_MEMFS_ENOTDIR;
    }
    return RVVM_MEMFS_OK;
}

static void memfs_fill_info(const rvvm_memfs_t* fs, uint32_t idx, rvvm_memfs_info_t* out)
{
    const memfs_node_t* n = &fs->entries[idx];

    out->index = idx;
    memcpy(out->path, n->path, strlen(n->path) + 1);
    out->kind = n->kind;
    out->mode = n->mode;
    out->size = (n->kind == RVVM_MEMFS_REG) ? (uint64_t)n->size : 0;
    out->ino  = n->ino;
    /* No hard links (see the header), so a file is 1 and a directory is 2 -
     * which is what a guest's stat() of a fresh directory expects, and what
     * makes the two distinguishable by a program that counts links. */
    out->nlink = (n->kind == RVVM_MEMFS_DIR) ? 2 : 1;
    out->mtime = n->mtime;
}

/* --- consistency check -------------------------------------------------- */

/* Walk the map and the node array against each other and report the first
 * disagreement. Returns 0 when they agree.
 *
 * This exists because the two structures are updated by hand in eight places and
 * the failure mode when they drift is not a crash but a quiet wrong answer: a
 * lookup that compares a node's path against a key belonging to a different node
 * concludes the file is not there. Every way that can happen - a removal leaving
 * a stale slot, a rename forgetting to re-key the root, a reused slot keeping an
 * old identity - produces the same symptom from the outside, and none of them
 * points at the line that caused it. Checking the invariant directly is the
 * difference between a test that says "the map is wrong" and one that says
 * "entry 37 is keyed as /clu/02 but its node says /clu/03".
 *
 * Caller holds the lock. */
static size_t memfs_check_locked(rvvm_memfs_t* fs, char* why, size_t whysize)
{
    size_t i;
    size_t j;
    size_t occupied = 0;

    for (i = 0; i < fs->count; ++i) {
        if (!fs->entries[i].path) {
            continue; /* a free slot */
        }
        if (memfs_find_locked(fs, fs->entries[i].path) != (uint32_t)i) {
            snprintf(why, whysize, "node %zu (%s) is not findable by its own path", i,
                     fs->entries[i].path);
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
        if (idx >= fs->count || !fs->entries[idx].path) {
            snprintf(why, whysize, "slot %zu names node %u, which is free or past the end",
                     i, idx);
            return i;
        }
        h = memfs_hash(fs->entries[idx].path) & (uint32_t)(fs->map_size - 1);
        for (t = h; t != i; t = (t + 1) & (uint32_t)(fs->map_size - 1)) {
            if (!fs->map[t] || ++steps > fs->map_size) {
                snprintf(why, whysize,
                         "slot %zu holds node %u (%s), whose home is %u: a gap in between",
                         i, idx, fs->entries[idx].path, h);
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
                snprintf(why, whysize, "node %u is filed in two slots (%zu and %zu)",
                         fs->map[i] - 1, i, j);
                return i;
            }
        }
    }
    if (occupied != fs->live) {
        snprintf(why, whysize, "%zu slots occupied but %zu nodes live", occupied, fs->live);
        return fs->map_size;
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
    fs->next_ino   = 0;
    fs->root       = RVVM_MEMFS_NONE;
    /* Not 0, which is what calloc left: index 0 is the root and is about to be a
     * live node, so a free list that starts there hands the root itself back out
     * as the next free slot. The filesystem then has two claims on index 0 and
     * the second one to be written wins. */
    fs->free_head  = RVVM_MEMFS_NONE;
    fs->map_size   = MEMFS_MIN_MAP;
    fs->map        = calloc(fs->map_size, sizeof(uint32_t));
    if (!fs->map) {
        free(fs);
        return NULL;
    }
    {
        uint32_t root;
        /* The root is created through the same path as everything else, so it has
         * the same mode and the same mtime and a directory the guest makes is
         * indistinguishable from the one it started with. Not read-only-checked:
         * the root exists whether or not the mount can be written to, which is
         * what makes a read-only mount still have a readable root. */
        if (memfs_add_locked(fs, "/", RVVM_MEMFS_NONE, RVVM_MEMFS_DIR, 0755, &root) !=
            RVVM_MEMFS_OK) {
            free(fs->map);
            free(fs);
            return NULL;
        }
        fs->root = root;
    }
    return fs;
}

void rvvm_memfs_free(rvvm_memfs_t* fs)
{
    size_t i;

    if (!fs) {
        return;
    }
    for (i = 0; i < fs->count; ++i) {
        memfs_node_release(&fs->entries[i]);
    }
    free(fs->entries);
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

uint32_t rvvm_memfs_count(rvvm_memfs_t* fs)
{
    uint32_t count;
    if (!fs) {
        return 0;
    }
    rvvm_lock(&fs->lock);
    count = (uint32_t)fs->count;
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

/* Shared prologue for the four name-changing operations: normalize, refuse a
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
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    if (memfs_find_locked(fs, norm) != RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EEXIST;
    }
    rc = memfs_add_locked(fs, norm, parent, RVVM_MEMFS_DIR, mode, &idx);
    if (rc == RVVM_MEMFS_OK) {
        memfs_attach_locked(fs, parent, idx);
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

rvvm_memfs_result_t rvvm_memfs_create_file(rvvm_memfs_t* fs, const char* path, uint32_t mode)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    idx = memfs_find_locked(fs, norm);
    if (idx != RVVM_MEMFS_NONE) {
        memfs_node_t* n = &fs->entries[idx];
        if (n->kind == RVVM_MEMFS_DIR) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EISDIR;
        }
        if (n->kind == RVVM_MEMFS_LNK) {
            /* O_CREAT on a symlink is EEXIST, not "follow it and create the
             * target". Creating through a link is what open() without O_EXCL
             * does, and open() is the caller's decision to make, not this one. */
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EEXIST;
        }
        /* O_CREAT on an existing file truncates it, which is what the flag says
         * and what a caller opening with O_TRUNC|O_CREAT expects. The bytes it
         * held go back to the size budget as they are released: a guest that
         * fills a tmpfs to its limit, truncates the file and rewrites it has to
         * be able to, and keeping the old contents charged would fail the
         * rewrite with ENOSPC on a filesystem that is now empty. */
        if (fs->size_limit && fs->used >= n->size) {
            fs->used -= n->size;
        }
        n->size  = 0;
        n->mtime = memfs_now();
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_OK;
    }
    rc = memfs_add_locked(fs, norm, parent, RVVM_MEMFS_REG, mode, &idx);
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
    rvvm_memfs_result_t rc;
    uint32_t            idx;

    rc = memfs_prepare_write(fs, path, norm, sizeof(norm), &parent);
    if (rc != RVVM_MEMFS_OK) {
        return rc;
    }
    idx = memfs_find_locked(fs, norm);
    if (idx == RVVM_MEMFS_NONE) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOENT;
    }
    if (fs->entries[idx].kind == RVVM_MEMFS_DIR) {
        if (!dir) {
            /* rmdir() asked to remove a file. */
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_EISDIR;
        }
        if (fs->entries[idx].child != RVVM_MEMFS_NONE) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOTEMPTY;
        }
    } else if (dir) {
        /* unlink() asked to remove a directory. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOTDIR;
    }
    /* One node, not the sibling chain: unlinking /a must leave /b alone, and
     * the two are the same call site one character apart. */
    memfs_remove_node_locked(fs, idx);
    /* The parent's mtime moved: a name came out of it. */
    fs->entries[parent].mtime = memfs_now();
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

/* The longest path any node in @idx's subtree would have if @idx were moved to a
 * path of @prefix_len bytes. Measured before the move rather than discovered
 * while doing it, because a subtree whose deepest path no longer fits cannot be
 * half-renamed: the nodes already rewritten would be under the new prefix and the
 * rest under the old one, and neither key would be findable. Caller holds the
 * lock. */
static size_t memfs_subtree_extent(const rvvm_memfs_t* fs, uint32_t idx,
                                   size_t prefix_len)
{
    const memfs_node_t* n = &fs->entries[idx];
    size_t              deepest = prefix_len;
    uint32_t            kid;

    for (kid = n->child; kid != RVVM_MEMFS_NONE; kid = fs->entries[kid].next) {
        size_t here = prefix_len + 1 + strlen(memfs_basename(fs->entries[kid].path));
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
 * An explicit stack was the first shape and it had to be sized for the node
 * count rather than the depth: the walk is over child lists, so a directory with
 * a thousand subdirectories is legal and would have put a thousand entries on
 * the stack at once, and the cap would have silently abandoned the rest of the
 * subtree mid-rename. Depth-recursion has no such failure mode, because the
 * thing it recurses on is exactly the thing that is bounded. */
static void memfs_reindex_locked(rvvm_memfs_t* fs, uint32_t idx)
{
    uint32_t kid = fs->entries[idx].child;

    while (kid != RVVM_MEMFS_NONE) {
        /* Safe to build without a length check: memfs_subtree_extent() measured
         * this exact prefix before the move was committed, and refused the rename
         * if any of these would not fit. */
        size_t need = strlen(fs->entries[idx].path) + 1 +
                      strlen(memfs_basename(fs->entries[kid].path)) + 1;
        char*  fresh = malloc(need);
        char*  oldpath;

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
        memcpy(fresh, fs->entries[idx].path, strlen(fs->entries[idx].path));
        fresh[strlen(fs->entries[idx].path)] = '/';
        memcpy(fresh + strlen(fs->entries[idx].path) + 1,
               memfs_basename(fs->entries[kid].path), strlen(memfs_basename(fs->entries[kid].path)) + 1);

        /* Remove by the OLD key before inserting the new one. The map has no
         * tombstones, and changing a key in place would leave the old spelling
         * findable and the new one absent - a lookup of either name answering
         * about the wrong file. */
        oldpath = strdup(fs->entries[kid].path);
        if (oldpath) {
            memfs_map_remove(fs, oldpath);
            free(oldpath);
        }
        free(fs->entries[kid].path);
        fs->entries[kid].path = fresh;
        memfs_map_insert(fs, kid);
        memfs_reindex_locked(fs, kid);
        kid = fs->entries[kid].next;
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
    /* A directory cannot be moved inside itself. Checked by walking the
     * destination's parent chain: /a/b/a exists iff /a/b is being moved into a
     * directory under itself, and this is the only way to ask without copying
     * the tree. */
    if (fs->entries[idx].kind == RVVM_MEMFS_DIR) {
        for (walk = dstparent; walk != RVVM_MEMFS_NONE; walk = fs->entries[walk].parent) {
            if (walk == idx) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_EINVAL;
            }
        }
    }
    dstidx = memfs_find_locked(fs, dst);
    if (dstidx != RVVM_MEMFS_NONE) {
        memfs_node_t* d = &fs->entries[dstidx];
        if (d->kind == RVVM_MEMFS_DIR) {
            if (fs->entries[idx].kind != RVVM_MEMFS_DIR) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_EISDIR;
            }
            if (d->child != RVVM_MEMFS_NONE) {
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_ENOTEMPTY;
            }
        } else if (fs->entries[idx].kind == RVVM_MEMFS_DIR) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOTDIR;
        }
    }
    /* Every path under the moved node is prefixed with the new one, so the
     * subtree's keys have to be rewritten. That is what makes a rename O(size of
     * subtree) here rather than O(1) - a link-counted inode would make it O(1),
     * and this module has no inodes to count because it has no hard links. The
     * measurement is what makes the rewrite below unable to fail on a path that
     * does not fit. */
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
    /* The node being replaced goes FIRST, before this one is filed under its new
     * name. Order matters here in a way that is not obvious from either step:
     * once this node is inserted under @dst, the map holds two entries whose key
     * is the same string - the replaced node's and this one's - and
     * memfs_map_remove() finds a key, not a node. Removing the replaced one
     * afterwards would therefore take whichever slot the probe reached first,
     * which is the one just inserted, and leave the other behind pointing at a
     * node whose path string is about to be freed. The map would then hold a
     * dangling key, and the next lookup that walks past that slot reads freed
     * memory to compare a path with - which is a use-after-free that looks like
     * an ordinary ENOENT until it does not. */
    if (dstidx != RVVM_MEMFS_NONE) {
        memfs_remove_node_locked(fs, dstidx);
    }
    /* This node's own key moves too, not only its descendants'. Doing that for
     * the subtree only - which is what the reindex below does, and it is easy to
     * assume it covers the root - leaves the map holding this node's OLD path as
     * its key while entries[idx].path is the new one. Every later lookup then
     * compares the two and finds neither: the old name hashes to a slot whose
     * entry answers with a different path, and the new name finds nothing. */
    {
        char* oldkey = strdup(src);
        if (oldkey) {
            memfs_map_remove(fs, oldkey);
            free(oldkey);
        }
    }
    free(fs->entries[idx].path);
    fs->entries[idx].path = newpath;
    memfs_map_insert(fs, idx);
    memfs_detach_locked(fs, idx);
    memfs_attach_locked(fs, dstparent, idx);
    /* Reindex the moved subtree, and only now that the move is committed. */
    memfs_reindex_locked(fs, idx);
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

rvvm_memfs_result_t rvvm_memfs_symlink(rvvm_memfs_t* fs, const char* target, const char* path)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    uint32_t            parent;
    rvvm_memfs_result_t rc;
    uint32_t            idx;
    char*               tcopy;

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
    rc = memfs_add_locked(fs, norm, parent, RVVM_MEMFS_LNK, 0777, &idx);
    if (rc != RVVM_MEMFS_OK) {
        free(tcopy);
        rvvm_unlock(&fs->lock);
        return rc;
    }
    fs->entries[idx].target = tcopy;
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
        } else if (fs->entries[idx].kind != RVVM_MEMFS_LNK) {
            rc = RVVM_MEMFS_EINVAL;
        } else {
            const char* t = fs->entries[idx].target ? fs->entries[idx].target : "";
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

rvvm_memfs_result_t rvvm_memfs_truncate(rvvm_memfs_t* fs, const char* path, uint64_t size)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;
    memfs_node_t*       n;

    if (!fs) {
        return RVVM_MEMFS_ENOENT;
    }
    rvvm_lock(&fs->lock);
    if (fs->read_only) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EROFS;
    }
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    rc = memfs_resolve_locked(fs, norm, true, &idx);
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    n = &fs->entries[idx];
    if (n->kind == RVVM_MEMFS_DIR) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EISDIR;
    }
    if (n->kind == RVVM_MEMFS_LNK) {
        /* truncate() follows the link, so this is the target's answer. Reaching
         * here means the target is not a regular file. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    if (size > n->size) {
        uint64_t delta = size - n->size;
        if (fs->size_limit && (fs->used + delta) > fs->size_limit) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOSPC;
        }
        if (!memfs_node_reserve(n, (size_t)size)) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOMEM;
        }
        memset(n->data + n->size, 0, (size_t)delta);
        fs->used += delta;
    } else if (fs->size_limit && fs->used >= n->size) {
        /* Shrinking releases, and the budget has to follow for the same reason
         * truncation does: a guest that cuts a file to fit inside its limit and
         * then writes to it again would otherwise be refused by a filesystem
         * that is holding fewer bytes than it did before. */
        fs->used -= n->size - (size_t)size;
    }
    n->size = (size_t)size;
    n->mtime = memfs_now();
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
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
            fs->entries[idx].mode = mode & 07777;
        }
    }
    rvvm_unlock(&fs->lock);
    return rc;
}

/* --- reading and writing ------------------------------------------------ */

rvvm_memfs_result_t rvvm_memfs_read(rvvm_memfs_t* fs, const char* path,
                                    uint64_t off, void* buf, size_t count, size_t* done)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;
    memfs_node_t*       n;
    size_t              avail;

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
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    n = &fs->entries[idx];
    if (n->kind == RVVM_MEMFS_DIR) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EISDIR;
    }
    if (n->kind != RVVM_MEMFS_REG) {
        /* Resolved with follow, so a link that survived is a link with no
         * target - which is ENOENT, not a readable file. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    if (off >= n->size) {
        /* Past the end is a short read, not an error: read(2) at or beyond EOF
         * returns 0, and a guest looping until it sees 0 is how a file copy
         * terminates. */
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_OK;
    }
    avail = n->size - (size_t)off;
    if (avail > count) {
        avail = count;
    }
    memcpy(buf, n->data + off, avail);
    if (done) {
        *done = avail;
    }
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

rvvm_memfs_result_t rvvm_memfs_write(rvvm_memfs_t* fs, const char* path,
                                     uint64_t off, const void* buf, size_t count, size_t* done)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            idx;
    memfs_node_t*       n;
    uint64_t            need;

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
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    n = &fs->entries[idx];
    if (n->kind == RVVM_MEMFS_DIR) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EISDIR;
    }
    if (n->kind != RVVM_MEMFS_REG) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_EINVAL;
    }
    need = off + count;
    if (need > n->size) {
        uint64_t delta = need - n->size;
        if (fs->size_limit && (fs->used + delta) > fs->size_limit) {
            /* ENOSPC and not a short write: the guest asked for @count bytes at
             * @off and either all of it is stored or the call fails, so a
             * partial write cannot leave a file the guest believes it filled. */
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOSPC;
        }
        if (!memfs_node_reserve(n, (size_t)need)) {
            rvvm_unlock(&fs->lock);
            return RVVM_MEMFS_ENOMEM;
        }
        /* The gap a sparse write leaves has to read back as zeros, or a guest
         * that seeks past the end and writes would hand its own reader whatever
         * was in the reused buffer. */
        if (off > n->size) {
            memset(n->data + n->size, 0, (size_t)(off - n->size));
        }
        fs->used += delta;
        n->size = (size_t)need;
    }
    memcpy(n->data + off, buf, count);
    n->mtime = memfs_now();
    if (done) {
        *done = count;
    }
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_OK;
}

/* --- listing ------------------------------------------------------------ */

/* The basename of @path, for a listing. A node's own last component, read off
 * its stored path rather than recomputed, so it cannot disagree with the key it
 * is filed under. */
static const char* memfs_node_name(const memfs_node_t* n)
{
    return memfs_basename(n->path);
}

rvvm_memfs_result_t rvvm_memfs_getdents(rvvm_memfs_t* fs, const char* path,
                                        uint32_t* pos, char* out_name, size_t size,
                                        uint8_t* out_kind, uint64_t* out_ino)
{
    char                norm[RVVM_MEMFS_PATH_MAX];
    rvvm_memfs_result_t rc;
    uint32_t            dir;
    uint32_t            seen = 0;
    uint32_t            kid;

    if (!fs || !pos || !out_name || size == 0) {
        return RVVM_MEMFS_EINVAL;
    }
    rvvm_lock(&fs->lock);
    rc = memfs_normalize(norm, sizeof(norm), path);
    if (rc == RVVM_MEMFS_OK) {
        rc = memfs_resolve_locked(fs, norm, true, &dir);
    }
    if (rc != RVVM_MEMFS_OK) {
        rvvm_unlock(&fs->lock);
        return rc;
    }
    if (fs->entries[dir].kind != RVVM_MEMFS_DIR) {
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_ENOTDIR;
    }
    /* The cursor is a count of entries already handed out, not an index into the
     * child list. That costs a walk from the first child per call and buys a
     * cursor that cannot be invalidated: a child removed between two calls is
     * then simply not visited, where a list-position cursor would name a node
     * that is gone and have to be repaired against whatever index was reused in
     * its place - a file appearing in a directory it was never created in. */
    for (kid = fs->entries[dir].child; kid != RVVM_MEMFS_NONE; kid = fs->entries[kid].next) {
        if (seen++ < *pos) {
            continue;
        }
        {
            const char* name = memfs_node_name(&fs->entries[kid]);
            size_t      nlen = strlen(name);
            if (nlen >= size) {
                /* A name this module stored cannot exceed RVVM_MEMFS_NAME_MAX,
                 * so this is the caller's buffer being too small rather than a
                 * corrupt entry. Refusing is better than truncating a name,
                 * which would be a different name. */
                rvvm_unlock(&fs->lock);
                return RVVM_MEMFS_EINVAL;
            }
            memcpy(out_name, name, nlen + 1);
        }
        if (out_kind) {
            *out_kind = fs->entries[kid].kind;
        }
        if (out_ino) {
            *out_ino = fs->entries[kid].ino;
        }
        *pos = seen;
        rvvm_unlock(&fs->lock);
        return RVVM_MEMFS_OK;
    }
    rvvm_unlock(&fs->lock);
    return RVVM_MEMFS_EOF;
}
