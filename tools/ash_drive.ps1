#Requires -Version 5.1
<#
    The session driving layer shared by the e2e drivers.

    ash_sock.ps1 answers "where is the core, and how do I connect to it"; this
    file answers "how do I talk to a session once I am connected". It is the
    part the jobctl driver grew (output-event driven, no fixed sleeps) pulled
    out of that one script so every driver gets the same guarantees:

      accumulate   every byte a session sends goes into one buffer, and a wait
                   matches against that whole text - never against a window
                   that starts at "whatever arrived since the last call", which
                   is how a prompt landing in the same chunk as a ^C echo used
                   to be swallowed and read as a timeout.
      wait, don't
      sleep        Expect-Pattern / -Newlines / -Offset / -Quiet. A command
                   that emits nothing (a bare `sleep`, a blocking `wait`) has
                   no output to wait for, so Expect-Quiet settles on silence
                   instead of guessing a duration.
      queue then
      replay       writes and waits are enqueued in order and replayed by
                   Invoke-Session, so a scenario reads top-to-bottom and the
                   reader runs interleaved with the writer.

    Two shapes, one engine:

      one scenario, one session (jobctl)
          Reset-Steps; Step/Key/Expect-*; $out = Invoke-Session
          - Invoke drains to EOF and closes, the shell having exited.

      one long-lived session, one check at a time (session_e2e)
          Reset-Steps $A; Step -On $A ...; Expect-* -On $A ...;
          $out = Invoke-Session $A -KeepOpen
          - the shell stays up, the next check goes on typing into it; each
            Invoke still starts from its own buffer, so "wait for this" means
            "wait for this from here".

    With no -On, the actions act on the most recently opened session, which is
    what a single-session driver wants. -On <session> targets a specific one.

    The buffer is matched twice: once raw, once with CSI sequences stripped.
    The shell emits ESC[6n right after a prompt, so a pattern anchored at a
    line start ('(?m)^\s*18\s*$') would otherwise miss its own line.

    Check is defined here as a plain printer: a driver that dot-sources this
    file and then defines its own Check (counters, timestamps, whatever) has
    the last word - the waits below call Check by name at run time.

    Dot-source this file; it pulls in ash_sock.ps1 itself.
#>

. (Join-Path $PSScriptRoot 'ash_sock.ps1')

# --- one session ------------------------------------------------------------
# Connects, and - if @Frame is given - sends that control frame at once (the
# frame is what makes the server start the shell immediately instead of after
# its own grace delay). The session becomes the default target for the action
# helpers; -NoCurrent leaves the previous default alone.
function New-AshSession {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][int]$Port,
        [string]$Frame,
        [switch]$NoCurrent
    )
    $sess = Connect-AshSession -Exe $Exe -Port $Port
    # Bounds the read when Poll claims readability and the bytes are not there
    # yet (see Read-AshChunk). Long enough for a real read on loopback, short
    # enough that the "readable but empty" case is a millisecond, not a hang.
    try { $sess.Socket.ReceiveTimeout = 250 } catch { }
    $d = [pscustomobject]@{
        Path      = $sess.Path
        Sock      = $sess.Socket
        Stream    = $sess.Stream
        Buf       = (New-Object System.Text.StringBuilder)
        Decoder   = [Text.Encoding]::UTF8.GetDecoder()
        Actions   = @()
        ReadEnded = $false
        EmptySince = $null
        ReadEndWhy = ''
        Closed    = $false
        Rate      = @{}
        SampleSw  = [Diagnostics.Stopwatch]::StartNew()
        Text      = ''
    }
    if ($Frame) { Send-AshFrame $d $Frame }
    if (-not $NoCurrent) { $script:cur = $d }
    return $d
}

function Close-AshSession($d) {
    try { $d.Stream.Close() } catch { }
    try { $d.Sock.Close() } catch { }
}

function Send-AshFrame($d, [string]$body) {
    Write-AshBytes $d ([Text.Encoding]::ASCII.GetBytes(
        ([string][char]27) + ']999;' + $body + ([string][char]7)))
}

# --- bytes ------------------------------------------------------------------
function Write-AshBytes($d, [byte[]]$b) {
    if ($d.ReadEnded) { return }
    try {
        $sent = 0
        while ($sent -lt $b.Length) {
            $sent += $d.Sock.Send($b, $sent, $b.Length - $sent,
                                  [System.Net.Sockets.SocketFlags]::None)
        }
    } catch {
        $d.ReadEnded = $true
    }
}

