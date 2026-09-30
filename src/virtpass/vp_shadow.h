/*
vp_shadow.h - the archive "shape" of a guest rootfs (VirtPass)

The guest's `/` is an Alpine minirootfs archive, and an archive entry is not a
file: a symlink has no host counterpart at all (Windows needs a privilege to
create one), a directory may be empty, and many names share one target. This
index holds that shape - path, kind, mode, size, mtime, link target, synthetic
inode - so the core can answer lstat()/readlink()/getdents64() from it while the
regular files (and only those) are materialized into a host directory.

It is a plain data structure on purpose: the core owns the lookups, a host fills
it from a tar archive (vp_rootfs.c), and nothing here depends on zlib or on the
host's file system. Entries are shared by every run in one process, so the only
mutable per-run state - whether the guest unlinked an archive-only entry - is a
flag the host clears between runs (vp_shadow_unhide_all()).

Deliberate M1 limits, so a reader does not expect more:
  - the index is built per run. A tar index parse is cheap (a few ms for a
    minirootfs) and it makes "hidden" unambiguously per-run; sharing one index
    across runs is a later optimization, not a correctness requirement.
  - a lookup never resolves a symlink: that is the core's job, because the
    resolution has to happen in the guest namespace.
*/

#ifndef VIRTPASS_VP_SHADOW_H
#define VIRTPASS_VP_SHADOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// End of a child list / "no such entry" in the uint32 index space.
#define VP_SHADOW_NONE ((uint32_t)0xFFFFFFFFu)

typedef enum {
    VP_SHADOW_DIR = 1,    // an archive directory (materialized as a real one)
    VP_SHADOW_FILE,       // an archive regular file (materialized on the host)
    VP_SHADOW_LINK,       // an archive symlink: the guest sees a link, the host nothing
    VP_SHADOW_OVERRIDE,   // the host serves @target instead of a guest path
} vp_shadow_kind_t;

typedef struct {
    char*    path;    // absolute guest path ("/", "/bin/sh"); owned by the index
    char*    target;  // symlink target, or the host path of a VP_SHADOW_OVERRIDE
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;    // permission bits only; the type comes from @kind
    uint32_t ino;     // synthetic, stable and non-zero (one per distinct path)
    uint32_t parent;  // RVVM_SHADOW_NONE for the root, else an entry index
    uint32_t child;   // first child in @parent's list, VP_SHADOW_NONE ends it
    uint32_t next;    // next sibling, VP_SHADOW_NONE ends the list
    uint8_t  kind;    // vp_shadow_kind_t
    bool     hidden;  // unlinked by the guest in this run (vp_shadow_hide)
} vp_shadow_entry_t;

typedef struct vp_shadow vp_shadow_t;

vp_shadow_t* vp_shadow_create(void);
void         vp_shadow_free(vp_shadow_t* shadow);

// Add one entry. @guest_path is normalized (absolute, "."/".." resolved, no
// trailing '/'); a duplicate path updates the existing entry in place so the
// archive's last word wins. Returns the entry index, or VP_SHADOW_NONE when the
// path is unusable (too deep, or out of memory).
//
// @target is the symlink target for VP_SHADOW_LINK and the host path for
// VP_SHADOW_OVERRIDE; it is ignored (and may be NULL) for the other kinds.
uint32_t vp_shadow_add(vp_shadow_t* shadow, const char* guest_path, vp_shadow_kind_t kind,
                       uint32_t mode, uint64_t size, int64_t mtime, const char* target);

// Resolve parent links and build the child lists. Call once, after the last add.
// Entries whose parent is not in the index stay unreachable through enumeration.
void vp_shadow_finalize(vp_shadow_t* shadow);

size_t                     vp_shadow_count(const vp_shadow_t* shadow);
const vp_shadow_entry_t*   vp_shadow_entry(const vp_shadow_t* shadow, uint32_t idx);

// Look up a guest-absolute path. The plain lookup reports a hidden entry as
// absent (which is what every syscall except unhide() wants); the raw one sees
// through it.
const vp_shadow_entry_t* vp_shadow_lookup(const vp_shadow_t* shadow, const char* guest_path);
const vp_shadow_entry_t* vp_shadow_lookup_raw(const vp_shadow_t* shadow, const char* guest_path);

// The entry index for a path, or VP_SHADOW_NONE. Enumerating a directory and
// walking a child list need indices, so this is the handle that goes with
// vp_shadow_first_child()/vp_shadow_entry().
uint32_t vp_shadow_index(const vp_shadow_t* shadow, const char* guest_path);

// Hide an entry for this run: the guest unlinked (or rmdir'd) a path the host
// never had, so the archive must stop reporting it. Returns true when it hid
// something. unhide_all() is what a new run starts from.
bool vp_shadow_hide(vp_shadow_t* shadow, const char* guest_path);
bool vp_shadow_unhide(vp_shadow_t* shadow, const char* guest_path);
void vp_shadow_unhide_all(vp_shadow_t* shadow);

