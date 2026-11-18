#Requires -Version 5.1
<#
    Shared console plumbing for the ash e2e drivers.

    A core serves the guest console on a loopback TCP port and speaks adb's
    ShellProtocol over it: a 5-byte header (id, then length as four little-endian
    bytes) and one id per logical stream - 0 stdin, 1 stdout, 2 stderr, 3 exit,
    4 close stdin, 5 window size, 6 ready. There is no socket file and no
    `--sock-path` any more: those went with the guest-side session server, and
    the port the core was started with *is* the endpoint, so the driver derives
    it instead of asking. The framing itself lives in ash_drive.ps1; these
    helpers only answer "where is the core, and how do I connect to it".
#>

# The host endpoint for the core on @Port.
function Get-AshSockPath {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][int]$Port)
    return "127.0.0.1:$Port"
}

# A connected TCP stream socket to @Path (throws if nothing is listening).
function New-AshSocket {
    param([Parameter(Mandatory)][string]$Path)
    $hostName, $portText = $Path -split ':', 2
    $s = [System.Net.Sockets.TcpClient]::new()
    $s.Connect($hostName, [int]$portText)
    return $s.Client
}

# A connected session: the raw Socket and a NetworkStream over it.
function Connect-AshSession {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][int]$Port)
    $path = Get-AshSockPath -Exe $Exe -Port $Port
    $sock = New-AshSocket -Path $path
    return [PSCustomObject]@{
        Path   = $path
        Socket = $sock
        Stream = [System.Net.Sockets.NetworkStream]::new($sock)
    }
}

# Send one framed packet to a raw console socket.
function Send-AshPacketTo {
    param([Parameter(Mandatory)]$Sock, [Parameter(Mandatory)][int]$Id, [byte[]]$Payload)
    $len = if ($Payload) { $Payload.Length } else { 0 }
    $frame = New-Object byte[] (5 + $len)
    $frame[0] = [byte]$Id
    $frame[1] = [byte]($len -band 0xff)
    $frame[2] = [byte](($len -shr 8) -band 0xff)
    $frame[3] = [byte](($len -shr 16) -band 0xff)
    $frame[4] = [byte](($len -shr 24) -band 0xff)
    if ($len) { [Array]::Copy($Payload, 0, $frame, 5, $len) }
    $sent = 0
    while ($sent -lt $frame.Length) {
        $sent += $Sock.Send($frame, $sent, $frame.Length - $sent, [System.Net.Sockets.SocketFlags]::None)
    }
}

