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

// First child of @parent (finalize() must have run), VP_SHADOW_NONE at the end.
uint32_t vp_shadow_first_child(const vp_shadow_t* shadow, uint32_t parent);

// Normalize @path into an absolute guest path, writing at most @size bytes
// (including the NUL). Returns false when the result would not fit or the path
// has more components than the index supports (VP_SHADOW_MAX_SEGS).
bool vp_shadow_normalize(char* out, size_t size, const char* path);

#endif
