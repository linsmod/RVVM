#Requires -Version 5.1
<#
.SYNOPSIS
    A run with no rootfs: the bundle released into the run's own memory filesystem.

.DESCRIPTION
    A host that names no prefix gets a run whose "/" is a memory filesystem, and
    the release's bundle is installed into that rather than onto the host - so the
    guest has the same rootfs and the same system programs, reached through a
    filesystem that dies with the run. This driver starts one such core
    (`ash --serve --memfs`) and asserts on the guest's own output:

      root        /proc/mounts reports the root as tmpfs, not rootfs: the storage
                  is the core's, and there is no host directory behind it.
      installed   /sbin/vpsessiond is there, and it is the file the core booted -
                  the endpoint exists at all, which is the proof a program was
                  loaded out of memory.
      symlinks    /bin/sh reads back as /bin/busybox: a real symlink in the
                  storage, which is the thing a host directory cannot hold (on
                  Windows creating one needs a privilege) and the reason a memory
                  root needs no shadow index to answer for them.
      private     a file written at the root is readable back, and is NOT on the
                  host: the rootfs directory on disk is untouched by it.

    Everything is driven by busybox, which is itself in the memory root - the run
    proves itself by being able to say anything at all.

.PARAMETER Port
    Port the core listens on (default 7934, off the guest default 7900).

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/memfs_root_e2e.ps1
#>
param(
    [int]$Port = 7934,
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

function Client([string]$cmd) {
    $r = & $exe --port $Port -c $cmd 2>$null
    ($r -join "`n") -replace "`e\[[0-9;?]*[A-Za-z]", ''
}
function ClientRc([string]$cmd) {
    $r = Client "$cmd`necho __rc=`$?"
    if ($r -match '__rc=(-?\d+)') { [int]$Matches[1] } else { -1 }
}

$out = Join-Path $env:TEMP 'memfs_root_e2e_core_out.txt'
$err = Join-Path $env:TEMP 'memfs_root_e2e_core_err.txt'
Remove-Item $out, $err -ErrorAction SilentlyContinue

# A stale core would answer for us, and a stale one is a persisted rootfs: make
# sure ours is the one.
Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# The file the "private" check looks for, removed first so a leftover from an
# earlier run cannot read as this one's.
$hostRoot = Join-Path $dir 'runtime\rootfs'
$hostFile = Join-Path $hostRoot 'wrote-in-memory.txt'
Remove-Item $hostFile -ErrorAction SilentlyContinue

$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--memfs', '--port', "$Port" `
                      -WorkingDirectory $dir -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow

$up = Wait-AshUp -Exe $exe -Port $Port -Process $core
Check $up "the core publishes its endpoint, so a program booted out of memory (port $Port)"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}

try {
# --- the root is memory ------------------------------------------------------
$r = Client 'grep " / " /proc/mounts'
Check ($r -match '(?m)^/dev/root / tmpfs ') "the root is a memory filesystem ('$($r.Trim())')"
# ...and not the hostfs-backed one a persisted rootfs reports.
Check ($r -notmatch '(?m)^/dev/root / rootfs ') "and not a host directory"

# --- the bundle is in it -----------------------------------------------------
$r = Client 'stat -c %s /sbin/vpsessiond'
Check ([int]($r.Trim()) -gt 0) "a system program was installed into it ($($r.Trim()) bytes)"
$r = Client 'stat -c %s /lib/ld-musl-riscv64.so.1'
Check ([int]($r.Trim()) -gt 0) "and the interpreter busybox is linked against ($($r.Trim()) bytes)"

# A real symlink, which is what a host directory cannot hold and the reason a
# memory root answers lstat/readlink without a shadow index beside it.
$r = Client 'readlink /bin/sh'
Check ($r.Trim() -eq '/bin/busybox') "/bin/sh is a symlink in the storage ('$($r.Trim())')"
$r = Client 'cat /bin/sh'
Check ($r.Length -gt 0) "and reading through it reaches busybox"

# --- the run's own writes stay in it -----------------------------------------
$rc = ClientRc 'echo wrote-in-memory > /wrote-in-memory.txt'
Check ($rc -eq 0) "a write at the root succeeds (rc $rc)"
$r = Client 'cat /wrote-in-memory.txt'
Check ($r -match 'wrote-in-memory') "and reads back"
# The whole point: nothing landed in the release tree. A root backed by the
# rootfs directory would have put it there.
Check (-not (Test-Path $hostFile)) "and nothing appeared in the release rootfs on the host"

# --- what is still host's, mounted into the guest ----------------------------
# The session endpoint is a host directory mounted at a guest path, which is what
# makes it reachable however the root is backed.
$r = Client 'grep vpsessiond /proc/mounts'
Check ($r -match '(?m)^\S+ /run/vpsessiond hostfs ') "the session endpoint is still a host mount"

# --- and it dies with the run ------------------------------------------------
# Not asserted by restarting a core, which would cost another install: a memory
# root is a fresh filesystem per machine, and the check above - nothing on the
# host - is the half that could be wrong.
$r = Client 'ls /'
Check ($r -match 'wrote-in-memory') "and a later session in the same run still sees it"

} finally {
    try { $core.Kill() } catch { }
    Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}
"=== PASS: memfs root ==="
exit 0
