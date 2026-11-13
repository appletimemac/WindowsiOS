# SPDX-License-Identifier: GPL-2.0-or-later
# WindowsiOS benchmark, run inside the guest (PowerShell 5.1, no modules, ASCII only).
#
#   powershell -ep bypass -File winios_bench.ps1 [-Quick] [-Rounds 3] [-IdleSec 30]
#
# Writes "name ms" lines to C:\Users\Public\winios_bench.txt and PUTs that file to the host share
# (http://10.0.2.4/winios_bench_result.txt) at the start and the end of every section, so the host can
# line the sections up with its perf CSV. Lines starting with '#' are comments: the guest clock at each
# section boundary, the poll counts of the launches. The last line is "done".
#
# Sections: idle, launch.<app>.r<N> (notepad, mspaint, charmap, msinfo32, explorer, settings),
# compute.loop, compute.pipeline, jitter.p50, jitter.p95, jitter.max (overshoot of Start-Sleep -Milliseconds 1).
# A launch that shows no window within 60 s is written as "launch.<app>.r<N> timeout".
param(
    [switch]$Quick,
    [int]$Rounds = 3,
    [int]$IdleSec = 30
)

$ErrorActionPreference = 'Continue'
$inv = [Globalization.CultureInfo]::InvariantCulture
$out = 'C:\Users\Public\winios_bench.txt'
$resultName = 'winios_bench_result.txt'
$res = New-Object System.Collections.Generic.List[string]

$loopN = 2000000
$pipeN = 150000
$jitterN = 2000
if ($Quick) {
    $Rounds = 1
    $IdleSec = 3
    $loopN = 200000
    $pipeN = 15000
    $jitterN = 200
}

