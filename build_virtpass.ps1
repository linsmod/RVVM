#Requires -Version 5.1
<#
.SYNOPSIS
    One build entry point for every RVVM virtpass artifact: Win32 host, Android
    guest assets / JNI / APK and the guest VirtPass SDK.

.DESCRIPTION
    Thin wrapper around the repository Makefile: every compile/link rule lives
    in ./Makefile + ./project.mk, this script only locates the toolchain, drives
    mingw32-make and reports the produced artifacts.

    -Target selects what to build, and everything runs in a single make
    invocation (so -jN is shared by all goals of the run):

      all     -> bin android vp-sdk   (default when -Target is omitted)
      win32   -> bin                  (rvvm_winhost_<arch>.exe + the other Windows binaries)
      apk     -> android              (guest assets + librvvm_jni.so + APK)
      android -> same as apk
      assets  -> android-assets       (zig/musl riscv64 guest ELFs into APK assets)
      jni     -> android-jni          (librvvm_jni.so only)
      sdk     -> vp-sdk               (guest VirtPass SDK -> lib/libvpsdk.{a,so})
      clean   -> clean android-clean  (make build tree + Gradle outputs)

.PARAMETER Target
    What to build: all (default), win32, apk, android, assets, jni, sdk or clean.

.PARAMETER Variant
    Gradle build variant for the Android goals: debug (default) or release.
    Only passed to make when the run actually touches Gradle (apk/android/jni).

.PARAMETER Clean
    Clean before building: `make clean` for the make-side artifacts (host
    binaries, guest ELFs, SDK) and `make android-clean` for the Gradle outputs,
    each only when the selected targets need it.

.PARAMETER Jobs
    Parallel make jobs (default: logical processor count).

.PARAMETER RegenGlAbi
    Regenerate include/virtpass/vp_gl.h, src/virtpass/vp_gl_stub.c and the
    Win32 dispatch tables with tools/gen_gl_abi.py before building, then audit
    the pointer classification and regenerate the "not implemented" SDK sources
    (`make vp-sdk-gen`) so they cannot drift behind the new ABI.

.PARAMETER MakeArgs
    Any extra arguments are passed verbatim to make (e.g. USE_RVJIT=0).

.EXAMPLE
    pwsh ./build_virtpass.ps1

.EXAMPLE
    pwsh ./build_virtpass.ps1 -Target win32 -Clean -Jobs 8

.EXAMPLE
    pwsh ./build_virtpass.ps1 -Target assets -RegenGlAbi

.EXAMPLE
    pwsh ./build_virtpass.ps1 -Target apk -Variant release
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('all', 'win32', 'apk', 'android', 'assets', 'jni', 'sdk', 'clean')]
    [string]$Target = 'all',
    [ValidateSet('debug', 'release')]
    [string]$Variant = 'debug',
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$RegenGlAbi,

    # Anything not recognised as a named parameter is forwarded to make,
    # e.g. 'USE_RVJIT=0' or 'ANDROID_GUEST_SAMPLES=test_render'
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

# --- -Target -> make goals (the single source of truth for what gets built) ---
$targetMap = @{
    'all'     = @('bin', 'android', 'vp-sdk')
    'win32'   = @('bin')
    'apk'     = @('android')
    'android' = @('android')
    'assets'  = @('android-assets')
    'jni'     = @('android-jni')
    'sdk'     = @('vp-sdk')
    'clean'   = @('clean', 'android-clean')
}

$makeTargets = $targetMap[$Target]

# Does the current run touch any of the given make goals?
function Test-Goal {
    param([Parameter(Mandatory)][string[]]$Goals)
    foreach ($goal in $Goals) {
        if ($makeTargets -contains $goal) { return $true }
    }
    return $false
}

$needsZig    = Test-Goal @('android-assets', 'android', 'vp-sdk')
$needsGradle = Test-Goal @('android', 'android-jni')

# --- Preflight: fail before any work instead of halfway through ---
if ($needsZig -and -not (Get-Command zig -ErrorAction SilentlyContinue)) {
    throw "zig not found - the riscv64 guest ELFs and the VirtPass SDK are cross-compiled with 'zig cc'. Install: winget install zig.zig  (host binaries only: -Target win32)"
}
if ($needsGradle -and -not (Test-Path -LiteralPath (Join-Path $ANDROID_HOST 'local.properties'))) {
    if (-not $env:ANDROID_HOME -and -not $env:ANDROID_SDK_ROOT) {
        Write-Warning "No local.properties in src/virtpass/android-host and neither ANDROID_HOME nor ANDROID_SDK_ROOT is set - Gradle will not find the Android SDK."
    }
}
if ($RegenGlAbi -and -not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "python not found - tools/gen_gl_abi.py needs Python 3 on PATH."
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Jobs -lt 1) { $Jobs = 1 }

Write-Host "RVVM root : $RVVM_ROOT" -ForegroundColor Cyan
Write-Host "make      : $makeExe (-j$Jobs)" -ForegroundColor Cyan
Write-Host "target    : $Target -> make $($makeTargets -join ' ')" -ForegroundColor Cyan
if ($needsGradle) {
    Write-Host "variant   : $Variant" -ForegroundColor Cyan
}

# Run one make invocation and abort the whole run if it fails.
function Invoke-Make {
    param([Parameter(Mandatory)][string[]]$Cmd)
    Write-Host "`n==> $makeExe $($Cmd -join ' ')" -ForegroundColor Cyan
    & $makeExe @Cmd
    if ($LASTEXITCODE -ne 0) { throw "make failed (exit $LASTEXITCODE)" }
}

