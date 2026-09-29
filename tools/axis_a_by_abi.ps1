#Requires -Version 7.0
<#
.SYNOPSIS
    Checks the syscall out-parameter classification against the table that lives in
    the dispatch itself.

.DESCRIPTION
    Guest RAM reaches the host through to_ptr() / to_ptr_sz() in
    src/core/rvvm_user.c, which hand out a read-only pointer unless the call site
    declares itself a writer. Once rvvm_ram_t::pgt is non-NULL, a call site that
    writes through the read accessor is handed a pointer into the shared read-only
    base and faults on the store.

    That classification cannot be done by reading the call sites. Thirteen of them
    are textually identical - `a2 ? to_ptr_sz(a1, a2) : NULL` - and whether one is a
    writer depends on which syscall's argument it is: read() has the kernel fill
    it, write() has it as the source, and the two lines differ only in the variable
    name. So the verdict comes from the Linux syscall ABI, recorded as TABLE 1 in
    src/core/rvvm_user.c immediately above the dispatch.

    The table is read from there rather than from a copy. A copy is the worst
    failure mode available: the checker keeps reporting success while validating a
    table that no longer describes the code.

    Two directions, because they catch different things:

      soundness     every to_ptr*() landing on an out-parameter is a wr variant.
                    A hit here is a real defect: it faults on the store.
      completeness  every syscall the dispatch handles is either in TABLE 1 or
                    listed as explicitly filling nothing. A hit here is a hole in
                    TABLE 1 - a syscall whose call sites are unclassified, which is
                    also a real defect and the one invisible from the code.

    The completeness list is long by nature: most syscalls fill no buffer. What
    matters is that it is finite and reviewable, and that anything in it which
    turns out to fill something is a table bug.

.PARAMETER File
    Path to rvvm_user.c. Defaults to the one in this repository.

.EXAMPLE
    pwsh ./tools/axis_a_by_abi.ps1 -ShowMissing

.OUTPUT
    Exit code 0 when both directions are clean, 1 otherwise.
#>
param(
    [string]$File = (Join-Path $PSScriptRoot '..\src\core\rvvm_user.c')
)

$ErrorActionPreference = 'Stop'
$file = (Resolve-Path -LiteralPath $File).Path
$lines = Get-Content -LiteralPath $file

# ---------------------------------------------------------------------------
# Read TABLE 1 out of the source. Format: "name<TAB>0,1,2" entries inside
# syscalls_filling_guest_memory[], terminated by NULL.
# ---------------------------------------------------------------------------
$tableLine = ($lines | Select-String -Pattern 'syscalls_filling_guest_memory\[\]\s*=\s*\{' | Select-Object -First 1)
if (-not $tableLine) { Write-Host "could not find syscalls_filling_guest_memory[] in $file"; exit 2 }

$fills = @{}
$fillsNothing = @{}
for ($i = $tableLine.LineNumber; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '\}\s*;\s*$') { break }
    foreach ($m in [regex]::Matches($lines[$i], '"([^"]+)"')) {
        # The C source holds the two-character escape \t, not a tab byte, so
        # split on the literal sequence. Accept a real tab too, so the format
        # stays readable if someone pastes one in.
        $parts = $m.Groups[1].Value -split '\\t|`t'
        $name = $parts[0].Trim()
        if (-not $name) { continue }
        $idx = @()
        if ($parts.Count -ge 2 -and $parts[1].Trim()) {
            $idx = @($parts[1] -split ',' | ForEach-Object { [int]$_.Trim() })
        }
        if ($idx.Count) { $fills[$name] = $idx } else { $fillsNothing[$name] = $true }
    }
}
if (-not $fills.Count -and -not $fillsNothing.Count) { Write-Host 'TABLE 1 parsed as empty'; exit 2 }

# Pointer-to-pointer cases: argument position tells the whole story for
# everything except these, where the position names a struct whose fields point at
# the buffers that actually get filled.
$exceptions = @{
    'recvmsg' = 'the msghdr is read; its name/control buffers are translated in rvvm_msghdr_from_guest()'
    'sendmsg' = 'same shape; the backfilled buffers are in rvvm_msghdr_from_guest()'
}

# The complete set of ways this file can reach guest memory.
#
# This list is the reach criterion, so it is written out rather than left as a
# shape like '\bto_ptr' that happens to cover most of it. A reach test built from
# a partial shape is a fact rather than a guarantee: it says the syscalls below are
# settled only for as long as nobody reaches guest memory a new way. The obvious
# way that happens is someone calling guest_write_mem() straight from a `case`,
# which is a perfectly reasonable refactor and which this list would then miss -
# the syscall would be reported as touching no guest pointer, when it writes one.
#
# When a new entry point is added, add it here in the same commit. That is the same
# discipline as TABLE 1 itself: a closed set that is written down, rather than a
# convention that is remembered.
$ReachEntries = @(
    'to_ptr', 'to_ptr_sz', 'to_ptr_wr', 'to_ptr_sz_wr', 'to_ptr_ioctl', 'to_str',
    'guest_copy_read', 'guest_copy_write', 'rvvm_user_guest_ptr'
)

