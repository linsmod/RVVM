#!/usr/bin/env python3
"""pack_system.py - build the system.tar.gz layer of a VirtPass bundle.

A system program is not an app: it has no manifest and no per-id directory. It
is a host-provided program laid out at the *guest path* it is to appear at, so
the host installs it by extracting the archive as a layer - no copy logic, and
the guest names it like any other program:

    sbin/vpshhd
    sbin/tinysshd

SYSTEM_PROGRAMS below is the single place a system program is declared: adding
one here is the whole change. The source tree is separate from the app one
(BUILDDIR/system-assets vs BUILDDIR/guest-assets), so tools/pack_apps.py never
sees a system program and needs no exclusion list. The listener's guest path
must match VP_GUEST_SSHD in src/virtpass/vp_rootfs.h.

tinysshd is a multicall binary whose mode comes from basename(argv[0])
(tinysshd.c), so nothing here needs a second name for it: vpshhd execs
/sbin/tinysshd with argv[0] set to the mode it wants (see vpshhd.c).

Alpine packages (--pkgs) are laid into the same layer. The guest's rootfs is an
Alpine minirootfs and its programs are dynamically linked against musl, which
works in the guest (an Alpine `apk --version` runs there), so the distribution's
own riscv64 binaries are usable as they are - no cross-compiling curl, sqlite or
python. Dependencies are resolved from the mirror's APKINDEX, the same mirror the
rootfs comes from, and downloads are cached so a rebuild does not need the
network.

The archive is built deterministically - zero mtime, no owner, gzip mtime 0 -
so repacking an unchanged set produces the same bytes.

Usage:
    pack_system.py --src <dir with <name>.exe> --out system.tar.gz
    pack_system.py --src <dir> --out system.tar.gz --pkgs "curl sqlite python3"
    pack_system.py --names          # just the source names
    pack_system.py --list           # the guest paths
"""

import argparse
import gzip
import io
import os
import sys
import tarfile
import urllib.error
import urllib.request

# Packages the rootfs already provides, at the version the release was built
# from. Layering the mirror's copy over them would replace a working musl/busybox
# with whatever the index says today, for no gain.
ALREADY_IN_ROOTFS = {
    "musl", "busybox", "busybox-binsh", "alpine-baselayout",
    "alpine-baselayout-data", "alpine-keys", "apk-tools", "ssl_client",
    "scanelf",
}

DEFAULT_MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
DEFAULT_BRANCH = "v3.24"
DEFAULT_ARCH = "riscv64"

