/*
vp_zip.c - a small random-access zip reader (see vp_zip.h)
*/

#include "virtpass/vp_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define ZIP_EOCD_SIG    0x06054b50u
#define ZIP_CD_SIG      0x02014b50u
#define ZIP_LOCAL_SIG   0x04034b50u
#define ZIP_EOCD_MIN    22u
#define ZIP_COMMENT_MAX 0xFFFFu

typedef struct {
    vp_zip_entry_t pub;
    uint32_t       local_offset;  // start of this entry's local header
    uint32_t       comp_size;     // compressed size from the central directory
} vp_zip_item_t;

struct vp_zip {
    uint8_t*       data;
    size_t         size;
    vp_zip_item_t* items;
    uint32_t       count;
};

static uint16_t zip_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t zip_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Find the end-of-central-directory record: the last comment can be up to
 * 64 KiB, so scan back from the end. */
static const uint8_t* zip_find_eocd(const uint8_t* data, size_t size)
{
    size_t max_back = ZIP_COMMENT_MAX + ZIP_EOCD_MIN;
    size_t start = size > max_back ? size - max_back : 0;

    if (size < ZIP_EOCD_MIN) {
        return NULL;
    }
    for (size_t at = size - ZIP_EOCD_MIN + 1; at-- > start;) {
        if (zip_u32(data + at) == ZIP_EOCD_SIG) {
            return data + at;
        }
    }
    return NULL;
}

/* Build the entry index from the central directory. @zip->data/@zip->size must
 * already be set. */
static bool zip_index(vp_zip_t* zip, const char** error)
{
    const uint8_t* eocd;
    uint32_t       total, cd_offset, cd_size;
    size_t         at;
    uint32_t       i;

    eocd = zip_find_eocd(zip->data, zip->size);
    if (!eocd) {
        *error = "not a zip (no end-of-central-directory record)";
        return false;
    }
    total     = zip_u16(eocd + 10);
    cd_size   = zip_u32(eocd + 12);
    cd_offset = zip_u32(eocd + 16);
    if (total == 0xFFFFu || cd_offset == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        *error = "zip64 packages are not supported";
        return false;
    }
    if ((uint64_t)cd_offset + cd_size > zip->size) {
        *error = "the central directory is out of bounds";
        return false;
    }

    zip->items = calloc(total ? total : 1, sizeof(*zip->items));
    if (!zip->items) {
        *error = "out of memory";
        return false;
    }

    at = cd_offset;
    for (i = 0; i < total; i++) {
        vp_zip_item_t* item = &zip->items[i];
        uint16_t name_len, extra_len, comment_len;
        uint32_t external;
        char*    name;

        if (at + 46 > zip->size || zip_u32(zip->data + at) != ZIP_CD_SIG) {
            *error = "the central directory is malformed";
            return false;
        }
        name_len    = zip_u16(zip->data + at + 28);
        extra_len   = zip_u16(zip->data + at + 30);
        comment_len = zip_u16(zip->data + at + 32);
        external    = zip_u32(zip->data + at + 38);
        item->local_offset = zip_u32(zip->data + at + 42);
        item->comp_size    = zip_u32(zip->data + at + 20);
        item->pub.size     = zip_u32(zip->data + at + 24);
        item->pub.method   = zip_u16(zip->data + at + 10);
        item->pub.mode     = external >> 16;

        if (at + 46 + name_len + extra_len + comment_len > zip->size) {
            *error = "the central directory is malformed";
            return false;
        }
        name = malloc((size_t)name_len + 1);
        if (!name) {
            *error = "out of memory";
            return false;
        }
        memcpy(name, zip->data + at + 46, name_len);
        name[name_len] = 0;
        item->pub.name = name;
        item->pub.is_dir = (name_len && name[name_len - 1] == '/') ||
                           (item->pub.mode & 0xF000u) == VP_ZIP_S_IFDIR;
        zip->count = i + 1;   /* so close() frees what has been built so far */
        if (item->pub.size == 0xFFFFFFFFu || item->comp_size == 0xFFFFFFFFu) {
            *error = "a zip64 member is not supported";
            return false;
        }
        at += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

static vp_zip_t* zip_from_owned_data(uint8_t* data, size_t size, const char** error)
{
    vp_zip_t* zip = calloc(1, sizeof(*zip));

    if (!zip) {
        free(data);
        *error = "out of memory";
        return NULL;
    }
    zip->data = data;
    zip->size = size;
    if (!zip_index(zip, error)) {
        vp_zip_close(zip);
        return NULL;
    }
    return zip;
}

vp_zip_t* vp_zip_open(const char* path, const char** error)
{
    const char* local_error = NULL;
    uint8_t*    data;
    size_t      size;
    FILE*       f;
    long        len;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!path) {
        *error = "no package was named";
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        *error = "the package could not be opened";
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        *error = "the package could not be sized";
        return NULL;
    }
    size = (size_t)len;
    data = malloc(size ? size : 1);
    if (!data) {
        fclose(f);
        *error = "out of memory";
        return NULL;
    }
    if (size && fread(data, 1, size, f) != size) {
        free(data);
        fclose(f);
        *error = "the package could not be read";
        return NULL;
    }
    fclose(f);
    return zip_from_owned_data(data, size, error);
}

vp_zip_t* vp_zip_open_memory(const void* data, size_t size, const char** error)
{
    const char* local_error = NULL;
    uint8_t*    copy;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!data || !size) {
        *error = "no package bytes were named";
        return NULL;
    }
    copy = malloc(size);
    if (!copy) {
        *error = "out of memory";
        return NULL;
    }
    memcpy(copy, data, size);
    return zip_from_owned_data(copy, size, error);
}

