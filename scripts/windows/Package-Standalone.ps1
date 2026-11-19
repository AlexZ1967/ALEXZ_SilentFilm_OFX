[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageDirectory,
    [string]$MsysRoot = 'C:\msys64',
    [string]$VcRedistPath = '',
    [string]$InnoCompiler = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Build the standalone installer on Windows x64.' }
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$StageDirectory = (Resolve-Path -LiteralPath $StageDirectory).Path
$app = Join-Path $StageDirectory 'SilentFilmCadence'
if (!(Test-Path -LiteralPath (Join-Path $app 'bin/silentfilm_cadence.exe'))) { throw 'Build and pass CTest with -Cadence first.' }
if (!$OutputDirectory) { $OutputDirectory = Split-Path -Parent $StageDirectory }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
if ($OutputDirectory -eq $StageDirectory -or $OutputDirectory.StartsWith($StageDirectory.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The installer output directory must be outside the staged payload.'
}

# Inno Setup нужен только на машине, создающей установщик.
if (!$InnoCompiler) {
    $options = @("$env:ProgramFiles\Inno Setup 7\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
                 "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
    foreach ($option in $options) { if (Test-Path -LiteralPath $option) { $InnoCompiler = $option; break } }
}
if (!$InnoCompiler -or !(Test-Path -LiteralPath $InnoCompiler)) {
    throw 'Install Inno Setup 6.3+ or 7 on this build machine, or specify -InnoCompiler. See docs/WINDOWS_STANDALONE.md.'
}
if (!$VcRedistPath) {
    $vsRoot = if ($env:VSINSTALLDIR) { $env:VSINSTALLDIR } else { "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
    $redists = @(Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC/Redist') -Filter 'vc_redist.x64.exe' -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($redists.Count -gt 0) { $VcRedistPath = $redists[0].FullName }
}
if (!$VcRedistPath -or !(Test-Path -LiteralPath $VcRedistPath)) {
    throw 'Download the Microsoft Visual C++ x64 Redistributable and pass -VcRedistPath. See docs/WINDOWS_STANDALONE.md.'
}
$signature = Get-AuthenticodeSignature -LiteralPath $VcRedistPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
    throw 'The Visual C++ Redistributable must have a valid Microsoft signature.'
}
$sourcePrefix = (Resolve-Path -LiteralPath (Join-Path $MsysRoot 'ucrt64')).Path
if ($app.StartsWith($sourcePrefix.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The stage must be outside MSYS2/ucrt64 to avoid recursively copying the runtime into itself.'
}
foreach ($relative in @('bin/python.exe', 'bin/gdk-pixbuf-query-loaders.exe', 'lib/girepository-1.0/Gtk-4.0.typelib', 'share/glib-2.0/schemas/gschemas.compiled')) {
    if (!(Test-Path -LiteralPath (Join-Path $sourcePrefix $relative))) { throw "Missing MSYS2 runtime component: $relative" }
}
$runtime = Join-Path $app 'runtime'
$incoming = Join-Path $app ('.runtime-' + [guid]::NewGuid().ToString('N'))
$previous = Join-Path $app ('.previous-runtime-' + [guid]::NewGuid().ToString('N'))
$saved = $false
$published = $false
& (Join-Path $sourcePrefix 'bin/python.exe') -I -X utf8 (Join-Path $repo 'tests/windows_runtime_bundle_tests.py')
if ($LASTEXITCODE -ne 0) { throw 'PE dependency selection tests failed.' }
try {
    New-Item -ItemType Directory -Path (Join-Path $incoming 'ucrt64') -Force | Out-Null
    # Отбираем только runtime и замыкание DLL, а не всё установленное UCRT64.
    & (Join-Path $sourcePrefix 'bin/python.exe') -I -X utf8 (Join-Path $PSScriptRoot 'bundle_runtime.py') --prefix $sourcePrefix --destination (Join-Path $incoming 'ucrt64')
    if ($LASTEXITCODE -ne 0) { throw 'Selecting Python/GTK runtime dependencies failed.' }
    $packages = & (Join-Path $MsysRoot 'usr/bin/pacman.exe') -Q
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record the installed MSYS2 package versions.' }
    [IO.File]::WriteAllLines((Join-Path $incoming 'MSYS2-PACKAGES.txt'), [string[]]$packages, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $runtime) { Move-Item -LiteralPath $runtime -Destination $previous; $saved = $true }
    Move-Item -LiteralPath $incoming -Destination $runtime
    $published = $true

    # Обновляем Python/bootstrap и launcher даже при упаковке старого stage.
    Copy-Item -Path (Join-Path $repo 'scripts/cadence_*.py') -Destination (Join-Path $app 'scripts') -Force
    foreach ($file in @('Start-Cadence.ps1', 'Start-Cadence.cmd', 'Install-Cadence-Menu.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $app -Force
    }
    & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $app 'Start-Cadence.ps1') -CheckRuntime
    if ($LASTEXITCODE -ne 0) { throw 'Relocated Python/GTK runtime check failed. The installer was not created.' }
    if ($saved) { Remove-Item -LiteralPath $previous -Recurse -Force; $saved = $false }
} catch {
    if ($published -and (Test-Path -LiteralPath $runtime)) { Remove-Item -LiteralPath $runtime -Recurse -Force }
    if (Test-Path -LiteralPath $previous) {
        Move-Item -LiteralPath $previous -Destination $runtime
    }
    throw
} finally {
    if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
}
foreach ($file in @('Install-Windows.ps1', 'Install-Windows.cmd')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $StageDirectory -Force
}
$prerequisites = Join-Path $StageDirectory 'prerequisites'
New-Item -ItemType Directory -Path $prerequisites -Force | Out-Null
Copy-Item -LiteralPath $VcRedistPath -Destination (Join-Path $prerequisites 'vc_redist.x64.exe') -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/WINDOWS_STANDALONE.md') -Destination $StageDirectory -Force
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
$version = [regex]::Match($project, 'project\([^\r\n]+VERSION\s+([0-9.]+)').Groups[1].Value
if (!$version) { throw 'Cannot read the project version.' }
$variant = Split-Path -Leaf (Split-Path -Parent $StageDirectory)
$setupName = "ALEXZSilentFilm-Setup-$variant"
$cache = Join-Path (Split-Path -Parent $StageDirectory) 'CMakeCache.txt'
$architectures = ''
if (Test-Path -LiteralPath $cache) {
    $match = Select-String -LiteralPath $cache -Pattern '^CMAKE_CUDA_ARCHITECTURES:[^=]+=(.+)$'
    if ($match) { $architectures = $match.Matches[0].Groups[1].Value }
}
if ($architectures) { $setupName += '-sm' + $architectures.Replace(';', '-') }
$metadata = @{ version = $version; variant = $variant; cuda_architectures = $architectures;
    runtime = 'bundled-ucrt64'; created_utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json
[IO.File]::WriteAllText((Join-Path $StageDirectory 'package-info.json'), $metadata, (New-Object Text.UTF8Encoding($false)))
& $InnoCompiler /Qp "/DPayloadRoot=$StageDirectory" "/DOutputRoot=$OutputDirectory" "/DSetupName=$setupName" "/DPackageVersion=$version" (Join-Path $PSScriptRoot 'Standalone.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
Write-Host "Standalone installer: $OutputDirectory\$setupName.exe"
Write-Host 'Test this installer on the target Windows machine without MSYS2. No target-machine test has been run here.'
