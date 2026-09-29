# axis_android_sources.ps1 - does the NDK build compile the same RVVM sources the make build does?
#
# Why this exists
# ---------------
# There are two build systems for one source tree, and they collect sources by
# different means:
#
#   project.mk      recursive_match over $(SRCDIR)  - a new file joins by existing
#   CMakeLists.txt  an explicit list                - a new file has to be added
#
# Neither is wrong. The consequence is that the two source sets are supposed to be
# equal and nothing compares them, so a difference is discovered by whichever
# build links last. cow_pgt.c was the instance that turned out to be fatal: the
# win32 build passed, Android had not been built since the page-table work began,
# and the first Android link failed on four undefined symbols from the paged
# accessors in rvvm.c. The one host that could not have caught it was the only
# host that ran.
#
# A check that only says "these two lists differ" is not enough, because they are
# *meant* to differ - Android has no /dev/fb0, no PCI, no raw block device and no
# gdb stub, and its sockets go through a shim. So the difference is real and
# correct, and the check has to say which half of it is a decision. The
# $ExpectedExclusions below is that half, written down.
#
# What this asserts:
#
#   1. every file project.mk would compile under src/{core,cpu,util} is either in
#      CMakeLists.txt or listed in $ExpectedExclusions with a reason;
#   2. every entry in that part of CMakeLists.txt names a file that exists;
#   3. no exclusion has gone stale - one that no longer excludes anything is a
#      comment that will be read as a reason and believed.
#
# It is a gate over the *build description*, not over behaviour, so a failure here
# is always "the two build systems disagree" and never "a test failed".
param(
    [string]$Repo = 'H:\github_repos\RVVM'
)

$ErrorActionPreference = 'Stop'
$mk = Join-Path $Repo 'project.mk'
$cm = Join-Path $Repo 'src\virtpass\android-host\app\src\main\cpp\CMakeLists.txt'

# The directories that make up the shared core on both sides. virtpass front-ends,
# guest-samples, gui and devices are each built by their own rules on both sides and
# are not part of this comparison.
$LibDirs = @('core', 'cpu', 'util')
$DirAlt = 'core|cpu|util'

# Files project.mk compiles that the NDK build deliberately does not, each with the
# reason. This list is the interesting part: it is what turns "the sets differ" from
# a finding into a decision. Adding a line is a claim that the feature is genuinely
# unavailable on Android; deleting one is a claim that it is available and was
# forgotten.
$ExpectedExclusions = [ordered]@{
    'core/rvvm_fdt.c'           = 'all fdt_* calls in rvvm.c sit inside #if defined(USE_FDT); the NDK build does not define it, so there is nothing to reach'
    'core/rvvm_fbdev.c'         = 'framebuffer via /dev/fb0; Android renders through EGL'
    'core/rvvm_blk.c'           = 'raw host block device passthrough; no such device on Android'
    'core/rvvm_pci.c'           = 'PCI passthrough; not present on Android'
    'core/gdbstub.c'            = 'gdb server; the app is driven by its own session protocol'
    'core/rvvm_snapshot.c'      = 'snapshot/restore of a live machine; not offered on Android'
    'cpu/riscv32_interpreter.c' = 'only reached for an rv32 machine; the NDK build is RVVM_USE_RV64 only'
    'util/networking.c'         = 'TCP/IP for the tap devices; Android sockets go through the src/win/win_socket.c shim'
}

