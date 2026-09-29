#Requires -Version 5.1
<#
.SYNOPSIS
    Drive an Android-hosted guest's console from the PC, over adb forward.

.DESCRIPTION
    The Android host had exactly one way to run a guest for a test: start it,
    look at the screen, tap it. This is the other one - a text protocol on
    loopback (see RvvmConsoleServer) that a driver can read and type through, so
    a run can be checked without a screenshot or a synthesised touch.

    It is pull-only by design: the console is screen state, not a byte stream,
    so the question worth asking is "what does the screen say now" and the
    answer is one consistent snapshot. That makes Wait-Screen the only wait
    this needs, and it waits on a pattern in that snapshot - never on a fixed
    sleep.

.PARAMETER App
    App id to launch, booted through `am start` with the console already on:

        adb shell am start -n com.rvvm.android/.GuestActivity \
            --es guest_app_name <App> --ez console true

.PARAMETER Argv
    Guest argv, passed as `--esa argv`. Element 0 is the guest's own argv[1].

.PARAMETER Command
    Text to type, Enter appended unless -NoEnter. Converted to the hex the
    protocol carries, so a command with a control character in it is not a
    special case.

.PARAMETER Expect
    Regex to wait for in the screen. This is the assertion: the run's result is
    "did this pattern appear", not "does the picture look right".

.PARAMETER NotExpect
    Regex that must NOT appear within the same window. For "this must not
    print", where waiting for its absence is the only honest test.

.PARAMETER ExpectExit
    Wait until the guest reports it is no longer running.

.PARAMETER Screen
    Print the screen and exit - the "what does it say now" one-liner.

.PARAMETER Interactive
    A shell at the guest, for a person rather than a check. Type a line, it is
    typed at the guest, and what the guest added comes back when the guest stops
    talking - `serial` decides that, not a sleep. `:keys CtrlC` sends a
    keystroke, `:scroll n` walks the scrollback, `:screen` repaints everything,
    `:clear` forgets the baseline, `:quit` leaves.

.EXAMPLE
    # launch, type a command, wait for its answer
    pwsh ./tools/android_console.ps1 -App test_cli -Command 'ls /' -Expect 'bin'

.EXAMPLE
    # a failing command: the prompt comes back, the marker does not
    pwsh ./tools/android_console.ps1 -App test_cli -Command 'nosuchcmd' -Expect '\$' -NotExpect 'nosuchcmd: not found'

.EXAMPLE
    # dot-source it and drive the console directly
    . ./tools/android_console.ps1
    $s = New-ConsoleSession -Port 7979
    Send-Console $s "ls`n"
    Wait-Screen $s -Pattern 'usr'

.EXAMPLE
    pwsh ./tools/android_console.ps1 -SelfTest     # no device needed
