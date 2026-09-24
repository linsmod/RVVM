#Requires -Version 5.1
# Temporary probe (two clients + fd trace), deleted once the collision is fixed.
param([int]$Port = 7900)

$exe = Join-Path $PSScriptRoot '..\release.windows.x86_64\rvvm_ash_x86_64.exe'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path -LiteralPath $exe).Path
$psi.WorkingDirectory = [IO.Path]::GetDirectoryName($psi.FileName)
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.EnvironmentVariables['RVVM_ASH_SHELL'] = 'guest-assets\vpsessiond.exe'
$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()

for ($i = 0; $i -lt 150; $i++) {
    try { $x = New-Object System.Net.Sockets.TcpClient; $x.Connect('127.0.0.1', $Port); $x.Close(); break }
    catch { Start-Sleep -Milliseconds 100 }
}

function Conn() {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $Port)
    $c.NoDelay = $true
    return $c
}
function Send($c, [string]$s) {
    $b = [Text.Encoding]::UTF8.GetBytes($s)
    $c.GetStream().Write($b, 0, $b.Length); $c.GetStream().Flush()
}
function Dump($c, [int]$ms) {
    $deadline = [Environment]::TickCount64 + $ms
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 4096
    while ([Environment]::TickCount64 -lt $deadline) {
        if ($c.Available -gt 0) {
            $n = $c.GetStream().Read($buf, 0, $buf.Length)
            if ($n -le 0) { [void]$sb.Append('<EOF>'); break }
            [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $n))
        } else { Start-Sleep -Milliseconds 20 }
    }
    return $sb.ToString()
}

$A = Conn
Send $A "echo A-ONE`n"
"== A one: " + (Dump $A 1500)

$B = Conn
Send $B "echo B-ONE`n"
"== B one: " + (Dump $B 1500)

Send $A "echo A-TWO`n"
"== A two: " + (Dump $A 1500)

try { $p.Kill() } catch { }
$p.WaitForExit(5000) | Out-Null
"--- console ---"
$outTask.Result
"--- stderr ---"
$errTask.Result
