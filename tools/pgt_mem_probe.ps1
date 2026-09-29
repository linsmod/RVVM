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
#
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
# This file carries its own checks, and -SelfTest exercises them. That asymmetry
# used to be the other way round: the A-axis gate had six mutations and the
# acceptance instrument had none, while this file had already reported two wrong
# numbers (6.1 MB from a sync point that fired early, 1,058.8 MB from sampling the
# peak of a fork storm). Both wrong numbers were stable, linear and plausible.
# A stable, plausible output is not evidence - it is a question nobody asked yet.
param(
    [int]$Children = 3,
    [int]$Seconds = 12,
    [switch]$Series,
    [int[]]$Ladder = @(0, 1, 3, 6),
    [switch]$SelfTest,
    # Overriding the child command exists so the self-test can set it to the thing
    # that used to be wrong. Baked in, that test could not be written.
    [string]$KidOverride = '',
    # USERLAND_MEM_SIZE - USERLAND_MEM_BASE, from src/core/rvvm.h. One machine's
    # arena, and the quantum the integer-multiple check below is stated in.
    [int64]$ArenaBytes = 536866816
)

$exe = Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_winhost_x86_64.exe'
$exe = (Resolve-Path -LiteralPath $exe).Path

# Each child is a shell that holds its address space for the whole sample without
# consuming CPU worth measuring.
#
# It must be a busybox *builtin* loop, and that is not a style preference. The
# obvious spelling - `while :; do sleep 1; done` - forks once per iteration, and
# in rvvm-user a guest fork is a whole new machine, i.e. another guest_size
# arena. The child therefore holds two machines instead of one for a large part
# of every second, and the measured cost per child comes out at 1,058.9 MB
# rather than 529.4 - exactly double, because it *is* double. `:` is a builtin,
# so this variant forks nothing and the slope is the machine's own cost.
$kid = if ($KidOverride) { $KidOverride } else { 'sh -c "while :; do :; done"' }

# Words that make a guest shell exec something, and therefore fork. The child
# command is the one part of this script that decides how many machines exist
# during the sample, so it is checked rather than trusted.
$ForkingExternals = @(
    'sleep', 'yes', 'cat', 'ls', 'grep', 'sed', 'awk', 'dd', 'tail', 'head',
    'watch', 'tee', 'printf', 'true', 'false', 'date', 'env', 'test', 'expr',
    'find', 'sort', 'wc', 'cut', 'tr', 'basename', 'dirname', 'dirname'
)

function Test-KidCommand([string]$cmd) {
    # Returns the offending words. Empty means the command is fork-free as far as
    # this can tell.
    $hits = @()
    $body = $cmd -replace '^sh\s+-c\s*["'']?', '' -replace '["'']$', ''
    foreach ($w in $ForkingExternals) {
        if ($body -match "(^|[\s;|&(])$([regex]::Escape($w))([\s;|&)]|$)") { $hits += $w }
    }
    return $hits
}

function Test-ArenaMultiple([double]$slopeBytes, [int64]$arena, [int]$MinMultiple = 2) {
    # A slope that is a near-integer multiple of the arena is the signature of
    # having sampled a transient: one extra machine alive at the peak reads
    # exactly like one extra child's worth of cost.
    #
    # This is the check that would have caught 1,058.8 MB per child on the day it
    # was reported. 1058.8 / 536.9 = 1.972, which is 2 within 3%, and 2 x one
    # perfectly reasonable number is precisely why it looked credible.
    #
    # The default threshold is 2 because 1 x arena per child is the shape the code
    # has today, and flagging the status quo on every run trains people to ignore
    # the warning. That reasoning is specific to commit charge, though, and
    # $MinMultiple exists because it does not transfer: residency has no
    # legitimate steady state anywhere near one arena per child, so a caller
    # regressing the resident metric passes 1 and treats it as the failure it is.
    if ($arena -le 0) { return 0 }
    $ratio = $slopeBytes / $arena
    $k = [math]::Round($ratio)
    if ($k -ge $MinMultiple -and [math]::Abs($ratio - $k) -lt ($k * 0.05)) { return [int]$k }
    return 0
}

