/*
 * vp_asset.h - bundled assets: the guest-visible mount
 *
 * Ship-with-the-app resources (the APK's assets/ tree, or the equivalent
 * directory a host keeps) are not part of the guest's file system: they live
 * inside the package, so no host open() can reach them and no guest path names
 * them. A host that has such a tree mounts it here, and the guest then treats it
 * as what it is - an ordinary read-only directory:
 *
 *     fd = open("/assets/fonts/x.ttf", O_RDONLY);
 *     read(fd, buf, n);
 *
 * How that fd is served is the host's business, with one expectation: it is a
 * stream, not a copy. An asset is read a chunk at a time and need never be
 * resident anywhere - a host that materializes the whole thing on open() defeats
 * the point of the mount. So the fd may well be a pipe, and then non-seekable is
 * the honest answer (`lseek` -> ESPIPE, `fstat` -> FIFO) rather than something to
 * paper over; a guest that needs random access has to be given a seekable source
 * instead. The core reaps the fd when the run ends, so a host can hang background
 * work off it (the thread feeding that pipe) and rely on the run end unblocking
 * it. See rvvm_user_set_assets() in core/rvvm_user.h.
 *
 * The mount is also a directory: stat() of it says S_IFDIR, and opendir()/
 * readdir() walk it through the host's own enumeration (a synthetic fd, since an
 * asset directory has no host fd). What that enumeration reports is the host's
 * API - on Android it is the NDK's AAssetDir, which lists the files at a level
 * but not its subdirectory names, so a subdirectory is reachable by name rather
 * than discoverable by reading the parent.
 *
 * One namespace per guest: there is no second asset root to name, exactly as the
 * NDK's AAssetManager is one per package. That is also why this mount has no
 * handle - a path is the whole identity.
 *
 * This is not the place for an application's own compressed payloads: a .gz under
 * the tree is decompressed by the *guest*, which links its own library. The host
 * only ever peels its own packaging format - an APK entry stored deflated comes
 * back inflated - because that format is the host's, not the guest's.
 */
#ifndef VP_ASSET_H
#define VP_ASSET_H

/* Where the host's asset tree appears in the guest's namespace. Fixed rather
 * than configurable so a guest can hardcode it, like /proc and /dev. */
#define VP_ASSET_MOUNT "/assets"

#endif /* VP_ASSET_H */
