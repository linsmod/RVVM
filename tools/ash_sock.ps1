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
