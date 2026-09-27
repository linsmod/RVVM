#Requires -Version 5.1
<#
.SYNOPSIS
    Regression for the core dying under an sshd privsep session.

.DESCRIPTION
    The core can take itself down in the middle of a working sshd session:

        WARN[109:109]: _Exit due to HOST FAULT inside guest
        === HOST FAULT inside guest === sig=b
        fault addr(si_addr): 0
        in-flight host call: (none - guest code)
        guest PC: 11052bac
        guest insn bytes: 73 00 00 00 82 80 6f d0

    exit code 139, roughly one run in two. The guest PC is musl's syscall
    wrapper (0x52bac into ld-musl-riscv64.so.1, loaded at 0x11000000): an
    `ecall` followed by `addi ra, ra, -698`. The fault is therefore inside the
    emulator servicing that syscall, with a NULL dereference, not in guest code.

    The last thing logged before it is the privsep child dropping its
    descriptors and then reaching for its own procfs:

        fd_drop pid=109 fd=7 host=28 socket peer 127.0.0.1:51411
        fd_wr[close] pid=109 fd=7 ...
        fd_drop pid=109 fd=6 host=27
        fd_wr[close] pid=109 fd=6 ...
        sys_openat(-100, /proc/109/fd, 98000, 0)
        === HOST FAULT ===

    so the suspect is the synthesized /proc/<pid>/fd directory - see
    userland_proc_opendir_fd() - being built for a process mid-teardown. A
    minimal `close N; ls /proc/self/fd` does NOT reproduce it; it takes the full
    privsep context, which is why this driver spends a whole sshd session on it.

    This exists as its own driver because the crash had nowhere to live before.
    It surfaced as an intermittent failure in whatever step happened to be
    running, and as a session socket reset that read like a guest going quiet.
    A fault with a fault block, an exit code and a guest PC is worth a test of
    its own: one that fails while the bug is still there, rather than a symptom
    that gets misfiled.

    Why this driver does not go through ash_drive.ps1: the thing under test is
    the core *process*, not a session. A session would have to be recovered from
    the very failure being detected, and recovering it is exactly what would
    hide the crash. Here the assertion is one line - is the core still running -
    and everything else is setup.

    Why one core for every round: a round that does not kill the core leaves the
    guest exactly as it was, so the next round needs no restart and no settling.
    Only a crash ends the run, and that is the finding. Nothing here waits on a
    schedule; the one poll is for the endpoint, which is a real condition.

.PARAMETER Port
    The core's logical id (default 7921, clear of the other drivers).

.PARAMETER SshPort
    Port the debug sshd listens on inside the guest (default 2242).

.PARAMETER Rounds
    How many sshd sessions to run. The fault is roughly one round in two, so the
    default catches it with room to spare (default 6).

.PARAMETER IdleSecs
    `--idle` for the core, the backstop for a driver that dies here (default 10, dont let user wait a long time).

.PARAMETER Trace
    RVVM_TRACE for the core, because the fault block is much easier to read with
    the last syscalls in the log next to it. Empty to run without.

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/sshd_crash_e2e.ps1

.EXAMPLE
    pwsh ./tools/sshd_crash_e2e.ps1 -Rounds 20
    # The fault is probabilistic; this is how you get a rate rather than a yes/no.
#>
param(
    [int]$Port = 7921,
    [int]$SshPort = 2242,
    [int]$Rounds = 6,
    [int]$IdleSecs = 10,
    [string]$Trace = 'fd,sys',
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$exedir = [IO.Path]::GetDirectoryName($exe)
$fails = 0
. (Join-Path $PSScriptRoot 'ash_sock.ps1')

function TS() { "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] " }

function Check($ok, $what, $detail) {
    if ($ok) {
        "$(TS)ok   $what"
    } else {
        "$(TS)FAIL $what"
        if ($detail) { $detail | ForEach-Object { "     $_" } }
        $script:fails++
    }
}

# One guest command, in one session, waited on to completion. The commands here
# are setup and a single ssh attempt: they end by themselves, so there is no
# output to wait for and no reason to hold a session open across them.
function Guest([string]$Cmd, [int]$TimeoutSec = 240) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    # ArgumentList, not a quoted Arguments string. These commands contain double
    # quotes of their own (ssh-keygen -N "", the passwd line), and hand-escaping
    # those into a command line gets them mangled - the setup step failed with
    # "e2e-keys-ready" missing and nothing else wrong, which is what a quoting bug
    # looks like from the outside. One element per argument, quoted per argument.
    foreach ($a in @('--no-autostart', '--port', "$Port", '-c', $Cmd)) { $psi.ArgumentList.Add($a) }
    $psi.WorkingDirectory = $exedir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $ot = $p.StandardOutput.ReadToEndAsync()
    $et = $p.StandardError.ReadToEndAsync()
    # A bound, because a guest command that never returns must fail here rather
    # than hold the driver: that is the same failure this driver exists to catch,
    # one layer up.
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return "<< timed out after ${TimeoutSec}s >>"
    }
    return (($ot.Result + $et.Result) -replace "`e\[[0-9;?]*[A-Za-z]", '')
}

