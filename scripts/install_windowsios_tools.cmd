@echo off
rem Installs the WindowsiOS guest pieces from the WINIOSTOOLS disk. Safe to run twice.
rem Copied to the disk as "Install WindowsiOS Tools.cmd" by scripts/build_tools_disk.sh (CRLF enforced there).
rem
rem Not set because Microsoft documents no registry location for them:
rem   touch keyboard "show when no keyboard attached" (only a Policy CSP and an unattend setting exist),
rem   touch-optimized taskbar toggle, ConvertibleSlateModePromptPreference (unattend setting only).
setlocal EnableExtensions
title Install WindowsiOS Tools

set "SRC=%~dp0"
set "DEST=%ProgramFiles%\WindowsiOS"
set "TASK=WindowsiOS Agent"
set "FAILS=0"
set "TWOSCREENS=0"

fsutil dirty query "%SystemDrive%" >nul 2>&1
if not errorlevel 1 goto :admin
if /i "%~1"=="elevated" goto :noadmin
echo Asking for administrator rights...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs" >nul 2>&1
if errorlevel 1 goto :noadmin
exit /b 0

:noadmin
echo.
echo This needs administrator rights and they were not granted.
pause
exit /b 1

:admin
echo ================================================
echo  WindowsiOS Tools
echo ================================================
echo.
call :step_files
call :step_service
call :step_task
call :step_settings
call :step_updates
call :step_hibernate
call :step_tune
rem Last, back to back: while the driver runs and Basic Display is still on, Windows extends the
rem desktop onto the virtio-gpu screen and this window stays on the other one, which the app does not show.
call :step_driver
call :step_display
goto :summary

:fail
set /a FAILS+=1
echo   FAILED: %~1
exit /b

:step_driver
echo [8/9] Display driver (virtio-gpu)
echo   The screen may change for a moment. Once the sharp display is set up, Windows restarts by itself.
if exist "%SRC%drivers\viogpudo\viogpudo.inf" goto :drv_go
call :fail "viogpudo.inf is missing from the tools disk"
exit /b
:drv_go
pnputil /add-driver "%SRC%drivers\viogpudo\viogpudo.inf" /install >nul
set "RC=%errorlevel%"
if "%RC%"=="0" exit /b
if "%RC%"=="3010" exit /b
rem 259: staged, but no virtio-gpu device in this VM yet; it binds when one appears.
if "%RC%"=="259" exit /b
call :fail "pnputil returned %RC% for viogpudo.inf"
exit /b

:step_files
echo [1/9] Program files in %DEST%
if not exist "%DEST%" mkdir "%DEST%"
sc stop vgpusrv >nul 2>&1
taskkill /f /im vgpusrv.exe >nul 2>&1
schtasks /end /tn "%TASK%" >nul 2>&1
taskkill /f /im WinIOSAgent.exe >nul 2>&1
taskkill /f /im viogpuap.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
rem viogpuap.exe goes with the service: vgpusrv starts it in the user's session from its own folder.
call :copyfile "%SRC%drivers\viogpudo\vgpusrv.exe" vgpusrv.exe
call :copyfile "%SRC%drivers\viogpudo\viogpuap.exe" viogpuap.exe
call :copyfile "%SRC%WinIOSAgent.exe" WinIOSAgent.exe
call :copyfile "%SRC%WinIOSBrowser.exe" WinIOSBrowser.exe
exit /b
:copyfile
copy /y "%~1" "%DEST%\%~2" >nul
if errorlevel 1 call :fail "could not copy %~2"
exit /b

:step_service
echo [2/9] Display resolution service (vgpusrv)
sc query vgpusrv >nul 2>&1
if not errorlevel 1 goto :svc_have
"%DEST%\vgpusrv.exe" -i >nul 2>&1
sc query vgpusrv >nul 2>&1
if not errorlevel 1 goto :svc_have
echo   vgpusrv.exe -i did not register the service, using sc create instead
sc create vgpusrv binPath= "\"%DEST%\vgpusrv.exe\"" start= auto DisplayName= "VioGpu Resolution Service" >nul
if not errorlevel 1 goto :svc_have
call :fail "could not create the vgpusrv service"
exit /b
:svc_have
sc config vgpusrv start= auto >nul
sc start vgpusrv >nul 2>&1
exit /b

:step_task
echo [3/9] Posture agent (scheduled task "%TASK%")
schtasks /create /f /sc onlogon /rl highest /tn "%TASK%" /tr "\"%DEST%\WinIOSAgent.exe\"" >nul
if not errorlevel 1 goto :task_ok
call :fail "schtasks /create failed"
exit /b
:task_ok
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew; Set-ScheduledTask -TaskName 'WindowsiOS Agent' -Settings $s | Out-Null } catch { exit 1 }" >nul 2>&1
if errorlevel 1 echo   note: could not lift the task's battery and 72 hour limits
schtasks /run /tn "%TASK%" >nul 2>&1
if errorlevel 1 call :fail "could not start the agent now, it starts at next sign-in"
exit /b

