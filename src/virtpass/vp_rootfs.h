/*
vp_rootfs.h - read a bundle archive into the guest's rootfs (VirtPass)

A release carries two archives next to the host binary, and neither ever enters
the guest's namespace:

    <release root>/bundle/rootfs.tar.gz   the guest's `/` (Alpine minirootfs)
    <release root>/bundle/apps.tar.gz     the sample apps, one directory each

This module reads a .tar.gz twice over, for two different consumers:

  - vp_rootfs_shadow()   the guest-visible *shape* of the archive (vp_shadow.h):
                         every directory, regular file and symlink, with mode,
                         size, mtime and link target. The core answers lstat()/
                         readlink()/getdents64() from it, so a symlink never has
                         to exist on the host - which is what makes the archive
                         usable on Windows, where creating one needs a privilege.
  - vp_rootfs_extract()  the *bytes*: directories and regular files are written
                         under a host directory. Symlinks and hardlinks are
                         deliberately not created; the shadow index covers them,
                         so one busybox serves its 300 names without 300 copies.

Only the second half needs zlib, and only hosts that install a bundle need the
module at all, so it is excluded from librvvm and built into a host that links
-lz (see the Makefile / the Android CMakeLists).
*/

#ifndef VIRTPASS_VP_ROOTFS_H
#define VIRTPASS_VP_ROOTFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virtpass/vp_shadow.h"

// Bundle layout under the release root.
#define VP_BUNDLE_DIR    "bundle"
#define VP_ROOTFS_TAR_GZ "rootfs.tar.gz"
#define VP_APPS_TAR_GZ   "apps.tar.gz"

// Where the guest sees the app model, and where the app payload lives inside
// apps.tar.gz:
//
//   apps/<app_id>/app.json       {"id", "entry", "args"} - the declaration
//   apps/<app_id>/bin/<entry>    the guest ELF
//   apps/<app_id>/assets/...     the app's own resources
//
// which the host installs at <guest>/data/app/<app_id>.
#define VP_GUEST_APP_DIR   "/data/app"
#define VP_GUEST_DATA_DIR  "/data/data"
#define VP_APPS_MEMBER_DIR "apps"
#define VP_APP_MANIFEST    "app.json"

typedef struct vp_rootfs vp_rootfs_t;

// Open and index a .tar.gz. Returns NULL when the file cannot be read or is not
// a usable tar archive; @error (optional) receives the reason.
vp_rootfs_t* vp_rootfs_open(const char* tar_gz_path, const char** error);
void         vp_rootfs_close(vp_rootfs_t* rootfs);

// The guest-visible shape of the archive. Valid until the rootfs is closed; the
// host hands it to the core (rvvm_user_set_shadow) for the lifetime of a run.
// Non-const on purpose: the core records a guest's unlink of an archive-only
// entry in it, and that flag is per-run state the index owns.
vp_shadow_t* vp_rootfs_shadow(const vp_rootfs_t* rootfs);

// Entries indexed, in archive order. Entry indices are the shadow indices.
size_t vp_rootfs_count(const vp_rootfs_t* rootfs);

// Recreate the archive under @dest_dir: directories and regular files only. A
// regular file whose destination already has the same size is left alone, so
// repeating an install is cheap and an updated bundle still lands. Returns the
// number of files written; 0 with *error set means nothing could be installed.
size_t vp_rootfs_extract(vp_rootfs_t* rootfs, const char* dest_dir, const char** error);

// Drop the inflated archive bytes, keeping the index (the shadow) and the entry
// table. Call once the tree has been materialized: the guest's filesystem is
// answered from the shadow and the files on disk afterwards, so holding the
// whole archive (a minirootfs inflates to ~7 MB) for the life of the run only
// costs resident memory. After this, the extract/read calls below refuse.
void vp_rootfs_release_data(vp_rootfs_t* rootfs);

// Write one indexed entry's bytes to an explicit host path, creating the parent
// directory as needed. This is what an app install uses: the archive stores
// apps/<id>/... while the guest expects it under /data/app/<id>/.... Returns
// false for anything that is not a regular file.
bool vp_rootfs_extract_to(const vp_rootfs_t* rootfs, uint32_t idx, const char* dest_path);

// ---------------------------------------------------------------------------
// Apps (the apps.tar.gz half of the bundle)
//
// One directory per app, and its manifest is the only place the entry point is
// named - the guest is booted as /data/app/<id>/<entry>, so the host never has
// to guess a file name. Everything else in the directory (bin/, assets/) is the
// app's own payload and is installed as-is.
// ---------------------------------------------------------------------------

#define VP_APP_ID_MAX    64
#define VP_APP_ENTRY_MAX 256
#define VP_APP_ARGS_MAX  256

typedef struct {
    char id[VP_APP_ID_MAX];
    char entry[VP_APP_ENTRY_MAX]; // relative to the app dir, e.g. "bin/test_cli.exe"
    char args[VP_APP_ARGS_MAX];   // default arguments, "" when the manifest has none
} vp_app_t;

// Copy one entry's bytes into @buffer (NUL-terminated when it fits). Returns the
// entry's size, or (size_t)-1 when the index holds no such regular file.
size_t vp_rootfs_read_entry(const vp_rootfs_t* rootfs, uint32_t idx, char* buffer, size_t size);

// The manifest of @id, or false when the archive has no such app. A manifest
// without "entry" gets the conventional "bin/<id>.exe", so a hand-packed app
// only has to declare what is unusual about it.
bool vp_rootfs_app_manifest(vp_rootfs_t* apps, const char* id, vp_app_t* out);

// Every app the archive declares, in archive order, up to @max entries. Returns
// how many were found, which may exceed @max.
size_t vp_rootfs_apps(vp_rootfs_t* apps, vp_app_t* out, size_t max);

// Install @id's payload under @dest_app_dir, which becomes the app's root
// (<dest_app_dir>/bin/<entry>, <dest_app_dir>/assets/...). A file already
// present with the same size is left alone. Returns the number of files
// written, or 0 with *error set when the archive has no such app.
size_t vp_rootfs_install_app(vp_rootfs_t* apps, const char* id, const char* dest_app_dir,
                             const char** error);

#endif
