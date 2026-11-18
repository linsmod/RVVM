#!/usr/bin/env python3
"""us - the userland supervisor's thin CLI.

Talks only to the daemon's HTTP API (see docs/design-userland-supervisor.md):
never touches the database, never spawns a core itself. Standard library only.

    us list
    us start [--port N] [label]
    us stop <port>
    us rm <port>
    us logs <port> [-n N]
    us exec <port> <command...>
    us health

Endpoint/token: --url/--token flags, then US_URL/US_TOKEN, then
http://127.0.0.1:7911 with SUPERVISOR_TOKEN unset (loopback daemon).
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_URL = "http://127.0.0.1:7911"


def die(msg):
    print("us: " + msg, file=sys.stderr)
    sys.exit(1)


def call(method, path, body=None):
    """One HTTP round-trip against the daemon; errors exit non-zero."""
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument("--url")
    p.add_argument("--token")
    known, _ = p.parse_known_args()
    url = (known.url or os.environ.get("US_URL") or DEFAULT_URL).rstrip("/")
    token = known.token or os.environ.get("US_TOKEN") or \
        os.environ.get("SUPERVISOR_TOKEN") or ""
    req = urllib.request.Request(url + path, method=method)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data, timeout=120) as r:
            payload = r.read()
            return r.status, (json.loads(payload) if payload else {})
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        try:
            detail = json.loads(detail).get("error", detail)
        except (ValueError, AttributeError):
            pass
        die("%d %s" % (e.code, detail))
    except urllib.error.URLError as e:
        die("daemon unreachable at %s (%s)" % (url, e.reason))


def cmd_list(_):
    _, data = call("GET", "/api/instances")
    inst = data.get("instances", [])
    if not inst:
        print("no instances")
        return
    print("%-6s %-9s %-8s %-4s %-20s %s" %
          ("PORT", "ACTUAL", "DESIRED", "TCP", "STARTED", "LABEL"))
    for i in inst:
        print("%-6d %-9s %-8s %-4s %-20s %s" % (
            i["port"], i["actual"], i["desired"],
            "yes" if i.get("tcp") else "no",
            i.get("started_at") or "-",
            i.get("label") or "-"))


def cmd_start(rest):
    p = argparse.ArgumentParser(prog="us start", add_help=False)
    p.add_argument("--port", type=int)
    p.add_argument("label", nargs="*")
    a = p.parse_args(rest)
    _, data = call("POST", "/api/instances",
                   {"label": " ".join(a.label) or None, "port": a.port})
    if data.get("actual") == "running":
        print("instance up: port=%d pid=%s" % (data["port"], data.get("pid")))
    else:
        print("instance %s: port=%d (see `us logs %d`)"
              % (data.get("actual"), data["port"], data["port"]))
        sys.exit(1)


def cmd_stop(rest):
    p = argparse.ArgumentParser(prog="us stop", add_help=False)
    p.add_argument("port", type=int)
    a = p.parse_args(rest)
    call("POST", "/api/instances/%d/stop" % a.port, {})
    print("stopped: port=%d" % a.port)


def cmd_rm(rest):
    p = argparse.ArgumentParser(prog="us rm", add_help=False)
    p.add_argument("port", type=int)
    a = p.parse_args(rest)
    call("DELETE", "/api/instances/%d" % a.port)
    print("removed: port=%d" % a.port)


def cmd_logs(rest):
    p = argparse.ArgumentParser(prog="us logs", add_help=False)
    p.add_argument("port", type=int)
    p.add_argument("-n", type=int, default=50)
    a = p.parse_args(rest)
    _, data = call("GET", "/api/instances/%d/log?tail=%d" % (a.port, a.n))
    for line in data.get("log", []):
        print(line)


def cmd_exec(rest):
    p = argparse.ArgumentParser(prog="us exec", add_help=False)
    p.add_argument("port", type=int)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    a = p.parse_args(rest)
    if not a.cmd:
        die("a command is required")
    _, data = call("POST", "/api/instances/%d/exec" % a.port,
                   {"cmd": " ".join(a.cmd)})
    out = data.get("stdout", "")
    err = data.get("stderr", "")
    if out:
        print(out, end="" if out.endswith("\n") else "\n")
    if err:
        print(err, end="" if err.endswith("\n") else "\n", file=sys.stderr)
    sys.exit(data.get("exit", 0))


def cmd_health(_):
    _, data = call("GET", "/api/health")
    print("daemon  : %s" % data.get("version"))
    print("release : %s" % data.get("release"))
    cores = data.get("cores", {})
    print("cores   : %s" % (", ".join(
        "%d(pid %d, %s)" % (p, v["pid"],
                            "alive" if v["alive"] else "dead")
        for p, v in sorted(cores.items())) or "none"))


COMMANDS = {
    "list": cmd_list,
    "start": cmd_start,
    "stop": cmd_stop,
    "rm": cmd_rm,
    "logs": cmd_logs,
    "exec": cmd_exec,
    "health": cmd_health,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(0 if len(sys.argv) >= 2 else 2)
    cmd = sys.argv[1]
    if cmd not in COMMANDS:
        die("unknown command '%s' (try: %s)" % (cmd, ", ".join(COMMANDS)))
    COMMANDS[cmd](sys.argv[2:])


if __name__ == "__main__":
    main()
