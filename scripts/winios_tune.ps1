#requires -version 5.1
<#
.SYNOPSIS
Applies the WindowsiOS guest performance settings. Run elevated.
.PARAMETER WhatIf
Read-only preview. No log, backup, upload, service stop or persistent setting is written.
.PARAMETER Undo
Restores saved original settings. Removed apps must be reinstalled from the Store.
#>
[CmdletBinding()]
param([switch]$WhatIf, [switch]$Undo)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$script:Counts = @{ Changed = 0; AlreadySet = 0; NotPresent = 0; Failed = 0; WouldChange = 0; Protected = 0 }
$script:Restart = $false
$script:SignOut = $false
$script:Writer = $null
$script:DefaultKey = $null
$script:DefaultMounted = $false
$script:NativeReady = $false
$script:PowerDirty = $false
$script:UserSid = $null
$script:BackupReady = $false
$script:Backup = [pscustomobject]@{ Version = 1; Entries = @() }
$dir = 'C:\ProgramData\WindowsiOS'
$backupPath = Join-Path $dir 'tune-backup.json'
$defaultMount = 'WindowsiOS_Tune_Default_' + $PID
$script:Mutex = $null

$removeApps = @(
    'Clipchamp.Clipchamp', 'MSTeams', 'Microsoft.BingNews', 'Microsoft.BingSearch',
    'Microsoft.BingWeather', 'Microsoft.GetHelp', 'Microsoft.Getstarted',
    'Microsoft.MicrosoftOfficeHub', 'Microsoft.MicrosoftSolitaireCollection',
    'Microsoft.MicrosoftStickyNotes', 'Microsoft.OutlookForWindows', 'Microsoft.Todos',
    'Microsoft.Windows.DevHome', 'Microsoft.WindowsAlarms', 'Microsoft.WindowsFeedbackHub',
    'Microsoft.WindowsSoundRecorder', 'Microsoft.YourPhone', 'MicrosoftCorporationII.QuickAssist',
    'MicrosoftWindows.Client.WebExperience', 'Microsoft.WidgetsPlatformRuntime',
    'MicrosoftWindows.CrossDevice', 'Microsoft.Copilot', 'Microsoft.PowerAutomateDesktop',
    'Microsoft.MicrosoftJournal', 'microsoft.windowscommunicationsapps', 'Microsoft.People',
    'Microsoft.WindowsMaps', 'Microsoft.549981C3F5F10', 'Microsoft.GamingApp',
    'Microsoft.XboxGamingOverlay', 'Microsoft.XboxGameOverlay', 'Microsoft.XboxSpeechToTextOverlay'
)
# UnistoreSvc goes last: it cannot stop while UserDataSvc or PimIndexMaintenanceSvc instances still run.
$templateServices = @('CDPUserSvc', 'OneSyncSvc', 'PimIndexMaintenanceSvc',
    'UserDataSvc', 'MessagingService', 'UnistoreSvc')
$disabledServices = $templateServices + @('CDPSvc', 'TrkWks', 'SSDPSRV', 'upnphost', 'WSAIFabricSvc')
$manualServices = @('edgeupdate', 'edgeupdatem', 'Spooler', 'iphlpsvc')
$taskFolders = @(
    '\Microsoft\Windows\Application Experience\',
    '\Microsoft\Windows\Customer Experience Improvement Program\',
    '\Microsoft\Windows\DiskDiagnostic\', '\Microsoft\Windows\Feedback\Siuf\',
    '\Microsoft\Windows\Windows Error Reporting\', '\Microsoft\Windows\Maps\',
    '\Microsoft\Windows\Power Efficiency Diagnostics\',
    '\Microsoft\Windows\DeviceDirectoryClient\', '\Microsoft\Windows\RetailDemo\'
)
$taskNames = @('\Microsoft\Windows\Maintenance\WinSAT',
    '\Microsoft\Windows\Registry\RegIdleBackup', '\Microsoft\Windows\Chkdsk\Proxy',
    '\Microsoft\Windows\Defrag\ScheduledDefrag')
