#!/usr/bin/env python3
"""pack_apps.py - build the apps.tar.gz half of a VirtPass bundle.

The archive carries one directory per app:

    apps/<id>/app.json       {"id", "entry", "args"} - how the host boots it
    apps/<id>/bin/<entry>    the guest program
    apps/<id>/assets/...     the app's own resources (the AAssetManager tree)

The host unpacks exactly one app per run, into <guest>/data/app/<id>: that, and
nothing else, is what keeps one app from seeing another's payload or assets.

The archive is built deterministically - sorted names, zero mtime, no owner - so
repacking an unchanged tree produces the same bytes. A release artifact should
not churn just because it was rebuilt.

Usage:
    pack_apps.py --src <dir with <id>.exe> --asset fonts=<dir> --out apps.tar.gz
    pack_apps.py --src <dir> --list              # just print the app ids
"""

import argparse
import gzip
import io
import json
import os
import sys
import tarfile

MANIFEST = "app.json"


def die(msg):
    sys.stderr.write("pack_apps: %s\n" % msg)
    return 1


def tar_dir(tar, arcname):
    info = tarfile.TarInfo(arcname)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    tar.addfile(info)


def tar_bytes(tar, arcname, payload, mode=0o644):
    info = tarfile.TarInfo(arcname)
    info.size = len(payload)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    tar.addfile(info, io.BytesIO(payload))


def tar_file(tar, arcname, path, mode=None):
    st = os.stat(path)
    info = tarfile.TarInfo(arcname)
    info.size = st.st_size
    # A guest program has to be executable; everything else is a resource.
    info.mode = mode if mode is not None else (0o755 if path.endswith(".exe") else 0o644)
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    with open(path, "rb") as fh:
        tar.addfile(info, fh)
    return st.st_size


def tar_tree(tar, arcroot, hostroot):
    """Add @hostroot under @arcroot, sorted, directories included.

    Directories matter: an empty one is part of an app's shape (a place it will
    write to), and the host's installer creates exactly what the archive holds.
    """
    total = 0
    for dirpath, dirnames, filenames in os.walk(hostroot):
        dirnames.sort()
        filenames.sort()
        rel = os.path.relpath(dirpath, hostroot)
        arc = arcroot if rel == "." else arcroot + "/" + rel.replace(os.sep, "/")
        tar_dir(tar, arc)
        for name in filenames:
            total += tar_file(tar, arc + "/" + name, os.path.join(dirpath, name))
    return total


def find_ids(src, only):
    if only:
        return [x for x in only.split(",") if x]
    if not os.path.isdir(src):
        return []
    return sorted(n[:-4] for n in os.listdir(src) if n.endswith(".exe"))


def main():
    ap = argparse.ArgumentParser(description="Pack the VirtPass apps archive")
    ap.add_argument("--src", required=True, help="directory holding <id>.exe")
    ap.add_argument("--out", help="the apps.tar.gz to write")
    ap.add_argument("--asset", action="append", default=[], metavar="NAME=DIR",
                    help="tree to place under every app's assets/NAME (repeatable)")
    ap.add_argument("--args", action="append", default=[], metavar="ID=ARGS",
                    help="default arguments for one app (repeatable)")
    ap.add_argument("--only", default="", help="comma-separated app ids to pack")
    ap.add_argument("--list", action="store_true", help="print the app ids and stop")
    opts = ap.parse_args()

    assets = []
    for spec in opts.asset:
        name, _, path = spec.partition("=")
        if not name or not path or not os.path.isdir(path):
            return die("--asset wants NAME=DIR with an existing directory (got %r)" % spec)
        assets.append((name, path))

    args_by_id = {}
    for spec in opts.args:
        app_id, _, value = spec.partition("=")
        if app_id:
            args_by_id[app_id] = value

    ids = find_ids(opts.src, opts.only)
    if not ids:
        return die("no guest programs found in %s" % opts.src)

    if opts.list:
        for app_id in ids:
            print(app_id)
        return 0

    if not opts.out:
        return die("--out is required unless --list is used")

    out_dir = os.path.dirname(os.path.abspath(opts.out))
    os.makedirs(out_dir, exist_ok=True)

    packed = []
    # gzip's own header carries an mtime, and tarfile would set it to "now": the
    # whole point of packing this way is that the bytes only change when the
    # input does, so the stream is opened here with mtime=0.
    with open(opts.out, "wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
                tar_dir(tar, "apps")
                for app_id in ids:
                    exe = os.path.join(opts.src, app_id + ".exe")
                    if not os.path.isfile(exe):
                        return die("%s: no such guest program" % exe)

                    root = "apps/" + app_id
                    entry = "bin/%s.exe" % app_id
                    tar_dir(tar, root)
                    tar_dir(tar, root + "/bin")

                    manifest = json.dumps({
                        "id": app_id,
                        "entry": entry,
                        "args": args_by_id.get(app_id, ""),
                    }, separators=(",", ":"), sort_keys=True)
                    tar_bytes(tar, root + "/" + MANIFEST,
                              (manifest + "\n").encode("utf-8"))

                    size = tar_file(tar, root + "/" + entry, exe, 0o755)

                    # The app's own resources: the shared trees first, then
                    # <src>/<id>.assets/ on top, which is how one app overrides
                    # or extends what the others share.
                    if assets:
                        tar_dir(tar, root + "/assets")
                        for name, path in assets:
                            size += tar_tree(tar, root + "/assets/" + name, path)
                    override = os.path.join(opts.src, app_id + ".assets")
                    if os.path.isdir(override):
                        size += tar_tree(tar, root + "/assets", override)

                    packed.append((app_id, entry, size))

    with open(opts.out, "rb") as fh:
        total = len(fh.read())

    # Read it back: a tar written by a script is worth verifying before a host
    # is asked to mount it.
    with tarfile.open(opts.out, "r:gz") as tar:
        members = tar.getnames()
        for app_id, entry, _ in packed:
            want = "apps/%s/%s" % (app_id, entry)
            if want not in members:
                return die("verification failed: %s is missing from %s" % (want, opts.out))

    for app_id, entry, size in packed:
        print("app %-16s /data/app/%s/%s  %d byte(s) of payload"
              % (app_id, app_id, entry, size))
    print("wrote %s: %d app(s), %d member(s), %d byte(s)"
          % (opts.out, len(packed), len(members), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
