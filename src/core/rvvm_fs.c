/*
rvvm_fs.c - the two providers, behind the one shape

See rvvm_fs.h for what the operations are and why the list is exactly this long.
This file is the adapters and nothing else: each slot calls the provider that
implements it and brings the answer back in this layer's words.

The conversion is a cast, which is only honest because the three enums are
deliberately the same numbers - and that is a claim about three files that live
apart, so it is pinned here rather than trusted. If a provider ever renumbers,
this stops compiling instead of silently answering a guest the wrong errno.
*/

#include "core/rvvm_fs.h"

#include <stdio.h>
#include <string.h>

/* Compared as ints: they are three different enum types, and comparing those is a
 * diagnostic rather than an error, which would put eight warnings in front of the
 * one thing they exist to say. */
#define RVVM_FS_CODES_AGREE(a, b) ((int)(a) == (int)(b))

typedef char rvvm_fs_codes_agree_enoent_m[RVVM_FS_CODES_AGREE(RVVM_FS_ENOENT, RVVM_MEMFS_ENOENT) ? 1 : -1];
typedef char rvvm_fs_codes_agree_enoent_h[RVVM_FS_CODES_AGREE(RVVM_FS_ENOENT, RVVM_HOSTFS_ENOENT) ? 1 : -1];
typedef char rvvm_fs_codes_agree_erofs_m[RVVM_FS_CODES_AGREE(RVVM_FS_EROFS, RVVM_MEMFS_EROFS) ? 1 : -1];
typedef char rvvm_fs_codes_agree_erofs_h[RVVM_FS_CODES_AGREE(RVVM_FS_EROFS, RVVM_HOSTFS_EROFS) ? 1 : -1];
typedef char rvvm_fs_codes_agree_exdev_m[RVVM_FS_CODES_AGREE(RVVM_FS_EXDEV, RVVM_MEMFS_EXDEV) ? 1 : -1];
typedef char rvvm_fs_codes_agree_exdev_h[RVVM_FS_CODES_AGREE(RVVM_FS_EXDEV, RVVM_HOSTFS_EXDEV) ? 1 : -1];
typedef char rvvm_fs_codes_agree_enospc_m[RVVM_FS_CODES_AGREE(RVVM_FS_ENOSPC, RVVM_MEMFS_ENOSPC) ? 1 : -1];
typedef char rvvm_fs_codes_agree_enospc_h[RVVM_FS_CODES_AGREE(RVVM_FS_ENOSPC, RVVM_HOSTFS_ENOSPC) ? 1 : -1];

static void fs_info_from_memfs(rvvm_fs_info_t* out, const rvvm_memfs_info_t* in)
{
    snprintf(out->path, sizeof(out->path), "%s", in->path);
    out->kind  = in->kind;
    out->mode  = in->mode;
    out->size  = in->size;
    out->ino   = in->ino;
    out->nlink = in->nlink;
    out->mtime = in->mtime;
}

static void fs_info_from_hostfs(rvvm_fs_info_t* out, const rvvm_hostfs_info_t* in)
{
    snprintf(out->path, sizeof(out->path), "%s", in->path);
    out->kind  = in->kind;
    out->mode  = in->mode;
    out->size  = in->size;
    out->ino   = in->ino;
    out->nlink = in->nlink;
    out->mtime = in->mtime;
}

/* --- memfs -------------------------------------------------------------- */

static bool fs_memfs_read_only(void* priv)
{
    return rvvm_memfs_read_only((rvvm_memfs_t*)priv);
}

static void fs_memfs_set_read_only(void* priv, bool read_only)
{
    rvvm_memfs_set_read_only((rvvm_memfs_t*)priv, read_only);
}

static rvvm_fs_result_t fs_memfs_stat(void* priv, const char* rel, bool follow,
                                      rvvm_fs_info_t* out)
{
    rvvm_memfs_info_t   mi;
    rvvm_memfs_result_t rc = rvvm_memfs_stat((rvvm_memfs_t*)priv, rel, follow, &mi);

    if (rc == RVVM_MEMFS_OK) {
        fs_info_from_memfs(out, &mi);
    }
    return (rvvm_fs_result_t)rc;
}

static rvvm_fs_result_t fs_memfs_isdir(void* priv, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_memfs_isdir((rvvm_memfs_t*)priv, rel);
}