# Run one command in a fresh session and return its output - the job `ash -c`
# used to do, now over the console pipe the way a client does it: the text goes
# in as stdin, then close-stdin, so the shell sees EOF, runs it and exits. The
# connection closing is the end of the output; the timeout is only there for a
# command that neither prints nor ends.
# Run one command in a fresh session and return its output - the job `ash -c`
# used to do, now over the console pipe the way a client does it.
#
# Two things the protocol requires, and both are why this is not "write, read":
# the server sends a READY packet before it will take stdin (a client that sends
# first is writing into a session that does not exist yet), and the command has
# to be closed with a close-stdin packet or the shell waits for more input
# instead of running it. Everything the session says in between is stdout/stderr
# frames.
function Invoke-AshCommand {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][string]$Command,
        [int]$TimeoutMs = 20000
    )
    $s = Connect-AshSession -Exe $Exe -Port $Port
    $sock = $s.Socket
    try {
        $sock.ReceiveTimeout = 300
        $sb    = New-Object System.Text.StringBuilder
        $dec   = [Text.Encoding]::UTF8.GetDecoder()
        $rx    = New-Object 'System.Collections.Generic.List[byte]'
        $chars = New-Object char[] 8192
        $sent  = $false
        $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
        while ([DateTime]::UtcNow -lt $deadline) {
            $tmp = New-Object byte[] 4096
            try {
                $n = $sock.Receive($tmp, 0, $tmp.Length, [System.Net.Sockets.SocketFlags]::None)
            } catch {
                $code = $_.Exception.SocketErrorCode
                if ($code -eq [System.Net.Sockets.SocketError]::TimedOut -or
                    $code -eq [System.Net.Sockets.SocketError]::WouldBlock) { continue }
                break
            }
            if ($n -le 0) { break }
            for ($i = 0; $i -lt $n; $i++) { $rx.Add($tmp[$i]) }
            while ($rx.Count -ge 5) {
                $len = [int]$rx[1] -bor ([int]$rx[2] -shl 8) -bor ([int]$rx[3] -shl 16) -bor ([int]$rx[4] -shl 24)
                if ($rx.Count -lt 5 + $len) { break }
                $id = [int]$rx[0]
                if ($len -gt 0) {
                    $payload = $rx.GetRange(5, $len).ToArray()
                    if ($id -eq 1 -or $id -eq 2) {
                        $c = $dec.GetChars($payload, 0, $payload.Length, $chars, 0)
                        if ($c -gt 0) { [void]$sb.Append($chars, 0, $c) }
                    } elseif ($id -eq 6 -and -not $sent) {
                        # Ready: a terminal has a size, then the command, then
                        # the end of its input.
                        Send-AshPacketTo -Sock $sock -Id 5 -Payload ([Text.Encoding]::ASCII.GetBytes('24:80'))
                        Send-AshPacketTo -Sock $sock -Id 0 -Payload ([Text.Encoding]::ASCII.GetBytes($Command + "`n"))
                        Send-AshPacketTo -Sock $sock -Id 4 -Payload $null
                        $sent = $true
                    }
                }
                $rx.RemoveRange(0, 5 + $len)
            }
        }

        # The session is a terminal, and a terminal echoes what is typed: the
        # transcript starts with the prompt and the command itself. `ash -c`
        # hid that; a driver asserting on the answer should not have to know it.
        $text = $sb.ToString()
        $at = $text.IndexOf($Command, [StringComparison]::Ordinal)
        if ($at -ge 0) {
            $nl = $text.IndexOf("`n", $at)
            if ($nl -ge 0) { $text = $text.Substring($nl + 1) }
        }
        return $text
    } finally {
        try { $s.Stream.Close() } catch { }
        try { $sock.Close() } catch { }
    }
}

function Test-AshUp {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][int]$Port)
    try {
        $s = New-AshSocket -Path (Get-AshSockPath -Exe $Exe -Port $Port)
        $s.Close()
        return $true
    } catch {
        return $false
    }
}

# How long to wait for a core to publish its endpoint.
#
# Derived, not guessed: before it binds, the core reads and unpacks a 515-entry
# rootfs archive, and that has been measured at over six seconds from launch. A
# window that short does not read as a timeout when it expires - it reads as a
# flake, which is what it was.
$script:AshUpTimeoutMs = 30000

# Poll Test-AshUp until the endpoint answers, up to $TimeoutMs. $true only if it
# answered inside the window.
#
# This loop used to be spelled out in each driver with its own iteration count,
# and the six counts disagreed by 30x to 120x - two of them allowed 500 ms, an
# order of magnitude less than the work being waited for, and those two are the
# ones that flaked. One number, stated once, next to the reason for it.
#
# $Process, when given, ends the wait as soon as that process is gone: a core
# that has died will never publish, so a driver that is testing the death (see
# sshd_crash_e2e) should not sit out the whole window waiting for it. The probe
# outside the loop is why a core that binds in the final poll interval still
# counts as up instead of being lost to the clock.
function Wait-AshUp {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][int]$Port,
        [int]$TimeoutMs = $script:AshUpTimeoutMs,
        [Diagnostics.Process]$Process,
        [int]$PollMs = 200
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-AshUp -Exe $Exe -Port $Port) { return $true }
        if ($Process -and $Process.HasExited) { return $false }
        Start-Sleep -Milliseconds $PollMs
    }
    return (Test-AshUp -Exe $Exe -Port $Port)
}

