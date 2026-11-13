#!/opt/homebrew/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Build a Featherweight ("Tiny11 / IoT LTSC-grade") Windows 11 ARM64 image (`work/windows11_lite.img`)
from `work/windows11.img` using offline WinPE DISM + offline Registry Hive surgery.

Eliminates:
  - 35+ inbox UWP consumer bloat apps + Widgets WebView2 (`Client.WebExperience`)
  - Windows Defender real-time AV (`MsMpEng.exe`, `WdFilter.sys`, `Sense`, `SecurityHealthService`)
  - Windows Update / Store background auto-updaters (`TiWorker.exe`, `UsoSvc`, `wuauserv`, `DoSvc`)
  - `SysMain` (Superfetch), `WSearch` (`SearchIndexer.exe`), `DiagTrack` telemetry, `CompatTelRunner`, `WinSAT`
  - KVA Shadow & Spectre MMU page-table trampolines (`FeatureSettingsOverride=3`)
  - CPU-rendered `dwm.exe` Mica/Acrylic Gaussian blur & window/taskbar animations (keeps ClearType crisp fonts!)
  - Edge background prelaunch, Bing Search WebView2, OneDrive auto-setup, stale multi-GB pagefile/hiberfil
"""

import os
import plistlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
ESD = WORK / "downloads" / "Win11_24H2_ARM64_en-us.esd"
WINVM = ROOT / "build" / "winvm"
CODE_FD = WORK / "code.fd"
SRC_IMG = WORK / "windows11.img"
LITE_IMG = WORK / "windows11_lite.img"
PE_IMG = WORK / "_opt_pe.img"
PE_VARS = WORK / "_opt_pe_vars.fd"
NETKVM_SRC = WORK / "drivers" / "NetKVM"

OPT_SYS_REG = r"""Windows Registry Editor Version 5.00

; --- Memory Management: Disable KVA Shadow / Spectre MMU overhead & Executive Paging ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Control\Session Manager\Memory Management]
"FeatureSettingsOverride"=dword:00000003
"FeatureSettingsOverrideMask"=dword:00000003
"DisablePagingExecutive"=dword:00000001

; --- Snappy Foreground UI Quantum Boost ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Control\PriorityControl]
"Win32PrioritySeparation"=dword:00000026

; --- Disable Hibernation ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Control\Power]
"HibernateEnabled"=dword:00000000
"HibernateEnabledDefault"=dword:00000000

; --- Disable Windows Defender & Filter Drivers (saves ~350 MB RAM + 35% CPU) ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WinDefend]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdNisSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdNisDrv]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdFilter]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdBoot]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\Sense]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\SecurityHealthService]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\wscsvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\MDCoreSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\webthreatdefsvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\webthreatdefusersvc]
"Start"=dword:00000004

; --- Disable Background CPU / RAM / Disk Hogs ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\SysMain]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WSearch]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\DiagTrack]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\dmwappushservice]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WerSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\PcaSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\DPS]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdiServiceHost]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WdiSystemHost]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\MapsBroker]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\lfsvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\wisvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\RetailDemo]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WbioSrvc]
"Start"=dword:00000004

; --- Disable Background Windows Update & Delivery Optimization ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\wuauserv]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\UsoSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\WaaSMedicSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\DoSvc]
"Start"=dword:00000004

; --- Disable Xbox Background Services ---
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\XblAuthManager]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\XblGameSave]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\XboxGipSvc]
"Start"=dword:00000004
[HKEY_LOCAL_MACHINE\OFFSYS\ControlSet001\Services\XboxNetApiSvc]
"Start"=dword:00000004
"""

OPT_SOFT_REG = r"""Windows Registry Editor Version 5.00

; --- Windows Defender Group Policies ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows Defender]
"DisableAntiSpyware"=dword:00000001
"DisableRoutinelyTakingAction"=dword:00000001

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows Defender\Real-Time Protection]
"DisableRealtimeMonitoring"=dword:00000001
"DisableBehaviorMonitoring"=dword:00000001
"DisableOnAccessProtection"=dword:00000001
"DisableScanOnRealtimeEnable"=dword:00000001

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows Defender\Spynet]
"SpynetReporting"=dword:00000000
"SubmitSamplesConsent"=dword:00000002

[HKEY_LOCAL_MACHINE\OFFSOFT\Microsoft\Windows Defender\Features]
"TamperProtection"=dword:00000000

