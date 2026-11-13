<#
.SYNOPSIS
  Read-only CPU diagnostics for the WindowsiOS Windows 11 ARM64 guest.

.DESCRIPTION
  Shipped on the WINIOSTOOLS disk (run it through "Run WindowsiOS Diagnostics.cmd", or directly).
  Answers "what does the guest spend its CPU on while nobody touches it": post-update servicing,
  x64 processes under Windows' own emulation, animations, background apps, scheduled tasks, a 1 ms timer
  request, our own agent. It changes no service, task, registry value or app. The only writes are the report
  file and the temporary powercfg /energy trace.

  The report goes to C:\ProgramData\WindowsiOS\diag-<yyyyMMdd-HHmmss>.txt and is PUT to the host share
  (http://10.0.2.4/, the WebDAV server in host/share.c) so it shows up in the share folder on the host.
  Elevation adds the process-start trace and the powercfg /energy timer report; without it those say so.

.PARAMETER Seconds
  Length of the CPU sampling window (default 60), sampled every 5 seconds.

.PARAMETER Quick
  A 10 second window and no powercfg /energy. For testing.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 60,
    [switch]$Quick
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

if ($Quick -and -not $PSBoundParameters.ContainsKey('Seconds')) { $Seconds = 10 }
if ($Seconds -lt 2) { $Seconds = 2 }
$interval = 5
if ($Seconds -lt $interval) { $interval = $Seconds }
$sampleCount = [int][Math]::Ceiling($Seconds / $interval)

$started = Get-Date
$stamp = $started.ToString('yyyyMMdd-HHmmss')
$reportName = "diag-$stamp.txt"

$isAdmin = $false
try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { }

$sections = New-Object System.Collections.Generic.List[object]
$nativeOk = $false
$nativeErr = ''

# Results the sections hand to each other and to the summary.
$script:cpuCount = 0
$script:avgByName = @{}
$script:topRows = @()
$script:busyPct = $null
$script:svcSeen = @()
$script:emuBusy = @()
$script:timerHist = $null
$script:sysStats = @()
$script:startTotal = $null
$script:startTop = @()
$script:startApprox = $false
$script:energyHtml = $null

function Add-Section {
    param([string]$Title, [scriptblock]$Body)
    Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Title)
    $text = New-Object System.Collections.Generic.List[string]
    try {
        foreach ($x in @(& $Body)) {
            if ($null -eq $x) { continue }
            if ($x -is [string]) { $text.Add($x) }
            else { $text.Add(([string]($x | Out-String -Width 220)).TrimEnd()) }
        }
    } catch {
        $text.Add('ERROR in this section, the rest of the report is unaffected: ' + $_.Exception.Message)
    }
    $sections.Add([pscustomobject]@{ Title = $Title; Text = ($text -join "`r`n") })
}

function ToTable($rows) {
    if (-not $rows) { return '(none)' }
    ([string]($rows | Format-Table -AutoSize | Out-String -Width 220)).TrimEnd()
}

function Cut($s, [int]$n) {
    $t = [string]$s
    if ($t.Length -gt $n) { return $t.Substring(0, $n - 3) + '...' }
    return $t
}

function Round1($v) { [Math]::Round([double]$v, 1) }

function PidKey($p) { return ([string]$p.ProcessId + '|' + [string]$p.CreationDate.Ticks) }

function HM($d) { if ($d) { return ([datetime]$d).ToString('HH:mm:ss') } return '' }

