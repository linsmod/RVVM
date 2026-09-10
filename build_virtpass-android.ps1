#Requires -Version 5.1
<#
.SYNOPSIS
    Build the RVVM virtpass Android side (guest ELFs and/or the APK).

.DESCRIPTION
    Thin wrapper around the repository Makefile: the guest cross-compilation
    and the Gradle cascade are declared in ./Makefile + ./project.mk, this
    script only checks the toolchain, drives mingw32-make and reports the
    produced artifacts.

    Targets map onto the Makefile rules:
      apk    -> android        (guest assets + librvvm_jni.so + APK)
      assets -> android-assets (zig/musl riscv64 guest ELFs into APK assets)
      jni    -> android-jni    (librvvm_jni.so only)
      clean  -> android-clean  (Gradle clean)

.PARAMETER Target
    What to build: apk (default), assets, jni or clean.

.PARAMETER Variant
    Gradle build variant: debug (default) or release.

.PARAMETER Clean
    Run `android-clean` before building.

.PARAMETER Jobs
    Parallel make jobs (default: logical processor count).

.PARAMETER RegenGlAbi
    Regenerate include/virtpass/vp_gl.h + src/virtpass/vp_gl_stub.c with
    tools/gen_gl_abi.py before building.

.PARAMETER MakeArgs
    Any extra arguments are passed verbatim to make (e.g. ANDROID_GRADLE_OPTS=...).

.EXAMPLE
    pwsh ./build_virtpass-android.ps1

.EXAMPLE
    pwsh ./build_virtpass-android.ps1 -Target assets

.EXAMPLE
    pwsh ./build_virtpass-android.ps1 -Variant release -Clean
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('apk', 'assets', 'jni', 'clean')]
    [string]$Target = 'apk',
    [ValidateSet('debug', 'release')]
    [string]$Variant = 'debug',
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$RegenGlAbi,

    # Anything not recognised as a named parameter is forwarded to make,
    # e.g. 'ANDROID_GUEST_SAMPLES=test_render test_render_gles'
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MakeArgs
)

$ErrorActionPreference = 'Stop'

# --- Repo root: this script lives at the repository root ---
$RVVM_ROOT = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $RVVM_ROOT 'Makefile'))) {
    throw "Makefile not found in $RVVM_ROOT - keep this script at the repository root."
}

$ANDROID_HOST = Join-Path $RVVM_ROOT 'src\virtpass\android-host'

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

$targetMap = @{ apk = 'android'; assets = 'android-assets'; jni = 'android-jni'; clean = 'android-clean' }
$makeTarget = $targetMap[$Target]

# --- Toolchain checks (zig cross-compiles the riscv64 guest ELFs) ---
if ($MakeTarget -ne 'android-clean' -and -not (Get-Command zig -ErrorAction SilentlyContinue)) {
    throw "zig not found - the riscv64 guest ELFs are cross-compiled with 'zig cc'. Install: winget install zig.zig"
}
if ($MakeTarget -eq 'android' -and -not (Test-Path -LiteralPath (Join-Path $ANDROID_HOST 'local.properties'))) {
    if (-not $env:ANDROID_HOME -and -not $env:ANDROID_SDK_ROOT) {
        Write-Warning "No local.properties in src/virtpass/android-host and neither ANDROID_HOME nor ANDROID_SDK_ROOT is set - Gradle will not find the Android SDK."
    }
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Jobs -lt 1) { $Jobs = 1 }

Write-Host "RVVM root : $RVVM_ROOT" -ForegroundColor Cyan
Write-Host "make      : $makeExe (-j$Jobs)" -ForegroundColor Cyan
Write-Host "target    : $makeTarget (variant: $Variant)" -ForegroundColor Cyan

Push-Location -LiteralPath $RVVM_ROOT
$exitCode = 0
try {
    if ($RegenGlAbi) {
        Write-Host "`n==> python tools/gen_gl_abi.py" -ForegroundColor Cyan
        & python (Join-Path $RVVM_ROOT 'tools\gen_gl_abi.py')
        if ($LASTEXITCODE -ne 0) { throw "gen_gl_abi.py failed (exit $LASTEXITCODE) - is Python 3 on PATH?" }
    }

    if ($Clean -and $makeTarget -ne 'android-clean') {
        Write-Host "`n==> $makeExe android-clean" -ForegroundColor Cyan
        & $makeExe android-clean
        if ($LASTEXITCODE -ne 0) { throw "make android-clean failed (exit $LASTEXITCODE)" }
    }

    $makeCmd = @("-j$Jobs", $makeTarget, "ANDROID_VARIANT=$Variant")
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
    Write-Host "`nAndroid virtpass build FAILED (exit $exitCode)" -ForegroundColor Red
    exit $exitCode
}

Write-Host "`nAndroid virtpass build complete." -ForegroundColor Green
exit 0
