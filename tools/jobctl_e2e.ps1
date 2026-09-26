#Requires -Version 5.1
<#
.SYNOPSIS
    Job-control end-to-end driver for the VirtPass run root (rvvm_ash --serve).

.DESCRIPTION
    A plain pipe cannot reproduce "a key pressed while a command runs", which is
    the whole point of ^C / ^Z / jobs / fg. This driver starts one core
    (`rvvm_ash --serve`, which boots /sbin/vpsessiond) and drives a *session*
    over raw TCP - the session server's own interface, not the interactive
    ash client's console handling - writing bytes exactly the way a terminal
    would deliver typing. Every assertion is the shell's own output read off
    the socket.

    The session is output-event driven, not time driven. The socket is read
    incrementally into a growing buffer while the scenario runs, and each step
    may *expect* a position in that buffer before the next burst is sent:

      Expect-Pattern <regex>   wait until the regex matches (its offset is the
                               hit position)
      Expect-Newlines <n>      wait until n newlines have arrived
      Expect-Offset <n>        wait until at least n chars have arrived
      Expect-Quiet <ms>        wait until there has been no output for ms
                               (the fallback when a command can emit nothing)

    This replaces fixed sleeps with "wait for the shell to reach here", so a
    slow host no longer flaky-fails and a fast host no longer wastes time. A
    bare `sleep` cannot be observed from its output, so those steps still use a
    short Expect-Quiet settle; everything else waits on real output.

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

.PARAMETER ShowRate
    Print a per-second character-arrival histogram for the session.

.EXAMPLE
    pwsh ./tools/jobctl_e2e.ps1 -Scenario sigtstp

.EXAMPLE
    $env:RVVM_TRACE = 'job'   # and the job: trace lines show up in the core log
    pwsh ./tools/jobctl_e2e.ps1 -Scenario killpg
#>
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe'),
    [int]$Port = 7950,
    [switch]$ShowRate
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

# The scenario is an action queue. A write action types a burst; a wait action
# blocks until the session output reaches a position. Both are enqueued here
# and replayed by Invoke-Session, so a scenario reads top-to-bottom.
$script:actions = @()
function Reset-Steps { $script:actions = @() }
function Add-Action($a) { $script:actions += , $a }
function Step([int]$ms, [string]$text) { Add-Action @('sleep', $ms); Add-Action @('txt', $text) }
function Key([int]$ms, [int]$byte) { Add-Action @('sleep', $ms); Add-Action @('key', $byte) }

# Wait primitives (enqueued, executed in order by Invoke-Session).
function Expect-Pattern([string]$regex, [int]$timeout = 5000) { Add-Action @('wait', 'pattern', $regex, $timeout) }
function Expect-Newlines([int]$n, [int]$timeout = 5000) { Add-Action @('wait', 'newlines', $n, $timeout) }
function Expect-Offset([int]$n, [int]$timeout = 5000) { Add-Action @('wait', 'offset', $n, $timeout) }
function Expect-Quiet([int]$ms = 300, [int]$timeout = 5000) { Add-Action @('wait', 'quiet', $ms, $timeout) }

# --- incremental reader state ----------------------------------------------
$script:stream = $null
$script:client = $null
$script:buf = $null
$script:decoder = $null
$script:readEnded = $false
$script:sampleSw = $null
$script:rate = @{}

function Record-Sample([int]$n) {
    if ($n -le 0 -or $null -eq $script:sampleSw) { return }
    $sec = [int]($script:sampleSw.ElapsedMilliseconds / 1000)
    if (-not $script:rate.ContainsKey($sec)) { $script:rate[$sec] = 0 }
    $script:rate[$sec] += $n
}

