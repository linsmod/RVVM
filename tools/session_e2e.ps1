#Requires -Version 5.1
<#
.SYNOPSIS
    Session-server end-to-end driver: one guest daemon, several clients.

.DESCRIPTION
    Runs vpsessiond as the run root (the bundle's default /sbin/vpsessiond),
    so the machine is a *core* that stays up instead of a one-shot shell, and
    drives it with real socket clients. This is the shape the WSL-style split needs:
    the core holds the state, each client gets its own session.

    Every assertion is the guest's own output, read from the socket. The
    driving is the shared ash_drive.ps1 layer (output-event driven, no fixed
    sleeps): each check is a short queued scenario replayed against its session,
    and the reader accumulates, so a pattern never slips past the end of a
    chunk.

      prompt       a client connects, sends its window size (the R frame) and
                   gets an interactive shell prompt back.
      command      the shell runs what the client types and echoes the result.
      size         `stty size` reports the size the frame asked for, so the
                   session's terminal really is the client's.
      devtty       `echo ... > /dev/tty` inside a session arrives *on that
                   client's socket* - the run's console never sees it.
      state        cwd changes stay in the session (it is one shell, not a
                   command runner).
      two          a second client gets a shell of its own: a different pid,
                   its own tty, and the same filesystem (a file written in one
                   session is readable in the other).
      sigint       ^C written to the socket kills the session's foreground
                   command (ISIG on the pty) and the shell survives.
      sigtstp      ^Z stops it and `jobs` reports it - job control inside a
                   session, driven from outside the machine.
      exit         a client leaving closes its session only: the core stays up
                   and the other client keeps working.

.PARAMETER Port
    Port vpsessiond listens on (default 7900, the guest's default).

.PARAMETER Exe
    The host binary to run. Defaults to the build tree's rvvm_ash.

.PARAMETER Keep
    Leave the core running after the checks (for poking at it by hand).

.PARAMETER Multi
    Also run the second-session checks (they are off by default to keep the
    ordinary run to one session).

.EXAMPLE
    pwsh ./tools/session_e2e.ps1
#>
param(
    [int]$Port = 7900,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe'),
    [switch]$Keep,
    [switch]$Multi
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$fails = 0
. (Join-Path $PSScriptRoot 'ash_drive.ps1')

# The harness's own lines carry the clock too: they are read next to the core's
# trace and the daemon's log, and a session's trouble is usually a race between
# them (what the client saw vs. what the guest did, milliseconds apart).
function TS() {
    "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] "
}

function Check($ok, $what, $detail) {
    if ($ok) {
        "$(TS)ok   $what"
    } else {
        "$(TS)FAIL $what"
        # What the socket answered, so a failing check can be read without
        # re-running the driver.
        $shown = if ($detail) { $detail } else { $script:lastOut }
        if ($shown) {
            "     got: " + (($shown -replace "`r", '' -replace "`n", '|'))
        }
        $script:fails++
    }
}

# Run one check against a session: replay the queued steps and keep the shell
# alive (a long-lived session, one assertion at a time). Each replay starts from
# its own buffer, so "wait for X" means "wait for X from here".
function Run-Session($d) {
    $t = Invoke-Session $d -KeepOpen
    $script:lastOut = $t
    return $t
}

function New-Client([string]$frame) {
    return (New-AshSession -Exe $exe -Port $Port -Frame $frame -NoCurrent)
}

# --- the core: vpsessiond as the run root -------------------------------
# The daemon's log is a *file* in the run's rootfs (guest /tmp -> this path),
# not the host process's stdout: the win32 host has no io callback, so a guest
# write to fd 1/2 is the core's own stdout, and every daemon line there would
# interleave - non-deterministically - into the transcript `ash -c` hands back,
# which is what the session checks match against. Removing the old log first
# makes "what this run logged" exactly what the file holds; the guest appends,
# so a stale file would otherwise read as this run's.
$dlog = Join-Path ([IO.Path]::GetDirectoryName($exe)) 'runtime\rootfs\tmp\vpsessiond.log'
if (Test-Path -LiteralPath $dlog) { Remove-Item -LiteralPath $dlog -Force }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = "--serve --port $Port"
$psi.WorkingDirectory = [IO.Path]::GetDirectoryName($exe)
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()

# Ready when the endpoint answers, not when a log line shows up: the socket is
# the contract, and reading the pipe would mean waiting for the whole run.
$up = $false
for ($i = 0; $i -lt 150; $i++) {
    if (Test-AshUp -Exe $exe -Port $Port) { $up = $true; break }
    Start-Sleep -Milliseconds 100
}
Check $up "vpsessiond publishes its endpoint (port $Port)"
if (-not $up) {
    try { $p.Kill() } catch { }
    "--- core output ---"
    $outTask.Result
    "--- daemon log ---"
    if (Test-Path -LiteralPath $dlog) { Get-Content -LiteralPath $dlog } else { "(no log at $dlog)" }
    exit 1
}

try {
# --- one client, one shell ----------------------------------------------
$A = New-Client 'R30;100'

Reset-Steps -On $A
Expect-Pattern -On $A '/ #' 10000
$out = Run-Session $A
Check ($out -match '/ #') 'a client gets a prompt of its own'

Reset-Steps -On $A
Step -On $A 0 ('echo A-UNIQ-$((6*7))' + "`n")
Expect-Pattern -On $A 'A-UNIQ-42' 5000
$out = Run-Session $A
Check ($out -match 'A-UNIQ-42') 'the session runs what the client types'

Reset-Steps -On $A
Step -On $A 0 ('stty size' + "`n")
Expect-Pattern -On $A '30 100' 5000
$out = Run-Session $A
Check ($out -match '30 100') 'stty size is the size the client asked for (the R frame)'

Reset-Steps -On $A
Step -On $A 0 ('echo via-dev-tty > /dev/tty' + "`n")
Expect-Pattern -On $A 'via-dev-tty' 5000
$out = Run-Session $A
Check ($out -match 'via-dev-tty') '/dev/tty in the session answers its own pty, not the console'

Reset-Steps -On $A
Step -On $A 0 ('cd /tmp && pwd' + "`n")
Expect-Pattern -On $A '/tmp' 5000
$out = Run-Session $A
Check ($out -match '/tmp') 'the session keeps its own state (cwd)'

# --- one session, one file ----------------------------------------------
# The shape a shared filesystem stands on: a session redirects into a file
# (which hands the shell's fd 1 to a host descriptor) and then reads it back.
# Checked in *one* session first, so a failure here says "a session cannot read
# what it wrote" rather than "the other session cannot see it".
Reset-Steps -On $A
Step -On $A 0 ('echo selfcheck-content > /tmp/selfcheck.txt' + "`n")
Expect-Quiet -On $A 400
Step -On $A 0 ('ls -l /tmp/selfcheck.txt' + "`n")
Expect-Pattern -On $A 'selfcheck.txt' 5000
$out = Run-Session $A
"$(TS)DIAG: ls -l: " + ($out -replace "`r", '' -replace "`n", '|')
Check ($out -match 'selfcheck.txt') 'a session writes a file to the core fs'

Reset-Steps -On $A
Step -On $A 0 ('cat /tmp/selfcheck.txt' + "`n")
Expect-Pattern -On $A 'selfcheck-content' 5000
$out = Run-Session $A
Check ($out -match 'selfcheck-content') 'a session reads back the file it wrote'

Reset-Steps -On $A
Step -On $A 0 ('wc -c < /tmp/selfcheck.txt' + "`n")
Expect-Pattern -On $A '(?m)^\s*18\s*$' 5000
$out = Run-Session $A
Check ($out -match '(?m)^\s*18\s*$') 'the file the session wrote has 18 bytes in it'

Reset-Steps -On $A
Step -On $A 0 ('rm -f /tmp/selfcheck.txt' + "`n")
Expect-Quiet -On $A 300
$out = Run-Session $A

Reset-Steps -On $A
Step -On $A 0 ('echo pid-a=$$' + "`n")
Expect-Pattern -On $A 'pid-a=[0-9]+' 5000
$out = Run-Session $A
$pidA = if ($out -match 'pid-a=([0-9]+)') { $Matches[1] } else { '' }
Check ($pidA -ne '') "the session shell reports a pid ($pidA)"

# --- a second client is a second session --------------------------------
# Off by default to keep the ordinary run to one session; `-Multi` adds the
# shared-core checks (they pass - the fd-reuse and sendfile bugs they once
# reproduced are fixed, see handover.md §6).
if ($Multi) {
    $B = New-Client 'R30;100'
    Reset-Steps -On $B
    Expect-Pattern -On $B '/ #' 10000
    $out = Run-Session $B
    Check ($out -match '/ #') 'a second client gets a prompt too'

    Reset-Steps -On $B
    Step -On $B 0 ('echo pid-b=$$' + "`n")
    Expect-Pattern -On $B 'pid-b=[0-9]+' 5000
    $out = Run-Session $B
    $pidB = if ($out -match 'pid-b=([0-9]+)') { $Matches[1] } else { '' }
    Check ($pidB -ne '' -and $pidB -ne $pidA) "it is a different shell (pid $pidB vs $pidA)"

    # A reads its own file back first: this separates "cat with an argument is
    # broken in a session" from "the second session cannot see the file".
    Reset-Steps -On $A
    Step -On $A 0 ('echo shared-core-state > /tmp/session-shared.txt' + "`n")
    Expect-Quiet -On $A 400
    Reset-Steps -On $A
    Step -On $A 0 ('cat /tmp/session-shared.txt' + "`n")
    Expect-Pattern -On $A 'shared-core-state' 5000
    $out = Run-Session $A
    Check ($out -match 'shared-core-state') 'the writing session reads the file back'

    Reset-Steps -On $B
    Step -On $B 0 ('cat /tmp/session-shared.txt' + "`n")
    Expect-Pattern -On $B 'shared-core-state' 5000
    $out = Run-Session $B
    Check ($out -match 'shared-core-state') 'both sessions share one filesystem (the core state)'
} else {
    "skip second-session checks (pass -Multi to run them)"
}

# --- job control, driven from outside the machine -----------------------
# ^C and the prompt it triggers can land in the same chunk; the matching runs
# over the whole accumulated buffer, so the second wait still sees it.
Reset-Steps -On $A
Step -On $A 0 ('sleep 30' + "`n")
Expect-Quiet -On $A 400
Key -On $A 0 0x03
Expect-Pattern -On $A '\^C' 5000
Expect-Pattern -On $A '#\s*' 5000
$out = Run-Session $A
Check ($out -match '\^C') '^C written to the socket reaches the session (ISIG on the pty)'
Check ($out -match '# ') 'the shell survives its command being interrupted'

Reset-Steps -On $A
Step -On $A 0 ('sleep 30' + "`n")
Expect-Quiet -On $A 400
Key -On $A 0 0x1A
Expect-Pattern -On $A '\^Z' 5000
$out = Run-Session $A
Check ($out -match '\^Z') '^Z stops the job'

Reset-Steps -On $A
Step -On $A 0 ('jobs' + "`n")
Expect-Pattern -On $A 'Stopped' 5000
$out = Run-Session $A
Check ($out -match 'Stopped') 'jobs reports it as stopped'

Reset-Steps -On $A
Step -On $A 0 ('kill -KILL %1' + "`n")
Expect-Pattern -On $A '#\s*' 5000
$out = Run-Session $A
Check ($out -match '# ') 'the stopped job can be killed and the shell carries on'

# --- a client leaving is not the core leaving ---------------------------
# The 400ms settle lets the previous command finish draining, then `exit` goes
# in and the peer closes the socket: Expect-Closed is the assertion, no prompt
# guess needed (the shell is idle between invocations, so it will not re-emit
# its prompt).
Reset-Steps -On $A
Step -On $A 400 ('exit' + "`n")
Expect-Closed -On $A 8000
$tsExit = [Environment]::TickCount64
$out = Invoke-Session $A
$tsDone = [Environment]::TickCount64
"$(TS)DIAG: A.path=$($A.Path) exitSent=$tsExit elapsed=$($tsDone-$tsExit)ms closed=$($A.Closed)"
Check $A.Closed 'the client that exits has its session closed'

if ($Multi) {
    Reset-Steps -On $B
    Step -On $B 0 ('echo B-ALIVE' + "`n")
    Expect-Pattern -On $B 'B-ALIVE' 5000
    $out = Run-Session $B
    Check ($out -match 'B-ALIVE') 'the other session is untouched (the core stayed up)'

    Close-AshSession $B
}
Start-Sleep -Milliseconds 500

if (-not $Keep) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(5000) | Out-Null
}

$out = $outTask.Result
$err = $errTask.Result

# The console is the *core's* terminal, not any session's: nothing a session
# wrote to its own /dev/tty may show up here. The daemon's own lines must not be
# here either - they go to the log file, so this console carries only what the
# host itself printed.
Check ($out -notmatch 'via-dev-tty') 'the run console never sees a session''s /dev/tty'
Check ($out -notmatch 'vpsessiond:') 'the daemon log does not leak into the run console'

# The core's own account of the run, read from the file instead of the console.
# Its presence is also a check: a core that ran sessions but logged nothing has a
# log nobody can read, which is worse than no log.
# An empty log reads as $null, and the run must *report* that rather than die on
# it: a log that stayed empty is the finding, not a reason to lose the checks
# that were already run.
$log = if (Test-Path -LiteralPath $dlog) { [string](Get-Content -LiteralPath $dlog -Raw) } else { '' }
$sessions = ([regex]::Matches($log, 'session \d+ started')).Count
$want = if ($Multi) { 2 } else { 1 }
Check ($sessions -ge $want) "the core started $sessions session(s) without exiting"
Check ($log -match 'listening on') 'the daemon logged its endpoint'

"--- core console ---"
$out
"--- daemon log ($dlog) ---"
$log
"--- core stderr (filtered) ---"
($err -split "`n" | Where-Object { $_ -and $_ -notmatch 'Syscall \d+ failed' }) -join "`n"

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    exit 1
}
"=== PASS: sessions ==="
exit 0
} finally {
    # Whatever a check threw, leave no session socket and - unless -Keep - no
    # core behind: a lingering core holds the rootfs lock and the endpoint.
    try { Close-AshSession $A } catch { }
    if ($B) { try { Close-AshSession $B } catch { } }
    if (-not $Keep) { try { $p.Kill() } catch { } }
}
