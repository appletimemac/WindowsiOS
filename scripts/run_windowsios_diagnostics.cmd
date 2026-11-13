@echo off
rem Runs the WindowsiOS CPU diagnostics (winios_diag.ps1, next to this file on the WINIOSTOOLS disk) and leaves the
rem report in C:\ProgramData\WindowsiOS and in the host's share folder. Read-only: it changes no setting.
rem Copied to the disk as "Run WindowsiOS Diagnostics.cmd" by scripts/build_tools_disk.sh (CRLF enforced there).
rem Optional argument "quick": a 10 second sample and no powercfg /energy trace.
setlocal EnableExtensions
title Run WindowsiOS Diagnostics

set "SRC=%~dp0"
set "QUICK=0"
set "ELEV=0"
for %%A in (%*) do call :arg "%%~A"
set "PSARGS="
if "%QUICK%"=="1" set "PSARGS=-Quick"

fsutil dirty query "%SystemDrive%" >nul 2>&1
if not errorlevel 1 goto :run
if "%ELEV%"=="1" goto :noadmin
set "ELEVARGS=elevated"
if "%QUICK%"=="1" set "ELEVARGS=elevated quick"
echo Asking for administrator rights (the process-start trace and the timer report need them)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%ELEVARGS%' -Verb RunAs" >nul 2>&1
if errorlevel 1 goto :noadmin
exit /b 0

:arg
if /i "%~1"=="quick" set "QUICK=1"
if /i "%~1"=="elevated" set "ELEV=1"
exit /b

:noadmin
echo.
echo Administrator rights were not granted. Running without them: the process-start trace and the
echo timer report are skipped, everything else still runs.

:run
echo.
echo ================================================
echo  WindowsiOS Diagnostics
echo ================================================
echo.
if exist "%SRC%winios_diag.ps1" goto :have
echo winios_diag.ps1 is missing next to this file.
goto :done
:have
if "%QUICK%"=="1" echo Quick run: about 1 minute.
if not "%QUICK%"=="1" echo This takes a few minutes. Leave Windows alone until it says it is done: touching it changes what it measures.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%winios_diag.ps1" %PSARGS%

:done
echo.
pause
exit /b 0
