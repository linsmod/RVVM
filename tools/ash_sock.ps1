#Requires -Version 5.1
<#
    Shared AF_UNIX plumbing for the ash e2e drivers.

    The core no longer listens on a TCP port: vpsessiond binds a filesystem
    socket at /cores/vpsessiond-<port>.sock (a guest path the host maps into the
    run's rootfs), and `ash --sock-path` is the single source of truth for where
    that lands on the host. These helpers turn that path into a connected
    System.Net.Sockets.Socket / NetworkStream, so no driver re-derives the
    layout. Dot-source this file to use them.
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

# Stop every core left behind in this release tree, and say which ones.
#
# A leftover core is worse than no core. The endpoint is a filesystem socket
# rather than a port, so a stale one still answers Test-AshUp: the readiness
# probe below latches onto it and the whole run then drives somebody else's
# session server - a different run, with its own rootfs and nothing bounding how
# long it lives. That is not hypothetical. Two session_e2e -Multi runs failed
# their four shared-core checks that way, while the identical steps replayed by
# hand against a fresh core passed every time; the failure also misdescribed
# itself, since the file the second session looked for was never created at all
# rather than created empty.
#
# The sweep is narrow on purpose, because it runs unconditionally:
#
#   - a core is a `--serve` process, which is what a client's `ash -c` is not;
#   - it must be *this* executable, so another release tree's core (its own
#     runtime, its own socket) and the other host binaries are left alone.
#
# Each core is asked to stop before it is killed, because a core that stops
# cleanly drops its endpoint, the rootfs lock and the sockets it forwarded for
# the guest; only one that will not go is killed. Returns one line per core, so
# a driver can print what it cleared instead of doing it silently.
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
        if ($port -gt 0) {
            $ask = Start-Process -FilePath $exePath `
                -ArgumentList '--no-autostart', '--port', "$port", '--shutdown' `
                -WorkingDirectory ([IO.Path]::GetDirectoryName($exePath)) -PassThru -NoNewWindow `
                -RedirectStandardOutput (Join-Path $env:TEMP 'ash_stale_stop.txt') `
                -RedirectStandardError  (Join-Path $env:TEMP 'ash_stale_stop.err')
            $ask.WaitForExit($GraceMs) | Out-Null
            if (-not $ask.HasExited) { try { $ask.Kill() } catch { } }
        }
        for ($i = 0; $i -lt 50; $i++) {
            if (-not (Get-Process -Id $id -EA SilentlyContinue)) { break }
            Start-Sleep -Milliseconds 100
        }
        if (Get-Process -Id $id -EA SilentlyContinue) {
            try { Stop-Process -Id $id -Force -EA Stop; $how = 'killed' } catch { $how = 'would not stop' }
        } else {
            $how = 'stopped'
        }
        $stopped += "port $port (pid $id) $how"
    }
    # A core that was killed leaves its registry file behind. --list reads the
    # registry and reclaims a stale entry (a dead pid), so running it is what
    # clears the bookkeeping the next --serve would otherwise refuse the port on.
    & $exePath --list 2>$null | Out-Null
    return $stopped
}