#>
[CmdletBinding()]
param(
    [string]$App,
    [string[]]$Argv,
    [int]$Port = 7979,
    [int]$GuestId = -1,
    [string]$Command,
    [switch]$NoEnter,
    [string]$Expect,
    [string]$NotExpect,
    [switch]$ExpectExit,
    [switch]$Screen,
    [int]$TimeoutSec = 20,
    [string]$Adb = 'adb',
    [string]$Serial,
    [switch]$NoForward,
    [switch]$NoLaunch,
    [switch]$Interactive,
    [ValidateSet('finish', 'stay')][string]$AfterExit = 'stay',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$script:PKG = 'com.rvvm.android'

# ==================================================================
# encoding
# ==================================================================

# Keystroke names, so a driver does not have to know that ^C is 0x03 and the
# up arrow is ESC [ A. Anything not in here is typed as literal text.
$script:Keys = @{
    'Enter'      = @(0x0d)
    'Tab'        = @(0x09)
    'Esc'        = @(0x1b)
    'Backspace'  = @(0x7f)
    'CtrlC'      = @(0x03)
    'CtrlD'      = @(0x04)
    'CtrlZ'      = @(0x1a)
    'Up'         = @(0x1b, 0x5b, 0x41)
    'Down'       = @(0x1b, 0x5b, 0x42)
    'Right'      = @(0x1b, 0x5b, 0x43)
    'Left'       = @(0x1b, 0x5b, 0x44)
}

function ConvertTo-ConsoleHex {
    <#
      Bytes -> the hex the protocol carries. Hex rather than an escaped string
      because the bytes a terminal protocol needs are mostly unprintable: 0x0d
      is Enter, 0x03 is SIGINT, 0x1b starts every arrow key. A representation
      that cannot carry them is a representation that needs an escape table, and
      the escape table is where a driver's bugs live.
    #>
    param([byte[]]$Bytes)
    if (-not $Bytes -or $Bytes.Length -eq 0) { return '' }
    return -join ($Bytes | ForEach-Object { $_.ToString('x2') })
}

function ConvertTo-KeyBytes {
    <#
      Text plus key names -> bytes. A literal key name in the text is replaced,
      so 'ls' + 'Enter' is two arguments rather than one string with an escape
      in it - and a script that types Ctrl-C at a guest gets the byte that
      actually means Ctrl-C.
    #>
    param(
        [string]$Text = '',
        [string[]]$Key = @(),
        [switch]$NoEnter
    )
    $bytes = New-Object System.Collections.Generic.List[byte]
    if ($Text) {
        foreach ($b in [Text.Encoding]::UTF8.GetBytes($Text)) { $bytes.Add($b) }
    }
    if (-not $NoEnter -and $Key.Count -eq 0) { $bytes.Add(0x0d) }
    foreach ($k in $Key) {
        if (-not $script:Keys.ContainsKey($k)) {
            throw "unknown key '$k' (known: $(($script:Keys.Keys | Sort-Object) -join ', '))"
        }
        foreach ($b in $script:Keys[$k]) { $bytes.Add($b) }
    }
    return , $bytes.ToArray()
}

# ==================================================================
# session
# ==================================================================

function Test-ConsoleAdb {
    <#
      Exactly one usable device, and it is the one named. With two phones
      attached, `adb forward` without -s fails with "more than one device" -
      and the failure looks like a console that is not listening, so the check
      is here rather than discovered at connect time.
    #>
    param([string]$AdbPath = 'adb', [string]$DeviceSerial)
    & $AdbPath devices | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $AdbPath" }
    $devs = & $AdbPath devices | Select-Object -Skip 1 |
            Where-Object { $_ -match '\sdevice$' } |
            ForEach-Object { ($_ -split '\s+')[0] }
    if (-not $devs) { throw 'no device: `adb devices` lists none in "device" state' }
    if ($DeviceSerial) {
        if ($devs -notcontains $DeviceSerial) {
            throw "device '$DeviceSerial' not connected (have: $($devs -join ', '))"
        }
    } elseif ($devs.Count -gt 1) {
        throw ("more than one device attached ($($devs -join ', ')) - name one with -Serial," +
               " or the forward below would go to whichever adb picks")
    }
}

function Get-AdbTargetArgs {
    <#
      The -s <serial> prefix, or nothing. Every adb call that touches the
      device needs it, and deriving it in one place is what stops a second
      device from being addressed by accident.
    #>
    param([string]$Serial)
    if ($Serial) { return @('-s', $Serial) }
    return @()
}

function Start-ConsoleGuest {
    <#
      Boot the app with the console already listening.

      The console flag rides the launch intent rather than a setprop: the app
      process is already running by the time an Activity reads an intent, so a
      property would have to be set before every launch, and forgetting it once
      would look like a hang.

      after_guest_exit defaults to 'stay' here, unlike the Activity's own
      default of 'finish'. Finishing tears the Activity down, which releases the
      host and closes the run's console session - so the last screen and the
      scrollback, the two things a failed run is read back through, would be
      gone by the time the driver got around to asking. Staying keeps them.
    #>
    param(
        [Parameter(Mandatory)][string]$AppId,
        [string[]]$GuestArgv,
        [string]$AdbPath = 'adb',
        [string]$Serial,
        [string]$AfterExit = 'stay'
    )
    $a = (Get-AdbTargetArgs $Serial) +
         @('shell', 'am', 'start', '-n', "$script:PKG/.GuestActivity",
           '--es', 'guest_app_name', $AppId,
           '--ez', 'console', 'true',
           '--es', 'after_guest_exit', $AfterExit)
    if ($GuestArgv -and $GuestArgv.Count -gt 0) {
        $a += @('--esa', 'argv') + $GuestArgv
    }
    $out = & $AdbPath @a 2>&1
    if ($LASTEXITCODE -ne 0) { throw "am start failed: $out" }
    # am start answers 'Warning: Activity not started' when the task was
    # already there and brought forward - which is a success for our purposes.
    return ($out | Out-String).Trim()
}

function New-ConsoleSession {
    <#
      Forward the port and connect. One socket per session; the protocol is
      request/response, so there is no reader thread and nothing to interleave -
      the discipline that a pushed byte stream would have needed.

      Connecting retries until the deadline: the console starts when the app
      process does, and a launch that has just been issued is not listening yet.

      Retrying the *connect* alone is not enough. `adb forward` accepts on the
      host side before the device side has anything listening, so a TCP connect
      to a port nobody is serving succeeds and the connection is then closed -
      which read as "the console hung up" and looked like a server fault. So a
      connection only counts once it has answered a ping.
    #>
    param(
        [int]$PortNumber = 7979,
        [int]$Guest = -1,
        [string]$AdbPath = 'adb',
        [string]$Serial,
        [int]$ConnectTimeoutMs = 30000,
        [switch]$SkipForward
    )
    Test-ConsoleAdb -AdbPath $AdbPath -DeviceSerial $Serial
    if (-not $SkipForward) {
        $a = (Get-AdbTargetArgs $Serial) + @("forward", "tcp:$PortNumber", "tcp:$PortNumber")
        & $AdbPath @a | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb forward tcp:$PortNumber failed" }
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
    $last = 'timed out'
    while ([DateTime]::UtcNow -lt $deadline) {
        $s = $null
        try {
            $sock = New-Object System.Net.Sockets.TcpClient
            $sock.Connect('127.0.0.1', $PortNumber)
            # Sock is the Socket, not the TcpClient: ReceiveTimeout and Receive
            # live on the socket, and the client object has neither. Client is
            # kept only to own the connection (closing it closes the socket).
            $s = [pscustomobject]@{
                Port    = $PortNumber
                Client  = $sock
                Sock    = $sock.Client
                GuestId = $Guest
                Buf     = (New-Object System.Text.StringBuilder)
                Eof     = $false
            }
            $pong = Invoke-Console $s -Command 'ping' -TimeoutMs 2000
            if ($pong.Ok -and $pong.Fields.Count -ge 0) {
                if ($Guest -ge 0) { [void](Invoke-Console $s -Command "attach $Guest") }
                return $s
            }
            $last = 'connected but not speaking the protocol: ' + $pong.Status
        } catch {
            $last = $_.Exception.Message
        }
        if ($s) { Close-ConsoleSession $s }
        Start-Sleep -Milliseconds 250
    }
    throw ("cannot reach the console on 127.0.0.1:{0} within {1} ms: {2}`n" -f
           $PortNumber, $ConnectTimeoutMs, $last) +
          "`nis the app running with --ez console true?  check: adb logcat -s RVVM-Console RVVM-GuestActivity"
}

function Close-ConsoleSession($Session) {
    if (-not $Session) { return }
    try { $Session.Client.Close() } catch { }
}

# ==================================================================
# framing
# ==================================================================

function Read-ConsoleLine {
    <#
      One line, or $null at end of stream. A deadline rather than a blocking
      read, so a device that stopped answering reports "the console did not
      answer" instead of hanging the driver with no way to tell.
    #>
    param($Session, [int]$TimeoutMs = 5000)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ($true) {
        $nl = $Session.Buf.ToString().IndexOf("`n")
        if ($nl -ge 0) {
            $line = $Session.Buf.ToString(0, $nl)
            [void]$Session.Buf.Remove(0, $nl + 1)
            return $line.TrimEnd("`r")
        }
        if ($Session.Eof) { return $null }
        $left = [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds
        if ($left -le 0) { return $null }
        $Session.Sock.ReceiveTimeout = [Math]::Min($left, 250)
        $tmp = New-Object byte[] 8192
        try { $n = $Session.Sock.Receive($tmp, 0, $tmp.Length, [System.Net.Sockets.SocketFlags]::None) }
        catch {
            $code = $_.Exception.SocketErrorCode
            if ($code -eq [System.Net.Sockets.SocketError]::TimedOut -or
                $code -eq [System.Net.Sockets.SocketError]::WouldBlock) { continue }
            $Session.Eof = $true
            return $null
        }
        if ($n -le 0) { $Session.Eof = $true; return $null }
        # Incremental decode: a multi-byte character split across two reads is
        # not two replacement characters, and a screen full of CJK would
        # otherwise be full of them.
        $chars = New-Object char[] 8192
        $c = [Text.Encoding]::UTF8.GetDecoder().GetChars($tmp, 0, $n, $chars, 0)
        if ($c -gt 0) { [void]$Session.Buf.Append($chars, 0, $c) }
    }
}

function Read-ConsoleResponse {
    <#
      A status line, the payload lines, and the '.' that ends it. The dot is
      why the payload can be counted without counting: a screen row that is
      itself a dot arrives as ' .'.
    #>
    param($Session, [int]$TimeoutMs = 5000)
    $status = Read-ConsoleLine $Session $TimeoutMs
    if ($null -eq $status) { throw 'the console closed the connection' }
    $rows = New-Object System.Collections.Generic.List[string]
    while ($true) {
        $line = Read-ConsoleLine $Session $TimeoutMs
        if ($null -eq $line) { throw 'the console stopped answering mid-response' }
        if ($line -eq '.') { break }
        # The reverse of the server's escaping: a lone dot is data, and a row
        # that started with a backslash had one added to it.
        if ($line.StartsWith('\.')) { $line = '.' }
        elseif ($line.StartsWith('\\')) { $line = $line.Substring(1) }
        $rows.Add($line)
    }
    $ok = $status.StartsWith('+ ')
    $fields = @{}
    $body = if ($ok) { $status.Substring(2) } else { $status.Substring(2) }
    foreach ($tok in ($body -split '\s+')) {
        if ($tok -match '^([A-Za-z_][A-Za-z_0-9]*)=(.*)$') { $fields[$Matches[1]] = $Matches[2] }
    }
    return [pscustomobject]@{
        Ok     = $ok
        Status = $status
        Body   = $body
        Fields = $fields
        Rows   = $rows.ToArray()
        Text   = ($rows -join "`n")
    }
}

function Invoke-Console {
    param($Session, [Parameter(Mandatory)][string]$Command, [int]$TimeoutMs = 5000)
    $b = [Text.Encoding]::ASCII.GetBytes($Command + "`n")
    $sent = 0
    while ($sent -lt $b.Length) {
        $sent += $Session.Sock.Send($b, $sent, $b.Length - $sent,
                                    [System.Net.Sockets.SocketFlags]::None)
    }
    return Read-ConsoleResponse $Session $TimeoutMs
}

# ==================================================================
# driving
# ==================================================================

function Send-Console {
    <#
      Type into the console. A refusal is returned, not thrown: a guest that is
      not running yet is a normal state between launch and start, not an error
      the driver should die on.
    #>
    param($Session, [string]$Text = '', [string[]]$Key = @(), [switch]$NoEnter)
    $hex = ConvertTo-ConsoleHex (ConvertTo-KeyBytes -Text $Text -Key $Key -NoEnter:$NoEnter)
    if (-not $hex) { return $null }
    return Invoke-Console $Session -Command "in $hex"
}

function Get-ConsoleScreen {
    <#
      The screen now, as one object: the fields the header carries, the rows,
      and the text the waits match against.

      The match is against the *latest* snapshot on purpose. A terminal
      overwrites in place, so a pattern that only held for one frame is a
      pattern that should not have been waited on, and there is no accumulation
      to be had here that the screen itself does not already hold. The scrollback
      is what a driver reads when it wants history, and `scroll` is how it gets
      there.
    #>
    param($Session, [int]$TimeoutMs = 5000)
    $r = Invoke-Console $Session -Command 'snap' -TimeoutMs $TimeoutMs
    $Session | Add-Member -NotePropertyName Last -NotePropertyValue $r -Force
    return $r
}

function Wait-ConsoleSettle {
    <#
      The screen has stopped changing.

      `serial` is exactly this question - native bumps it on every output burst,
      resize, reset and scroll - so "wait until it stops moving" is a poll on one
      int, not a heuristic about elapsed time. Two consecutive reads of the same
      serial mean the guest has nothing more to say right now.

      This is what makes a read usable by a person. A fixed sleep is either too
      short (the answer is still arriving, so you see half of it) or too long
      (you wait a second for a command that answered in 20 ms); the serial says
      when the answer is complete, whatever that turns out to be.
    #>
    param($Session, [int]$QuietMs = 120, [int]$TimeoutMs = 15000)
    $last = -1
    $stillSince = [DateTime]::UtcNow
    $deadline = $stillSince.AddMilliseconds($TimeoutMs)
    while ($true) {
        $snap = Get-ConsoleScreen $Session
        $ser = if ($snap.Fields.ContainsKey('serial')) { [int]$snap.Fields['serial'] } else { -1 }
        if ($ser -ne $last) {
            $last = $ser
            $stillSince = [DateTime]::UtcNow
        } elseif (([DateTime]::UtcNow - $stillSince).TotalMilliseconds -ge $QuietMs) {
            return $snap
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            # Still moving at the deadline: return what is there rather than
            # nothing. A guest that never stops printing is a real case, and
            # its output is worth reading even though it is not finished.
            return $snap
        }
        Start-Sleep -Milliseconds 30
    }
}

function Get-ConsoleDelta {
    <#
      The rows the guest added since the last read.

      A terminal does not append, it scrolls: the grid shifts up and the new
      lines appear at the bottom. So "what is new" is what sits below the longest
      run of leading rows the two screens still agree on - which is the scroll
      distance, and is exactly where the guest's new output starts.

      Printing the whole screen instead is what a first cut did, and it is
      unusable: three commands into a session and you are reading the banner
      and every earlier answer again, with the actual answer at the bottom. The
      screen is the truth; this is the difference between two truths.

      A screen that got shorter (cleared, resized) shares no prefix worth
      trusting, so everything is new - which is the honest answer, not a
      heuristic that hides output.
    #>
    param([string[]]$Previous, [string[]]$Current)
    $n = [Math]::Min($Previous.Count, $Current.Count)
    $keep = 0
    while ($keep -lt $n -and $Previous[$keep] -ceq $Current[$keep]) { $keep++ }
    if ($keep -ge $Current.Count) { return @() }
    return @($Current[$keep..($Current.Count - 1)])
}

function Write-ConsoleScreen {
    <#
      Print what the guest added since @Baseline, not the whole grid again.
      An empty baseline means "everything", which is what the first read wants.
    #>
    param($Screen, [string[]]$Baseline = @())
    $rows = Get-ConsoleDelta -Previous $Baseline -Current @($Screen.Rows)
    $trimmed = @(Format-ConsoleRows $rows)
    if ($trimmed.Count -eq 0) { return }
    Write-Host ('-' * 60) -ForegroundColor DarkGray
    foreach ($r in $trimmed) { Write-Host $r -ForegroundColor Gray }
    Write-Host ('-' * 60) -ForegroundColor DarkGray
}

function Format-ConsoleRows {
    <#
      Rows between the first and the last that have anything on them, so a
      reply does not arrive padded out with blank rows. Blank rows in the
      middle stay: their absence is information, and that is where a prompt
      and the output below it are separated.
    #>
    param([string[]]$Rows)
    if (-not $Rows -or $Rows.Count -eq 0) { return @() }
    $first = -1; $last = -1
    for ($i = 0; $i -lt $Rows.Count; $i++) {
        if (($Rows[$i] -replace '\s', '') -ne '') { if ($first -lt 0) { $first = $i }; $last = $i }
    }
    if ($first -lt 0) { return @() }
    return $Rows[$first..$last]
}

function Wait-ConsoleScreen {
    <#
      Poll until the screen matches, or the deadline passes.

      Output-event driven, not sleep driven: every iteration asks the device,
      so a command that answers in 20 ms is noticed in 20 ms and one that never
      answers costs exactly the timeout.

      -Pattern and -NotPattern are independent conditions that must both hold,
      which is how "the prompt came back, and it was not an error" is written:
      a shell that prints its prompt after failing still matches 'vp>', so on
      its own that assertion would pass for a command that failed. -NotPattern
      is the half that makes it not.

      With neither, this is just "the screen settled", which is rarely what
      anyone wants but is a legitimate thing to poll for.
    #>
    param(
        $Session,
        [string]$Pattern,
        [string]$NotPattern,
        [int]$TimeoutMs = 20000,
        [int]$IntervalMs = 60
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $seen = ''
    while ($true) {
        $snap = Get-ConsoleScreen $Session
        $seen = $snap.Text
        $okWant = (-not $Pattern) -or ($seen -match $Pattern)
        $okNot  = (-not $NotPattern) -or ($seen -notmatch $NotPattern)
        if ($okWant -and $okNot) {
            return [pscustomobject]@{ Ok = $true; Text = $seen; Rows = $snap.Rows; Fields = $snap.Fields }
        }
        if ([DateTime]::UtcNow -ge $deadline) { break }
        Start-Sleep -Milliseconds $IntervalMs
    }
    return [pscustomobject]@{ Ok = $false; Text = $seen; Rows = @(); Fields = @{} }
}

function Get-ConsoleStatus {
    <#
      What the attached guest is doing. `status` is three plain int reads
      natively, so this is cheap enough to poll - and it is the only question
      about a guest that a screen cannot answer, because the answer is
      sometimes "there is no screen left".
    #>
    param($Session)
    return Invoke-Console $Session -Command 'status'
}

function Wait-ConsoleRunning {
    <#
      The run up: a console exists and the guest in it is running.

      A launch does not boot the guest. The Activity starts it from
      surfaceCreated, which the framework calls after the launch intent has
      already returned - so a driver that types the moment it can connect is
      typing into a run that has not started yet. The console refuses that
      rather than guessing (typing into "the active run" would mean typing into
      whichever guest is in the foreground, which is how a driver ends up
      talking to the wrong one), so the wait belongs here.

      Both conditions, because either alone is ambiguous: running without a
      screen is a run whose session is gone, and a screen without a running
      guest is one that has not started or has already finished.
    #>
    param($Session, [int]$TimeoutMs = 30000, [int]$IntervalMs = 60)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $r = Get-ConsoleStatus $Session
        if ($r.Ok -and $r.Fields['screen'] -eq '1' -and $r.Fields['running'] -eq '1') {
            return $true
        }
        Start-Sleep -Milliseconds $IntervalMs
    }
    return $false
}

function Wait-ConsoleExit {
    <#
      The run is over.

      The signal is "this run has a console and is not running" - not "is not
      running". A run that never started is also not running, and a wait keyed
      on that reports a launch that failed to boot as a run that finished
      cleanly, which is the one distinction here that matters. The screen is the
      thing that says a run existed.

      screen=none afterwards means the host tore the whole thing down, which is
      also the end of the run - but only once a screen has been seen at all,
      or a driver that connects to a host with no guest in it would be told a
      run ended that never began.
    #>
    param($Session, [int]$TimeoutMs = 20000, [int]$IntervalMs = 60)
    $sawRun = $false
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $r = Get-ConsoleStatus $Session
        if ($r.Ok) {
            if ($r.Fields['screen'] -eq '1') { $sawRun = $true }
            if ($sawRun -and $r.Fields['running'] -eq '0') { return $true }
            if ($sawRun -and $r.Fields['screen'] -eq '0') { return $true }
        }
        Start-Sleep -Milliseconds $IntervalMs
    }
    return $false
}

# ==================================================================
# self-test
# ==================================================================

function Invoke-ConsoleSelfTest {
    <#
      The parts that break without a device, tested without one: the hex, the
      key table, and the response framing - including the case that actually
      bites, a screen row that is a bare dot and must not end the response.
    #>
    $fails = 0
    function Check($name, $cond) {
        if ($cond) { Write-Host "  ok    $name" -ForegroundColor DarkGray }
        else { Write-Host "  FAIL  $name" -ForegroundColor Red; $script:stFails++ }
    }
    $script:stFails = 0

    Write-Host "encoding"
    Check 'text -> hex' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes 'ls')) -eq '6c730d')
    Check 'no Enter when asked' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes 'ls' -NoEnter)) -eq '6c73')
    Check 'CtrlC is one byte' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes '' -Key CtrlC)) -eq '03')
    Check 'Up is ESC [ A' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes '' -Key Up)) -eq '1b5b41')
    Check 'utf8 survives' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes '中' -NoEnter)) -eq 'e4b8ad')
    Check 'text plus key' ((ConvertTo-ConsoleHex (ConvertTo-KeyBytes 'ls' -Key Enter)) -eq '6c730d')
    try { [void](ConvertTo-KeyBytes '' -Key Nope); Check 'unknown key throws' $false }
    catch { Check 'unknown key throws' $true }

    Write-Host "framing"
    # A real loopback round trip, on this thread. The transcript is a few
    # hundred bytes, so it fits in the socket buffers and the server can write
    # it before the client reads without either side waiting on the other -
    # which is what lets this be one thread and no Start-Job, and what makes it
    # deterministic instead of timing-dependent.
    # Exactly the three cases the server's escaping exists for: a row that is a
    # lone dot (would otherwise end the response), a row that begins with a dot
    # (sent as-is, because it is not a terminator), and a row that begins with a
    # backslash (doubled, so it cannot be read as an escape).
    $transcript =
        "+ rows=4 cols=80 serial=7 cursor=1,3`n" +
        "first line`n" +
        "\.`n" +
        ".config`n" +
        "\\busybox`n" +
        ".`n" +
        "- unknown command 'bogus'`n" +
        ".`n"

    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $sock = New-Object System.Net.Sockets.TcpClient
    # connect() completes off the listen backlog, so the server below can
    # accept() this same connection without a second thread.
    $sock.Connect('127.0.0.1', $port)

    $serverSide = $listener.AcceptTcpClient()
    $bytes = [Text.Encoding]::UTF8.GetBytes($transcript)
    $serverSide.GetStream().Write($bytes, 0, $bytes.Length)
    $serverSide.GetStream().Flush()

    $s = [pscustomobject]@{
        Sock = $sock.Client; GuestId = -1
        Buf = (New-Object System.Text.StringBuilder); Eof = $false
    }
    $r = Read-ConsoleResponse $s 4000
    Check 'header ok'      ($r.Ok -eq $true)
    Check 'fields parsed'  ($r.Fields['serial'] -eq '7' -and $r.Fields['rows'] -eq '4')
    Check 'payload rows'   ($r.Rows.Count -eq 4)
    Check 'dot row kept'   ($r.Rows[1] -eq '.')
    Check 'dot prefix kept' ($r.Rows[2] -eq '.config')
    Check 'backslash kept' ($r.Rows[3] -eq '\busybox')
    $r2 = Read-ConsoleResponse $s 4000
    Check 'refusal parsed' ($r2.Ok -eq $false)
    Check 'refusal reason' ($r2.Body -like "*bogus*")

    $sock.Close()
    $serverSide.Close()
    $listener.Stop()

    Write-Host "delta"
    # The scroll rule, without a screen. These are the shapes a terminal
    # actually produces, and getting any of them wrong either hides output or
    # replays the whole grid after every command.
    $a = @('one', 'two', 'three')
    $same = @('one', 'two', 'three')
    Check 'unchanged is empty'   ((Get-ConsoleDelta $a $same).Count -eq 0)
    $appended = @('one', 'two', 'three', 'four', 'five')
    Check 'append at the bottom'  ((Get-ConsoleDelta $a $appended) -join ',') -eq 'four,five'
    $scrolled = @('two', 'three', 'four', 'five')
    Check 'scroll shows the new'  ((Get-ConsoleDelta $a $scrolled) -join ',') -eq 'four,five'
    $cleared = @('fresh')
    Check 'a clear is all new'   ((Get-ConsoleDelta $a $cleared) -join ',') -eq 'fresh'
    Check 'nothing before, all'   ((Get-ConsoleDelta @() $a) -join ',') -eq 'one,two,three'
    $edited = @('one', 'TWO', 'three')
    Check 'in-place edit, from there' ((Get-ConsoleDelta $a $edited) -join ',') -eq 'TWO,three'
    $blank = @('one', '', 'three')
    Check 'blank rows inside kept'   ((Get-ConsoleDelta $a $blank) -join '|') -eq '|three'
    Check 'all blank prints nothing' ((Format-ConsoleRows @('', '  ', '')).Count -eq 0)

    Write-Host ""
    if ($script:stFails -eq 0) { Write-Host "self-test PASS" -ForegroundColor Green }
    else { Write-Host "self-test FAIL ($script:stFails)" -ForegroundColor Red }
    return $script:stFails
}

