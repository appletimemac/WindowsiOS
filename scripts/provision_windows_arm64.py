#!/opt/homebrew/bin/python3
"""
Provision a ready-to-boot Windows 11 Pro ARM64 (24H2, Build 26100) GPT NVMe disk image
and UEFI NVRAM vars file from the official Microsoft ARM64 retail ESD using our native
ARM64 JIT (`build/winvm`), WinPE DISM (from an unsplit ExFAT `install.wim`), and BCDBOOT.

All intermediate and output files stay strictly inside work/.
"""

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
WINVM = ROOT / "build" / "winvm"
ESD = WORK / "downloads" / "Win11_24H2_ARM64_en-us.esd"
INST_IMG = WORK / "installer_v4.img"
WIN_IMG = WORK / "windows11.img"
CODE_FD = WORK / "code.fd"
VARS_FD = WORK / "vars.fd"

STARTNET_CMD = r"""@echo off
wpeinit
echo [WIN11-PROVISION] WinPE booted on winvm ARM64 JIT! Locating 32 GB target NVMe disk...
echo list disk > X:\list.txt
diskpart /s X:\list.txt > X:\disks.log
type X:\disks.log

set TARGET_DISK=
for /f "tokens=1,2,3,4,5" %%a in (X:\disks.log) do (
  if /i "%%a"=="Disk" if /i "%%d %%e"=="32 GB" set TARGET_DISK=%%b
)
if "%TARGET_DISK%"=="" set TARGET_DISK=0
echo [WIN11-PROVISION] Partitioning Disk %TARGET_DISK% as GPT (EFI + MSR + NTFS)...
(
  echo select disk %TARGET_DISK%
  echo clean
  echo convert gpt
  echo create partition efi size=100
  echo format quick fs=fat32 label="System"
  echo assign letter=S
  echo create partition msr size=16
  echo create partition primary
  echo format quick fs=ntfs label="Windows"
  echo assign letter=W
) > X:\part.txt
diskpart /s X:\part.txt

set INST_WIM=
for %%l in (C D E F G H I J K L) do (
  if exist %%l:\sources\install.wim set INST_WIM=%%l:\sources\install.wim
)
if "%INST_WIM%"=="" (
  echo [WIN11-PROVISION] ERROR: Could not find unsplit sources\install.wim!
  ping -n 30 127.0.0.1 >nul
  exit /b 1
)

echo [WIN11-PROVISION] Applying unsplit Windows 11 Pro ARM64 image (%INST_WIM%) to W:\ ...
dism /Apply-Image /ImageFile:%INST_WIM% /Index:1 /ApplyDir:W:\
if errorlevel 1 (
  echo [WIN11-PROVISION] ERROR: DISM Apply-Image failed!
  find /i "Error" X:\windows\Logs\DISM\dism.log
  ping -n 60 127.0.0.1 >nul
  exit /b 1
)

echo [WIN11-PROVISION] Injecting unattend.xml and offline registry settings...
mkdir W:\Windows\Panther
copy /y X:\Windows\System32\unattend_panther.xml W:\Windows\Panther\unattend.xml

reg load HKLM\OFFSYS W:\Windows\System32\config\SYSTEM
reg add HKLM\OFFSYS\ControlSet001\Control\BitLocker /v PreventDeviceEncryption /t REG_DWORD /d 1 /f
reg add HKLM\OFFSYS\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f
reg add HKLM\OFFSYS\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f
reg add HKLM\OFFSYS\Setup\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f
reg add HKLM\OFFSYS\Setup\LabConfig /v BypassCPUCheck /t REG_DWORD /d 1 /f
reg unload HKLM\OFFSYS

reg load HKLM\OFFSOFT W:\Windows\System32\config\SOFTWARE
reg add HKLM\OFFSOFT\Microsoft\Windows\CurrentVersion\OOBE /v BypassNRO /t REG_DWORD /d 1 /f
reg unload HKLM\OFFSOFT

echo [WIN11-PROVISION] Installing UEFI Windows Boot Manager via bcdboot...
bcdboot W:\Windows /s S: /f UEFI
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} recoveryenabled no
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} bootmenupolicy standard

echo [WIN11-PROVISION] Phase 1 DISM + BCDBOOT complete! Shutting down cleanly...
wpeutil shutdown
"""