# One bounded non-blocking read. Poll tells us whether data is waiting (or the
# peer closed) without throwing on timeout; a ready socket with 0 available is
# EOF. Returns $true if bytes arrived, $false on timeout or EOF.
function Read-Chunk([int]$timeoutMs) {
    if ($script:readEnded) { return $false }
    $sock = $script:client.Client
    if (-not $sock.Poll($timeoutMs * 1000, [System.Net.Sockets.SelectMode]::SelectRead)) {
        return $false
    }
    $avail = $sock.Available
    if ($avail -le 0) { $script:readEnded = $true; return $false }
    $tmp = New-Object byte[] ([Math]::Min($avail, 4096))
    $n = 0
    try {
        $n = $script:stream.Read($tmp, 0, $tmp.Length)
    } catch {
        $script:readEnded = $true
        return $false
    }
    if ($n -le 0) { $script:readEnded = $true; return $false }
    $chars = New-Object char[] 8192
    $c = $script:decoder.GetChars($tmp, 0, $n, $chars, 0)
    if ($c -gt 0) { [void]$script:buf.Append($chars, 0, $c) }
    Record-Sample $c
    return $true
}

function Write-Bytes([byte[]]$b) {
    if ($script:readEnded) { return }
    try {
        $script:stream.Write($b, 0, $b.Length)
        $script:stream.Flush()
    } catch {
        $script:readEnded = $true
    }
}

function Count-Newlines { ([regex]::Matches($script:buf.ToString(), "`n")).Count }

function Get-NewlineOffsets([string]$text) {
    $offs = @()
    for ($i = 0; $i -lt $text.Length; $i++) { if ($text[$i] -eq "`n") { $offs += $i } }
    return , $offs
}

function Assert-Pattern([string]$regex, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $m = [regex]::Match($script:buf.ToString(), $regex)
        if ($m.Success) { $script:lastHit = $m; return $true }
        if ($script:readEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-Chunk 100 | Out-Null
    }
    Check $false "wait: pattern '$regex' (timeout ${timeoutMs}ms)"
    return $false
}

function Assert-Newlines([int]$n, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ((Count-Newlines) -ge $n) { return $true }
        if ($script:readEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-Chunk 100 | Out-Null
    }
    Check $false "wait: $n newline(s) (timeout ${timeoutMs}ms)"
    return $false
}

function Assert-Offset([int]$n, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($script:buf.Length -ge $n) { return $true }
        if ($script:readEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-Chunk 100 | Out-Null
    }
    Check $false "wait: offset >= $n (timeout ${timeoutMs}ms)"
    return $false
}

# Quiet: return once no output has arrived for quietMs. This is the only valid
# signal for a command that emits nothing (a bare `sleep` starting, `wait`
# blocking); it is also what makes the transition adaptive instead of fixed.
function Assert-Quiet([int]$quietMs, [int]$timeoutMs) {
    $quiet = [Diagnostics.Stopwatch]::StartNew()
    $total = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($script:readEnded) { return $true }
        if ($quiet.ElapsedMilliseconds -ge $quietMs) { return $true }
        if ($total.ElapsedMilliseconds -ge $timeoutMs) {
            Check $false "wait: quiet for ${quietMs}ms (timeout ${timeoutMs}ms)"
            return $false
        }
        $slice = [Math]::Min(50, [Math]::Max(10, $quietMs - [int]$quiet.ElapsedMilliseconds))
        if (Read-Chunk $slice) { $quiet.Restart() }
    }
}