# P/Invoke helpers: process architecture, system timer resolution, DPI, animation setting.
try {
    if (-not ('WiNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class WiNative {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool IsWow64Process2(IntPtr h, out ushort processMachine, out ushort nativeMachine);
    [DllImport("ntdll.dll")]
    static extern int NtQueryTimerResolution(out uint maximum, out uint minimum, out uint current);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool SystemParametersInfo(uint action, uint param, ref int value, uint winIni);

    // { 1 = ok, processMachine, nativeMachine, 0 } or { 0, 0, 0, last error }
    public static int[] Wow64(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return new int[] { 0, 0, 0, Marshal.GetLastWin32Error() };
        try {
            ushort pm, nm;
            if (!IsWow64Process2(h, out pm, out nm)) return new int[] { 0, 0, 0, Marshal.GetLastWin32Error() };
            return new int[] { 1, pm, nm, 0 };
        } finally {
            CloseHandle(h);
        }
    }

    // The Machine field of an image's PE header (0xAA64 ARM64, 0x8664 x64, 0x14C x86, 0x1C4 ARM32), -1 when unreadable.
    // For processes IsWow64Process2 cannot open without elevation.
    public static int PeMachine(string path) {
        try {
            using (System.IO.FileStream fs = new System.IO.FileStream(path, System.IO.FileMode.Open,
                System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite | System.IO.FileShare.Delete)) {
                byte[] b = new byte[64];
                if (fs.Read(b, 0, 64) < 64) return -1;
                int pe = BitConverter.ToInt32(b, 60);
                if (pe < 64 || pe > 4096) return -1;
                fs.Seek(pe, System.IO.SeekOrigin.Begin);
                byte[] h = new byte[6];
                if (fs.Read(h, 0, 6) < 6) return -1;
                if (h[0] != 0x50 || h[1] != 0x45) return -1;
                return BitConverter.ToUInt16(h, 4);
            }
        } catch (Exception) {
            return -1;
        }
    }

    // SPI_GETCLIENTAREAANIMATION: 1 on, 0 off, -1 unknown
    public static int ClientAreaAnimation() {
        int v = 0;
        if (!SystemParametersInfo(0x1042, 0, ref v, 0)) return -1;
        return v;
    }

    // The system-wide timer resolution is the finest period any process has asked for. Poll it.
    static readonly Dictionary<uint, int> hist = new Dictionary<uint, int>();
    static uint coarsest, finest;
    static volatile bool running;
    static Thread poller;
    static int periodMs;

    static void Poll() {
        while (running) {
            uint mx, mn, cur;
            if (NtQueryTimerResolution(out mx, out mn, out cur) == 0) {
                lock (hist) {
                    coarsest = mx; finest = mn;
                    int n;
                    hist.TryGetValue(cur, out n);
                    hist[cur] = n + 1;
                }
            }
            Thread.Sleep(periodMs);
        }
    }

    public static void StartTimerPoll(int ms) {
        lock (hist) { hist.Clear(); }
        periodMs = ms;
        running = true;
        poller = new Thread(new ThreadStart(Poll));
        poller.IsBackground = true;
        poller.Start();
    }

    // "coarsest;finest;current:count,current:count"
    public static string StopTimerPoll() {
        running = false;
        if (poller != null) poller.Join(3000);
        StringBuilder sb = new StringBuilder();
        lock (hist) {
            sb.Append(coarsest).Append(';').Append(finest).Append(';');
            bool first = true;
            foreach (KeyValuePair<uint, int> kv in hist) {
                if (!first) sb.Append(',');
                sb.Append(kv.Key).Append(':').Append(kv.Value);
                first = false;
            }
        }
        return sb.ToString();
    }
}
'@
    }
    $nativeOk = $true
} catch {
    $nativeErr = $_.Exception.Message
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Header' {
    $os = Get-CimInstance Win32_OperatingSystem
    $cs = Get-CimInstance Win32_ComputerSystem
    $cpus = @(Get-CimInstance Win32_Processor)
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    $script:cpuCount = [int]$cs.NumberOfLogicalProcessors
    $up = (Get-Date) - $os.LastBootUpTime
    'Report:       WindowsiOS guest CPU diagnostics (read-only)'
    ('Date:         {0}' -f $started.ToString('yyyy-MM-dd HH:mm:ss zzz'))
    ('Computer:     {0}   user: {1}' -f $env:COMPUTERNAME, $env:USERNAME)
    ('Uptime:       {0}d {1:00}:{2:00}:{3:00} (booted {4})' -f $up.Days, $up.Hours, $up.Minutes, $up.Seconds,
        $os.LastBootUpTime.ToString('yyyy-MM-dd HH:mm:ss'))
    ('Windows:      {0}, version {1}, build {2}.{3} ({4})' -f $os.Caption, $cv.DisplayVersion, $os.BuildNumber, $cv.UBR,
        $os.OSArchitecture)
    ('Build lab:    {0}' -f $cv.BuildLabEx)
    ('PowerShell:   {0}, process arch {1}, 64-bit process: {2}' -f $PSVersionTable.PSVersion, $env:PROCESSOR_ARCHITECTURE,
        [Environment]::Is64BitProcess)
    ('CPUs:         {0} logical ({1})' -f $cs.NumberOfLogicalProcessors, (($cpus | % { Cut $_.Name 60 }) -join '; '))
    ('RAM:          {0} MB total, {1} MB free' -f [Math]::Round($cs.TotalPhysicalMemory / 1MB),
        [Math]::Round($os.FreePhysicalMemory / 1KB))
    $res = @(Get-CimInstance Win32_VideoController | % {
        '{0}x{1}, {2} Hz as the driver reports it ({3})' -f $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution, $_.CurrentRefreshRate, (Cut $_.Name 40) })
    ('Resolution:   {0}' -f ($res -join '; '))
    $lp = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction SilentlyContinue).LogPixels
    $applied = (Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name AppliedDPI -ErrorAction SilentlyContinue).AppliedDPI
    if ($null -eq $lp) { $lpText = 'not set (96 default)' } else { $lpText = [string]$lp }
    if ($null -eq $applied) { $apText = 'not set' } else { $apText = [string]$applied }
    ('DPI:          LogPixels {0}, AppliedDPI {1} (in effect since the last sign-in; 96 = 100 percent, 144 = 150 percent)' -f $lpText, $apText)
    ('Elevated:     {0}' -f $(if ($isAdmin) { 'yes' } else { 'no' }))
    ('Mode:         {0}, sampling window {1} s every {2} s' -f $(if ($Quick) { 'quick' } else { 'full' }), $Seconds, $interval)
    if (-not $nativeOk) { 'Native helper: Add-Type failed, architecture, DPI and timer polling are skipped: ' + $nativeErr }
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Servicing and background processes present right now' {
    $names = @('TiWorker', 'TrustedInstaller', 'mscorsvw', 'ngen', 'ngentask', 'MoUsoCoreWorker', 'msiexec', 'UsoClient',
        'wuauclt', 'CompatTelRunner', 'DismHost', 'SIHClient', 'MsMpEng', 'MpCmdRun', 'SearchIndexer', 'MicrosoftEdgeUpdate')
    $procs = @(Get-CimInstance Win32_Process | ? { $names -contains [IO.Path]::GetFileNameWithoutExtension($_.Name) })
    $script:svcSeen = @($procs | % { [IO.Path]::GetFileNameWithoutExtension($_.Name) } | Sort-Object -Unique)
    'Processes (CPU seconds are cumulative since each process started; WMI kernel + user time):'
    $rows = $procs | Sort-Object { $_.KernelModeTime + $_.UserModeTime } -Descending | % {
        [pscustomobject]@{
            Name       = $_.Name
            PID        = $_.ProcessId
            CpuSeconds = Round1 (($_.KernelModeTime + $_.UserModeTime) / 1e7)
            Started    = HM $_.CreationDate
            Threads    = $_.ThreadCount
            WorkingMB  = [Math]::Round($_.WorkingSetSize / 1MB)
        }
    }
    if ($rows) { ToTable $rows } else { '(none of them are running)' }
    ''
    'Related services:'
    $svcNames = 'wuauserv', 'UsoSvc', 'TrustedInstaller', 'WaaSMedicSvc', 'BITS', 'DoSvc', 'CryptSvc', 'WinDefend', 'WSearch',
        'DiagTrack', 'SysMain', 'sppsvc', 'vgpusrv'
    $svcRows = foreach ($n in $svcNames) {
        $s = Get-CimInstance Win32_Service -Filter "Name='$n'" -ErrorAction SilentlyContinue
        if ($s) { [pscustomobject]@{ Service = $s.Name; State = $s.State; StartMode = $s.StartMode; PID = $s.ProcessId } }
    }
    ToTable $svcRows
    ''
    'Top 15 processes by cumulative CPU seconds since they started:'
    $top = Get-CimInstance Win32_Process | ? { $_.ProcessId -ne 0 } | Sort-Object { $_.KernelModeTime + $_.UserModeTime } -Descending | Select-Object -First 15 | % {
        [pscustomobject]@{
            Name       = $_.Name
            PID        = $_.ProcessId
            CpuSeconds = Round1 (($_.KernelModeTime + $_.UserModeTime) / 1e7)
            Started    = HM $_.CreationDate
        }
    }
    ToTable $top
}