# ==================================================================
# entry point
# ==================================================================

if ($SelfTest) {
    exit ([int]((Invoke-ConsoleSelfTest) -gt 0))
}

function Invoke-ConsoleRepl {
    <#
      The human form of the same thing.

      A person at a terminal does not want a protocol; they want a shell. This
      is that: type a line, it is typed at the guest, and the screen comes back
      when the guest stops talking about it - which the serial decides, not a
      sleep. Ctrl-D leaves, `:quit` too.

      The screen is redrawn rather than appended, because that is what the guest
      did: it rewrote its own screen. Printing only what is new would need a
      diff against the previous read, and a diff of a grid that scrolls and
      repaints in place is harder to make honest than the grid itself.

      `:keys <name>...` sends keystrokes instead of text, for the ones that are
      not words - Ctrl-C, the arrows. `:scroll <n>` walks the scrollback.
      Anything else is sent as typed, Enter included.
    #>
    param($Session, [string]$Prompt = 'rvvm> ')
    Write-Host 'Ctrl-D or :quit to leave. :keys CtrlC to send a keystroke, :clear to forget the screen.' -ForegroundColor DarkGray
    $baseline = @()
    while ($true) {
        Write-Host -NoNewline $Prompt -ForegroundColor Cyan
        $line = Read-Host
        if ($null -eq $line) { return 0 }

        switch -Regex ($line.Trim()) {
            '^(:q|quit|exit|:quit)$' { return 0 }
            '^:clear$' { $baseline = @(); Write-Host 'screen forgotten' -ForegroundColor DarkGray; continue }
            '^:keys\s+(.+)$' {
                $names = $Matches[1] -split '\s+'
                $r = Send-Console $Session -Key $names
                if ($r -and -not $r.Ok) {
                    Write-Host $r.Body -ForegroundColor Red
                } else {
                    $snap = Wait-ConsoleSettle $Session
                    Write-ConsoleScreen $snap $baseline
                    $baseline = @($snap.Rows)
                }
                continue
            }
            '^:scroll\s+(-?\d+)$' {
                $snap = Invoke-Console $Session -Command "scroll $($Matches[1])"
                if ($snap.Ok) {
                    $s2 = Wait-ConsoleSettle $Session
                    Write-ConsoleScreen $s2 @()      # a scroll is not new output
                }
                continue
            }
            '^:screen$' {
                $snap = Get-ConsoleScreen $Session
                $rows = Format-ConsoleRows @($snap.Rows)
                Write-Host ('-' * 60) -ForegroundColor DarkGray
                foreach ($r in $rows) { Write-Host $r -ForegroundColor Gray }
                Write-Host ('-' * 60) -ForegroundColor DarkGray
                continue
            }
            default {
                if ($line.Trim() -eq '') { continue }
                $sent = Send-Console $Session -Text $line
                if ($sent -and -not $sent.Ok) {
                    Write-Host "not typed: $($sent.Body)" -ForegroundColor Red
                    continue
                }
                $snap = Wait-ConsoleSettle $Session
                Write-ConsoleScreen $snap $baseline
                $baseline = @($snap.Rows)
            }
        }
    }
}

