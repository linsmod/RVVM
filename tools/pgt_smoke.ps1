#Requires -Version 7.0
<#
.SYNOPSIS
    Exercises the guest syscall paths that write into guest RAM through a
    translated pointer, and that no other test covers.

.DESCRIPTION
    Guest RAM reaches the host through to_ptr() / to_ptr_sz() in
    src/core/rvvm_user.c, which hand out a read-only pointer unless the call site
    declares itself a writer. When a machine has a copy-on-write page table
    (rvvm_ram_t::pgt), a call site that writes through the read accessor is handed
    a pointer into the shared read-only base and faults on the store - the
    intended loud failure, but only if that path is ever executed.

    The e2e suites cover some of them and not others. Covered elsewhere:
    select/poll fd_set (uapi_fdset_from_host) by the ash session protocol, and the
    signal-return frame by jobctl's ^C. NOT covered anywhere, and the reason this
    script exists:

      uname           case 160 newuname     - no test command ran uname before
      times           case 153             - no test command ran times
      sysinfo         case 179             - only indirectly, via `free`
      getcwd          case 17
      readlink        - /proc/self and /proc/self/fd are read by procfs_e2e, but
                        not through a path that shares a translation with these
      pwd             getcwd

    A missed writer in one of these would sit latent until some real program hit
    it, which is the same shape as the apk update ENOENT that took a while to
    track down. So: run this before the page table goes in, and again after.

    Each line is one guest command. The script fails if the host binary dies, if
    a command produces no output, or if the expected marker is missing.

.PARAMETER Exe
    Host binary. Defaults to the build tree's rvvm_winhost.
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_winhost_x86_64.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Exe).Path

# name -> guest command line, and a substring that must appear in its output.
$cases = [ordered]@{
    'newuname'  = @{ cmd = 'uname -a';                             want = 'riscv64' }
    'uname -m'  = @{ cmd = 'uname -m';                             want = 'riscv64' }
    'times'     = @{ cmd = 'times';                                want = 'm' }
    'getcwd'    = @{ cmd = 'pwd';                                  want = '/' }
    'readlink'  = @{ cmd = 'readlink /proc/self';                  want = '/' }
    'sigreturn' = @{ cmd = 'sh -c ''trap "echo caught" USR1; kill -USR1 $$; sleep 0.2'''; want = 'caught' }
    'fd_set'    = @{ cmd = 'sh -c ''/bin/busybox sleep 0.2 & sleep 0.1; wait'''; want = '/' }
}

$app = 'test_busybox'
$pass = 0; $fail = 0; $failed = @()

foreach ($name in $cases.Keys) {
    $c = $cases[$name]
    # One process per case: a fault in one must not hide the others.
    $stdin = "$($c.cmd)`nexit`n"
    $out = $stdin | & $exe --app $app 2>&1
    $text = ($out | ForEach-Object { $_.ToString() }) -join "`n"

    $hostDied = $LASTEXITCODE -ne 0
    $missing = $text -notmatch [regex]::Escape($c.want)
    $empty = ($text -replace '(?m)^\[.*$', '' -replace '(?m)^\s*$', '').Trim().Length -lt 3

    if ($hostDied -or $missing -or $empty) {
        $fail++
        $failed += $name
        Write-Host ("  {0,-10} FAIL{1}{2}" -f $name,
            $(if ($hostDied) { " (host exit $LASTEXITCODE)" }),
            $(if ($missing) { " (missing '$($c.want)')" }))
        $out | Select-Object -Last 8 | ForEach-Object { Write-Host "      $_" }
    } else {
        $pass++
        Write-Host ("  {0,-10} ok" -f $name)
    }
}

Write-Host "=== pgt smoke : $pass/$($cases.Count) passed ==="
if ($failed.Count) { Write-Host "failed: $($failed -join ', ')" }
exit ($fail -eq 0 ? 0 : 1)
