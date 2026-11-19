[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Bundle)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows.' }
if (Get-Process -Name Resolve -ErrorAction SilentlyContinue) { throw 'Close DaVinci Resolve before installing.' }
$Bundle = (Resolve-Path $Bundle).Path
if ((Split-Path $Bundle -Leaf) -ne 'ALEXZSilentFilm.ofx.bundle' -or
    !(Test-Path (Join-Path $Bundle 'Contents/Win64/ALEXZSilentFilm.ofx'))) { throw 'Invalid Windows OFX bundle.' }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open PowerShell as Administrator for this installation step.'
}
$commonFiles64 = if ($env:CommonProgramW6432) { $env:CommonProgramW6432 }
    elseif ($env:ProgramW6432) { Join-Path $env:ProgramW6432 'Common Files' } else { $env:CommonProgramFiles }
$plugins = Join-Path $commonFiles64 'OFX/Plugins'
$destination = Join-Path $plugins 'ALEXZSilentFilm.ofx.bundle'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
if (Test-Path $destination) {
    # Старый bundle сохраняется вне OFX/Plugins, чтобы Resolve не сканировал копию.
    $backupRoot = Join-Path $env:LOCALAPPDATA 'ALEXZSilentFilm/ofx-backups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backup = Join-Path $backupRoot ([guid]::NewGuid().ToString() + '.ofx.bundle')
    Move-Item -LiteralPath $destination -Destination $backup
    Write-Host "Previous bundle: $backup"
}
try { Copy-Item -LiteralPath $Bundle -Destination $destination -Recurse }
catch {
    if (Test-Path $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    if (Get-Variable backup -ErrorAction SilentlyContinue) { Move-Item -LiteralPath $backup -Destination $destination }
    throw
}
Write-Host "Installed: $destination"
Write-Host 'Start Resolve manually and follow docs/WINDOWS_SETUP.md.'
