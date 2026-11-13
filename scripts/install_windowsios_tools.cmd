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
call :step_task
call :step_settings
call :step_updates
call :step_hibernate
goto :summary

:fail
set /a FAILS+=1
echo   FAILED: %~1
exit /b

:step_files
echo [1/5] Program files in %DEST%
if not exist "%DEST%" mkdir "%DEST%"
schtasks /end /tn "%TASK%" >nul 2>&1
taskkill /f /im WinIOSAgent.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
call :copyfile "%SRC%WinIOSAgent.exe" WinIOSAgent.exe
call :copyfile "%SRC%WinIOSBrowser.exe" WinIOSBrowser.exe
exit /b
:copyfile
copy /y "%~1" "%DEST%\%~2" >nul
if errorlevel 1 call :fail "could not copy %~2"
exit /b

:step_task
echo [2/5] Posture agent (scheduled task "%TASK%")
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
echo [3/5] Tablet mode
call :regdword "HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl" ConvertibilityEnabled 1
exit /b
:step_updates
echo [4/5] Windows Update on demand
rem The image's optimizer disabled these services. Manual start lets Settings check for and
rem install updates when asked, while the NoAutoUpdate policy keeps it from running on its own.
call :svc_manual wuauserv
call :svc_manual UsoSvc
call :svc_manual DoSvc
exit /b
:step_hibernate
echo [5/5] Hibernation (the app's Hibernate action)
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

:regdword
reg add "%~1" /v %2 /t REG_DWORD /d %3 /f >nul
if errorlevel 1 call :fail "could not set %~1 %2"
exit /b

:summary
echo.
echo ------------------------------------------------
if "%FAILS%"=="0" echo   Done, no problems.
if not "%FAILS%"=="0" echo   Finished with %FAILS% problem(s), see the messages above.
echo   Restart Windows so tablet mode and Windows Update take effect.
echo.
choice /c YN /n /m "Restart now? [Y/N] "
if errorlevel 2 goto :done
shutdown /r /t 5 /c "WindowsiOS Tools setup finished"
exit /b 0
:done
echo.
pause
exit /b 0
