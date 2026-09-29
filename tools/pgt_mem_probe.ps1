# Sample one guest run's process while it holds N concurrent long-lived children.
#
# The three numbers are reported separately and never added together, because on
# this problem they move independently and conflating them is what made the
# original bug hard to see: one run reported 1.22 GB of VSZ against 116 MB of RSS.
#   PrivateMemorySize64 -> PrivateUsage, commit charged as private. This is the
#                          one that has to drop when the page table lands.
#   WorkingSet64        -> physical pages actually resident.
#   VirtualMemorySize64 -> address space reserved. On a 32-bit host this is the
#                          binding constraint, and no view-based scheme reduces it.
# Measure the per-child cost, and the intercept, separately.
#
# The per-child slope is the thing commit 3 changes and the only thing it can
# change. The intercept is the base plus the first machine's arena, which commit 3
# does not touch, so an absolute target is the wrong criterion: at zero children
# Private is already ~1,060 MB, and an absolute "drop to ~100 MB" would be
# unreachable without also changing how the base is reserved. Measuring the slope
# cannot be satisfied by doing nothing, and cannot be satisfied by quietly
# shrinking something else either.
#
# Run with -Series to get the slope directly; the default single run is for
# spot checks against a known baseline.
param(
    [int]$Children = 3,
    [int]$Seconds = 12,
    [switch]$Series,
    [int[]]$Ladder = @(0, 1, 3, 6)
)

$exe = Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_winhost_x86_64.exe'
$exe = (Resolve-Path -LiteralPath $exe).Path

# Each child is a shell spinning on sleep, so it holds its address space for the whole
# sample without consuming CPU worth measuring.
$kid = 'sh -c "while :; do sleep 1; done"'

function Measure-Run([int]$n, [int]$secs) {
    $script = ''
    for ($i = 0; $i -lt $n; $i++) { $script += "$kid &`n" }
    $script += "sleep $secs`nexit`n"

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    $psi.Arguments = '--app test_busybox'
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Write($script)
    $proc.StandardInput.Flush()

    # Sample the whole window and keep the peak. Reading a fixed number of output
    # lines is not a reliable sync point: the guest emits a variable number of
    # host log lines before its console is ready, and a probe that samples early
    # reports a few MB and looks like a spectacular result. Peak over the window
    # is also the honest number, since a transient spike during fork is part of
    # what the measurement is about.
    $peak = $null
    for ($s = 0; $s -lt ($secs + 4); $s++) {
        try { $proc.Refresh() } catch { break }
        if ($proc.HasExited) { break }
        if ($null -eq $peak -or $proc.PrivateMemorySize64 -gt $peak.Private) {
            $peak = [pscustomobject]@{
                Private = $proc.PrivateMemorySize64
                WS      = $proc.WorkingSet64
                VSZ     = $proc.VirtualMemorySize64
            }
        }
        Start-Sleep -Milliseconds 500
    }
    try { if (-not $proc.HasExited) { $proc.Kill($true) } } catch { }
    try { $proc.WaitForExit(5000) | Out-Null } catch { }
    return $peak
}

if ($Series) {
    Write-Host "per-child slope: $($Ladder -join ' / ') concurrent long-lived children"
    Write-Host ''
    $rows = @()
    foreach ($n in $Ladder) {
        $m = Measure-Run $n $Seconds
        if (-not $m) { Write-Host "  n=$n  (process exited before it could be sampled)"; continue }
        $rows += [pscustomobject]@{ N = $n; Private = $m.Private; WS = $m.WS; VSZ = $m.VSZ }
        Write-Host ("  n={0,-3} Private={1,9:N1} MB   WS={2,7:N1} MB   VSZ={3,9:N1} MB" -f `
                    $n, ($m.Private / 1MB), ($m.WS / 1MB), ($m.VSZ / 1MB))
    }
    if ($rows.Count -lt 2) { Write-Host 'not enough points for a slope'; exit 1 }

    Write-Host ''
    Write-Host '  interval        per-child Private   per-child VSZ'
    for ($i = 1; $i -lt $rows.Count; $i++) {
        $dn = $rows[$i].N - $rows[$i - 1].N
        $dp = ($rows[$i].Private - $rows[$i - 1].Private) / 1MB / $dn
        $dv = ($rows[$i].VSZ - $rows[$i - 1].VSZ) / 1MB / $dn
        Write-Host ("  {0,2} -> {1,-3} {2,14:N1} MB   {3,13:N1} MB" -f `
                    $rows[$i - 1].N, $rows[$i].N, $dp, $dv)
    }
    $first = $rows[0]
    Write-Host ''
    Write-Host ("  intercept (n={0}): Private={1:N1} MB  VSZ={2:N1} MB  -- base plus the first machine," -f `
                $first.N, ($first.Private / 1MB), ($first.VSZ / 1MB))
    Write-Host '  arena, and the host itself. Commit 3 does not change this; the'
    Write-Host '  criterion is the slope above.'
    exit 0
}

$peak = Measure-Run $Children $Seconds
Write-Host "1 guest + $Children concurrent long-lived children"
if (-not $peak) { Write-Host '  (process exited before it could be sampled)'; exit 1 }
Write-Host ("  PrivateMemorySize64 = {0,8:N1} MB   (commit charged private)" -f ($peak.Private / 1MB))
Write-Host ("  WorkingSet64        = {0,8:N1} MB   (physical resident)"    -f ($peak.WS / 1MB))
Write-Host ("  VirtualMemorySize64 = {0,8:N1} MB   (address space)"        -f ($peak.VSZ / 1MB))
exit 0
