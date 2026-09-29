#Requires -Version 5.1
<#
.SYNOPSIS
    OpenSSH end-to-end driver: a guest sshd, a guest ssh client, one run.

.DESCRIPTION
    OpenSSH is the heaviest user of socketpair/fork/close a guest has, so this
    driver is really an integration test of the emulator's process, fd and IPC
    layer wearing an ssh costume. The checks, in order:

      core        the run's core publishes its endpoint
      install     openssh is not in the bundle's rootfs, so apk puts it there
      setup       host keys, the privsep user and directory, a client key in
                  authorized_keys
      debug       `sshd -d -d` (no fork, one connection, full transcript) serves
                  a pubkey session - the diagnostic step
      daemon      sshd really daemonizes (fork/setsid) and serves a pubkey
                  session that runs a remote command - the capability step
      second      a second session reaches the same running daemon

    The driving is the shared ash_drive.ps1 layer: output-event driven, no fixed
    sleeps. Every step types one command and waits for an end marker the step
    itself prints, so each wait has a condition and a ceiling. That is the whole
    reason the debug step cannot hang the run - and it is what makes the hang
    this driver was written to chase observable: a step that stops making
    progress fails with its transcript instead of blocking a tool call, and
    `$Out` plus the core's own log are sitting there to be read.

    Two sshd steps, two ports, on purpose. `sshd -d -d` does not fork: it serves
    one connection and exits, so it can still be holding -p $SshPort when the
    next step starts its long-lived daemon. The daemon then cannot bind and the
    client reaches the departing debug daemon instead - the two steps fail
    alternately, one per run, which reads like an emulator fault and is only two
    daemons sharing a number. -SshPortDebug takes them apart.

.PARAMETER Port
    The core's logical id, and the name of its endpoint (default 7913).

.PARAMETER SshPort
    Port sshd listens on inside the guest, bound on the host loopback because
    guest bind/connect are forwarded (default 2222).

.PARAMETER SshPortDebug
    Port for the `sshd -d -d` step, which serves one connection and exits but can
    still be holding the port when the next step starts its daemon (default 2223).

.PARAMETER IdleSecs
    `--idle` for the core: it stops itself after this many seconds with no
    session. A driver that dies must not leave a core behind holding the rootfs
    lock, and a core with no bound at all blocks a tool call forever, because the
    run *is* the tool call's job.

.PARAMETER Trace
    RVVM_TRACE for the core. Empty to run without it.

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/openssh_e2e.ps1

.EXAMPLE
    pwsh ./tools/openssh_e2e.ps1 -IdleSecs 60
    # A core that cannot die on its own is a core this driver will not wait for.
