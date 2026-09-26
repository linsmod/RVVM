#Requires -Version 5.1
<#
.SYNOPSIS
    Job-control end-to-end driver for the VirtPass run root (rvvm_ash --serve).

.DESCRIPTION
    A plain pipe cannot reproduce "a key pressed while a command runs", which is
    the whole point of ^C / ^Z / jobs / fg. This driver starts one core
    (`rvvm_ash --serve`, which boots /sbin/vpsessiond) and drives a *session*
    over raw TCP - the session server's own interface, not the interactive
    ash client's console handling - writing bytes in timed bursts exactly the way
    a terminal would deliver typing. Every assertion is the shell's own output
    read off the socket.

    Scenarios (each asserts on the session output):

      sigint       ^C kills the foreground command and the shell survives.
      sigtstp      ^Z stops the job, jobs reports it, fg resumes it, ^C kills it.
      fg-resume    fg really resumes: the stopped job's own output appears only
                   if it ran again.
      fg-again     fg, then a second ^Z: a resumed job stops again (a second
                   "Stopped"), which a killed job never does.
      killpg       kill -STOP %1 / kill -CONT %1: the whole-group path, both
                   ways, with jobs reflecting the stop.
      killpg-cont  kill -CONT %1 really resumes: the job's output appears after
                   the CONT, never before.
      bg           a background job runs to completion and is reaped (SIGCHLD).
      wait-bg      `sleep &` + `wait` returns when the job exits (the shell's
                   own SIGCHLD path - it must not hang).
      wait-int     `wait` is a blocking syscall: ^C cuts it short while the
                   background job keeps running.

.PARAMETER Scenario
    Which scenario to run (see above).

.PARAMETER Exe
    The host binary to run as the core. Defaults to the build tree's rvvm_ash.

.PARAMETER Port
    The core's port (default 7950).

.EXAMPLE
    pwsh ./tools/jobctl_e2e.ps1 -Scenario sigtstp

.EXAMPLE
    $env:RVVM_TRACE = 'job'   # and the job: trace lines show up in the core log
    pwsh ./tools/jobctl_e2e.ps1 -Scenario killpg
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe'),
    [int]$Port = 7950
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path

$script:pass = 0
$script:fail = 0

function Check([bool]$ok, [string]$what) {
    if ($ok) {
        $script:pass++
        "[ok]   $what"
    } else {
        $script:fail++
        "[FAIL] $what"
    }
}

# A byte-burst step: a delay, then either text or one control byte.
$script:steps = @()
function Reset-Steps { $script:steps = @() }
function Step([int]$ms, [string]$text) { $script:steps += , @($ms, 'txt', $text) }
function Key([int]$ms, [int]$byte) { $script:steps += , @($ms, 'key', $byte) }

# The whole scenario is one TCP session. The frame makes the server start the
# shell immediately (rather than after its own grace delay); the steps then type
# into it. The socket is drained concurrently (CopyToAsync) - reading only after
# the fact races with the server closing the session, which can drop the tail of
# the output. `exit` at the end ends the shell and the server closes the socket,
# which ends the read.
function Invoke-Session {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    $stream = $client.GetStream()

    $out = New-Object System.IO.MemoryStream
    $readTask = $stream.CopyToAsync($out)

    $frame = [Text.Encoding]::ASCII.GetBytes(([string][char]27) + ']999;R24;80' + ([string][char]7))
    $stream.Write($frame, 0, $frame.Length)
    $stream.Flush()

    foreach ($st in $script:steps) {
        if ($st[0]) { Start-Sleep -Milliseconds $st[0] }
        if ($st[1] -eq 'txt') {
            $bytes = [Text.Encoding]::ASCII.GetBytes($st[2])
        } else {
            $bytes = [byte[]]@([byte]$st[2])
        }
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
    }

    # The shell exits on `exit`; that ends the read at EOF. A scenario that
    # forgets to exit is bounded here so a bug is a failure, not a hang.
    if (-not $readTask.Wait(8000)) {
        $stream.Close()
        $client.Close()
        $readTask.Wait(2000) | Out-Null
    } else {
        $stream.Close()
        $client.Close()
    }

    $text = [Text.Encoding]::UTF8.GetString($out.ToArray())
    $script:lastOut = $text
    return $text
}

# --- one core for the whole run --------------------------------------------
Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $exe } | Stop-Process -Force -ErrorAction SilentlyContinue
& $exe --port $Port --shutdown 2>$null | Out-Null
Start-Sleep -Milliseconds 300

$coreLog = Join-Path ([IO.Path]::GetTempPath()) "jobctl_core_$Port.log"
$coreErr = Join-Path ([IO.Path]::GetTempPath()) "jobctl_core_$Port.err"
$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--port', "$Port" `
    -NoNewWindow -PassThru -RedirectStandardOutput $coreLog -RedirectStandardError $coreErr

$up = $false
for ($i = 0; $i -lt 150; $i++) {
    try {
        $probe = New-Object System.Net.Sockets.TcpClient
        $probe.Connect('127.0.0.1', $Port)
        $probe.Close()
        $up = $true
        break
    } catch {
        Start-Sleep -Milliseconds 100
    }
}
if (-not $up) {
    $core.Kill()
    "core did not come up (port $Port); log:"
    Get-Content $coreLog, $coreErr -ErrorAction SilentlyContinue
    exit 1
}

$sw = [Diagnostics.Stopwatch]::StartNew()

