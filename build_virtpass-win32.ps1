#Requires -Version 5.1
<#
.SYNOPSIS
    Build the RVVM virtpass Win32 host (rvvm_winhost_<arch>.exe).

.DESCRIPTION
    Thin wrapper around the repository Makefile: every compile/link rule lives
    in ./Makefile + ./project.mk, this script only locates the MinGW toolchain,
    drives mingw32-make and reports the produced executable.

    project.mk adds the `rvvm_winhost` bin target automatically on 64-bit
    Windows hosts (it implies USE_VERTPASS, so src/virtpass/vp_cmdpost.c is
    linked in). Sources: src/virtpass/win32-host/*.c + src/win/posix_shim.c.

.PARAMETER Target
    Make target to run. Defaults to `bin` (all Windows binaries, incl.
    rvvm_winhost). Pass an explicit file target such as
    `release.windows.x86_64\rvvm_winhost_x86_64.exe` to build only the host.

.PARAMETER Clean
    Run `make clean` before building.

.PARAMETER Jobs
    Parallel make jobs (default: logical processor count).

.PARAMETER RegenGlAbi
    Regenerate include/virtpass/vp_gl.h, src/virtpass/vp_gl_stub.c and the
    Win32 dispatch tables with tools/gen_gl_abi.py before building.

.PARAMETER MakeArgs
    Any extra arguments are passed verbatim to make (e.g. USE_RVJIT=0).

.EXAMPLE
    pwsh ./build_virtpass-win32.ps1

.EXAMPLE
    pwsh ./build_virtpass-win32.ps1 -Clean -Jobs 8

.EXAMPLE
    pwsh ./build_virtpass-win32.ps1 -RegenGlAbi
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Target = 'bin',
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$RegenGlAbi,

    # Anything not recognised as a named parameter is forwarded to make,
    # e.g. 'USE_RVJIT=0' or 'BUILD_TYPE=debug'
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MakeArgs
)

$ErrorActionPreference = 'Stop'

# --- Repo root: this script lives at the repository root ---
$RVVM_ROOT = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $RVVM_ROOT 'Makefile'))) {
    throw "Makefile not found in $RVVM_ROOT - keep this script at the repository root."
}

# --- Toolchain lookup: prefer PATH, then the usual MSYS2 locations ---
function Resolve-Make {
    foreach ($name in @('mingw32-make', 'make')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    foreach ($dir in @('C:\msys64\mingw64\bin', 'C:\msys64\usr\bin', 'D:\msys64\mingw64\bin')) {
        $exe = Join-Path $dir 'mingw32-make.exe'
        if (Test-Path -LiteralPath $exe) { return $exe }
    }
    return $null
}

$makeExe = Resolve-Make
if (-not $makeExe) {
    throw "mingw32-make not found. Install MSYS2 MinGW64 (https://www.msys2.org/) and add C:\msys64\mingw64\bin to PATH."
}

# mingw32-make sits next to gcc/ar in the MinGW64 toolchain - make sure that
# directory is on PATH so the compiler is found even if only make was.
$mingwBin = Split-Path -Parent $makeExe
if (($env:PATH -split ';') -notcontains $mingwBin) {
    $env:PATH = "$mingwBin;$env:PATH"
}
if (-not (Get-Command cc -ErrorAction SilentlyContinue) -and -not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Warning "Neither 'cc' nor 'gcc' found on PATH - the build will fail. Add $mingwBin to PATH."
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Jobs -lt 1) { $Jobs = 1 }

Write-Host "RVVM root : $RVVM_ROOT" -ForegroundColor Cyan
Write-Host "make      : $makeExe (-j$Jobs)" -ForegroundColor Cyan
Write-Host "target    : $Target" -ForegroundColor Cyan

Push-Location -LiteralPath $RVVM_ROOT
$exitCode = 0
try {
    if ($RegenGlAbi) {
        Write-Host "`n==> python tools/gen_gl_abi.py" -ForegroundColor Cyan
        & python (Join-Path $RVVM_ROOT 'tools\gen_gl_abi.py')
        if ($LASTEXITCODE -ne 0) { throw "gen_gl_abi.py failed (exit $LASTEXITCODE) - is Python 3 on PATH?" }
    }

    if ($Clean) {
        Write-Host "`n==> $makeExe clean" -ForegroundColor Cyan
        & $makeExe clean
        if ($LASTEXITCODE -ne 0) { throw "make clean failed (exit $LASTEXITCODE)" }
    }

    $makeCmd = @("-j$Jobs", $Target)
    if ($MakeArgs) { $makeCmd += $MakeArgs }

    Write-Host "`n==> $makeExe $($makeCmd -join ' ')" -ForegroundColor Cyan
    & $makeExe @makeCmd
    $exitCode = $LASTEXITCODE
} catch {
    Write-Host $_ -ForegroundColor Red
    $exitCode = 1
} finally {
    Pop-Location
}

if ($exitCode -ne 0) {
    Write-Host "`nWin32 virtpass host build FAILED (exit $exitCode)" -ForegroundColor Red
    exit $exitCode
}

Write-Host "`nWin32 virtpass host build complete." -ForegroundColor Green
exit 0