$machineValues = @(
    @('SOFTWARE\Policies\Microsoft\Windows\Windows Search', 'EnableDynamicContentInWSB', 0),
    @('SOFTWARE\Policies\Microsoft\Windows\GameDVR', 'AllowGameDVR', 0),
    @('SOFTWARE\Policies\Microsoft\Windows\System', 'EnableMmx', 0),
    @('SOFTWARE\Policies\Microsoft\Windows\Explorer', 'HideRecommendedSection', 1),
    @('SOFTWARE\Policies\Microsoft\Windows\WindowsAI', 'AllowRecallEnablement', 0),
    @('SOFTWARE\Policies\Microsoft\Windows\WindowsAI', 'DisableAIDataAnalysis', 1),
    @('SOFTWARE\Policies\Microsoft\Edge', 'EfficiencyModeEnabled', 1),
    @('SOFTWARE\Policies\Microsoft\Edge', 'EfficiencyMode', 0),
    @('SOFTWARE\Policies\Microsoft\Edge', 'SleepingTabsEnabled', 1),
    @('SOFTWARE\Policies\Microsoft\Edge', 'SleepingTabsTimeout', 300),
    @('SOFTWARE\Policies\Microsoft\EdgeUpdate', 'AutoUpdateCheckPeriodMinutes', 1440)
)
$userValues = @(
    @('System\GameConfigStore', 'GameDVR_Enabled', 0),
    @('Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'Start_IrisRecommendations', 0),
    @('Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'Start_TrackDocs', 0),
    @('SOFTWARE\Policies\Microsoft\Windows\DWM', 'DisallowAnimations', 1)
)
$oobeKeys = @('SOFTWARE\Microsoft\WindowsUpdate\Orchestrator\UScheduler_Oobe\OutlookUpdate',
    'SOFTWARE\Microsoft\WindowsUpdate\Orchestrator\UScheduler_Oobe\DevHomeUpdate')
# GET/SET pairs from winuser.h. All seven setters take BOOL in pvParam, not uiParam.
$visuals = @(
    @('ClientAreaAnimation', 0x1042, 0x1043), @('MenuAnimation', 0x1002, 0x1003),
    @('MenuFade', 0x1012, 0x1013), @('TooltipAnimation', 0x1016, 0x1017),
    @('ComboBoxAnimation', 0x1004, 0x1005), @('ListBoxSmoothScrolling', 0x1006, 0x1007),
    @('CursorShadow', 0x101A, 0x101B)
)
$powerSettings = @(
    @('USB selective suspend', '2a737441-1930-4402-8d77-b2bebba308a3',
        '48e6b7a6-50f5-4782-a5d4-53bb8f07e226', 0),
    @('Processor minimum unparked cores (CPMINCORES)', '54533251-82be-4824-96c1-47b60b740d00',
        '0cc5b647-c1df-4637-891a-dec35c318583', 100)
)

function Write-Log([string]$Text) {
    $line = [regex]::Replace(('[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $Text),
        '[^\x09\x20-\x7E]', '?')
    Write-Host $line
    if ($script:Writer) { $script:Writer.WriteLine($line) }
}
function Format-State($State) {
    if ($null -eq $State) { return '<unknown>' }
    return (ConvertTo-Json -InputObject $State -Depth 40 -Compress)
}
function Same-State($Left, $Right) { return (Format-State $Left) -ceq (Format-State $Right) }
function Test-ProfileSid([string]$Sid) {
    try {
        $parsed = [Security.Principal.SecurityIdentifier]::new($Sid)
        return $parsed.Value -eq $Sid -and $Sid -notin @('S-1-5-18', 'S-1-5-19', 'S-1-5-20')
    } catch { return $false }
}
function Report-Failure([string]$Item, $Old, $New, $ErrorRecord) {
    $script:Counts.Failed++
    Write-Log ('FAILED {0}: old={1}; new={2}; {3}' -f $Item,
        (Format-State $Old), (Format-State $New), $ErrorRecord.Exception.Message)
}
function Report-Absent([string]$Item) {
    $script:Counts.NotPresent++
    Write-Log ('NOT PRESENT ' + $Item)
}
function Save-Original($Entry) {
    if (-not $script:BackupReady) { throw 'Backup unavailable; refusing the change.' }
    if (@($script:Backup.Entries | Where-Object { $_.Id -eq $Entry.Id }).Count) { return }
    $previous = @($script:Backup.Entries)
    $script:Backup.Entries = @($previous) + @($Entry)
    $temporary = $backupPath + '.tmp-' + $PID
    try {
        $json = ConvertTo-Json -InputObject $script:Backup -Depth 50
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        # Windows PowerShell passes a plain $null to a string parameter as "", which File.Replace rejects.
        if ([IO.File]::Exists($backupPath)) { [IO.File]::Replace($temporary, $backupPath, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $backupPath) }
    } catch {
        $script:Backup.Entries = $previous
        throw
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}
function Invoke-Change($Entry, $Old, $New, [scriptblock]$Apply, [string]$Effect = '') {
    if (Same-State $Old $New) {
        $script:Counts.AlreadySet++
        Write-Log ('ALREADY SET {0}: old={1}; new={2}; OK' -f $Entry.Id, (Format-State $Old), (Format-State $New))
        return
    }
    if (-not $WhatIf -and -not $Undo) { Save-Original $Entry }
    if ($Effect -eq 'Restart') { $script:Restart = $true }
    if ($Effect -eq 'SignOut') { $script:SignOut = $true }
    if ($WhatIf) {
        $script:Counts.WouldChange++
        Write-Log ('WHATIF {0}: old={1}; new={2}' -f $Entry.Id, (Format-State $Old), (Format-State $New))
        return
    }
    Write-Log ('APPLY {0}: old={1}; new={2}' -f $Entry.Id, (Format-State $Old), (Format-State $New))
    & $Apply
    $script:Counts.Changed++
    Write-Log ('OK ' + $Entry.Id)
}
function Open-Root([string]$Root) {
    switch ($Root) {
        'Machine' { return [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64') }
        'Default' {
            if (-not $script:DefaultKey) { throw 'Default profile hive is unavailable.' }
            return $script:DefaultKey
        }
        default {
            if (-not (Test-ProfileSid $Root)) { throw 'Invalid profile SID.' }
            $users = [Microsoft.Win32.RegistryKey]::OpenBaseKey('Users', 'Registry64')
            try { $key = $users.OpenSubKey($Root, -not [bool]$WhatIf) } finally { $users.Dispose() }
            if (-not $key) { throw ('User hive is not loaded: ' + $Root) }
            return $key
        }
    }
}
function Close-Root($Key, [string]$Root) { if ($Root -ne 'Default' -and $Key) { $Key.Dispose() } }
function Read-Value($Key, [string]$Name) {
    if (-not $Key -or $Key.GetValueNames() -notcontains $Name) {
        return [pscustomobject][ordered]@{ Exists = $false; Kind = ''; Value = $null }
    }
    $kind = $Key.GetValueKind($Name).ToString()
    $data = $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($kind -in @('Binary', 'None')) { $data = [Convert]::ToBase64String([byte[]]$data) }
    if ($kind -eq 'DWord') { $data = [long][BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$data), 0) }
    if ($kind -eq 'QWord') { $data = ([long]$data).ToString([Globalization.CultureInfo]::InvariantCulture) }
    return [pscustomobject][ordered]@{ Exists = $true; Kind = $kind; Value = $data }
}
function Write-Value($Key, [string]$Name, $State) {
    if (-not $State.Exists) { $Key.DeleteValue($Name, $false); return }
    $value = $State.Value
    switch ($State.Kind) {
        'Binary' { $value = [Convert]::FromBase64String([string]$value) }
        'None' { $value = [Convert]::FromBase64String([string]$value) }
        'DWord' { $value = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$value), 0) }
        'QWord' { $value = [long]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture) }
        'MultiString' { $value = [string[]]@($value) }
        default { $value = [string]$value }
    }
    $Key.SetValue($Name, $value, [Microsoft.Win32.RegistryValueKind]$State.Kind)
}
function Read-Registry([string]$Root, [string]$Path, [string]$Name) {
    $base = Open-Root $Root
    $key = $null
    try { $key = $base.OpenSubKey($Path); return Read-Value $key $Name }
    finally { if ($key) { $key.Dispose() }; Close-Root $base $Root }
}
function Set-RegistryState([string]$Root, [string]$Path, [string]$Name, $State) {
    $base = Open-Root $Root
    $key = $null
    try {
        if ($State.Exists) { $key = $base.CreateSubKey($Path) }
        else { $key = $base.OpenSubKey($Path, $true) }
        if ($key) { Write-Value $key $Name $State }
    } finally { if ($key) { $key.Dispose() }; Close-Root $base $Root }
    if (-not (Same-State (Read-Registry $Root $Path $Name) $State)) { throw 'Registry verification failed.' }
}
function Get-MissingKeys([string]$Root, [string]$Path) {
    $base = Open-Root $Root
    try {
        $partial = ''
        foreach ($part in $Path.Split('\')) {
            $partial = ($partial + '\' + $part).TrimStart('\')
            $key = $base.OpenSubKey($partial)
            if ($key) { $key.Dispose() } else { $partial }
        }
    } finally { Close-Root $base $Root }
}
function Remove-EmptyKeys($Entry) {
    $paths = @($Entry.MissingKeys)
    [array]::Reverse($paths)
    $base = Open-Root $Entry.Root
    try {
        foreach ($path in $paths) {
            $key = $base.OpenSubKey($path)
            if (-not $key) { continue }
            try { $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 } finally { $key.Dispose() }
            if ($empty) {
                if ($Entry.Path -ne $path -and -not $Entry.Path.StartsWith($path + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw 'Backup cleanup path is outside the original registry path.'
                }
                $base.DeleteSubKey($path, $false)
            }
        }
    } finally { Close-Root $base $Entry.Root }
}
function Tune-Registry([string]$Root, [string]$Path, [string]$Name, [long]$Value, [string]$Effect = 'SignOut') {
    $old = $null
    $new = [pscustomobject][ordered]@{ Exists = $true; Kind = 'DWord'; Value = $Value }
    $id = 'Registry ' + $Root + '\' + $Path + ' [' + $Name + ']'
    try {
        $old = Read-Registry $Root $Path $Name
        $entry = [pscustomobject]@{ Id = $id; Kind = 'Registry'; Root = $Root; Path = $Path; Name = $Name;
            State = $old; MissingKeys = @(Get-MissingKeys $Root $Path); Effect = $Effect }
        Invoke-Change $entry $old $new { Set-RegistryState $Root $Path $Name $new } $Effect
    } catch { Report-Failure $id $old $new $_ }
}
function Read-Tree($Key) {
    $values = @(foreach ($name in ($Key.GetValueNames() | Sort-Object)) {
        [pscustomobject]@{ Name = $name; State = (Read-Value $Key $name) }
    })
    $children = @(foreach ($name in ($Key.GetSubKeyNames() | Sort-Object)) {
        $child = $Key.OpenSubKey($name)
        try { [pscustomobject]@{ Name = $name; Tree = (Read-Tree $child) } } finally { $child.Dispose() }
    })
    return [pscustomobject]@{ Values = $values; Children = $children }
}
function Write-Tree($Key, $Tree) {
    foreach ($value in $Tree.Values) { Write-Value $Key $value.Name $value.State }
    foreach ($child in $Tree.Children) {
        $key = $Key.CreateSubKey($child.Name)
        try { Write-Tree $key $child.Tree } finally { $key.Dispose() }
    }
}
function Tune-OobeKey([string]$Path) {
    $old = $null
    $id = 'Registry tree Machine\' + $Path
    try {
        $base = Open-Root 'Machine'
        try {
            $key = $base.OpenSubKey($Path)
            if (-not $key) { Report-Absent $id; return }
            try { $old = Read-Tree $key } finally { $key.Dispose() }
        } finally { $base.Dispose() }
        $entry = [pscustomobject]@{ Id = $id; Kind = 'RegistryTree'; Path = $Path; State = $old }
        Invoke-Change $entry $old $null {
            $base = Open-Root 'Machine'
            try {
                $base.DeleteSubKeyTree($Path, $false)
                $remaining = $base.OpenSubKey($Path)
                if ($remaining) { $remaining.Dispose(); throw 'Registry tree deletion verification failed.' }
            } finally { $base.Dispose() }
        }
    } catch { Report-Failure $id $old $null $_ }
}
function Tune-Service([string]$Name, [int]$Start) {
    $id = 'Service ' + $Name
    $old = $null
    $new = [pscustomobject][ordered]@{ Exists = $true; Kind = 'DWord'; Value = $Start }
    $path = 'SYSTEM\CurrentControlSet\Services\' + $Name
    try {
        $old = Read-Registry 'Machine' $path 'Start'
        if (-not $old.Exists) { Report-Absent $id; return }
        $entry = [pscustomobject]@{ Id = $id; Kind = 'Service'; Name = $Name; State = $old }
        $effect = if ($templateServices -contains $Name) { 'Restart' } else { '' }
        Invoke-Change $entry $old $new {
            if ($templateServices -notcontains $Name) {
                $mode = if ($Start -eq 4) { 'Disabled' } else { 'Manual' }
                Set-Service -Name $Name -StartupType $mode -ErrorAction Stop
            }
            Set-RegistryState 'Machine' $path 'Start' $new
        } $effect
    } catch { Report-Failure $id $old $new $_ }
    # A per-user template may have running suffixed instances. Only stop this allowlist's instances.
    try {
        $services = @(Get-Service -ErrorAction Stop | Where-Object {
            $_.Name -eq $Name -or ($templateServices -contains $Name -and $_.Name -like ($Name + '_*'))
        })
        foreach ($service in $services) {
            if ($service.Status -ne 'Running') { continue }
            if ($WhatIf) {
                $script:Counts.WouldChange++
                Write-Log ('WHATIF Service {0}: old=Running; new=Stopped' -f $service.Name)
                continue
            }
            try {
                Stop-Service -InputObject $service -ErrorAction Stop
                $service.Refresh()
                Write-Log ('OK Service {0}: old=Running; new={1}' -f $service.Name, $service.Status)
                $script:Counts.Changed++
            } catch { Write-Log ('IGNORED stop failure Service {0}: old=Running; new=Stopped; {1}' -f $service.Name, $_.Exception.Message) }
        }
    } catch { Write-Log ('IGNORED service stop query ' + $Name + ': ' + $_.Exception.Message) }
}
function Test-TargetTask([string]$Path, [string]$Name) {
    if ($Name -eq 'WindowsiOS Agent' -or $Name -match '(?i)Edge.*Update|Update.*Edge') { return $false }
    foreach ($part in @('UpdateOrchestrator', 'WindowsUpdate', 'InstallService', 'Time Synchronization', 'Servicing')) {
        if ($Path -like ('*\' + $part + '\*')) { return $false }
    }
    $full = $Path + $Name
    if ($Name -like 'OneDrive*' -or $taskNames -contains $full) { return $true }
    foreach ($folder in $taskFolders) {
        if ($Path.StartsWith($folder, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}
function Tune-Tasks {
    try { $tasks = @(Get-ScheduledTask -ErrorAction Stop) }
    catch { Report-Failure 'Scheduled task enumeration' $null 'Disabled' $_; return }
    foreach ($target in @($taskFolders + $taskNames + @('OneDrive*'))) {
        $found = @($tasks | Where-Object {
            if ($target -eq 'OneDrive*') { $_.TaskName -like 'OneDrive*' }
            elseif ($target.EndsWith('\')) { $_.TaskPath.StartsWith($target, [StringComparison]::OrdinalIgnoreCase) }
            else { ($_.TaskPath + $_.TaskName) -eq $target }
        })
        if (-not $found.Count) { Report-Absent ('Task ' + $target) }
    }
    foreach ($task in $tasks) {
        if (-not (Test-TargetTask $task.TaskPath $task.TaskName)) { continue }
        $id = 'Task ' + $task.TaskPath + $task.TaskName
        $old = $null
        try {
            $old = [bool]$task.Settings.Enabled
            $entry = [pscustomobject]@{ Id = $id; Kind = 'Task'; Path = $task.TaskPath;
                Name = $task.TaskName; State = $old }
            Invoke-Change $entry $old $false {
                Disable-ScheduledTask -TaskPath $task.TaskPath -TaskName $task.TaskName -ErrorAction Stop | Out-Null
                $check = Get-ScheduledTask -TaskPath $task.TaskPath -TaskName $task.TaskName -ErrorAction Stop
                if ($check.Settings.Enabled) { throw 'Task is still enabled.' }
            }
        } catch {
            # A few SYSTEM-owned tasks refuse even an elevated administrator. That is not a script fault.
            if ($_.Exception.Message -match 'Access is denied') {
                $script:Counts.Protected++
                Write-Log ('SKIPPED protected task ' + $id + ': only SYSTEM may change it; unchanged')
            } else { Report-Failure $id $old $false $_ }
        }
    }
}
function Tune-Apps {
    Write-Log 'App removal cannot be undone. The Microsoft Store can reinstall removed apps.'
    $provisionedInventory = @()
    $provisionedError = $null
    try { $provisionedInventory = @(Get-AppxProvisionedPackage -Online -ErrorAction Stop) }
    catch { $provisionedError = $_ }
    foreach ($name in $removeApps) {
        $blocked = $false
        $changedBefore = $script:Counts.Changed
        try {
            $packages = @(Get-AppxPackage -AllUsers -Name $name -PackageTypeFilter Main,Bundle -ErrorAction Stop |
                Where-Object { $_.Name -eq $name })
            $blocked = @($packages | Where-Object { $_.NonRemovable }).Count -gt 0
            if (-not $packages.Count) { Report-Absent ('Appx all users ' + $name) }
            $bundles = @($packages | Where-Object { $_.IsBundle })
            # Removing a bundle also removes its same-version main package. Retain standalone versions.
            $packages = @($packages | Where-Object {
                $candidate = $_
                $candidate.IsBundle -or -not @($bundles | Where-Object {
                    $_.Version -eq $candidate.Version -and $_.PublisherId -eq $candidate.PublisherId
                }).Count
            })
            foreach ($package in $packages) {
                $id = 'Appx all users ' + $package.PackageFullName
                if ($blocked -or $package.NonRemovable) {
                    $script:Counts.AlreadySet++
                    Write-Log ('SKIPPED ' + $id + ': NonRemovable; unchanged')
                    continue
                }
                try {
                    $entry = [pscustomobject]@{ Id = $id; Kind = 'AppRemoval'; Name = $name; State = 'Installed' }
                    Invoke-Change $entry 'Installed for one or more users' 'Removed for all users (irreversible)' {
                        Remove-AppxPackage -Package $package.PackageFullName -AllUsers -ErrorAction Stop
                    } 'SignOut'
                } catch { Report-Failure $id 'Installed' 'Removed for all users (irreversible)' $_ }
            }
        } catch {
            $blocked = $true
            Report-Failure ('Appx query ' + $name) $null 'Removed for all users' $_
        }
        try {
            if ($provisionedError) { throw $provisionedError }
            $provisioned = @($provisionedInventory | Where-Object { $_.DisplayName -eq $name })
            if ($provisioned.Count -and $script:Counts.Changed -gt $changedBefore) {
                # Remove-AppxPackage -AllUsers already deprovisions: re-read, or the second removal fails with "not found".
                $provisioned = @(Get-AppxProvisionedPackage -Online -ErrorAction Stop | Where-Object { $_.DisplayName -eq $name })
            }
            if (-not $provisioned.Count) { Report-Absent ('Provisioned Appx ' + $name) }
            foreach ($package in $provisioned) {
                $id = 'Provisioned Appx ' + $package.PackageName
                if ($blocked -or $package.NonRemovable) {
                    $script:Counts.AlreadySet++
                    Write-Log ('SKIPPED ' + $id + ': NonRemovable or installed-package query failed; unchanged')
                    continue
                }
                try {
                    $entry = [pscustomobject]@{ Id = $id; Kind = 'AppRemoval'; Name = $name; State = 'Provisioned' }
                    Invoke-Change $entry 'Provisioned' 'Deprovisioned (irreversible)' {
                        $result = Remove-AppxProvisionedPackage -Online -PackageName $package.PackageName -AllUsers -ErrorAction Stop
                        if ($result.RestartNeeded) { $script:Restart = $true }
                    }
                } catch { Report-Failure $id 'Provisioned' 'Deprovisioned (irreversible)' $_ }
            }
        } catch { Report-Failure ('Provisioned Appx query ' + $name) $null 'Deprovisioned' $_ }
    }
}

function Initialize-Native {
    if ('WindowsiOSTuneNative' -as [type]) { $script:NativeReady = $true; return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
public static class WindowsiOSTuneNative {
    [DllImport("user32.dll", EntryPoint="SystemParametersInfoW", SetLastError=true)]
    static extern bool GetSpi(uint action, uint param, ref int value, uint flags);
    [DllImport("user32.dll", EntryPoint="SystemParametersInfoW", SetLastError=true)]
    static extern bool SetSpi(uint action, uint param, IntPtr value, uint flags);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    static extern int RegLoadAppKey(string file, out IntPtr key, int access, int options, int reserved);
    [DllImport("powrprof.dll")]
    static extern uint PowerGetActiveScheme(IntPtr root, out IntPtr scheme);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("powrprof.dll")]
    static extern uint PowerReadACValueIndex(IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, out uint value);
    [DllImport("powrprof.dll")]
    static extern uint PowerReadDCValueIndex(IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, out uint value);
    public static bool GetVisual(uint action) {
        int value = 0;
        if (!GetSpi(action, 0, ref value, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return value != 0;
    }
    public static void SetVisual(uint action, bool value) {
        if (!SetSpi(action, 0, new IntPtr(value ? 1 : 0), 3))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public static RegistryKey OpenDefaultReadOnly(string file) {
        IntPtr key;
        int rc = RegLoadAppKey(file, out key, 0x20019, 1, 0); // KEY_READ, REG_PROCESS_APPKEY
        if (rc != 0) throw new Win32Exception(rc);
        return RegistryKey.FromHandle(new SafeRegistryHandle(key, true), RegistryView.Registry64);
    }
    public static Guid ActiveScheme() {
        IntPtr scheme;
        uint rc = PowerGetActiveScheme(IntPtr.Zero, out scheme);
        if (rc != 0) throw new Win32Exception((int)rc);
        try { return (Guid)Marshal.PtrToStructure(scheme, typeof(Guid)); }
        finally { LocalFree(scheme); }
    }
    public static uint[] ReadPower(Guid scheme, Guid subgroup, Guid setting, bool ac) {
        uint value;
        uint rc = ac ? PowerReadACValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, out value)
                     : PowerReadDCValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, out value);
        return new uint[] { rc, value };
    }
}
'@
    $script:NativeReady = $true
}
function Initialize-User {
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    $owners = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$session" -ErrorAction Stop |
        ForEach-Object {
            $owner = Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid -ErrorAction Stop
            if ($owner.ReturnValue -ne 0) { throw 'Could not resolve the desktop owner.' }
            $owner.Sid
        } | Sort-Object -Unique)
    if ($owners.Count -ne 1) { throw 'Cannot uniquely identify the signed-in desktop user in this session.' }
    $script:UserSid = $owners[0]
    Write-Log ('Signed-in desktop user SID: ' + $script:UserSid)
}
function Initialize-Default {
    if ($script:DefaultKey) { return }
    if ($WhatIf) {
        if (-not $script:NativeReady) { throw 'Read-only default hive helper unavailable.' }
        $script:DefaultKey = [WindowsiOSTuneNative]::OpenDefaultReadOnly('C:\Users\Default\NTUSER.DAT')
        return
    }
    $output = (& reg.exe load ('HKU\' + $defaultMount) 'C:\Users\Default\NTUSER.DAT' 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw ('reg load failed: ' + $output) }
    $script:DefaultMounted = $true
    $users = [Microsoft.Win32.RegistryKey]::OpenBaseKey('Users', 'Registry64')
    try { $script:DefaultKey = $users.OpenSubKey($defaultMount, $true) } finally { $users.Dispose() }
    if (-not $script:DefaultKey) { throw 'Could not open the loaded default profile hive.' }
    Write-Log 'OK loaded C:\Users\Default\NTUSER.DAT (not HKU\.DEFAULT)'
}
function Assert-LiveUser([string]$Sid) {
    if (-not $script:NativeReady) { throw 'Native helper unavailable.' }
    if (-not $Sid -or $Sid -ne $script:UserSid -or
        $Sid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) {
        throw 'Live visual effects require elevation as the signed-in desktop user; run as that user.'
    }
}
function Tune-Visual($Visual) {
    $old = $null
    $id = 'Visual ' + $Visual[0]
    try {
        Assert-LiveUser $script:UserSid
        $old = [WindowsiOSTuneNative]::GetVisual($Visual[1])
        $entry = [pscustomobject]@{ Id = $id + ' ' + $script:UserSid; Kind = 'Visual';
            Sid = $script:UserSid; Get = $Visual[1]; Set = $Visual[2]; State = $old }
        Invoke-Change $entry $old $false {
            [WindowsiOSTuneNative]::SetVisual($Visual[2], $false)
            if ([WindowsiOSTuneNative]::GetVisual($Visual[1])) { throw 'Visual effect is still enabled.' }
        }
    } catch { Report-Failure $id $old $false $_ }
}
function Read-Power([string]$Scheme, [string]$Subgroup, [string]$Setting, [string]$Mode) {
    if (-not $script:NativeReady) { throw 'Native helper unavailable.' }
    $result = [WindowsiOSTuneNative]::ReadPower([guid]$Scheme, [guid]$Subgroup, [guid]$Setting, ($Mode -eq 'AC'))
    if ($result[0] -in @(2, 1168)) { return $null }
    if ($result[0] -ne 0) { throw ('PowerReadValueIndex failed: ' + $result[0]) }
    return [uint32]$result[1]
}
function Set-Power($Entry, [uint32]$Value) {
    $verb = if ($Entry.Mode -eq 'AC') { '/setacvalueindex' } else { '/setdcvalueindex' }
    $output = (& powercfg.exe $verb $Entry.Scheme $Entry.Subgroup $Entry.Setting $Value 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw ('powercfg failed: ' + $output) }
    $script:PowerDirty = $true
    if ((Read-Power $Entry.Scheme $Entry.Subgroup $Entry.Setting $Entry.Mode) -ne $Value) { throw 'Power setting verification failed.' }
}
function Tune-Power {
    try {
        if (-not $script:NativeReady) { throw 'Native helper unavailable.' }
        $scheme = [WindowsiOSTuneNative]::ActiveScheme().ToString()
    } catch { Report-Failure 'Active power scheme' $null 'Read' $_; return }
    foreach ($setting in $powerSettings) {
        foreach ($mode in @('AC', 'DC')) {
            $old = $null
            $id = 'Power ' + $scheme + ' ' + $setting[0] + ' ' + $mode
            try {
                $old = Read-Power $scheme $setting[1] $setting[2] $mode
                if ($null -eq $old) { Report-Absent $id; continue }
                $entry = [pscustomobject]@{ Id = $id; Kind = 'Power'; Scheme = $scheme;
                    Subgroup = $setting[1]; Setting = $setting[2]; Mode = $mode; State = $old }
                Invoke-Change $entry $old ([uint32]$setting[3]) { Set-Power $entry $setting[3] }
            } catch { Report-Failure $id $old $setting[3] $_ }
        }
    }
}
function Apply-ActivePower {
    if ($WhatIf -or -not $script:PowerDirty) { return }
    try {
        $output = (& powercfg.exe /setactive scheme_current 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw ('powercfg /setactive failed: ' + $output) }
        Write-Log 'OK powercfg /setactive scheme_current'
    } catch { Report-Failure 'Apply active power scheme' 'Current scheme' 'Reload current scheme' $_ }
}

function Test-BackupEntry($Entry) {
    switch ($Entry.Kind) {
        'Registry' {
            if ($Entry.Root -eq 'Machine') {
                foreach ($spec in $machineValues) {
                    if ($Entry.Path -eq $spec[0] -and $Entry.Name -eq $spec[1]) { return $true }
                }
                return $Entry.Path -eq 'SYSTEM\CurrentControlSet\Control' -and $Entry.Name -eq 'SvcHostSplitThresholdInKB'
            }
            if ($Entry.Root -ne 'Default' -and -not (Test-ProfileSid $Entry.Root)) { return $false }
            foreach ($spec in $userValues) {
                if ($Entry.Path -eq $spec[0] -and $Entry.Name -eq $spec[1]) { return $true }
            }
            return $false
        }
        'RegistryTree' { return $oobeKeys -contains $Entry.Path }
        'Service' { return @($disabledServices + $manualServices) -contains $Entry.Name }
        'Task' { return Test-TargetTask $Entry.Path $Entry.Name }
        'Visual' {
            foreach ($spec in $visuals) { if ($Entry.Get -eq $spec[1] -and $Entry.Set -eq $spec[2]) { return $true } }
            return $false
        }
        'Power' {
            if ($Entry.Mode -notin @('AC', 'DC')) { return $false }
            foreach ($spec in $powerSettings) {
                if ($Entry.Subgroup -eq $spec[1] -and $Entry.Setting -eq $spec[2]) { return $true }
            }
            return $false
        }
        'AppRemoval' { return $removeApps -contains $Entry.Name }
    }
    return $false
}
function Undo-Entry($Entry) {
    $old = $null
    try {
        if (-not (Test-BackupEntry $Entry)) { throw 'Backup entry is outside the tuning allowlist.' }
        switch ($Entry.Kind) {
            'Registry' {
                if ($Entry.Root -eq 'Default') { Initialize-Default }
                $old = Read-Registry $Entry.Root $Entry.Path $Entry.Name
                Invoke-Change $Entry $old $Entry.State {
                    Set-RegistryState $Entry.Root $Entry.Path $Entry.Name $Entry.State
                    Remove-EmptyKeys $Entry
                } $Entry.Effect
            }
            'RegistryTree' {
                $base = Open-Root 'Machine'
                try {
                    $key = $base.OpenSubKey($Entry.Path)
                    if ($key) { try { $old = Read-Tree $key } finally { $key.Dispose() } }
                } finally { $base.Dispose() }
                Invoke-Change $Entry $old $Entry.State {
                    $base = Open-Root 'Machine'
                    try {
                        $base.DeleteSubKeyTree($Entry.Path, $false)
                        $key = $base.CreateSubKey($Entry.Path)
                        try { Write-Tree $key $Entry.State } finally { $key.Dispose() }
                        $key = $base.OpenSubKey($Entry.Path)
                        try {
                            if (-not (Same-State (Read-Tree $key) $Entry.State)) { throw 'Registry tree restore verification failed.' }
                        } finally { $key.Dispose() }
                    } finally { $base.Dispose() }
                }
            }
            'Service' {
                $path = 'SYSTEM\CurrentControlSet\Services\' + $Entry.Name
                $old = Read-Registry 'Machine' $path 'Start'
                if (-not $old.Exists) { Report-Absent $Entry.Id; return }
                $effect = if ($templateServices -contains $Entry.Name) { 'Restart' } else { '' }
                Invoke-Change $Entry $old $Entry.State {
                    if ($templateServices -notcontains $Entry.Name) {
                        $modes = @{ 0 = 'boot'; 1 = 'system'; 2 = 'auto'; 3 = 'demand'; 4 = 'disabled' }
                        $mode = $modes[[int]$Entry.State.Value]
                        if (-not $mode) { throw 'Invalid original service start type.' }
                        $output = (& sc.exe config $Entry.Name 'start=' $mode 2>&1 | Out-String).Trim()
                        if ($LASTEXITCODE -ne 0) { throw ('sc config failed: ' + $output) }
                    }
                    Set-RegistryState 'Machine' $path 'Start' $Entry.State
                } $effect
            }
            'Task' {
                $task = @(Get-ScheduledTask -ErrorAction Stop | Where-Object {
                    $_.TaskPath -eq $Entry.Path -and $_.TaskName -eq $Entry.Name
                })
                if (-not $task.Count) { Report-Absent $Entry.Id; return }
                $old = [bool]$task[0].Settings.Enabled
                Invoke-Change $Entry $old ([bool]$Entry.State) {
                    if ($Entry.State) { Enable-ScheduledTask -TaskPath $Entry.Path -TaskName $Entry.Name -ErrorAction Stop | Out-Null }
                    else { Disable-ScheduledTask -TaskPath $Entry.Path -TaskName $Entry.Name -ErrorAction Stop | Out-Null }
                    $check = Get-ScheduledTask -TaskPath $Entry.Path -TaskName $Entry.Name -ErrorAction Stop
                    if ([bool]$check.Settings.Enabled -ne [bool]$Entry.State) { throw 'Task restore verification failed.' }
                }
            }
            'Visual' {
                Assert-LiveUser $Entry.Sid
                $old = [WindowsiOSTuneNative]::GetVisual($Entry.Get)
                Invoke-Change $Entry $old ([bool]$Entry.State) {
                    [WindowsiOSTuneNative]::SetVisual($Entry.Set, [bool]$Entry.State)
                    if ([WindowsiOSTuneNative]::GetVisual($Entry.Get) -ne [bool]$Entry.State) { throw 'Visual restore verification failed.' }
                }
            }
            'Power' {
                $old = Read-Power $Entry.Scheme $Entry.Subgroup $Entry.Setting $Entry.Mode
                if ($null -eq $old) { Report-Absent $Entry.Id; return }
                Invoke-Change $Entry $old ([uint32]$Entry.State) { Set-Power $Entry $Entry.State }
            }
            'AppRemoval' { Write-Log ('IRREVERSIBLE ' + $Entry.Id + ': reinstall through the Microsoft Store; -Undo cannot restore apps.') }
        }
    } catch { Report-Failure $Entry.Id $old $Entry.State $_ }
}

try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run from an elevated Windows PowerShell prompt.' }
    if (-not [Environment]::Is64BitProcess) { throw 'Run the native 64-bit Windows PowerShell, not a 32-bit process.' }
    if (-not $WhatIf) {
        $script:Mutex = [Threading.Mutex]::new($false, 'Global\WindowsiOSTune')
        if (-not $script:Mutex.WaitOne(0)) { $script:Mutex.Dispose(); $script:Mutex = $null; throw 'Another tuning run is active.' }
        if (-not [IO.Directory]::Exists($dir)) { [void][IO.Directory]::CreateDirectory($dir) }
        $date = Get-Date
        do {
            $reportName = 'tune-' + $date.ToString('yyyyMMdd-HHmmss') + '.txt'
            $logPath = Join-Path $dir $reportName
            $date = $date.AddSeconds(1)
        } while ([IO.File]::Exists($logPath))
        $stream = [IO.File]::Open($logPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $script:Writer = [IO.StreamWriter]::new($stream, [Text.Encoding]::ASCII)
        $script:Writer.AutoFlush = $true
    }
    Write-Log ('WindowsiOS performance tuning; Undo={0}; WhatIf={1}' -f [bool]$Undo, [bool]$WhatIf)
    if ($WhatIf) { Write-Log 'Read-only preview: no logs, settings, backups or uploads are written.' }
    Write-Log 'Only the explicit tuning allowlists are changed. Microsoft Defender remains untouched.'
    try {
        if ([IO.File]::Exists($backupPath)) {
            $script:Backup = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($backupPath))
            if ($script:Backup.Version -ne 1 -or $null -eq $script:Backup.Entries) { throw 'Unknown or invalid backup format.' }
            $ids = @{}
            foreach ($entry in $script:Backup.Entries) {
                if (-not $entry.Id -or $ids.ContainsKey($entry.Id)) { throw 'Invalid or duplicate backup entry ID.' }
                $ids[$entry.Id] = $true
            }
        }
        $script:BackupReady = $true
    } catch { Report-Failure 'Load original-state backup' $null $backupPath $_ }
    try { Initialize-Native } catch { Report-Failure 'Initialize native helpers' $null 'Available' $_ }
    try { Initialize-User } catch { Report-Failure 'Identify desktop user' $null 'Unique desktop SID' $_ }
    if ($Undo) {
        Write-Log 'Undo restores recorded settings, not removed apps or previously running service processes.'
        if (-not [IO.File]::Exists($backupPath)) { Report-Absent ('Backup ' + $backupPath) }
        if ($script:BackupReady) {
            $entries = @($script:Backup.Entries)
            [array]::Reverse($entries)
            foreach ($entry in $entries) { Undo-Entry $entry }
        }
    } else {
        Tune-Apps
        foreach ($path in $oobeKeys) { Tune-OobeKey $path }
        foreach ($name in $disabledServices) { Tune-Service $name 4 }
        foreach ($name in $manualServices) { Tune-Service $name 3 }
        Tune-Tasks
        foreach ($spec in $machineValues) { Tune-Registry 'Machine' $spec[0] $spec[1] $spec[2] }
        try {
            $ram = [Math]::Ceiling([double](Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory / 1024)
            if ($ram -le 0 -or $ram -gt [uint32]::MaxValue) { throw 'Installed RAM is outside the supported DWORD range.' }
            Tune-Registry 'Machine' 'SYSTEM\CurrentControlSet\Control' 'SvcHostSplitThresholdInKB' ([long]$ram) 'Restart'
        } catch { Report-Failure 'Installed RAM / SvcHostSplitThresholdInKB' $null 'RAM in KB rounded up' $_ }
        foreach ($spec in $userValues) {
            if ($script:UserSid) { Tune-Registry $script:UserSid $spec[0] $spec[1] $spec[2] }
            else { try { throw 'Desktop user could not be identified.' } catch { Report-Failure ('Desktop ' + $spec[1]) $null $spec[2] $_ } }
        }
        try { Initialize-Default } catch { Report-Failure 'Load default profile hive' $null 'Loaded' $_ }
        foreach ($spec in $userValues) { Tune-Registry 'Default' $spec[0] $spec[1] $spec[2] }
        foreach ($visual in $visuals) { Tune-Visual $visual }
        Tune-Power
    }
    Apply-ActivePower
} catch { Report-Failure 'Tuning initialization or unexpected error' $null $null $_ }
finally {
    if ($script:DefaultKey) { $script:DefaultKey.Dispose(); $script:DefaultKey = $null }
    if ($script:DefaultMounted) {
        try {
            $output = (& reg.exe unload ('HKU\' + $defaultMount) 2>&1 | Out-String).Trim()
            if ($LASTEXITCODE -ne 0) { throw ('reg unload failed: ' + $output) }
            Write-Log 'OK unloaded the default profile hive'
        } catch { Report-Failure 'Unload default profile hive' 'Loaded' 'Unloaded' $_ }
    }
    Write-Log ('Summary: changed={0}; already set={1}; not present={2}; failed={3}; would change={4}; protected={5}' -f
        $script:Counts.Changed, $script:Counts.AlreadySet, $script:Counts.NotPresent, $script:Counts.Failed, $script:Counts.WouldChange, $script:Counts.Protected)
    if ($script:Counts.Changed -eq 0 -and $script:Counts.WouldChange -eq 0 -and $script:Counts.Failed -eq 0) {
        Write-Log 'No changes needed.'
    }
    $qualifier = if ($WhatIf) { ' after applying the preview' } else { '' }
    Write-Log ('Sign-out needed for this run{0}: {1}; restart needed for this run{0}: {2}. A restart includes sign-out.' -f
        $qualifier, $script:SignOut, $script:Restart)
    if ($script:Writer) {
        Write-Log ('Log: ' + $logPath)
        Write-Log ('Uploading this log to http://10.0.2.4/' + $reportName)
        $script:Writer.Dispose()
        $script:Writer = $null
        try {
            $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
            if (-not $curl) { throw 'curl.exe is not available.' }
            $output = (& $curl.Source -s --connect-timeout 5 --max-time 60 -w '%{http_code}' -T $logPath ('http://10.0.2.4/' + $reportName) 2>&1 | Out-String).Trim()
            $rc = $LASTEXITCODE
            if ($rc -ne 0 -or $output -notmatch '^(200|201|204)$') {
                throw ('curl exit {0}, HTTP/output {1}; the local log is retained.' -f $rc, $output)
            }
            Write-Host ('Upload: OK (HTTP ' + $output + ')')
        } catch { Write-Host ('Upload: FAILED: ' + $_.Exception.Message) }
    }
    if ($script:Mutex) { $script:Mutex.ReleaseMutex(); $script:Mutex.Dispose() }
}
if ($script:Counts.Failed -gt 0) { exit 1 }
exit 0