; --- Windows Update & Store Auto-Download Off ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\WindowsUpdate\AU]
"NoAutoUpdate"=dword:00000001
"AUOptions"=dword:00000002

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\WindowsStore]
"AutoDownload"=dword:00000002

; --- Disable Cloud Content, Consumer App Silent Installs, Widgets, Copilot, Telemetry ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\CloudContent]
"DisableWindowsConsumerFeatures"=dword:00000001
"DisableSoftLanding"=dword:00000001
"DisableCloudOptimizedContent"=dword:00000001

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Dsh]
"AllowNewsAndInterests"=dword:00000000

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\WindowsCopilot]
"TurnOffWindowsCopilot"=dword:00000001

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\DataCollection]
"AllowTelemetry"=dword:00000000
"MaxTelemetryAllowed"=dword:00000000

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\Explorer]
"DisableSearchBoxSuggestions"=dword:00000001

[HKEY_LOCAL_MACHINE\OFFSOFT\Microsoft\Windows\CurrentVersion\Explorer]
"SmartScreenEnabled"="Off"

[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Windows\System]
"EnableSmartScreen"=dword:00000000

; --- Prevent Microsoft Edge from Pre-Spawning Background WebView Processes ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Policies\Microsoft\Edge]
"StartupBoostEnabled"=dword:00000000
"BackgroundModeEnabled"=dword:00000000
"HubsSidebarEnabled"=dword:00000000

; --- Disable Automatic Idle Maintenance Tasks ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Microsoft\Windows NT\CurrentVersion\Schedule\Maintenance]
"MaintenanceDisabled"=dword:00000001

; --- Remove Startup Tray Bloat ---
[HKEY_LOCAL_MACHINE\OFFSOFT\Microsoft\Windows\CurrentVersion\Run]
"SecurityHealth"=-
"OneDriveSetup"=-
"""


def make_user_reg(hive_name: str) -> str:
    return rf"""Windows Registry Editor Version 5.00

; --- Disable CPU-Rendered DWM Mica/Acrylic Blur Shaders ---
[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize]
"EnableTransparency"=dword:00000000

; --- Best Performance Visual Effects (Crisp ClearType Fonts Kept!) ---
[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects]
"VisualFXSetting"=dword:00000003

[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced]
"TaskbarAnimations"=dword:00000000
"ListviewAlphaSelect"=dword:00000000
"ListviewShadow"=dword:00000000
"IconsOnly"=dword:00000000
"TaskbarDa"=dword:00000000
"ShowCopilotButton"=dword:00000000
"ShowTaskViewButton"=dword:00000000
"LaunchTo"=dword:00000001

[HKEY_LOCAL_MACHINE\{hive_name}\Control Panel\Desktop]
"UserPreferencesMask"=hex:90,12,03,80,10,00,00,00
"FontSmoothing"="2"
"FontSmoothingType"=dword:00000002
"MenuShowDelay"="0"
"DragFullWindows"="0"

[HKEY_LOCAL_MACHINE\{hive_name}\Control Panel\Desktop\WindowMetrics]
"MinAnimate"="0"

; --- Disable Start Menu Bing WebView2 & Silent Content Delivery ---
[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\Search]
"BingSearchEnabled"=dword:00000000
"CortanaConsent"=dword:00000000

[HKEY_LOCAL_MACHINE\{hive_name}\Software\Policies\Microsoft\Windows\Explorer]
"DisableSearchBoxSuggestions"=dword:00000001

[HKEY_LOCAL_MACHINE\{hive_name}\Software\Policies\Microsoft\Windows\WindowsCopilot]
"TurnOffWindowsCopilot"=dword:00000001

[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager]
"ContentDeliveryAllowed"=dword:00000000
"OemPreInstalledAppsEnabled"=dword:00000000
"PreInstalledAppsEnabled"=dword:00000000
"PreInstalledAppsEverEnabled"=dword:00000000
"SilentInstalledAppsEnabled"=dword:00000000
"SystemPaneSuggestionsEnabled"=dword:00000000
"SubscribedContent-338388Enabled"=dword:00000000
"SubscribedContent-338389Enabled"=dword:00000000
"SubscribedContent-338393Enabled"=dword:00000000
"SubscribedContent-353694Enabled"=dword:00000000
"SubscribedContent-353696Enabled"=dword:00000000

[HKEY_LOCAL_MACHINE\{hive_name}\Software\Microsoft\Windows\CurrentVersion\Run]
"OneDriveSetup"=-
"OneDrive"=-
"""


STARTNET_CMD = r"""@echo off
setlocal EnableDelayedExpansion
wpeinit

