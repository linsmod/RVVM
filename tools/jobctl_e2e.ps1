#Requires -Version 5.1
<#
.SYNOPSIS
    Job-control end-to-end driver for the Win32 console host (rvvm_ash.exe).

.DESCRIPTION
    Drives an interactive guest shell through its console with byte-level writes
    and delays. A plain pipe cannot reproduce "a key pressed while a command
    runs" (the whole point of ^C / ^Z), so the console is written to through
    System.Diagnostics.Process's standard input, one burst at a time, exactly
    the way a terminal would deliver typing.

    Scenarios (each assertion is the guest's own output, not the host's):

      sigint       ^C kills the foreground command, the shell survives, and the
                   exit code of the guest is honoured.
      sigtstp      ^Z stops the job, jobs reports it, fg resumes it, ^C then
                   kills the resumed job.
      fg-resume    fg really resumes: the stopped job's own output can only
                   appear if it ran again.
      fg-again     fg, then a second ^Z: a resumed job stops again (a second
                   "Stopped"), which a killed job never does.
      killpg       kill -STOP %1 / kill -CONT %1: the whole-group path, both
                   ways, with jobs reflecting the stop.
      killpg-cont  kill -CONT %1 really resumes: no job output before the CONT,
                   and the job's output after it.
      bg           a background job runs to completion and is reaped (SIGCHLD).
      wait-bg      `sleep 2 &` + `wait` returns when the job exits (the shell's
                   own SIGCHLD path - it must not hang).
      wait-int     `wait` is a blocking syscall like any other: ^C cuts it
                   short while the background job keeps running.

    Every assertion needs the process to be alive to the end, so both pipes are
    drained from the start: an emulator whose stderr write blocks on a full pipe
    stops running its own code, which looks exactly like a hang (and is what
    RVVM_TRACE_PATH's verbosity triggers).

.PARAMETER Scenario
    Which scenario to run (see above).

.PARAMETER Exe
    The host binary to drive. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/jobctl_e2e.ps1 -Scenario sigtstp

.EXAMPLE
    $env:RVVM_TRACE_PATH = '1'   # and the job: trace lines show up in the result
    pwsh ./tools/jobctl_e2e.ps1 -Scenario killpg
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path

function New-Guest {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = [IO.Path]::GetDirectoryName($exe)
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($psi)
}

$p = New-Guest # noqa: the guest is driven as a terminal would be
$in = $p.StandardInput.BaseStream
# Drain both pipes from the moment the process starts: a guest whose stderr
# blocks on a full pipe stops running its own code, which looks exactly like a
# hang (and is what RVVM_TRACE_PATH's verbosity triggers).
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()

function Send([string]$s) {
    try {
        $b = [Text.Encoding]::ASCII.GetBytes($s)
        $in.Write($b, 0, $b.Length)
        $in.Flush()
    } catch {
        "SEND-FAILED: $($s.Trim()) -- $($_.Exception.Message)"
    }
}
function SendKey([byte]$c) {
    try {
        $b = [byte[]]@($c)
        $in.Write($b, 0, 1)
        $in.Flush()
    } catch {
        "SEND-FAILED: key 0x$('{0:x2}' -f $c) -- $($_.Exception.Message)"
    }
}
function Finish([int]$timeoutMs) {
    if (-not $p.WaitForExit($timeoutMs)) {
        "RESULT: TIMEOUT (no exit within $timeoutMs ms)"
        try { $p.Kill() } catch { }
        $p.WaitForExit(5000) | Out-Null
    } else {
        "RESULT: exit=$($p.ExitCode)"
    }
    $out = $outTask.Result
    $err = $errTask.Result
    "--- stdout ---"
    $out
    "--- stderr (filtered) ---"
    ($err -split "`n" | Where-Object { $_ -notmatch 'Syscall 79 failed|Syscall 260 failed' }) -join "`n"
}

$sw = [Diagnostics.Stopwatch]::StartNew()

switch ($Scenario) {
    'sigint' {
        # A command in the foreground must die on ^C, the shell must survive.
        Send "sleep 30`n"
        Start-Sleep -Milliseconds 1500
        SendKey 0x03
        Start-Sleep -Milliseconds 800
        Send "echo AFTER-INT`n"
        Start-Sleep -Milliseconds 800
        Send "exit 7`n"
        Finish 20000
    }
    'sigtstp' {
        # ^Z must stop the job, jobs must report it and fg must resume it.
        Send "sleep 30`n"
        Start-Sleep -Milliseconds 1500
        SendKey 0x1A
        Start-Sleep -Milliseconds 1200
        Send "echo AFTER-TSTP`n"
        Start-Sleep -Milliseconds 800
        Send "jobs`n"
        Start-Sleep -Milliseconds 800
        Send "fg`n"
        Start-Sleep -Milliseconds 1200
        SendKey 0x03
        Start-Sleep -Milliseconds 1000
        Send "echo AFTER-FG-INT`n"
        Start-Sleep -Milliseconds 800
        Send "exit 0`n"
        Finish 25000
    }
    'fg-resume' {
        # The only definitive proof that fg resumes: the job's own output, which
        # can only appear if the stopped process really ran again.
        Send "sh -c 'sleep 3; echo RESUMED-PROOF'`n"
        Start-Sleep -Milliseconds 700
        SendKey 0x1A
        Start-Sleep -Milliseconds 2000
        Send "fg`n"
        Start-Sleep -Milliseconds 3000
        Send "echo END-MARKER`n"
        Start-Sleep -Milliseconds 800
        Send "exit 0`n"
        Finish 25000
    }
    'fg-again' {
        # fg, then a second ^Z: a resumed job stops again (a second Stopped),
        # a job that was killed on fg never does.
        Send "sleep 30`n"
        Start-Sleep -Milliseconds 1500
        SendKey 0x1A
        Start-Sleep -Milliseconds 1000
        Send "fg`n"
        Start-Sleep -Milliseconds 1500
        SendKey 0x1A
        Start-Sleep -Milliseconds 1500
        Send "jobs`n"
        Start-Sleep -Milliseconds 800
        Send "echo KILL-IT`n"
        Start-Sleep -Milliseconds 300
        Send "kill -KILL %1`n"
        Start-Sleep -Milliseconds 500
        # A stopped job makes the shell refuse the first exit.
        Send "exit 0`n"
        Start-Sleep -Milliseconds 500
        Send "exit 0`n"
        Finish 25000
    }
    'killpg' {
        # kill %1 goes through kill(-pgrp, sig): the whole-group path, both ways.
        Send "sleep 5 &`n"
        Start-Sleep -Milliseconds 400
        Send "jobs`n"
        Start-Sleep -Milliseconds 400
        Send "kill -STOP %1; echo STOP-RC=`$?`n"
        Start-Sleep -Milliseconds 600
        Send "jobs`n"
        Start-Sleep -Milliseconds 400
        Send "kill -CONT %1; echo CONT-RC=`$?`n"
        Start-Sleep -Milliseconds 600
        Send "jobs`n"
        Start-Sleep -Milliseconds 400
        Send "echo MARKER-`$((6*7))`n"
        Start-Sleep -Milliseconds 400
        Send "exit 0`n"
        Start-Sleep -Milliseconds 400
        Send "exit 0`n"
        Finish 25000
    }
    'killpg-cont' {
        # Proof that `kill -CONT %1` (the whole-group path) really resumes: the
        # job's own output can only appear after the CONT, never before.
        Send "sh -c 'sleep 2; echo CONT-PROOF' &`n"
        Start-Sleep -Milliseconds 400
        Send "kill -STOP %1; jobs`n"
        Start-Sleep -Milliseconds 2500
        Send "echo BEFORE-CONT-MARKER`n"
        Start-Sleep -Milliseconds 400
        Send "kill -CONT %1; jobs`n"
        Start-Sleep -Milliseconds 3500
        Send "echo AFTER-CONT-MARKER`n"
        Start-Sleep -Milliseconds 400
        Send "exit 0`n"
        Start-Sleep -Milliseconds 400
        Send "exit 0`n"
        Finish 30000
    }
    'bg' {
        # A background job must run to completion and be reaped (SIGCHLD path).
        Send "sleep 1 &`n"
        Start-Sleep -Milliseconds 400
        Send "jobs`n"
        Start-Sleep -Milliseconds 2200
        Send "jobs`n"
        Start-Sleep -Milliseconds 400
        Send "echo BG-MARKER-`$((6*7))`n"
        Start-Sleep -Milliseconds 400
        Send "exit 0`n"
        Finish 20000
    }
    'wait-bg' {
        # A shell's own job loop: `wait` must return when the background job
        # exits (it is the shell's SIGCHLD path, not a poll), and must not hang.
        Send "sleep 2 &`n"
        Start-Sleep -Milliseconds 400
        Send "wait`n"
        Start-Sleep -Milliseconds 3500
        Send "echo WAIT-DONE-`$((6*7))`n"
        Start-Sleep -Milliseconds 600
        Send "jobs`n"
        Start-Sleep -Milliseconds 600
        Send "exit 0`n"
        Finish 25000
    }
    'wait-int' {
        # A `wait` is a blocking syscall like any other: ^C has to cut it short
        # (the shell returns to its prompt) while the job keeps running.
        Send "sleep 30 &`n"
        Start-Sleep -Milliseconds 800
        Send "wait`n"
        Start-Sleep -Milliseconds 1200
        SendKey 0x03
        Start-Sleep -Milliseconds 1200
        Send "echo AFTER-WAIT-INT`n"
        Start-Sleep -Milliseconds 800
        Send "jobs`n"
        Start-Sleep -Milliseconds 800
        Send "kill -KILL %1`n"
        Start-Sleep -Milliseconds 600
        Send "exit 0`n"
        Finish 25000
    }
    default { throw "unknown scenario $Scenario" }
}

$sw.Stop()
"elapsed_ms=$($sw.ElapsedMilliseconds)"
