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
param(
    [int]$Children = 3,
    [int]$Seconds = 12
)

$exe = Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_winhost_x86_64.exe'
$exe = (Resolve-Path -LiteralPath $exe).Path

# Each child is a shell spinning on sleep, so it holds its address space for the
# whole sample without consuming CPU worth measuring.
$kid = 'sh -c "while :; do sleep 1; done"'
$script = ''
for ($i = 0; $i -lt $Children; $i++) { $script += "$kid &`n" }
$script += "sleep $Seconds`nexit`n"

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

# Wait for the children to exist before sampling, so the number is the steady
# state and not the boot transient.
$null = $proc.StandardOutput.ReadLine()
$null = $proc.StandardOutput.ReadLine()
$null = $proc.StandardOutput.ReadLine()
Start-Sleep -Seconds 3

$peak = $null
for ($s = 0; $s -lt $Seconds; $s++) {
    try { $proc.Refresh() } catch { break }
    if ($proc.HasExited) { break }
    if ($peak -eq $null -or $proc.PrivateMemorySize64 -gt $peak.Private) {
        $peak = [pscustomobject]@{
            Private = $proc.PrivateMemorySize64
            WS      = $proc.WorkingSet64
            VSZ     = $proc.VirtualMemorySize64
        }
    }
    Start-Sleep -Milliseconds 500
}

try { if (-not $proc.HasExited) { $proc.Kill($true) } } catch { }
$proc.WaitForExit(5000) | Out-Null

Write-Host "1 guest + $Children concurrent long-lived children"
if (-not $peak) { Write-Host '  (process exited before it could be sampled)'; exit 1 }
Write-Host ("  PrivateMemorySize64 = {0,8:N1} MB   (commit charged private)" -f ($peak.Private / 1MB))
Write-Host ("  WorkingSet64        = {0,8:N1} MB   (physical resident)"    -f ($peak.WS / 1MB))
Write-Host ("  VirtualMemorySize64 = {0,8:N1} MB   (address space)"        -f ($peak.VSZ / 1MB))
exit 0
