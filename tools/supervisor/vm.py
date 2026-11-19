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


def reachable(release, port):
    """One cheap round-trip: does this instance actually take a session?

    'running' in the daemon means the core's registry is alive, not that its
    sshd accepts connections - a wedged guest answers TCP and resets the key
    exchange. Probing avoids sending the user's real command to a machine that
    cannot run it (and avoids the double-execution risk of retrying the real
    command after a transport failure)."""
    argv = ssh_argv(release, port, pty=False) + ["true"]
    try:
        return subprocess.run(argv, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=15).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def pick_instance(release, inst, wanted_port, wanted_label):
    if wanted_port is not None and wanted_label is not None:
        uscli.die("use --port or --label, not both")
    if wanted_port is not None:
        match = [i for i in inst if int(i["port"]) == wanted_port]
        if not match:
            uscli.die("no running instance on port %d (see `vm --list`)" % wanted_port)
        return match[0]
    if wanted_label is not None:
        match = [i for i in inst if (i.get("label") or "") == wanted_label]
        if not match:
            labels = sorted({i.get("label") for i in inst if i.get("label")})
            uscli.die("no running instance labelled %r (running labels: %s)"
                      % (wanted_label, ", ".join(labels) or "none"))
        if len(match) == 1:
            return match[0]
        # Labels are not unique in the database (a daemon from before the
        # uniqueness check may have left duplicates): take a reachable one and
        # say which, rather than failing on an otherwise usable machine.
        for i in match:
            if reachable(release, int(i["port"])):
                print("vm: %d instances are labelled %r; using port %s"
                      % (len(match), wanted_label, i["port"]), file=sys.stderr)
                return i
        print("vm: %d instances are labelled %r; none answered a probe, "
              "using port %s" % (len(match), wanted_label, match[0]["port"]),
              file=sys.stderr)
        return match[0]
    if not inst:
        return None
    if len(inst) == 1:
        return inst[0]
    for i in inst:
        if reachable(release, int(i["port"])):
            if int(i["port"]) != int(inst[0]["port"]):
                print("vm: port %s did not answer; using %s"
                      % (inst[0]["port"], i["port"]), file=sys.stderr)
            return i
    # Probes are a convenience, not a gate: a transient failure must not stop
    # the user from running the command on the machine they expect.
    print("vm: no instance answered a probe; trying port %s anyway"
          % inst[0]["port"], file=sys.stderr)
    return inst[0]


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
    ap.add_argument("-L", "--label", metavar="NAME",
                    help="pick the instance carrying this label")
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
    if a.port is not None or a.label is not None:
        if a.port is not None:
            inst = [i for i in inst if int(i["port"]) == a.port]
            if not inst:
                uscli.die("no running instance on port %d (see `vm --list`)"
                          % a.port)
    elif not inst and a.start:
        _, data = uscli.call("POST", "/api/instances", {"label": None, "port": None})
        if data.get("actual") != "running":
            uscli.die("could not start an instance (see `us logs %s`)"
                      % data.get("port", "?"))
        inst = running_instances()
    if not inst:
        uscli.die("no running instance - start one with `us start`, "
                  "or use `vm --start`")

    chosen = pick_instance(release, inst, a.port, a.label)
    port = int(chosen["port"])
    argv = ssh_argv(release, port, pty=a.command is None)
    if a.command is not None:
        argv.append(a.command)
    try:
        rc = subprocess.call(argv)
    except OSError as e:
        uscli.die("could not spawn ssh: %s" % e)
    return rc


if __name__ == "__main__":
    sys.exit(main())