PANTHER_UNATTEND_XML = r"""<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <ComputerName>IPHONE-DUO</ComputerName>
      <TimeZone>Central Standard Time</TimeZone>
    </component>
    <component name="Microsoft-Windows-Deployment" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Path>reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add">
          <Order>2</Order>
          <Path>reg add HKLM\SYSTEM\CurrentControlSet\Control\BitLocker /v PreventDeviceEncryption /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
      </RunSynchronous>
    </component>
  </settings>

  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <InputLocale>0409:00000409</InputLocale>
      <SystemLocale>en-US</SystemLocale>
      <UILanguage>en-US</UILanguage>
      <UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideLocalAccountScreen>true</HideLocalAccountScreen>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Password>
              <Value></Value>
              <PlainText>true</PlainText>
            </Password>
            <Description>Alan</Description>
            <DisplayName>Alan</DisplayName>
            <Group>Administrators</Group>
            <Name>Alan</Name>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Password>
          <Value></Value>
          <PlainText>true</PlainText>
        </Password>
        <Enabled>true</Enabled>
        <Username>Alan</Username>
      </AutoLogon>
      <FirstLogonCommands>
        <SynchronousCommand wcm:action="add">
          <Order>1</Order>
          <CommandLine>cmd /c "powercfg /h off &amp; powercfg /change monitor-timeout-ac 0 &amp; powercfg /change standby-timeout-ac 0"</CommandLine>
          <Description>Disable hibernation and display/sleep timeouts</Description>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>2</Order>
          <CommandLine>cmd /c "bcdedit /set {default} bootmenupolicy standard &amp; bcdedit /set {default} recoveryenabled no"</CommandLine>
          <Description>Disable automatic recovery loops on hard VM stop</Description>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>3</Order>
          <CommandLine>cmd /c "timeout /t 15 /nobreak &amp; shutdown /s /t 2 /f"</CommandLine>
          <Description>Clean shutdown once first desktop logon settles</Description>
        </SynchronousCommand>
      </FirstLogonCommands>
    </component>
  </settings>
</unattend>
"""


