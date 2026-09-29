# axis_android_sources_selftest.ps1 - prove axis_android_sources.ps1 can fail.
#
# The cow_pgt.c incident: a new file under src/util/ joined the make build by
# existing, was never added to CMakeLists.txt, and the win32 build passed. Android
# had not been built since the page-table work began, so the first Android link in
# weeks failed on four undefined symbols - on the one host that could not have
# caught it, because that host was the only one that ran.
#
# ec4d558 fixed the instance. It said so itself: "Fixing the one line makes Android
# build; it does not make the next new file appear there." So the thing that needed
# proving is not that the sets agree today - they do, and that is a reading - but
# that the check notices when they stop agreeing. A gate that has only ever been
# green is a question nobody has asked.
#
# Each mutation below drives one of the three assertions, and each is applied to a
# scratch copy of the repository description rather than to the tree, so a failure
# here cannot leave the working tree dirty.
param([string]$Repo = 'H:\github_repos\RVVM')

$ErrorActionPreference = 'Stop'
$gate = Join-Path $Repo 'tools\axis_android_sources.ps1'
$failed = 0

function Invoke-Gate {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = 'pwsh'
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void]$psi.ArgumentList.Add('-NoProfile')
    [void]$psi.ArgumentList.Add('-File')
    [void]$psi.ArgumentList.Add($gate)
    [void]$psi.ArgumentList.Add('-Repo')
    [void]$psi.ArgumentList.Add($Repo)
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit(30000) | Out-Null
    return [pscustomobject]@{ Code = $p.ExitCode; Text = $out }
}

# A scratch tree: same layout, only the files the gate reads.
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("axsrc_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
foreach ($d in @('', 'core', 'cpu', 'util', 'virtpass\android-host\app\src\main\cpp')) {
    New-Item -ItemType Directory -Path (Join-Path $scratch "src\$d") -Force -ErrorAction SilentlyContinue | Out-Null
}
Copy-Item (Join-Path $Repo 'project.mk') $scratch
Copy-Item (Join-Path $Repo 'src\virtpass\android-host\app\src\main\cpp\CMakeLists.txt') `
          (Join-Path $scratch 'src\virtpass\android-host\app\src\main\cpp')
foreach ($d in @('core', 'cpu', 'util')) {
    Copy-Item (Join-Path $Repo "src\$d\*.c") (Join-Path $scratch "src\$d")
}
$cmPath = Join-Path $scratch 'src\virtpass\android-host\app\src\main\cpp\CMakeLists.txt'
$cmOrig = [System.IO.File]::ReadAllText($cmPath)

function Invoke-ScratchGate {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = 'pwsh'
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void]$psi.ArgumentList.Add('-NoProfile')
    [void]$psi.ArgumentList.Add('-File')
    [void]$psi.ArgumentList.Add($gate)
    [void]$psi.ArgumentList.Add('-Repo')
    [void]$psi.ArgumentList.Add($scratch)
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit(30000) | Out-Null
    return [pscustomobject]@{ Code = $p.ExitCode; Text = $out }
}

Write-Host 'axis_android_sources_selftest'
Write-Host ''

Write-Host 'BASELINE: the real tree agrees'
$r = Invoke-Gate
Write-Host "  exit=$($r.Code)"
if ($r.Code -ne 0) { Write-Host '  UNEXPECTED - the real tree disagrees'; $failed++ }
else { Write-Host '  clean' }

Write-Host ''
Write-Host 'MUTATION 1: a new file under src/util/ that CMakeLists.txt does not list'
Write-Host '  (this is the cow_pgt.c case)'
$newFile = Join-Path $scratch 'src\util\zz_newfeature.c'
[System.IO.File]::WriteAllText($newFile, "int zz_newfeature(void) { return 0; }`n")
$m1 = Invoke-ScratchGate
($m1.Text -split "`n" | Where-Object { $_ -match 'zz_newfeature|undeclared|cow_pgt.c case' }) |
    ForEach-Object { Write-Host "  $_" }
if ($m1.Code -eq 0) { Write-Host '  NOT CAUGHT - a new source can be added without the NDK build seeing it'; $failed++ }
else { Write-Host "  caught (exit $($m1.Code))" }
Remove-Item $newFile -Force

Write-Host ''
Write-Host 'MUTATION 2: an exclusion that no longer excludes anything'
Write-Host '  (a reason that has quietly stopped being true)'
[System.IO.File]::WriteAllText($cmPath, $cmOrig.Replace('${RVVM_SRC}/util/cow_pgt.c', "${RVVM_SRC}/util/zz_never_built.c"))
$m2 = Invoke-ScratchGate
($m2.Text -split "`n" | Where-Object { $_ -match 'zz_never_built|stale|not present on disk|read as a reason' }) |
    ForEach-Object { Write-Host "  $_" }
if ($m2.Code -eq 0) { Write-Host '  NOT CAUGHT - CMakeLists.txt can name a file that does not exist'; $failed++ }
else { Write-Host "  caught (exit $($m2.Code))" }
[System.IO.File]::WriteAllText($cmPath, $cmOrig)

Write-Host ''
Write-Host 'MUTATION 3: a declared exclusion is deleted, so the file is undeclared'
Write-Host '  (the drift the check exists to prevent, from the other direction)'
[System.IO.File]::WriteAllText($cmPath, $cmOrig.Replace("    `${RVVM_SRC}/util/cow_pgt.c`n", ''))
$m3 = Invoke-ScratchGate
($m3.Text -split "`n" | Where-Object { $_ -match 'cow_pgt|undeclared' }) | ForEach-Object { Write-Host "  $_" }
if ($m3.Code -eq 0) { Write-Host '  NOT CAUGHT - dropping a source from CMakeLists.txt is invisible'; $failed++ }
else { Write-Host "  caught (exit $($m3.Code))" }
[System.IO.File]::WriteAllText($cmPath, $cmOrig)

Write-Host ''
Write-Host 'RESTORED: the scratch tree agrees again'
$m4 = Invoke-ScratchGate
Write-Host "  exit=$($m4.Code)"
if ($m4.Code -ne 0) { Write-Host '  the scratch tree did not come back'; $failed++ }
else { Write-Host '  clean' }

Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($failed) { Write-Host "$failed mutation(s) not caught"; exit 1 }
Write-Host 'all three mutations caught, the real tree untouched'
exit 0