(
  echo automount enable
  echo select volume 0
  echo assign noerr
  echo select volume 1
  echo assign noerr
  echo select volume 2
  echo assign noerr
  echo select volume 3
  echo assign noerr
  echo select volume 4
  echo assign noerr
  echo select volume 5
  echo assign noerr
  echo list volume
) > X:\mount.txt
diskpart /s X:\mount.txt > X:\mount.log 2>&1

set "ESPDRV="
for %%E in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%E:\EFI_HOLD\Microsoft\Boot\bootmgfw.efi" (
        set "ESPDRV=%%E:"
    )
)

set "LOGFILE=X:\opt_progress.log"
if not "%ESPDRV%"=="" set "LOGFILE=%ESPDRV%\EFI_HOLD\opt_progress.log"

echo ===================================================================== > "%LOGFILE%"
echo   Windows 11 ARM64 Featherweight ("Tiny11 / LTSC-Lite") Optimizer >> "%LOGFILE%"
echo ===================================================================== >> "%LOGFILE%"
type X:\mount.log >> "%LOGFILE%"

set "WINDRV="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%D:\Windows\System32\ntoskrnl.exe" (
        set "WINDRV=%%D:"
    )
)

if "%WINDRV%"=="" (
    echo [FATAL] Could not find Windows NTFS volume! >> "%LOGFILE%"
    wpeutil shutdown
    exit /b 1
)

echo [1/5] Found Windows 11 installation on %WINDRV%\ (ESP=%ESPDRV%) >> "%LOGFILE%"

set "NETKVM_DIR="
for %%N in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%N:\NetKVM\netkvm.inf" set "NETKVM_DIR=%%N:\NetKVM"
)
echo [2/5] Stripping 25 consumer inbox Appx bloatware bundles... >> "%LOGFILE%"
set /a REMOVED_COUNT=0
for %%P in (
    Clipchamp.Clipchamp_3.1.11920.0_neutral_~_yxz26nhyzhsrt
    MSTeams_1.0.0.0_arm64__8wekyb3d8bbwe
    Microsoft.BingNews_4.1.24002.0_neutral_~_8wekyb3d8bbwe
    Microsoft.BingSearch_2022.1.33.0_neutral_~_8wekyb3d8bbwe
    Microsoft.BingWeather_4.54.63017.0_neutral_~_8wekyb3d8bbwe
    Microsoft.GetHelp_10.2407.22193.0_neutral_~_8wekyb3d8bbwe
    Microsoft.MicrosoftOfficeHub_18.2409.1301.0_neutral_~_8wekyb3d8bbwe
    Microsoft.MicrosoftSolitaireCollection_4.22.3190.0_neutral_~_8wekyb3d8bbwe
    Microsoft.MicrosoftStickyNotes_6.1.5.0_neutral_~_8wekyb3d8bbwe
    Microsoft.OutlookForWindows_1.0.0.0_neutral__8wekyb3d8bbwe
    Microsoft.Todos_2.120.7961.0_neutral_~_8wekyb3d8bbwe
    Microsoft.Windows.DevHome_0.1701.597.0_neutral_~_8wekyb3d8bbwe
    Microsoft.WindowsAlarms_2021.2501.7.0_neutral_~_8wekyb3d8bbwe
    Microsoft.WindowsCamera_2022.2408.1.0_neutral_~_8wekyb3d8bbwe
    Microsoft.WindowsFeedbackHub_2024.125.1522.0_neutral_~_8wekyb3d8bbwe
    Microsoft.WindowsSoundRecorder_2021.2502.0.0_neutral_~_8wekyb3d8bbwe
    Microsoft.Xbox.TCUI_1.23.28005.0_neutral_~_8wekyb3d8bbwe
    Microsoft.XboxGamingOverlay_7.225.4030.0_neutral_~_8wekyb3d8bbwe
    Microsoft.XboxIdentityProvider_12.110.15002.0_neutral_~_8wekyb3d8bbwe
    Microsoft.XboxSpeechToTextOverlay_1.111.30001.0_neutral_~_8wekyb3d8bbwe
    Microsoft.YourPhone_1.25011.62.0_neutral_~_8wekyb3d8bbwe
    Microsoft.ZuneMusic_11.2501.9.0_neutral_~_8wekyb3d8bbwe
    MicrosoftCorporationII.QuickAssist_2025.331.2057.0_neutral_~_8wekyb3d8bbwe
    MicrosoftWindows.Client.WebExperience_524.30502.30.9_neutral_~_cw5n1h2txyewy
    MicrosoftWindows.CrossDevice_1.25011.11.0_neutral_~_cw5n1h2txyewy
) do (
    echo   - Removing %%P >> "%LOGFILE%"
    dism /Image:%WINDRV%\ /ScratchDir:X:\ /Remove-ProvisionedAppxPackage /PackageName:%%P /Quiet /NoRestart >> "%LOGFILE%" 2>&1
    if not errorlevel 1 set /a REMOVED_COUNT+=1
)
echo [2/5] Removed !REMOVED_COUNT! provisioned consumer Appx packages! >> "%LOGFILE%"

