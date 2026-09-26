/*
vp_bundle.h - what a host does with a release bundle (VirtPass)

A release carries three archives next to the host binary (vp_rootfs.h reads
them), and they are three layers of the guest's file system:

    bundle/rootfs.tar.gz    the base: the guest's `/` (Alpine minirootfs)
    bundle/system.tar.gz    host-provided system programs (sbin/vpsessiond)
    bundle/apps.tar.gz      the pre-deployed apps: one `<id>.vapp` package
                            (vp_app.h) per member, installed under /data/app/<id>

This module is the *host half* of that, in one place because both hosts have to
do exactly the same thing and nothing else:

    vp_bundle_mount()            materialize all three layers into a run's own
                                 directory, write the few files the archive does
                                 not ship, and hand the core the prefix + the
                                 base archive's shape (the shadow).
    vp_bundle_app_assets_path()  an app's own resources, as a host directory.

The layers are flattened at install time, not overlaid at run time: the core
answers guest paths from one materialized tree plus the shadow index, so a later
layer simply overwrites an earlier one on disk (and Windows could not express
the archive's symlinks without materializing anyway). Each layer is stamped
against its own archive, so a repeated run of an unchanged bundle is a no-op and
an updated archive re-extracts - the shape a preinstalled system image has.

The app model is Android's, not per-run: every app the archive declares is
installed once, persistently, at <guest>/data/app/<id>, and a run simply boots
one of them. Nothing is emptied between runs; a run that boots no app still sees
the installed ones, exactly as the system image's `/data/app` is there for
everyone. When apps.tar.gz changes, the whole /data/app tree is rebuilt, so an
app the archive dropped does not linger.

Nothing here is Android- or Windows-specific except a path separator and the
binary-mode flag on an opened descriptor, and nothing here knows how a host
finds its bundle directory: the caller names the archives and the destination.
*/

#ifndef VIRTPASS_VP_BUNDLE_H
#define VIRTPASS_VP_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/rvvm_user.h"
#include "virtpass/vp_app.h"
#include "virtpass/vp_rootfs.h"

// What vp_bundle_mount() found and did.
typedef struct {
    size_t entries;        // base archive entries indexed (the guest-visible shape)
    size_t files;          // base regular files written into the run's directory
    size_t system_files;   // system layer regular files written
    size_t apps;           // apps installed under /data/app
} vp_bundle_stats_t;

// Give @machine the filesystem view of a run: materialize the base rootfs, then
// the system layer, then provision the apps - each stamped against its own
// input, so a repeated run of an unchanged bundle only walks the tree. Writes
// the session files the base archive does not ship (inittab, /proc/mounts) and
// creates /dev/pts. @system_tar_gz may be NULL (a host may have no system
// layer); a named-but-unreadable archive is an error. @apps_tar_gz is the
// pre-deployment form of the apps: a tar of `<id>.vapp` packages (vp_app.h); it
// may be NULL for a host without apps. Returns false with *error set when the
// base cannot be read, or a named layer cannot be installed - the caller may
// then run the guest with no filesystem view at all, which is what a host
// without a bundle has always done. @stats is optional.
bool vp_bundle_mount(rvvm_machine_t* machine, const char* rootfs_tar_gz,
                     const char* system_tar_gz, const char* apps_tar_gz,
                     const char* dest, vp_bundle_stats_t* stats, const char** error);

// Drop the mounted run's base index. A bundle belongs to one run; the next run
// mounts its own, and the index records that run's guest-side deletions.
void vp_bundle_unmount(void);

// The app a guest path names: "/data/app/<id>/..." is the app model's own
// spelling, so naming one is also the request to boot that app.
bool vp_bundle_app_id_from_guest_path(const char* guest_path, char* out, size_t size);

// An app's own resources, as a host directory (<dest>/data/app/<id>/assets),
// which is what a host points its asset mount at. False when the app has none -
// which is also what a boot of no app gets.
bool vp_bundle_app_assets_path(const char* dest, const char* app_id, char* out, size_t size);

// How many apps a picker asks for in one call.
#define VP_BUNDLE_APPS_MAX 64

// One picker entry: an app, the guest path that boots it, and the arguments its
// manifest asks for.
typedef struct {
    char id[VP_APP_ID_MAX];
    char guest_path[VP_APP_ID_MAX + VP_APP_ENTRY_MAX + 16];
    char args[VP_APP_ARGS_MAX];
} vp_bundle_app_t;

// The apps an @apps_tar_gz holds (its `<id>.vapp` members), up to @max, sorted
// by id. Returns how many were found (which may exceed @max); 0 for a missing or
// unreadable archive, which is not an error - a host may run without apps.
size_t vp_bundle_list_apps(const char* apps_tar_gz, vp_bundle_app_t* out, size_t max);

// One app's manifest out of @apps_tar_gz, by id - the controlled launcher's
// resolver, without installing anything. False when the archive has no such
// package.
bool vp_bundle_read_app(const char* apps_tar_gz, const char* id, vp_app_t* out);

// The /assets mount: a real directory tree, which is what both hosts want once
// an app's payload is on disk. Point it at vp_bundle_app_assets_path() - as the
// userdata of rvvm_user_set_assets() when the host can have more than one run
// alive at a time, which is what keeps one run from reading another's resources;
// a host with a single run at a time can set the process-wide root instead. The
// ops read whichever is set when they are called, so nothing here is copied.
void vp_bundle_set_assets_root(const char* dir);
const rvvm_asset_ops_t* vp_bundle_assets(void);

#endif /* VIRTPASS_VP_BUNDLE_H */