/*
vp_shadow.c - the archive "shape" of a guest rootfs (VirtPass)

See vp_shadow.h for what this is and why. Implementation notes:

  - the path -> entry map is open addressing with linear probing, keyed by the
    FNV-1a hash of the normalized path. Slots hold index + 1 so 0 means empty.
  - entries live in one growable array; a returned pointer is invalidated by the
    next vp_shadow_add(). Callers look up again rather than holding one.
  - allocation failures are reported (VP_SHADOW_NONE / false) instead of
    aborting: this runs while a host is installing its bundle, where a clean
    error beats taking the process down. Plain malloc() is used so the module
    has no dependency beyond libc.
*/

#include "virtpass/vp_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Components a single path may have; deeper paths are refused rather than
// silently truncated.
#define VP_SHADOW_MAX_SEGS 64

#define VP_SHADOW_MIN_MAP 64

struct vp_shadow {
    vp_shadow_entry_t* entries;
    uint32_t*          map;      // open addressing over entry indices + 1
    uint32_t*          tails;    // child-list tail per entry, live during finalize
    size_t             count;
    size_t             capacity;
    size_t             map_size; // power of two
    uint32_t           root;     // the "/" entry
    char*              hidden_store; // file the hidden set is kept in, or NULL
};

static uint32_t shadow_hash(const char* str)
{
    // FNV-1a
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

static void shadow_map_insert(vp_shadow_t* shadow, uint32_t idx)
{
    size_t slot = shadow_hash(shadow->entries[idx].path) & (shadow->map_size - 1);
    while (shadow->map[slot]) {
        slot = (slot + 1) & (shadow->map_size - 1);
    }
    shadow->map[slot] = idx + 1;
}

static bool shadow_map_grow(vp_shadow_t* shadow)
{
    size_t new_size = shadow->map_size ? shadow->map_size * 2 : VP_SHADOW_MIN_MAP;
    uint32_t* map = calloc(new_size, sizeof(uint32_t));
    if (!map) {
        return false;
    }
    uint32_t* old = shadow->map;
    size_t old_size = shadow->map_size;
    shadow->map = map;
    shadow->map_size = new_size;
    for (size_t i = 0; i < old_size; ++i) {
        if (old[i]) {
            shadow_map_insert(shadow, old[i] - 1);
        }
    }
    free(old);
    return true;
}

static uint32_t shadow_map_find(const vp_shadow_t* shadow, const char* path)
{
    if (!shadow->map_size) {
        return VP_SHADOW_NONE;
    }
    size_t slot = shadow_hash(path) & (shadow->map_size - 1);
    while (shadow->map[slot]) {
        uint32_t idx = shadow->map[slot] - 1;
        if (!strcmp(shadow->entries[idx].path, path)) {
            return idx;
        }
        slot = (slot + 1) & (shadow->map_size - 1);
    }
    return VP_SHADOW_NONE;
}

// Parent path of an absolute normalized path, in place ("/" stays "/", "/bin"
// becomes "/", "/bin/sh" becomes "/bin").
static void shadow_dirname(char* path)
{
    char* slash = strrchr(path, '/');
    if (slash && slash != path) {
        *slash = 0;
    } else {
        path[1] = 0;
    }
}

bool vp_shadow_normalize(char* out, size_t size, const char* path)
{
    const char* segs[VP_SHADOW_MAX_SEGS];
    size_t lens[VP_SHADOW_MAX_SEGS];
    size_t count = 0;
    const char* pos = path ? path : "";

    while (*pos) {
        const char* seg;
        size_t len;

        while (*pos == '/') {
            pos++;
        }
        if (!*pos) {
            break;
        }
        seg = pos;
        while (*pos && *pos != '/') {
            pos++;
        }
        len = (size_t)(pos - seg);

        if (len == 1 && seg[0] == '.') {
            continue;
        }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (count) {
                count--;
            }
            continue;
        }
        if (count == VP_SHADOW_MAX_SEGS) {
            return false;
        }
        segs[count] = seg;
        lens[count] = len;
        count++;
    }

    if (size < 2) {
        return false;
    }
    size_t used = 0;
    out[used++] = '/';
    for (size_t i = 0; i < count; ++i) {
        if (i) {
            if (used + 1 >= size) {
                return false;
            }
            out[used++] = '/';
        }
        if (used + lens[i] >= size) {
            return false;
        }
        memcpy(out + used, segs[i], lens[i]);
        used += lens[i];
    }
    out[used] = 0;
    return true;
}