# --- what project.mk would compile -------------------------------------------------
# recursive_match over $(SRCDIR), filtered to C/C++ sources. It does not descend
# into build output, and neither does this: a stale CMakeFiles/ tree under src/ is
# not a source.
$mkExts = 'c', 'cpp', 'cc', 'cxx'
$mkFiles = @()
foreach ($d in $LibDirs) {
    $mkFiles += Get-ChildItem -LiteralPath (Join-Path $Repo "src\$d") -File -Recurse |
        Where-Object { $_.Extension.TrimStart('.') -in $mkExts } |
        ForEach-Object { $_.FullName.Substring($Repo.Length + 1).Replace('\', '/') }
}
# Everything below is named relative to src/, which is how CMakeLists.txt names it
# and how $ExpectedExclusions reads. Normalising here is deliberate: the comparison
# has to happen in one namespace, and it is the kind of detail that silently makes
# two sets look maximally different when they are identical.
$mkFiles = @($mkFiles |
    ForEach-Object { $_ -replace '^src/', '' } |
    Where-Object { $_ -notmatch '(^|/)(build|\.cxx)/' } |
    Sort-Object -Unique)

# --- what CMakeLists.txt lists -----------------------------------------------------
if (-not (Test-Path -LiteralPath $cm)) { Write-Host "missing: $cm"; exit 1 }
$cmText = [string](Get-Content -LiteralPath $cm -Raw)

# The extension alternation has to be spelled out. Written as \.[ch]xx? it means
# "one of c/h, then a literal x, then an optional x", which matches .cxx and .cx
# and therefore nothing at all here - the check then reported that the NDK build
# compiles zero files, i.e. that the two sets are maximally different. Narrower than
# the thing it describes, and pointing at the other of the two ways: it made the
# second build look broken rather than making the regex look wrong.
$cmAll = @([regex]::Matches($cmText, '\$\{RVVM_SRC\}/([\w/\.]+\.(?:c|cc|cpp|cxx|h))') |
    ForEach-Object { $_.Groups[1].Value }) | Sort-Object -Unique
$cmFiles = @($cmAll | Where-Object { $_ -match "^($DirAlt)/" })

Write-Host ''
Write-Host ("  make  (recursive_match over src/{{{0}}})   : {1} files" -f ($LibDirs -join ','), $mkFiles.Count)
Write-Host ("  ndk   (CMakeLists.txt rvvm_core, same dirs): {0} files" -f $cmFiles.Count)
Write-Host ("  of which declared excluded, with a reason  : {0} files" -f $ExpectedExclusions.Count)
Write-Host ''

$bad = 0

# --- 1. undeclared differences -------------------------------------------------
$undeclared = $mkFiles | Where-Object { $cmFiles -notcontains $_ -and -not $ExpectedExclusions.Contains($_) }
if ($undeclared) {
    Write-Host 'in the make build, but neither in CMakeLists.txt nor declared excluded:'
    foreach ($f in $undeclared) { Write-Host "    $f" }
    Write-Host '  This is the cow_pgt.c case: a new file that only the win32 build ever'
    Write-Host '  compiled. Either add it to CMakeLists.txt, or add it to'
    Write-Host '  $ExpectedExclusions with the reason it does not belong on Android.'
    $bad++
} else {
    Write-Host 'no undeclared source difference: every file the make build compiles is either'
    Write-Host 'listed for Android or declared excluded with a reason'
}

# --- 2. stale entries ----------------------------------------------------------
$stale = $cmFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Repo "src\$($_.Replace('/', '\'))")) }
if ($stale) {
    Write-Host ''
    Write-Host 'listed in CMakeLists.txt but not present on disk (stale entry):'
    foreach ($f in $stale) { Write-Host "    $f" }
    $bad++
}

# --- 3. exclusions that no longer exclude anything -----------------------------
$staleClaims = $ExpectedExclusions.Keys | Where-Object {
    $cmFiles -contains $_ -or $mkFiles -notcontains $_
}
if ($staleClaims) {
    Write-Host ''
    Write-Host 'declared as excluded, but the file is missing or is in fact compiled:'
    foreach ($f in $staleClaims) { Write-Host "    $f" }
    Write-Host '  An exclusion that no longer excludes anything is a comment that will be'
    Write-Host '  read as a reason and believed.'
    $bad++
}

# --- the per-ABI decisions, which are the other way the two sides can drift -----
Write-Host ''
$cmJitAbis = if ($cmText -match 'set\(RVVM_JIT_ABIS ([^)]+)\)') { $Matches[1].Trim() } else { 'not found' }
$cmMem = if ($cmText -match 'USERLAND_MEM_SIZE=(0x[0-9a-fA-F]+)') { $Matches[1] } else { 'no override' }
Write-Host "  USE_JIT        ndk decides per ABI: $cmJitAbis"
Write-Host "                 (rvjit/ sources are conditional and are not in the comparison above)"
Write-Host "  guest size     ndk overrides to $cmMem on a 32-bit ABI; the make build uses the default"
Write-Host ''
Write-Host '  Both of those are decisions made once, in CMakeLists.txt, and both are the kind of'
Write-Host '  thing that goes stale silently - the guest size in particular is the budget for how'
Write-Host '  many guest processes fit, so it is worth reading rather than assuming.'

# --- report --------------------------------------------------------------------
Write-Host ''
if ($bad) {
    Write-Host "$bad problem(s): the two build descriptions do not agree about the source set."
    exit 1
}
Write-Host 'the two build descriptions agree about which RVVM sources are compiled'
exit 0
