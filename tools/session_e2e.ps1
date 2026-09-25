#Requires -Version 5.1
<#
.SYNOPSIS
    Session-server end-to-end driver: one guest daemon, several clients.

.DESCRIPTION
    Runs vpsessiond as the run root (RVVM_ASH_SHELL=guest-assets\vpsessiond.exe),
    so the machine is a *core* that stays up instead of a one-shot shell, and
    drives it with real TCP clients. This is the shape the WSL-style split needs:
    the core holds the state, each client gets its own session.

    Every assertion is the guest's own output, read from the socket:

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
$ESC = [char]27
$BEL = [char]7

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
        $shown = if ($detail) { $detail } else { $script:lastRead }
        if ($shown) {
            "     got: " + (($shown -replace "`r", '' -replace "`n", '|'))
        }
        $script:fails++
    }
}

function New-Client() {
    $c = [pscustomobject]@{ Tcp = $null; Buf = $null }
    $c.Tcp = New-Object System.Net.Sockets.TcpClient
    $c.Tcp.Connect('127.0.0.1', $Port)
    $c.Tcp.NoDelay = $true
    $c.Buf = New-Object System.Text.StringBuilder
    return $c
}

function Send-Text($c, [string]$s) {
    $b = [Text.Encoding]::UTF8.GetBytes($s)
    $c.Tcp.GetStream().Write($b, 0, $b.Length)
    $c.Tcp.GetStream().Flush()
}

function Send-Key($c, [byte]$k) {
    Send-Text $c ([string][char]$k)
}

# One control frame: ESC ] 999 ; <body> BEL. Never forwarded to the shell.
function Send-Frame($c, [string]$body) {
    Send-Text $c ($ESC + ']999;' + $body + $BEL)
}

