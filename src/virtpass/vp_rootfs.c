/*
vp_rootfs.c - read a bundle archive into the guest's rootfs (VirtPass)

See vp_rootfs.h. Two passes over the same bytes, deliberately:

  - the index pass records every entry's shape (rvvm/vp_shadow.h) plus, for a
    regular file, where its content starts in the inflated archive. It is what
    the guest's lstat()/readlink()/getdents64() are answered from.
  - the extract pass writes directories and regular files under a host
    directory. Symlinks are skipped on purpose: the shadow index answers them,
    which is how one busybox covers ~300 names without a privilege and without
    300 copies.

The inflated archive is held in memory while it is being lifted out (a minirootfs
expands to ~7 MB), which is what lets a caller pull one entry out by index. Once
the tree is materialized, vp_rootfs_release_data() drops it: the guest is
answered from the shadow and the files on disk, so the ~7 MB need not stay
resident for the life of the run. (An app install opens its own vp_rootfs_t and
reads entries before closing it, so it does not rely on the rootfs keeping its
bytes.)
*/

#include "virtpass/vp_rootfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#if defined(_WIN32)
#include <direct.h>
#define vp_mkdir_one(path) _mkdir(path)
#else
#include <sys/types.h>
#define vp_mkdir_one(path) mkdir((path), 0755)
#endif

#define TAR_BLOCK_SIZE  512
#define ROOTFS_MAX_PATH 4096

#define VP_ROOTFS_NO_DATA UINT64_MAX

struct vp_rootfs {
    vp_shadow_t* shadow;
    uint8_t*     tar;
    size_t       tar_size;
    uint64_t*    data;          // content offset per entry, VP_ROOTFS_NO_DATA when none
    size_t       data_count;
    size_t       data_capacity;
};

// ---------------------------------------------------------------------------
// Archive decoding
// ---------------------------------------------------------------------------

static bool rootfs_inflate(const char* path, uint8_t** out, size_t* out_size, const char** error)
{
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        *error = "the archive could not be opened";
        return false;
    }

    size_t capacity = 1u << 20;
    size_t size = 0;
    uint8_t* buffer = malloc(capacity);
    if (!buffer) {
        gzclose(gz);
        *error = "out of memory";
        return false;
    }

    for (;;) {
        if (size == capacity) {
            size_t grown = capacity * 2;
            uint8_t* next = realloc(buffer, grown);
            if (!next) {
                free(buffer);
                gzclose(gz);
                *error = "out of memory";
                return false;
            }
            buffer = next;
            capacity = grown;
        }
        // gzread() takes an unsigned length; keep each call small so a huge
        // archive cannot overflow it.
        unsigned chunk = (unsigned)((capacity - size) > (1u << 20) ? (1u << 20) : (capacity - size));
        int got = gzread(gz, buffer + size, chunk);
        if (got < 0) {
            free(buffer);
            gzclose(gz);
            *error = "the archive is not valid gzip";
            return false;
        }
        if (got == 0) {
            break;
        }
        size += (size_t)got;
    }
    gzclose(gz);

    if (!size) {
        free(buffer);
        *error = "the archive is empty";
        return false;
    }
    *out = buffer;
    *out_size = size;
    return true;
}

// Copy a tar header field into a NUL-terminated buffer, trimming at the field
// length (tar fields are not required to be terminated).
static void rootfs_field(char* out, size_t out_size, const char* field, size_t len)
{
    size_t used = 0;
    while (used < len && field[used]) {
        used++;
    }
    if (used >= out_size) {
        used = out_size - 1;
    }
    memcpy(out, field, used);
    out[used] = 0;
}

// Read an octal field, including the GNU base-256 encoding used when a value
// does not fit in the field.
static uint64_t rootfs_number(const char* field, size_t len)
{
    if ((uint8_t)field[0] & 0x80) {
        uint64_t value = 0;
        for (size_t i = 1; i < len; ++i) {
            value = (value << 8) | (uint8_t)field[i];
        }
        return value;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = field[i];
        if (c >= '0' && c <= '7') {
            value = (value << 3) | (uint64_t)(c - '0');
        } else if (c && c != ' ') {
            break;
        } else if (value || c == 0) {
            break;
        }
    }
    return value;
}

static bool rootfs_zero_block(const uint8_t* block)
{
    for (size_t i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (block[i]) {
            return false;
        }
    }
    return true;
}