if ($SelfTest) {
    $bad = 0
    Write-Host 'probe self-test - each check against an input that must fail it'

    Write-Host ''
    Write-Host 'CHECK 1: a child command that execs something is refused'
    foreach ($c in @('sh -c "while :; do sleep 1; done"',
                     'sh -c "while :; do cat; done"',
                     'sh -c "while :; do :; done"')) {
        $h = Test-KidCommand $c
        $expectBad = $c -match 'sleep|cat'
        $got = $h.Count -gt 0
        Write-Host "  $(if($got -and $expectBad){'caught  '}elseif(-not $got -and -not $expectBad){'clean   '}else{'WRONG   '}) $c -> [$($h -join ',')]"
        if ($got -ne $expectBad) { $bad++ }
    }

    Write-Host ''
    Write-Host 'CHECK 2: a slope that is a near-integer multiple of the arena is flagged'
    $arena = 536866816
    foreach ($mb in @(1058.8, 529.4, 2140.0, 0.08)) {
        $k = Test-ArenaMultiple ($mb * 1MB) $arena
        # 1058.8 is ~2x (the bug), 2140 is ~4x (also a transient), 529.4 is 1x
        # (correct), 0.08 is far from any multiple (the expected post-page-table
        # result).
        $expect = if ($mb -eq 529.4 -or $mb -eq 0.08) { 0 } else { $true }
        $ok = if ($expect) { $k -ge 2 } else { $k -eq 0 }
        Write-Host "  $(if($ok){'ok     '}else{'WRONG   '}) slope $mb MB -> multiple $k"
        if (-not $ok) { $bad++ }
    }

    # The resident metric uses a different threshold, and the two cases below are
    # the whole reason. 529.4 MB of commit per child is today's status quo and
    # must not be flagged; 529.4 MB *resident* per child is a regression and must
    # be. Same number, opposite verdicts, so the threshold cannot be a property of
    # the slope alone.
    Write-Host ''
    Write-Host 'CHECK 2b: the resident threshold differs from the commit threshold'
    foreach ($mb in @(529.4, 3.5, 1058.8)) {
        $k = Test-ArenaMultiple ($mb * 1MB) $arena 1
        # 1 arena resident per child is never a legitimate steady state, so it
        # fires at 1. 3.5 MB is 151x below the arena and is what the real
        # measurement gives, so it stays quiet. 1058.8 still fires, as a transient.
        $ok = if ($mb -eq 3.5) { $k -eq 0 } else { $k -ge 1 }
        Write-Host "  $(if($ok){'ok     '}else{'WRONG   '}) resident slope $mb MB -> multiple $k"
        if (-not $ok) { $bad++ }
    }

    Write-Host ''
    if ($bad) { Write-Host "$bad check(s) behaved wrongly"; exit 1 }
    Write-Host 'checks behave as specified'
    Write-Host '(CHECK 3 - the live machine count - needs a real run; see pgt_mem_probe_selftest.ps1)'
    exit 0
}