# Each entry's name must still occur in the source at all. Without this, the reach
# criterion is not falsifiable: renaming guest_copy_write would leave the
# completeness direction silently reclassifying every syscall that reached guest
# memory through it as "touches no guest pointer" - and the reported count of
# unclassified-but-benign syscalls would *rise*, which reads as an improvement.
# A filter that stops recognising an entry is the worst failure mode here, because
# it points in the flattering direction and no guard speaks.
#
# The count has to be file-wide, not per-case. Per-case is exactly why
# guest_copy_read/write and rvvm_user_guest_ptr are currently unfalsifiable: all
# nine of their call sites sit below the dispatch, so they contribute zero to every
# case and dropping them from the list changes nothing the gate reports.
$text = $lines -join "`n"
$drifted = @()
$entryHits = @{}
foreach ($e in $ReachEntries) {
    $n = ([regex]::Matches($text, "\b$e\s*\(")).Count
    $entryHits[$e] = $n
    if ($n -eq 0) { $drifted += $e }
}

$ReachPattern = ($ReachEntries | ForEach-Object { "\b$_\b" }) -join '|'

# ---------------------------------------------------------------------------
# Index the dispatch: case number -> { name, start, end }
# ---------------------------------------------------------------------------
$cases = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*case\s+(\d+)\s*:\s*(?:\{\s*)?(?://\s*([A-Za-z_][A-Za-z0-9_]*))?') {
        $cases += [pscustomobject]@{
            Num   = [int]$Matches[1]
            Name  = $Matches[2]
            Start = $i
            End   = [int]::MaxValue
        }
    }
}
for ($k = 0; $k -lt ($cases.Count - 1); $k++) { $cases[$k].End = $cases[$k + 1].Start }
if ($cases.Count) { $cases[$cases.Count - 1].End = $lines.Count }

# ---------------------------------------------------------------------------
# Direction 1: soundness
# ---------------------------------------------------------------------------
$soundness = @()
foreach ($c in $cases) {
    $name = $c.Name
    if (-not $name) { continue }
    if ($exceptions.ContainsKey($name)) { continue }
    if (-not $fills.ContainsKey($name)) { continue }
    for ($i = $c.Start; $i -lt $c.End; $i++) {
        $line = $lines[$i]
        foreach ($m in [regex]::Matches($line, '(?<fn>to_ptr|to_str|to_ptr_sz|to_ptr_wr|to_ptr_sz_wr|to_ptr_ioctl)\s*\(\s*(?<arg>[^,)]*)')) {
            $fn = $m.Groups['fn'].Value
            $isWritable = ($fn -like '*_wr') -or ($fn -eq 'to_ptr_ioctl')
            $am = [regex]::Match($m.Groups['arg'].Value, '\ba([0-5])\b')
            if (-not $am.Success) { continue }
            $pos = [int]$am.Groups[1].Value
            if (($fills[$name] -contains $pos) -and -not $isWritable) {
                $soundness += [pscustomobject]@{
                    Line = $i + 1; Syscall = $name; Arg = "a$pos"; Fn = $fn; Text = $line.Trim()
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Direction 2: completeness
# ---------------------------------------------------------------------------
$missing = @()
$benign = 0
foreach ($c in $cases) {
    $name = $c.Name
    if (-not $name) { continue }
    if ($fills.ContainsKey($name) -or $fillsNothing.ContainsKey($name)) { continue }
    $ptrSites = @()
    for ($i = $c.Start; $i -lt $c.End; $i++) {
        if ($lines[$i] -match $ReachPattern) { $ptrSites += ($i + 1) }
    }
    if ($ptrSites.Count -eq 0) {
        # No guest-pointer access at all, so there is nothing to classify. Not a
        # finding: adding it to TABLE 1 would be noise, and the point of the
        # completeness direction is to catch a syscall that IS unclassified, not
        # one that cannot be classified wrongly.
        $benign++
        continue
    }
    $missing += [pscustomobject]@{ Num = $c.Num; Name = $name; PtrSites = $ptrSites }
}

Write-Host "TABLE 1 (from src/core/rvvm_user.c): $($fills.Count) filling, $($fillsNothing.Count) explicitly empty"
Write-Host "dispatch: $($cases.Count) named cases; $benign of the unclassified ones touch no guest pointer"
Write-Host ''
Write-Host 'reach criterion - file-wide call sites per entry:'
$standby = @()
foreach ($e in $ReachEntries) {
    # An entry that is never called from inside a case is on standby: correct
    # today, because the calls sit below the dispatch. Printed so a reader knows
    # it is deliberate rather than redundant.
    $inCase = 0
    foreach ($c in $cases) {
        for ($i = $c.Start; $i -lt $c.End; $i++) {
            if ($lines[$i] -match "\b$e\s*\(") { $inCase++ }
        }
    }
    $tag = if ($inCase -eq 0) { '  (on standby: never called from a case)' } else { '' }
    if ($inCase -eq 0) { $standby += $e }
    Write-Host "    $e = $($entryHits[$e])$tag"
}
if ($drifted.Count) {
    Write-Host ''
    Write-Host "reach entries with no call site in the file - the list has drifted: $($drifted -join ', ')"
    Write-Host '  either the entry was renamed, or it was removed. Update $ReachEntries in the same commit.'
}
Write-Host ''
Write-Host "soundness    a wr accessor missing: $($soundness.Count)"
foreach ($s in $soundness) { Write-Host "    :$($s.Line)  $($s.Syscall) $($s.Arg) -> $($s.Fn)  |  $($s.Text)" }
Write-Host "completeness a dispatched syscall that touches guest memory but is not in TABLE 1: $($missing.Count)"
foreach ($m in $missing) { Write-Host "    case $($m.Num)  $($m.Name)  pointer sites: $($m.PtrSites -join ',')" }

if ($drifted.Count) { exit 1 }
if ($soundness.Count -or $missing.Count) { exit 1 }
Write-Host ''
Write-Host "both directions clean; $($standby.Count) reach entr$($(if($standby.Count -eq 1){'y is'}else{'ies are'})) on standby"
exit 0
