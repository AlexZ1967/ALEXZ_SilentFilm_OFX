[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OfxIncludeDir,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot,
    [switch]$Cuda,
    [switch]$Cadence,
    [ValidateSet('VisualStudio', 'Ninja')][string]$Generator = 'VisualStudio',
    [string]$MsysRoot = 'C:\msys64',
    [string]$CudaRoot = $env:CUDA_PATH,
    [ValidatePattern('^[0-9]+(;[0-9]+)*$')][string]$CudaArchitectures = '86',
    [ValidateRange(1, 64)][int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows x64.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$OfxIncludeDir = (Resolve-Path $OfxIncludeDir).Path
$VcpkgRoot = (Resolve-Path $VcpkgRoot).Path
if (!(Test-Path (Join-Path $OfxIncludeDir 'ofxImageEffect.h'))) { throw 'OpenFX headers not found.' }
if (!(Test-Path (Join-Path $VcpkgRoot 'installed/x64-silentfilm/include/fftw3.h'))) {
    throw 'Install fftw3:x64-silentfilm first; see docs/WINDOWS_SETUP.md.'
}
$variant = if ($Cuda) { 'cuda' } else { 'cpu' }
if ($Cadence) { $variant = "cadence-$variant" }
if ($Generator -eq 'Ninja') { $variant = "$variant-ninja" }
$build = Join-Path $repo "build-windows-$variant"
$stage = Join-Path $build 'stage'
$toolset = 'v143,version=14.39'
if ($Cuda) {
    if (!$CudaRoot) { throw 'Set -CudaRoot to the CUDA Toolkit installation directory.' }
    $CudaRoot = (Resolve-Path $CudaRoot).Path
    if (!(Test-Path (Join-Path $CudaRoot 'bin/nvcc.exe'))) { throw 'nvcc.exe not found.' }
    $toolset += ",cuda=$CudaRoot"
}
$configure = @('-S', $repo, '-B', $build, '-DSILENTFILM_BUILD_CADENCE=OFF', '-DBUILD_TESTING=ON',
    "-DOFX_INCLUDE_DIR=$OfxIncludeDir", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    '-DVCPKG_TARGET_TRIPLET=x64-silentfilm', "-DVCPKG_OVERLAY_TRIPLETS=$repo/cmake/triplets",
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL')
if ($Generator -eq 'VisualStudio') {
    $configure += @('-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', $toolset)
} else {
    # vcvarsall вызывается в CMD wrapper: меняется только окружение дочернего процесса.
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64' -or
        $env:VCToolsVersion -notlike '14.39.*' -or !$env:VCToolsInstallDir -or !$env:VSINSTALLDIR) {
        throw 'Ninja requires the MSVC 14.39 x64 environment. Use scripts/windows/Build-Windows-Ninja.cmd from a normal PowerShell window.'
    }
    $compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    if (!(Test-Path $compiler)) { throw "MSVC x64 compiler not found: $compiler" }
    $activeCompiler = (Get-Command cl.exe -ErrorAction Stop).Source
    if ([IO.Path]::GetFullPath($activeCompiler) -ine [IO.Path]::GetFullPath($compiler)) {
        throw 'The first cl.exe in PATH does not match MSVC 14.39 x64. Open a normal PowerShell and use Build-Windows-Ninja.cmd.'
    }
    $ninja = Join-Path $env:VSINSTALLDIR 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    if (!(Test-Path $ninja)) {
        $ninjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
        if (!$ninjaCommand) { throw 'ninja.exe not found. Install Ninja or the Visual Studio C++ CMake tools.' }
        $ninja = $ninjaCommand.Source
    }
    Write-Host "Ninja compiler: $compiler"
    $configure += @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$compiler", "-DCMAKE_MAKE_PROGRAM=$ninja")
    if ($Cuda) {
        $configure += @("-DCMAKE_CUDA_COMPILER=$CudaRoot/bin/nvcc.exe", "-DCMAKE_CUDA_HOST_COMPILER=$compiler")
    }
}
if ($Cuda) {
    $configure += @('-DSILENTFILM_ENABLE_CUDA=ON', "-DCUDAToolkit_ROOT=$CudaRoot",
        "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures")
} else { $configure += '-DSILENTFILM_ENABLE_CUDA=OFF' }
if ($Cadence) {
    $python = Join-Path $MsysRoot 'ucrt64/bin/python.exe'
    if (!(Test-Path $python)) { throw 'Install MSYS2 UCRT64 Python/GTK first; see docs/WINDOWS_CADENCE_APP.md.' }
    $configure += @('-DSILENTFILM_BUILD_CADENCE=ON', "-DSILENTFILM_WINDOWS_MEDIA_ROOT=$VcpkgRoot/installed/x64-silentfilm-media",
        "-DSILENTFILM_TEST_PYTHON=$python")
}
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $build --config Release --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Build failed; tests and packaging were not started.' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed; packaging was not started.' }
& cmake --install $build --config Release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'Staging failed.' }
$artifact = Join-Path $build "ALEXZSilentFilm-windows-x64-$variant.zip"
Compress-Archive -Path (Join-Path $stage 'ALEXZSilentFilm.ofx.bundle') -DestinationPath $artifact -Force
if ($Cadence) {
    $cadenceArtifact = Join-Path $build "SilentFilmCadence-windows-x64-$variant.zip"
    Compress-Archive -Path (Join-Path $stage 'SilentFilmCadence') -DestinationPath $cadenceArtifact -Force
    Write-Host "Cadence app: $stage/SilentFilmCadence/Start-Cadence.cmd"
    Write-Host "Cadence archive: $cadenceArtifact"
}
Write-Host "Bundle: $stage/ALEXZSilentFilm.ofx.bundle"
Write-Host "Archive: $artifact"
Write-Host 'No files were installed into Resolve. Check CTest output for skipped CUDA tests.'