// Hide by entry index rather than by path. For the recursive case: once a
// directory's index is known, its children are indices too, and a walk that
// rebuilt a path at each step would have to know which namespace those paths are
// in - the index's own, which is not the one a caller holding a root= sub-root
// writes. Indices have no such ambiguity.
bool vp_shadow_hide_index(vp_shadow_t* shadow, uint32_t idx);

// Persist the "hidden" set to @path (one guest path per line) and load what is
// already there. With a store set, every hide/unhide rewrites it, so a run that
// is killed still leaves the deletions recorded. The set is tiny - it only holds
// archive-only entries the host never materialized (symlinks, and the like) - so
// a rewrite per change is cheaper than a journal. Pass NULL to detach.
void vp_shadow_set_hidden_store(vp_shadow_t* shadow, const char* path);

// First child of @parent (finalize() must have run), VP_SHADOW_NONE at the end.
uint32_t vp_shadow_first_child(const vp_shadow_t* shadow, uint32_t parent);

// Normalize @path into an absolute guest path, writing at most @size bytes
// (including the NUL). Returns false when the result would not fit or the path
// has more components than the index supports (VP_SHADOW_MAX_SEGS).
bool vp_shadow_normalize(char* out, size_t size, const char* path);

/* The longest guest path the index accepts; the internal buffers are this size,
 * and a view's own root field is sized by it. */
#define VP_SHADOW_PATH_MAX 4096

/* --- A sub-root view of an index ---
 *
 * root= names a directory inside the rootfs that becomes the guest's "/", and the
 * index has to be read through that root or the guest walks out of it: an
 * unprefixed lookup of "/etc/resolv.conf" finds the whole filesystem's
 * resolv.conf, not the one the root makes reachable. (This was measured - the
 * first attempt applied root= only where guest paths become host paths, and a
 * guest chrooted into /sbin still read /etc/resolv.conf, because the answer came
 * from here and never reached the prefix.)
 *
 * A view is that one prefix, applied on the way *into* the index. It adds no
 * state to the index and does not copy the tree: the index already has
 * parent/child links and vp_shadow_index(), so a sub-root is a path, not a new
 * structure. The one thing a view owns is its own copy of the root string.
 *
 * Every view function takes a path in the GUEST's namespace - the one where "/"
 * is the sub-root - and looks it up in the index's own namespace. The reverse
 * direction (a guest path back out, which nothing needs) is deliberately absent:
 * an entry's `path` is the index's spelling, and a caller that reached in with
 * vp_shadow_entry() directly is deliberately opting out of the view.
 *
 * vp_shadow_view_path() is the primitive the others are built from, and it is
 * exported because symlink resolution needs it in the other direction: an
 * absolute link target is written in the guest's namespace ("/bin/busybox") and
 * has to be read in the index's ("<root>/bin/busybox"). Same join, same answer.
 *
 * @root is an absolute guest path in the index's namespace, or NULL/"" for the
 * whole index. vp_shadow_view_init() normalizes and copies it, so the caller
 * does not have to keep the string alive and a view is safely a plain field.
 */
typedef struct {
    vp_shadow_t* shadow;
    char         root[VP_SHADOW_PATH_MAX];   /* "" = the whole index */
} vp_shadow_view_t;

void vp_shadow_view_init(vp_shadow_view_t* view, vp_shadow_t* shadow, const char* root);

/* True when this view is the whole index - the case every caller can skip work
 * for, and the reason an unrooted run pays nothing for the concept. */
bool vp_shadow_view_is_whole(const vp_shadow_view_t* view);

/* @guest_path (in the guest's namespace) as the index spells it, normalized.
 * Returns false when the result would not fit, which is a caller bug rather than
 * a guest-visible error: every caller has a UAPI_PATH_MAX buffer and a path that
 * came out of the guest, so it fits by construction unless the root is absurd. */
bool vp_shadow_view_path(const vp_shadow_view_t* view, char* out, size_t size,
                         const char* guest_path);

const vp_shadow_entry_t* vp_shadow_view_lookup(const vp_shadow_view_t* view,
                                              const char* guest_path);
const vp_shadow_entry_t* vp_shadow_view_lookup_raw(const vp_shadow_view_t* view,
                                                   const char* guest_path);
uint32_t vp_shadow_view_index(const vp_shadow_view_t* view, const char* guest_path);

/* Hide/unhide through the view. Hiding takes a guest path, because that is what
 * the guest called unlink() with; the store records the index's spelling, which
 * is what a later run in the same rootfs needs to recognise it. */
bool vp_shadow_view_hide(vp_shadow_view_t* view, const char* guest_path);
bool vp_shadow_view_unhide(vp_shadow_view_t* view, const char* guest_path);

#endif