// Pull "path=" / "linkpath=" out of a pax extended header. The record format is
// "<len> <key>=<value>\n", with <len> covering the whole record.
static void rootfs_pax(const char* data, size_t size, char* path, size_t path_size,
                       char* link, size_t link_size)
{
    size_t pos = 0;
    while (pos < size) {
        size_t record_len = 0;
        size_t digits = pos;

        while (digits < size && data[digits] >= '0' && data[digits] <= '9') {
            record_len = record_len * 10 + (size_t)(data[digits] - '0');
            digits++;
        }
        if (digits >= size || data[digits] != ' ' || !record_len || pos + record_len > size) {
            break;
        }

        const char* key = data + digits + 1;
        size_t key_room = (pos + record_len) - (digits + 1);
        const char* eq = memchr(key, '=', key_room);
        if (eq) {
            size_t key_len = (size_t)(eq - key);
            const char* value = eq + 1;
            size_t value_len = (pos + record_len) - (size_t)(value - data);
            if (value_len && value[value_len - 1] == '\n') {
                value_len--;
            }
            char* dest = NULL;
            size_t dest_size = 0;
            if (key_len == 4 && !memcmp(key, "path", 4)) {
                dest = path;
                dest_size = path_size;
            } else if (key_len == 8 && !memcmp(key, "linkpath", 8)) {
                dest = link;
                dest_size = link_size;
            }
            if (dest && value_len < dest_size) {
                memcpy(dest, value, value_len);
                dest[value_len] = 0;
            }
        }
        pos += record_len;
    }
}

static bool rootfs_note_data(vp_rootfs_t* rootfs, uint32_t idx, uint64_t offset)
{
    if (idx == VP_SHADOW_NONE) {
        return false;
    }
    while (rootfs->data_count <= idx) {
        if (rootfs->data_count == rootfs->data_capacity) {
            size_t grown = rootfs->data_capacity ? rootfs->data_capacity * 2 : 256;
            uint64_t* next = realloc(rootfs->data, grown * sizeof(*next));
            if (!next) {
                return false;
            }
            rootfs->data = next;
            rootfs->data_capacity = grown;
        }
        rootfs->data[rootfs->data_count++] = VP_ROOTFS_NO_DATA;
    }
    rootfs->data[idx] = offset;
    return true;
}