# ---------------------------------------------------------------------------------------------------------
# The sampling window: counters, the process-start trace and the timer poll all run over the same seconds.
$script:snapBefore = $null
$script:snapAfter = $null
$script:startErr = ''
Add-Section ('Per-process CPU over the sampling window ({0} s, {1} samples every {2} s)' -f $Seconds, $sampleCount, $interval) {
    $counters = @('\Process(*)\% Processor Time', '\Processor(_Total)\% Idle Time', '\Processor(_Total)\Interrupts/sec',
        '\System\Context Switches/sec', '\System\Processes')
    $evtId = 'WinIOSDiagProcStart'
    $haveEvt = $false
    if ($isAdmin) {
        try {
            Register-CimIndicationEvent -ClassName Win32_ProcessStartTrace -SourceIdentifier $evtId -ErrorAction Stop | Out-Null
            $haveEvt = $true
        } catch { $script:startErr = $_.Exception.Message }
    }
    if (-not $haveEvt) {
        try { $script:snapBefore = @(Get-CimInstance Win32_Process | Select-Object ProcessId, Name, CreationDate) } catch { }
    }
    if ($nativeOk) { try { [WiNative]::StartTimerPoll(500) } catch { } }
    $t0 = Get-Date
    $sets = @()
    try {
        # A process that exits mid-window makes its sample invalid and Get-Counter reports that as an error; the rest of the
        # samples are fine, so keep going and count the complaints.
        $sets = @(Get-Counter -Counter $counters -SampleInterval $interval -MaxSamples $sampleCount -ErrorAction SilentlyContinue -ErrorVariable counterErrors)
    } finally {
        $t1 = Get-Date
        if ($nativeOk) { try { $script:timerHist = [WiNative]::StopTimerPoll() } catch { } }
        if ($haveEvt) {
            try {
                $evts = @(Get-Event -SourceIdentifier $evtId -ErrorAction SilentlyContinue)
                $script:startTotal = $evts.Count
                $script:startTop = @($evts | % { $_.SourceEventArgs.NewEvent.ProcessName } | Group-Object |
                    Sort-Object Count -Descending | Select-Object -First 25 | % { [pscustomobject]@{ Process = $_.Name; Starts = $_.Count } })
            } catch { $script:startErr = $_.Exception.Message }
            Unregister-Event -SourceIdentifier $evtId -ErrorAction SilentlyContinue
            Get-Event -SourceIdentifier $evtId -ErrorAction SilentlyContinue | Remove-Event -ErrorAction SilentlyContinue
        } else {
            try { $script:snapAfter = @(Get-CimInstance Win32_Process | Select-Object ProcessId, Name, CreationDate) } catch { }
        }
    }
    $n = $sets.Count
    if ($n -eq 0) {
        'No counter samples came back.' + $(if ($counterErrors) { ' First error: ' + $counterErrors[0].Exception.Message } else { '' })
        return
    }
    if ($counterErrors) { '{0} counter samples were invalid and skipped (processes that exited during the window).' -f @($counterErrors).Count }

    $tot = @{}; $peak = @{}
    $idle = @(); $ints = @(); $ctx = @(); $procCount = @()
    foreach ($set in $sets) {
        $per = @{}
        foreach ($s in $set.CounterSamples) {
            if ($s.Status -ne 0) { continue }
            $p = $s.Path
            if ($p -like '*\process(*)\% processor time') {
                $nm = $s.InstanceName -replace '#\d+$', ''
                if ($nm -eq '_total') { continue }
                $per[$nm] = [double]$per[$nm] + $s.CookedValue
            }
            elseif ($p -like '*\% idle time') { $idle += $s.CookedValue }
            elseif ($p -like '*\interrupts/sec') { $ints += $s.CookedValue }
            elseif ($p -like '*\context switches/sec') { $ctx += $s.CookedValue }
            elseif ($p -like '*\system\processes') { $procCount += $s.CookedValue }
        }
        foreach ($k in @($per.Keys)) {
            $tot[$k] = [double]$tot[$k] + $per[$k]
            if (-not $peak.ContainsKey($k) -or $per[$k] -gt $peak[$k]) { $peak[$k] = $per[$k] }
        }
    }
    $avg = @{}
    foreach ($k in @($tot.Keys)) { $avg[$k] = $tot[$k] / $n }
    $script:avgByName = $avg

    $win = ($t1 - $t0).TotalSeconds
    $cpus = $script:cpuCount
    if ($cpus -lt 1) { $cpus = 1 }
    'Window: {0:N1} s, {1} samples, {2} logical CPUs.' -f $win, $n, $cpus
    if ($idle.Count -gt 0) {
        $idleAvg = ($idle | Measure-Object -Average).Average
        $script:busyPct = [Math]::Max(0, 100 - $idleAvg)
        'Processor(_Total) idle time: {0:N1} percent, so the CPUs were {1:N1} percent busy (= {2:N0} percent of ONE CPU, of a possible {3}).' -f
            $idleAvg, $script:busyPct, ($script:busyPct * $cpus), (100 * $cpus)
    }
    $script:sysStats = @()
    if ($ints.Count -gt 0) {
        $script:sysStats += ('interrupts/sec {0:N0}' -f ($ints | Measure-Object -Average).Average)
        $m = $ints | Measure-Object -Average -Minimum -Maximum
        'Interrupts/sec:         avg {0:N0}  (min {1:N0}, max {2:N0})' -f $m.Average, $m.Minimum, $m.Maximum
    }
    if ($ctx.Count -gt 0) {
        $script:sysStats += ('context switches/sec {0:N0}' -f ($ctx | Measure-Object -Average).Average)
        $m = $ctx | Measure-Object -Average -Minimum -Maximum
        'Context switches/sec:   avg {0:N0}  (min {1:N0}, max {2:N0})' -f $m.Average, $m.Minimum, $m.Maximum
    }
    if ($procCount.Count -gt 0) {
        $m = $procCount | Measure-Object -Average -Minimum -Maximum
        'Processes:              avg {0:N1}  (min {1:N0}, max {2:N0})' -f $m.Average, $m.Minimum, $m.Maximum
    }
    ''
    'Top 20 processes by CPU. Percent of ONE CPU (100 = one whole CPU busy), averaged over the window; peak is the busiest single 5 s sample.'
    'Instances of the same name (svchost, ...) are added together. "powershell" is this script.'
    $rows = @($avg.GetEnumerator() | ? { $_.Key -ne 'idle' } | Sort-Object Value -Descending | Select-Object -First 20 | % {
        [pscustomobject]@{ Process = $_.Key; AvgPctOfOneCPU = Round1 $_.Value; PeakPct = Round1 $peak[$_.Key] }
    })
    $script:topRows = $rows
    ToTable $rows
    if ($avg.ContainsKey('idle')) { 'For scale: the Idle process averaged {0:N0} percent of one CPU.' -f $avg['idle'] }
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Process architecture (IsWow64Process2)' {
    if (-not $nativeOk) { 'skipped: native helper not available'; return }
    function Get-ArchLabel($r, $exeMachine) {
        if ($r[0] -ne 1) {
            switch ($exeMachine) {
                0xAA64 { return 'ARM64' }
                0x8664 { return 'x64 emulated' }
                0x014C { return 'x86 emulated' }
                0x01C4 { return 'ARM32' }
                default { return 'unknown (no image)' }
            }
        }
        $pm = [int]$r[1]; $nm = [int]$r[2]
        if ($pm -eq 0) {
            switch ($nm) {
                0xAA64 { return 'ARM64' }
                0x8664 { return 'x64 (native x64 OS)' }
                default { return ('native 0x{0:X}' -f $nm) }
            }
        }
        switch ($pm) {
            0x8664 { return 'x64 emulated' }
            0x014C { return 'x86 emulated' }
            0x01C4 { return 'ARM32' }
            0xAA64 { return 'ARM64EC (or ARM64 under WOW)' }
            0xA641 { return 'ARM64EC' }
            default { return ('emulated 0x{0:X}' -f $pm) }
        }
    }
    $procs = @(Get-CimInstance Win32_Process)
    $rows = foreach ($p in $procs) {
        $r = [WiNative]::Wow64([int]$p.ProcessId)
        $exe = -1
        $guessed = $false
        if ($r[0] -ne 1) {
            $img = $p.ExecutablePath
            if (-not $img) {
                # WMI hides the path of other sessions' processes from a non-elevated caller: try the System32 file of that name.
                $img = Join-Path $env:SystemRoot ('System32\' + $p.Name)
                $guessed = $true
            }
            if ($guessed -and -not (Test-Path -LiteralPath $img)) { $img = Join-Path $env:SystemRoot ('System32\wbem\' + $p.Name) }
            if (Test-Path -LiteralPath $img) { $exe = [WiNative]::PeMachine([string]$img) }
        }
        $short = [IO.Path]::GetFileNameWithoutExtension($p.Name)
        $a = $script:avgByName[$short]
        if ($null -eq $a) { $a = 0 }
        if ($r[0] -eq 1) { $src = 'IsWow64Process2'; $mach = '0x{0:X}' -f $r[1] }
        elseif ($exe -ge 0 -and $guessed) { $src = 'System32 exe header'; $mach = '0x{0:X}' -f $exe }
        elseif ($exe -ge 0) { $src = 'exe header'; $mach = '0x{0:X}' -f $exe }
        else { $src = 'none'; $mach = 'err {0}' -f $r[3] }
        [pscustomobject]@{
            Name       = $p.Name
            PID        = $p.ProcessId
            Arch       = Get-ArchLabel $r $exe
            Machine    = $mach
            Source     = $src
            CpuSeconds = Round1 (($p.KernelModeTime + $p.UserModeTime) / 1e7)
            AvgPct     = Round1 $a
        }
    }
    'Count by architecture:'
    ToTable ($rows | Group-Object Arch | Sort-Object Count -Descending | % { [pscustomobject]@{ Arch = $_.Name; Processes = $_.Count } })
    ''
    'Not native ARM64 (Machine is the IsWow64Process2 processMachine value, 0 for native, or the exe header machine for processes that cannot be opened without elevation; AvgPct is the name-wide average from the sampling window, percent of one CPU):'
    $emu = @($rows | ? { $_.Arch -ne 'ARM64' -and $_.Arch -notlike 'unknown*' } | Sort-Object CpuSeconds -Descending)
    $script:emuBusy = @($emu | ? { $_.AvgPct -ge 1 })
    ToTable $emu
    $viaExe = @($rows | ? { $_.Source -like '*exe header' })
    if ($viaExe.Count) { '{0} processes could not be opened (they need elevation); their architecture comes from the PE header of their exe, for the ones WMI hides the path of from the System32 file of the same name.' -f $viaExe.Count }
    $unk = @($rows | ? { $_.Arch -like 'unknown*' })
    if ($unk.Count) { ('{0} processes have neither: {1}' -f $unk.Count, ((($unk | Select-Object -First 10 | % { $_.Name }) -join ', '))) }
    ''
    'Note: Windows reports x64 and ARM64EC processes alike as machine 0x8664 here; Task Manager (Details, Architecture) tells them apart.'
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Process starts during the window' {
    if ($isAdmin) {
        if ($null -eq $script:startTotal) {
            'The Win32_ProcessStartTrace subscription failed: ' + $script:startErr
            return
        }
        $rate = if ($Seconds -gt 0) { $script:startTotal * 60.0 / $Seconds } else { 0 }
        'Win32_ProcessStartTrace: {0} processes started in about {1} s ({2:N1} per minute). This script and WMI add a few of their own (powershell, WmiPrvSE, curl).' -f
            $script:startTotal, $Seconds, $rate
        ToTable $script:startTop
        return
    }
    'The start trace needs elevation, so it was skipped. Run "Run WindowsiOS Diagnostics.cmd" and accept the prompt for it.'
    if ($script:snapBefore -and $script:snapAfter) {
        $before = @{}
        foreach ($p in $script:snapBefore) { $before[(PidKey $p)] = 1 }
        $new = @($script:snapAfter | ? { -not $before.ContainsKey((PidKey $_)) })
        $script:startApprox = $true
        $script:startTotal = $new.Count
        $script:startTop = @($new | Group-Object Name | Sort-Object Count -Descending | Select-Object -First 25 |
            % { [pscustomobject]@{ Process = $_.Name; Starts = $_.Count } })
        'Approximation without it: {0} processes that were not there at the start of the window and still were at its end (short-lived ones are missed).' -f $new.Count
        ToTable $script:startTop
    }
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Scheduled tasks that ran in the last 2 hours' {
    $cut = (Get-Date).AddHours(-2)
    $all = @(Get-ScheduledTask -ErrorAction Stop)
    '{0} tasks registered.' -f $all.Count
    $running = @($all | ? { $_.State -eq 'Running' })
    'Running right now: {0}' -f $(if ($running.Count) { (($running | % { $_.TaskPath + $_.TaskName }) -join '; ') } else { '(none)' })
    ''
    $info = @($all | Get-ScheduledTaskInfo -ErrorAction SilentlyContinue | ? { $_.LastRunTime -gt $cut } | Sort-Object LastRunTime -Descending)
    'Ran since {0} (newest first):' -f $cut.ToString('HH:mm:ss')
    $rows = $info | Select-Object -First 60 | % {
        [pscustomobject]@{
            LastRun = $_.LastRunTime.ToString('HH:mm:ss')
            Task    = Cut ($_.TaskPath + $_.TaskName) 90
            Result  = '0x{0:X}' -f $_.LastTaskResult
            Next    = $(if ($_.NextRunTime -and $_.NextRunTime.Year -gt 2000) { $_.NextRunTime.ToString('MM-dd HH:mm') } else { '' })
        }
    }
    ToTable $rows
    if ($info.Count -gt 60) { '... and {0} more' -f ($info.Count - 60) }
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Timer resolution' {
    '--- System timer resolution, polled every 500 ms during the sampling window (NtQueryTimerResolution) ---'
    if ($script:timerHist) {
        $parts = $script:timerHist -split ';'
        $coarse = [double]$parts[0]; $fine = [double]$parts[1]
        'Coarsest (default) period {0:N3} ms, finest the hardware allows {1:N3} ms.' -f ($coarse / 10000), ($fine / 10000)
        $total = 0
        $pairs = @()
        if ($parts.Count -gt 2 -and $parts[2]) {
            foreach ($kv in ($parts[2] -split ',')) {
                $a = $kv -split ':'
                $pairs += [pscustomobject]@{ PeriodMs = [Math]::Round([double]$a[0] / 10000, 3); Polls = [int]$a[1] }
                $total += [int]$a[1]
            }
        }
        foreach ($p in $pairs) { $p | Add-Member -NotePropertyName Share -NotePropertyValue ('{0:N0} percent' -f (100.0 * $p.Polls / [Math]::Max(1, $total))) }
        ToTable ($pairs | Sort-Object PeriodMs)
        if ($pairs | ? { $_.PeriodMs -lt 15 }) { 'Something asked for a period finer than the 15.6 ms default during the window.' }
        else { 'The system stayed at the default 15.6 ms period: nothing held a finer timer resolution.' }
    } else {
        '(no polling data)'
    }
    ''
    '--- powercfg /energy: Platform Timer Resolution requests (which process asked for which period) ---'
    if ($Quick) { 'skipped: -Quick'; return }
    if (-not $isAdmin) { 'skipped: powercfg /energy needs elevation. Run "Run WindowsiOS Diagnostics.cmd" and accept the prompt.'; return }
    $html = Join-Path $env:TEMP ('winios-energy-{0}.html' -f $stamp)
    Remove-Item $html -ErrorAction SilentlyContinue
    $console = (& powercfg.exe /energy /duration 30 /output $html 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    foreach ($l in ($console -split "`r?`n")) { $t = $l.Trim(); if ($t) { 'powercfg: ' + (Cut $t 200) } }
    if (-not (Test-Path $html)) {
        'powercfg exited with code {0} and wrote no report. The power trace it needs may be unavailable in this VM; the timer poll above still shows whether anything held a fine timer.' -f $rc
        return
    }
    $script:energyHtml = $html
    $t = [IO.File]::ReadAllText($html)
    $t = [regex]::Replace($t, '(?is)<(script|style)[^>]*>.*?</\1>', ' ')
    $t = [regex]::Replace($t, '(?i)</t[dh]\s*>', ' | ')
    $t = [regex]::Replace($t, '(?i)</?(tr|p|div|br|li|ul|table|h\d)[^>]*>', "`n")
    $t = [regex]::Replace($t, '<[^>]+>', '')
    $t = [Net.WebUtility]::HtmlDecode($t)
    $lines = @($t -split "`r?`n" | % { ($_ -replace '\s+', ' ').Trim() } | ? { $_ })
    $heads = @(0..($lines.Count - 1) | ? { $lines[$_] -match 'Platform Timer Resolution' })
    if (-not $heads.Count) {
        'No "Platform Timer Resolution" entry in the report (powercfg lists one when a program held a finer timer, or the report layout is not the one this expects).'
        ('Report kept at {0} ({1} text lines).' -f $html, $lines.Count)
        return
    }
    $pairsOut = @()
    $per = $null
    foreach ($l in $lines) {
        if ($l -match 'Requested Period\W+(\d+)') { $per = [double]$Matches[1] }
        elseif ($l -match 'Requesting Process Path\W+(.+)$') {
            $pairsOut += ('  period {0} (100 ns units) = {1:N2} ms   process {2}' -f $per, ($per / 10000), (Cut $Matches[1] 150))
        }
    }
    if ($pairsOut.Count) { 'Requests found (requested period, requesting process):'; $pairsOut }
    ''
    'Report text around the entries:'
    $keep = New-Object 'System.Collections.Generic.SortedSet[int]'
    foreach ($i in $heads) { for ($j = $i; $j -le [Math]::Min($i + 14, $lines.Count - 1); $j++) { [void]$keep.Add($j) } }
    foreach ($j in $keep) { '  ' + (Cut $lines[$j] 200) }
}

# ---------------------------------------------------------------------------------------------------------
Add-Section 'Installed Appx packages' {
    if ($isAdmin) {
        $pk = @(Get-AppxPackage -AllUsers -ErrorAction Stop | Sort-Object Name)
        'All users: {0} packages.' -f $pk.Count
    } else {
        $pk = @(Get-AppxPackage -ErrorAction Stop | Sort-Object Name)
        'Current user only ({0} packages): -AllUsers needs elevation, so it was skipped.' -f $pk.Count
    }
    ToTable ($pk | Select-Object Name, Version -Unique)
}

Add-Section 'Services running' {
    $svc = @(Get-CimInstance Win32_Service -Filter "State='Running'" | Sort-Object Name)
    '{0} running.' -f $svc.Count
    ToTable ($svc | % { [pscustomobject]@{ Name = $_.Name; Display = Cut $_.DisplayName 50; Start = $_.StartMode; PID = $_.ProcessId } })
}

Add-Section 'Startup entries' {
    'Win32_StartupCommand:'
    $sc = @(Get-CimInstance Win32_StartupCommand -ErrorAction SilentlyContinue)
    ToTable ($sc | % { [pscustomobject]@{ Name = $_.Name; Location = $_.Location; User = $_.User; Command = Cut $_.Command 90 } })
    ''
    'Run keys:'
    $keys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run', 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
    foreach ($k in $keys) {
        $item = Get-ItemProperty $k -ErrorAction SilentlyContinue
        $vals = @()
        if ($item) { $vals = @($item.PSObject.Properties | ? { $_.Name -notmatch '^PS(Path|ParentPath|ChildName|Drive|Provider)$' }) }
        '{0}: {1}' -f $k, $(if ($vals.Count) { '' } else { '(empty)' })
        foreach ($v in $vals) { '    {0} = {1}' -f $v.Name, (Cut $v.Value 140) }
    }
}

Add-Section 'Power settings (powercfg /q, read only)' {
    $queries = @(
        @('Active scheme', '/getactivescheme'),
        @('USB selective suspend', '/q', 'scheme_current', '2a737441-1930-4402-8d77-b2bebba308a3', '48e6b7a6-50f5-4782-a5d4-53bb8f07e226'),
        @('Processor parking: minimum unparked cores', '/q', 'scheme_current', '54533251-82be-4824-96c1-47b60b740d00', '0cc5b647-c1df-4637-891a-dec35c318583'),
        @('Processor parking: maximum unparked cores', '/q', 'scheme_current', '54533251-82be-4824-96c1-47b60b740d00', 'ea062031-0e34-4ff1-9b6d-eb1059334028'),
        @('Minimum processor state', '/q', 'scheme_current', 'sub_processor', 'procthrottlemin'),
        @('Maximum processor state', '/q', 'scheme_current', 'sub_processor', 'procthrottlemax')
    )
    foreach ($q in $queries) {
        '--- ' + $q[0]
        $argv = @($q[1..($q.Count - 1)])
        $out = (& powercfg.exe $argv 2>&1 | Out-String)
        foreach ($l in ($out -split "`r?`n")) { if ($l.Trim()) { '  ' + $l.TrimEnd() } }
        if ($q[1] -eq '/q' -and $out -notmatch 'Current AC Power Setting Index') { '  (this setting does not exist on this system)' }
    }
}

Add-Section 'Display' {
    $vc = @(Get-CimInstance Win32_VideoController)
    ToTable ($vc | % {
        [pscustomobject]@{
            Adapter = Cut $_.Name 36
            Mode    = '{0}x{1}' -f $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution
            Hz      = $_.CurrentRefreshRate
            Bits    = $_.CurrentBitsPerPixel
            Driver  = $_.DriverVersion
            Status  = $_.Status
        }
    })
    ''
    $dwm = $script:avgByName['dwm']
    if ($null -ne $dwm) { 'dwm.exe averaged {0:N1} percent of one CPU over the sampling window.' -f $dwm }
    else { 'dwm.exe did not show up in the sampling window.' }
    foreach ($n in 'csrss', 'WinIOSAgent', 'vgpusrv', 'viogpuap', 'explorer', 'SearchHost', 'StartMenuExperienceHost', 'ShellExperienceHost') {
        $v = $script:avgByName[$n]
        if ($null -ne $v) { '{0}: {1:N1} percent of one CPU' -f $n, $v }
    }
    if ($nativeOk) {
        $anim = [WiNative]::ClientAreaAnimation()
        'Client area animations (SPI_GETCLIENTAREAANIMATION): {0}' -f $(switch ($anim) { 1 { 'on' } 0 { 'off' } default { 'unknown' } })
    }
    $pers = Get-ItemProperty 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue
    if ($pers) { 'Transparency effects (EnableTransparency): {0}   apps theme light: {1}' -f $pers.EnableTransparency, $pers.AppsUseLightTheme }
    $wm = Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name MinAnimate -ErrorAction SilentlyContinue
    if ($wm) { 'Minimize/maximize animation (MinAnimate): {0}' -f $wm.MinAnimate }
}

# ---------------------------------------------------------------------------------------------------------
# The short read, placed right after the header.
$summary = New-Object System.Collections.Generic.List[string]
try {
    if ($null -ne $script:busyPct) {
        $summary.Add(('CPU: {0:N0} percent busy over the window ({1:N0} percent of ONE CPU out of {2}00).' -f
            $script:busyPct, ($script:busyPct * [Math]::Max(1, $script:cpuCount)), [Math]::Max(1, $script:cpuCount)))
    }
    if ($script:topRows.Count) {
        $summary.Add('Busiest (percent of one CPU): ' + ((($script:topRows | Select-Object -First 6) |
            % { '{0} {1}' -f $_.Process, $_.AvgPctOfOneCPU }) -join ', '))
    }
    if ($script:sysStats.Count) { $summary.Add('System rates (average): ' + ($script:sysStats -join ', ')) }
    $summary.Add('Servicing/background processes present: ' + $(if ($script:svcSeen.Count) { $script:svcSeen -join ', ' } else { 'none' }))
    if ($script:emuBusy.Count) {
        $summary.Add('Non-ARM64 processes using 1 percent of a CPU or more: ' + (($script:emuBusy | % { '{0} ({1} {2})' -f $_.Name, $_.Arch, $_.AvgPct }) -join ', '))
    } elseif ($nativeOk) {
        $summary.Add('Non-ARM64 processes using 1 percent of a CPU or more: none')
    }
    foreach ($n in 'WinIOSAgent', 'dwm') {
        $v = $script:avgByName[$n]
        if ($null -ne $v) { $summary.Add(('{0}: {1:N1} percent of one CPU' -f $n, $v)) }
    }
    if ($null -ne $script:startTotal) {
        $summary.Add(('Process starts in the window: {0}{1}; most frequent: {2}' -f $script:startTotal, $(if ($script:startApprox) { ' (approximate, not elevated)' } else { '' }),
            $(if ($script:startTop.Count) { (($script:startTop | Select-Object -First 5 | % { '{0} x{1}' -f $_.Process, $_.Starts }) -join ', ') } else { 'none' })))
    }
    if ($script:timerHist) {
        $parts = $script:timerHist -split ';'
        if ($parts.Count -gt 2 -and $parts[2]) {
            $res = foreach ($kv in ($parts[2] -split ',')) { $a = $kv -split ':'; '{0:N1} ms x{1}' -f ([double]$a[0] / 10000), $a[1] }
            $summary.Add('System timer resolution seen (polls): ' + ($res -join ', '))
        }
    }
    if ($isAdmin) { $summary.Add('Elevated: every section ran' + $(if ($Quick) { ' except powercfg /energy (-Quick).' } else { '.' })) }
    else { $summary.Add('Not elevated: the process-start trace, the powercfg /energy timer report and the all-users Appx list were skipped.') }
} catch {
    $summary.Add('summary failed: ' + $_.Exception.Message)
}
$sections.Insert(1, [pscustomobject]@{ Title = 'Summary'; Text = ($summary -join "`r`n") })

# ---------------------------------------------------------------------------------------------------------
$crlf = "`r`n"
$body = New-Object System.Text.StringBuilder
foreach ($s in $sections) {
    [void]$body.Append('==== ' + $s.Title + ' ====' + $crlf)
    [void]$body.Append($s.Text + $crlf + $crlf)
}
$ascii = [regex]::Replace($body.ToString(), '[^\x09\x0A\x0D\x20-\x7E]', '?')

$dir = Join-Path $env:ProgramData 'WindowsiOS'
$path = Join-Path $dir $reportName
$where = ''
try {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -ErrorAction Stop | Out-Null }
    [IO.File]::WriteAllText($path, $ascii, [Text.Encoding]::ASCII)
} catch {
    $where = ' (could not write to ' + $dir + ': ' + $_.Exception.Message + ')'
    $path = Join-Path $env:TEMP $reportName
    [IO.File]::WriteAllText($path, $ascii, [Text.Encoding]::ASCII)
}

function Send-ToHost([string]$file, [string]$name) {
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if (-not $curl) { return 'not uploaded: curl.exe is not on this Windows' }
    $out = (& $curl.Source -s --connect-timeout 5 --max-time 60 -w '%{http_code}' -T $file ('http://10.0.2.4/' + $name) 2>&1 | Out-String).Trim()
    if ($out -match '^(200|201|204)$') { return ('uploaded to the host share as {0} (HTTP {1})' -f $name, $out) }
    return ('upload failed (curl exit {0}, output "{1}"): the host share is not reachable or not running' -f $LASTEXITCODE, (Cut $out 120))
}

$upload = Send-ToHost $path $reportName
$energyUpload = $null
if ($script:energyHtml -and (Test-Path $script:energyHtml)) {
    $energyUpload = Send-ToHost $script:energyHtml ('diag-{0}-energy.html' -f $stamp)
}

Write-Host ''
Write-Host '---- Summary ----'
$summary | % { Write-Host ('  ' + $_) }
Write-Host ''
Write-Host ('Report: ' + $path + $where)
Write-Host ('Upload: ' + $upload)
if ($energyUpload) { Write-Host ('Energy report: ' + $energyUpload) }
