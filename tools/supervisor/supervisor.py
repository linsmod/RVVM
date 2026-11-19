#!/usr/bin/env python3
"""userland supervisor daemon - manage N userland cores on one machine.

Python 3.9+, standard library only. Design: docs/design-userland-supervisor.md.
Instances are `rvvm_ash --serve --port N --rootfs <dir>` cores, one rootfs
directory per instance (runtime/roots/<uuid>), so several cores live beside
each other, each behind its own rootfs lock. Discovery is the cores' own
registry (runtime/cores/<port>.core, key=value: pid= / shell= / endpoint=);
logs go through RVVM_LOG_FILE; commands run over the cores' ssh endpoint
(vpshhd -> tinysshnoneauthd, the vpssh link).

The database records DESIRED state and metadata only. Liveness truth is the
core registry plus the pid: on startup (and on every list) the two are
reconciled, so a daemon restart never loses or double-books an instance.
"""

import argparse
import base64
import ctypes
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DAEMON_VERSION = "0.2.0"
STARTING_GRACE_S = 25      # ceiling for a start, not the typical time: a core
                           # registers in ~2 s and its ssh answers right after
READY_FALLBACK_S = 8       # by then a TCP connect may stand in for the marker
                           # (a guest built before vpshhd wrote one)
STOP_GRACE_S = 10
EXEC_TIMEOUT_S = 60
PORT_BASE = 7910           # 7900 is the dev/test port, instances start above it
TOKEN_ENV = "SUPERVISOR_TOKEN"

# ---------------------------------------------------------------- utils


def log_line(msg):
    print(time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg, flush=True)


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


BUNDLE_NEEDS = ("rootfs.tar.gz", "system.tar.gz", "apps.tar.gz")


def core_binary(release):
    """rvvm_ash inside a release tree, .exe spelling first."""
    exe = os.path.join(release, "rvvm_ash_x86_64.exe")
    return exe if os.path.isfile(exe) else os.path.join(release, "rvvm_ash")


def release_missing(release):
    """What keeps this tree from booting an instance: the core binary, or one of
    the bundle archives it unpacks at startup.

    A build tree - `make bin` into a scratch BUILDDIR, or a debug tree - has the
    binary but no bundle. Picking one used to look like a start that silently
    failed 25 seconds later ("no live core"), while the actual cause was a
    directory choice nobody could see. Now a tree either has all four pieces or
    it is skipped with the reason."""
    missing = []
    if not os.path.isfile(os.path.join(release, "rvvm_ash_x86_64.exe")) and \
            not os.path.isfile(os.path.join(release, "rvvm_ash")):
        missing.append("rvvm_ash")
    for name in BUNDLE_NEEDS:
        if not os.path.isfile(os.path.join(release, "bundle", name)):
            missing.append("bundle/" + name)
    return missing


def find_release_dir(explicit):
    """The release tree holding rvvm_ash / vp *and* the bundle an instance boots
    from. Explicit wins and is validated; otherwise every release.* directory is
    checked and the one with the newest core binary wins, so a freshly built
    tree is the one picked - never a half-built one that only looks usable."""
    if explicit:
        full = os.path.abspath(explicit)
        missing = release_missing(full)
        if missing:
            raise SystemExit("supervisor: %s cannot run instances: missing %s"
                             % (full, ", ".join(missing)))
        return full
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    usable, skipped = [], []
    for name in sorted(os.listdir(repo)):
        full = os.path.join(repo, name)
        if not (name.startswith("release.") and os.path.isdir(full)):
            continue
        missing = release_missing(full)
        if missing:
            skipped.append((name, missing))
            continue
        usable.append(full)
    if not usable:
        lines = ["supervisor: no release.* tree under %s can run instances" % repo]
        for name, missing in skipped:
            lines.append("  %s: missing %s" % (name, ", ".join(missing)))
        if not skipped:
            lines.append("  (no release.* directory at all - build one, or pass --release)")
        raise SystemExit("\n".join(lines))
    usable.sort(key=lambda p: -os.path.getmtime(core_binary(p)))
    chosen = usable[0]
    if len(usable) > 1:
        log_line("release trees: %s -> using %s (newest rvvm_ash; each tree keeps "
                 "its own runtime/ and database)"
                 % (", ".join(os.path.basename(p) for p in usable),
                    os.path.basename(chosen)))
    if skipped:
        log_line("release trees skipped: %s"
                 % "; ".join("%s (missing %s)" % (name, ", ".join(missing))
                             for name, missing in skipped))
    return chosen