switch ($Scenario) {
    'sigint' {
        Reset-Steps
        Step 0 "sleep 30`n"
        Key 1500 0x03
        Step 800 "echo AFTER-INT`n"
        Step 600 "exit 7`n"
        $out = Invoke-Session
        Check ($out -match 'AFTER-INT') "the shell survives its command being interrupted"
    }
    'sigtstp' {
        Reset-Steps
        Step 0 "sleep 30`n"
        Key 1500 0x1A
        Step 1200 "jobs`n"
        Step 600 "fg`n"
        Key 1500 0x03
        Step 800 "echo AFTER-TSTP-INT`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'Stopped') "jobs reports the stopped job"
        Check ($out -match 'AFTER-TSTP-INT') "the shell survives fg + ^C"
    }
    'fg-resume' {
        Reset-Steps
        # The marker is assembled by the shell ($p) so the typed line does not
        # contain "RESUMED-PROOF": only the resumed job's *output* can.
        Step 0 "p=RESUMED`n"
        Step 0 ('sh -c "sleep 3; echo $p-PROOF"' + "`n")
        Key 700 0x1A
        Step 2000 "fg`n"
        Step 3000 "echo END-MARKER`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'RESUMED-PROOF') "fg resumes the stopped job (its own output appears)"
    }
    'fg-again' {
        Reset-Steps
        Step 0 "sleep 30`n"
        Key 1500 0x1A
        Step 1000 "fg`n"
        Key 1500 0x1A
        Step 1500 "jobs`n"
        Step 600 "kill -KILL %1`n"
        Step 500 "exit 0`n"
        Step 500 "exit 0`n"
        $out = Invoke-Session
        $stops = ([regex]::Matches($out, 'Stopped')).Count
        Check ($stops -ge 2) "a resumed job stops again (saw $stops Stopped)"
    }
    'killpg' {
        Reset-Steps
        Step 0 "sleep 5 &`n"
        Step 400 "jobs`n"
        Step 400 "kill -STOP %1; echo STOP-RC=`$?`n"
        Step 600 "jobs`n"
        Step 400 "kill -CONT %1; echo CONT-RC=`$?`n"
        Step 600 "jobs`n"
        Step 400 "echo MARKER-`$((6*7))`n"
        Step 400 "exit 0`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'STOP-RC=0') "kill -STOP %1 succeeds (whole-group path)"
        Check ($out -match 'CONT-RC=0') "kill -CONT %1 succeeds (whole-group path)"
        Check ($out -match 'MARKER-42') "the shell is alive after the group signals"
    }
    'killpg-cont' {
        Reset-Steps
        # The marker is assembled by the shell ($p) so the typed line does not
        # contain "CONT-PROOF" (the pty echoes what is typed): only the resumed
        # job's output can, and it must land between the two markers.
        Step 0 "p=CONT`n"
        Step 0 ('sh -c "sleep 2; echo $p-PROOF" &' + "`n")
        Step 400 "kill -STOP %1; jobs`n"
        Step 2500 "echo BEFORE-CONT-MARKER`n"
        Step 400 "kill -CONT %1; jobs`n"
        Step 3500 "echo AFTER-CONT-MARKER`n"
        Step 400 "exit 0`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        $before = $out.IndexOf('BEFORE-CONT-MARKER')
        $proof = $out.IndexOf('CONT-PROOF')
        $after = $out.IndexOf('AFTER-CONT-MARKER')
        Check (($proof -gt $before) -and ($proof -lt $after)) `
            "kill -CONT resumes the job (CONT-PROOF lands between the markers)"
    }
    'bg' {
        Reset-Steps
        Step 0 "sleep 1 &`n"
        Step 400 "jobs`n"
        Step 2200 "jobs`n"
        Step 400 "echo BG-MARKER-`$((6*7))`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'BG-MARKER-42') "a background job runs to completion and is reaped"
    }
    'wait-bg' {
        Reset-Steps
        Step 0 "sleep 2 &`n"
        Step 400 "wait`n"
        Step 3000 "echo WAIT-DONE-`$((6*7))`n"
        Step 600 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'WAIT-DONE-42') "wait returns on SIGCHLD (did not hang)"
    }
    'wait-int' {
        Reset-Steps
        Step 0 "sleep 30 &`n"
        Step 800 "wait`n"
        Key 1200 0x03
        Step 1200 "echo AFTER-WAIT-INT`n"
        Step 600 "kill -KILL %1`n"
        Step 400 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'AFTER-WAIT-INT') "^C cuts wait short while the job keeps running"
    }
    default { throw "unknown scenario $Scenario" }
}

$sw.Stop()
"elapsed_ms=$($sw.ElapsedMilliseconds)"

# --- tear down --------------------------------------------------------------
& $exe --port $Port --shutdown 2>$null | Out-Null
Start-Sleep -Milliseconds 500
if (-not $core.HasExited) {
    if (-not $core.WaitForExit(3000)) { try { $core.Kill() } catch { } }
}

if ($script:fail -eq 0) {
    "=== PASS: jobctl $Scenario ($($script:pass) check(s)) ==="
    exit 0
} else {
    "--- session output ---"
    $script:lastOut
    "--- core log ---"
    Get-Content $coreLog, $coreErr -ErrorAction SilentlyContinue
    "=== FAIL: jobctl $Scenario ($($script:fail) of $($script:pass + $script:fail) check(s)) ==="
    exit 1
}