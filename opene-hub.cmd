@echo off
setlocal
set "psdir=%SystemRoot%\System32"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "psdir=%SystemRoot%\Sysnative"
"%psdir%\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -ExecutionPolicy RemoteSigned -File "%~dp0scripts\OpenE-Hub.ps1"
if errorlevel 1 pause
