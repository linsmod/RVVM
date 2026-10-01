#Requires -Version 5.1
<#
.SYNOPSIS
    rvvm_fs's own checks: one set of operations, asked of two filesystems.

.DESCRIPTION
    rvvm_memfs and rvvm_hostfs are nothing alike - bytes in the core, bytes in a
    host directory - and the claim rvvm_fs makes is that a caller cannot tell.
    This builds rvvm_fs.c with both providers and tools/fs_selftest.c and runs one
    set of assertions twice, through rvvm_fs_* and never through a provider's own
    entry points.

    It is not a retelling of the providers' own tests. Those ask whether memfs and
    hostfs are correct; this asks whether they are INTERCHANGEABLE, which is a
    different claim and the one the syscall layer will depend on: a slot that only
    works for one of them is a shape the table got wrong.

    It links no platform shim. Both providers are self-contained by design -
    hostfs answers for symlinks itself rather than reaching the Windows shim,
    which drags in sockets, logging and the allocator - so this is four C files
    and nothing else.

.PARAMETER Cc
    The C compiler. Defaults to gcc on PATH.

.PARAMETER NoSanitize
    Skip -fsanitize=address,undefined. MinGW's gcc ships no sanitizer runtimes.

.EXAMPLE
    pwsh ./tools/fs_selftest.ps1
#>
param(
    [string]$Cc = 'gcc',
    [switch]$NoSanitize
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$test = Join-Path $PSScriptRoot 'fs_selftest.c'
$exe  = Join-Path $env:TEMP 'rvvm_fs_selftest.exe'

# The scratch directory the host provider is pointed at. Made and removed here, not
# by the test.
$scratch = Join-Path $env:TEMP 'rvvm_fs_selftest_root'
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

if (-not (Get-Command $Cc -ErrorAction SilentlyContinue)) {
    "SKIP: no '$Cc' on PATH; rvvm_fs's own checks did not run"
    exit 0
}

$sources = @(
    $test,
    (Join-Path $repo 'src\core\rvvm_fs.c'),
    (Join-Path $repo 'src\core\rvvm_memfs.c'),
    (Join-Path $repo 'src\core\rvvm_hostfs.c')
)
$flags = @('-std=c99', '-g', '-DUSE_THREAD_EMU', '-Wall', '-Wextra',
           '-Wno-unused-parameter', '-I', (Join-Path $repo 'src'),
           '-I', (Join-Path $repo 'include'))
$libs = @()

$sanitize = $false
if (-not $NoSanitize) {
    $probe = & $Cc '-fsanitize=address,undefined' '-x' 'c' '-o' "$env:TEMP\rvvm_san_probe.exe" '-' 2>&1
    if ($LASTEXITCODE -eq 0) {
        $flags += '-fsanitize=address,undefined'
        $sanitize = $true
        Remove-Item "$env:TEMP\rvvm_san_probe.exe" -ErrorAction SilentlyContinue
    }
}

"building rvvm_fs_selftest$(if ($sanitize) { ' (asan+ubsan)' })"
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
    "FAIL: rvvm_fs's own checks failed (exit $rc)"
    exit 1
}
"=== PASS: rvvm_fs ==="
exit 0
