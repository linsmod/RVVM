#Requires -Version 5.1
<#
.SYNOPSIS
    procfs end-to-end: `ps`, `top` and /proc on the Win32 userland host.

.DESCRIPTION
    The guest runs on a host directory tree with no kernel behind it, so /proc
    used to be empty and `ps` printed only its header. `rvvm_user.c`'s procfs
    section now synthesizes it from the guest process registry. This driver
    starts one `ash --serve` core, runs commands through `-c` sessions and
    asserts on the guest's own output:

      listing     /proc lists the root files and the live pids, and /proc/self
                  exists.
      per-pid     /proc/<pid>/{stat,status,statm,cmdline,comm} read back, and
                  /proc/self resolves to a pid.
      links       /proc/self, /proc/self/cwd and /proc/self/fd/<n> readlink.
      ps          `ps` shows real rows (pid, user, argv), including a running
                  background job.
      system      /proc/{uptime,stat,meminfo,version} answer sane content.

.PARAMETER Port
    Port the core listens on (default 7912, off the guest default 7900).

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/procfs_e2e.ps1
#>
param(
    [int]$Port = 7912,
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path
$dir = Split-Path $exe
$fails = 0
. (Join-Path $PSScriptRoot 'ash_sock.ps1')

function TS() { "[" + ([Environment]::TickCount64).ToString().PadLeft(9) + " ms] " }
function Check($ok, $what) {
    if ($ok) { "$(TS)ok   $what" } else { "$(TS)FAIL $what"; $script:fails++ }
}

# One client run: attach to the core, ask for the command, return its output with
# the terminal escapes stripped.
function Client([string]$cmd) {
    $r = & $exe --port $Port -c $cmd 2>$null
    ($r -join "`n") -replace "`e\[[0-9;?]*[A-Za-z]", ''
}

$out = Join-Path $env:TEMP 'procfs_e2e_core_out.txt'
$err = Join-Path $env:TEMP 'procfs_e2e_core_err.txt'
Remove-Item $out, $err -ErrorAction SilentlyContinue

# A stale core from an earlier run would answer for us: make sure ours is the one.
Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--port', "$Port" `
                      -WorkingDirectory $dir -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow

$up = $false
for ($i = 0; $i -lt 120; $i++) {
    if (Test-AshUp -Exe $exe -Port $Port) { $up = $true; break }
    Start-Sleep -Milliseconds 100
}
Check $up "the core (ash --serve) publishes its endpoint (port $Port)"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}

try {
# --- the /proc listing ----------------------------------------------------
$r = Client 'ls /proc'
Check ($r -match '(?m)^.*\bself\b' -and $r -match '\buptime\b' -and $r -match '\bstat\b' -and $r -match '\bmounts\b') `
      "/proc lists self, uptime, stat and mounts"
Check ($r -match '(?m)(^|\s)\d{2,}(\s|$)') "/proc lists live pids"
Check ($r -match '\bthread-self\b') "/proc lists thread-self"

# --- per-pid files --------------------------------------------------------
$r = Client 'cat /proc/self/stat'
Check ($r -match '^\s*\d+ \([^)]+\) [RSDTZ] ') "/proc/self/stat has pid (comm) state"

$r = Client 'cat /proc/self/status'
Check ($r -match 'Uid:\s*0\s+0\s+0\s+0' -and $r -match 'Pid:\s*\d+') "/proc/self/status has Uid/Pid"

$r = Client 'cat /proc/self/cmdline'
Check ($r.Length -gt 0) "/proc/self/cmdline is not empty"

$r = Client 'cat /proc/self/comm'
Check ($r -match '\S') "/proc/self/comm names the program"

# --- links ----------------------------------------------------------------
$r = Client 'readlink /proc/self'
Check ($r -match '^\s*\d+\s*$') "/proc/self readlinks to a pid"

$r = Client 'readlink /proc/self/cwd'
Check ($r -match '^\s*/\s*$') "/proc/self/cwd readlinks to /"

$r = Client 'ls -l /proc/self/fd'
Check ($r -match '/dev/pts/\d+') "/proc/self/fd names the session terminal"

# --- ps -------------------------------------------------------------------
$r = Client 'ps'
Check ($r -match 'PID\s+USER\s+TIME\s+COMMAND') "ps prints the busybox header"
Check ($r -match '(?m)^\s*\d+\s+root\s+') "ps shows at least one process row"
Check ($r -match 'vpsessiond') "ps shows the run root (vpsessiond)"

$r = Client 'sleep 30 & ps'
Check ($r -match 'sleep 30') "ps shows a running background job"

# --- system-wide files ----------------------------------------------------
$r = Client 'cat /proc/uptime'
Check ($r -match '\d+\.\d+\s+\d+\.\d+') "/proc/uptime is two numbers"

$r = Client 'cat /proc/stat'
Check ($r -match '(?m)^cpu ' -and $r -match 'processes') "/proc/stat has cpu and processes"

$r = Client 'cat /proc/meminfo'
Check ($r -match 'MemTotal:') "/proc/meminfo has MemTotal"
$mt = if ($r -match 'MemTotal:\s+(\d+) kB') { [int64]$Matches[1] } else { 0 }
$mf = if ($r -match 'MemFree:\s+(\d+) kB') { [int64]$Matches[1] } else { 0 }
Check ($mt -gt 900000) "MemTotal reflects the ~1 GiB guest address space ($mt kB)"
Check ($mf -gt 0 -and $mf -lt $mt) "MemFree is the unallocated address space ($mf kB)"

# `free` reads sysinfo() (total/free) while `top` reads meminfo: both must agree.
$r = Client 'free'
$ft = if ($r -match 'Mem:\s+(\d+)') { [int64]$Matches[1] } else { 0 }
Check ($ft -eq $mt) "free (sysinfo) and meminfo report the same total ($ft kB)"

$r = Client 'cat /proc/version'
Check ($r -match 'Linux version') "/proc/version names the kernel"

} finally {
    # Cleanup, even when a check threw - a lingering core holds the rootfs lock.
    Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}
"=== PASS: procfs ==="
exit 0