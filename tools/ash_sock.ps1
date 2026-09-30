#Requires -Version 5.1
<#
    Shared AF_UNIX plumbing for the ash e2e drivers.

    The core no longer listens on a TCP port: vpsessiond binds a filesystem
    socket at /run/vpsessiond/<port>.sock, a guest path the host mounts a
    directory of its own at, and `ash --sock-path` is the single source of truth
    for where that lands on the host - the core writes the same path into its
    registry file, so the client asks rather than derives. These helpers turn
    that path into a connected System.Net.Sockets.Socket / NetworkStream, so no
    driver re-derives the layout. Dot-source this file to use them.
#>

# The host endpoint for the core on @Port, as printed by `ash --sock-path`.
function Get-AshSockPath {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][int]$Port)
    return (& $Exe --sock-path --port $Port | Select-Object -First 1).Trim()
}

# A connected AF_UNIX stream socket to @Path (throws if nothing is listening).
function New-AshSocket {
    param([Parameter(Mandatory)][string]$Path)
    $s = [System.Net.Sockets.Socket]::new(
        [System.Net.Sockets.AddressFamily]::Unix,
        [System.Net.Sockets.SocketType]::Stream,
        [System.Net.Sockets.ProtocolType]::Unspecified)
    $s.Connect([System.Net.Sockets.UnixDomainSocketEndPoint]::new($Path))
    return $s
}

# A connected session: the raw Socket (for Poll) and a NetworkStream over it.
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

# Readiness probe: the endpoint answers right now. A stale socket file (crashed
# core) fails the connect and reads as down.
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
# The code is the core's own vocabulary (see src/virtpass/win32-host/ash_core.h):
# 0 for a clean stop, 64/65 for the two refusals that want different remedies. It
# never carries the guest program's status, which the core prints on its own
# stderr instead - separating the two is the whole point.
function Stop-AshCore {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [int]$TimeoutMs = 5000
    )
    $exePath = (Resolve-Path -LiteralPath $Exe).Path
    $ask = Start-Process -FilePath $exePath `
        -ArgumentList '--no-autostart', '--port', "$Port", '--shutdown' `
        -WorkingDirectory ([IO.Path]::GetDirectoryName($exePath)) -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $env:TEMP 'ash_stop_out.txt') `
        -RedirectStandardError  (Join-Path $env:TEMP 'ash_stop_err.txt')
    $ask.WaitForExit(3000) | Out-Null
    if (-not $ask.HasExited) { try { $ask.Kill() } catch { } }
    if ($Process.WaitForExit($TimeoutMs)) { return $Process.ExitCode }
    try { $Process.Kill() } catch { }
    $Process.WaitForExit(2000) | Out-Null
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
