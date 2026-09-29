#Requires -Version 7.0
<#
.SYNOPSIS
    Drive an Android-hosted guest's console from the PC.

.DESCRIPTION
    The console is a byte pipe on loopback, framed exactly as adb frames a
    shell (ShellProtocol): a 5-byte header - one id byte, then a 4-byte
    little-endian length - and a payload, with one id per logical stream.

        0  stdin           this side -> guest      raw keystrokes
        1  stdout          guest      -> this side
        2  stderr          guest      -> this side
        3  exit            guest      -> this side  the status, as text
        4  close stdin     this side -> guest      the guest sees EOF
        5  window size     this side -> guest      ASCII winsize

    stdout and stderr are separate ids and this driver keeps them apart,
    because they are indistinguishable by the time they reach the guest's
    terminal and a test that matches on output does not want an error
    message satisfying it. -Stream picks which one to read.

    Everything blocks. There is no poll loop and nothing to tune: a client
    waits on a read, and the one wait here is for a stream to go quiet,
    which is the same shape as Expect-Quiet in ash_drive.ps1.

    Keystrokes go in raw. The core's own line discipline runs them - ICRNL,
    ISIG for Ctrl-C, erase, echo - so this types and does not emulate a
    terminal.

.PARAMETER App
    Guest to boot. Defaults to test_busybox, which with no arguments is an
    interactive busybox shell: the useful thing to land in, and the reason
    a driver needs no ceremony to get a working prompt.

.PARAMETER Argv
    Guest argv. One string is split on '|' (`-Argv '-c|ls -l'`); an array is
    taken as-is. The string form is the one `pwsh -File` can deliver - that
    binder reads a token starting with a single dash as a parameter name, so
    the array form cannot carry `sh -c` at all.

.PARAMETER Command
    Text to type, Enter appended unless -NoEnter.

.PARAMETER Expect
    Regex to wait for. This is the assertion.

.PARAMETER NotExpect
    Regex that must not appear within the quiet window that follows.

.PARAMETER ExpectExit
    Wait for the guest's exit packet, and exit with the status it carried.
    A batch guest's result *is* its status, so a driver that ignores it
    passes a guest that failed.

.PARAMETER Stream
    Which stream -Expect and -Screen read: stdout, stderr or both.

.PARAMETER Interactive
    A shell at the guest. Type a line, it is typed, and what the guest says
    comes back when it stops saying it. `:keys CtrlC` sends a keystroke,
    `:quit` leaves.

.EXAMPLE
    # a guest that prints and exits 7
    pwsh ./tools/android_console.ps1 -Serial <adb-serial> \
         -App test_busybox -Argv '-c|exit 7' -ExpectExit

.EXAMPLE
    # a shell to type in
    pwsh ./tools/android_console.ps1 -Serial <adb-serial> -Interactive

.EXAMPLE
    # dot-source and drive it
    . ./tools/android_console.ps1
    $s = New-ShellSession -Serial <adb-serial>
    Send-Shell $s -Keys CtrlC
    Wait-ShellText $s -Pattern 'vp>' -Stream stderr
    Close-ShellSession $s

.EXAMPLE
    pwsh ./tools/android_console.ps1 -SelfTest     # no device needed