# Read until @pattern shows up in what arrived *since the last call*, or the
# timeout runs out. What arrived is returned either way, so a failing check can
# print it. The full history stays in $c.Buf for a look at the end.
function Read-Until($c, [string]$pattern, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    $seen = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 4096
    while ([Environment]::TickCount64 -lt $deadline) {
        if ($c.Tcp.Available -gt 0) {
            $n = $c.Tcp.GetStream().Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
            [void]$seen.Append($text)
            [void]$c.Buf.Append($text)
            # Match on the text with ANSI sequences stripped: the shell
            # emits ESC[6n (and friends) right after a prompt, and a pattern
            # like '# ' would then never see its space.
            $plain = $seen.ToString() -replace "`e\[[0-9;]*[A-Za-z]", ''
            if ($plain -match $pattern) { break }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    $script:lastRead = $seen.ToString()
    return $seen.ToString()
}

# Read whatever is there until the socket goes away (or the timeout).
function Read-Until-Closed($c, [int]$timeoutMs) {
    $deadline = [Environment]::TickCount64 + $timeoutMs
    $buf = New-Object byte[] 4096
    $seen = New-Object System.Text.StringBuilder
    while ([Environment]::TickCount64 -lt $deadline) {
        if ($c.Tcp.Available -gt 0) {
            $n = $c.Tcp.GetStream().Read($buf, 0, $buf.Length)
            if ($n -le 0) {
                # The peer closed: anything already read is what the caller
                # gets to look at when the check fails.
                $script:lastRead = $seen.ToString()
                return $true
            }
            $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
            [void]$seen.Append($text)
            [void]$c.Buf.Append($text)
            continue
        }
        # Available == 0 and the peer is gone is what a closed socket looks
        # like: probe with a zero-length read.
        try {
            if ($c.Tcp.Client.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead) -and
                $c.Tcp.Available -eq 0) {
                $script:lastRead = $seen.ToString()
                return $true
            }
        } catch {
            $script:lastRead = $seen.ToString()
            return $true
        }
        Start-Sleep -Milliseconds 20
    }
    $script:lastRead = $seen.ToString()
    return $false
}

function Close-Client($c) {
    try { $c.Tcp.Close() } catch { }
}

# --- the core: vpsessiond as the run root -------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.WorkingDirectory = [IO.Path]::GetDirectoryName($exe)
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.EnvironmentVariables['RVVM_ASH_SHELL'] = 'guest-assets\vpsessiond.exe'

$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()

# Ready when the listener answers, not when a log line shows up: the port is the
# contract, and reading the pipe would mean waiting for the whole run.
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
Check $up "vpsessiond listens on 127.0.0.1:$Port"
if (-not $up) {
    try { $p.Kill() } catch { }
    "--- core output ---"
    $outTask.Result
    exit 1
}

$nl = "`n"

# --- one client, one shell ----------------------------------------------
$A = New-Client
Send-Frame $A 'R30;100'
$r = Read-Until $A '/ #' 10000
Check ($r -match '/ #') 'a client gets a prompt of its own'

Send-Text $A ('echo A-UNIQ-$((6*7))' + $nl)
$r = Read-Until $A 'A-UNIQ-42' 5000
Check ($r -match 'A-UNIQ-42') 'the session runs what the client types'

Send-Text $A ('stty size' + $nl)
$r = Read-Until $A '30 100' 5000
Check ($r -match '30 100') 'stty size is the size the client asked for (the R frame)'

Send-Text $A ('echo via-dev-tty > /dev/tty' + $nl)
$r = Read-Until $A 'via-dev-tty' 5000
Check ($r -match 'via-dev-tty') '/dev/tty in the session answers its own pty, not the console'

Send-Text $A ('cd /tmp && pwd' + $nl)
$r = Read-Until $A '/tmp' 5000
Check ($r -match '/tmp') 'the session keeps its own state (cwd)'

# --- one session, one file ----------------------------------------------
# The shape a shared filesystem stands on: a session redirects into a file
# (which hands the shell's fd 1 to a host descriptor) and then reads it back.
# Checked in *one* session first, so a failure here says "a session cannot read
# what it wrote" rather than "the other session cannot see it".
Send-Text $A ('echo selfcheck-content > /tmp/selfcheck.txt' + $nl)
Start-Sleep -Milliseconds 400
Send-Text $A ('ls -l /tmp/selfcheck.txt' + $nl)
$r = Read-Until $A 'selfcheck.txt' 5000
"$(TS)DIAG: ls -l: " + ($r -replace "`r", '' -replace "`n", '|')
Send-Text $A ('cat /tmp/selfcheck.txt' + $nl)
$r = Read-Until $A 'selfcheck-content' 5000
Check ($r -match 'selfcheck-content') 'a session reads back the file it wrote'
Send-Text $A ('wc -c < /tmp/selfcheck.txt' + $nl)
$r = Read-Until $A 'selfcheck-content|\d+' 5000
Check ($r -match '(?m)^\s*18\s*$') 'the file the session wrote has 18 bytes in it'
Send-Text $A ('rm -f /tmp/selfcheck.txt' + $nl)
Start-Sleep -Milliseconds 300

Send-Text $A ('echo pid-a=$$' + $nl)
$r = Read-Until $A 'pid-a=[0-9]+' 5000
$pidA = if ($r -match 'pid-a=([0-9]+)') { $Matches[1] } else { '' }
Check ($pidA -ne '') "the session shell reports a pid ($pidA)"

# --- a second client is a second session --------------------------------
# Off by default: a second connection currently hits a known core bug (a host
# descriptor number recycled under a still-tracked guest slot - see
# handover.md §6), and this driver is the reproduction for it.
if ($Multi) {
    $B = New-Client
    $r = Read-Until $B '/ #' 10000
    Check ($r -match '/ #') 'a second client gets a prompt too'

    Send-Text $B ('echo pid-b=$$' + $nl)
    $r = Read-Until $B 'pid-b=[0-9]+' 5000
    $pidB = if ($r -match 'pid-b=([0-9]+)') { $Matches[1] } else { '' }
    Check ($pidB -ne '' -and $pidB -ne $pidA) "it is a different shell (pid $pidB vs $pidA)"

    Send-Text $A ('echo shared-core-state > /tmp/session-shared.txt' + $nl)
    Start-Sleep -Milliseconds 400
    # A reads its own file back first: this separates "cat with an argument is
    # broken in a session" from "the second session cannot see the file".
    Send-Text $A ('cat /tmp/session-shared.txt' + $nl)
    $r = Read-Until $A 'shared-core-state' 5000
    Check ($r -match 'shared-core-state') 'the writing session reads the file back'

    Send-Text $B ('cat /tmp/session-shared.txt' + $nl)
    $r = Read-Until $B 'shared-core-state' 5000
    Check ($r -match 'shared-core-state') 'both sessions share one filesystem (the core state)'
} else {
    "skip second-session checks (pass -Multi to run them; they need a core fix)"
}

# --- job control, driven from outside the machine -----------------------
Send-Text $A ('sleep 30' + $nl)
Start-Sleep -Milliseconds 900
Send-Key $A 0x03
$r = Read-Until $A '\^C' 5000
Check ($r -match '\^C') '^C written to the socket reaches the session (ISIG on the pty)'
$r = Read-Until $A '#\s*' 5000
Check ($r -match '# ') 'the shell survives its command being interrupted'

Send-Text $A ('sleep 30' + $nl)
Start-Sleep -Milliseconds 900
Send-Key $A 0x1A
$r = Read-Until $A '\^Z' 5000
Check ($r -match '\^Z') '^Z stops the job'
Send-Text $A ('jobs' + $nl)
$r = Read-Until $A 'Stopped' 5000
Check ($r -match 'Stopped') 'jobs reports it as stopped'
Send-Text $A ('kill -KILL %1' + $nl)
$r = Read-Until $A '#\s*' 5000
Check ($r -match '# ') 'the stopped job can be killed and the shell carries on'

# --- a client leaving is not the core leaving ---------------------------
# Let the previous command (`kill -KILL %1`) finish draining before the exit
# goes in: the pty is byte-ordered, and an `exit` sent while its output is
# still in flight races the driver's own read (it would see the trailing
# prompt of the *previous* command and call the session still open).
$r = Read-Until $A '#\s*' 5000
Start-Sleep -Milliseconds 300
$tsExit = [Environment]::TickCount64
Send-Text $A ('exit' + $nl)
$rc = Read-Until-Closed $A 8000
$tsDone = [Environment]::TickCount64
$pollR = $A.Tcp.Client.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead)
$pollW = $A.Tcp.Client.Poll(0, [System.Net.Sockets.SelectMode]::SelectWrite)
$localPort = ($A.Tcp.Client.LocalEndPoint).Port
"$(TS)DIAG: A.localPort=$localPort exitSent=$tsExit elapsed=$($tsDone-$tsExit)ms rc=$rc pollR=$pollR pollW=$pollW"
Check $rc 'the client that exits has its session closed'

if ($Multi) {
    Send-Text $B ('echo B-ALIVE' + $nl)
    $r = Read-Until $B 'B-ALIVE' 5000
    Check ($r -match 'B-ALIVE') 'the other session is untouched (the core stayed up)'

    Close-Client $B
}
Start-Sleep -Milliseconds 500

if (-not $Keep) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(5000) | Out-Null
}

$out = $outTask.Result
$err = $errTask.Result

# The console is the *core's* terminal, not any session's: nothing a session
# wrote to its own /dev/tty may show up here.
Check ($out -notmatch 'via-dev-tty') 'the run console never sees a session''s /dev/tty'

$sessions = ([regex]::Matches($out, 'session \d+ started')).Count
$want = if ($Multi) { 2 } else { 1 }
Check ($sessions -ge $want) "the core started $sessions session(s) without exiting"

"--- core console ---"
($out -split "`n" | Where-Object { $_ -match 'vpsessiond|dbg:' }) -join "`n"
"--- core stderr (filtered) ---"
($err -split "`n" | Where-Object { $_ -and $_ -notmatch 'Syscall \d+ failed' }) -join "`n"

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    exit 1
}
"=== PASS: sessions ==="
exit 0
