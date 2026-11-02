@echo off
setlocal EnableExtensions
title PPT Lab setup
rem ================================================================
rem  PPT Lab installer (double-click this file).
rem  Japanese messages are printed by install.ps1 in this folder.
rem  Usage: install.cmd [-DryRun] [-Yes] [-NonInteractive] [-RamDisk]
rem  Keep all text in this file ASCII: cmd.exe reads .cmd files in the
rem  OEM code page, so non-ASCII text would be garbled. (Also do not add
rem  a UTF-8 BOM: it breaks the first line.)
rem ================================================================
set "HERE=%~dp0"
set "SCRIPT=%HERE%install.ps1"

if not exist "%SCRIPT%" goto :missing_script

where powershell >nul 2>&1
if errorlevel 1 goto :no_powershell

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
set "CODE=%ERRORLEVEL%"

rem 0 = success / 2 = required files missing / 3 = cancelled by user
if "%CODE%"=="0" exit /b 0
if "%CODE%"=="2" exit /b 2
if "%CODE%"=="3" exit /b 3

echo.
echo  [PPT Lab] setup ended with exit code %CODE%.
echo  Log files: %%LOCALAPPDATA%%\PPTLab\logs
echo.
pause
exit /b %CODE%

:missing_script
echo.
echo  [PPT Lab] install.ps1 was not found next to this file.
echo  Extract the whole distribution folder again and run install.cmd.
echo.
pause
exit /b 1

:no_powershell
echo.
echo  [PPT Lab] Windows PowerShell was not found.
echo  This app requires Windows 10 or later.
echo.
pause
exit /b 1