#>
param(
    [int]$Port = 7913,
    [int]$SshPort = 2222,
    [int]$SshPortDebug = 2223,
    [int]$IdleSecs = 900,
    [string]$Trace = 'wsock,fd,sys',
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$exedir = [IO.Path]::GetDirectoryName($exe)
$fails = 0
. (Join-Path $PSScriptRoot 'ash_drive.ps1')

# The harness's lines carry the clock too: they are read next to the core's trace
# and the daemon's log, and a step's trouble is usually a race between them.
function TS() { "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] " }

# Defined after the dot-source, so the engine's own waits (which call Check by
# name) report into this driver's counters: a step that runs out of time fails
# here rather than passing quietly.
function Check($ok, $what, $detail) {
    if ($ok) {
        "$(TS)ok   $what"
    } else {
        "$(TS)FAIL $what"
        $shown = if ($detail) { $detail } else { $script:lastOut }
        if ($shown) { "     got: " + (($shown -replace "`r", '' -replace "`n", '|')) }
        $script:fails++
    }
}

# One step: type a command into the run's session, wait for the marker it always
# prints, and hand back everything the session said. Waiting on the marker rather
# than on the thing being tested is what keeps a failing step bounded - the step
# always ends, so the transcript always arrives, and the assertion below says what
# was missing from it.
#
# $TimeoutMs is a named parameter on purpose. Written as Guest ('a' + 'b', 180000)
# the comma is *inside* the parentheses, so PowerShell passes one argument - an
# array - and "$Cmd" then interpolates the timeout into the command line, where
# the guest duly echoes it. Named, there is nowhere for it to go.
function Guest {
    param([string]$Cmd, [int]$TimeoutMs = 30000)
    Reset-Steps -On $script:S
    Step -On $script:S 0 ($Cmd + "; echo " + $script:marker + "`n")
    Expect-Pattern -On $script:S ([regex]::Escape($script:marker)) $TimeoutMs
    # Then the prompt, then silence. Without these two the step ends the moment
    # the marker shows up and leaves the rest of its output unread in the socket,
    # so the *next* step's reader picks it up and matches the *next* marker
    # against the previous step's bytes - every check then reads one step late,
    # which looks like a guest that is slow rather than a driver that is offset.
    Expect-Pattern -On $script:S '/ #' 10000
    Expect-Quiet   -On $script:S -ms 400 -timeout 3000
    $t = Invoke-Session $script:S -KeepOpen
    $script:lastOut = $t

    # The engine marks a session read-ended and then stops writing to it, so a
    # step after a reset silently types nothing and reads nothing - which reads
    # as a guest that went quiet rather than as a dead socket. Report it every
    # time, and open a fresh session so the remaining steps are still measured.
    #
    # A new session is the right recovery and not a paper-over: what these steps
    # share is the run's rootfs (openssh, the host keys, the running daemon), not
    # the shell. Nothing here depends on a shell surviving, so the checks that
    # follow measure what they claim to. The reset itself is still counted, below.
    $flat = ($Cmd -replace '\s+', ' ')
    if ($script:S.ReadEnded) {
        $script:resets++
        Write-Host ("{0}WARN session socket ended during this step (reset #{1}): {2}" -f (TS), $script:resets, $script:S.ReadEndWhy)
        Write-Host ("{0}WARN   step was: {1}" -f (TS), $flat.Substring(0, [Math]::Min(70, $flat.Length)))
        try { Close-AshSession $script:S } catch { }
        $script:S = New-AshSession -Exe $exe -Port $Port -Frame 'R40;5'
        Reset-Steps -On $script:S
        Expect-Pattern -On $script:S '/ #' 20000
        Step -On $script:S 0 ("stty -echo; echo " + $script:marker + "`n")
        Expect-Pattern -On $script:S ([regex]::Escape($script:marker)) 10000
        Expect-Quiet -On $script:S -ms 300 -timeout 2000
        Invoke-Session $script:S -KeepOpen | Out-Null
        Write-Host ("{0}DIAG reconnected: readEnded={1}" -f (TS), $script:S.ReadEnded)
    } else {
        Write-Host ("{0}DIAG step: readEnded=False chars={1} :: {2}" -f (TS), $t.Length,
                    $flat.Substring(0, [Math]::Min(70, $flat.Length)))
    }
    return $t
}

# The core's own log, in the run's rootfs, at a path this run names - see --dlog
# below. Removed first: the guest appends, so a stale file would otherwise read
# as this run's.
$dlog = Join-Path $exedir 'runtime\rootfs\tmp\openssh_e2e-vpsessiond.log'
if (Test-Path -LiteralPath $dlog) { Remove-Item -LiteralPath $dlog -Force }

# The core. Started here, by hand, with a bounded life:
#   --idle   it stops itself after IdleSecs with no session
#   --dlog   its session server logs where this run can find it, instead of the
#            well-known /tmp/vpsessiond.log that every other driver shares
# A client that autostarts its own core would defeat all of that: the core it
# starts is a different run with a different rootfs, and nothing bounds its life.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--serve --port $Port --idle $IdleSecs --dlog /tmp/openssh_e2e-vpsessiond.log"
$psi.WorkingDirectory = $exedir
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
if ($Trace) { $psi.EnvironmentVariables['RVVM_TRACE'] = $Trace }

$core = [System.Diagnostics.Process]::Start($psi)
$outTask = $core.StandardOutput.ReadToEndAsync()
$errTask = $core.StandardError.ReadToEndAsync()

# Ready when the endpoint answers: the socket is the contract. A poll on a real
# condition, not a schedule - there is no output to wait on before it is up.
$up = Wait-AshUp -Exe $exe -Port $Port -Process $core
Check $up "the core (ash --serve) publishes its endpoint (port $Port)"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; $outTask.Result
    "--- core stderr ---"; $errTask.Result
    exit 1
}

# One session for the whole run: the guest's state (the installed openssh, the
# host keys, the daemon) has to be the same state every step reads.
$script:marker = 'e2e-step-end'
$script:S = New-AshSession -Exe $exe -Port $Port -Frame 'R40;5'
$script:lastOut = ''
$script:resets = 0