# The core, started by hand: --idle bounds it if this driver dies, --dlog puts
# the session server's log where this run can find it instead of in the /tmp
# every other driver shares.
#
# Output goes to files, not to pipes this script reads at the end. A redirected
# pipe nobody is draining fills at about 4KB and blocks the writer, and a core
# that logs per syscall reaches that long before it is done booting - so the
# driver waits for an endpoint that the core is no longer able to publish, and
# reports a core that never came up when in fact it is alive and stuck. Files
# have no such ceiling, and they are what a crash report wants to read anyway.
$errfile = Join-Path $env:TEMP 'sshd_crash_core_err.txt'
$outfile = Join-Path $env:TEMP 'sshd_crash_core_out.txt'
$dlog = Join-Path $exedir 'runtime\rootfs\tmp\sshd_crash-vpsessiond.log'
Remove-Item $errfile, $outfile -EA SilentlyContinue
if (Test-Path -LiteralPath $dlog) { Remove-Item -LiteralPath $dlog -Force }

# RVVM_TRACE has to reach the core, and Start-Process inherits the driver's
# environment, so it is set around the launch and put back afterwards.
$prevTrace = $env:RVVM_TRACE
if ($Trace) { $env:RVVM_TRACE = $Trace } else { Remove-Item Env:RVVM_TRACE -EA SilentlyContinue }
$core = Start-Process -FilePath $exe `
    -ArgumentList '--serve', '--port', "$Port", '--idle', "$IdleSecs", '--dlog', '/tmp/sshd_crash-vpsessiond.log' `
    -WorkingDirectory $exedir -PassThru -NoNewWindow `
    -RedirectStandardOutput $outfile -RedirectStandardError $errfile
if ($null -eq $prevTrace) { Remove-Item Env:RVVM_TRACE -EA SilentlyContinue } else { $env:RVVM_TRACE = $prevTrace }

# Ready when the endpoint answers. A poll on a real condition: there is no output
# to wait on before the socket exists.
$up = $false
for ($i = 0; $i -lt 600; $i++) {
    if (Test-AshUp -Exe $exe -Port $Port) { $up = $true; break }
    if ($core.HasExited) { break }
    Start-Sleep -Milliseconds 100
}
Check $up "the core (ash --serve) publishes its endpoint (port $Port)"
if (-not $up) {
    "--- core stderr ---"; Get-Content -LiteralPath $errfile -EA SilentlyContinue | Select-Object -Last 20
    "--- core console ---"; Get-Content -LiteralPath $outfile -EA SilentlyContinue | Select-Object -Last 20
    if (-not $core.HasExited) { try { $core.Kill() } catch { } }
    exit 1
}

# sshd's own log goes to a file in the guest, not down this session: `sshd -d -d`
# is ~15KB of debug, and it is noise next to the two lines this driver reads.
$sshdLog = '/tmp/sshd_crash-sshd.log'
$sshOpts = "-p $SshPort -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
           "-o BatchMode=yes -o IdentitiesOnly=yes -i /root/.ssh/id_e2e -o ConnectTimeout=30"
$sshdOpts = "-d -d -E $sshdLog -p $SshPort -o PermitRootLogin=yes -o PasswordAuthentication=no -o StrictModes=no"

# --- the guest has to be in a state where sshd will actually run a session ----
$o = Guest 'apk add --no-cache openssh >/dev/null 2>&1; command -v sshd >/dev/null && echo e2e-ready'
Check ($o -match 'e2e-ready') 'openssh is installed in the guest'

if (-not $core.HasExited) {
    # Kept under vpsessiond's CMD_MAX (512) on purpose. It stores a command in a
    # fixed char[CMD_MAX] and truncates what does not fit *silently* - no error,
    # no warning to the guest - so a longer command simply stops mid-line. A 611
    # byte version of this step ended as
    #     cat /root/.ssh/id_e2e.pub > /root/.ssh/autho
    # and reported it as "cat: read error: Invalid argument", which reads like a
    # fault in the read path and is not one. Two steps instead of one.
    $o = Guest ('mkdir -p /run/sshd /var/empty && chmod 0755 /run/sshd /var/empty; ' +
                'mkdir -p /root/.ssh && chmod 700 /root/.ssh; ' +
                'grep -q "^sshd:" /etc/passwd || ' +
                'echo "sshd:x:22:22:sshd:/var/empty:/sbin/nologin" >> /etc/passwd; ' +
                'echo e2e-dirs-ready') 60
    Check ($o -match 'e2e-dirs-ready') 'privsep dir, the sshd user and /root/.ssh are in place' $o
}
if (-not $core.HasExited) {
    $o = Guest ('[ -f /etc/ssh/ssh_host_ed25519_key ] || ' +
                'ssh-keygen -q -t ed25519 -N "" -f /etc/ssh/ssh_host_ed25519_key; ' +
                '[ -f /root/.ssh/id_e2e ] || ' +
                'ssh-keygen -q -t ed25519 -N "" -f /root/.ssh/id_e2e; ' +
                'cp /root/.ssh/id_e2e.pub /root/.ssh/authorized_keys; ' +
                'chmod 600 /root/.ssh/authorized_keys; ' +
                'echo e2e-keys-ready') 120
    Check ($o -match 'e2e-keys-ready') 'host keys and an authorized key are in place' $o
}

# --- the rounds -------------------------------------------------------------
# Each one is a complete sshd session: `sshd -d -d` (which does not fork, so it
# serves one connection and exits) plus a single pubkey attempt from the guest's
# own client. The assertion is the core's liveness, checked the moment the step
# returns - the crash takes the core down inside the step, not after it.
$crashed = $false
$crashRound = 0
for ($r = 1; $r -le $Rounds; $r++) {
    if ($core.HasExited) { break }
    $t0 = [Environment]::TickCount64
    $o = Guest ("rm -f $sshdLog; /usr/sbin/sshd $sshdOpts & sshdpid=`$!; " +
                "sleep 1; /usr/bin/ssh $sshOpts root@127.0.0.1 'echo CRASH-ROUND-OK' < /dev/null 2>&1; " +
                'echo ssh-rc=$?; ' +
                'i=0; while [ $i -lt 20 ] && kill -0 $sshdpid 2>/dev/null; do sleep 1; i=$((i+1)); done; ' +
                'echo round-end') 120
    $ms = [Environment]::TickCount64 - $t0
    if ($core.HasExited) {
        $crashed = $true
        $crashRound = $r
        "     round ${r}: core DIED after $ms ms (exit $($core.ExitCode))"
        break
    }
    $sess = if ($o -match 'CRASH-ROUND-OK') { 'session ok' } else { 'no session' }
    "$(TS)     round ${r}: core alive, $sess, ${ms}ms"
}
Check (-not $crashed) "the core survived $Rounds sshd sessions"

