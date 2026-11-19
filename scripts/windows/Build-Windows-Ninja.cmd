@echo off
setlocal
rem Окружение MSVC меняется только у дочернего CMD и PowerShell сборки.
set "silentfilm_vs_root=%SILENTFILM_VS_ROOT%"
if not defined silentfilm_vs_root set "silentfilm_vs_root=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%silentfilm_vs_root%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo vcvarsall.bat not found. Set SILENTFILM_VS_ROOT to your Visual Studio 2022 installation directory.
    exit /b 1
)
call "%silentfilm_vs_root%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.39
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Build-Windows.ps1" -Generator Ninja %*
exit /b %ERRORLEVEL%
