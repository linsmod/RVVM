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

// Bundle layout under the release root. Three archives, three roles:
//
//   rootfs.tar.gz   the guest's `/` (Alpine minirootfs) - the base layer
//   system.tar.gz   host-provided *system programs*, laid out at their guest
//                   paths (sbin/vpsessiond) - the middle layer, no manifest
//   apps.tar.gz     the apps pre-deployed for the run: one `<id>.vapp` package
//                   (vp_app.h) per member, under apps/ in the archive
//
// The archives are extracted/installed in order into the run's own directory:
// the two layers are flattened (a later one wins), and each `.vapp` is installed
// under /data/app/<id> by vp_app. A `.vapp` is itself a zip, so the two formats
// nest: the tar is the bundle's delivery container, the zip is the app package.
#define VP_BUNDLE_DIR      "bundle"
#define VP_ROOTFS_TAR_GZ   "rootfs.tar.gz"
#define VP_SYSTEM_TAR_GZ   "system.tar.gz"
#define VP_APPS_TAR_GZ     "apps.tar.gz"
#define VP_APPS_MEMBER_DIR "apps"

// Where the guest sees the app model. The apps themselves are `.vapp` packages
// (vp_app.h), installed by the host at <guest>/data/app/<id>; this header only
// fixes the paths so the two agree.
#define VP_GUEST_APP_DIR   "/data/app"
#define VP_GUEST_DATA_DIR  "/data/data"

// The session server: a host-provided system program (the first entry of
// system.tar.gz), installed into a run's rootfs at this guest path. A core
// boots it by the path, like any other program - never as a host-relative
// loose ELF. tools/pack_system.py declares the same path when packing.
#define VP_GUEST_SESSIOND  "/sbin/vpsessiond"

// The idle core: a run root that never returns, so the machine it was booted
// into outlives every client. A command's run is torn down by on_guest_exit the
// moment its root exits, which is right for `vp exec-out` and wrong for a core
// several clients attach to - each arriving with its own run would get its own
// machine, and separate machines are separate /proc, so no two of them could
// see each other's processes. This holds the machine open; the host puts the
// sessions on top of it.
#define VP_GUEST_IDLE      "/sbin/idle"

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

// Read one indexed entry's bytes into @buffer, which must be at least the
// entry's size. Returns the entry's size, or (size_t)-1 when the index holds no
// regular file. What reads a `.vapp` member out of the bundle's apps.tar.gz
// without a scratch file; the archive's bytes must still be held (i.e. before
// vp_rootfs_release_data()).
size_t vp_rootfs_read_entry(const vp_rootfs_t* rootfs, uint32_t idx, void* buffer, size_t size);

#endif
