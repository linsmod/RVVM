#!/usr/bin/env python3
"""us - the userland supervisor's thin CLI.

Talks only to the daemon's HTTP API (see docs/design-userland-supervisor.md):
never touches the database, never spawns a core itself. Standard library only.

    us list
    us start [--port N] [--data NAME] [label]
    us stop <port>
    us rm <port> [--purge]
    us logs <port> [-n N]
    us exec <port> <command...>
    us volumes
    us health

Endpoint/token: --url/--token/--token-file flags, then US_URL / US_TOKEN /
US_TOKEN_FILE / SUPERVISOR_TOKEN, then the newest
release.*/runtime/supervisor/token, then http://127.0.0.1:7911. The daemon
requires a token: loopback is not an authentication boundary (a guest shares
the host's network stack), so leaving it empty is not a fallback.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_URL = "http://127.0.0.1:7911"

# Set by main() from the flags; call() is used by every command, so keeping
# the options here avoids threading them through seven signatures.
OPTS = {"url": None, "token": None, "token_file": None}


def parse_globals(argv):
    """Pull --url/--token/--token-file (and = spellings) out of argv, wherever
    they appear, and return the command and its own arguments."""
    rest = []
    i = 0
    while i < len(argv):
        a = argv[i]
        key = None
        if a in ("--url", "--token", "--token-file") and i + 1 < len(argv):
            key, val = a[2:].replace("-", "_"), argv[i + 1]
            i += 2
        elif a.startswith("--url="):
            key, val = "url", a.split("=", 1)[1]
            i += 1
        elif a.startswith("--token-file="):
            key, val = "token_file", a.split("=", 1)[1]
            i += 1
        elif a.startswith("--token="):
            key, val = "token", a.split("=", 1)[1]
            i += 1
        else:
            rest.append(a)
            i += 1
            continue
        OPTS[key] = val
    return rest


def default_token_file():
    """The daemon writes its generated token beside its database. With several
    release trees, the newest token wins; an explicit --token/-file or env var
    always overrides this."""
    path = OPTS["token_file"] or os.environ.get("US_TOKEN_FILE")
    if path:
        return path
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    candidates = []
    try:
        for name in os.listdir(repo):
            if not name.startswith("release."):
                continue
            full = os.path.join(repo, name, "runtime", "supervisor", "token")
            if os.path.isfile(full):
                candidates.append(full)
    except OSError:
        return None
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


def token_value():
    token = OPTS["token"] or os.environ.get("US_TOKEN") or \
        os.environ.get("SUPERVISOR_TOKEN")
    if token:
        return token
    path = default_token_file()
    if not path:
        return ""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def die(msg):
    print("us: " + msg, file=sys.stderr)
    sys.exit(1)


def call(method, path, body=None):
    """One HTTP round-trip against the daemon; errors exit non-zero."""
    url = (OPTS["url"] or os.environ.get("US_URL") or
           DEFAULT_URL).rstrip("/")
    token = token_value()
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
    print("%-6s %-9s %-8s %-4s %-20s %-12s %s" %
          ("PORT", "ACTUAL", "DESIRED", "TCP", "STARTED", "LABEL", "DATA"))
    for i in inst:
        print("%-6d %-9s %-8s %-4s %-20s %-12s %s" % (
            i["port"], i["actual"], i["desired"],
            "yes" if i.get("tcp") else "no",
            i.get("started_at") or "-",
            i.get("label") or "-",
            i.get("data") or "-"))


def cmd_start(rest):
    p = argparse.ArgumentParser(prog="us start", add_help=False)
    p.add_argument("--port", type=int)
    p.add_argument("--data", metavar="NAME",
                   help="give this instance a durable directory "
                        "(<data-root>/volumes/<NAME>-<uuid>), mounted at /persist")
    p.add_argument("label", nargs="*")
    a = p.parse_args(rest)
    _, data = call("POST", "/api/instances",
                   {"label": " ".join(a.label) or None, "port": a.port,
                    "data": a.data})
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
    p.add_argument("--purge", action="store_true",
                   help="also delete the instance's durable directory "
                        "(the default is to keep it)")
    a = p.parse_args(rest)
    path = "/api/instances/%d" % a.port + ("?purge=1" if a.purge else "")
    _, data = call("DELETE", path)
    kept = data.get("data")
    if a.purge:
        print("removed: port=%d (data purged)" % a.port)
    elif kept:
        print("removed: port=%d (data kept: %s; `us rm %d --purge` deletes it)"
              % (a.port, kept, a.port))
    else:
        print("removed: port=%d" % a.port)


def cmd_volumes(_):
    _, data = call("GET", "/api/volumes")
    vols = data.get("volumes", [])
    if not vols:
        print("no volumes")
        return
    print("%-40s %-10s %-6s %s" % ("NAME", "BYTES", "PORT", "STATE"))
    for v in vols:
        print("%-40s %-10d %-6s %s" % (
            v["name"], v.get("bytes", 0),
            v.get("port") if v.get("port") is not None else "-",
            "locked" if v.get("locked") else "idle"))


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
    # JSON object keys are strings, and the pid may come back as one too.
    print("cores   : %s" % (", ".join(
        "%s(pid %s, %s)" % (p, v.get("pid", "?"),
                            "alive" if v.get("alive") else "dead")
        for p, v in sorted(cores.items(), key=lambda kv: int(kv[0]))) or "none"))


COMMANDS = {
    "list": cmd_list,
    "start": cmd_start,
    "stop": cmd_stop,
    "rm": cmd_rm,
    "logs": cmd_logs,
    "exec": cmd_exec,
    "volumes": cmd_volumes,
    "health": cmd_health,
}


def main():
    rest = parse_globals(sys.argv[1:])
    if not rest or rest[0] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(0 if rest else 2)
    cmd = rest[0]
    if cmd not in COMMANDS:
        die("unknown command '%s' (try: %s)" % (cmd, ", ".join(COMMANDS)))
    COMMANDS[cmd](rest[1:])


if __name__ == "__main__":
    main()