static rvvm_fs_result_t fs_memfs_mkdir(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_memfs_mkdir((rvvm_memfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_memfs_create_file(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_memfs_create_file((rvvm_memfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_memfs_unlink(void* priv, const char* rel, bool dir)
{
    return (rvvm_fs_result_t)rvvm_memfs_unlink((rvvm_memfs_t*)priv, rel, dir);
}

static rvvm_fs_result_t fs_memfs_rename(void* priv, const char* from, const char* to)
{
    return (rvvm_fs_result_t)rvvm_memfs_rename((rvvm_memfs_t*)priv, from, to);
}

static rvvm_fs_result_t fs_memfs_link(void* priv, const char* from, const char* to)
{
    return (rvvm_fs_result_t)rvvm_memfs_link((rvvm_memfs_t*)priv, from, to);
}

static rvvm_fs_result_t fs_memfs_symlink(void* priv, const char* target, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_memfs_symlink((rvvm_memfs_t*)priv, target, rel);
}

static rvvm_fs_result_t fs_memfs_readlink(void* priv, const char* rel, char* buf, size_t size)
{
    return (rvvm_fs_result_t)rvvm_memfs_readlink((rvvm_memfs_t*)priv, rel, buf, size);
}

static rvvm_fs_result_t fs_memfs_truncate(void* priv, const char* rel, uint64_t size)
{
    return (rvvm_fs_result_t)rvvm_memfs_truncate((rvvm_memfs_t*)priv, rel, size);
}

static rvvm_fs_result_t fs_memfs_chmod(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_memfs_chmod((rvvm_memfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_memfs_touch(void* priv, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_memfs_touch((rvvm_memfs_t*)priv, rel);
}

static rvvm_fs_result_t fs_memfs_read(void* priv, const char* rel, uint64_t off,
                                      void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_memfs_read((rvvm_memfs_t*)priv, rel, off, buf, count, done);
}

static rvvm_fs_result_t fs_memfs_write(void* priv, const char* rel, uint64_t off,
                                       const void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_memfs_write((rvvm_memfs_t*)priv, rel, off, buf, count, done);
}

/* The one slot the two providers spell differently, and the adapter is where that
 * is absorbed. memfs pins by inode and hostfs opens by name, because on a host the
 * thing that keeps a file alive after its last name is gone is the open
 * descriptor - there is no number to pin. So the slot takes a name, and memfs
 * resolves it first.
 *
 * @writable is not passed on: memfs has no read-only descriptors, and asking for
 * one is not an error a guest should see. */
static rvvm_fs_result_t fs_memfs_pin(void* priv, const char* rel, bool writable, uint32_t* out_ino)
{
    rvvm_memfs_t*        fs = (rvvm_memfs_t*)priv;
    rvvm_memfs_info_t    info;
    rvvm_memfs_result_t  rc = rvvm_memfs_stat(fs, rel, true, &info);

    (void)writable;
    if (rc != RVVM_MEMFS_OK) {
        return (rvvm_fs_result_t)rc;
    }
    rc = rvvm_memfs_pin(fs, info.index);
    if (rc == RVVM_MEMFS_OK && out_ino) {
        *out_ino = info.index;
    }
    return (rvvm_fs_result_t)rc;
}

static void fs_memfs_unpin(void* priv, uint32_t ino)
{
    rvvm_memfs_unpin((rvvm_memfs_t*)priv, ino);
}

static rvvm_fs_result_t fs_memfs_read_at(void* priv, uint32_t ino, uint64_t off,
                                         void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_memfs_read_at((rvvm_memfs_t*)priv, ino, off, buf, count, done);
}

static rvvm_fs_result_t fs_memfs_write_at(void* priv, uint32_t ino, uint64_t off,
                                          const void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_memfs_write_at((rvvm_memfs_t*)priv, ino, off, buf, count, done);
}

static rvvm_fs_result_t fs_memfs_truncate_ino(void* priv, uint32_t ino, uint64_t size)
{
    return (rvvm_fs_result_t)rvvm_memfs_truncate_ino((rvvm_memfs_t*)priv, ino, size);
}

static rvvm_fs_result_t fs_memfs_stat_ino(void* priv, uint32_t ino, rvvm_fs_info_t* out)
{
    rvvm_memfs_info_t   mi;
    rvvm_memfs_result_t rc = rvvm_memfs_stat_ino((rvvm_memfs_t*)priv, ino, &mi);

    if (rc == RVVM_MEMFS_OK) {
        fs_info_from_memfs(out, &mi);
    }
    return (rvvm_fs_result_t)rc;
}

static rvvm_fs_result_t fs_memfs_getdents(void* priv, const char* rel, uint32_t* pos,
                                          char* out_name, size_t size,
                                          uint8_t* out_kind, uint64_t* out_ino)
{
    return (rvvm_fs_result_t)rvvm_memfs_getdents((rvvm_memfs_t*)priv, rel, pos,
                                                 out_name, size, out_kind, out_ino);
}

static rvvm_fs_result_t fs_memfs_getdents_ino(void* priv, uint32_t ino, uint32_t* pos,
                                              char* out_name, size_t size,
                                              uint8_t* out_kind, uint64_t* out_ino)
{
    return (rvvm_fs_result_t)rvvm_memfs_getdents_ino((rvvm_memfs_t*)priv, ino, pos,
                                                     out_name, size, out_kind, out_ino);
}

/* --- hostfs ------------------------------------------------------------- */

static bool fs_hostfs_read_only(void* priv)
{
    return rvvm_hostfs_read_only((rvvm_hostfs_t*)priv);
}

static void fs_hostfs_set_read_only(void* priv, bool read_only)
{
    rvvm_hostfs_set_read_only((rvvm_hostfs_t*)priv, read_only);
}

static rvvm_fs_result_t fs_hostfs_stat(void* priv, const char* rel, bool follow,
                                       rvvm_fs_info_t* out)
{
    rvvm_hostfs_info_t   hi;
    rvvm_hostfs_result_t rc = rvvm_hostfs_stat((rvvm_hostfs_t*)priv, rel, follow, &hi);

    if (rc == RVVM_HOSTFS_OK) {
        fs_info_from_hostfs(out, &hi);
    }
    return (rvvm_fs_result_t)rc;
}

static rvvm_fs_result_t fs_hostfs_isdir(void* priv, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_hostfs_isdir((rvvm_hostfs_t*)priv, rel);
}

static rvvm_fs_result_t fs_hostfs_mkdir(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_hostfs_mkdir((rvvm_hostfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_hostfs_create_file(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_hostfs_create_file((rvvm_hostfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_hostfs_unlink(void* priv, const char* rel, bool dir)
{
    return (rvvm_fs_result_t)rvvm_hostfs_unlink((rvvm_hostfs_t*)priv, rel, dir);
}

static rvvm_fs_result_t fs_hostfs_rename(void* priv, const char* from, const char* to)
{
    return (rvvm_fs_result_t)rvvm_hostfs_rename((rvvm_hostfs_t*)priv, from, to);
}

static rvvm_fs_result_t fs_hostfs_link(void* priv, const char* from, const char* to)
{
    return (rvvm_fs_result_t)rvvm_hostfs_link((rvvm_hostfs_t*)priv, from, to);
}

static rvvm_fs_result_t fs_hostfs_symlink(void* priv, const char* target, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_hostfs_symlink((rvvm_hostfs_t*)priv, target, rel);
}

static rvvm_fs_result_t fs_hostfs_readlink(void* priv, const char* rel, char* buf, size_t size)
{
    return (rvvm_fs_result_t)rvvm_hostfs_readlink((rvvm_hostfs_t*)priv, rel, buf, size);
}

static rvvm_fs_result_t fs_hostfs_truncate(void* priv, const char* rel, uint64_t size)
{
    return (rvvm_fs_result_t)rvvm_hostfs_truncate((rvvm_hostfs_t*)priv, rel, size);
}

static rvvm_fs_result_t fs_hostfs_chmod(void* priv, const char* rel, uint32_t mode)
{
    return (rvvm_fs_result_t)rvvm_hostfs_chmod((rvvm_hostfs_t*)priv, rel, mode);
}

static rvvm_fs_result_t fs_hostfs_touch(void* priv, const char* rel)
{
    return (rvvm_fs_result_t)rvvm_hostfs_touch((rvvm_hostfs_t*)priv, rel);
}

static rvvm_fs_result_t fs_hostfs_read(void* priv, const char* rel, uint64_t off,
                                       void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_hostfs_read((rvvm_hostfs_t*)priv, rel, off, buf, count, done);
}

static rvvm_fs_result_t fs_hostfs_write(void* priv, const char* rel, uint64_t off,
                                        const void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_hostfs_write((rvvm_hostfs_t*)priv, rel, off, buf, count, done);
}

static rvvm_fs_result_t fs_hostfs_pin(void* priv, const char* rel, bool writable, uint32_t* out_ino)
{
    return (rvvm_fs_result_t)rvvm_hostfs_pin((rvvm_hostfs_t*)priv, rel, writable, out_ino);
}

static void fs_hostfs_unpin(void* priv, uint32_t ino)
{
    rvvm_hostfs_unpin((rvvm_hostfs_t*)priv, ino);
}

static rvvm_fs_result_t fs_hostfs_read_at(void* priv, uint32_t ino, uint64_t off,
                                          void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_hostfs_read_at((rvvm_hostfs_t*)priv, ino, off, buf, count, done);
}

static rvvm_fs_result_t fs_hostfs_write_at(void* priv, uint32_t ino, uint64_t off,
                                           const void* buf, size_t count, size_t* done)
{
    return (rvvm_fs_result_t)rvvm_hostfs_write_at((rvvm_hostfs_t*)priv, ino, off, buf, count, done);
}

static rvvm_fs_result_t fs_hostfs_truncate_ino(void* priv, uint32_t ino, uint64_t size)
{
    return (rvvm_fs_result_t)rvvm_hostfs_truncate_ino((rvvm_hostfs_t*)priv, ino, size);
}

static rvvm_fs_result_t fs_hostfs_stat_ino(void* priv, uint32_t ino, rvvm_fs_info_t* out)
{
    rvvm_hostfs_info_t   hi;
    rvvm_hostfs_result_t rc = rvvm_hostfs_stat_ino((rvvm_hostfs_t*)priv, ino, &hi);

    if (rc == RVVM_HOSTFS_OK) {
        fs_info_from_hostfs(out, &hi);
    }
    return (rvvm_fs_result_t)rc;
}

static rvvm_fs_result_t fs_hostfs_getdents(void* priv, const char* rel, uint32_t* pos,
                                           char* out_name, size_t size,
                                           uint8_t* out_kind, uint64_t* out_ino)
{
    return (rvvm_fs_result_t)rvvm_hostfs_getdents((rvvm_hostfs_t*)priv, rel, pos,
                                                  out_name, size, out_kind, out_ino);
}

static rvvm_fs_result_t fs_hostfs_getdents_ino(void* priv, uint32_t ino, uint32_t* pos,
                                               char* out_name, size_t size,
                                               uint8_t* out_kind, uint64_t* out_ino)
{
    return (rvvm_fs_result_t)rvvm_hostfs_getdents_ino((rvvm_hostfs_t*)priv, ino, pos,
                                                      out_name, size, out_kind, out_ino);
}

/* --- the two tables ----------------------------------------------------- */

static const rvvm_fs_ops_t rvvm_fs_ops_memfs = {
    .name          = "memfs",
    .read_only     = fs_memfs_read_only,
    .set_read_only = fs_memfs_set_read_only,
    .stat          = fs_memfs_stat,
    .isdir         = fs_memfs_isdir,
    .mkdir         = fs_memfs_mkdir,
    .create_file   = fs_memfs_create_file,
    .unlink        = fs_memfs_unlink,
    .rename        = fs_memfs_rename,
    .link          = fs_memfs_link,
    .symlink       = fs_memfs_symlink,
    .readlink      = fs_memfs_readlink,
    .truncate      = fs_memfs_truncate,
    .chmod         = fs_memfs_chmod,
    .touch         = fs_memfs_touch,
    .read          = fs_memfs_read,
    .write         = fs_memfs_write,
    .pin           = fs_memfs_pin,
    .unpin         = fs_memfs_unpin,
    .read_at       = fs_memfs_read_at,
    .write_at      = fs_memfs_write_at,
    .truncate_ino  = fs_memfs_truncate_ino,
    .stat_ino      = fs_memfs_stat_ino,
    .getdents      = fs_memfs_getdents,
    .getdents_ino  = fs_memfs_getdents_ino,
};

static const rvvm_fs_ops_t rvvm_fs_ops_hostfs = {
    .name          = "hostfs",
    .read_only     = fs_hostfs_read_only,
    .set_read_only = fs_hostfs_set_read_only,
    .stat          = fs_hostfs_stat,
    .isdir         = fs_hostfs_isdir,
    .mkdir         = fs_hostfs_mkdir,
    .create_file   = fs_hostfs_create_file,
    .unlink        = fs_hostfs_unlink,
    .rename        = fs_hostfs_rename,
    .link          = fs_hostfs_link,
    .symlink       = fs_hostfs_symlink,
    .readlink      = fs_hostfs_readlink,
    .truncate      = fs_hostfs_truncate,
    .chmod         = fs_hostfs_chmod,
    .touch         = fs_hostfs_touch,
    .read          = fs_hostfs_read,
    .write         = fs_hostfs_write,
    .pin           = fs_hostfs_pin,
    .unpin         = fs_hostfs_unpin,
    .read_at       = fs_hostfs_read_at,
    .write_at      = fs_hostfs_write_at,
    .truncate_ino  = fs_hostfs_truncate_ino,
    .stat_ino      = fs_hostfs_stat_ino,
    .getdents      = fs_hostfs_getdents,
    .getdents_ino  = fs_hostfs_getdents_ino,
};

rvvm_fs_t rvvm_fs_view_memfs(rvvm_memfs_t* fs)
{
    rvvm_fs_t view = { &rvvm_fs_ops_memfs, fs };
    return view;
}

rvvm_fs_t rvvm_fs_view_hostfs(rvvm_hostfs_t* fs)
{
    rvvm_fs_t view = { &rvvm_fs_ops_hostfs, fs };
    return view;
}