void vp_zip_close(vp_zip_t* zip)
{
    if (!zip) {
        return;
    }
    for (uint32_t i = 0; i < zip->count; i++) {
        free((void*)zip->items[i].pub.name);
    }
    free(zip->items);
    free(zip->data);
    free(zip);
}

size_t vp_zip_count(const vp_zip_t* zip)
{
    return zip ? zip->count : 0;
}

const vp_zip_entry_t* vp_zip_entry(const vp_zip_t* zip, uint32_t idx)
{
    if (!zip || idx >= zip->count) {
        return NULL;
    }
    return &zip->items[idx].pub;
}

uint32_t vp_zip_find(const vp_zip_t* zip, const char* name)
{
    if (!zip || !name) {
        return VP_ZIP_NONE;
    }
    for (uint32_t i = 0; i < zip->count; i++) {
        if (!strcmp(zip->items[i].pub.name, name)) {
            return i;
        }
    }
    return VP_ZIP_NONE;
}

bool vp_zip_read(const vp_zip_t* zip, uint32_t idx, void* buf, size_t buf_size)
{
    const vp_zip_item_t* item;
    uint8_t* local;
    uint16_t name_len, extra_len;
    uint8_t* src;
    uint32_t src_len;

    if (!zip || idx >= zip->count) {
        return false;
    }
    item = &zip->items[idx];
    if (item->pub.is_dir || item->pub.size == 0) {
        return true;
    }
    if (buf_size < item->pub.size) {
        return false;
    }
    if ((uint64_t)item->local_offset + 30 > zip->size) {
        return false;
    }
    local = zip->data + item->local_offset;
    if (zip_u32(local) != ZIP_LOCAL_SIG) {
        return false;
    }
    name_len  = zip_u16(local + 26);
    extra_len = zip_u16(local + 28);
    if ((uint64_t)item->local_offset + 30 + name_len + extra_len + item->comp_size > zip->size) {
        return false;
    }
    src = local + 30 + name_len + extra_len;
    src_len = item->comp_size;

    if (item->pub.method == 0) {
        if (src_len < item->pub.size) {
            return false;
        }
        memcpy(buf, src, (size_t)item->pub.size);
        return true;
    }
    if (item->pub.method == 8) {
        z_stream zs;
        int      rc;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            return false;
        }
        zs.next_in = (Bytef*)src;
        zs.avail_in = src_len;
        zs.next_out = (Bytef*)buf;
        zs.avail_out = (uInt)item->pub.size;
        rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        return rc == Z_STREAM_END;
    }
    return false;
}