vp_shadow_t* vp_shadow_create(void)
{
    vp_shadow_t* shadow = calloc(1, sizeof(vp_shadow_t));
    if (shadow) {
        shadow->root = VP_SHADOW_NONE;
    }
    return shadow;
}

void vp_shadow_free(vp_shadow_t* shadow)
{
    if (!shadow) {
        return;
    }
    for (size_t i = 0; i < shadow->count; ++i) {
        free(shadow->entries[i].path);
        free(shadow->entries[i].target);
    }
    free(shadow->entries);
    free(shadow->map);
    free(shadow->tails);
    free(shadow->hidden_store);
    free(shadow);
}

/* Rewrite the hidden set to its store, through a temp file so a crash cannot
 * leave a half-written list. */
static void shadow_hidden_store_save(vp_shadow_t* shadow)
{
    char*  tmp;
    size_t len;
    FILE*  f;

    if (!shadow || !shadow->hidden_store) {
        return;
    }
    len = strlen(shadow->hidden_store);
    tmp = malloc(len + 8);
    if (!tmp) {
        return;
    }
    snprintf(tmp, len + 8, "%s.tmp", shadow->hidden_store);
    f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return;
    }
    for (size_t i = 0; i < shadow->count; ++i) {
        if (shadow->entries[i].hidden && shadow->entries[i].path) {
            fprintf(f, "%s\n", shadow->entries[i].path);
        }
    }
    fclose(f);
    remove(shadow->hidden_store);
    if (rename(tmp, shadow->hidden_store) != 0) {
        remove(tmp);
    }
    free(tmp);
}

void vp_shadow_set_hidden_store(vp_shadow_t* shadow, const char* path)
{
    FILE* f;
    char  line[4096];

    if (!shadow) {
        return;
    }
    free(shadow->hidden_store);
    shadow->hidden_store = NULL;
    if (!path || !*path) {
        return;
    }
    shadow->hidden_store = malloc(strlen(path) + 1);
    if (!shadow->hidden_store) {
        return;
    }
    strcpy(shadow->hidden_store, path);

    f = fopen(path, "rb");
    if (!f) {
        return;   // nothing recorded yet
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        uint32_t idx;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        if (!n) {
            continue;
        }
        idx = vp_shadow_index(shadow, line);
        if (idx != VP_SHADOW_NONE) {
            shadow->entries[idx].hidden = true;
        }
    }
    fclose(f);
}

size_t vp_shadow_count(const vp_shadow_t* shadow)
{
    return shadow ? shadow->count : 0;
}

const vp_shadow_entry_t* vp_shadow_entry(const vp_shadow_t* shadow, uint32_t idx)
{
    if (!shadow || idx >= shadow->count) {
        return NULL;
    }
    return &shadow->entries[idx];
}

