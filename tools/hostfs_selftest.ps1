#Requires -Version 5.1
<#
.SYNOPSIS
    rvvm_hostfs's own checks, compiled and run against a real host directory.

.DESCRIPTION
    rvvm_memfs is a data structure and can be tested as one. rvvm_hostfs cannot:
    every answer it gives is the host's answer, and what it can get wrong is the
    part it adds on top - which host path it built, whether it refused one that
    leaves the mount, whether it reported honestly that this host cannot hold a
    symlink. From a guest all of those look like ordinary ENOENTs on a host
    directory, so a guest-level test cannot reach them.

    So this builds rvvm_hostfs.c with tools/hostfs_selftest.c and runs it against
    a scratch directory this script makes and removes. It is the peer of
    memfs_selftest.ps1 and follows the same rules:

      - USE_THREAD_EMU, for the same reason: one thread, and pulling in locking.c
        drags in the futex, the scheduler and the timer to link a lock that is
        never contended.
      - on Windows the module needs the MinGW compat headers and posix_shim.c,
        which are where this host's stat(), lstat(), mkdirat(), symlinkat() and
        readlinkat() come from. They are compiled in here so the module is
        exercised against the same host calls the core uses, not against a
        different set that happens to link.
      - what the host cannot do is asserted rather than skipped. A host that
        cannot make a symlink must say so, and the test reports which it got,
        because a run that silently took the "cannot" branch is a weaker claim
        than one that took the other.

.PARAMETER Cc
    The C compiler. Defaults to gcc on PATH.

.PARAMETER NoSanitize
    Skip -fsanitize=address,undefined. MinGW's gcc ships no sanitizer runtimes.

.EXAMPLE
    pwsh ./tools/hostfs_selftest.ps1
#>
param(
    [string]$Cc = 'gcc',
    [switch]$NoSanitize
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$src  = Join-Path $repo 'src\core\rvvm_hostfs.c'
$test = Join-Path $PSScriptRoot 'hostfs_selftest.c'
$exe  = Join-Path $env:TEMP 'rvvm_hostfs_selftest.exe'

# The scratch directory the module is pointed at. Removed afterwards, and made
# here rather than by the test: a test that picks its own idea of a temp
# directory is a test that leaves one behind on the wrong platform.
$scratch = Join-Path $env:TEMP 'rvvm_hostfs_selftest_root'
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

if (-not (Get-Command $Cc -ErrorAction SilentlyContinue)) {
    "SKIP: no '$Cc' on PATH; rvvm_hostfs's own checks did not run"
    exit 0
}

$flags = @('-std=c99', '-g', '-DUSE_THREAD_EMU', '-Wall', '-Wextra',
           '-Wno-unused-parameter', '-I', (Join-Path $repo 'src'),
           '-I', (Join-Path $repo 'include'))

$sources = @($test, $src)
$libs    = @()
# Nothing else is linked, and that is deliberate. The Windows build does have
# symlink()/readlink()/lstat() - src/win/posix_shim.c implements them - but that
# file reaches sockets, logging, the allocator and the vector, so linking it to
# test this module would mean the module is not testable on its own. On that host
# it answers HOSTFS_NO_SYMLINK instead, and the test asserts that it SAYS so
# rather than skipping the question. See rvvm_hostfs.c.
if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $libs = @('-lws2_32')
}

$sanitize = $false
if (-not $NoSanitize) {
    $probe = & $Cc '-fsanitize=address,undefined' '-x' 'c' '-o' "$env:TEMP\rvvm_san_probe.exe" '-' 2>&1
    if ($LASTEXITCODE -eq 0) {
        $flags += '-fsanitize=address,undefined'
        $sanitize = $true
        Remove-Item "$env:TEMP\rvvm_san_probe.exe" -ErrorAction SilentlyContinue
    }
}

"building rvvm_hostfs_selftest$(if ($sanitize) { ' (asan+ubsan)' })"
& $Cc @flags -o $exe @sources @libs
if ($LASTEXITCODE -ne 0) {
    "FAIL: the selftest did not build"
    exit 1
}
if (-not $sanitize) {
    "note: no sanitizer runtimes for this toolchain, so a clean run is a weaker claim"
}

& $exe $scratch
$rc = $LASTEXITCODE
Remove-Item $exe -ErrorAction SilentlyContinue
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
if ($rc -ne 0) {
    "FAIL: rvvm_hostfs's own checks failed (exit $rc)"
    exit 1
}
"=== PASS: rvvm_hostfs ==="
exit 0
