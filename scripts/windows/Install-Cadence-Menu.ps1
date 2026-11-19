[CmdletBinding()]
param(
    [string]$AppDirectory = '',
    [string]$MsysRoot = 'C:\msys64',
    [string]$ResolvePython = "$env:ProgramFiles\Blackmagic Design\DaVinci Resolve\ResolvePython\ResolvePython.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Папку определяем после param: это работает и при powershell.exe -File.
if ([string]::IsNullOrWhiteSpace($AppDirectory)) { $AppDirectory = $PSScriptRoot }
$AppDirectory = (Resolve-Path -LiteralPath $AppDirectory).Path
if (!(Test-Path (Join-Path $AppDirectory 'bin/silentfilm_cadence.exe'))) { throw 'Choose a complete staged Cadence app directory.' }
$configuration = @{ app = $AppDirectory; msys = $MsysRoot; resolve_python = $ResolvePython } | ConvertTo-Json -Compress
$encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($configuration))
$body = @"
import base64
import json
import os
from pathlib import Path
import subprocess

configuration = json.loads(base64.b64decode('$encoded').decode('utf-8'))
app = Path(configuration['app'])
launcher = app / 'Start-Cadence.ps1'
if not launcher.is_file():
    raise RuntimeError('Cadence installation moved; reinstall its menu launcher')
logs = Path(os.environ['LOCALAPPDATA']) / 'ALEXZSilentFilm/cadence'
logs.mkdir(parents=True, exist_ok=True)
environment = os.environ.copy()
for name in ('PYTHONHOME', 'PYTHONPATH'):
    environment.pop(name, None)
with (logs / 'app_start.log').open('ab') as log:
    process = subprocess.Popen(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'RemoteSigned',
                                '-File', str(launcher), '-MsysRoot', configuration['msys'],
                                '-ResolvePython', configuration['resolve_python']],
                               stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                               env=environment, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW)
print('[SilentFilm Cadence] GUI PID=' + str(process.pid) + '; log: ' + str(logs / 'app_start.log'))
"@
$directory = Join-Path $env:APPDATA 'Blackmagic Design/DaVinci Resolve/Support/Fusion/Scripts/Utility'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$destination = Join-Path $directory 'SilentFilm Cadence.py'
if (Test-Path $destination) {
    $backups = Join-Path $env:LOCALAPPDATA 'ALEXZSilentFilm/cadence/menu-backups'
    New-Item -ItemType Directory -Path $backups -Force | Out-Null
    Copy-Item -LiteralPath $destination -Destination (Join-Path $backups ([guid]::NewGuid().ToString() + '.py'))
}
[IO.File]::WriteAllText($destination, $body, (New-Object Text.UTF8Encoding($false)))
Write-Host "Menu script: $destination"
Write-Host 'Start/restart Resolve manually, then Workspace > Scripts > Utility > SilentFilm Cadence.'
