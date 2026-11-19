#!/usr/bin/env python3
"""supervisor selftest - token auth, path projection, and the guest hostfs gate.

Runs a throwaway daemon against a *test* release tree (never the tree a user's
daemon is serving), starts one instance, and checks the API from the outside:

  - /api/health and /api/instances without a token -> 401;
  - with the token -> 200, and no absolute host paths or registry endpoints;
  - the guest's own mount -t hostfs is refused unless the host granted it
    (this daemon is started without --allow-guest-hostfs, so the default is the
    one under test).

Usage:
    python tools/supervisor/selftest.py --release <test release tree>
    python tools/supervisor/selftest.py --release <tree> --daemon-port 7912 \
        --instance-port 7940
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SUPERVISOR = os.path.join(HERE, "supervisor.py")
KEY_REL = os.path.join("runtime", "supervisor", "id_ed25519")
TOKEN_REL = os.path.join("runtime", "supervisor", "token")
KNOWN_HOSTS = "NUL" if os.name == "nt" else "/dev/null"


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    return 0 if ok else 1


def api(token, method, path, body=None, port=7912, timeout=120):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                 method=method)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data, timeout=timeout) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        return e.code, {}


def wait_up(port, proc, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            code, _ = api("", "GET", "/api/health", port=port, timeout=2)
            if code in (200, 401):
                return True
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(0.2)
    return False


def ssh_command(key, port, command):
    return ["ssh", "-i", key, "-p", str(port),
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=" + KNOWN_HOSTS,
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "root@127.0.0.1", command]


def run_ssh(key, port, command, timeout=60):
    return subprocess.run(ssh_command(key, port, command),
                          capture_output=True, text=True, timeout=timeout)


def no_host_paths(obj):
    text = json.dumps(obj)
    bad = re.search(r"[A-Za-z]:[\\/]", text) or "github_repos" in text or \
        "linswin" in text
    return not bad


def main():
    ap = argparse.ArgumentParser(description="supervisor selftest")
    ap.add_argument("--release", required=True,
                    help="test release tree (rvvm_ash + bundle/), not a user's")
    ap.add_argument("--daemon-port", type=int, default=7912)
    ap.add_argument("--instance-port", type=int, default=7940)
    ap.add_argument("--token", default=None,
                    help="explicit token; otherwise read the generated file")
    args = ap.parse_args()

    release = os.path.abspath(args.release)
    exe = os.path.join(release, "rvvm_ash_x86_64.exe")
    if not os.path.isfile(exe):
        exe = os.path.join(release, "rvvm_ash")
    bundle = os.path.join(release, "bundle")
    if not os.path.isfile(exe) or not os.path.isdir(bundle):
        print("FAIL release %s is not a test tree with rvvm_ash + bundle/"
              % release)
        return 2

    tmp = tempfile.mkdtemp(prefix="rvvm-sup-selftest-")
    db = os.path.join(tmp, "sup.db")
    out = os.path.join(tmp, "daemon.out")
    err = os.path.join(tmp, "daemon.err")
    hostdir = os.path.join(tmp, "hostdir")
    os.makedirs(hostdir)
    with open(os.path.join(hostdir, "marker.txt"), "w", encoding="ascii") as f:
        f.write("HOSTFS-MARKER")

    fails = 0
    proc = None
    try:
        logf = open(out, "wb")
        errf = open(err, "wb")
        proc = subprocess.Popen(
            [sys.executable, SUPERVISOR, "--release", release, "--db", db,
             "--bind", "127.0.0.1:%d" % args.daemon_port],
            cwd=HERE, stdout=logf, stderr=errf)
        if not wait_up(args.daemon_port, proc):
            print("FAIL daemon did not answer")
            return 1

        code, _ = api("", "GET", "/api/health", port=args.daemon_port)
        fails += check(code == 401, "health without token is 401 (got %s)" % code)

        token = args.token
        if not token:
            try:
                with open(os.path.join(release, TOKEN_REL), "r",
                          encoding="utf-8") as f:
                    token = f.read().strip()
            except OSError:
                token = ""
        fails += check(bool(token), "daemon has a token")
        if not token:
            return 1

        code, health = api(token, "GET", "/api/health",
                           port=args.daemon_port)
        fails += check(code == 200, "health with token is 200 (got %s)" % code)
        fails += check(no_host_paths(health), "health has no host paths")
        fails += check("endpoint" not in json.dumps(health),
                       "health has no registry endpoint")
        fails += check(not any(sep in str(health.get("release", ""))
                               for sep in ("/", "\\")),
                       "health release is a basename")

        second_port = args.instance_port + 1
        started = []
        for port, label in ((args.instance_port, "selftest"),
                            (second_port, "selftest-2")):
            code, body = api(token, "POST", "/api/instances",
                             {"label": label, "port": port},
                             port=args.daemon_port)
            fails += check(code == 200 and body.get("actual") == "running",
                           "instance %d starts (got %s %s)"
                           % (port, code, body))
            if code != 200 or body.get("actual") != "running":
                with open(err, "r", encoding="utf-8", errors="replace") as f:
                    print(f.read()[-4000:])
                for p in started:
                    api(token, "DELETE", "/api/instances/%d" % p,
                        port=args.daemon_port)
                return 1
            started.append(port)
        try:
            code, listing = api(token, "GET", "/api/instances",
                                port=args.daemon_port)
            fails += check(code == 200,
                           "instances with token is 200 (got %s)" % code)
            fails += check(no_host_paths(listing), "instances has no host paths")
            insts = {i.get("port"): i
                     for i in (listing.get("instances") or [])}
            for port in (args.instance_port, second_port):
                inst = insts.get(port, {})
                fails += check(
                    "endpoint" not in json.dumps(inst.get("registry", {})),
                    "registry %d has no endpoint" % port)
                fails += check(
                    not any(sep in str(inst.get("rootfs", ""))
                            for sep in ("/", "\\")),
                    "rootfs %d is a basename" % port)
                fails += check(
                    not any(sep in str(inst.get("log_path", ""))
                            for sep in ("/", "\\")),
                    "log_path %d is a basename" % port)

            key = os.path.join(release, KEY_REL)
            p1, p2 = args.instance_port, second_port
            r = run_ssh(key, p1,
                        "test -f /run/vpsessiond/%d.ready; echo OWN=$?; "
                        "test -f /run/vpsessiond/%d.ready; echo OTHER=$?; "
                        "test -f /run/vpsessiond/auth.pub; echo AUTH=$?"
                        % (p1, p2))
            fails += check("OWN=0" in r.stdout,
                           "instance %d sees its own ready marker" % p1)
            fails += check("OTHER=1" in r.stdout,
                           "instance %d cannot see %d's ready marker" % (p1, p2))
            fails += check("AUTH=0" in r.stdout,
                           "instance %d has its own auth.pub" % p1)

            # C4: the mode store and rootfs lock are host-side sidecars, not
            # files under the guest's /. A rootfs chmod is persisted; a hostfs
            # chmod is not, because the store is keyed by host path.
            r = run_ssh(
                key, p1,
                "chmod 700 /etc/hostname; "
                "touch /run/vpsessiond/selftest-hostfs; "
                "chmod 600 /run/vpsessiond/selftest-hostfs; "
                "test ! -e /rootfs.modes; echo NOMODES=$?; "
                "test ! -e /.ash.lock; echo NOLOCK=$?")
            fails += check("NOMODES=0" in r.stdout,
                           "guest / has no rootfs.modes")
            fails += check("NOLOCK=0" in r.stdout,
                           "guest / has no .ash.lock")
            time.sleep(0.5)
            rootfs_name = insts.get(p1, {}).get("rootfs", "")
            if rootfs_name:
                rootfs = os.path.join(release, "runtime", "roots",
                                      rootfs_name)
                fails += check(os.path.isfile(rootfs + ".modes"),
                               "mode store lives beside the rootfs")
                fails += check(os.path.isfile(rootfs + ".lock"),
                               "rootfs lock lives beside the rootfs")
                try:
                    with open(rootfs + ".modes", "r",
                              encoding="utf-8", errors="replace") as f:
                        modes = f.read()
                except OSError:
                    modes = ""
                run_dir = os.path.join(release, "runtime", "run", str(p1))
                fails += check(run_dir not in modes and
                               run_dir.replace("\\", "/") not in modes,
                               "hostfs chmod is not persisted")

            r = run_ssh(key, p1,
                        "mkdir -p /hostdir; mount -t hostfs '%s' /hostdir; "
                        "echo RC=$?; rmdir /hostdir 2>/dev/null"
                        % hostdir.replace("\\", "/"))
            fails += check(
                re.search(r"RC=([1-9]\d*)", r.stdout) is not None,
                "guest hostfs is denied by default (%r)" % r.stdout.strip())

            # C5: df/statfs is the fixed guest budget, not the host volume.
            r = run_ssh(key, p1, "stat -f /; stat -f /run/vpsessiond")
            fails += check("Total: 262144" in r.stdout and
                           "Free: 196608" in r.stdout,
                           "root statfs is the synthetic 1 GiB budget (%r)"
                           % r.stdout.strip())
            fails += check("Total: 16384" in r.stdout and
                           "Free: 12288" in r.stdout,
                           "hostfs statfs is the synthetic 64 MiB budget (%r)"
                           % r.stdout.strip())
            fails += check("255012694" not in r.stdout and
                           "7131305" not in r.stdout,
                           "statfs has no host inode/usage numbers")

            # C6: host st_ino/st_dev never reach the guest; stat(2) and
            # getdents64 agree on the opaque inode.
            r = run_ssh(key, p1,
                        "stat -c '%i %d' /etc/hostname; "
                        "ls -i /etc/hostname")
            numbers = [line.split() for line in r.stdout.strip().splitlines()]
            guest_ino = int(numbers[0][0]) if numbers and numbers[0] else 0
            guest_dev = int(numbers[0][1]) if len(numbers[0]) > 1 else 0
            dir_ino = int(numbers[1][0]) if len(numbers) > 1 else 0
            fails += check(guest_dev == 0x52564D01,
                           "stat device is the fixed guest dev (%d)" % guest_dev)
            fails += check(guest_ino != 0 and guest_ino == dir_ino,
                           "stat and getdents agree on the guest inode")
            if rootfs_name:
                host_file = os.path.join(release, "runtime", "roots",
                                         rootfs_name, "etc", "hostname")
                try:
                    host_ino = os.stat(host_file).st_ino
                except OSError:
                    host_ino = None
                fails += check(host_ino is None or guest_ino != host_ino,
                               "guest inode is not the host st_ino")
        finally:
            for port in started:
                api(token, "DELETE", "/api/instances/%d" % port,
                    port=args.daemon_port)
        for port in started:
            name = insts.get(port, {}).get("rootfs", "")
            if not name:
                continue
            base = os.path.join(release, "runtime", "roots", name)
            for suffix in (".modes", ".lock"):
                fails += check(not os.path.exists(base + suffix),
                               "rm removed %s%s" % (name, suffix))
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    print("=== %s: supervisor selftest (%d failure(s)) ==="
          % ("PASS" if fails == 0 else "FAIL", fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