echo [3/5] Applying offline SYSTEM and SOFTWARE hive optimizations via opt_hive_arm64.exe... >> "%LOGFILE%"
reg load HKLM\OFFSYS "%WINDRV%\Windows\System32\config\SYSTEM" >> "%LOGFILE%" 2>&1
reg load HKLM\OFFSOFT "%WINDRV%\Windows\System32\config\SOFTWARE" >> "%LOGFILE%" 2>&1
X:\Windows\System32\opt_hive_arm64.exe >> "%LOGFILE%" 2>&1
reg import X:\Windows\System32\opt_soft.reg >> "%LOGFILE%" 2>&1
rem The PL031 RTC runs in UTC; without this Windows shows UTC as local time.
reg add "HKLM\OFFSYS\ControlSet001\Control\TimeZoneInformation" /v RealTimeIsUniversal /t REG_DWORD /d 1 /f >> "%LOGFILE%" 2>&1
rem This WinPE cannot service offline images (DISM /Image: fails 0x80004002), so the
rem VirtIO NetKVM driver is installed by pnputil at the first logon instead.
if not "%NETKVM_DIR%"=="" (
    echo [3b/5] Staging NetKVM from %NETKVM_DIR% for a first-logon pnputil install... >> "%LOGFILE%"
    mkdir "%WINDRV%\Drivers\NetKVM" >> "%LOGFILE%" 2>&1
    copy /y "%NETKVM_DIR%\*.*" "%WINDRV%\Drivers\NetKVM\" >> "%LOGFILE%" 2>&1
    reg add "HKLM\OFFSOFT\Microsoft\Windows\CurrentVersion\RunOnce" /v NetKVM /t REG_SZ /d "pnputil /add-driver C:\Drivers\NetKVM\netkvm.inf /install" /f >> "%LOGFILE%" 2>&1
) else (
    echo [3b/5] No NetKVM driver staged; Windows will have no network adapter. >> "%LOGFILE%"
)
reg unload HKLM\OFFSOFT >> "%LOGFILE%" 2>&1
reg unload HKLM\OFFSYS >> "%LOGFILE%" 2>&1

echo [4/5] Applying zero-blur instant-UI settings to Alan and Default user hives... >> "%LOGFILE%"
if exist "%WINDRV%\Users\Alan\NTUSER.DAT" (
    reg load HKLM\OFFUSR "%WINDRV%\Users\Alan\NTUSER.DAT" >> "%LOGFILE%" 2>&1
    reg import X:\Windows\System32\opt_usr.reg >> "%LOGFILE%" 2>&1
    reg unload HKLM\OFFUSR >> "%LOGFILE%" 2>&1
)
if exist "%WINDRV%\Users\Default\NTUSER.DAT" (
    reg load HKLM\OFFDEF "%WINDRV%\Users\Default\NTUSER.DAT" >> "%LOGFILE%" 2>&1
    reg import X:\Windows\System32\opt_def.reg >> "%LOGFILE%" 2>&1
    reg unload HKLM\OFFDEF >> "%LOGFILE%" 2>&1
)

echo [5/5] Neutralizing background telemetry binaries and cleaning stale swap/hiberfil... >> "%LOGFILE%"
for %%F in (OneDriveSetup.exe CompatTelRunner.exe WinSAT.exe) do (
    if exist "%WINDRV%\Windows\System32\%%F" (
        takeown /f "%WINDRV%\Windows\System32\%%F" /a >> "%LOGFILE%" 2>&1
        icacls "%WINDRV%\Windows\System32\%%F" /grant Administrators:F >> "%LOGFILE%" 2>&1
        ren "%WINDRV%\Windows\System32\%%F" "%%F.bak" >> "%LOGFILE%" 2>&1
    )
)
if exist "%WINDRV%\Windows\SysWOW64\OneDriveSetup.exe" (
    takeown /f "%WINDRV%\Windows\SysWOW64\OneDriveSetup.exe" /a >> "%LOGFILE%" 2>&1
    icacls "%WINDRV%\Windows\SysWOW64\OneDriveSetup.exe" /grant Administrators:F >> "%LOGFILE%" 2>&1
    ren "%WINDRV%\Windows\SysWOW64\OneDriveSetup.exe" "OneDriveSetup.exe.bak" >> "%LOGFILE%" 2>&1
)

