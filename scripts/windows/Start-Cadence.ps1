[CmdletBinding()]
param(
    [string]$AppDirectory = '',
    [string]$MsysRoot = $(if ($env:SILENTFILM_MSYS_ROOT) { $env:SILENTFILM_MSYS_ROOT } else { 'C:\msys64' }),
    [string]$ResolvePython = $(if ($env:SILENTFILM_RESOLVE_PYTHON) { $env:SILENTFILM_RESOLVE_PYTHON } else { "$env:ProgramFiles\Blackmagic Design\DaVinci Resolve\ResolvePython\ResolvePython.exe" }),
    [ValidateSet('auto', 'cpu', 'cuda')][string]$Backend = 'auto'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# При -File Windows PowerShell может не заполнить PSScriptRoot в param defaults.
if ([string]::IsNullOrWhiteSpace($AppDirectory)) { $AppDirectory = $PSScriptRoot }
$AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
$gtkBin = Join-Path $MsysRoot 'ucrt64/bin'
$python = Join-Path $gtkBin 'python.exe'
$helper = Join-Path $AppDirectory 'bin/silentfilm_cadence.exe'
$script = Join-Path $AppDirectory 'scripts/cadence_app.py'
foreach ($required in @($python, $helper, $script, $ResolvePython)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required file not found: $required" }
}
if ($Backend -eq 'auto') {
    $configuration = Get-Content -LiteralPath (Join-Path $AppDirectory 'cadence_runtime.json') -Raw | ConvertFrom-Json
    $Backend = $configuration.backend
}
$output = Join-Path $env:LOCALAPPDATA 'ALEXZSilentFilm/cadence/renders'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$environment = @{
    'SILENTFILM_NATIVE_PATH' = $env:PATH
    'PATH' = "$gtkBin;$env:PATH"
    'PYGI_DLL_PATH' = $gtkBin
    'GI_TYPELIB_PATH' = (Join-Path $MsysRoot 'ucrt64/lib/girepository-1.0')
    'XDG_DATA_DIRS' = (Join-Path $MsysRoot 'ucrt64/share')
    'GSETTINGS_SCHEMA_DIR' = (Join-Path $MsysRoot 'ucrt64/share/glib-2.0/schemas')
    'SILENTFILM_ZSTD_LIBRARY' = (Join-Path $AppDirectory 'bin/zstd.dll')
    'SILENTFILM_FFMPEG' = (Join-Path $AppDirectory 'bin/ffmpeg.exe')
    'PYTHONUTF8' = '1'
    'PYTHONIOENCODING' = 'utf-8'
    'PYTHONHOME' = $null
    'PYTHONPATH' = $null
}
$previous = @{}
try {
    foreach ($name in $environment.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    & $python $script --helper $helper --root $output --backend $Backend --resolve-python $ResolvePython
    if ($LASTEXITCODE -ne 0) { throw "Cadence GUI exited with code $LASTEXITCODE" }
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