def probe_daemon(host, port, timeout=2.0):
    """Who already answers on this bind address, or None.

    Two daemons on one port is not a modelling problem, it is a data problem:
    SO_REUSEADDR lets the second bind while the first is listening, requests
    land on either one, and each keeps its own database - so `stop` works half
    the time and two `list` calls disagree. Refusing to start is the cheap
    guard; the message names the daemon that holds the port."""
    url = "http://%s:%d/api/health" % (host, port)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            info = json.loads(r.read().decode("utf-8", "replace"))
        return "daemon %s, release %s" % (info.get("version", "?"),
                                          info.get("release", "?"))
    except urllib.error.HTTPError as e:
        # Something answers, it just wants a token we do not have.
        return "HTTP %d (token required)" % e.code
    except Exception:
        return None


def exe_path(release, stem):
    """rvvm_ash / vp inside the release tree, .exe spelling first."""
    for name in (stem + "_x86_64.exe", stem + "_x86_64", stem + ".exe", stem):
        p = os.path.join(release, name)
        if os.path.isfile(p):
            return p
    raise SystemExit("supervisor: %s not found in %s" % (stem, release))


# ---------------------------------------------------------------- pid liveness

def pid_alive(pid):
    if not pid or pid <= 0:
        return False
    if os.name == "nt":
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return False
        try:
            code = ctypes.c_ulong()
            if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                return False
            return code.value == STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, owned by someone else