function Clock { (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fff', $inv) }

function Publish {
    $res | Set-Content -Encoding ascii $out
    & curl.exe -s -m 20 -T $out ('http://10.0.2.4/' + $resultName) | Out-Null
}

function Add-Result([string]$name, [double]$ms) {
    $res.Add($name + ' ' + $ms.ToString('F0', $inv))
}

function Section([string]$name, [scriptblock]$body) {
    $res.Add('# begin ' + $name + ' ' + (Clock))
    Publish
    try { & $body }
    catch { $res.Add($name + ' error ' + ($_.Exception.Message -replace '[\r\n]+', ' ')) }
    $res.Add('# end ' + $name + ' ' + (Clock))
    Publish
}

function Pids([string]$procName) {
    $ids = @()
    foreach ($q in [Diagnostics.Process]::GetProcessesByName($procName)) { $ids += $q.Id; $q.Dispose() }
    return $ids
}

# True once a process named $procName that was not running before has a window.
function Has-Window([string]$procName, $before) {
    foreach ($q in [Diagnostics.Process]::GetProcessesByName($procName)) {
        $hit = ($before -notcontains $q.Id) -and ($q.MainWindowHandle -ne [IntPtr]::Zero)
        $q.Dispose()
        if ($hit) { return $true }
    }
    return $false
}

function Close-New([string]$procName, $before) {
    foreach ($q in [Diagnostics.Process]::GetProcessesByName($procName)) {
        if ($before -notcontains $q.Id) { [void]$q.CloseMainWindow() }
        $q.Dispose()
    }
    $end = [Diagnostics.Stopwatch]::StartNew()
    while ($end.ElapsedMilliseconds -lt 3000) {
        if (@(Pids $procName | Where-Object { $before -notcontains $_ }).Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    }
    foreach ($q in [Diagnostics.Process]::GetProcessesByName($procName)) {
        if ($before -notcontains $q.Id) { try { $q.Kill() } catch {} }
        $q.Dispose()
    }
}

# Times Start-Process until the app has a window. $kind: 'proc' (process $procName gets a window),
# 'explorer' (a File Explorer window titled Windows) or 'settings' (SystemSettings has a window, or the
# frame host shows a window titled Settings).
function Launch([string]$label, [string]$kind, [string]$file, [string]$procName) {
    for ($r = 1; $r -le $Rounds; $r++) {
        $name = 'launch.' + $label + '.r' + $r
        $before = Pids $procName
        $polls = 0
        $ok = $false
        $sw = [Diagnostics.Stopwatch]::StartNew()
        if ($kind -eq 'explorer') {
            Start-Process explorer.exe -ArgumentList 'C:\Windows'
            while ($sw.ElapsedMilliseconds -lt 60000) {
                $polls++
                foreach ($w in $script:shell.Windows()) {
                    try { if ($w.LocationName -eq 'Windows') { $ok = $true } } catch {}
                }
                if ($ok) { break }
                Start-Sleep -Milliseconds 10
            }
        } else {
            Start-Process $file
            while ($sw.ElapsedMilliseconds -lt 60000) {
                $polls++
                if (Has-Window $procName $before) { $ok = $true; break }
                if ($kind -eq 'settings') {
                    foreach ($q in [Diagnostics.Process]::GetProcessesByName('ApplicationFrameHost')) {
                        if ($q.MainWindowTitle -eq 'Settings') { $ok = $true }
                        $q.Dispose()
                    }
                    if ($ok) { break }
                }
                Start-Sleep -Milliseconds 10
            }
        }
        $ms = $sw.Elapsed.TotalMilliseconds
        if ($ok) { Add-Result $name $ms } else { $res.Add($name + ' timeout') }
        $res.Add('# polls ' + $name + ' ' + $polls)
        if ($kind -eq 'explorer') {
            foreach ($w in $script:shell.Windows()) {
                try { if ($w.LocationName -eq 'Windows') { $w.Quit() } } catch {}
            }
            Start-Sleep -Milliseconds 1500
        } else {
            Close-New $procName $before
            if ($kind -eq 'settings') {
                foreach ($q in [Diagnostics.Process]::GetProcessesByName('ApplicationFrameHost')) {
                    if ($q.MainWindowTitle -eq 'Settings') { try { $q.Kill() } catch {} }
                    $q.Dispose()
                }
            }
        }
        Start-Sleep -Milliseconds 1500
    }
}

$res.Add('# winios_bench quick=' + $Quick + ' rounds=' + $Rounds + ' idle_s=' + $IdleSec +
         ' cpus=' + $env:NUMBER_OF_PROCESSORS + ' ps=' + $PSVersionTable.PSVersion)
$res.Add('# start ' + (Clock))

Section 'idle' {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $IdleSec
    Add-Result 'idle' $sw.Elapsed.TotalMilliseconds
}

$script:shell = New-Object -ComObject Shell.Application

Section 'launch' {
    Launch 'notepad' 'proc' 'notepad.exe' 'notepad'
    Launch 'mspaint' 'proc' 'mspaint.exe' 'mspaint'
    Launch 'charmap' 'proc' 'charmap.exe' 'charmap'
    Launch 'msinfo32' 'proc' 'msinfo32.exe' 'msinfo32'
    Launch 'explorer' 'explorer' 'explorer.exe' 'explorer'
    Launch 'settings' 'settings' 'ms-settings:' 'SystemSettings'
}

Section 'compute' {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $s = 0
    foreach ($i in 1..$loopN) { $s += $i }
    Add-Result 'compute.loop' $sw.Elapsed.TotalMilliseconds
    $sw.Restart()
    1..$pipeN | ForEach-Object { $_ * 2 } | Measure-Object -Sum | Out-Null
    Add-Result 'compute.pipeline' $sw.Elapsed.TotalMilliseconds
}

Section 'jitter' {
    $over = New-Object 'double[]' $jitterN
    $sw = [Diagnostics.Stopwatch]::new()
    for ($i = 0; $i -lt $jitterN; $i++) {
        $sw.Restart()
        Start-Sleep -Milliseconds 1
        $over[$i] = $sw.Elapsed.TotalMilliseconds - 1.0
    }
    [Array]::Sort($over)
    $p50 = $over[[int][Math]::Floor($jitterN * 0.50)]
    $p95 = $over[[int][Math]::Floor($jitterN * 0.95)]
    $res.Add('jitter.p50 ' + $p50.ToString('F3', $inv))
    $res.Add('jitter.p95 ' + $p95.ToString('F3', $inv))
    $res.Add('jitter.max ' + $over[$jitterN - 1].ToString('F3', $inv))
}

$res.Add('# finish ' + (Clock))
$res.Add('done')
Publish