# The shell's echo has to go before any step can be trusted.
#
# A pty echoes what is typed into it, and these steps type the very strings they
# are about to wait for - `ssh ... 'echo openssh-e2e-ok'` - so with echo on,
# Expect-Pattern matches the driver's own command line and every ssh check passes
# in milliseconds without a session happening. It reads as a working driver and
# measures nothing. With echo off the only occurrence of a marker in the
# transcript is output the guest actually produced.
Reset-Steps -On $script:S
Expect-Pattern -On $script:S '/ #' 20000
$out = Invoke-Session $script:S -KeepOpen
$out = Guest -Cmd 'stty -echo' -TimeoutMs 10000
Check ($out -notmatch 'apk add') 'the session stopped echoing what the driver types'

# No -o UsePAM: this OpenSSH is built without PAM and rejects the option.
# ConnectTimeout bounds the whole initial handshake in OpenSSH, not just the TCP
# connect, and this guest is an interpreter doing post-quantum key exchange: a
# healthy handshake lands around 1.6s but a slow one passes 3. At 3 the client
# gave up mid-handshake and reported "Connection timed out during banner
# exchange", which reads like a server fault and is not one. 30 still bounds the
# case that matters (a handshake that never finishes).
#
# -E, not -e: both sshd steps log to a *file* in the guest, and the session
# carries only the client's own output and the markers the driver waits on.
# `sshd -d -d` on stderr is ~15KB of debug into the same pty the assertions come
# back through - the driver's buffer is a 32KB ring with backpressure, and the
# text it is trying to read is competing with the text explaining what went wrong.
$sshdOpts  = "-E /tmp/openssh-e2e-daemon-sshd.log -p $SshPort -o PermitRootLogin=yes -o PasswordAuthentication=no -o StrictModes=no"
$sshOpts   = "-p $SshPort -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
             "-o BatchMode=yes -o IdentitiesOnly=yes -i /root/.ssh/id_e2e -o ConnectTimeout=30"
$dbgSshd   = ($sshdOpts -replace "-p $SshPort ", "-p $SshPortDebug ") -replace 'openssh-e2e-daemon-sshd', 'openssh-e2e-debug-sshd'
$dbgSsh    = $sshOpts  -replace "-p $SshPort ", "-p $SshPortDebug "

