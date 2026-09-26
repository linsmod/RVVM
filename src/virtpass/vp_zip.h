/*
vp_zip.h - a small random-access zip reader (VirtPass)

A `.vapp` is an APK-shaped zip: the package's files are zip entries, and the
central directory - not a stream - is what makes them addressable. That is the
whole reason a package is a zip and not a tar.gz: a reader can list the members
and pull one (the manifest, one payload) without inflating the rest.

This module is deliberately tiny and host-neutral: it reads methods 0 (store)
and 8 (deflate) through zlib, keeps the Unix mode bits a packer stored in the
external attributes, and reports directories. Zip64 is not supported - a `.vapp`
is a few megabytes, and a member that needs it is reported as an error rather
than silently mis-addressed. Symlinks are reported as regular entries (a `.vapp`
payload is directories and regular files); a reader that cares can look at
entry->mode.

The header lives with the other host-side VirtPass modules, not under
include/virtpass: nothing on the guest side of the ABI sees a zip.
*/
#ifndef VIRTPASS_VP_ZIP_H
#define VIRTPASS_VP_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VP_ZIP_NONE ((uint32_t)0xFFFFFFFFu)

// Unix file-type bits, as they appear in an entry's external attributes.
#define VP_ZIP_S_IFDIR 0x4000u
#define VP_ZIP_S_IFLNK 0xA000u

typedef struct {
    const char* name;    // entry name, '/'-separated; owned by the reader
    uint64_t    size;    // uncompressed size
    uint16_t    method;  // 0 store, 8 deflate
    uint32_t    mode;    // Unix mode from the external attributes (0 when absent)
    bool        is_dir;
} vp_zip_entry_t;

typedef struct vp_zip vp_zip_t;

// Open and index a zip by path. The whole file is read into memory (a `.vapp`
// is small), so the entry data can be pulled by index afterwards. NULL with
// *error set when the file cannot be read or is not a usable zip.
vp_zip_t* vp_zip_open(const char* path, const char** error);

// Open and index a zip held in memory - how an app package is read out of the
// bundle's apps.tar.gz without a scratch file. @data is copied, so the caller's
// buffer need not outlive the call. NULL with *error set on a bad zip.
vp_zip_t* vp_zip_open_memory(const void* data, size_t size, const char** error);

void      vp_zip_close(vp_zip_t* zip);

size_t                vp_zip_count(const vp_zip_t* zip);
const vp_zip_entry_t* vp_zip_entry(const vp_zip_t* zip, uint32_t idx);

// The index of the entry named @name exactly, or VP_ZIP_NONE.
uint32_t vp_zip_find(const vp_zip_t* zip, const char* name);

// Read entry @idx in full into @buf, which must be at least entry->size bytes.
// A directory has no bytes and reads as an empty buffer. False on a bad offset,
// an unsupported method, or an inflate error.
bool vp_zip_read(const vp_zip_t* zip, uint32_t idx, void* buf, size_t buf_size);

#endif /* VIRTPASS_VP_ZIP_H */