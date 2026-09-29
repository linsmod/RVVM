# pgt_mem_probe_selftest.ps1 - prove pgt_mem_probe.ps1's own checks fire.
#
# The A-axis gate has six mutations. The instrument that decides whether the page
# table works had none, and it had already produced two wrong numbers: 6.1 MB from
# a sync point that fired before the guest was up, and 1,058.8 MB per child from
# sampling the peak of a fork storm. Both were stable across three intervals and
# both looked reasonable, which is the reason they survived.
#
# A stable, plausible output is not evidence. It is a question nobody has asked
# yet. So each check gets an input that must make it red, and the red has to
# point at the line that produced it.
#
# CHECK 1 and CHECK 2 are pure functions of a string and of two numbers, so they
# are exercised directly by pgt_mem_probe.ps1 -SelfTest. CHECK 3 needs a real
# guest run, because what it detects is a machine that only exists while the
# measurement is in progress. That is this file.
param(
    [int]$Seconds = 6
)

$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot 'pgt_mem_probe.ps1'
$failed = 0

# Run the probe with a caller-chosen child command, and report what it said.
# The child command is a parameter precisely so it can be set to the thing that
# used to be wrong; if it were baked in, this test could not exist.
#
# Arguments go through ArgumentList rather than a quoted string: the child
# command itself contains double quotes, and a string-built command line splits
# them into separate arguments, which fails in a way that looks like the probe
# being broken rather than the harness being wrong.
function Run-Probe([string]$kid, [int]$n, [switch]$SelfTest) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = 'pwsh'
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void]$psi.ArgumentList.Add('-NoProfile')
    [void]$psi.ArgumentList.Add('-File')
    [void]$psi.ArgumentList.Add($probe)
    if ($SelfTest) {
        [void]$psi.ArgumentList.Add('-SelfTest')
    } else {
        [void]$psi.ArgumentList.Add('-Children'); [void]$psi.ArgumentList.Add("$n")
        [void]$psi.ArgumentList.Add('-Seconds');  [void]$psi.ArgumentList.Add("$Seconds")
        [void]$psi.ArgumentList.Add('-KidOverride'); [void]$psi.ArgumentList.Add($kid)
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit(30000) | Out-Null
    return [pscustomobject]@{ Code = $p.ExitCode; Text = ($out + $err) }
}

Write-Host 'pgt_mem_probe_selftest'
Write-Host ''

Write-Host 'PART 1: the pure checks (pgt_mem_probe.ps1 -SelfTest)'
$r1 = Run-Probe '' 0 -SelfTest
($r1.Text.TrimEnd() -split "`n") | ForEach-Object { Write-Host "  $_" }
if ($r1.Code -eq 0) { Write-Host '  ok' } else { Write-Host "  FAILED (exit $($r1.Code))"; $failed++ }

# CHECK 3. The bug this exists for: `while :; do sleep 1; done` forks once per
# second, so each child holds a second machine, and the probe reports 1,058.8 MB
# per child - exactly twice the arena, and for exactly the reason it is twice.
# The live machine count is what makes that visible without knowing the truth in
# advance: the probe asked for 1 child and should see 2 machines, and it will see
# 3.
Write-Host ''
Write-Host 'PART 2: a child command that forks must be caught by the live count'
Write-Host '  running with: sh -c "while :; do sleep 1; done"'
$r = Run-Probe 'sh -c "while :; do sleep 1; done"' 1
($r.Text -split "`n" | Where-Object { $_ -match 'machines alive|WARNING|Child command|forking' }) |
    ForEach-Object { Write-Host "  $_" }
if ($r.Code -eq 0) {
    Write-Host '  NOT CAUGHT - a forking child was accepted'
    $failed++
} else {
    Write-Host "  caught (exit $($r.Code))"
}

Write-Host ''
Write-Host 'PART 3: the same run with a builtin child must be clean'
Write-Host '  running with: sh -c "while :; do :; done"'
$r2 = Run-Probe 'sh -c "while :; do :; done"' 1
($r2.Text -split "`n" | Where-Object { $_ -match 'machines alive|WARNING|PrivateMemory' }) |
    ForEach-Object { Write-Host "  $_" }
if ($r2.Code -eq 0) {
    Write-Host '  clean, as it should be'
} else {
    Write-Host "  UNEXPECTED - the fork-free child was flagged (exit $($r2.Code))"
    $failed++
}

Write-Host ''
if ($failed) { Write-Host "$failed part(s) failed"; exit 1 }
Write-Host 'the instrument can no longer report a number it should have refused'
exit 0