# The whole scenario is one TCP session. The frame makes the server start the
# shell immediately (rather than after its own grace delay); the actions then
# type into it and wait on its output. Reading is incremental and interleaved
# with writing, so the scenario can react to the shell reaching a position -
# no fixed schedule, and no tail loss: after `exit` we drain to EOF.
function Invoke-Session {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    $script:client = $client
    $script:stream = $client.GetStream()
    $script:buf = New-Object System.Text.StringBuilder
    $script:decoder = [Text.Encoding]::UTF8.GetDecoder()
    $script:readEnded = $false
    $script:rate = @{}
    $script:sampleSw = [Diagnostics.Stopwatch]::StartNew()

    $frame = [Text.Encoding]::ASCII.GetBytes(([string][char]27) + ']999;R24;80' + ([string][char]7))
    Write-Bytes $frame

    foreach ($a in $script:actions) {
        if ($script:readEnded) { break }
        switch ($a[0]) {
            'sleep' { if ($a[1]) { Start-Sleep -Milliseconds $a[1] } }
            'txt'   { Write-Bytes ([Text.Encoding]::ASCII.GetBytes($a[1])) }
            'key'   { Write-Bytes ([byte[]]@([byte]$a[1])) }
            'wait'  {
                switch ($a[1]) {
                    'pattern'  { Assert-Pattern $a[2] $a[3] | Out-Null }
                    'newlines' { Assert-Newlines $a[2] $a[3] | Out-Null }
                    'offset'   { Assert-Offset $a[2] $a[3] | Out-Null }
                    'quiet'    { Assert-Quiet $a[2] $a[3] | Out-Null }
                }
            }
        }
    }

    # The shell exits on `exit`; that ends the read at EOF. A scenario that
    # forgets to exit (or a command that hangs) is bounded here so a bug is a
    # failure, not an infinite wait.
    $drain = [Diagnostics.Stopwatch]::StartNew()
    while (-not $script:readEnded -and $drain.ElapsedMilliseconds -lt 8000) {
        Read-Chunk 200 | Out-Null
    }

    try { $script:stream.Close() } catch { }
    try { $client.Close() } catch { }

    $text = $script:buf.ToString()
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
        Expect-Quiet 300                       # a bare sleep emits nothing; settle
        Key 0 0x03
        Step 0 "echo AFTER-INT`n"
        Expect-Pattern 'AFTER-INT' 4000
        Step 0 "exit 7`n"
        $out = Invoke-Session
        Check ($out -match 'AFTER-INT') "the shell survives its command being interrupted"
    }
    'sigtstp' {
        Reset-Steps
        Step 0 "sleep 30`n"
        Expect-Quiet 400
        Key 0 0x1A
        Step 0 "jobs`n"
        Expect-Pattern 'Stopped' 4000
        Step 0 "fg`n"
        Expect-Quiet 400
        Key 0 0x03
        Step 0 "echo AFTER-TSTP-INT`n"
        Expect-Pattern 'AFTER-TSTP-INT' 5000
        Step 0 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'Stopped') "jobs reports the stopped job"
        Check ($out -match 'AFTER-TSTP-INT') "the shell survives fg + ^C"
    }
    'fg-resume' {
        Reset-Steps
        # The marker is assembled by the shell ($p) so the typed line does not
        # contain "RESUMED-PROOF": only the resumed job's *output* can.
        Step 0 "p=RESUMED`n"
        Expect-Quiet 200
        Step 0 ('sh -c "sleep 3; echo $p-PROOF"' + "`n")
        Expect-Quiet 400                       # the job is sleeping; it started
        Key 0 0x1A
        Step 0 "fg`n"
        Expect-Pattern 'RESUMED-PROOF' 8000    # only the resumed job can emit it
        Step 0 "echo END-MARKER`n"
        Expect-Pattern 'END-MARKER' 5000
        Step 0 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'RESUMED-PROOF') "fg resumes the stopped job (its own output appears)"
    }
    'fg-again' {
        Reset-Steps
        Step 0 "sleep 30`n"
        Expect-Quiet 400
        Key 0 0x1A
        Step 0 "fg`n"
        Expect-Quiet 400
        Key 0 0x1A
        Step 0 "jobs`n"
        Expect-Pattern 'Stopped' 4000
        Expect-Quiet 200                       # let jobs flush its rows
        Step 0 "kill -KILL %1`n"
        Expect-Quiet 200
        Step 0 "exit 0`n"
        Step 0 "exit 0`n"
        $out = Invoke-Session
        $stops = ([regex]::Matches($out, 'Stopped')).Count
        Check ($stops -ge 2) "a resumed job stops again (saw $stops Stopped)"
    }
    'killpg' {
        Reset-Steps
        Step 0 "sleep 5 &`n"
        Expect-Quiet 200
        Step 0 "jobs`n"
        Expect-Quiet 200
        Step 0 "kill -STOP %1; echo STOP-RC=`$?`n"
        Expect-Pattern 'STOP-RC=0' 3000
        Step 0 "jobs`n"
        Expect-Quiet 200
        Step 0 "kill -CONT %1; echo CONT-RC=`$?`n"
        Expect-Pattern 'CONT-RC=0' 3000
        Step 0 "jobs`n"
        Expect-Quiet 200
        Step 0 "echo MARKER-`$((6*7))`n"
        Expect-Pattern 'MARKER-42' 3000
        Step 0 "exit 0`n"
        Step 0 "exit 0`n"
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
        Expect-Quiet 200
        Step 0 ('sh -c "sleep 2; echo $p-PROOF" &' + "`n")
        Expect-Quiet 300
        Step 0 "kill -STOP %1; jobs`n"
        Expect-Quiet 300
        Step 0 "echo BEFORE-CONT-MARKER`n"
        Expect-Pattern 'BEFORE-CONT-MARKER' 3000
        Step 0 "kill -CONT %1; jobs`n"
        Expect-Pattern 'CONT-PROOF' 6000
        Step 0 "echo AFTER-CONT-MARKER`n"
        Expect-Pattern 'AFTER-CONT-MARKER' 3000
        Step 0 "exit 0`n"
        Step 0 "exit 0`n"
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
        Expect-Quiet 200
        Step 0 "jobs`n"
        Expect-Quiet 1300                      # let the 1s job finish (emits nothing)
        Step 0 "jobs`n"
        Expect-Quiet 200
        Step 0 "echo BG-MARKER-`$((6*7))`n"
        Expect-Pattern 'BG-MARKER-42' 3000
        Step 0 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'BG-MARKER-42') "a background job runs to completion and is reaped"
    }
    'wait-bg' {
        Reset-Steps
        Step 0 "sleep 2 &`n"
        Expect-Quiet 200
        # The echo is queued behind `wait`; it only runs once wait returns, so
        # the pattern is the "did not hang" assertion (a hang times out).
        Step 0 "wait`n"
        Step 0 "echo WAIT-DONE-`$((6*7))`n"
        Expect-Pattern 'WAIT-DONE-42' 6000
        Step 0 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'WAIT-DONE-42') "wait returns on SIGCHLD (did not hang)"
    }
    'wait-int' {
        Reset-Steps
        Step 0 "sleep 30 &`n"
        Expect-Quiet 200
        Step 0 "wait`n"
        Expect-Quiet 400                       # wait is blocking; it emits nothing
        Key 0 0x03
        Step 0 "echo AFTER-WAIT-INT`n"
        Expect-Pattern 'AFTER-WAIT-INT' 4000
        Step 0 "kill -KILL %1`n"
        Expect-Quiet 200
        Step 0 "exit 0`n"
        $out = Invoke-Session
        Check ($out -match 'AFTER-WAIT-INT') "^C cuts wait short while the job keeps running"
    }
    default { throw "unknown scenario $Scenario" }
}

$sw.Stop()
"elapsed_ms=$($sw.ElapsedMilliseconds)"

if ($ShowRate) {
    "--- output rate (chars/second) ---"
    $script:rate.GetEnumerator() | Sort-Object { [int]$_.Key } | ForEach-Object {
        "  t=$($_.Key)s  $($_.Value) chars"
    }
}

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
    "--- newline offsets ($((Get-NewlineOffsets $script:lastOut).Count)) ---"
    (Get-NewlineOffsets $script:lastOut) -join ','
    "--- core log ---"
    Get-Content $coreLog, $coreErr -ErrorAction SilentlyContinue
    "=== FAIL: jobctl $Scenario ($($script:fail) of $($script:pass + $script:fail) check(s)) ==="
    exit 1
}