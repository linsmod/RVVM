#Requires -Version 5.1
<#
.SYNOPSIS
    OpenSSH end-to-end: sshd inside the Win32 userland guest, reached by the
    guest's own ssh client through the host-forwarded loopback.

.DESCRIPTION
    OpenSSH is the heaviest user of socketpair/fork/close in the guest: the
    master daemonizes (fork/vfork/setsid), the privsep child talks to it over a
    socketpair monitor, and both sides close their copies of the pair while the
    other still holds one. This driver runs the whole path non-interactively
    through `-c` sessions, with every check on the guest's own output:

      install   openssh is not in the bundle's rootfs (Alpine minirootfs ships
               busybox only), so it is apk-installed first - a fresh runtime
               otherwise fails every later step with `ssh-keygen: not found`.
      setup    host keys (ssh-keygen -A), the privsep user/dir, and a client
               key installed into authorized_keys.
      daemon   sshd starts and daemonizes, and the *same* session then gets a
               pubkey session back (openssh-e2e-ok).
      reuse    a second session reaches the still-running daemon again
               (openssh-e2e-ok2).

    The core's stderr carries the RVVM_TRACE lines (default wsock,fd - the two
    categories behind the socketpair/close path; every line carries [pid:tid])
    and is dumped when a check fails.

    Command strings contain no double quotes: PowerShell 5.1 mangles those on
    their way to a native argv, while single quotes pass through and are parsed
    by the guest's sh -c (vpsessiond hands the -c body over verbatim).

.PARAMETER Port
    Port the core's session socket is published on (default 7913).

.PARAMETER SshPort
    Port sshd listens on inside the guest (bound on the host loopback, since
    guest bind/connect are forwarded) - default 2222.

.PARAMETER Trace
    RVVM_TRACE categories for the core run; '' disables tracing.

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/openssh_e2e.ps1
#>
param(
    [int]$Port = 7913,
    [int]$SshPort = 2222,
    [string]$Trace = 'wsock,fd,sys',
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$dir = Split-Path $exe
$fails = 0
. (Join-Path $PSScriptRoot 'ash_sock.ps1')

function TS() { "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] " }
function Check($ok, $what) {
    if ($ok) { "$(TS)ok   $what" } else { "$(TS)FAIL $what"; $script:fails++ }
}

# A check on guest output: on failure also show what the guest actually said.
function CheckOut([string]$r, $ok, $what) {
    if ($ok) { "$(TS)ok   $what" } else {
        "$(TS)FAIL $what"
        $script:fails++
        "--- guest output ---"; $r; "---"
    }
}

# One client run: attach to the core, ask for the command, return its output with
# the terminal escapes stripped.
function Client([string]$cmd) {
    $r = & $exe --port $Port -c $cmd 2>$null
    ($r -join "`n") -replace "`e\[[0-9;?]*[A-Za-z]", ''
}

$out = Join-Path $env:TEMP 'openssh_e2e_core_out.txt'
$err = Join-Path $env:TEMP 'openssh_e2e_core_err.txt'
Remove-Item $out, $err -ErrorAction SilentlyContinue

# A stale core from an earlier run would answer for us: make sure ours is the one.
Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

if ($Trace) { $env:RVVM_TRACE = $Trace }
           else { Remove-Item Env:RVVM_TRACE -ErrorAction SilentlyContinue }

$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--port', "$Port" `
                      -WorkingDirectory $dir -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow

$up = $false
for ($i = 0; $i -lt 120; $i++) {
    if (Test-AshUp -Exe $exe -Port $Port) { $up = $true; break }
    Start-Sleep -Milliseconds 100
}
Check $up "the core (ash --serve) publishes its endpoint (port $Port)"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}

# No -o UsePAM here: this OpenSSH is built without PAM and rejects the option outright.
$sshdOpts  = "-e -p $SshPort -o PermitRootLogin=yes -o PasswordAuthentication=no -o StrictModes=no"
$sshOpts   = "-p $SshPort -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
             "-o BatchMode=yes -o IdentitiesOnly=yes -i /root/.ssh/id_e2e -o ConnectTimeout=3"