# (source basename without .exe, guest path in the system layer). A guest path is
# relative - the archive's root is the guest's `/`.
SYSTEM_PROGRAMS = [
    ("vpshhd", "sbin/vpshhd"),
    ("tinysshd", "sbin/tinysshd"),
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


# --- Alpine packages -------------------------------------------------------
#
# The dependency data is the mirror's own APKINDEX, read the way apk reads it:
# one blank-line separated record per package, fields introduced by one letter
# and a colon. Only three are needed - P (name), V (version), D (depends) - plus
# p (provides), which is how a `so:libcurl.so.4` dependency finds its package.


def http_get(url, cache_path):
    """Bytes at @url, via @cache_path when it is already there. The cache is what
    makes a rebuild offline: the mirror is needed once, not at every make."""
    if cache_path and os.path.isfile(cache_path):
        with open(cache_path, "rb") as fh:
            return fh.read()
    try:
        with urllib.request.urlopen(url, timeout=120) as resp:
            blob = resp.read()
    except (urllib.error.URLError, OSError) as exc:
        sys.stderr.write("pack_system: fetch %s: %s\n" % (url, exc))
        raise SystemExit(1)
    if cache_path:
        os.makedirs(os.path.dirname(cache_path), exist_ok=True)
        with open(cache_path, "wb") as fh:
            fh.write(blob)
    return blob


def load_index(mirror, branch, arch, repo, cache_dir):
    """{name: record} and {provides-token: record} for one repository."""
    url = "%s/%s/%s/%s/APKINDEX.tar.gz" % (mirror, branch, repo, arch)
    cache = os.path.join(cache_dir, "APKINDEX-%s.tar.gz" % repo) if cache_dir else None
    blob = http_get(url, cache)
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tar:
        text = tar.extractfile("APKINDEX").read().decode("utf-8", "replace")
    by_name = {}
    by_provides = {}
    for record in text.split("\n\n"):
        fields = {}
        for line in record.splitlines():
            if len(line) > 2 and line[1] == ":":
                fields.setdefault(line[0], []).append(line[2:])
        if "P" not in fields or "V" not in fields:
            continue
        fields["repo"] = [repo]
        by_name[fields["P"][0]] = fields
        # One `p:` line holds every token, space separated, so the line has to be
        # split as well as the records.
        for prov in " ".join(fields.get("p", [])).split():
            by_provides.setdefault(prov.split("=", 1)[0], fields)
    return by_name, by_provides


def dep_name(token):
    """`libssl3>=3.3.0` -> `libssl3`; `!foo` -> None (a conflict, not a need)."""
    if not token or token.startswith("!"):
        return None
    for sep in ("=", ">", "<", "~"):
        token = token.split(sep, 1)[0]
    return token or None


def resolve_packages(wanted, indexes, skip):
    """Every package the closure of @wanted needs, by name -> record."""
    chosen = {}
    queue = list(wanted)
    missing = []
    while queue:
        token = queue.pop(0)
        name = dep_name(token)
        if not name or name in skip:
            continue
        record = None
        for by_name, by_provides in indexes:
            record = by_name.get(name) or by_provides.get(name)
            if record:
                break
        if not record:
            # Not in the index: either a library the rootfs already has (musl's
            # soname is the common one) or a gap worth reporting.
            if name.startswith("so:"):
                missing.append(name)
            else:
                missing.append(name)
            continue
        pkg = record["P"][0]
        if pkg in chosen or pkg in skip:
            # `so:libc.musl-riscv64.so.1` resolves to the musl package: the
            # rootfs already has it, and layering another copy over a working
            # loader is how a guest stops booting.
            continue
        chosen[pkg] = record
        for dep in " ".join(record.get("D", [])).split():
            queue.append(dep)
    return chosen, missing


def add_apk(tar, blob, added):
    """Copy one .apk's payload into the output archive.

    A member keeps its mode and its type - a symlink is written as a symlink,
    which the host's rootfs installer turns into a shadow entry rather than a
    host file. mtime and ownership are zeroed like everything else in this
    layer: two identical inputs have to produce identical bytes."""
    count = 0
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as apk:
        for member in sorted(apk.getmembers(), key=lambda m: m.name):
            name = member.name[2:] if member.name.startswith("./") else member.name
            if not name or name.startswith(".SIGN") or name.startswith(".PKGINFO"):
                continue
            if member.isdir():
                if name in added:
                    continue
                tar_dir(tar, name)
                added.add(name)
                continue
            if not (member.isfile() or member.issym() or member.islnk()):
                # Device nodes and fifos have no business in a bundle.
                continue
            if name in added:
                continue
            info = tarfile.TarInfo(name)
            info.mode = member.mode
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if member.isfile():
                info.size = member.size
                tar.addfile(info, apk.extractfile(member))
            elif member.issym():
                info.type = tarfile.SYMTYPE
                info.linkname = member.linkname
                tar.addfile(info)
            else:
                info.type = tarfile.LNKTYPE
                info.linkname = member.linkname.lstrip("./")
                tar.addfile(info)
            added.add(name)
            count += 1
    return count


def main():
    ap = argparse.ArgumentParser(description="Pack the VirtPass system layer")
    ap.add_argument("--src", help="directory holding <name>.exe")
    ap.add_argument("--out", help="the system.tar.gz to write")
    ap.add_argument("--pkgs", default="",
                    help="space separated Alpine packages to lay into the layer"
                         " with their dependencies (e.g. \"curl sqlite python3\")")
    ap.add_argument("--mirror", default=DEFAULT_MIRROR)
    ap.add_argument("--branch", default=DEFAULT_BRANCH)
    ap.add_argument("--arch", default=DEFAULT_ARCH)
    ap.add_argument("--cache", default=None,
                    help="directory to keep downloaded apk/APKINDEX in, so a"
                         " rebuild does not need the network")
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
    apk_packed = []
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

                if opts.pkgs:
                    wanted = [p for p in opts.pkgs.replace(",", " ").split() if p]
                    indexes = [load_index(opts.mirror, opts.branch, opts.arch,
                                          repo, opts.cache)
                               for repo in ("main", "community")]
                    chosen, missing = resolve_packages(wanted, indexes,
                                                       ALREADY_IN_ROOTFS)
                    for token in wanted:
                        name = dep_name(token)
                        if name and name not in chosen and name not in ALREADY_IN_ROOTFS:
                            return die("package %r is not in the %s/%s index"
                                       % (name, opts.branch, opts.arch))
                    if missing:
                        # Usually a soname the rootfs's own musl provides. It is
                        # worth printing: a real gap shows up here first.
                        print("pack_system: not in the index (assumed to come"
                              " from the rootfs): %s"
                              % ", ".join(sorted(set(missing))))
                    added = {guest for _n, guest, _s in packed}
                    for pkg in sorted(chosen):
                        record = chosen[pkg]
                        filename = "%s-%s.apk" % (pkg, record["V"][0])
                        cache = os.path.join(opts.cache, filename) if opts.cache else None
                        url = "%s/%s/%s/%s/%s" % (opts.mirror, opts.branch,
                                                  record["repo"][0], opts.arch,
                                                  filename)
                        blob = http_get(url, cache)
                        count = add_apk(tar, blob, added)
                        apk_packed.append((filename, count))

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
    for filename, count in apk_packed:
        print("apk    %-40s %d file(s)" % (filename, count))
    print("wrote %s: %d entry(ies), %d package(s), %d member(s), %d byte(s)"
          % (opts.out, len(packed), len(apk_packed), len(members), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())