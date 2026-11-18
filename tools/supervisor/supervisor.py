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
import ctypes
import json
import os
import re
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DAEMON_VERSION = "0.2.0"
STARTING_GRACE_S = 25      # guest boot to a live registry takes ~20s
STOP_GRACE_S = 10
EXEC_TIMEOUT_S = 60
PORT_BASE = 7910           # 7900 is the dev/test port, instances start above it
TOKEN_ENV = "SUPERVISOR_TOKEN"

# ---------------------------------------------------------------- utils


def log_line(msg):
    print(time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg, flush=True)


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def find_release_dir(explicit):
    """The release tree holding rvvm_ash / vp. Explicit wins; otherwise walk
    up from this script (tools/supervisor) into the repo and pick the
    release.*.x86_64 directory."""
    if explicit:
        return os.path.abspath(explicit)
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    for name in sorted(os.listdir(repo)):
        full = os.path.join(repo, name)
        if name.startswith("release.") and os.path.isdir(full) and (
                os.path.isfile(os.path.join(full, "rvvm_ash_x86_64.exe")) or
                os.path.isfile(os.path.join(full, "rvvm_ash"))):
            return full
    raise SystemExit("supervisor: no release.* directory with rvvm_ash found "
                     "(pass --release)")


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

    # -- listing -----------------------------------------------------------

    def listing(self):
        snap = core_snapshot(self.release)
        out = []
        for row in self.db.instances():
            item = dict(row)
            item["tcp"] = tcp_open(row["port"]) if row["actual"] in (
                "starting", "running") else False
            item["registry"] = snap.get(row["port"])
            out.append(item)
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
        logp = self.log_path(port)
        rootfs = self.new_rootfs_dir(port)
        env = dict(os.environ)
        env["RVVM_LOG_FILE"] = logp
        argv = [self.ash, "--serve", "--port", str(port),
                "--rootfs", rootfs]
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
            subprocess.Popen(argv, cwd=self.release, env=env,
                             stdout=logf, stderr=logf, **kwargs)
        except OSError as e:
            logf.close()
            raise ApiError(500, "could not spawn %s: %s" % (self.ash, e))
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
        self.db.event(port, "start", rootfs)
        deadline = time.time() + self.grace
        while time.time() < deadline:
            time.sleep(0.5)
            snap = core_snapshot(self.release)
            if port in snap and snap[port]["alive"]:
                self.db.set(port, actual="running", pid=snap[port]["pid"])
                self.db.event(port, "running",
                              "pid=%d" % snap[port]["pid"])
                return {"port": port, "actual": "running",
                        "pid": snap[port]["pid"]}
        self.db.set(port, actual="failed", stopped_at=now_iso())
        self.db.event(port, "failed", "no live core within %ds" % self.grace)
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

    # -- exec ---------------------------------------------------------------

    def exec_cmd(self, port, cmd):
        """One command through the core's own ssh endpoint (vpshhd ->
        tinysshnoneauthd): the same link `ssh -p <port>` drives."""
        known_hosts = "NUL" if os.name == "nt" else "/dev/null"
        argv = ["ssh", "-p", str(port),
                "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=" + known_hosts,
                "-o", "BatchMode=yes",
                "-o", "ConnectTimeout=10",
                "127.0.0.1", cmd]
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
            if not token:
                return self.client_address[0] == "127.0.0.1"
            return self.headers.get("Authorization", "") == \
                "Bearer " + token

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

        def do_GET(self):
            if not self._authed():
                return self._send(401, {"error": "unauthorized"})
            path = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(path.query)
            parts = path.path.strip("/").split("/")
            try:
                if path.path == "/api/health":
                    return self._send(200, {
                        "version": DAEMON_VERSION,
                        "release": sup.release,
                        "cores": core_snapshot(sup.release),
                    })
                if path.path == "/api/instances":
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
                        import shutil
                        shutil.rmtree(row["rootfs"], ignore_errors=True)
                    sup.db.delete(port)
                    sup.db.event(port, "deleted", "")
                    return self._send(200, {"port": port, "deleted": True})
                return self._send(404, {"error": "not found"})
            except (ValueError, IndexError):
                return self._send(400, {"error": "bad request"})
            except ApiError as e:
                return self._send(e.status, {"error": e.message})

    return Handler


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
    httpd = ThreadingHTTPServer((host or "127.0.0.1", int(port)),
                                make_handler(sup, args.token))
    log_line("supervisor %s: release=%s db=%s bind=%s token=%s"
             % (DAEMON_VERSION, release, db_path, args.bind,
                "set" if args.token else "none(loopback only)"))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