def run(cmd, **kwargs):
    print(f"+ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def make_custom_boot_wim(dest_boot_wim: Path):
    dest_boot_wim.parent.mkdir(parents=True, exist_ok=True)
    if dest_boot_wim.exists():
        dest_boot_wim.unlink()
    run(["/opt/homebrew/bin/wimlib-imagex", "export", ESD, "2", dest_boot_wim, "--compress=LZX", "--boot"])
    snet = WORK / "startnet.cmd"
    unat = WORK / "unattend_panther.xml"
    snet.write_text(STARTNET_CMD.replace("\n", "\r\n"), encoding="utf-8")
    unat.write_text(PANTHER_UNATTEND_XML.replace("\n", "\r\n"), encoding="utf-8")
    cmds = (
        f"add '{snet}' '/Windows/System32/startnet.cmd'\n"
        f"add '{unat}' '/Windows/System32/unattend_panther.xml'\n"
    ).encode("utf-8")
    run(["/opt/homebrew/bin/wimlib-imagex", "update", dest_boot_wim, "1"], input=cmds)
    snet.unlink(missing_ok=True)
    unat.unlink(missing_ok=True)


def build_two_partition_installer():
    marker = WORK / ".installer_exfat_v4"
    if INST_IMG.exists() and marker.exists():
        print(f"[skip] {INST_IMG} already built ({INST_IMG.stat().st_size} bytes)", flush=True)
        return

    print("Building 2-partition GPT installer (FAT32 WinPE + ExFAT unsplit install.wim)...", flush=True)
    INST_IMG.unlink(missing_ok=True)
    with open(INST_IMG, "wb") as f:
        f.truncate(6400 * 1024 * 1024)

    out = subprocess.check_output(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", str(INST_IMG)], text=True)
    whole_disk = out.strip().splitlines()[0].split()[0]
    print(f"Attached raw installer disk as {whole_disk}", flush=True)
    try:
        run([
            "diskutil", "partitionDisk", whole_disk, "GPT",
            "MS-DOS", "WINPE", "900M",
            "ExFAT", "WINWIM", "R",
        ])
        pe_root = Path("/Volumes/WINPE")
        wim_root = Path("/Volumes/WINWIM")
        if not pe_root.exists() or not wim_root.exists():
            raise RuntimeError(f"Expected /Volumes/WINPE and /Volumes/WINWIM after partitionDisk on {whole_disk}")

        # Extract small UEFI boot files (Index 1, skipping install.esd/boot.wim) into FAT32 WINPE
        stage_pe = WORK / "stage_pe_v4"
        shutil.rmtree(stage_pe, ignore_errors=True)
        stage_pe.mkdir(parents=True, exist_ok=True)
        run(["/opt/homebrew/bin/wimlib-imagex", "extract", ESD, "1", "/efi", "/boot", "/bootmgr.efi", f"--dest-dir={stage_pe}"])
        for item in stage_pe.iterdir():
            dest = pe_root / item.name
            if item.is_dir():
                shutil.copytree(item, dest, dirs_exist_ok=True)
            else:
                shutil.copy2(item, dest)
        shutil.rmtree(stage_pe, ignore_errors=True)

        make_custom_boot_wim(pe_root / "sources" / "boot.wim")

        # Export unsplit Windows 11 Pro (Index 6) directly onto ExFAT WINWIM!
        (wim_root / "sources").mkdir(parents=True, exist_ok=True)
        run(["/opt/homebrew/bin/wimlib-imagex", "export", ESD, "6", wim_root / "sources" / "install.wim", "--compress=LZX"])
    finally:
        subprocess.run(["hdiutil", "detach", whole_disk], check=False)

    marker.write_text("ok\n")
    # Clean old installer.dmg to save space inside work/
    old_dmg = WORK / "installer.dmg"
    if old_dmg.exists():
        old_dmg.unlink()


def run_winvm_phase(phase_name: str, extra_args: list[str], screen_png: Path, max_minutes: int):
    cmd_file = WORK / f"prov_{phase_name}.cmds"
    log_file = WORK / f"prov_{phase_name}.log"
    cmd_file.write_text("")
    cmd = [
        str(WINVM),
        "--headless",
        "--code", str(CODE_FD),
        "--vars", str(VARS_FD),
        "--disk", str(WIN_IMG),
        "--cpus", "4",
        "--ram", "4096",
        "--no-net",
        "--cmd-file", str(cmd_file),
        "--screenshot", str(screen_png),
        *extra_args,
    ]
    print(f"\n=== Starting {phase_name}: {' '.join(cmd)} ===", flush=True)
    t0 = time.time()
    with open(log_file, "wb") as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
        next_shot = 15
        while proc.poll() is None:
            elapsed = time.time() - t0
            if elapsed >= next_shot:
                with open(cmd_file, "a", encoding="utf-8") as cf:
                    cf.write(f"screenshot {screen_png}\n")
                st = subprocess.check_output(["du", "-sh", str(WIN_IMG)], text=True).split()[0]
                print(f"[{phase_name} +{int(elapsed)}s] running (windows11.img={st}, screen={screen_png.name})", flush=True)
                next_shot += 20
            if elapsed > max_minutes * 60:
                proc.terminate()
                raise TimeoutError(f"{phase_name} exceeded {max_minutes} minutes")
            time.sleep(2)
        if proc.returncode != 0:
            raise RuntimeError(f"{phase_name} exited with code {proc.returncode}; check {log_file}")
    print(f"=== {phase_name} finished cleanly in {int(time.time() - t0)}s! ===", flush=True)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    if not ESD.exists():
        sys.exit(f"Missing {ESD}")
    if not WINVM.exists():
        sys.exit(f"Missing {WINVM}")

    build_two_partition_installer()

    shutil.copyfile("/opt/homebrew/share/qemu/edk2-aarch64-code.fd", CODE_FD)
    shutil.copyfile("/opt/homebrew/share/qemu/edk2-arm-vars.fd", VARS_FD)

    if WIN_IMG.exists():
        WIN_IMG.unlink()
    with open(WIN_IMG, "wb") as f:
        f.truncate(32 * 1024 * 1024 * 1024)

    # Phase 1: Boot WinPE -> partition -> DISM unsplit install.wim -> offline reg + unattend -> bcdboot -> wpeutil shutdown
    run_winvm_phase("phase1_dism", ["--disk1", str(INST_IMG)], WORK / "prov_phase1.png", max_minutes=40)
    shutil.copyfile(VARS_FD, WORK / "vars_post_dism.fd")

    # Phase 2: Boot installed Windows 11 Pro ARM64 -> specialize -> reboot -> oobeSystem -> AutoLogon Alan -> clean shutdown
    run_winvm_phase("phase2_oobe", [], WORK / "prov_phase2.png", max_minutes=60)
    shutil.copyfile(VARS_FD, WORK / "vars_clean.fd")
    print("SUCCESS: Windows 11 Pro ARM64 provisioned and cleanly shut down!", flush=True)


if __name__ == "__main__":
    main()