def pid_terminate(pid):
    """Ask once, then force. Returns when the pid is gone (or best effort)."""
    if os.name == "nt":
        PROCESS_TERMINATE = 0x0001
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(PROCESS_TERMINATE, False, pid)
        if h:
            k32.TerminateProcess(h, 1)
            k32.CloseHandle(h)
    else:
        import signal
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.time() + 5
        while time.time() < deadline and pid_alive(pid):
            time.sleep(0.2)
        if pid_alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def tcp_open(port, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        return s.connect_ex(("127.0.0.1", port)) == 0
    finally:
        s.close()


# ---------------------------------------------------------------- core registry

CORE_RE = re.compile(r"^\d+\.core$")


def cores_dir(release):
    return os.path.join(release, "runtime", "cores")


def read_core(path):
    """Parse the key=value registry file a core writes for itself."""
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\r\n").split("=", 1)
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def core_snapshot(release):
    """port -> {pid, shell, endpoint, alive} for every registry file."""
    snap = {}
    d = cores_dir(release)
    if not os.path.isdir(d):
        return snap
    for name in os.listdir(d):
        if not CORE_RE.match(name):
            continue
        port = int(name.split(".")[0])
        fields = read_core(os.path.join(d, name))
        try:
            pid = int(fields.get("pid", "0"))
        except ValueError:
            pid = 0
        snap[port] = {
            "pid": pid,
            "shell": fields.get("shell", ""),
            "endpoint": fields.get("endpoint", ""),
            "alive": pid_alive(pid),
        }
    return snap


def public_core(info):
    """What an API client may know about a core: no host endpoint path.

    The registry's endpoint is a host path (the socket/session endpoint), and
    the pid is host metadata. The API is the daemon's admin surface, but it is
    still reachable from the guest's loopback (the guest shares the host
    network stack), so the token and this projection are two layers, not one."""
    if not info:
        return None
    return {
        "pid": info.get("pid", 0),
        "shell": info.get("shell", ""),
        "alive": bool(info.get("alive")),
    }


# ---------------------------------------------------------------- token

def token_path(release):
    return os.path.join(release, "runtime", "supervisor", "token")


def load_or_create_token(release):
    """Return (token, path, generated). A token is mandatory: loopback is not
    an authentication boundary here, because a guest's sockets are the host's
    sockets and a guest sees 127.0.0.1. The file is the stable, copyable half
    for the CLI and the web UI; an explicit --token stays the caller's own."""
    path = token_path(release)
    try:
        with open(path, "r", encoding="utf-8") as f:
            token = f.read().strip()
        if token:
            return token, path, False
    except OSError:
        pass
    token = secrets.token_urlsafe(32)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(token + "\n")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return token, path, True


# ---------------------------------------------------------------- database

class DB:
    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA busy_timeout=5000")
        self.conn.executescript("""
        CREATE TABLE IF NOT EXISTS instances (
            port       INTEGER PRIMARY KEY,
            label      TEXT,
            desired    TEXT NOT NULL,   -- running | stopped
            actual     TEXT NOT NULL,   -- starting | running | failed |
                                        -- stopped | crashed | orphan
            rootfs     TEXT,            -- per-instance rootfs directory
            pid        INTEGER,
            started_at TEXT,
            stopped_at TEXT,
            log_path   TEXT,
            note       TEXT
        );
        CREATE TABLE IF NOT EXISTS events (
            id     INTEGER PRIMARY KEY AUTOINCREMENT,
            ts     TEXT NOT NULL,
            port   INTEGER,
            kind   TEXT NOT NULL,
            detail TEXT
        );
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT
        );
        """)

    def event(self, port, kind, detail=""):
        self.conn.execute(
            "INSERT INTO events(ts, port, kind, detail) VALUES(?,?,?,?)",
            (now_iso(), port, kind, detail))
        self.conn.commit()
        log_line("event port=%s kind=%s %s" % (port, kind, detail))

    def instances(self):
        return self.conn.execute(
            "SELECT * FROM instances ORDER BY port").fetchall()

    def instance(self, port):
        return self.conn.execute(
            "SELECT * FROM instances WHERE port=?", (port,)).fetchone()

    def set(self, port, **kw):
        cols = ",".join(k + "=?" for k in kw)
        self.conn.execute("UPDATE instances SET " + cols + " WHERE port=?",
                          (*kw.values(), port))
        self.conn.commit()

    def insert(self, **kw):
        keys = ",".join(kw)
        marks = ",".join("?" for _ in kw)
        self.conn.execute("INSERT INTO instances(" + keys + ") VALUES(" +
                          marks + ")", tuple(kw.values()))
        self.conn.commit()

    def delete(self, port):
        self.conn.execute("DELETE FROM instances WHERE port=?", (port,))
        self.conn.commit()

    def meta_get(self, key, default=None):
        row = self.conn.execute("SELECT value FROM meta WHERE key=?",
                                (key,)).fetchone()
        return row["value"] if row else default

    def meta_set(self, key, value):
        self.conn.execute(
            "INSERT INTO meta(key, value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (key, str(value)))
        self.conn.commit()


# ---------------------------------------------------------------- supervisor

class Supervisor:
    def __init__(self, release, db, grace=STARTING_GRACE_S):
        self.release = os.path.abspath(release)
        self.db = db
        self.grace = grace
        self.ash = exe_path(self.release, "rvvm_ash")
        self.sup_dir = os.path.join(self.release, "runtime", "supervisor")
        os.makedirs(self.sup_dir, exist_ok=True)
        self.ssh_key, self.ssh_pub = self._ensure_ssh_key()

    def _ensure_ssh_key(self):
        """One daemon-owned ed25519 pair: the private key is what the daemon
        (and only it) uses to reach instances. The public half is written into
        each instance's own run directory by start(), where vpshhd plants it
        for root and switches tinysshd to publickey auth. Without auth.pub the
        guest keeps the dev none-auth behaviour."""
        key = os.path.join(self.sup_dir, "id_ed25519")
        pub = key + ".pub"
        if not os.path.isfile(key):
            subprocess.run(["ssh-keygen", "-t", "ed25519", "-N", "", "-q",
                            "-f", key], check=True)
            log_line("generated instance ssh key %s" % key)
        return key, pub

    def run_dir(self, port):
        """<release>/runtime/run/<port>: this instance's /run/vpsessiond."""
        return os.path.join(self.release, "runtime", "run", str(port))

    def ensure_run_dir(self, port):
        d = self.run_dir(port)
        os.makedirs(d, exist_ok=True)
        return d

    def write_auth_pub(self, run_dir):
        """Drop the public half into the instance's run directory. The comment
        is stripped: tinysshd matches algorithm + base64 only."""
        with open(self.ssh_pub, "r", encoding="utf-8") as f:
            publine = f.read().strip().split()
        with open(os.path.join(run_dir, "auth.pub"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write("%s %s\n" % (publine[0], publine[1]))

    def ssh_args(self, port):
        """Common ssh spelling for reaching instance <port>: key auth,
        no host-key prompts, a pty for interactive sessions (-tt)."""
        known_hosts = "NUL" if os.name == "nt" else "/dev/null"
        return ["ssh", "-tt", "-i", self.ssh_key,
                "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=" + known_hosts,
                "-o", "BatchMode=yes",
                "-o", "ConnectTimeout=10",
                "-p", str(port), "root@127.0.0.1"]

    # -- reconcile ---------------------------------------------------------

    def reconcile(self):
        """Align DB rows with the core registry. Returns the instance list."""
        snap = core_snapshot(self.release)
        seen = set()
        for row in self.db.instances():
            port = row["port"]
            live = port in snap and snap[port]["alive"]
            if row["actual"] in ("starting", "running"):
                if live:
                    if row["actual"] == "starting" or row["pid"] != snap[port]["pid"]:
                        self.db.set(port, actual="running",
                                    pid=snap[port]["pid"])
                    seen.add(port)
                else:
                    # Starting up? The grace window is the register's own
                    # convention: a core writes its file late on purpose.
                    fresh = row["started_at"] and \
                        row["actual"] == "starting" and \
                        self._starting_age_s(row) < self.grace
                    if fresh:
                        seen.add(port)
                        continue
                    self.db.set(port, actual="crashed", pid=None,
                                stopped_at=now_iso())
                    self.db.event(port, "crashed",
                                  "core registry gone or pid dead")
                    self.drop_ready(port)
            elif row["actual"] in ("crashed", "stopped", "failed", "orphan"):
                if live:
                    # A stopped record with a live core means someone started
                    # it outside the daemon: report, do not kill.
                    self.db.set(port, actual="orphan",
                                pid=snap[port]["pid"])
                    self.db.event(port, "orphan",
                                  "live core outside the daemon")
                    seen.add(port)
                elif port in snap:
                    # Stale registry file for a dead pid: reclaim it, the C
                    # side would do the same on its next --serve.
                    try:
                        os.remove(os.path.join(cores_dir(self.release),
                                               "%d.core" % port))
                    except OSError:
                        pass
        for port, info in snap.items():
            if port in seen:
                continue
            if not info["alive"]:
                try:
                    os.remove(os.path.join(cores_dir(self.release),
                                           "%d.core" % port))
                except OSError:
                    pass
                continue
            self.db.insert(port=port, label=None, desired="running",
                           actual="orphan", rootfs=None, pid=info["pid"],
                           started_at=now_iso(),
                           log_path=self.log_path(port), note=info["shell"])
            self.db.event(port, "orphan", "registered core without DB row")
        return self.listing()

    def _starting_age_s(self, row):
        try:
            t = time.mktime(time.strptime(row["started_at"],
                                          "%Y-%m-%dT%H:%M:%S"))
            return time.time() - t
        except (ValueError, TypeError):
            return 0

    def log_path(self, port):
        return os.path.join(self.sup_dir, "%d.log" % port)

    # -- readiness ---------------------------------------------------------

    def ready_path(self, port):
        """Where the guest's ssh listener says it is up. Each instance mounts
        its own run directory at /run/vpsessiond, so this is
        runtime/run/<port>/<port>.ready; vpshhd's marker is visible here the
        moment listen() succeeded - no TCP probe, no guessing from the registry
        (which appears before the guest has even booted)."""
        return os.path.join(self.run_dir(port), "%d.ready" % port)

    def legacy_ready_path(self, port):
        """The pre-per-instance shared marker, still checked for an instance
        started by an older daemon (and removed on the next start/stop)."""
        return os.path.join(self.release, "runtime", "run", "%d.ready" % port)

    def ready_present(self, port):
        return os.path.isfile(self.ready_path(port)) or \
            os.path.isfile(self.legacy_ready_path(port))

    def drop_ready(self, port):
        for path in (self.ready_path(port), self.legacy_ready_path(port)):
            try:
                os.remove(path)
            except OSError:
                pass

    def wait_ready(self, port, started_at, child_pid=None):
        """("ready"|"dead"|"timeout", pid). The marker is the answer; a live
        core with no marker after READY_FALLBACK_S falls back to a TCP connect,
        logged, so an older guest still works instead of failing the start."""
        deadline = started_at + self.grace
        seen_pid = None
        while time.time() < deadline:
            time.sleep(0.25)
            info = core_snapshot(self.release).get(port)
            if info and info["alive"]:
                seen_pid = info["pid"]
                if self.ready_present(port):
                    return "ready", seen_pid
                if time.time() - started_at >= READY_FALLBACK_S and tcp_open(port):
                    log_line("port %d: ssh answers but no readiness marker - "
                             "guest predates it? using the TCP probe" % port)
                    return "ready", seen_pid
            elif info is not None:
                # The registry is there and its pid is gone: the core died.
                return "dead", seen_pid
            elif child_pid and time.time() - started_at > 1.0 and \
                    not pid_alive(child_pid):
                # Died before it ever wrote a registry file.
                return "dead", seen_pid
        return "timeout", seen_pid

    # -- listing -----------------------------------------------------------

    def public_instance(self, item):
        """The API view of a row: no absolute host paths.

        rootfs/log_path are useful to the daemon, not to a client that could be
        the guest itself. The basenames keep them identifiable; the full paths
        stay in the database and the daemon's own log."""
        item = dict(item)
        for key in ("rootfs", "log_path"):
            if item.get(key):
                item[key] = os.path.basename(item[key])
        item["registry"] = public_core(item.get("registry"))
        note = item.get("note")
        if note:
            item["note"] = note.replace(self.release,
                                        os.path.basename(self.release))
        return item

    def listing(self):
        snap = core_snapshot(self.release)
        out = []
        for row in self.db.instances():
            item = dict(row)
            item["tcp"] = tcp_open(row["port"]) if row["actual"] in (
                "starting", "running") else False
            item["ssh_ready"] = (row["actual"] == "running" and
                                 self.ready_present(row["port"]))
            item["registry"] = snap.get(row["port"])
            out.append(self.public_instance(item))
        return out

    # -- port allocation ---------------------------------------------------

    def alloc_port(self, wanted=None):
        snap = core_snapshot(self.release)
        if wanted is not None:
            if wanted in snap and snap[wanted]["alive"]:
                raise ApiError(409, "port %d holds a live core" % wanted)
            if tcp_open(wanted):
                raise ApiError(409, "port %d is in use by another process"
                               % wanted)
            return wanted
        high = self.db.meta_get("next_port", PORT_BASE)
        try:
            high = int(high)
        except (TypeError, ValueError):
            high = PORT_BASE
        port = max(high, PORT_BASE)
        while True:
            if port not in snap and not tcp_open(port, 0.3):
                self.db.meta_set("next_port", port + 1)
                return port
            port += 1

    # -- lifecycle ---------------------------------------------------------

    def new_rootfs_dir(self, port):
        """One rootfs directory per instance: the bundle materializes here and
        the core's rootfs lock lives inside it, so instances on distinct
        directories never contend. <release>/runtime/roots/<port>-<uuid>."""
        d = os.path.join(self.release, "runtime", "roots",
                         "%d-%s" % (port, uuid.uuid4().hex[:8]))
        os.makedirs(d, exist_ok=True)
        return d

    def start(self, port, label=None, memfs=False):
        if memfs:
            raise ApiError(400, "memfs mode is broken (guest exits 1); "
                           "see issues/INDEX.md")
        missing = release_missing(self.release)
        if missing:
            # Say why, instead of starting a process that cannot boot and
            # leaving a "failed" row 25 seconds later. The release is named by
            # its basename: the API answer must not carry a host path.
            raise ApiError(500, "release %s cannot run instances: missing %s"
                           % (os.path.basename(self.release),
                              ", ".join(missing)))
        logp = self.log_path(port)
        rootfs = self.new_rootfs_dir(port)
        # One run directory per instance: auth.pub, the readiness marker and the
        # guest's vpshhd/tinysshd logs go here and are mounted at
        # /run/vpsessiond. A shared directory let one guest read or overwrite
        # another instance's state.
        try:
            run_dir = self.ensure_run_dir(port)
            self.write_auth_pub(run_dir)
        except OSError:
            raise ApiError(500, "could not prepare the instance run directory")
        # Whatever is in this instance's directory belongs to a previous run,
        # never to the one about to boot (the legacy shared marker too).
        self.drop_ready(port)
        env = dict(os.environ)
        env["RVVM_LOG_FILE"] = logp
        argv = [self.ash, "--serve", "--port", str(port),
                "--rootfs", rootfs, "--run-dir", run_dir]
        flags = 0
        kwargs = {}
        if os.name == "nt":
            DETACHED_PROCESS = 0x00000008
            CREATE_NEW_PROCESS_GROUP = 0x00000200
            flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
            kwargs["creationflags"] = flags
            kwargs["stdin"] = subprocess.DEVNULL
        else:
            kwargs["start_new_session"] = True
        logf = open(logp, "ab")
        try:
            proc = subprocess.Popen(argv, cwd=self.release, env=env,
                                    stdout=logf, stderr=logf, **kwargs)
        except OSError as e:
            logf.close()
            raise ApiError(500, "could not spawn %s: %s"
                           % (os.path.basename(self.ash), e))
        try:
            self.db.insert(port=port, label=label, desired="running",
                           actual="starting", rootfs=rootfs,
                           pid=None, started_at=now_iso(), log_path=logp)
        except Exception:
            # The core is already running; a bookkeeping failure must not
            # orphan it - it would hold the port and its rootfs lock with
            # nothing in the database to stop it.
            snap = core_snapshot(self.release)
            if snap.get(port, {}).get("alive"):
                pid_terminate(snap[port]["pid"])
            try:
                os.remove(os.path.join(cores_dir(self.release),
                                       "%d.core" % port))
            except OSError:
                pass
            logf.close()
            raise ApiError(500, "core started but could not be recorded; "
                           "it was terminated to avoid an orphan")
        self.db.event(port, "start", os.path.basename(rootfs))
        started_at = time.time()
        outcome, pid = self.wait_ready(port, started_at, proc.pid)
        if outcome == "ready":
            self.db.set(port, actual="running", pid=pid)
            self.db.event(port, "running", "pid=%s, sshd ready" % pid)
            return {"port": port, "actual": "running", "pid": pid}
        if outcome == "dead":
            self.db.set(port, actual="failed", stopped_at=now_iso(),
                        note="core exited before sshd was ready - see %s"
                             % os.path.basename(logp))
            self.db.event(port, "failed", "core exited before sshd was ready")
            return {"port": port, "actual": "failed"}
        self.db.set(port, actual="failed", stopped_at=now_iso(),
                    note="sshd not ready within %ds - see %s"
                         % (self.grace, os.path.basename(logp)))
        self.db.event(port, "failed", "sshd not ready within %ds" % self.grace)
        return {"port": port, "actual": "failed"}

    def stop(self, port):
        row = self.db.instance(port)
        snap = core_snapshot(self.release)
        pid = snap.get(port, {}).get("pid") or (row["pid"] if row else None)
        if pid and pid_alive(pid):
            pid_terminate(pid)
        deadline = time.time() + STOP_GRACE_S
        gone = False
        while time.time() < deadline:
            if port not in core_snapshot(self.release):
                gone = True
                break
            time.sleep(0.3)
        if not gone:
            # A terminated core cannot run its own cleanup: the registry file
            # it would have removed is now stale, and the same reclaim the C
            # side does before a --serve applies here. A core that is still
            # genuinely alive after the grace is a real error.
            still = core_snapshot(self.release).get(port)
            if still and not still["alive"]:
                try:
                    os.remove(os.path.join(cores_dir(self.release),
                                           "%d.core" % port))
                except OSError:
                    pass
            elif still:
                raise ApiError(500, "core on port %d did not release its "
                               "registration in %ds" % (port, STOP_GRACE_S))
        if row:
            self.db.set(port, desired="stopped", actual="stopped", pid=None,
                        stopped_at=now_iso())
        self.db.event(port, "stopped", "")
        self.drop_ready(port)

    # -- exec ---------------------------------------------------------------

    def exec_cmd(self, port, cmd):
        """One command through the instance's ssh endpoint, key-authenticated."""
        argv = self.ssh_args(port)
        argv.remove("-tt")  # one-shot command needs no pty
        argv.append(cmd)
        try:
            p = subprocess.run(argv, capture_output=True,
                               timeout=EXEC_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise ApiError(504, "exec timed out after %ds" % EXEC_TIMEOUT_S)
        except OSError as e:
            raise ApiError(500, "could not spawn ssh: %s" % e)
        return {
            "exit": p.returncode,
            "stdout": p.stdout.decode("utf-8", "replace"),
            "stderr": p.stderr.decode("utf-8", "replace"),
        }

    def tail_log(self, port, n=200):
        row = self.db.instance(port)
        path = row["log_path"] if row else self.log_path(port)
        if not os.path.isfile(path):
            return []
        with open(path, "rb") as f:
            lines = f.read().decode("utf-8", "replace").splitlines()
        return lines[-n:]


class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


# ---------------------------------------------------------------- http

def make_handler(sup, token):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # route through our log
            log_line("http " + (fmt % args))

        def _authed(self):
            # Loopback is not an authentication boundary: a guest's sockets are
            # the host's sockets, so a guest reaches this API as 127.0.0.1.
            # The token is mandatory (main() always has one).
            return hmac.compare_digest(self.headers.get("Authorization", ""),
                                       "Bearer " + token)

        def _send(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type",
                             "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n) or b"{}")

        def _ws_terminal(self, port, qs):
            """The web terminal: ws handshake, then a byte pump between the
            browser and a key-authenticated ssh session on the instance.
            Runs on this request's thread until either side goes away."""
            key = self.headers.get("Sec-WebSocket-Key")
            if not key or (self.headers.get("Upgrade", "").lower()
                           != "websocket"):
                return self._send(400, {"error": "websocket upgrade required"})
            accept = base64.b64encode(hashlib.sha1(
                (key + WS_GUID).encode()).digest()).decode()
            self.connection.sendall(
                b"HTTP/1.1 101 Switching Protocols\r\n"
                b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n\r\n")

            cols = min(int(qs.get("cols", ["120"])[0]) or 120, 500)
            rows = min(int(qs.get("rows", ["32"])[0]) or 32, 200)
            argv = sup.ssh_args(port)
            argv.append("stty rows %d cols %d; exec /bin/sh -l" %
                        (rows, cols))
            try:
                p = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     bufsize=0)
            except OSError as e:
                ws_send(self.connection, ("ssh: %s" % e).encode(), 0x1)
                return

            def pump_out():
                try:
                    while True:
                        chunk = p.stdout.read(4096)
                        if not chunk:
                            break
                        ws_send(self.connection, chunk, 0x2)
                except OSError:
                    pass
                finally:
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass

            threading.Thread(target=pump_out, daemon=True).start()
            try:
                while True:
                    frame = ws_read_frame(self.rfile)
                    if frame is None:
                        break
                    op, payload = frame
                    if op == 0x8:  # close
                        break
                    if op == 0x9:  # ping -> pong
                        ws_send(self.connection, payload, 0xA)
                        continue
                    if op in (0x1, 0x2) and payload and p.poll() is None:
                        p.stdin.write(payload)
                        p.stdin.flush()
            except OSError:
                pass
            finally:
                if p.poll() is None:
                    p.kill()
                self.connection.close()
                log_line("ws terminal closed port=%d" % port)

        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path
            if path in ("/", "/index.html"):
                code, ctype, body = serve_index()
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return
            if path.startswith("/vendor/"):
                code, ctype, body = serve_vendor(path[len("/vendor/"):])
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path.startswith("/ws/"):
                # The browser cannot put a bearer header on a WebSocket, so
                # the token rides the query string. No loopback exemption: the
                # guest is loopback too.
                qs = urllib.parse.parse_qs(parsed.query)
                tok = qs.get("token", [""])[0]
                if not (tok and hmac.compare_digest(tok, token)):
                    return self._send(401, {"error": "unauthorized"})
                try:
                    port = int(path[len("/ws/"):])
                except ValueError:
                    return self._send(400, {"error": "bad request"})
                return self._ws_terminal(port, qs)
            if not self._authed():
                return self._send(401, {"error": "unauthorized"})
            qs = urllib.parse.parse_qs(parsed.query)
            parts = path.strip("/").split("/")
            try:
                if path == "/api/health":
                    return self._send(200, {
                        "version": DAEMON_VERSION,
                        "release": os.path.basename(sup.release),
                        "cores": {p: public_core(info) for p, info in
                                  core_snapshot(sup.release).items()},
                    })
                if path == "/api/instances":
                    return self._send(200, {"instances": sup.reconcile()})
                if len(parts) == 3 and parts[0] == "api" and \
                        parts[1] == "instances" and parts[2] == "log":
                    port = int(parts[2])
                    n = int(qs.get("tail", ["200"])[0])
                    return self._send(200, {"log": sup.tail_log(port, n)})
                return self._send(404, {"error": "not found"})
            except (ValueError, IndexError):
                return self._send(400, {"error": "bad request"})
            except ApiError as e:
                return self._send(e.status, {"error": e.message})

        def do_POST(self):
            if not self._authed():
                return self._send(401, {"error": "unauthorized"})
            parts = urllib.parse.urlparse(self.path).path.strip("/").split("/")
            body = self._body()
            try:
                if parts[:2] == ["api", "instances"] and len(parts) == 2:
                    port = sup.alloc_port(body.get("port"))
                    return self._send(200, sup.start(
                        port, label=body.get("label"),
                        memfs=bool(body.get("memfs", False))))
                if parts[:2] == ["api", "instances"] and len(parts) == 4:
                    port = int(parts[2])
                    if parts[3] == "start":
                        row = sup.db.instance(port)
                        if row:
                            sup.db.set(port, desired="running")
                        return self._send(200, sup.start(
                            port, label=row["label"] if row else None,
                            memfs=bool(row["memfs"]) if row else True))
                    if parts[3] == "stop":
                        sup.stop(port)
                        return self._send(200, {"port": port,
                                                "actual": "stopped"})
                    if parts[3] == "exec":
                        cmd = body.get("cmd")
                        if not cmd:
                            raise ApiError(400, "cmd is required")
                        return self._send(200, sup.exec_cmd(port, cmd))
                return self._send(404, {"error": "not found"})
            except (ValueError, IndexError):
                return self._send(400, {"error": "bad request"})
            except ApiError as e:
                return self._send(e.status, {"error": e.message})

        def do_DELETE(self):
            if not self._authed():
                return self._send(401, {"error": "unauthorized"})
            parts = urllib.parse.urlparse(self.path).path.strip("/").split("/")
            try:
                if parts[:2] == ["api", "instances"] and len(parts) == 3:
                    port = int(parts[2])
                    sup.stop(port)
                    row = sup.db.instance(port)
                    if row and row["rootfs"]:
                        # The rootfs, plus the sidecars that live beside it:
                        # <rootfs>.modes and <rootfs>.lock (host state, not
                        # guest-visible files inside the tree).
                        shutil.rmtree(row["rootfs"], ignore_errors=True)
                        for suffix in (".modes", ".lock"):
                            try:
                                os.remove(row["rootfs"] + suffix)
                            except OSError:
                                pass
                    shutil.rmtree(sup.run_dir(port), ignore_errors=True)
                    sup.db.delete(port)
                    sup.db.event(port, "deleted", "")
                    return self._send(200, {"port": port, "deleted": True})
                return self._send(404, {"error": "not found"})
            except (ValueError, IndexError):
                return self._send(400, {"error": "bad request"})
            except ApiError as e:
                return self._send(e.status, {"error": e.message})

    return Handler


WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
VENDOR_FILES = {
    "xterm.js": "application/javascript",
    "xterm.css": "text/css",
    "addon-fit.js": "application/javascript",
}

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def serve_index():
    """The one static page of the preview web UI. Same-origin as the API, so
    the token question is unchanged: loopback or bearer, nothing else."""
    path = os.path.join(WEB_DIR, "index.html")
    with open(path, "rb") as f:
        body = f.read()
    return 200, "text/html; charset=utf-8", body


def serve_vendor(name):
    """Vendored frontend terminal bits (xterm.js). Whitelist only."""
    if name not in VENDOR_FILES:
        return 404, "text/plain", b"not found"
    path = os.path.join(WEB_DIR, "vendor", name)
    with open(path, "rb") as f:
        return 200, VENDOR_FILES[name], f.read()


# ------------------------------------------------------------ websocket (RFC6455
# The terminal bridge needs exactly one dialect of it: a single client whose
# frames are small binary blobs in both directions. Handshake, masked client
# frames, unmasked server frames, close and ping - nothing else.

def ws_send(conn, payload, opcode=0x2):
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([n])
    elif n < 65536:
        header += bytes([126]) + n.to_bytes(2, "big")
    else:
        header += bytes([127]) + n.to_bytes(8, "big")
    conn.sendall(header + payload)


def ws_read_frame(rfile):
    """One client frame, unmasked. Returns (opcode, payload) or None on EOF."""
    hdr = rfile.read(2)
    if len(hdr) < 2:
        return None
    opcode = hdr[0] & 0x0F
    masked = hdr[1] & 0x80
    ln = hdr[1] & 0x7F
    if ln == 126:
        ln = int.from_bytes(rfile.read(2), "big")
    elif ln == 127:
        ln = int.from_bytes(rfile.read(8), "big")
    mask = rfile.read(4) if masked else None
    payload = rfile.read(ln) if ln else b""
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload  # data, continuation, ping, close - caller sorts


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--release", default=None,
                    help="release tree holding rvvm_ash/vp")
    ap.add_argument("--db", default=None,
                    help="sqlite path (default <release>/runtime/supervisor)")
    ap.add_argument("--bind", default="127.0.0.1:7911",
                    help="daemon listen address (default 127.0.0.1:7911)")
    ap.add_argument("--token", default=os.environ.get(TOKEN_ENV, ""),
                    help="API bearer token (or %s)" % TOKEN_ENV)
    args = ap.parse_args()

    release = find_release_dir(args.release)
    db_path = args.db or os.path.join(release, "runtime", "supervisor",
                                      "supervisor.db")
    db = DB(db_path)
    sup = Supervisor(release, db)
    sup.reconcile()

    host, _, port = args.bind.rpartition(":")
    host = host or "127.0.0.1"
    probe_host = "127.0.0.1" if host in ("0.0.0.0", "::", "") else host
    holder = probe_daemon(probe_host, int(port))
    if holder:
        # Not an error: the daemon the user is asking for is already there.
        # Exiting 0 keeps a tray entry from piling up "failed" runs when its
        # instance got orphaned (a host restart drops the tray's job table but
        # not the detached daemon) and the user presses start again.
        print("supervisor: already running at http://%s:%s/ (%s)"
              % (host, port, holder))
        return
    # The token is created after the duplicate-daemon probe: a second start
    # must not rewrite the token of the daemon that is already serving.
    if args.token:
        token = args.token
        token_src = "set"
    else:
        token, token_file, generated = load_or_create_token(release)
        token_src = "file %s%s" % (token_file, " (generated)" if generated else "")
    httpd = ThreadingHTTPServer((host, int(port)),
                                make_handler(sup, token))
    log_line("supervisor %s: release=%s db=%s bind=%s token=%s"
             % (DAEMON_VERSION, os.path.basename(release),
                os.path.basename(db_path), args.bind, token_src))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