static bool rootfs_parse(vp_rootfs_t* rootfs, const char** error)
{
    const uint8_t* tar = rootfs->tar;
    size_t size = rootfs->tar_size;
    size_t pos = 0;
    size_t skipped = 0;

    char long_name[ROOTFS_MAX_PATH] = {0};
    char long_link[ROOTFS_MAX_PATH] = {0};
    char pax_path[ROOTFS_MAX_PATH] = {0};
    char pax_link[ROOTFS_MAX_PATH] = {0};
    bool have_long_name = false;
    bool have_long_link = false;
    bool have_pax_path = false;
    bool have_pax_link = false;

    while (pos + TAR_BLOCK_SIZE <= size) {
        const uint8_t* header = tar + pos;
        char name[256];
        char prefix[192];
        char linkname[256];
        char effective[ROOTFS_MAX_PATH];
        char target[ROOTFS_MAX_PATH];
        const char* magic;

        if (rootfs_zero_block(header)) {
            break; // end-of-archive marker
        }

        rootfs_field(name, sizeof(name), (const char*)header, 100);
        rootfs_field(prefix, sizeof(prefix), (const char*)header + 345, 155);
        rootfs_field(linkname, sizeof(linkname), (const char*)header + 157, 100);
        magic = (const char*)header + 257;

        uint64_t bytes = rootfs_number((const char*)header + 124, 12);
        uint32_t mode = (uint32_t)(rootfs_number((const char*)header + 100, 8) & 07777);
        int64_t mtime = (int64_t)rootfs_number((const char*)header + 136, 12);
        char type = (char)header[156];
        if (type == 0) {
            type = '0';
        }

        // A truncated archive is indexed as far as it goes rather than refused:
        // the caller gets a usable rootfs and sees the shortfall in its listing.
        if (bytes > size || pos + TAR_BLOCK_SIZE + bytes > size) {
            break;
        }
        const char* content = (const char*)header + TAR_BLOCK_SIZE;

        if (have_long_name) {
            snprintf(effective, sizeof(effective), "%s", long_name);
        } else if (have_pax_path) {
            snprintf(effective, sizeof(effective), "%s", pax_path);
        } else if (prefix[0] && !memcmp(magic, "ustar", 5)) {
            snprintf(effective, sizeof(effective), "%s/%s", prefix, name);
        } else {
            snprintf(effective, sizeof(effective), "%s", name);
        }

        if (have_long_link) {
            snprintf(target, sizeof(target), "%s", long_link);
        } else if (have_pax_link) {
            snprintf(target, sizeof(target), "%s", pax_link);
        } else {
            snprintf(target, sizeof(target), "%s", linkname);
        }

        switch (type) {
            case 'L':
                rootfs_field(long_name, sizeof(long_name), content, (size_t)bytes);
                have_long_name = true;
                break;
            case 'K':
                rootfs_field(long_link, sizeof(long_link), content, (size_t)bytes);
                have_long_link = true;
                break;
            case 'x':
                rootfs_pax(content, (size_t)bytes, pax_path, sizeof(pax_path), pax_link, sizeof(pax_link));
                have_pax_path = pax_path[0] != 0;
                have_pax_link = pax_link[0] != 0;
                break;
            case 'g':
                break; // global pax header: nothing in it names an entry
            case '5': {
                uint32_t idx = vp_shadow_add(rootfs->shadow, effective, VP_SHADOW_DIR, mode, 0, mtime, NULL);
                if (idx == VP_SHADOW_NONE) {
                    *error = "the archive contains an unusable path";
                    return false;
                }
                break;
            }
            case '2': {
                uint32_t idx = vp_shadow_add(rootfs->shadow, effective, VP_SHADOW_LINK, mode,
                                             strlen(target), mtime, target);
                if (idx == VP_SHADOW_NONE) {
                    *error = "the archive contains an unusable path";
                    return false;
                }
                break;
            }
            case '0':
            case '7': {
                uint32_t idx = vp_shadow_add(rootfs->shadow, effective, VP_SHADOW_FILE, mode,
                                             bytes, mtime, NULL);
                if (idx == VP_SHADOW_NONE || !rootfs_note_data(rootfs, idx, pos + TAR_BLOCK_SIZE)) {
                    *error = "the archive contains an unusable path";
                    return false;
                }
                break;
            }
            default:
                // '1' (hardlink) shares the content of an earlier entry, and the
                // device/fifo kinds have no meaning in a userland rootfs. Both
                // are skipped rather than guessed at.
                skipped++;
                break;
        }

        if (type != 'L' && type != 'K' && type != 'x' && type != 'g') {
            have_long_name = false;
            have_long_link = false;
            have_pax_path = false;
            have_pax_link = false;
            long_name[0] = 0;
            long_link[0] = 0;
            pax_path[0] = 0;
            pax_link[0] = 0;
        }

        pos += TAR_BLOCK_SIZE + (size_t)((bytes + TAR_BLOCK_SIZE - 1) & ~(uint64_t)(TAR_BLOCK_SIZE - 1));
    }

    (void)skipped;

    if (!vp_shadow_count(rootfs->shadow)) {
        *error = "the archive holds no entries";
        return false;
    }
    if (!vp_shadow_lookup_raw(rootfs->shadow, "/")) {
        vp_shadow_add(rootfs->shadow, "/", VP_SHADOW_DIR, 0755, 0, 0, NULL);
    }
    vp_shadow_finalize(rootfs->shadow);
    return true;
}

// ---------------------------------------------------------------------------
// Host-side install
// ---------------------------------------------------------------------------

// mkdir -p for the directory part of @path (the buffer is modified in place).
static void rootfs_mkdir_parents(char* path)
{
    for (char* pos = path + 1; *pos; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            char saved = *pos;
            *pos = 0;
            vp_mkdir_one(path);
            *pos = saved;
        }
    }
}

static bool rootfs_join(char* out, size_t out_size, const char* directory, const char* guest_path)
{
    size_t dir_len = strlen(directory);
    bool need_sep = dir_len && directory[dir_len - 1] != '/' && directory[dir_len - 1] != '\\';
    size_t guest_len = strlen(guest_path);
    if (dir_len + (need_sep ? 1 : 0) + guest_len + 1 > out_size) {
        return false;
    }
    snprintf(out, out_size, "%s%s%s", directory, need_sep ? "/" : "", guest_path);
    return true;
}

vp_rootfs_t* vp_rootfs_open(const char* tar_gz_path, const char** error)
{
    const char* local_error = NULL;
    if (!error) {
        error = &local_error;
    }
    *error = NULL;

    if (!tar_gz_path) {
        *error = "no archive was named";
        return NULL;
    }

    vp_rootfs_t* rootfs = calloc(1, sizeof(vp_rootfs_t));
    if (!rootfs) {
        *error = "out of memory";
        return NULL;
    }
    rootfs->shadow = vp_shadow_create();
    if (!rootfs->shadow) {
        free(rootfs);
        *error = "out of memory";
        return NULL;
    }

    if (!rootfs_inflate(tar_gz_path, &rootfs->tar, &rootfs->tar_size, error) ||
        !rootfs_parse(rootfs, error)) {
        vp_rootfs_close(rootfs);
        return NULL;
    }
    return rootfs;
}

