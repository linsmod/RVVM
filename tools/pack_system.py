#!/usr/bin/env python3
"""pack_system.py - build the system.tar.gz layer of a VirtPass bundle.

A system program is not an app: it has no manifest and no per-id directory. It
is a host-provided program laid out at the *guest path* it is to appear at, so
the host installs it by extracting the archive as a layer - no copy logic, and
the guest names it like any other program:

    sbin/vpsessiond
    sbin/idle

SYSTEM_PROGRAMS below is the single place a system program is declared: adding
one here is the whole change. tools/pack_apps.py is pointed at this archive
(--exclude-from) and skips every name it holds, so a system program is never
also packed as an app. The session server's guest path must match
VP_GUEST_SESSIOND in src/virtpass/vp_rootfs.h, and the idle core's must match
VP_GUEST_IDLE.

The archive is built deterministically - zero mtime, no owner, gzip mtime 0 -
so repacking an unchanged set produces the same bytes.

Usage:
    pack_system.py --src <dir with <name>.exe> --out system.tar.gz
    pack_system.py --names          # just the source names
    pack_system.py --list           # the guest paths
"""

import argparse
import gzip
import io
import os
import sys
import tarfile

# (source basename without .exe, guest path in the system layer). A guest path is
# relative - the archive's root is the guest's `/`.
SYSTEM_PROGRAMS = [
    ("vpsessiond", "sbin/vpsessiond"),
    ("idle", "sbin/idle"),
]

# (guest path, content, mode). Small config files layered over the rootfs -
# hand-written into the archive, no source file to keep in sync. musl has no
# built-in resolver config: without /etc/resolv.conf every getaddrinfo falls
# back to 127.0.0.1, where nobody listens, and apk's fetches all report
# "DNS: transient error (try again later)".
SYSTEM_FILES = [
    ("etc/resolv.conf", "nameserver 1.1.1.1\nnameserver 8.8.8.8\n", 0o644),
]


def die(msg):
    sys.stderr.write("pack_system: %s\n" % msg)
    return 1


def tar_dir(tar, arcname):
    info = tarfile.TarInfo(arcname)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    tar.addfile(info)


def tar_file(tar, arcname, path):
    st = os.stat(path)
    info = tarfile.TarInfo(arcname)
    info.size = st.st_size
    # A system program has to be executable.
    info.mode = 0o755
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    with open(path, "rb") as fh:
        tar.addfile(info, fh)
    return st.st_size


def tar_data(tar, arcname, content, mode):
    info = tarfile.TarInfo(arcname)
    data = content.encode("utf-8")
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    tar.addfile(info, io.BytesIO(data))
    return len(data)


def main():
    ap = argparse.ArgumentParser(description="Pack the VirtPass system layer")
    ap.add_argument("--src", help="directory holding <name>.exe")
    ap.add_argument("--out", help="the system.tar.gz to write")
    ap.add_argument("--names", action="store_true",
                    help="print the source names (what an app pack should exclude) and stop")
    ap.add_argument("--list", action="store_true", help="print the guest paths and stop")
    opts = ap.parse_args()

    if opts.names:
        for name, _guest in SYSTEM_PROGRAMS:
            print(name)
        return 0
    if opts.list:
        for _name, guest in SYSTEM_PROGRAMS:
            print("/" + guest)
        return 0

    if not opts.src or not opts.out:
        return die("--src and --out are required unless --names/--list is used")
    if not os.path.isdir(opts.src):
        return die("no such source directory: %s" % opts.src)

    out_dir = os.path.dirname(os.path.abspath(opts.out))
    os.makedirs(out_dir, exist_ok=True)

    packed = []
    # gzip's own header carries an mtime, and tarfile would set it to "now": the
    # whole point of packing this way is that the bytes only change when the
    # input does, so the stream is opened here with mtime=0.
    with open(opts.out, "wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
                for name, guest in SYSTEM_PROGRAMS:
                    exe = os.path.join(opts.src, name + ".exe")
                    if not os.path.isfile(exe):
                        return die("%s: no such system program" % exe)
                    parts = guest.split("/")
                    for i in range(1, len(parts)):
                        tar_dir(tar, "/".join(parts[:i]))
                    size = tar_file(tar, guest, exe)
                    packed.append((name, guest, size))
                for guest, content, mode in SYSTEM_FILES:
                    parts = guest.split("/")
                    for i in range(1, len(parts)):
                        tar_dir(tar, "/".join(parts[:i]))
                    size = tar_data(tar, guest, content, mode)
                    packed.append(("(data)", guest, size))

    with open(opts.out, "rb") as fh:
        total = len(fh.read())

    # Read it back: a tar written by a script is worth verifying before a host
    # is asked to install it.
    with tarfile.open(opts.out, "r:gz") as tar:
        members = tar.getnames()
        for _name, guest, _ in packed:
            if guest not in members:
                return die("verification failed: %s is missing from %s" % (guest, opts.out))

    for name, guest, size in packed:
        print("system %-16s /%s  %d byte(s)" % (name, guest, size))
    print("wrote %s: %d entry(ies), %d member(s), %d byte(s)"
          % (opts.out, len(packed), len(members), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())