try {
# --- install: openssh is NOT in the bundle's rootfs -------------------------
# rootfs.tar.gz is an Alpine minirootfs: busybox and nothing else, so a fresh
# runtime has no sshd, no ssh and no ssh-keygen (`ssh-keygen: not found`, and
# every later step then fails for the wrong reason). --no-cache keeps apk from
# writing an index into the run's rootfs; the repositories come from the image's
# own /etc/apk/repositories.
$out = Guest -Cmd ('apk add --no-cache openssh 2>&1 | tail -3; ' +
             'command -v sshd >/dev/null && command -v ssh >/dev/null && ' +
             'command -v ssh-keygen >/dev/null && echo e2e-ssh-present') -TimeoutMs 180000
Check ($out -match 'e2e-ssh-present') 'install: openssh present in the guest (apk)'

# --- setup: keys, privsep, an authorized key -------------------------------
$out = Guest -Cmd ('mkdir -p /run/sshd /var/empty && chmod 0755 /run/sshd /var/empty; ' +
             '[ -f /etc/ssh/ssh_host_ed25519_key ] || ssh-keygen -q -t ed25519 -N "" -f /etc/ssh/ssh_host_ed25519_key; ' +
             '[ -f /etc/ssh/ssh_host_rsa_key ]     || ssh-keygen -q -t rsa -b 2048 -N "" -f /etc/ssh/ssh_host_rsa_key; ' +
             'grep -q "^sshd:" /etc/passwd || echo "sshd:x:22:22:sshd:/var/empty:/sbin/nologin" >> /etc/passwd; ' +
             'mkdir -p /root/.ssh && chmod 700 /root/.ssh; ' +
             '[ -f /root/.ssh/id_e2e ] || ssh-keygen -q -t ed25519 -N "" -f /root/.ssh/id_e2e; ' +
             'cat /root/.ssh/id_e2e.pub > /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys; ' +
             'echo e2e-setup-done') -TimeoutMs 180000
Check ($out -match 'e2e-setup-done') 'setup: host keys, privsep dir/user, client key authorized'

# --- one session: sshd in single-connection debug mode ----------------------
# -d -d does not fork: it serves one connection and then exits. Backgrounded here
# so the shell keeps going, and waited on by pid with a ceiling - a bare `wait`
# would never return if no attempt ever reached the daemon, and nothing bounds
# the client side either.
$out = Guest -Cmd ("/usr/sbin/sshd $dbgSshd & sshdpid=`$!; " +
             'for i in 1 2 3 4 5 6; do ' +
             "/usr/bin/ssh $dbgSsh root@127.0.0.1 'echo openssh-e2e-ok' < /dev/null 2>&1 && break; sleep 1; done; " +
             'i=0; while [ $i -lt 20 ] && kill -0 $sshdpid 2>/dev/null; do sleep 1; i=$((i+1)); done; ' +
             'echo e2e-debug-daemon-gone') -TimeoutMs 5000
Check ($out -match 'openssh-e2e-ok') 'sshd (single-connection debug) serves a pubkey session'
Check ($out -match 'e2e-debug-daemon-gone') 'the debug daemon left on its own'

# --- one session: the daemon, and a remote command over pubkey --------------
$out = Guest -Cmd ("/usr/sbin/sshd $sshdOpts && echo e2e-sshd-started; " +
             'for i in 1 2 3 4 5 6 7 8; do ' +
             "/usr/bin/ssh $sshOpts root@127.0.0.1 'echo openssh-e2e-ok' < /dev/null 2>&1 && break; sleep 1; done") -TimeoutMs 180000
Check ($out -match 'e2e-sshd-started')  'sshd starts and daemonizes (fork/setsid)'
Check ($out -match 'openssh-e2e-ok')    'sshd serves a pubkey session and runs a remote command'

# --- the daemon outlives the session that started it ------------------------
$out = Guest -Cmd ("/usr/bin/ssh $sshOpts root@127.0.0.1 'echo openssh-e2e-ok2' < /dev/null 2>&1") -TimeoutMs 5000
Check ($out -match 'openssh-e2e-ok2') 'a second session reaches the same running daemon'

# --- the servers' own logs, read back only when something failed ------------
# Kept out of the session while it is healthy, and fetched here on failure: this
# is the transcript that says *where* a session died, which is the difference
# between "sshd did not answer" and a line number in sshd-session.c.
$diag = ''
if ($fails) {
    $diag = Guest -Cmd 'cat /tmp/openssh-e2e-debug-sshd.log /tmp/openssh-e2e-daemon-sshd.log 2>&1 | tail -5' -TimeoutMs 30000
}

# The session carried every step. Counted separately from the ssh checks above,
# because the driver recovers from a reset and carries on - the capability
# results stay meaningful, and this is the check that says the host is not.
# A reset here is a host-side abort of a live AF_UNIX session (WSAECONNRESET on
# the client socket) while the session server still holds the session open, in
# the same family as the post-fork/execve handle loss behind session_e2e -Multi.
Check ($script:resets -eq 0) "the session carried every step (host resets: $($script:resets))"

} finally {
    # Whatever a check threw, leave no session socket behind, and ask the core to
    # stop rather than killing it: --shutdown lets it drop the endpoint, the
    # rootfs lock and the sockets it forwarded for the guest. --idle is the
    # backstop for the case where this driver never gets here.
    try { Close-AshSession $script:S } catch { }
    & $exe --port $Port --no-autostart --shutdown 2>$null | Out-Null
    for ($i = 0; $i -lt 100; $i++) { if ($core.HasExited) { break }; Start-Sleep -Milliseconds 100 }
    if (-not $core.HasExited) { try { $core.Kill() } catch { } }
}

# The core's own account of the run, from the file this run named. Its presence
# is also a check: a core that ran sessions and logged nothing has a log nobody
# can read. An empty log is reported, not fatal - the finding is the empty log,
# not a reason to lose the checks already run.
$log = if (Test-Path -LiteralPath $dlog) { [string](Get-Content -LiteralPath $dlog -Raw) } else { '' }
Check ($log -match 'listening on') 'the session server logged its endpoint'

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- sshd logs (guest /tmp/openssh-e2e-*-sshd.log, tail) ---"; $diag
    "--- session server log ($dlog) ---"; $log
    "--- core console ---"; $outTask.Result
    "--- core stderr (tail) ---"
    ($errTask.Result -split "`n" | Where-Object { $_ } | Select-Object -Last 5) -join "`n"
    exit 1
}
"=== PASS: openssh ==="
exit 0
