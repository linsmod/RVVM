#Requires -Version 7.0
<#
.SYNOPSIS
    Mutation test for tools/axis_a_by_abi.ps1 - proves the gate can fail.

.DESCRIPTION
    A gate that has never been observed failing carries no information. This one
    was green for a while while comparing against nothing, because TABLE 1 used
    the generic Linux syscall spellings while the dispatch uses its own, and an
    unclassified syscall is skipped by the soundness direction. So each direction
    is deliberately broken here and the gate is required to notice.

    Three mutations:
      1 soundness    a known writer put back on the read accessor
      2 completeness a syscall that fills memory removed from TABLE 1
      3 completeness the name mismatch that actually shipped, which must show up
                     as completeness rather than passing

    Every mutation is verified to have applied before the gate is run, because a
    mutation that silently fails to apply reports a clean gate as broken - the
    same mistake in reverse, and one this harness made the first time.

    The source file is restored and re-checked afterwards, so a failure here
    cannot leave a mutation in the tree.
#>
# Mutation-test the gate in tools/axis_a_by_abi.ps1.
#
# A gate that has never been observed failing carries no information. This one had
# been green while comparing against nothing, so each direction is deliberately
# broken and the gate is required to notice. The file is restored afterwards and
# the restored state is re-checked, so a failure here cannot leave a mutation
# behind in the tree.
param([string]$Repo = 'H:\github_repos\RVVM')

$ErrorActionPreference = 'Stop'
$src = Join-Path $Repo 'src\core\rvvm_user.c'
$gate = Join-Path $Repo 'tools\axis_a_by_abi.ps1'
$orig = [System.IO.File]::ReadAllText($src)
$failed = 0
function Invoke-Gate {
    $out = & pwsh -NoProfile -File $gate 2>&1
    return [pscustomobject]@{
        Code = $LASTEXITCODE
        Text = ($out | ForEach-Object { $_.ToString() }) -join "`n"
    }
}

function Show-Result([string]$title, [string]$expect) {
    $r = Invoke-Gate
    $lines = $r.Text -split "`n" | Where-Object { $_ -match 'soundness|completeness|    :|    case' }
    Write-Host ''
    Write-Host "=== $title ==="
    Write-Host "  expected: $expect"
    $lines | ForEach-Object { Write-Host "  $_" }
    Write-Host "  exit=$($r.Code)"
    if ($r.Code -eq 0) { Write-Host '  MUTATION NOT CAUGHT'; $script:failed++ }
    else { Write-Host '  caught' }
}

# Apply a mutation and refuse to proceed if it changed nothing. A mutation that
# silently fails to apply reports the gate as broken when the gate is fine, which
# is the same mistake in reverse - so the harness checks its own work first.
function Invoke-Mutation([string]$title, [string]$from, [string]$to) {
    Write-Host ''
    Write-Host "MUTATION: $title"
    if (-not $orig.Contains($from)) {
        Write-Host "  HARNESS BUG: the mutation target is not in the file: $from"
        $script:failed++
        return $null
    }
    $mutated = $orig.Replace($from, $to)
    [System.IO.File]::WriteAllText($src, $mutated)
    return $mutated
}

Write-Host 'baseline'
$base = Invoke-Gate
Write-Host "  exit=$($base.Code)"
if ($base.Code -ne 0) { Write-Host 'baseline is not clean, aborting'; exit 2 }

# Direction 1 - soundness. Put a known writer back on the read accessor.
if (Invoke-Mutation 'wait4 a3 (rusage) back to the read accessor' `
        '(int)a2, to_ptr_wr(a3));' '(int)a2, to_ptr(a3));') {
    Show-Result 'soundness direction' 'report 1 and exit 1'
}
[System.IO.File]::WriteAllText($src, $orig)

# Direction 2 - completeness. Remove a syscall that fills memory from TABLE 1.
# The trailing comma with no space matters: entries are packed several per line,
# and statx is last on its line, so matching '", ' would have silently not
# applied and the gate would have been blamed for a clean run.
if (Invoke-Mutation 'drop the statx entry from TABLE 1' '"statx\t4",' '') {
    Show-Result 'completeness direction' 'report 1 and exit 1'
}
[System.IO.File]::WriteAllText($src, $orig)

# Direction 3 - the one this gate actually shipped broken: a name the dispatch
# does not use makes the soundness direction vacuous, because an unclassified
# syscall is skipped. It must show up as completeness, not pass.
if (Invoke-Mutation 'rename statfs64 to the generic Linux spelling in TABLE 1' `
        '"statfs64\t1",' '"statfs\t1",') {
    Show-Result 'completeness catches the name mismatch' 'report 1 and exit 1'
}
[System.IO.File]::WriteAllText($src, $orig)

Write-Host ''
Write-Host 'restored'
$after = Invoke-Gate
Write-Host "  exit=$($after.Code)"
if ($after.Code -ne 0) { Write-Host 'RESTORE FAILED - the tree is dirty'; exit 2 }
if ($after.Text -ne $base.Text) { Write-Host 'RESTORE MISMATCH'; exit 2 }