static uint32_t shadow_append(vp_shadow_t* shadow, const char* path, vp_shadow_kind_t kind,
                              uint32_t mode, uint64_t size, int64_t mtime, const char* target)
{
    if (shadow->count == shadow->capacity) {
        size_t capacity = shadow->capacity ? shadow->capacity * 2 : 64;
        vp_shadow_entry_t* entries = realloc(shadow->entries, capacity * sizeof(*entries));
        if (!entries) {
            return VP_SHADOW_NONE;
        }
        shadow->entries = entries;
        shadow->capacity = capacity;
    }
    // Keep the table at most 3/4 full, or probing degrades badly.
    if (!shadow->map_size || (shadow->count + 1) * 4 >= shadow->map_size * 3) {
        if (!shadow_map_grow(shadow)) {
            return VP_SHADOW_NONE;
        }
    }

    size_t path_len = strlen(path) + 1;
    char* path_copy = malloc(path_len);
    if (!path_copy) {
        return VP_SHADOW_NONE;
    }
    memcpy(path_copy, path, path_len);

    char* target_copy = NULL;
    if (target && *target) {
        size_t target_len = strlen(target) + 1;
        target_copy = malloc(target_len);
        if (!target_copy) {
            free(path_copy);
            return VP_SHADOW_NONE;
        }
        memcpy(target_copy, target, target_len);
    }

    uint32_t idx = (uint32_t)shadow->count;
    vp_shadow_entry_t* entry = &shadow->entries[idx];
    memset(entry, 0, sizeof(*entry));
    entry->path = path_copy;
    entry->target = target_copy;
    entry->kind = (uint8_t)kind;
    entry->mode = mode;
    entry->size = size;
    entry->mtime = mtime;
    entry->ino = idx + 1;
    entry->parent = VP_SHADOW_NONE;
    entry->child = VP_SHADOW_NONE;
    entry->next = VP_SHADOW_NONE;
    shadow->count++;
    shadow_map_insert(shadow, idx);
    if (!strcmp(path, "/")) {
        shadow->root = idx;
    }
    return idx;
}

uint32_t vp_shadow_add(vp_shadow_t* shadow, const char* guest_path, vp_shadow_kind_t kind,
                       uint32_t mode, uint64_t size, int64_t mtime, const char* target)
{
    if (!shadow) {
        return VP_SHADOW_NONE;
    }
    char path[4096];
    if (!vp_shadow_normalize(path, sizeof(path), guest_path)) {
        return VP_SHADOW_NONE;
    }

    uint32_t idx = shadow_map_find(shadow, path);
    if (idx != VP_SHADOW_NONE) {
        // The archive's last word about a path wins.
        vp_shadow_entry_t* entry = &shadow->entries[idx];
        if (target && *target && (!entry->target || strcmp(entry->target, target))) {
            size_t target_len = strlen(target) + 1;
            char* target_copy = realloc(entry->target, target_len);
            if (!target_copy) {
                return VP_SHADOW_NONE;
            }
            memcpy(target_copy, target, target_len);
            entry->target = target_copy;
        } else if (!target || !*target) {
            free(entry->target);
            entry->target = NULL;
        }
        entry->kind = (uint8_t)kind;
        entry->mode = mode;
        entry->size = size;
        entry->mtime = mtime;
        return idx;
    }
    return shadow_append(shadow, path, kind, mode, size, mtime, target);
}

void vp_shadow_finalize(vp_shadow_t* shadow)
{
    if (!shadow || !shadow->count) {
        return;
    }
    free(shadow->tails);
    shadow->tails = malloc(shadow->count * sizeof(uint32_t));
    if (!shadow->tails) {
        return;
    }

    for (size_t i = 0; i < shadow->count; ++i) {
        vp_shadow_entry_t* entry = &shadow->entries[i];
        char parent_path[4096];
        entry->parent = VP_SHADOW_NONE;
        entry->child = VP_SHADOW_NONE;
        entry->next = VP_SHADOW_NONE;
        shadow->tails[i] = VP_SHADOW_NONE;

        if (i == shadow->root) {
            continue;
        }
        size_t path_len = strlen(entry->path);
        if (path_len >= sizeof(parent_path)) {
            continue;
        }
        memcpy(parent_path, entry->path, path_len + 1);
        shadow_dirname(parent_path);
        entry->parent = shadow_map_find(shadow, parent_path);
    }

    for (size_t i = 0; i < shadow->count; ++i) {
        uint32_t parent = shadow->entries[i].parent;
        if (parent == VP_SHADOW_NONE) {
            continue;
        }
        if (shadow->tails[parent] == VP_SHADOW_NONE) {
            shadow->entries[parent].child = (uint32_t)i;
        } else {
            shadow->entries[shadow->tails[parent]].next = (uint32_t)i;
        }
        shadow->tails[parent] = (uint32_t)i;
    }

    free(shadow->tails);
    shadow->tails = NULL;
}