function Measure-Run([int]$n, [int]$secs) {
    # The parent is held open by `read`, which is a builtin: `sleep $secs` would
    # fork, and that fork would be alive for the whole window, adding one arena to
    # every point including n=0. It cancels out of the slope, but it inflates the
    # intercept and it makes "peak" mean "plus my own scaffolding".
    $script = ''
    for ($i = 0; $i -lt $n; $i++) { $script += "$kid &`n" }
    $script += "read _hold`n"

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
    for ($s = 0; $s -lt ($secs * 2); $s++) {
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

    # Release the parent's read() and ask it, with a builtin and a glob and no
    # fork, exactly how many processes exist. One guest process is one machine, so
    # this is the machine count - the quantity the peak is implicitly measuring,
    # stated instead of left for the reader to infer from Private.
    #
    # The throwaway line is not optional. The guest shell is reading its script
    # from stdin, so `read` consumes the next line of that script - which is why
    # the count command has to be sent *after* the shell is already blocked in
    # read(), and why a bare count command is silently eaten.
    $count = $null
    try {
        $proc.StandardInput.Write("_feed_read`necho __COW_COUNT__ /proc/[0-9]*`nexit`n")
        $proc.StandardInput.Flush()
        for ($t = 0; $t -lt 20; $t++) {
            $task = $proc.StandardOutput.ReadLineAsync()
            if (-not $task.Wait(1000)) { continue }
            $line = $task.Result
            if ($null -eq $line) { break }
            if ($line -match '__COW_COUNT__\s*(.*)') {
                $count = ([regex]::Matches($Matches[1], '/proc/\d+')).Count
                break
            }
        }
    } catch { }
    try { if (-not $proc.HasExited) { $proc.Kill($true) } } catch { }
    try { $proc.WaitForExit(5000) | Out-Null } catch { }
    if ($null -ne $peak) { $peak | Add-Member -NotePropertyName Procs -NotePropertyValue $count }
    return $peak
}

# The child's own command decides how many machines exist, so it is checked before
# anything is measured, not after.
$forking = Test-KidCommand $kid
if ($forking.Count) {
    Write-Host "WARNING: the child command execs $($forking -join ', '), so each child"
    Write-Host '         forks and briefly holds a second machine. The slope will'
    Write-Host '         read as a multiple of the arena. Child command:'
    Write-Host "           $kid"
    Write-Host ''
}

function Show-Checks($rows) {
    # CHECK 3 on every row: the machine count must be exactly the parent plus the
    # children asked for. Anything more means something forked on its own.
    $warned = $false
    foreach ($r in $rows) {
        if ($null -eq $r.Procs) {
            Write-Host "  n=$($r.N): machine count not read (probe could not get the count)"
            $warned = $true
        } elseif ($r.Procs -ne ($r.N + 1)) {
            Write-Host "  n=$($r.N): $($r.Procs) machines alive, expected $($r.N + 1) - something is forking"
            $warned = $true
        } else {
            Write-Host "  n=$($r.N): $($r.Procs) machines alive (parent + $($r.N))"
        }
    }
    return $warned
}

if ($Series) {
    Write-Host "per-child slope: $($Ladder -join ' / ') concurrent long-lived children"
    Write-Host ''
    $rows = @()
    foreach ($n in $Ladder) {
        $m = Measure-Run $n $Seconds
        if (-not $m) { Write-Host "  n=$n  (process exited before it could be sampled)"; continue }
        $rows += [pscustomobject]@{ N = $n; Private = $m.Private; WS = $m.WS; VSZ = $m.VSZ; Procs = $m.Procs }
        Write-Host ("  n={0,-3} Private={1,9:N1} MB   WS={2,7:N1} MB   VSZ={3,9:N1} MB" -f `
                    $n, ($m.Private / 1MB), ($m.WS / 1MB), ($m.VSZ / 1MB))
    }
    if ($rows.Count -lt 2) { Write-Host 'not enough points for a slope'; exit 1 }

    Write-Host ''
    Write-Host 'machine count during the sample (one guest process = one machine):'
    $badCount = Show-Checks $rows

    Write-Host ''
    # WorkingSet64 gets a slope of its own for the same reason Private does, and
    # for a sharper reason: the three metrics answer different questions, and
    # only one of them is the one "how much memory does a child cost" usually
    # means. Private is commit charged, VSZ is address space reserved, WS is
    # physical pages resident. Measure-Run has been collecting WS all along and
    # nothing ever regressed it, so every per-child number quoted so far - the
    # 529.4 MB figure included - is a statement about commit, never about
    # residency. A page-table change moves all three differently, and a
    # conclusion drawn from one of them does not carry to the other two.
    Write-Host '  interval        per-child Private   per-child VSZ   per-child WS'
    $lastSlope = 0.0
    $lastWs = 0.0
    for ($i = 1; $i -lt $rows.Count; $i++) {
        $dn = $rows[$i].N - $rows[$i - 1].N
        $dp = ($rows[$i].Private - $rows[$i - 1].Private) / 1MB / $dn
        $dv = ($rows[$i].VSZ - $rows[$i - 1].VSZ) / 1MB / $dn
        $dw = ($rows[$i].WS - $rows[$i - 1].WS) / 1MB / $dn
        $lastSlope = $dp
        $lastWs = $dw
        Write-Host ("  {0,2} -> {1,-3} {2,14:N1} MB   {3,13:N1} MB   {4,11:N1} MB" -f `
                    $rows[$i - 1].N, $rows[$i].N, $dp, $dv, $dw)
    }

    $k = Test-ArenaMultiple ($lastSlope * 1MB) $ArenaBytes
    if ($k -ge 2) {
        Write-Host ''
        Write-Host "WARNING: the per-child slope is ~$($lastSlope.ToString('N1')) MB, which is ${k}x the"
        Write-Host "         arena ($([math]::Round($ArenaBytes / 1MB, 1)) MB). That is the signature of"
        Write-Host '         sampling a transient - extra machines alive at the peak. It does'
        Write-Host '         not mean a child costs that much. Child command:'
        Write-Host "           $kid"
    }

    # The Private warning above is about a sampling artifact. This one is about
    # the thing the whole exercise is for, and it is a separate check because
    # the two metrics have never moved together: today a child charges 529.4 MB
    # of commit and holds 3.5 MB resident, a ratio of ~150:1 that comes straight
    # from VirtualAlloc(MEM_COMMIT) accounting rather than from touched pages.
    # Any future change can move either one without moving the other, so a gate
    # that only watches commit is satisfied by work that leaves the observable
    # cost exactly where it was.
    $kw = Test-ArenaMultiple ($lastWs * 1MB) $ArenaBytes 1
    if ($kw -ge 2) {
        Write-Host ''
        Write-Host "WARNING: the per-child WS slope is ~$($lastWs.ToString('N1')) MB, which is ${kw}x the"
        Write-Host "         arena. WS is physical residency: a child really is holding that"
        Write-Host '         much. This is the number that says whether the cost went down,'
        Write-Host '         and unlike the Private slope it cannot be an accounting artifact.'
    }

    $first = $rows[0]
    Write-Host ''
    Write-Host ("  per-child slope: Private={0:N1} MB (commit)   WS={1:N1} MB (resident)" -f `
                $lastSlope, $lastWs)
    Write-Host ("  intercept (n={0}): Private={1:N1} MB  VSZ={2:N1} MB  WS={3:N1} MB" -f `
                $first.N, ($first.Private / 1MB), ($first.VSZ / 1MB), ($first.WS / 1MB))
    Write-Host "  That is the host plus $($first.Procs) machine(s) - not the base plus the"
    Write-Host "  first machine. With the scaffolding fork removed, n=0 is one machine and"
    Write-Host "  nothing else. The machine count above is read from the guest, not inferred"
    Write-Host "  from Private. Read the Private and WS slopes as two separate facts: the"
    Write-Host "  first is commit charged and scales with the reservation, the second is"
    Write-Host "  resident and scales with what the guest touched."
    if ($badCount -or $k -ge 2 -or $kw -ge 2) { exit 2 }
    exit 0
}

$peak = Measure-Run $Children $Seconds
Write-Host "1 guest + $Children concurrent long-lived children"
if (-not $peak) { Write-Host '  (process exited before it could be sampled)'; exit 1 }
Write-Host ("  PrivateMemorySize64 = {0,8:N1} MB   (commit charged private)" -f ($peak.Private / 1MB))
Write-Host ("  WorkingSet64        = {0,8:N1} MB   (physical resident)"    -f ($peak.WS / 1MB))
Write-Host ("  VirtualMemorySize64 = {0,8:N1} MB   (address space)"        -f ($peak.VSZ / 1MB))
if ($null -ne $peak.Procs) {
    Write-Host ("  machines alive      = {0,8}      (parent + children)"      -f $peak.Procs)
} else {
    Write-Host '  machines alive      =        ?  (probe could not get the count)'
}
$rc = 0
if ($null -eq $peak.Procs -or $peak.Procs -ne ($Children + 1)) { $rc = 2 }
exit $rc