#>
[CmdletBinding()]
param(
    [string]$App = 'test_busybox',
    [string[]]$Argv,
    [int]$Port = 7979,
    [string]$Command,
    [switch]$NoEnter,
    [string]$Expect,
    [string]$NotExpect,
    [switch]$ExpectExit,
    [switch]$Screen,
    [ValidateSet('stdout', 'stderr', 'both')][string]$Stream = 'stdout',
    [switch]$Interactive,
    [int]$TimeoutSec = 20,
    [int]$QuietMs = 250,
    [string]$Adb = 'adb',
    [string]$Serial,
    [switch]$NoForward,
    [switch]$NoLaunch,
    [ValidateSet('finish', 'stay')][string]$AfterExit = 'stay',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$script:PKG = 'com.rvvm.android'

# adb's ShellProtocol ids, so the two ends cannot drift apart by a rename.
$script:IdStdin      = 0
$script:IdStdout     = 1
$script:IdStderr     = 2
$script:IdExit       = 3
$script:IdCloseStdin = 4
$script:IdWindowSize = 5

# Keystrokes, so a driver does not have to know that ^C is 0x03 and the up
# arrow is ESC [ A. Anything not in here is typed as literal text.
$script:Keys = @{
    'Enter'     = @(0x0d)
    'Tab'       = @(0x09)
    'Esc'       = @(0x1b)
    'Backspace' = @(0x7f)
    'CtrlC'     = @(0x03)
    'CtrlD'     = @(0x04)
    'CtrlZ'     = @(0x1a)
    'Up'        = @(0x1b, 0x5b, 0x41)
    'Down'      = @(0x1b, 0x5b, 0x42)
    'Right'     = @(0x1b, 0x5b, 0x43)
    'Left'      = @(0x1b, 0x5b, 0x44)
}

# ==================================================================
# encoding
# ==================================================================

function New-ShellPacket {
    <#
      The 5-byte header and its payload. Little endian is written by hand:
      the wire order is part of the format, and a BitConverter on a
      big-endian host would produce a packet only that host can read.
    #>
    param([int]$Id, [byte[]]$Payload = @())
    $len = $Payload.Length
    $h = [byte[]]::new(5)
    $h[0] = [byte]$Id
    $h[1] = [byte]($len -band 0xff)
    $h[2] = [byte](($len -shr 8) -band 0xff)
    $h[3] = [byte](($len -shr 16) -band 0xff)
    $h[4] = [byte](($len -shr 24) -band 0xff)
    if ($len -eq 0) { return $h }
    return [byte[]]($h + $Payload)
}

function ConvertTo-KeyBytes {
    <#
      Text plus key names -> the bytes a keyboard would deliver. A literal
      key name in the text is replaced, so 'ls' + 'Enter' is two arguments
      rather than one string with an escape in it.
    #>
    param([string]$Text = '', [string[]]$Key = @(), [switch]$NoEnter)
    $bytes = [System.Collections.Generic.List[byte]]::new()
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

function Resolve-GuestArgv {
    <#
      One string containing '|' is split on it; anything else is taken as-is.
      See .PARAMETER Argv for why the string form has to exist.
    #>
    param($Argv)
    if ($null -eq $Argv) { return @() }
    $arr = @($Argv)
    if ($arr.Count -eq 1 -and $arr[0].Contains('|')) { return @($arr[0] -split '\|') }
    return $arr
}

function ConvertTo-ArgvBlob {
    <#
      Guest argv as one base64 string: NUL-joined, then base64. `--esa` is a
      multi-value `am` option and multi-value options are where shells
      disagree - on a ColorOS build `--esa argv a b c` puts `[a]` in the
      intent, and `--esa argv -c "ls"` throws. One extra, nothing to
      reinterpret.
    #>
    param([string[]]$Argv)
    if (-not $Argv -or $Argv.Count -eq 0) { return '' }
    return [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes([string]::Join([char]0, $Argv)))
}

# ==================================================================
# session
# ==================================================================

function Test-ConsoleAdb {
    <#
      Exactly one usable device, and it is the one named. With two phones
      attached `adb forward` fails with "more than one device", and the
      failure looks like a console that is not listening.
    #>
    param([string]$AdbPath = 'adb', [string]$Serial)
    & $AdbPath devices | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $AdbPath" }
    $devs = & $AdbPath devices | Select-Object -Skip 1 |
            Where-Object { $_ -match '\sdevice$' } |
            ForEach-Object { ($_ -split '\s+')[0] }
    if (-not $devs) { throw 'no device: `adb devices` lists none in "device" state' }
    if ($Serial) {
        if ($devs -notcontains $Serial) {
            throw "device '$Serial' not connected (have: $($devs -join ', '))"
        }
    } elseif ($devs.Count -gt 1) {
        throw ("more than one device attached ($($devs -join ', ')) - name one with -Serial," +
               ' or the forward below would go to whichever adb picks')
    }
}

function Get-AdbTargetArgs {
    param([string]$Serial)
    if ($Serial) { return @('-s', $Serial) }
    return @()
}

function Start-ShellHost {
    <#
      Bring the host up with the console listening and *no* guest, through the
      picker rather than through a guest Activity.

      The handshake that makes a batch guest usable. The console is a live
      byte pipe, so it does not replay: a driver that launched the guest and
      then connected was racing a run that might be over already, and
      `busybox sh -c "..."` finishes in milliseconds. Host first, guest second,
      is the only order in which something is listening when the guest speaks.

      The picker is where that handshake happens, and it works because the two
      live in different tasks: the picker stays up holding nothing, the guest
      gets its own. Routing it through the guest Activity instead would mean
      clearing that task to get a second instance, and the moment between
      clearing it and the replacement starting is a window in which the
      process has no Activity at all - which is long enough for the system to
      take it, and the driver with it.
    #>
    param([string]$AdbPath = 'adb', [string]$Serial)
    $a = (Get-AdbTargetArgs $Serial) +
         @('shell', 'am', 'start', '-n', "$script:PKG/.SimpleLauncherActivity",
           '--ez', 'console', 'true')
    $out = & $AdbPath @a 2>&1
    if ($LASTEXITCODE -ne 0) { throw "am start failed: $(($out | Select-Object -First 3) -join ' ')" }
}

function Start-ShellGuest {
    <#
      Boot the guest, into a console that is already listening.

      The console flag rides the launch intent rather than a setprop: the app
      process is already running by the time an Activity reads an intent, so
      a property would have to be set before every launch and forgetting it
      once would look like a hang.

      after_guest_exit defaults to 'stay' here, unlike the Activity's own
      default of 'finish'. Finishing tears the Activity down, which releases
      the host; staying keeps the run's last screen around to read.
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
        $a += @('--es', 'argv_b64', (ConvertTo-ArgvBlob $GuestArgv))
    }
    $out = & $AdbPath @a 2>&1
    if ($LASTEXITCODE -ne 0) {
        # Trimmed: a failed `am start` on a ColorOS build prints the whole
        # Intent.parseCommandArgs stack, which buries the one line that says
        # what was wrong with the command.
        throw "am start failed: $(($out | Select-Object -First 3) -join ' ')"
    }
    return ($out | Out-String).Trim()
}

function Read-Exact {
    <#
      Exactly @n bytes, blocking. A short read is not a short packet: a
      length-prefixed frame is only meaningful whole, so a partial one is
      looped over rather than passed on.
    #>
    param($Sock, [int]$Count)
    $buf = [byte[]]::new($Count)
    $got = 0
    while ($got -lt $Count) {
        $n = $Sock.Receive($buf, $got, $Count - $got, [System.Net.Sockets.SocketFlags]::None)
        if ($n -le 0) { return $null }      # the peer closed
        $got += $n
    }
    return $buf
}

function New-ShellSession {
    <#
      Forward the port and connect.

      No reader thread, and none needed: the client is synchronous, so it
      blocks in Receive. A thread here would only have to hand the same
      packets to the same thread that has to interpret them, and PowerShell's
      threading is the least portable thing in this script. The waits below
      are a blocking read with a deadline on top, not a poll of a state
      variable - the difference matters when a guest prints nothing at all.
    #>
    param(
        [int]$PortNumber = 7979,
        [string]$AdbPath = 'adb',
        [string]$Serial,
        [int]$ConnectTimeoutMs = 30000,
        [switch]$SkipForward
    )
    Test-ConsoleAdb -AdbPath $AdbPath -Serial $Serial
    if (-not $SkipForward) {
        $a = (Get-AdbTargetArgs $Serial) + @('forward', "tcp:$PortNumber", "tcp:$PortNumber")
        & $AdbPath @a | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb forward tcp:$PortNumber failed" }
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
    $sock = $null
    $last = 'timed out'
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $sock = [System.Net.Sockets.TcpClient]::new()
            $sock.Connect('127.0.0.1', $PortNumber)

            # A successful Connect proves nothing here. `adb forward` accepts
            # on the host side first and only then reaches for the device
            # socket, so connecting before the listener is up yields a
            # connection that is already dead - and it looks exactly like a
            # console that is not listening. So: prove the peer is there by
            # waiting for a byte that does not come. A live console blocks; a
            # dead relay answers with end-of-stream immediately.
            $sock.Client.ReceiveTimeout = 400
            $probe = [byte[]]::new(1)
            $alive = $true
            try {
                $alive = $sock.Client.Receive($probe, 0, 1,
                                              [System.Net.Sockets.SocketFlags]::None) -gt 0
            } catch {
                # Timed out with nothing to read: that is a live console.
                $alive = $true
            }
            if ($alive) { break }
            $last = 'the forward accepted and then closed: no listener yet'
            $sock.Close(); $sock = $null
        } catch {
            $last = $_.Exception.Message
            if ($sock) { try { $sock.Close() } catch { }; $sock = $null }
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $sock) {
        throw ("cannot reach the console on 127.0.0.1:{0} within {1} ms: {2}`n" -f
               $PortNumber, $ConnectTimeoutMs, $last) +
              "`nis the app running with --ez console true?  check: adb logcat -s RVVM-Console"
    }

    return [pscustomobject]@{
        Port    = $PortNumber
        Client  = $sock
        Sock    = $sock.Client
        Stdout  = [System.Text.StringBuilder]::new()
        Stderr  = [System.Text.StringBuilder]::new()
        ExitCode = $null
        LastRx  = [DateTime]::UtcNow
        Closed  = $false
    }
}

function Close-ShellSession($Session) {
    if (-not $Session) { return }
    try { $Session.Client.Close() } catch { }
}

function Send-ShellPacket {
    param($Session, [int]$Id, [byte[]]$Payload = @())
    $b = New-ShellPacket -Id $Id -Payload $Payload
    $sent = 0
    while ($sent -lt $b.Length) {
        $sent += $Session.Sock.Send($b, $sent, $b.Length - $sent,
                                    [System.Net.Sockets.SocketFlags]::None)
    }
}

function Send-Shell {
    <#
      Type at the guest. The bytes go in raw and the core's line discipline
      does the rest, so this is keystrokes and not terminal escapes.
    #>
    param($Session, [string]$Text = '', [string[]]$Key = @(), [switch]$NoEnter)
    Send-ShellPacket $Session $script:IdStdin (ConvertTo-KeyBytes -Text $Text -Key $Key -NoEnter:$NoEnter)
}

function Send-ShellCloseStdin {
    <#
      The guest sees EOF. A shell reads it as end of input, which is what
      Ctrl-D does - except that here it is the client's decision and not a
      keystroke that happens to mean it.
    #>
    param($Session)
    Send-ShellPacket $Session $script:IdCloseStdin
}

function Send-ShellWindowSize {
    param($Session, [int]$Rows, [int]$Cols)
    # ASCII winsize, as adb sends it.
    $payload = [Text.Encoding]::ASCII.GetBytes("$Rows`:$Cols")
    Send-ShellPacket $Session $script:IdWindowSize $payload
}

function Read-ShellPacket {
    <#
      The next packet, blocking for up to @TimeoutMs. Returns $null when the
      wait elapsed or the peer closed - a packet is only meaningful whole, so
      a partial one is looped over inside rather than handed back.

      A packet that arrives updates LastRx, which is what the quiet wait
      below is really measuring: time since the guest last said anything.
    #>
    param($Session, [int]$TimeoutMs)
    $Sock = $Session.Sock
    $Sock.ReceiveTimeout = [Math]::Max(1, [Math]::Min($TimeoutMs, 250))
    $h = Read-Exact $Sock 5
    if ($null -eq $h) { $Session.Closed = $true; return $null }
    $len = [int]$h[1] -bor ([int]$h[2] -shl 8) -bor ([int]$h[3] -shl 16) -bor ([int]$h[4] -shl 24)
    if ($len -gt 0) {
        $body = Read-Exact $Sock $len
        if ($null -eq $body) { $Session.Closed = $true; return $null }
    } else {
        $body = [byte[]]::new(0)
    }
    $Session.LastRx = [DateTime]::UtcNow
    $text = [Text.Encoding]::UTF8.GetString($body)
    switch ($h[0]) {
        $script:IdStdout { [void]$Session.Stdout.Append($text) }
        $script:IdStderr { [void]$Session.Stderr.Append($text) }
        $script:IdExit   { $Session.ExitCode = $text.Trim() }
    }
    return $h[0]
}

function Get-ShellText {
    <#
      What each stream has said, without reading anything new.

      -Stream picks which one a caller wants, and the split is the point:
      they are one screen to the guest's terminal and two ids here, so a
      test can read stdout without an error message satisfying it.
    #>
    param($Session, [ValidateSet('stdout', 'stderr', 'both')][string]$Which = 'stdout')
    switch ($Which) {
        'stdout' { return $Session.Stdout.ToString() }
        'stderr' { return $Session.Stderr.ToString() }
        default  { return $Session.Stdout.ToString() + $Session.Stderr.ToString() }
    }
}

function Wait-ShellText {
    <#
      Read until what was asked for arrives, or the deadline passes.

      Every iteration is a blocking read, not a test of a flag: a command
      that answers in 20 ms is noticed in 20 ms, and one that never answers
      costs exactly the timeout. Quiet is the case that has no output to
      wait for - an interactive shell never stops talking - and it settles
      on the absence of packets instead, which is the same idea as
      Expect-Quiet in ash_drive.ps1.
    #>
    param(
        $Session,
        [string]$Pattern,
        [string]$NotPattern,
        [switch]$Quiet,
        [int]$QuietMs = 250,
        [switch]$Exit,
        [ValidateSet('stdout', 'stderr', 'both')][string]$Stream = 'stdout',
        [int]$TimeoutMs = 20000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ($true) {
        if ($Exit -and $null -ne $Session.ExitCode) {
            return [pscustomobject]@{ Ok = $true; Text = (Get-ShellText $Session $Stream); Code = $Session.ExitCode }
        }
        $text = Get-ShellText $Session $Stream
        if ($Pattern -and $text -match $Pattern) {
            return [pscustomobject]@{ Ok = $true; Text = $text; Code = $Session.ExitCode }
        }
        if ($NotPattern -and $text -notmatch $NotPattern) {
            return [pscustomobject]@{ Ok = $true; Text = $text; Code = $Session.ExitCode }
        }
        if ($Quiet -and
            ([DateTime]::UtcNow - $Session.LastRx).TotalMilliseconds -ge $QuietMs) {
            return [pscustomobject]@{ Ok = $true; Text = $text; Code = $Session.ExitCode }
        }
        if ($Session.Closed -and $null -eq $Session.ExitCode) {
            return [pscustomobject]@{ Ok = $false; Text = $text; Code = $Session.ExitCode }
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            return [pscustomobject]@{ Ok = $false; Text = $text; Code = $Session.ExitCode }
        }
        [void](Read-ShellPacket $Session 250)
    }
}

function Wait-ShellExit {
    <#
      The run's status, which arrives as its own packet - after the output,
      because the guest's thread has already handed every byte it wrote to
      the writer by the time it exits. A client that stops reading on `exit`
      therefore has a complete transcript, not a truncated one.
    #>
    param($Session, [int]$TimeoutMs = 20000)
    $r = Wait-ShellText $Session -Exit -TimeoutMs $TimeoutMs
    if (-not $r.Ok) { return $null }
    $code = 0
    if ([int]::TryParse($r.Code, [ref]$code)) { return $code }
    return $null
}

# ==================================================================
# interactive
# ==================================================================

function Invoke-ShellRepl {
    param($Session, [string]$Prompt = 'rvvm> ')
    Write-Host 'Ctrl-D or :quit to leave. :keys CtrlC sends a keystroke.' -ForegroundColor DarkGray
    while ($true) {
        Write-Host -NoNewline $Prompt -ForegroundColor Cyan
        $line = Read-Host
        if ($null -eq $line) { return 0 }

        switch -Regex ($line.Trim()) {
            '^(:q|quit|exit|:quit)$' { return 0 }
            '^:keys\s+(.+)$' {
                Send-Shell $Session -Key ($Matches[1] -split '\s+')
            }
            '^:eof$' { Send-ShellCloseStdin $Session }
            default {
                if ($line.Trim() -ne '') { Send-Shell $Session -Text $line }
            }
        }
        # What it said, once it stops saying it.
        $r = Wait-ShellText $Session -Quiet -QuietMs $QuietMsDefault `
                                 -Stream both -TimeoutMs 10000
        $rows = @(Format-ConsoleRows $r.Text)
        if ($rows.Count) {
            Write-Host ('-' * 60) -ForegroundColor DarkGray
            foreach ($row in $rows) { Write-Host $row -ForegroundColor Gray }
            Write-Host ('-' * 60) -ForegroundColor DarkGray
        }
    }
}

# The REPL and the CLI are in the same script, and the CLI's -QuietMs is the
# only definition of "how long is quiet" - so the REPL takes it as a
# parameter rather than reaching for a script-scope variable.
$QuietMsDefault = if ($PSBoundParameters.ContainsKey('QuietMs')) { $QuietMs } else { 250 }

function Format-ConsoleRows {
    <#
      Blank rows off the top and bottom, so a reply does not arrive padded
      out with nothing. Blank rows in the middle stay: their absence is
      what separates a command from its output.
    #>
    param([string]$Text)
    if (-not $Text) { return @() }
    $rows = @($Text -split "`n")
    $first = -1; $last = -1
    for ($i = 0; $i -lt $rows.Count; $i++) {
        if (($rows[$i] -replace '\s', '') -ne '') { if ($first -lt 0) { $first = $i }; $last = $i }
    }
    if ($first -lt 0) { return @() }
    return $rows[$first..$last]
}

# ==================================================================
# self-test
# ==================================================================

function Invoke-ShellSelfTest {
    <#
      What can be checked with no device: the framing, the key table, the
      argv blob, and the client's own reader against a scripted peer. The
      reader is the part worth testing - it is where a length-prefixed
      protocol goes wrong, and it is the part a device would only exercise
      by accident.
    #>
    $script:stFails = 0
    function Check($name, $cond) {
        if ($cond) { Write-Host "  ok    $name" -ForegroundColor DarkGray }
        else { Write-Host "  FAIL  $name" -ForegroundColor Red; $script:stFails++ }
    }

    Write-Host 'framing'
    $p = New-ShellPacket -Id 1 -Payload ([Text.Encoding]::ASCII.GetBytes('abc'))
    Check 'header is 5 bytes + payload' ($p.Length -eq 8)
    Check 'id first'   ($p[0] -eq 1)
    Check 'length LE'  ($p[1] -eq 3 -and $p[2] -eq 0 -and $p[3] -eq 0 -and $p[4] -eq 0)
    $big = New-ShellPacket -Id 2 -Payload ([byte[]]::new(300))
    Check 'length over 255' ($big[1] -eq 44 -and $big[2] -eq 1)
    Check 'empty payload'  ((New-ShellPacket -Id 3).Length -eq 5)

    Write-Host 'keys'
    Check 'text + Enter'  (((ConvertTo-KeyBytes 'ls') | ForEach-Object { $_.ToString('x2') }) -join '') -eq '6c730d'
    Check 'no Enter'      (((ConvertTo-KeyBytes 'ls' -NoEnter) | ForEach-Object { $_.ToString('x2') }) -join '') -eq '6c73'
    Check 'CtrlC is one'  (((ConvertTo-KeyBytes '' -Key CtrlC) | ForEach-Object { $_.ToString('x2') }) -join '') -eq '03'
    Check 'Up is ESC [ A' (((ConvertTo-KeyBytes '' -Key Up) | ForEach-Object { $_.ToString('x2') }) -join '') -eq '1b5b41'
    try { [void](ConvertTo-KeyBytes '' -Key Nope); Check 'unknown key throws' $false }
    catch { Check 'unknown key throws' $true }

    Write-Host 'argv'
    Check 'blob round-trips' (
        [Text.Encoding]::UTF8.GetString(
            [Convert]::FromBase64String((ConvertTo-ArgvBlob @('-c', 'echo hi')))) -ceq
        ("-c" + [char]0 + 'echo hi'))
    Check 'string splits on bar' ((Resolve-GuestArgv '-c|exit 7')[1] -ceq 'exit 7')
    Check 'array passes through' ((Resolve-GuestArgv @('ls', '-l')).Count -eq 2)

    Write-Host 'reader'
    # A scripted peer on loopback, on this thread: the transcript is a few
    # hundred bytes, so it fits in the socket buffers and the peer can write
    # it before the client reads without either side waiting on the other.
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.Connect('127.0.0.1', $port)   # completes off the backlog
    $peer = $listener.AcceptTcpClient()

    $script = @()
    $script += New-ShellPacket -Id 1 -Payload ([Text.Encoding]::UTF8.GetBytes("out-1`n"))
    $script += New-ShellPacket -Id 2 -Payload ([Text.Encoding]::UTF8.GetBytes("err-1`n"))
    $script += New-ShellPacket -Id 1 -Payload ([Text.Encoding]::UTF8.GetBytes("out-2`n"))
    $script += New-ShellPacket -Id 3 -Payload ([Text.Encoding]::UTF8.GetBytes('7'))
    $blob = [byte[]]($script | ForEach-Object { $_ })

    $s = [pscustomobject]@{
        Port = $port; Client = $client; Sock = $client.Client
        Stdout = [System.Text.StringBuilder]::new()
        Stderr = [System.Text.StringBuilder]::new()
        ExitCode = $null; LastRx = [DateTime]::UtcNow; Closed = $false
    }

    $peer.GetStream().Write($blob, 0, $blob.Length)
    $peer.GetStream().Flush()
    $peer.Close()

    $r = Wait-ShellText $s -Exit -TimeoutMs 5000
    Check 'reader saw the exit' ($r.Ok -and $r.Code -eq '7')
    Check 'stdout accumulated'  ((Get-ShellText $s 'stdout') -ceq "out-1`nout-2`n")
    Check 'stderr kept separate' ((Get-ShellText $s 'stderr') -ceq "err-1`n")
    Check 'both is both'         ((Get-ShellText $s 'both') -match 'out-1' -and
                                   (Get-ShellText $s 'both') -match 'err-1')
    Check 'a 300 byte packet' (
        ($p = New-ShellPacket -Id 1 -Payload ([byte[]]::new(300))) -and $p.Length -eq 305)
    $client.Close()
    $listener.Stop()

    Write-Host ''
    if ($script:stFails -eq 0) { Write-Host 'self-test PASS' -ForegroundColor Green }
    else { Write-Host "self-test FAIL ($script:stFails)" -ForegroundColor Red }
    return $script:stFails
}

# ==================================================================
# entry point
# ==================================================================

function Invoke-ShellDriver {
    param(
        [string]$App = 'test_busybox', [string[]]$Argv, [int]$Port = 7979,
        [string]$Command, [switch]$NoEnter, [string]$Expect, [string]$NotExpect,
        [switch]$ExpectExit, [switch]$Screen, [switch]$Interactive,
        [ValidateSet('stdout', 'stderr', 'both')][string]$Stream = 'stdout',
        [int]$TimeoutSec = 20, [int]$QuietMs = 250,
        [string]$Adb = 'adb', [string]$Serial, [switch]$NoForward, [switch]$NoLaunch,
        [string]$AfterExit = 'stay'
    )
    Test-ConsoleAdb -AdbPath $Adb -Serial $Serial

    $s = $null
    if ($App -and -not $NoLaunch) {
        # Host first, guest second, connect in between. See Start-ShellHost.
        Write-Host "launch  $App" -ForegroundColor DarkGray
        Start-ShellHost -AdbPath $Adb -Serial $Serial
        $s = New-ShellSession -PortNumber $Port -AdbPath $Adb -Serial $Serial `
                              -SkipForward:$NoForward
        $s | Add-Member -NotePropertyName OwnsForward -NotePropertyValue (-not $NoForward) -Force
        [void](Start-ShellGuest -AppId $App -GuestArgv (Resolve-GuestArgv $Argv) `
                                -AdbPath $Adb -Serial $Serial -AfterExit $AfterExit)
    } else {
        $s = New-ShellSession -PortNumber $Port -AdbPath $Adb -Serial $Serial `
                              -SkipForward:$NoForward
        $s | Add-Member -NotePropertyName OwnsForward -NotePropertyValue (-not $NoForward) -Force
    }
    $exitCode = 0
    try {
        if ($Screen) {
            # Read first: Get-ShellText only reports what has already been
            # taken off the socket, and nothing has been taken off it yet.
            [void](Wait-ShellText $s -Quiet -QuietMs $QuietMs -Stream $Stream `
                             -TimeoutMs 1500)
            Write-Host (Get-ShellText $s $Stream)
        }
        if ($Command) {
            Send-Shell $s -Text $Command -NoEnter:$NoEnter
        }
        if ($Interactive) {
            $exitCode = Invoke-ShellRepl $s
        }
        if ($Expect -or $NotExpect) {
            $r = Wait-ShellText $s -Pattern $Expect -NotPattern $NotPattern `
                               -Stream $Stream -TimeoutMs ($TimeoutSec * 1000)
            if ($r.Ok) { Write-Host "ok      /$Expect/" -ForegroundColor Green }
            else {
                Write-Host "TIMEOUT /$Expect/" -ForegroundColor Red
                Write-Host $r.Text
                $exitCode = 1
            }
        }
        if ($ExpectExit) {
            $code = Wait-ShellExit $s -TimeoutMs ($TimeoutSec * 1000)
            if ($null -eq $code) {
                Write-Host 'TIMEOUT guest did not report an exit' -ForegroundColor Red
                $exitCode = 1
            } elseif ($code -eq 0) {
                Write-Host 'ok      guest exited 0' -ForegroundColor Green
            } else {
                # A batch guest's result is its status. Reporting the timeout
                # colour for a guest that exited 7 would make a failing
                # command indistinguishable from a lost thread.
                Write-Host "guest exited $code" -ForegroundColor Red
                $exitCode = $code
            }
        }
    } finally {
        Close-ShellSession $s
    }
    return $exitCode
}

# Run only when executed, never when dot-sourced: this file is both a driver
# and the library it drives, and a dot-sourcing caller wants the functions
# with no device check and no exit.
if ($MyInvocation.InvocationName -ne '.') {
    if ($SelfTest) {
        exit ([int]((Invoke-ShellSelfTest) -gt 0))
    }
    try {
        exit (Invoke-ShellDriver @PSBoundParameters)
    } catch {
        # Write-Host, not Write-Error: this script runs with
        # $ErrorActionPreference = 'Stop', so a Write-Error here would itself
        # terminate and the exit code below would never run - a driver would
        # see a failed run reported as success, which is the one thing a driver
        # must never do.
        Write-Host $_.Exception.Message -ForegroundColor Red
        exit 3
    }
}
