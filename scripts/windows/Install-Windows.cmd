@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Install-Windows.ps1" %*
set "silentfilm_install_exit=%ERRORLEVEL%"
pause
exit /b %silentfilm_install_exit%