try {
# --- install: openssh is NOT in the bundle's rootfs -------------------------
# rootfs.tar.gz is an Alpine minirootfs: it ships busybox and nothing else, so
# a fresh runtime has no sshd, no ssh and no ssh-keygen (`ssh-keygen: not
# found`, and every later step then fails for the wrong reason). Install it
# first, and make the step idempotent so a warm runtime costs one `apk info`.
# --no-cache keeps apk from writing an index into the run's rootfs; the
# repositories come from the image's own /etc/apk/repositories.
$r = Client ('apk add --no-cache openssh 2>&1 | tail -3; ' +
             'command -v sshd >/dev/null && command -v ssh >/dev/null && ' +
             'command -v ssh-keygen >/dev/null && echo e2e-install-ok')
CheckOut $r ($r -match 'e2e-install-ok') 'install: openssh present in the guest (apk)'

# --- setup: host keys, privsep identity/dir, client key ----------------------
# The image's own /etc/passwd has `sshd:x:22:22:sshd:/dev/null:/sbin/nologin`,
# which is not the privsep identity OpenSSH wants, so the grep only adds the
# 74:74 entry when the image does not already carry one.
$r = Client ('mkdir -p /run/sshd /root/.ssh /var/empty; ' +
             'grep -q ''^sshd:'' /etc/passwd || echo ''sshd:x:74:74:privsep:/run/sshd:/bin/false'' >> /etc/passwd; ' +
             'ssh-keygen -A < /dev/null >/dev/null 2>&1; ' +
             'rm -f /root/.ssh/id_e2e /root/.ssh/id_e2e.pub; ' +
             'ssh-keygen -q -t ed25519 -f /root/.ssh/id_e2e -N '''' < /dev/null; ' +
             'cat /root/.ssh/id_e2e.pub > /root/.ssh/authorized_keys; ' +
             'chmod 600 /root/.ssh/authorized_keys; ' +
             'echo e2e-setup-ok')
CheckOut $r ($r -match 'e2e-setup-ok') 'setup: host keys, privsep dir/user, client key authorized'

# --- one session: sshd in single-connection debug mode + ssh attempts ---------
# `sshd -d -d` stays in this session and logs exactly why a connection dies;
# the transcript is the primary diagnostic when a kex reset shows up.
$r = Client ("/usr/sbin/sshd -e -d -d -p $SshPort -o PermitRootLogin=yes -o PasswordAuthentication=no -o StrictModes=no & " +
             'for i in 1 2 3 4 5 6; do ' +
             "/usr/bin/ssh $sshOpts root@127.0.0.1 'echo openssh-e2e-ok' < /dev/null && break; sleep 1; done; " +
             'wait')
CheckOut $r ($r -match 'openssh-e2e-ok') 'sshd (single-connection debug) serves a pubkey session'

# --- one session: start the daemon, get a pubkey session from it -------------
$r = Client ("/usr/sbin/sshd $sshdOpts && echo e2e-sshd-started; " +
             'for i in 1 2 3 4 5 6 7 8; do ' +
             "/usr/bin/ssh $sshOpts root@127.0.0.1 'echo openssh-e2e-ok' < /dev/null && break; sleep 1; done")
CheckOut $r ($r -match 'e2e-sshd-started') 'sshd starts and daemonizes (fork/setsid)'
CheckOut $r ($r -match 'openssh-e2e-ok')   'sshd serves a pubkey session and runs a remote command'

# --- the daemon outlives the session that started it -------------------------
$r = Client ("/usr/bin/ssh $sshOpts root@127.0.0.1 'echo openssh-e2e-ok2' < /dev/null")
CheckOut $r ($r -match 'openssh-e2e-ok2') 'a second session reaches the same running daemon'

} finally {
    # Cleanup: kill whatever core is listening now, even when a check threw -
    # a lingering core would hold the rootfs lock.
    Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- core output (tail) ---"; Get-Content $out -Tail 60 -ErrorAction SilentlyContinue
    "--- core stderr (tail) ---"; Get-Content $err -Tail 250 -ErrorAction SilentlyContinue
    exit 1
}
"=== PASS: openssh ==="
exit 0