const vp_shadow_entry_t* vp_shadow_lookup_raw(const vp_shadow_t* shadow, const char* guest_path)
{
    if (!shadow) {
        return NULL;
    }
    char path[4096];
    if (!vp_shadow_normalize(path, sizeof(path), guest_path)) {
        return NULL;
    }
    uint32_t idx = shadow_map_find(shadow, path);
    if (idx == VP_SHADOW_NONE) {
        return NULL;
    }
    return &shadow->entries[idx];
}

const vp_shadow_entry_t* vp_shadow_lookup(const vp_shadow_t* shadow, const char* guest_path)
{
    const vp_shadow_entry_t* entry = vp_shadow_lookup_raw(shadow, guest_path);
    return (entry && !entry->hidden) ? entry : NULL;
}

uint32_t vp_shadow_index(const vp_shadow_t* shadow, const char* guest_path)
{
    if (!shadow) {
        return VP_SHADOW_NONE;
    }
    char path[4096];
    if (!vp_shadow_normalize(path, sizeof(path), guest_path)) {
        return VP_SHADOW_NONE;
    }
    return shadow_map_find(shadow, path);
}

bool vp_shadow_hide(vp_shadow_t* shadow, const char* guest_path)
{
    uint32_t idx = vp_shadow_index(shadow, guest_path);
    if (!shadow || idx == VP_SHADOW_NONE || shadow->entries[idx].hidden) {
        return false;
    }
    shadow->entries[idx].hidden = true;
    shadow_hidden_store_save(shadow);
    return true;
}

bool vp_shadow_hide_index(vp_shadow_t* shadow, uint32_t idx)
{
    if (!shadow || idx >= shadow->count || shadow->entries[idx].hidden) {
        return false;
    }
    shadow->entries[idx].hidden = true;
    shadow_hidden_store_save(shadow);
    return true;
}

bool vp_shadow_unhide(vp_shadow_t* shadow, const char* guest_path)
{
    uint32_t idx = vp_shadow_index(shadow, guest_path);
    if (!shadow || idx == VP_SHADOW_NONE || !shadow->entries[idx].hidden) {
        return false;
    }
    shadow->entries[idx].hidden = false;
    shadow_hidden_store_save(shadow);
    return true;
}

void vp_shadow_unhide_all(vp_shadow_t* shadow)
{
    if (!shadow) {
        return;
    }
    for (size_t i = 0; i < shadow->count; ++i) {
        shadow->entries[i].hidden = false;
    }
}

uint32_t vp_shadow_first_child(const vp_shadow_t* shadow, uint32_t parent)
{
    if (!shadow || parent >= shadow->count) {
        return VP_SHADOW_NONE;
    }
    return shadow->entries[parent].child;
}

/* ===================== sub-root views =====================
 *
 * Every function here is the plain one plus one prefix, which is the whole of it:
 * the index already stores absolute guest paths in a hash map, so a sub-root is
 * a string to put in front of a lookup rather than a second structure to keep in
 * step. What that buys is that the unrooted case costs nothing - the view is
 * whole, the join is a copy of the caller's own path, and no call site has to
 * branch.
 */

void vp_shadow_view_init(vp_shadow_view_t* view, vp_shadow_t* shadow, const char* root)
{
    if (!view) {
        return;
    }
    view->shadow = shadow;
    view->root[0] = '\0';
    /* Normalized once, here, because every later call trusts it: a root of "/"
     * and a root of "" must mean the same thing, and deciding that per lookup
     * would be one more comparison on the path a stat() took. A root that will
     * not normalize is dropped rather than refused - a view with no root is the
     * whole index, which is what a caller that named something unusable gets,
     * and the command line that asked for it is still readable in
     * /proc/cmdline rather than becoming a silent narrower namespace. */
    if (root && root[0] && strcmp(root, "/") != 0) {
        if (!vp_shadow_normalize(view->root, sizeof(view->root), root)) {
            view->root[0] = '\0';
        }
    }
}

