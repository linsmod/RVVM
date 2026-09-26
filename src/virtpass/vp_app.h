/*
vp_app.h - the per-app package (`.vapp`) (VirtPass)

An app is a *package*, the way it is on Android: one file that declares its own
identity and entry point, so nothing boots an app by being handed a path. It is
an APK-shaped zip:

    meta.json            {"id","version","entry","args"} - the manifest
    files/bin/<entry>    the payload, laid out as the app's own directory
    files/assets/...     the app's resources

Installing extracts the `files/` tree to <guest>/data/app/<id> (and copies meta.json
there), and creates <guest>/data/data/<id> as the app's private state. A launch
reads the *installed* manifest and boots `/data/app/<id>/<entry>` - the entry
comes from the package, never from the command line.

The payload entries are relative to the app's own directory: `files/bin/x.exe`
installs as `<appdir>/bin/x.exe`, so `entry` is a path under the app dir (the
`files/` prefix is the package's, not the app's). This is what lets the same
directory be both the zip layout and the installed tree.
*/
#ifndef VIRTPASS_VP_APP_H
#define VIRTPASS_VP_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Identity limits shared with the bundle's picker and the launcher.
#define VP_APP_ID_MAX    64
#define VP_APP_ENTRY_MAX 256
#define VP_APP_ARGS_MAX  256

// A `.vapp` is a zip with this extension; its manifest sits at the zip root and
// its payload under this prefix.
#define VP_VAPP_EXT      ".vapp"
#define VP_VAPP_MANIFEST "meta.json"
#define VP_VAPP_FILES    "files/"

typedef struct {
    char     id[VP_APP_ID_MAX];
    char     entry[VP_APP_ENTRY_MAX];  // relative to the app dir, e.g. "bin/x.exe"
    char     args[VP_APP_ARGS_MAX];    // default arguments, "" when none
    uint32_t version;
} vp_app_t;

// Read a package's manifest without installing it (the picker's listing, a
// pre-install check). False with *error set when the zip or its manifest cannot
// be read, or the id/entry is unusable. `_memory` reads a package held in
// memory - a member read out of the bundle's apps.tar.gz.
bool vp_app_read(const char* vapp_path, vp_app_t* out, const char** error);
bool vp_app_read_memory(const void* data, size_t size, vp_app_t* out, const char** error);

// Install a package into @dest (the materialized rootfs): extract the files/
// tree to <dest>/data/app/<id>, copy the manifest beside it, create
// <dest>/data/data/<id> and record the id in <dest>/data/system/packages.list.
// False with *error set on a bad package or a write failure. `_memory` is the
// pre-deployment path (a package read out of apps.tar.gz); the path form is what
// a future `pm install <id>.vapp` uses.
bool vp_app_install(const char* vapp_path, const char* dest, const char** error);
bool vp_app_install_memory(const void* data, size_t size, const char* dest, const char** error);

// The installed manifest of @id (from <dest>/data/app/<id>/meta.json). This is
// what a launch reads, so a package that changed after install does not change
// what runs.
bool vp_app_installed(const char* dest, const char* id, vp_app_t* out);

// The guest path a launch boots for @id: "<VP_GUEST_APP_DIR>/<id>/<entry>".
bool vp_app_guest_entry(const char* dest, const char* id, char* out, size_t size);

#endif /* VIRTPASS_VP_APP_H */