#!/usr/bin/env python3
"""pack_apps.py - build the app packages of a VirtPass bundle.

An app is a *package*: one `<id>.vapp` file, an APK-shaped zip with its own
manifest

    meta.json            {"id","version","entry","args"} - the manifest
    files/bin/<id>.exe   the payload, laid out as the app's own directory
    files/assets/...     the app's resources

The host installs a package under <guest>/data/app/<id> and boots it from the
manifest (vp_app.h), so an app is never run by being handed a path.

The packages are delivered *nested*: this script packs them as `<id>.vapp`
members inside apps.tar.gz - the tar is the bundle's delivery container, the zip
is the app package. The two formats are both kept: the tar is what a release
ships and a host reads in one go (the pre-deployment form), the zip is what one
`pm install` will take later.

A system program is not an app (tools/pack_system.py): this script is pointed
at the system archive with --exclude-from and skips those names, so a program
that is a system program is never also packed as an app.

The archive is built deterministically - sorted names, zero mtime, no owner -
so repacking an unchanged tree produces the same bytes.

Usage:
    pack_apps.py --src <dir with <id>.exe> --out <apps.tar.gz>
                 [--asset fonts=<dir>] [--exclude-from system.tar.gz]
    pack_apps.py --src <dir> --list    # just print the ids
"""

import argparse
import gzip
import io
import json
import os
import sys
import tarfile
import zipfile

MANIFEST = "meta.json"
FILES = "files"
EXT = ".vapp"
MEMBER_DIR = "apps"
# A fixed DOS timestamp keeps the zip bytes stable across builds (zip has no
# "zero" date; 1980-01-01 is its own epoch).
DOS_TIME = (1980, 1, 1, 0, 0, 0)


def die(msg):
    sys.stderr.write("pack_apps: %s\n" % msg)
    return 1


def system_names(archive):
    """Source names held by a system layer: the basename of each program file.

    pack_system.py lays a program out at its guest path (sbin/vpsessiond), so
    the basename is exactly the app id it must not be packed under."""
    if not archive or not os.path.isfile(archive):
        return set()
    names = set()
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            if member.isfile():
                names.add(os.path.basename(member.name))
    return names


def find_ids(src, only, skip):
    ids = [x for x in only.split(",") if x] if only else (
        sorted(n[:-4] for n in os.listdir(src) if n.endswith(".exe"))
        if os.path.isdir(src) else [])
    return [i for i in ids if i not in skip]


def zip_dir(zf, arcname, mode=0o755):
    info = zipfile.ZipInfo(arcname + "/", date_time=DOS_TIME)
    info.external_attr = (mode << 16) | 0x10  # Unix mode + DOS directory bit
    zf.writestr(info, b"")


def zip_file(zf, arcname, path, mode=None):
    with open(path, "rb") as fh:
        payload = fh.read()
    info = zipfile.ZipInfo(arcname, date_time=DOS_TIME)
    # A guest program has to be executable; everything else is a resource.
    info.external_attr = (mode if mode is not None else
                          (0o755 if path.endswith(".exe") else 0o644)) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    zf.writestr(info, payload)
    return len(payload)


def zip_tree(zf, arcroot, hostroot):
    """Add @hostroot under @arcroot, sorted, directories included."""
    total = 0
    for dirpath, dirnames, filenames in os.walk(hostroot):
        dirnames.sort()
        filenames.sort()
        rel = os.path.relpath(dirpath, hostroot)
        arc = arcroot if rel == "." else arcroot + "/" + rel.replace(os.sep, "/")
        zip_dir(zf, arc)
        for name in filenames:
            total += zip_file(zf, arc + "/" + name, os.path.join(dirpath, name))
    return total


def build_vapp(app_id, exe, assets, args):
    """The `<id>.vapp` package's bytes."""
    entry = "bin/%s.exe" % app_id
    manifest = json.dumps({
        "id": app_id,
        "version": 1,
        "entry": entry,
        "args": args,
    }, separators=(",", ":"), sort_keys=True)

    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        zip_dir(zf, "bin")
        info = zipfile.ZipInfo(MANIFEST, date_time=DOS_TIME)
        info.external_attr = 0o644 << 16
        zf.writestr(info, (manifest + "\n").encode("utf-8"))
        zip_file(zf, FILES + "/" + entry, exe, 0o755)
        if assets:
            for name, path in assets:
                zip_tree(zf, FILES + "/assets/" + name, path)
        override = os.path.join(os.path.dirname(exe), app_id + ".assets")
        if os.path.isdir(override):
            zip_tree(zf, FILES + "/assets", override)
    return out.getvalue()


def tar_dir(tar, arcname):
    info = tarfile.TarInfo(arcname)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    tar.addfile(info)


def main():
    ap = argparse.ArgumentParser(description="Pack the VirtPass apps archive")
    ap.add_argument("--src", required=True, help="directory holding <id>.exe")
    ap.add_argument("--out", help="the apps.tar.gz to write")
    ap.add_argument("--asset", action="append", default=[], metavar="NAME=DIR",
                    help="tree to place under every app's assets/NAME (repeatable)")
    ap.add_argument("--args", action="append", default=[], metavar="ID=ARGS",
                    help="default arguments for one app (repeatable)")
    ap.add_argument("--only", default="", help="comma-separated app ids to pack")
    ap.add_argument("--exclude", default="",
                    help="comma-separated app ids to leave out (system programs)")
    ap.add_argument("--exclude-from", default="", metavar="SYSTEM_TAR",
                    help="a system.tar.gz whose programs are not packed as apps")
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

    skip = set(x for x in opts.exclude.split(",") if x) | system_names(opts.exclude_from)
    ids = find_ids(opts.src, opts.only, skip)
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
    # bytes should only change when the input does, so the stream is opened here
    # with mtime=0.
    with open(opts.out, "wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
                tar_dir(tar, MEMBER_DIR)
                for app_id in ids:
                    exe = os.path.join(opts.src, app_id + ".exe")
                    if not os.path.isfile(exe):
                        return die("%s: no such guest program" % exe)
                    vapp = build_vapp(app_id, exe, assets, args_by_id.get(app_id, ""))
                    arcname = "%s/%s%s" % (MEMBER_DIR, app_id, EXT)
                    info = tarfile.TarInfo(arcname)
                    info.size = len(vapp)
                    info.mode = 0o644
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    tar.addfile(info, io.BytesIO(vapp))
                    packed.append((app_id, arcname, len(vapp)))

    with open(opts.out, "rb") as fh:
        total = len(fh.read())

    # Read every package back: a tar/zip written by a script is worth verifying
    # before a host is asked to install it.
    with tarfile.open(opts.out, "r:gz") as tar:
        members = tar.getnames()
        for app_id, arcname, _ in packed:
            if arcname not in members:
                return die("verification failed: %s is missing from %s" % (arcname, opts.out))
            with zipfile.ZipFile(io.BytesIO(tar.extractfile(arcname).read())) as zf:
                manifest = json.loads(zf.read(MANIFEST).decode("utf-8"))
                if manifest.get("id") != app_id:
                    return die("verification failed: %s declares id %r"
                               % (arcname, manifest.get("id")))

    for app_id, arcname, size in packed:
        print("app %-16s /data/app/%s/bin/%s.exe  %d byte(s) of payload"
              % (app_id, app_id, app_id, size))
    print("wrote %s: %d package(s), %d member(s), %d byte(s)"
          % (opts.out, len(packed), len(members), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())