void vp_rootfs_close(vp_rootfs_t* rootfs)
{
    if (!rootfs) {
        return;
    }
    vp_shadow_free(rootfs->shadow);
    free(rootfs->tar);
    free(rootfs->data);
    free(rootfs);
}

/* Drop the inflated bytes and the content offsets that point into them. The
 * index (shadow) stays: the guest's lstat()/readlink()/getdents64() are answered
 * from it and from the materialized files, not from the archive. Any later
 * extract/read call then refuses (there is nothing to read from). */
void vp_rootfs_release_data(vp_rootfs_t* rootfs)
{
    if (!rootfs) {
        return;
    }
    free(rootfs->tar);
    rootfs->tar = NULL;
    rootfs->tar_size = 0;
    free(rootfs->data);
    rootfs->data = NULL;
    rootfs->data_count = 0;
    rootfs->data_capacity = 0;
}

vp_shadow_t* vp_rootfs_shadow(const vp_rootfs_t* rootfs)
{
    return rootfs ? rootfs->shadow : NULL;
}

size_t vp_rootfs_count(const vp_rootfs_t* rootfs)
{
    return rootfs ? vp_shadow_count(rootfs->shadow) : 0;
}

bool vp_rootfs_extract_to(const vp_rootfs_t* rootfs, uint32_t idx, const char* dest_path)
{
    if (!rootfs || !dest_path || idx >= rootfs->data_count) {
        return false;
    }
    const vp_shadow_entry_t* entry = vp_shadow_entry(rootfs->shadow, idx);
    if (!entry || entry->kind != VP_SHADOW_FILE) {
        return false;
    }
    uint64_t offset = rootfs->data[idx];
    if (offset == VP_ROOTFS_NO_DATA || offset + entry->size > rootfs->tar_size) {
        return false;
    }

    char parent[ROOTFS_MAX_PATH * 2];
    snprintf(parent, sizeof(parent), "%s", dest_path);
    rootfs_mkdir_parents(parent);

    // An already installed file of the same size is left alone, so a repeated
    // install is cheap and a changed bundle still lands.
    struct stat st;
    if (stat(dest_path, &st) == 0 && (uint64_t)st.st_size == entry->size) {
        return true;
    }

    FILE* file = fopen(dest_path, "wb");
    if (!file) {
        return false;
    }
    bool ok = true;
    if (entry->size && fwrite(rootfs->tar + offset, 1, (size_t)entry->size, file) != (size_t)entry->size) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

size_t vp_rootfs_extract(vp_rootfs_t* rootfs, const char* dest_dir, const char** error)
{
    const char* local_error = NULL;
    if (!error) {
        error = &local_error;
    }
    *error = NULL;

    if (!rootfs || !dest_dir) {
        *error = "no destination was named";
        return 0;
    }

    char path[ROOTFS_MAX_PATH * 2];
    size_t written = 0;
    const size_t count = vp_shadow_count(rootfs->shadow);

    for (uint32_t i = 0; i < count; ++i) {
        const vp_shadow_entry_t* entry = vp_shadow_entry(rootfs->shadow, i);
        if (!entry || entry->hidden) {
            continue;
        }
        // Symlinks and overrides are the shadow layer's business, not the host's.
        if (entry->kind != VP_SHADOW_DIR && entry->kind != VP_SHADOW_FILE) {
            continue;
        }
        if (!rootfs_join(path, sizeof(path), dest_dir, entry->path)) {
            *error = "a guest path is too long for the host destination";
            return written;
        }
        if (entry->kind == VP_SHADOW_DIR) {
            char parent[ROOTFS_MAX_PATH * 2];
            snprintf(parent, sizeof(parent), "%s", path);
            rootfs_mkdir_parents(parent);
            vp_mkdir_one(path);
            continue;
        }
        if (!vp_rootfs_extract_to(rootfs, i, path)) {
            *error = "a file from the archive could not be written";
            return written;
        }
        written++;
    }
    return written;
}

size_t vp_rootfs_read_entry(const vp_rootfs_t* rootfs, uint32_t idx, void* buffer, size_t size)
{
    const vp_shadow_entry_t* entry;
    uint64_t offset;

    if (!rootfs || !buffer || idx >= rootfs->data_count) {
        return (size_t)-1;
    }
    entry = vp_shadow_entry(rootfs->shadow, idx);
    if (!entry || entry->kind != VP_SHADOW_FILE || entry->size > size) {
        return (size_t)-1;
    }
    offset = rootfs->data[idx];
    if (offset == VP_ROOTFS_NO_DATA || offset + entry->size > rootfs->tar_size) {
        return (size_t)-1;
    }
    memcpy(buffer, rootfs->tar + offset, (size_t)entry->size);
    return (size_t)entry->size;
}
