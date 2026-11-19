@echo off
setlocal
set "silentfilm_shell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "silentfilm_shell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%silentfilm_shell%" -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Start-Cadence.ps1" %*
if errorlevel 1 pause
