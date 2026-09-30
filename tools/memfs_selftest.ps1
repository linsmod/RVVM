#Requires -Version 5.1
<#
.SYNOPSIS
    rvvm_memfs's own checks, compiled and run on the host.

.DESCRIPTION
    The memory filesystem is a data structure with no guest involved: nodes in
    an array, a path -> node map, and a free list for the holes removal leaves.
    Everything it can get wrong, it gets wrong as a *wrong answer* rather than a
    crash - a file that is in its parent's listing and answers ENOENT, a rename
    that succeeds and loses the subtree, a tmpfs that accepts a write it was told
    it would refuse. None of that is reachable from a guest-level test until the
    module is wired into the syscall layer, and by then a bug in here has been
    buried under fifteen other files.

    So this builds rvvm_memfs.c together with tools/memfs_selftest.c and runs it
    against the module directly. The C file is the test; this is the build and the
    verdict, so that it needs no toolchain knowledge of its own to run.

    Two things the harness is deliberate about:

      - USE_THREAD_EMU. That makes rvvm's lock a no-op, which is right here (one
        thread, and pulling in locking.c drags in futex, the scheduler and the
        timer just to link a lock that is never contended) and it means the
        module is exercised with the same code paths it uses in the core, minus
        the atomic.
      - The consistency check after every stage. rvvm_memfs_selftest_consistency()
        walks the map against the node array and names the first disagreement, so
        a failure says WHICH invariant broke and where, instead of leaving a pile
        of ENOENTs to be reasoned backwards from. Every one of the five real bugs
        this file was written against presented first as a run of ENOENTs from
        lookups that had nothing obviously wrong with them.

.PARAMETER Cc
    The C compiler. Defaults to gcc on PATH.

.PARAMETER Sanitize
    Build with -fsanitize=address,undefined when the toolchain has the runtimes.
    Turn it off for a toolchain that does not ship them (MinGW's gcc does not).

.EXAMPLE
    pwsh ./tools/memfs_selftest.ps1
#>
param(
    [string]$Cc = 'gcc',
    [switch]$NoSanitize
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$src  = Join-Path $repo 'src\core\rvvm_memfs.c'
$test = Join-Path $PSScriptRoot 'memfs_selftest.c'
$exe  = Join-Path $env:TEMP 'rvvm_memfs_selftest.exe'

if (-not (Get-Command $Cc -ErrorAction SilentlyContinue)) {
    "SKIP: no '$Cc' on PATH; rvvm_memfs's own checks did not run"
    exit 0
}

# The sanitizers are the difference between "the checks passed" and "the checks
# passed and nothing was freed twice". A toolchain without the runtimes is a
# normal thing to have - MinGW's gcc has no libasan - so their absence is
# reported rather than treated as a failure, and it is reported out loud because
# a green run without them is a weaker claim than a green run with them.
$flags = @('-std=c99', '-g', '-DUSE_THREAD_EMU', '-Wall', '-Wextra',
           '-Wno-unused-parameter', '-I', (Join-Path $repo 'src'),
           '-I', (Join-Path $repo 'include'))
$sanitize = $false
if (-not $NoSanitize) {
    $probe = & $Cc '-fsanitize=address,undefined' '-x' 'c' '-o' "$env:TEMP\rvvm_san_probe.exe" '-' 2>&1
    if ($LASTEXITCODE -eq 0) {
        $flags += '-fsanitize=address,undefined'
        $sanitize = $true
        Remove-Item "$env:TEMP\rvvm_san_probe.exe" -ErrorAction SilentlyContinue
    }
}

"building rvvm_memfs_selftest$(if ($sanitize) { ' (asan+ubsan)' })"
& $Cc @flags -o $exe $test $src
if ($LASTEXITCODE -ne 0) {
    "FAIL: the selftest did not build"
    exit 1
}
if (-not $sanitize) {
    "note: no sanitizer runtimes for this toolchain, so a clean run is a weaker claim"
}

& $exe
$rc = $LASTEXITCODE
Remove-Item $exe -ErrorAction SilentlyContinue
if ($rc -ne 0) {
    "FAIL: rvvm_memfs's own checks failed (exit $rc)"
    exit 1
}
"=== PASS: rvvm_memfs ==="
exit 0