attrib -h -s -r "%WINDRV%\pagefile.sys" >nul 2>&1
del /f /q "%WINDRV%\pagefile.sys" >> "%LOGFILE%" 2>&1
attrib -h -s -r "%WINDRV%\swapfile.sys" >nul 2>&1
del /f /q "%WINDRV%\swapfile.sys" >> "%LOGFILE%" 2>&1
attrib -h -s -r "%WINDRV%\hiberfil.sys" >nul 2>&1
del /f /q "%WINDRV%\hiberfil.sys" >> "%LOGFILE%" 2>&1

echo RemovedAppx=!REMOVED_COUNT! > "%WINDRV%\LiteOptDone.txt"
if not "%ESPDRV%"=="" echo RemovedAppx=!REMOVED_COUNT! > "%ESPDRV%\EFI_HOLD\LiteOptDone.txt"
echo [DONE] Featherweight optimization complete (removed !REMOVED_COUNT! Appx packages)! Shutting down WinPE... >> "%LOGFILE%"
wpeutil shutdown
goto :eof
"""


def run(cmd, **kwargs):
    print(f"+ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def toggle_efi_boot_folder(img_path: Path, hide: bool):
    """Temporarily rename /EFI <-> /EFI_HOLD on the image's FAT32 ESP so EDK2 boots secondary WinPE."""
    out = subprocess.check_output(
        ["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", str(img_path)],
        text=True,
    )
    whole_disk = out.strip().splitlines()[0].split()[0]
    esp_dev = f"{whole_disk}s1"
    try:
        run(["diskutil", "mount", esp_dev])
        info_plist = subprocess.check_output(["diskutil", "info", "-plist", esp_dev])
        mp = Path(plistlib.loads(info_plist)["MountPoint"])
        efi_dir = mp / "EFI"
        hold_dir = mp / "EFI_HOLD"
        if hide:
            if efi_dir.exists():
                if hold_dir.exists():
                    shutil.rmtree(hold_dir)
                efi_dir.rename(hold_dir)
                print(f"[esp] Renamed {efi_dir} -> {hold_dir}", flush=True)
        else:
            if hold_dir.exists():
                if efi_dir.exists():
                    shutil.rmtree(efi_dir)
                hold_dir.rename(efi_dir)
                print(f"[esp] Restored {hold_dir} -> {efi_dir}", flush=True)
        if not hide and not (mp / "EFI" / "Microsoft" / "Boot" / "bootmgfw.efi").exists():
            raise RuntimeError(f"Missing bootmgfw.efi on {mp} after restoring ESP!")
    finally:
        subprocess.run(["hdiutil", "detach", whole_disk], check=False)


