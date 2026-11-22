@echo off
setlocal
set "psdir=%SystemRoot%\System32"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "psdir=%SystemRoot%\Sysnative"
if not exist "%~dp0build\settings-x64\OpenEGoHubSettings.exe" (
    "%psdir%\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0scripts\build-settings-x64.ps1"
    if errorlevel 1 (
        pause
        exit /b 1
    )
)
start "" "%~dp0build\settings-x64\OpenEGoHubSettings.exe"
