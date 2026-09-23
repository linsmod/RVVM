/*
vp_bundle.h - what a host does with a release bundle (VirtPass)

A release carries two archives next to the host binary (vp_rootfs.h reads them):

    bundle/rootfs.tar.gz    the guest's `/`
    bundle/apps.tar.gz      one directory per app

This module is the *host half* of that, in one place because both hosts have to
do exactly the same thing and nothing else:

    vp_bundle_mount()        materialize the rootfs into a run's own directory,
                             write the few files the archive does not ship, and
                             hand the core the prefix + the archive's shape.
    vp_bundle_install_app()  install the one app the run boots, leaving the app
                             tree holding it alone - that is the isolation.
    vp_bundle_assets()       the /assets mount, served from a real directory.

Nothing here is Android- or Windows-specific except a path separator and the
binary-mode flag on an opened descriptor, and nothing here knows how a host
finds its bundle directory: the caller names the archive and the destination.
*/

#ifndef VIRTPASS_VP_BUNDLE_H
#define VIRTPASS_VP_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/rvvm_user.h"
#include "virtpass/vp_rootfs.h"

// What vp_bundle_mount() found in the archive.
typedef struct {
    size_t entries; // archive entries indexed (the guest-visible shape)
    size_t files;   // regular files written into the run's directory
} vp_bundle_stats_t;

// Give @machine the filesystem view of a run: materialize @rootfs_tar_gz under
// @dest (which becomes the guest's prefix), write the session files the archive
// does not ship (inittab, /proc/mounts) and create /dev/pts. Returns false with
// *error set when the archive cannot be read or installed - the caller may then
// run the guest with no filesystem view at all, which is what a host without a
// bundle has always done. @stats is optional.
bool vp_bundle_mount(rvvm_machine_t* machine, const char* rootfs_tar_gz, const char* dest,
                     vp_bundle_stats_t* stats, const char** error);

// Drop the mounted run's index. A bundle belongs to one run; the next run mounts
// its own, and the index records that run's guest-side deletions.
void vp_bundle_unmount(void);

// Install @app_id out of @apps_tar_gz into @dest (the materialized rootfs): the
// app tree is emptied first, so the guest can only ever see the app it was
// given, and /data/data/<app_id> is created as its writable state. When the app
// has an assets/ directory, its host path is copied into @assets_root - that is
// what the caller points the asset mount at, which is what makes /assets per-app
// without touching the guest ABI. Returns false with *error set when the archive
// has no such app or nothing could be written.
bool vp_bundle_install_app(const char* apps_tar_gz, const char* dest, const char* app_id,
                           char* assets_root, size_t assets_root_size, const char** error);

// The app a guest path names: "/data/app/<id>/..." is the app model's own
// spelling, so naming one is also the request to boot that app.
bool vp_bundle_app_id_from_guest_path(const char* guest_path, char* out, size_t size);

// How many apps a picker asks for in one call.
#define VP_BUNDLE_APPS_MAX 64

// One picker entry: an app, the guest path that boots it, and the arguments its
// manifest asks for.
typedef struct {
    char id[VP_APP_ID_MAX];
    char guest_path[VP_APP_ID_MAX + VP_APP_ENTRY_MAX + 16];
    char args[VP_APP_ARGS_MAX];
} vp_bundle_app_t;

// The apps @apps_tar_gz declares, up to @max, in archive order. Returns how many
// were found (which may exceed @max); 0 for a missing or unreadable archive,
// which is not an error - a host may run without a bundle.
size_t vp_bundle_list_apps(const char* apps_tar_gz, vp_bundle_app_t* out, size_t max);

// The /assets mount: a real directory tree, which is what both hosts want once
// an app's payload is on disk. Point it at the app's own assets directory
// (vp_bundle_install_app() reports it) - as the userdata of rvvm_user_set_assets
// when the host can have more than one run alive at a time, which is what keeps
// one run from reading another's resources; a host with a single run at a time
// can set the process-wide root instead. The ops read whichever is set when they
// are called, so nothing here is copied or owned.
void vp_bundle_set_assets_root(const char* dir);
const rvvm_asset_ops_t* vp_bundle_assets(void);

#endif