function Invoke-ConsoleDriver {
    <#
      The command-line form: one launch, one command, one assertion. The
      functions above are the reusable part - this is the shape a single check
      takes, and it is deliberately thin so that anything it can do, a dot-
      sourcing driver can also do.
    #>
    param(
        [string]$App, [string[]]$Argv, [int]$Port = 7979, [int]$GuestId = -1,
        [string]$Command, [switch]$NoEnter, [string]$Expect, [string]$NotExpect,
        [switch]$ExpectExit, [switch]$Screen, [switch]$Interactive, [int]$TimeoutSec = 20,
        [string]$Adb = 'adb', [string]$Serial, [switch]$NoForward, [switch]$NoLaunch,
        [ValidateSet('finish','stay')][string]$AfterExit = 'stay'
    )
    Test-ConsoleAdb -AdbPath $Adb -DeviceSerial $Serial

    if ($App -and -not $NoLaunch) {
        Write-Host "launch  $App" -ForegroundColor DarkGray
        [void](Start-ConsoleGuest -AppId $App -GuestArgv $Argv -AdbPath $Adb `
                                    -Serial $Serial -AfterExit $AfterExit)
    }

    $s = New-ConsoleSession -PortNumber $Port -Guest $GuestId -AdbPath $Adb `
                             -Serial $Serial -SkipForward:$NoForward
    $exitCode = 0
    try {
        # A launch returns before the guest boots, so typing straight away is a
        # race the console would (rightly) refuse. Wait for the run instead.
        if (-not (Wait-ConsoleRunning $s -TimeoutMs ($TimeoutSec * 1000))) {
            Write-Warning "guest did not start within $TimeoutSec s"
            $st = Get-ConsoleStatus $s
            Write-Warning "status: $($st.Body)"
            return 4
        }
        if ($Screen) {
            $snap = Get-ConsoleScreen $s
            Write-Host $snap.Status
            Write-Host ('-' * 60)
            Write-Host $snap.Text
            Write-Host ('-' * 60)
        }
        if ($Interactive) {
            $exitCode = Invoke-ConsoleRepl $s
        }
        if ($Command) {
            $sent = Send-Console $s -Text $Command -NoEnter:$NoEnter
            if ($sent -and -not $sent.Ok) {
                Write-Warning "not typed: $($sent.Body)"
                $exitCode = 2
            }
        }
        if ($Expect) {
            $r = Wait-ConsoleScreen $s -Pattern $Expect -NotPattern $NotExpect `
                                   -TimeoutMs ($TimeoutSec * 1000)
            if ($r.Ok) { Write-Host "ok      /$Expect/" -ForegroundColor Green }
            else {
                Write-Host "TIMEOUT /$Expect/" -ForegroundColor Red
                Write-Host $r.Text
                $exitCode = 1
            }
        }
        if ($ExpectExit) {
            if (Wait-ConsoleExit $s -TimeoutMs ($TimeoutSec * 1000)) {
                Write-Host 'ok      guest exited' -ForegroundColor Green
            } else {
                Write-Host 'TIMEOUT guest still running' -ForegroundColor Red
                $exitCode = 1
            }
        }
    } finally {
        Close-ConsoleSession $s
    }
    return $exitCode
}

# Run only when executed, never when dot-sourced: this file is both a driver
# and the library it drives, and a dot-sourcing caller wants the functions with
# no device check and no exit. InvocationName is '.' for a dot-source and the
# script's own name otherwise, which is the only thing that tells them apart.
if ($MyInvocation.InvocationName -ne '.') {
    if ($SelfTest) {
        exit ([int]((Invoke-ConsoleSelfTest) -gt 0))
    }
    try {
        exit (Invoke-ConsoleDriver @PSBoundParameters)
    } catch {
        Write-Error $_
        exit 3
    }
}