bool vp_shadow_view_is_whole(const vp_shadow_view_t* view)
{
    return !view || !view->shadow || !view->root[0];
}

bool vp_shadow_view_path(const vp_shadow_view_t* view, char* out, size_t size,
                         const char* guest_path)
{
    char   guest[VP_SHADOW_PATH_MAX];
    size_t root_len;
    size_t guest_len;
    size_t at;

    if (!out || size == 0) {
        return false;
    }
    if (vp_shadow_view_is_whole(view)) {
        return vp_shadow_normalize(out, size, guest_path);
    }
    /* Normalized before the join, not after: a guest path reaching above the
     * root ("/../etc") has to lose the ".." at the root rather than be clamped
     * afterwards, and normalizing first is what makes ".." mean what it means
     * everywhere else in the guest. */
    if (!vp_shadow_normalize(guest, sizeof(guest), guest_path ? guest_path : "/")) {
        return false;
    }
    root_len  = strlen(view->root);
    guest_len = strlen(guest);
    /* One separator, always needed and never doubled: the root is normalized so
     * it carries no trailing '/', and @guest is absolute so it brings none. */
    at = root_len + 1 + guest_len;
    if (at + 1 > size) {
        return false;
    }
    memcpy(out, view->root, root_len);
    out[root_len] = '/';
    /* guest + 1 skips the guest path's own leading '/', which the separator above
     * has just replaced. */
    memcpy(out + root_len + 1, guest + 1, guest_len);
    out[at] = '\0';
    return true;
}

const vp_shadow_entry_t* vp_shadow_view_lookup_raw(const vp_shadow_view_t* view,
                                                   const char* guest_path)
{
    char path[VP_SHADOW_PATH_MAX];

    if (vp_shadow_view_is_whole(view)) {
        return vp_shadow_lookup_raw(view ? view->shadow : NULL, guest_path);
    }
    if (!vp_shadow_view_path(view, path, sizeof(path), guest_path)) {
        return NULL;
    }
    return vp_shadow_lookup_raw(view->shadow, path);
}

const vp_shadow_entry_t* vp_shadow_view_lookup(const vp_shadow_view_t* view,
                                              const char* guest_path)
{
    const vp_shadow_entry_t* entry = vp_shadow_view_lookup_raw(view, guest_path);
    return (entry && !entry->hidden) ? entry : NULL;
}

uint32_t vp_shadow_view_index(const vp_shadow_view_t* view, const char* guest_path)
{
    char path[VP_SHADOW_PATH_MAX];

    if (vp_shadow_view_is_whole(view)) {
        return vp_shadow_index(view ? view->shadow : NULL, guest_path);
    }
    if (!vp_shadow_view_path(view, path, sizeof(path), guest_path)) {
        return VP_SHADOW_NONE;
    }
    return vp_shadow_index(view->shadow, path);
}

bool vp_shadow_view_hide(vp_shadow_view_t* view, const char* guest_path)
{
    char path[VP_SHADOW_PATH_MAX];

    if (vp_shadow_view_is_whole(view)) {
        return vp_shadow_hide(view ? view->shadow : NULL, guest_path);
    }
    if (!vp_shadow_view_path(view, path, sizeof(path), guest_path)) {
        return false;
    }
    return vp_shadow_hide(view->shadow, path);
}

bool vp_shadow_view_unhide(vp_shadow_view_t* view, const char* guest_path)
{
    char path[VP_SHADOW_PATH_MAX];

    if (vp_shadow_view_is_whole(view)) {
        return vp_shadow_unhide(view ? view->shadow : NULL, guest_path);
    }
    if (!vp_shadow_view_path(view, path, sizeof(path), guest_path)) {
        return false;
    }
    return vp_shadow_unhide(view->shadow, path);
}