if ($crashed) {
    # Everything needed to read the fault without re-running: the block itself,
    # the log leading up to it, and what the guest thought happened. A crash
    # report that only says "it crashed" costs another run to make useful.
    "--- core fault block ---"
    $log = if (Test-Path -LiteralPath $errfile) { Get-Content -LiteralPath $errfile } else { @() }
    $fi = ($log | Select-String -Pattern 'HOST FAULT' | Select-Object -First 1).LineNumber
    if ($fi) {
        $log[[Math]::Max(0, $fi - 2)..([Math]::Min($log.Count - 1, $fi + 8))] | ForEach-Object { "  $_" }
        "--- 40 log lines before the fault ---"
        $log[[Math]::Max(0, $fi - 40)..($fi - 1)] | Where-Object { $_ -notmatch '^\s*(x[0-9a-f]+|=)' } |
            ForEach-Object { "  $_" }
    } else {
        "  (no HOST FAULT in the core log - the core died some other way)"
        $log | Select-Object -Last 25 | ForEach-Object { "  $_" }
    }
    "--- guest sshd log (tail) ---"
    "  (unreachable: the core that ran the guest is the one that died)"
    "--- core console (tail) ---"
    Get-Content -LiteralPath $outfile -EA SilentlyContinue | Select-Object -Last 20 | ForEach-Object { "  $_" }
}

$log = if (Test-Path -LiteralPath $dlog) { [string](Get-Content -LiteralPath $dlog -Raw) } else { '' }
Check ($log -match 'listening on') 'the session server logged its endpoint'

# The core is asked to stop rather than killed, so it drops the endpoint, the
# rootfs lock and the sockets it forwarded for the guest. --idle is the backstop.
if (-not $core.HasExited) {
    & $exe --no-autostart --port $Port --shutdown 2>$null | Out-Null
    for ($i = 0; $i -lt 100; $i++) { if ($core.HasExited) { break }; Start-Sleep -Milliseconds 100 }
    if (-not $core.HasExited) { try { $core.Kill() } catch { } }
}

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    exit 1
}
"=== PASS: sshd crash regression ($Rounds rounds) ==="
exit 0
