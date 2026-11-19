[CmdletBinding()]
param([switch]$Child, [string]$RepoRoot = '', [string]$WorkRoot = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$RepoRoot) { $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path }
if (!$Child) {
    $WorkRoot = Join-Path ([IO.Path]::GetTempPath()) ('silentfilm-paths-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $WorkRoot | Out-Null
    try {
        $shell32 = Join-Path $env:SystemRoot 'SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
        & $shell32 -NoProfile -ExecutionPolicy RemoteSigned -File $PSCommandPath -Child -RepoRoot $RepoRoot -WorkRoot $WorkRoot
        if ($LASTEXITCODE -ne 0) { throw 'The 32-bit launcher path regression failed.' }
    } finally { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }
    exit 0
}
if ([Environment]::Is64BitProcess) { throw 'This regression must run in a real 32-bit process.' }

# Только дочерний тестовый процесс получает поддельные папки вместо системных.
$env:ProgramFiles = Join-Path $WorkRoot 'Program Files (x86)'
$env:ProgramW6432 = Join-Path $WorkRoot 'Program Files'
$env:APPDATA = Join-Path $WorkRoot 'Roaming'
$env:LOCALAPPDATA = Join-Path $WorkRoot 'Local'
$env:SILENTFILM_RESOLVE_PYTHON = $null
$app = Join-Path $WorkRoot 'Cadence app'
$nativePython = Join-Path $env:ProgramW6432 'Blackmagic Design/DaVinci Resolve/ResolvePython/ResolvePython.exe'
$helper = Join-Path $app 'bin/silentfilm_cadence.exe'
foreach ($file in @($nativePython, $helper)) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $file) -Force | Out-Null
    [IO.File]::WriteAllBytes($file, [byte[]]@())
}
$menu = Join-Path $RepoRoot 'scripts/windows/Install-Cadence-Menu.ps1'
$generated = Join-Path $env:APPDATA 'Blackmagic Design/DaVinci Resolve/Support/Fusion/Scripts/Utility/SilentFilm Cadence.py'

function Assert-MenuPython {
    param([string]$Expected)
    $body = [IO.File]::ReadAllText($generated)
    $encoded = [regex]::Match($body, "b64decode\('([^']+)'\)").Groups[1].Value
    if (!$encoded) { throw 'Generated menu configuration was not found.' }
    $configuration = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded)) | ConvertFrom-Json
    if ($configuration.resolve_python -ne $Expected) { throw "Wrong Resolve Python: $($configuration.resolve_python); expected $Expected" }
    if (!$body.Contains('Sysnative/WindowsPowerShell/v1.0/powershell.exe')) { throw 'Generated menu does not select native PowerShell.' }
}

& $menu -AppDirectory $app *> $null
Assert-MenuPython $nativePython
$custom = Join-Path $WorkRoot 'Custom ResolvePython.exe'
[IO.File]::WriteAllBytes($custom, [byte[]]@())
$env:SILENTFILM_RESOLVE_PYTHON = $custom
& $menu -AppDirectory $app *> $null
Assert-MenuPython $custom
& $menu -AppDirectory $app -ResolvePython $nativePython *> $null
Assert-MenuPython $nativePython
<# Проверка реального перехода x86 -> x64 без UAC и без файлов установки. #>
$shell32 = Join-Path $env:SystemRoot 'SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
$diagnostic = & $shell32 -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $RepoRoot 'scripts/windows/Install-Windows.ps1') -CheckLaunch
if ($LASTEXITCODE -ne 0) { throw 'Installer native host preflight failed.' }
$hostInfo = $diagnostic | ConvertFrom-Json
$expectedHost = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
if (!$hostInfo.is_64_bit -or $hostInfo.native_powershell -ne $expectedHost) {
    throw "Installer passed an unsuitable host to UAC: $diagnostic"
}
Write-Host 'Windows launcher paths OK: real 32-bit process, native Program Files, environment and explicit overrides.'
