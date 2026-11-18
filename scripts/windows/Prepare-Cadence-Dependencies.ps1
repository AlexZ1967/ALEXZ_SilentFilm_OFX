[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$VcpkgRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vcpkg = Join-Path (Resolve-Path $VcpkgRoot).Path 'vcpkg.exe'
if (!(Test-Path $vcpkg)) { throw 'Bootstrap vcpkg first; see docs/WINDOWS_USER_GUIDE_RU.md.' }
& $vcpkg install fftw3:x64-silentfilm "--overlay-triplets=$repo/cmake/triplets"
if ($LASTEXITCODE -ne 0) { throw 'FFTW dependency installation failed.' }
& $vcpkg install 'ffmpeg[core,avcodec,avformat,avfilter,swscale,zlib,ffmpeg,ffprobe]:x64-silentfilm-media' 'opencv4[core]:x64-silentfilm-media' 'zstd:x64-silentfilm-media' "--overlay-triplets=$repo/cmake/triplets"
if ($LASTEXITCODE -ne 0) { throw 'Cadence media dependency installation failed.' }
Write-Host 'Cadence dependencies prepared. No files were installed into Resolve.'