# Report one artifact per line, resolved relative to the repo root.
function Write-ArtifactLine {
    param([Parameter(Mandatory)][string]$Label, [Parameter(Mandatory)][string[]]$Paths)
    if (-not $Paths -or $Paths.Count -eq 0) {
        Write-Host ("  {0}: not found" -f $Label) -ForegroundColor Yellow
        return
    }
    foreach ($path in $Paths) {
        $shown = Resolve-Path -LiteralPath $path -Relative -ErrorAction SilentlyContinue
        if (-not $shown) { $shown = $path }
        Write-Host ("  {0}: {1}" -f $Label, $shown) -ForegroundColor Green
    }
}

function Show-Artifacts {
    Write-Host "`nArtifacts:" -ForegroundColor Cyan

    if (Test-Goal @('bin')) {
        # A build tree may hold both debug.<os>.<arch>\ and release.<os>.<arch>\
        # - newest first, so the one just built is the one on top.
        $hostBin = @(Get-ChildItem -Path "$RVVM_ROOT\*\rvvm_winhost_*.exe" -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | ForEach-Object { $_.FullName })
        Write-ArtifactLine 'Win32 host' $hostBin
    }

    if (Test-Goal @('android-assets', 'android')) {
        # android-assets links every sample straight into the APK assets tree
        $assetsDir = Join-Path $ANDROID_HOST 'app\src\main\assets'
        $guest = @(Get-ChildItem -LiteralPath $assetsDir -Filter '*.exe' -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
        Write-ArtifactLine 'guest ELFs' $guest
    }

    if (Test-Goal @('android-jni')) {
        $cxxDir = Join-Path $ANDROID_HOST 'app\build\intermediates\cxx'
        $jniSo = @()
        if (Test-Path -LiteralPath $cxxDir) {
            $jniSo = @(Get-ChildItem -LiteralPath $cxxDir -Recurse -Filter 'librvvm_jni.so' -File -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName })
        }
        Write-ArtifactLine 'JNI library' $jniSo
    }

    if (Test-Goal @('android')) {
        $outDir = Join-Path $ANDROID_HOST 'app\build\outputs'
        $apk = @()
        if (Test-Path -LiteralPath $outDir) {
            $apk = @(Get-ChildItem -LiteralPath $outDir -Recurse -Filter '*.apk' -File -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName })
        }
        Write-ArtifactLine 'APK' $apk
    }

    if (Test-Goal @('vp-sdk')) {
        Write-ArtifactLine 'VirtPass SDK' @(
            (Join-Path $RVVM_ROOT 'lib\libvpsdk.a'),
            (Join-Path $RVVM_ROOT 'lib\libvpsdk.so')
        )
    }
}

Push-Location -LiteralPath $RVVM_ROOT
$exitCode = 0
try {
    if ($RegenGlAbi) {
        Write-Host "`n==> python tools/gen_gl_abi.py" -ForegroundColor Cyan
        & python (Join-Path $RVVM_ROOT 'tools\gen_gl_abi.py')
        if ($LASTEXITCODE -ne 0) { throw "gen_gl_abi.py failed (exit $LASTEXITCODE) - is Python 3 on PATH?" }

        # The guest and host must agree on the fn_id space: an entry point
        # resolved in the guest (eglGetProcAddress) still needs its guest stub
        # generated, but must not consume a fn_id, or every later id shifts.
        Write-Host "`n==> python tools/audit_gl_ptr.py --check" -ForegroundColor Cyan
        & python (Join-Path $RVVM_ROOT 'tools\audit_gl_ptr.py') --check
        if ($LASTEXITCODE -ne 0) { throw "audit_gl_ptr.py found unclassified pointer params" }

        # gen_gl_abi.py rewrote the real guest stubs, so src/virtpass/vp-sdk's
        # checked-in "not implemented" sources are stale. `make vp-sdk` compiles
        # them as-is and would happily ship an SDK without the new entry points,
        # so regenerate them here - this is the only step that writes that
        # directory, and it stays out of the build rules on purpose.
        Invoke-Make @('vp-sdk-gen')
    }

    if ($Clean -and $Target -ne 'clean') {
        # The make-side artifacts live in the build tree, the APK/JNI outputs
        # belong to Gradle - clean only the side the selection actually builds.
        $cleanTargets = @()
        if (Test-Goal @('bin', 'android-assets', 'vp-sdk')) { $cleanTargets += 'clean' }
        if ($needsGradle) { $cleanTargets += 'android-clean' }
        if ($cleanTargets.Count -eq 0) { $cleanTargets = @('clean') }
        Invoke-Make $cleanTargets
    }

    $makeCmd = @("-j$Jobs") + $makeTargets
    if ($needsGradle) { $makeCmd += "ANDROID_VARIANT=$Variant" }
    if ($MakeArgs) { $makeCmd += $MakeArgs }
    Invoke-Make $makeCmd

    if ($Target -ne 'clean') { Show-Artifacts }
} catch {
    Write-Host $_ -ForegroundColor Red
    $exitCode = 1
} finally {
    Pop-Location
}

if ($exitCode -ne 0) {
    Write-Host "`nvirtpass build FAILED (exit $exitCode)" -ForegroundColor Red
    exit $exitCode
}

Write-Host "`nvirtpass build complete ($Target)." -ForegroundColor Green
exit 0