# The pid of the live core registered on @Port, or $null when none owns it.
#
# The registry is written by the core itself, once it has won the port, and
# removed when it stops (runtime/cores/<port>.core - `--list` reads it). That
# makes it the host's own answer to "is the core on this port mine?": compare it
# with the pid of the process the driver started and the loop is closed. A port
# that merely *answers* says nothing - the endpoint is a filesystem socket, so a
# core left over from another run answers just as well, and every check a driver
# makes afterwards then describes a run it never set up.
#
# `--list` is used rather than reading the file, so the release layout stays
# where it already is: in the binary, the same reason `--sock-path` exists.
function Get-AshCorePid {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][int]$Port)
    $line = & $Exe --list 2>$null |
            Where-Object { $_ -match "^\s*up\s+port=$Port(\s|$)" } | Select-Object -First 1
    if ($line -and $line -match 'pid=(\d+)') { return [int]$Matches[1] }
    return $null
}

# Ask the core on @Port to stop and report how it ended: its exit code when it
# stopped on its own, $null when it had to be killed - a killed process has no
# exit status worth reading, and reporting the kill as a code would be a lie.
#
# There is no `--shutdown` verb any more (it belonged to the ash client, which
# went with the guest-side session server), so the request is what the docs say
# a core answers to: ending the process. It is killed, not asked, and the answer
# is therefore $null.
function Stop-AshCore {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [int]$TimeoutMs = 5000
    )
    try { $Process.Kill() } catch { }
    $Process.WaitForExit($TimeoutMs) | Out-Null
    return $null
}

# Stop every core left behind in this release tree, and say which ones.
#
# A leftover core is worse than no core: the endpoint is a filesystem socket, so a
# stale one still answers Test-AshUp and the run drives that core's session server
# instead of its own. Planted on purpose, this driver passed all 27 of its checks
# against a core it had not started, and reported PASS.
#
# The sweep is narrow on purpose, because it runs unconditionally:
#
#   - a core is a `--serve` process, which is what a client's `ash -c` is not;
#   - it must be *this* executable, so another release tree's core (its own
#     runtime, its own socket) and the other host binaries are left alone.
#
# Each core is asked to stop before it is killed, because a core that stops
# cleanly drops its endpoint, the rootfs lock and the sockets it forwarded for
# the guest. Returns one line per core, so a driver can print what it cleared
# instead of doing it silently.
function Stop-AshCores {
    param([Parameter(Mandatory)][string]$Exe, [int]$GraceMs = 2000)
    $exePath = (Resolve-Path -LiteralPath $Exe).Path
    $name    = [IO.Path]::GetFileName($exePath)
    # `(^|\s)--serve(\s|$)`, not `\b--serve\b`: a word boundary sits between a word
    # character and a non-word one, and both the space before the flag and the '-'
    # are non-word, so the boundaries never exist and the pattern matches nothing.
    $cores   = @(Get-CimInstance Win32_Process -Filter "Name='$name'" -EA SilentlyContinue |
                 Where-Object { $_.CommandLine -match '(^|\s)--serve(\s|$)' -and $_.ExecutablePath -eq $exePath })
    $stopped = @()
    foreach ($core in $cores) {
        $id   = $core.ProcessId
        $port = if ($core.CommandLine -match '--port\s+(\d+)') { [int]$Matches[1] } else { 0 }
        $how  = 'stopped'
        if ($port -gt 0) {
            $p = Get-Process -Id $id -EA SilentlyContinue
            if ($p) { Stop-AshCore -Exe $exePath -Port $port -Process $p -TimeoutMs $GraceMs | Out-Null }
        }
        for ($i = 0; $i -lt 50; $i++) {
            if (-not (Get-Process -Id $id -EA SilentlyContinue)) { break }
            Start-Sleep -Milliseconds 100
        }
        if (Get-Process -Id $id -EA SilentlyContinue) {
            try { Stop-Process -Id $id -Force -EA Stop; $how = 'killed' } catch { $how = 'would not stop' }
        }
        $stopped += "port $port (pid $id) $how"
    }
    # A core that was killed leaves its registry file behind. --list reads the
    # registry and reclaims a stale entry (a dead pid), so running it is what
    # clears the bookkeeping the next --serve would otherwise refuse the port on.
    & $exePath --list 2>$null | Out-Null
    return $stopped
}