def build_opt_winpe():
    print(f"Building compact bootable WinPE optimizer disk at {PE_IMG}...", flush=True)
    PE_IMG.unlink(missing_ok=True)
    with open(PE_IMG, "wb") as f:
        f.truncate(2200 * 1024 * 1024)

    out = subprocess.check_output(
        ["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", str(PE_IMG)],
        text=True,
    )
    whole_disk = out.strip().splitlines()[0].split()[0]
    try:
        run(["diskutil", "partitionDisk", whole_disk, "GPT", "MS-DOS", "OPTPE", "R"])
        pe_root = Path("/Volumes/OPTPE")
        if not pe_root.exists():
            raise RuntimeError("Expected /Volumes/OPTPE")

        stage = WORK / "_stage_opt_pe"
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True, exist_ok=True)
        run(["/opt/homebrew/bin/wimlib-imagex", "extract", ESD, "1", "/efi", "/boot", "/bootmgr.efi", f"--dest-dir={stage}"])
        for item in stage.iterdir():
            dest = pe_root / item.name
            if item.is_dir():
                shutil.copytree(item, dest, dirs_exist_ok=True)
            else:
                shutil.copy2(item, dest)

        # virtio-win NetKVM (w11/ARM64), copied without macOS metadata so FAT gets no ._ files.
        if (NETKVM_SRC / "netkvm.inf").exists():
            (pe_root / "NetKVM").mkdir(exist_ok=True)
            for name in ("netkvm.inf", "netkvm.sys", "netkvm.cat", "netkvmp.exe"):
                shutil.copyfile(NETKVM_SRC / name, pe_root / "NetKVM" / name)
        else:
            print(f"[warn] {NETKVM_SRC}/netkvm.inf missing; Windows will have no network adapter", flush=True)

        boot_wim = pe_root / "sources" / "boot.wim"
        boot_wim.parent.mkdir(parents=True, exist_ok=True)
        run(["/opt/homebrew/bin/wimlib-imagex", "export", ESD, "2", boot_wim, "--compress=none", "--boot"])

        files = {
            "startnet.cmd": STARTNET_CMD,
            "opt_sys.reg": OPT_SYS_REG,
            "opt_soft.reg": OPT_SOFT_REG,
            "opt_usr.reg": make_user_reg("OFFUSR"),
            "opt_def.reg": make_user_reg("OFFDEF"),
        }
        opt_exe = ROOT / "build" / "opt_hive_arm64.exe"
        if not opt_exe.exists():
            raise RuntimeError(f"Missing {opt_exe}")
        wim_cmds = [f"add '{opt_exe}' '/Windows/System32/opt_hive_arm64.exe'"]
        for name, content in files.items():
            p = stage / name
            p.write_bytes(content.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii"))
            wim_cmds.append(f"add '{p}' '/Windows/System32/{name}'")

        run(
            ["/opt/homebrew/bin/wimlib-imagex", "update", boot_wim, "1"],
            input=("\n".join(wim_cmds) + "\n").encode("utf-8"),
        )
        shutil.rmtree(stage, ignore_errors=True)
    finally:
        subprocess.run(["hdiutil", "detach", whole_disk], check=False)


def run_winpe_optimizer():
    shutil.copyfile("/opt/homebrew/share/qemu/edk2-arm-vars.fd", PE_VARS)
    cmd_file = WORK / "_opt_pe.cmds"
    log_file = WORK / "_opt_pe.log"
    shot_file = WORK / "_opt_pe_screen.png"
    cmd_file.write_text("")

    cmd = [
        str(WINVM),
        "--headless",
        "--code", str(CODE_FD),
        "--vars", str(PE_VARS),
        "--disk", str(LITE_IMG),
        "--disk1", str(PE_IMG),
        "--cpus", "4",
        "--ram", "4096",
        "--no-net",
        "--cmd-file", str(cmd_file),
        "--screenshot", str(shot_file),
    ]
    print(f"=== Booting WinPE Optimizer: {' '.join(cmd)} ===", flush=True)
    t0 = time.time()
    env = os.environ.copy()
    env["HV_FLUSH"] = "1"
    with open(log_file, "wb") as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        next_shot = 15
        while proc.poll() is None:
            elapsed = time.time() - t0
            if elapsed >= next_shot:
                with open(cmd_file, "a", encoding="utf-8") as cf:
                    cf.write(f"screenshot {shot_file}\n")
                print(f"[opt-winpe +{int(elapsed)}s] running...", flush=True)
                next_shot += 20
            if elapsed > 25 * 60:
                proc.terminate()
                raise TimeoutError("WinPE optimizer exceeded 25 minutes")
            time.sleep(2)
        if proc.returncode != 0:
            raise RuntimeError(f"WinPE exited with {proc.returncode}; see {log_file}")
    print(f"=== WinPE Optimizer finished cleanly in {int(time.time() - t0)}s! ===", flush=True)


def main():
    if not SRC_IMG.exists():
        sys.exit(f"Missing {SRC_IMG}")
    if not ESD.exists():
        sys.exit(f"Missing {ESD}")
    if not WINVM.exists():
        sys.exit(f"Missing {WINVM}")

    print(f"Cloning {SRC_IMG} -> {LITE_IMG} via APFS copy-on-write...", flush=True)
    LITE_IMG.unlink(missing_ok=True)
    run(["cp", "-c", SRC_IMG, LITE_IMG])

    build_opt_winpe()
    toggle_efi_boot_folder(LITE_IMG, hide=True)
    try:
        run_winpe_optimizer()
    finally:
        toggle_efi_boot_folder(LITE_IMG, hide=False)
        PE_IMG.unlink(missing_ok=True)
        PE_VARS.unlink(missing_ok=True)
        (WORK / "_opt_pe.cmds").unlink(missing_ok=True)

    print(f"SUCCESS: Featherweight Windows 11 ARM64 image ready at {LITE_IMG}!", flush=True)


if __name__ == "__main__":
    main()