# Direction 4 - the reach list. Renaming an entry point must not be silent: the
# completeness direction skips a syscall it cannot classify, so a reach list that
# stops recognising guest_copy_write would reclassify every syscall reaching guest
# memory through it as "touches no guest pointer" - and the reported count of
# benign entries would rise, which reads as progress. This is the one failure that
# points in the flattering direction, so it is the one worth a test.
Write-Host ''
Write-Host 'MUTATION: rename guest_copy_write so the reach list no longer matches it'
if ($orig.Contains('guest_copy_write')) {
    $mutated = $orig.Replace('guest_copy_write', 'guest_copy_write_renamed')
    [System.IO.File]::WriteAllText($src, $mutated)
    $r = Invoke-Gate
    Write-Host "  exit=$($r.Code)"
    ($r.Text -split "`n" | Where-Object { $_ -match 'drift|standby|guest_copy' }) | ForEach-Object { Write-Host "  $_" }
    if ($r.Code -eq 0) { Write-Host '  NOT CAUGHT - the reach list is not falsifiable'; $failed++ }
    else { Write-Host '  caught' }
    [System.IO.File]::WriteAllText($src, $orig)
} else {
    Write-Host '  HARNESS BUG: guest_copy_write is not in the source'
    $failed++
}

# Direction 5 - the same rename, but leaving the prose behind. This is the shape
# that got past the first version of the guard: renaming a function updates its
# call sites and its definition, and does not update the explanatory comments that
# mention it, so a name-only count still sees matches and stays green while the
# entry is dead. A mutation that renames only the code and leaves the comments is
# therefore the one that matters, and the plain rename above does not test it.
Write-Host ''
Write-Host 'MUTATION: rename guest_copy_write in code only, leaving the comments intact'
if ($orig.Contains('guest_copy_write')) {
    # Rewrite only the lines that are not comments, i.e. what an IDE rename does.
    $code = $orig
    $code = [regex]::Replace($code, '(?m)^(?!\s*(?://|/\*|\*)).*\bguest_copy_write\b.*$', {
        param($m) $m.Value.Replace('guest_copy_write', 'guest_copy_write_renamed')
    })
    [System.IO.File]::WriteAllText($src, $code)
    $stillInComments = ([regex]::Matches($code, '(?m)^\s*(?://|/\*|\*)[^\n]*\bguest_copy_write\b')).Count
    Write-Host "  (left $stillInComments comment mention(s) on purpose)"
    $r = Invoke-Gate
    Write-Host "  exit=$($r.Code)"
    ($r.Text -split "`n" | Where-Object { $_ -match 'drift|guest_copy' }) | ForEach-Object { Write-Host "  $_" }
    if ($r.Code -eq 0) { Write-Host '  NOT CAUGHT - a comment is satisfying the reach guard'; $failed++ }
    else { Write-Host '  caught' }
    [System.IO.File]::WriteAllText($src, $orig)
} else {
    Write-Host '  HARNESS BUG: guest_copy_write is not in the source'
    $failed++
}

Write-Host ''
Write-Host 'final restore check'
$final = Invoke-Gate
Write-Host "  exit=$($final.Code)"
if ($final.Code -ne 0 -or $final.Text -ne $base.Text) { Write-Host 'RESTORE FAILED'; exit 2 }

# Direction 6 - the dispatch's extent. The last case used to run to end of file,
# so anything after the switch counted as syscall dispatch. A call to
# guest_copy_write() placed after the dispatch must therefore leave the standby
# count alone: it is not in a case, and a tool that thought it was would stop
# being able to tell a dispatch write from a helper write - which is the whole
# question the A axis asks.
Write-Host ''
Write-Host 'MUTATION: add a guest_copy_write() call after the dispatch, outside any case'
$tail = "`nstatic void selftest_tail_helper(void)`n{`n    guest_copy_write(0, ""selftest"", 8);`n}`n"
[System.IO.File]::WriteAllText($src, $orig + $tail)
$r = Invoke-Gate
Write-Host "  exit=$($r.Code)"
$sb = ($r.Text -split "`n" | Where-Object { $_ -match 'guest_copy_write = ' })
$sb | ForEach-Object { Write-Host "  $_" }
$standbyLine = ($r.Text -split "`n" | Where-Object { $_ -match 'reach entries are on standby' })
$standbyLine | ForEach-Object { Write-Host "  $_" }
if ($sb -and ($sb -notmatch 'on standby')) {
    Write-Host '  NOT CAUGHT - a call outside the switch was credited to the dispatch'
    $failed++
} else {
    Write-Host '  caught (still on standby, i.e. not attributed to a case)'
}
[System.IO.File]::WriteAllText($src, $orig)

if ($failed) { Write-Host "`n$failed mutation(s) not caught"; exit 1 }
Write-Host ''
Write-Host 'all six mutations caught, tree restored'
exit 0