# One bounded non-blocking read. Poll says "something is waiting" without
# throwing on timeout; the read itself is what tells us what.
#
# Socket.Available is not consulted, and that is the point. On this AF_UNIX
# socket it reports 0 on a stream that has bytes queued and the peer still
# attached, so an earlier version that read "Poll says readable && Available==0"
# as the end of the stream declared EOF in the middle of a live session. That is
# self-sealing and silent: ReadEnded is sticky and Write-AshBytes refuses to
# write once it is set, so the session stopped being driven at that instant -
# every later step typed nothing, read nothing and ran out its wait, with a
# transcript that looked like a guest which had gone quiet.
#
# So: ask Poll, then Receive, and let the byte count decide. 0 is a real close.
# A receive that times out means the socket claimed readability and then had
# nothing, which is not a close - ReceiveTimeout is set short so that case costs
# a millisecond rather than a hang.
function Read-AshChunk($d, [int]$timeoutMs) {
    if ($d.ReadEnded) { return $false }
    if (-not $d.Sock.Poll($timeoutMs * 1000, [System.Net.Sockets.SelectMode]::SelectRead)) {
        return $false
    }
    $tmp = New-Object byte[] 4096
    $n = 0
    try {
        $n = $d.Sock.Receive($tmp, 0, $tmp.Length, [System.Net.Sockets.SocketFlags]::None)
    } catch {
        $code = $_.Exception.SocketErrorCode
        if ($code -eq [System.Net.Sockets.SocketError]::TimedOut -or
            $code -eq [System.Net.Sockets.SocketError]::WouldBlock) {
            return $false        # readable a moment ago, nothing yet: not EOF
        }
        $d.ReadEnded = $true     # the socket itself is finished
        $d.ReadEndWhy = "recv $code : $($_.Exception.Message)"
        return $false
    }
    if ($n -le 0) {
        $d.ReadEnded = $true     # 0 bytes == peer closed
        $d.ReadEndWhy = "recv 0 bytes (peer closed)"
        return $false
    }
    $chars = New-Object char[] 8192
    # Incremental decoder: a multi-byte char split across two chunks is not
    # two replacement chars.
    $c = $d.Decoder.GetChars($tmp, 0, $n, $chars, 0)
    if ($c -gt 0) { [void]$d.Buf.Append($chars, 0, $c) }
    Add-AshSample $d $c
    return $true
}

function Add-AshSample($d, [int]$n) {
    if ($n -le 0 -or $null -eq $d.SampleSw) { return }
    $sec = [int]($d.SampleSw.ElapsedMilliseconds / 1000)
    if (-not $d.Rate.ContainsKey($sec)) { $d.Rate[$sec] = 0 }
    $d.Rate[$sec] = [int]$d.Rate[$sec] + $n
}

# --- the action queue -------------------------------------------------------
function Reset-Steps { param($On = $script:cur) $On.Actions = @() }

function Add-AshAction($d, $a) { $d.Actions += , $a }

# A write action, preceded by a settle. @ms is the silence before the burst:
# a bare `sleep` emits nothing, so those steps wait on quiet rather than on a
# pattern.
function Step {
    param([int]$ms, [string]$text, $On = $script:cur)
    Add-AshAction $On @('sleep', $ms)
    Add-AshAction $On @('txt', $text)
}

function Key {
    param([int]$ms, [int]$byte, $On = $script:cur)
    Add-AshAction $On @('sleep', $ms)
    Add-AshAction $On @('key', $byte)
}

# Wait primitives (enqueued, executed in order by Invoke-Session).
function Expect-Pattern { param([string]$regex, [int]$timeout = 5000, $On = $script:cur) Add-AshAction $On @('wait', 'pattern', $regex, $timeout) }
function Expect-Newlines { param([int]$n, [int]$timeout = 5000, $On = $script:cur) Add-AshAction $On @('wait', 'newlines', $n, $timeout) }
function Expect-Offset   { param([int]$n, [int]$timeout = 5000, $On = $script:cur) Add-AshAction $On @('wait', 'offset', $n, $timeout) }
function Expect-Quiet    { param([int]$ms = 300, [int]$timeout = 5000, $On = $script:cur) Add-AshAction $On @('wait', 'quiet', $ms, $timeout) }
# Wait until the peer closes the socket (the shell exited on `exit`).
function Expect-Closed   { param([int]$timeout = 8000, $On = $script:cur) Add-AshAction $On @('wait', 'closed', $timeout) }

# --- assertions over the buffer ---------------------------------------------
function Count-AshNewlines($d) { ([regex]::Matches($d.Buf.ToString(), "`n")).Count }

# Match against everything received so far, raw and ANSI-stripped. The second
# chance exists because the shell prints ESC[6n right after a prompt, which
# would otherwise break a line-anchored pattern.
function Match-AshPattern($d, [string]$regex) {
    $text  = $d.Buf.ToString()
    $plain = $text -replace "`e\[[0-9;?]*[A-Za-z]", ''
    $m = [regex]::Match($text, $regex)
    if ($m.Success) { return $m }
    return [regex]::Match($plain, $regex)
}

function Assert-AshPattern($d, [string]$regex, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $m = Match-AshPattern $d $regex
        if ($m.Success) { $script:lastHit = $m; return $true }
        if ($d.ReadEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-AshChunk $d 100 | Out-Null
    }
    Check $false "wait: pattern '$regex' (timeout ${timeoutMs}ms)"
    return $false
}

function Assert-AshNewlines($d, [int]$n, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ((Count-AshNewlines $d) -ge $n) { return $true }
        if ($d.ReadEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-AshChunk $d 100 | Out-Null
    }
    Check $false "wait: $n newline(s) (timeout ${timeoutMs}ms)"
    return $false
}