:step_settings
echo [4/9] Tablet mode
call :regdword "HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl" ConvertibilityEnabled 1
exit /b
:step_updates
echo [5/9] Windows Update on demand
rem The image's optimizer disabled these services. Manual start lets Settings check for and
rem install updates when asked, while the NoAutoUpdate policy keeps it from running on its own.
call :svc_manual wuauserv
call :svc_manual UsoSvc
call :svc_manual DoSvc
exit /b
:step_hibernate
echo [6/9] Hibernation (the app's Hibernate action)
powercfg /h on >nul 2>&1
if errorlevel 1 goto :hib_fail
rem The default "reduced" hiberfile only serves Fast Startup; shutdown /h needs a full one.
powercfg /h /type full >nul 2>&1
if errorlevel 1 goto :hib_fail
rem No sign-in prompt when it resumes: the account has no password anyway.
powercfg /setacvalueindex scheme_current sub_none consolelock 0 >nul 2>&1
powercfg /setdcvalueindex scheme_current sub_none consolelock 0 >nul 2>&1
powercfg /setactive scheme_current >nul 2>&1
exit /b
:hib_fail
call :fail "powercfg could not turn on hibernation"
exit /b
:svc_manual
sc config %1 start= demand >nul 2>&1
if not errorlevel 1 exit /b
reg add "HKLM\SYSTEM\CurrentControlSet\Services\%1" /v Start /t REG_DWORD /d 3 /f >nul 2>&1
if errorlevel 1 call :fail "could not set the %1 service to manual start"
exit /b

:step_tune
echo [7/9] Performance tuning
if exist "%SRC%winios_tune.ps1" goto :tune_go
call :fail "winios_tune.ps1 is missing from the tools disk"
exit /b
:tune_go
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%winios_tune.ps1"
if errorlevel 1 call :fail "performance tuning reported failures; see C:\ProgramData\WindowsiOS\tune-*.txt"
exit /b

:step_display
echo [9/9] Display adapter
rem Basic Display drives the firmware framebuffer as a second monitor. It is in the System class on ARM64,
rem so it is found by instance id, as is the virtio-gpu (no setup class until its driver binds). @() keeps
rem one device an array: .Count on a lone CimInstance is empty in Windows PowerShell. Basic Display
rem is only disabled once the virtio-gpu driver runs, so a VM without the device (or with the driver failing)
rem keeps a working screen. Exit codes: 2 no virtio-gpu, 5 its driver is not running, 3 Basic Display is off,
rem 6 both are running (two screens).
powershell -NoProfile -ExecutionPolicy Bypass -Command "$q={ Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | ? { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } }; $ok={ @($args[0] | ? { $_.Class -eq 'Display' -and $_.Status -eq 'OK' }).Count }; $g=@(& $q); if(-not $g.Count){ exit 2 }; for($i=0; $i -lt 15 -and -not (& $ok $g); $i++){ Start-Sleep 3; $g=@(& $q) }; if(-not (& $ok $g)){ exit 5 }; $b=@(Get-PnpDevice -InstanceId 'ROOT\BASICDISPLAY\*' -PresentOnly -ErrorAction SilentlyContinue | ? { $_.Status -eq 'OK' }); if(-not $b.Count){ exit 3 }; exit 6"
set "RC=%errorlevel%"
if "%RC%"=="2" echo   No virtio-gpu display device in this VM. Basic Display left on.
if "%RC%"=="5" echo   The virtio-gpu driver is installed but not running yet. Restart, then run this tool once more to finish this step.
if "%RC%"=="3" echo   Basic Display is already off. Nothing to do.
if "%RC%"=="3" call :scaling
if "%RC%"=="2" exit /b
if "%RC%"=="3" exit /b
if "%RC%"=="5" exit /b
if not "%RC%"=="6" call :fail "the display adapter check returned %RC%"
if not "%RC%"=="6" exit /b
set "TWOSCREENS=1"
rem The disable takes effect at the restart: until then Basic Display reports itself running.
call :disable_basic
if "%RC%"=="0" goto :disp_done
if "%RC%"=="3010" goto :disp_done
call :fail "pnputil returned %RC% disabling Basic Display"
exit /b
:disp_done
echo   Basic Display disabled: after the restart Windows uses the virtio-gpu display only.
call :scaling
exit /b
:disable_basic
pnputil /disable-device /deviceid "ROOT\BasicDisplay" >nul
set "RC=%errorlevel%"
if "%RC%"=="0" exit /b
if "%RC%"=="3010" exit /b
rem Windows may count its only running display adapter as critical system functionality.
pnputil /disable-device /deviceid "ROOT\BasicDisplay" /force >nul
set "RC=%errorlevel%"
exit /b
rem 150 percent makes one Windows pixel one iOS point at the 1.5 pixels per point the app asks for.
rem Only with the virtio-gpu display: at the firmware's 800x600 it would leave no room.
:scaling
call :regdword "HKCU\Control Panel\Desktop" LogPixels 144
call :regdword "HKCU\Control Panel\Desktop" Win8DpiScaling 1
call :regdword "HKU\.DEFAULT\Control Panel\Desktop" LogPixels 144
call :regdword "HKU\.DEFAULT\Control Panel\Desktop" Win8DpiScaling 1
exit /b

:regdword
reg add "%~1" /v %2 /t REG_DWORD /d %3 /f >nul
if errorlevel 1 call :fail "could not set %~1 %2"
exit /b

:summary
echo.
echo ------------------------------------------------
if "%FAILS%"=="0" echo   Done, no problems.
if not "%FAILS%"=="0" echo   Finished with %FAILS% problem(s), see the messages above.
if "%TWOSCREENS%"=="1" goto :autorestart
echo   Restart Windows so the new settings take effect.
echo.
choice /c YN /n /m "Restart now? [Y/N] "
if errorlevel 2 goto :done
shutdown /r /t 5 /c "WindowsiOS Tools setup finished"
exit /b 0
:done
echo.
pause
exit /b 0
:autorestart
rem With the virtio-gpu driver running and Basic Display still on, Windows extends the desktop onto the
rem virtio-gpu screen, which is the one the app shows, and this window stays on the other: nobody could
rem answer a prompt. The restart finishes the switch (or, if disabling failed, at least does not hang).
echo   Restarting in 10 seconds to finish the display switch.
shutdown /r /t 10 /c "WindowsiOS Tools: restarting to switch to the sharp display"
exit /b 0
