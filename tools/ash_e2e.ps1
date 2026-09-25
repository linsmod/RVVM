#Requires -Version 5.1
<#
.SYNOPSIS
    ash core/client end-to-end: one `ash --serve` (a run), several `ash` clients.

.DESCRIPTION
    The WSL shape this project aims at: "--serve is the run boundary, a client is
    a session boundary."

    A core is started in the background (`ash --serve`), which boots the session
    server (vpsessiond) as the run root. Clients then attach over the socket and
    get a session of their own. Every assertion is the guest's own output:

      command    a client runs a command in the core and reads its output back.
      sessions   two clients are two shells (different pids), not one.
      shared     a file written in one session is readable in another - the core
                 is the state they share.
      survives   the core outlives a client that leaves.
      autostart  with no core running, a client starts one (the "wsl" behaviour)
                 and connects to it.

    Exit codes are not carried by the session protocol yet (the server closes the
    socket when the shell is gone, and that is all a client sees); `-c` therefore
    answers the core's exit code, which is 0 once the session ran.

.PARAMETER Port
    Port the core listens on (default 7911, off the guest default 7900).

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/ash_e2e.ps1
#>
param(
    [int]$Port = 7911,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$dir = Split-Path $exe
$fails = 0

function TS() { "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] " }
function Check($ok, $what) {
    if ($ok) { "$(TS)ok   $what" } else { "$(TS)FAIL $what"; $script:fails++ }
}

# One client run: attach to the core, ask for the command, return its output.
function Client([string]$cmd) {
    $r = & $exe --port $Port -c $cmd 2>$null
    ($r -join "`n") -replace "`e\[[0-9;?]*[A-Za-z]", ''
}

$out = Join-Path $env:TEMP 'ash_e2e_core_out.txt'
$err = Join-Path $env:TEMP 'ash_e2e_core_err.txt'
Remove-Item $out, $err -ErrorAction SilentlyContinue

$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--port', "$Port" `
                      -WorkingDirectory $dir -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow

# Ready when the port answers, not when a log line shows up.
$up = $false
for ($i = 0; $i -lt 120; $i++) {
    try {
        $probe = New-Object System.Net.Sockets.TcpClient
        $probe.Connect('127.0.0.1', $Port)
        $probe.Close()
        $up = $true
        break
    } catch { Start-Sleep -Milliseconds 100 }
}
Check $up "the core (ash --serve) listens on 127.0.0.1:$Port"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}

# --- one client, one session ---------------------------------------------
$r = Client 'echo ash-hello-42'
Check ($r -match 'ash-hello-42') 'a client runs a command in the core'

$r = Client 'id -u'
Check ($r -match '0') 'the session is a real guest shell (root, as the run root)'

# --- two clients are two sessions ----------------------------------------
$p1 = Client 'echo pid=$$'
$p2 = Client 'echo pid=$$'
$pid1 = if ($p1 -match 'pid=(\d+)') { $Matches[1] } else { '' }
$pid2 = if ($p2 -match 'pid=(\d+)') { $Matches[1] } else { '' }
Check ($pid1 -ne '' -and $pid2 -ne '' -and $pid1 -ne $pid2) `
      "two clients are two shells (pid $pid1 vs $pid2), not one run per client"

# --- one core, one shared filesystem -------------------------------------
Client 'echo shared-core-state > /tmp/ash-e2e.txt' | Out-Null
$r = Client 'cat /tmp/ash-e2e.txt'
Check ($r -match 'shared-core-state') "a session reads what another session wrote (the core's fs)"
Client 'rm -f /tmp/ash-e2e.txt' | Out-Null

# --- the core outlives a client ------------------------------------------
$r = Client 'echo core-still-alive'
Check ($r -match 'core-still-alive') 'the core stays up after a client leaves'

# --- autostart: no core, a client starts one -----------------------------
Stop-Process -Id $core.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
$r = Client 'echo autostarted'
Check ($r -match 'autostarted') 'with no core running, a client starts one (wsl autostart)'

# Cleanup: kill whatever core is listening now (the autostarted one included).
Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}
"=== PASS: ash core/client ==="
exit 0