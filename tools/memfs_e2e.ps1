#Requires -Version 5.1
<#
.SYNOPSIS
    Memory filesystem end-to-end: a tmpfs mount that actually serves bytes.

.DESCRIPTION
    mount(2) accepts tmpfs and /proc/mounts reports it; what this driver adds is
    that the mount point ANSWERS. Until the memory filesystem was wired to the
    syscall layer, a tmpfs mount held the host's directory of the same name and a
    mount was a table row nothing resolved. The checks below open, write, read,
    list, truncate and re-mount - every one of them would have failed against a
    row that only describes a filesystem.

    A tmpfs is also private and dies with its mount, so a file written before an
    umount is gone after a re-mount. That is the property a host directory cannot
    have, and the reason the storage is the core's own.

.PARAMETER Port
    Port the core listens on (default 7932, off the guest default 7900 and off
    mount_e2e's 7931).

.PARAMETER Exe
    The host binary. Defaults to the build tree's rvvm_ash.

.EXAMPLE
    pwsh ./tools/memfs_e2e.ps1
#>
param(
    [int]$Port = 7932,
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

# The command's own exit status, not the client's - every error case here is about
# the command's.
function ClientRc([string]$cmd) {
    $r = Client "$cmd`necho __rc=`$?"
    if ($r -match '__rc=(-?\d+)') { [int]$Matches[1] } else { -1 }
}

$out = Join-Path $env:TEMP 'memfs_e2e_core_out.txt'
$err = Join-Path $env:TEMP 'memfs_e2e_core_err.txt'
Remove-Item $out, $err -ErrorAction SilentlyContinue

# A stale core from an earlier run would answer for us: make sure ours is the one.
Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$core = Start-Process -FilePath $exe -ArgumentList '--serve', '--port', "$Port" `
                      -WorkingDirectory $dir -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow

$up = Wait-AshUp -Exe $exe -Port $Port -Process $core
Check $up "the core (ash --serve) publishes its endpoint (port $Port)"
if (-not $up) {
    try { $core.Kill() } catch { }
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}

try {
# --- a tmpfs that serves bytes ----------------------------------------------
$rc = ClientRc 'mount -t tmpfs none /mnt'
Check ($rc -eq 0) "mount -t tmpfs none /mnt succeeds (rc $rc)"

$rc = ClientRc 'echo hello > /mnt/f'
Check ($rc -eq 0) "write a new file through the mount (rc $rc)"

$r = Client 'cat /mnt/f'
Check ($r -match 'hello') "read it back"

$r = Client 'stat -c %s /mnt/f'
Check ($r.Trim() -eq '6') "stat reports the six bytes the write left ('$($r.Trim())')"

$r = Client 'ls /mnt'
Check ($r -match '(?m)^f\r?$') "the directory lists the file"

# Append goes to the end, through the same descriptor.
$rc = ClientRc 'echo world >> /mnt/f'
Check ($rc -eq 0) "append to it (rc $rc)"
$r = Client 'cat /mnt/f'
Check (($r -match 'hello') -and ($r -match 'world')) "both lines are there"

# A larger round trip, so cursor arithmetic is exercised rather than one short
# line: 8 KiB in, exactly 8 KiB out.
$rc = ClientRc 'dd if=/dev/zero of=/mnt/big bs=1024 count=8 2>/dev/null'
Check ($rc -eq 0) "write 8 KiB (rc $rc)"
$r = Client 'stat -c %s /mnt/big'
Check ($r.Trim() -eq '8192') "8 KiB is exactly 8192 bytes ('$($r.Trim())')"
$r = Client 'wc -c < /mnt/big'
Check ($r.Trim() -eq '8192') "and reads back as 8192 bytes ('$($r.Trim())')"

# O_TRUNC is the one write that shrinks, through the shell's own `: >`.
$rc = ClientRc ': > /mnt/f'
Check ($rc -eq 0) "truncate through O_TRUNC (rc $rc)"
$r = Client 'stat -c %s /mnt/f'
Check ($r.Trim() -eq '0') "the file is zero bytes now ('$($r.Trim())')"

# The storage answers the errors itself, not a host directory's.
$r = Client 'cat /mnt/nope/x'
Check ($r -match 'No such file') "a file under a missing directory is ENOENT"
$r = Client 'cat /mnt'
Check ($r -match 'Is a directory') "reading a directory is EISDIR"
# A component that is a file. The storage hashes a whole path and so answers
# ENOENT where Linux would answer ENOTDIR - a fidelity gap, pinned here so the
# day it changes is deliberate rather than a surprise. The module's own
# name-changing calls do tell the two apart (memfs_parent_locked); its lookups do
# not.
$r = Client 'cat /mnt/big/also'
Check ($r -match 'No such file') "a component that is a file answers ENOENT (Linux would say ENOTDIR)"

# --- private, and it dies with the mount ------------------------------------
$r = Client 'ls /mnt'
Check ($r -match '(?m)\bf\b') "the file is there before the unmount"
$rc = ClientRc 'umount /mnt'
Check ($rc -eq 0) "umount /mnt succeeds (rc $rc)"
$rc = ClientRc 'mount -t tmpfs none /mnt'
Check ($rc -eq 0) "re-mount tmpfs on /mnt (rc $rc)"
$r = Client 'ls /mnt'
Check ($r -notmatch '(?m)^f\r?$') "the file is gone after a re-mount: the storage died with the mount"

} finally {
    # Leave the namespace as we found it, even when a check threw.
    Client 'umount /mnt' | Out-Null
    Get-Process -Name 'rvvm_ash_x86_64' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($fails) {
    "=== FAIL: $fails check(s) ==="
    "--- core output ---"; Get-Content $out -ErrorAction SilentlyContinue
    "--- core stderr ---"; Get-Content $err -ErrorAction SilentlyContinue
    exit 1
}
"=== PASS: memfs ==="
exit 0
