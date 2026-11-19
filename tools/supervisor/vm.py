#!/usr/bin/env python3
"""vm - run a command in, or open a shell on, the supervisor's first running VM.

    vm                          interactive shell (pty: ^C, ^D, window size)
    vm -c "uname -a"            one-shot; the exit code is the guest command's
    vm --list                   what could be connected, and on which port
    vm --port 7929 -c "ls /"    pick an instance instead of the first one
    vm --start                  start an instance first if none is running

The transport is the instance's own ssh endpoint, with the key and options the
daemon uses for `us exec` - no password, no host-key prompt, nothing to paste.
Endpoint/token flags are us.py's: --url / --token / --token-file, then the
environment, then the newest release.*/runtime/supervisor/token.

Standard library only, like the rest of tools/supervisor.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import us as uscli  # noqa: E402 - the sibling CLI owns token/HTTP discovery


def release_dir():
    """The daemon's release tree: its token file sits at
    <release>/runtime/supervisor/token, and the ssh key next to it."""
    path = uscli.default_token_file()
    if not path:
        uscli.die("no supervisor token found - pass --token-file "
                  "<release>/runtime/supervisor/token")
    return os.path.dirname(os.path.dirname(os.path.dirname(path)))


def running_instances():
    _, data = uscli.call("GET", "/api/instances")
    inst = [i for i in data.get("instances", [])
            if i.get("actual") == "running"]
    return sorted(inst, key=lambda i: int(i["port"]))


def ssh_argv(release, port, pty):
    key = os.path.join(release, "runtime", "supervisor", "id_ed25519")
    if not os.path.isfile(key):
        uscli.die("no ssh key at %s - the daemon belongs to a different "
                  "release tree than the token in use" % key)
    known = "NUL" if os.name == "nt" else "/dev/null"
    argv = ["ssh"]
    if pty:
        argv.append("-tt")
    argv += ["-i", key,
             "-o", "StrictHostKeyChecking=no",
             "-o", "UserKnownHostsFile=" + known,
             "-o", "LogLevel=ERROR",
             "-o", "BatchMode=yes",
             "-o", "ConnectTimeout=10",
             "-p", str(port), "root@127.0.0.1"]
    return argv


def print_list():
    inst = running_instances()
    if not inst:
        print("no running instance (start one: us start)")
        return
    for i in inst:
        print("%-6s %-8s %s" % (i["port"], i.get("started_at") or "-",
                                i.get("label") or ""))


def main():
    rest = uscli.parse_globals(sys.argv[1:])
    ap = argparse.ArgumentParser(
        prog="vm",
        description="Run a command in / open a shell on a supervisor VM.")
    ap.add_argument("-c", "--command", metavar="CMD",
                    help="run CMD instead of opening a shell")
    ap.add_argument("-p", "--port", type=int,
                    help="instance port (default: the lowest running one)")
    ap.add_argument("-l", "--list", action="store_true",
                    help="list running instances and their ports")
    ap.add_argument("--start", action="store_true",
                    help="start an instance if none is running")
    a = ap.parse_args(rest)

    if a.list:
        print_list()
        return 0

    release = release_dir()
    inst = running_instances()
    if a.port is not None:
        inst = [i for i in inst if int(i["port"]) == a.port]
        if not inst:
            uscli.die("no running instance on port %d (see `vm --list`)" % a.port)
    elif not inst and a.start:
        _, data = uscli.call("POST", "/api/instances", {"label": None, "port": None})
        if data.get("actual") != "running":
            uscli.die("could not start an instance (see `us logs %s`)"
                      % data.get("port", "?"))
        inst = running_instances()
    if not inst:
        uscli.die("no running instance - start one with `us start`, "
                  "or use `vm --start`")

    port = int(inst[0]["port"])
    argv = ssh_argv(release, port, pty=a.command is None)
    if a.command is not None:
        argv.append(a.command)
    try:
        rc = subprocess.call(argv)
    except OSError as e:
        uscli.die("could not spawn ssh: %s" % e)
    if rc == 255 and a.port is None and len(inst) > 1:
        # ssh only returns 255 for transport failures; say which instances
        # were not tried instead of leaving the port number unexplained.
        print("vm: could not connect to port %d; other running instances: %s "
              "(pick one with --port N)" % (port,
              ", ".join(str(i["port"]) for i in inst[1:])), file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
