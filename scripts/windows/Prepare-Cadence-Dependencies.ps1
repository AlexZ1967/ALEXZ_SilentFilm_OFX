[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$VcpkgRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vcpkg = Join-Path (Resolve-Path $VcpkgRoot).Path 'vcpkg.exe'
if (!(Test-Path $vcpkg)) { throw 'Bootstrap vcpkg first; see docs/WINDOWS_USER_GUIDE_RU.md.' }
& $vcpkg install fftw3:x64-silentfilm "--overlay-triplets=$repo/cmake/triplets"
if ($LASTEXITCODE -ne 0) { throw 'FFTW dependency installation failed.' }
# avdevice нужен для lavfi: интеграционные проверки создают кадры через testsrc2.
# --recurse позволяет добавить feature к уже установленному FFmpeg с пересборкой.
& $vcpkg install 'ffmpeg[core,avcodec,avdevice,avformat,avfilter,swscale,zlib,ffmpeg,ffprobe]:x64-silentfilm-media' 'opencv4[core]:x64-silentfilm-media' 'zstd:x64-silentfilm-media' "--overlay-triplets=$repo/cmake/triplets" --recurse
if ($LASTEXITCODE -ne 0) { throw 'Cadence media dependency installation failed.' }
Write-Host 'Cadence dependencies prepared. No files were installed into Resolve.'
