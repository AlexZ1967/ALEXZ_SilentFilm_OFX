@echo off
setlocal
set "silentfilm_shell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "silentfilm_shell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%silentfilm_shell%" -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Install-Windows.ps1" %*
set "silentfilm_install_exit=%ERRORLEVEL%"
pause
exit /b %silentfilm_install_exit%