function Assert-AshOffset($d, [int]$n, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($d.Buf.Length -ge $n) { return $true }
        if ($d.ReadEnded) { break }
        if ($t.ElapsedMilliseconds -ge $timeoutMs) { break }
        Read-AshChunk $d 100 | Out-Null
    }
    Check $false "wait: offset >= $n (timeout ${timeoutMs}ms)"
    return $false
}

# Quiet: return once no output has arrived for @quietMs. The only valid signal
# for a command that emits nothing; also what makes the transition adaptive.
function Assert-AshQuiet($d, [int]$quietMs, [int]$timeoutMs) {
    $quiet = [Diagnostics.Stopwatch]::StartNew()
    $total = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($d.ReadEnded) { return $true }
        if ($quiet.ElapsedMilliseconds -ge $quietMs) { return $true }
        if ($total.ElapsedMilliseconds -ge $timeoutMs) {
            Check $false "wait: quiet for ${quietMs}ms (timeout ${timeoutMs}ms)"
            return $false
        }
        $slice = [Math]::Min(50, [Math]::Max(10, $quietMs - [int]$quiet.ElapsedMilliseconds))
        if (Read-AshChunk $d $slice) { $quiet.Restart() }
    }
}

# Wait until the peer closes, or the timeout. Records the result on the session
# (Closed) so the caller can assert on it without another drain.
function Assert-AshClosed($d, [int]$timeoutMs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (-not $d.ReadEnded -and $t.ElapsedMilliseconds -lt $timeoutMs) {
        Read-AshChunk $d 200 | Out-Null
    }
    $d.Closed = $d.ReadEnded
    if (-not $d.Closed) {
        Check $false "wait: socket closed (timeout ${timeoutMs}ms)"
    }
    return $d.Closed
}

# --- replay -----------------------------------------------------------------
# The whole scenario is one session over the core's AF_UNIX endpoint. The
# actions are replayed in order: a write types a burst, a wait blocks on the
# session output. Reading is incremental and interleaved with writing, so the
# scenario reacts to the shell reaching a position - no fixed schedule, and no
# tail loss. With -KeepOpen the shell is left running (a long-lived session,
# one check at a time); otherwise the session drains to EOF and closes.
function Invoke-Session {
    param($On = $script:cur, [switch]$KeepOpen)
    $d = $On
    $d.Buf       = New-Object System.Text.StringBuilder
    $d.Decoder   = [Text.Encoding]::UTF8.GetDecoder()
    $d.ReadEnded = $false
    $d.EmptySince = $null
    $d.ReadEndWhy = ''
    $d.Closed    = $false
    $d.Rate      = @{}
    $d.SampleSw  = [Diagnostics.Stopwatch]::StartNew()

    foreach ($a in $d.Actions) {
        if ($d.ReadEnded) { break }
        switch ($a[0]) {
            'sleep' { if ($a[1]) { Start-Sleep -Milliseconds $a[1] } }
            'txt'   { Write-AshBytes $d ([Text.Encoding]::ASCII.GetBytes($a[1])) }
            'key'   { Write-AshBytes $d ([byte[]]@([byte]$a[1])) }
            'wait'  {
                switch ($a[1]) {
                    'pattern'  { Assert-AshPattern  $d $a[2] $a[3] | Out-Null }
                    'newlines' { Assert-AshNewlines $d $a[2] $a[3] | Out-Null }
                    'offset'   { Assert-AshOffset   $d $a[2] $a[3] | Out-Null }
                    'quiet'    { Assert-AshQuiet    $d $a[2] $a[3] | Out-Null }
                    'closed'   { Assert-AshClosed   $d $a[2] | Out-Null }
                }
            }
        }
    }

    if (-not $KeepOpen) {
        # The shell exits on `exit`; that ends the read at EOF. A scenario that
        # forgets to exit (or a command that hangs) is bounded here so a bug is
        # a failure, not an infinite wait.
        $drain = [Diagnostics.Stopwatch]::StartNew()
        while (-not $d.ReadEnded -and $drain.ElapsedMilliseconds -lt 8000) {
            Read-AshChunk $d 200 | Out-Null
        }
        Close-AshSession $d
    }

    $text = $d.Buf.ToString()
    $d.Text = $text
    $script:lastOut = $text
    $d.Actions = @()
    return $text
}

# --- diagnostics ------------------------------------------------------------
function Get-NewlineOffsets([string]$text) {
    $offs = @()
    for ($i = 0; $i -lt $text.Length; $i++) { if ($text[$i] -eq "`n") { $offs += $i } }
    return , $offs
}

function Show-SessionRate($d) {
    "--- output rate (chars/second) ---"
    $d.Rate.GetEnumerator() | Sort-Object { [int]$_.Key } | ForEach-Object {
        "  t=$($_.Key)s  $($_.Value) chars"
    }
}

# The waits call Check by name; a driver that defines its own (counters,
# timestamps) after dot-sourcing this file gets the last word. This default is
# just a plain printer so the engine runs on its own.
function Check([bool]$ok, [string]$what) {
    if ($ok) { "[ok]   $what" } else { "[FAIL] $what" }
}
