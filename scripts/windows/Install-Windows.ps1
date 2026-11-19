[CmdletBinding()]
param(
    [string]$StageDirectory = '',
    [string]$MsysRoot = 'C:\msys64',
    [switch]$OfxOnly,
    [string]$LogFile = '',
    [switch]$CheckLaunch
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Sysnative разрешается только внутри WOW64. Сначала без UAC переходим в x64,
# затем для ShellExecute/RunAs используем настоящий System32, доступный брокеру.
if ([Environment]::Is64BitOperatingSystem -and ![Environment]::Is64BitProcess) {
    $nativeShell = Join-Path $env:SystemRoot 'Sysnative/WindowsPowerShell/v1.0/powershell.exe'
    if (!(Test-Path -LiteralPath $nativeShell -PathType Leaf)) { throw "Native PowerShell not found: $nativeShell" }
    $nativeArguments = @('-NoProfile', '-ExecutionPolicy', 'RemoteSigned', '-File', $PSCommandPath)
    if ($StageDirectory) { $nativeArguments += @('-StageDirectory', $StageDirectory) }
    if ($MsysRoot) { $nativeArguments += @('-MsysRoot', $MsysRoot) }
    if ($LogFile) { $nativeArguments += @('-LogFile', $LogFile) }
    if ($OfxOnly) { $nativeArguments += '-OfxOnly' }
    if ($CheckLaunch) { $nativeArguments += '-CheckLaunch' }
    if (!$CheckLaunch) { Write-Host 'Restarting installer in native 64-bit PowerShell (before UAC).' }
    & $nativeShell @nativeArguments
    exit $LASTEXITCODE
}
if (![Environment]::Is64BitProcess) { throw 'SilentFilm requires 64-bit Windows.' }
$commonFiles64 = if ($env:CommonProgramW6432) { $env:CommonProgramW6432 }
    elseif ($env:ProgramW6432) { Join-Path $env:ProgramW6432 'Common Files' } else { $env:CommonProgramFiles }
$powerShell64 = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
if (!(Test-Path -LiteralPath $powerShell64 -PathType Leaf)) { throw "PowerShell host not found: $powerShell64" }
if ($CheckLaunch) {
    [ordered]@{ is_64_bit = [Environment]::Is64BitProcess; native_powershell = $powerShell64;
        common_files = $commonFiles64; script = $PSCommandPath } | ConvertTo-Json -Compress
    exit 0
}
$transcript = $false
if ($LogFile) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $LogFile) -Force | Out-Null
    Start-Transcript -Path $LogFile -Append | Out-Null
    $transcript = $true
}

function Install-Prerequisites {
    param([string]$Stage)
    $redist = Join-Path $Stage 'prerequisites/vc_redist.x64.exe'
    if (Test-Path -LiteralPath $redist) {
        $signature = Get-AuthenticodeSignature -LiteralPath $redist
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
            throw 'Invalid Microsoft signature on the bundled Visual C++ Redistributable.'
        }
        Write-Host "Visual C++ runtime: $redist"
        $process = Start-Process -FilePath $redist -WorkingDirectory $Stage -ArgumentList '/install', '/quiet', '/norestart' -Wait -PassThru
        if ($process.ExitCode -notin @(0, 1638, 3010)) { throw "Visual C++ runtime installation failed: $($process.ExitCode)" }
        if ($process.ExitCode -eq 3010) { Write-Host 'Visual C++ runtime installed; Windows requests a reboot.' }
    }
}

function Assert-ResolveClosed {
    if (Get-Process -Name Resolve -ErrorAction SilentlyContinue) {
        throw 'Close DaVinci Resolve before installing. The installer will not stop it automatically.'
    }
}

function Install-OfxBundle {
    param([string]$Source)
    Assert-ResolveClosed
    $plugins = Join-Path $commonFiles64 'OFX/Plugins'
    $destination = Join-Path $plugins 'ALEXZSilentFilm.ofx.bundle'
    $identifier = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
    $incoming = Join-Path $plugins ('.silentfilm-install-' + $identifier)
    $backup = Join-Path $env:ProgramData ('ALEXZSilentFilm/backups/OFX/' + $identifier + '/ALEXZSilentFilm.ofx.bundle')
    $saved = $false
    New-Item -ItemType Directory -Path $plugins -Force | Out-Null
    try {
        Copy-Item -LiteralPath $Source -Destination $incoming -Recurse
        Assert-ResolveClosed
        if (Test-Path -LiteralPath $destination) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
            Move-Item -LiteralPath $destination -Destination $backup
            $saved = $true
        }
        try { Move-Item -LiteralPath $incoming -Destination $destination }
        catch {
            if ($saved) { Move-Item -LiteralPath $backup -Destination $destination }
            throw
        }
        Write-Host "OFX installed: $destination"
        if ($saved) { Write-Host "Previous OFX: $backup" }
    } finally {
        if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($StageDirectory)) {
        if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'ALEXZSilentFilm.ofx.bundle')) {
            $StageDirectory = $PSScriptRoot
        } else {
            $repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
            $candidates = @(Get-ChildItem -LiteralPath $repo -Directory -Filter 'build-windows-*' |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'stage/ALEXZSilentFilm.ofx.bundle/Contents/Win64/ALEXZSilentFilm.ofx') } |
                Sort-Object LastWriteTime -Descending)
            if ($candidates.Count -eq 0) { throw 'No staged Windows build found. Build and pass CTest first, or specify -StageDirectory.' }
            for ($index = 0; $index -lt $candidates.Count; ++$index) {
                Write-Host ('{0}: {1}' -f ($index + 1), $candidates[$index].Name)
            }
            $selection = Read-Host 'Choose the build number to install'
            $number = 0
            if (![int]::TryParse($selection, [ref]$number) -or $number -lt 1 -or $number -gt $candidates.Count) {
                throw 'Invalid build number. Nothing was installed.'
            }
            $StageDirectory = Join-Path $candidates[$number - 1].FullName 'stage'
        }
    }
    $StageDirectory = (Resolve-Path -LiteralPath $StageDirectory).Path
    Write-Host "Installer host: $powerShell64; native64=$([Environment]::Is64BitProcess); payload: $StageDirectory"
    $sourceBundle = Join-Path $StageDirectory 'ALEXZSilentFilm.ofx.bundle'
    if (!(Test-Path -LiteralPath (Join-Path $sourceBundle 'Contents/Win64/ALEXZSilentFilm.ofx'))) {
        throw 'The stage directory does not contain the complete OFX bundle.'
    }
    Assert-ResolveClosed
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $administrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($OfxOnly) {
        if (!$administrator) { throw 'The OFX copy requires administrator rights.' }
        Install-Prerequisites $StageDirectory
        Install-OfxBundle $sourceBundle
        exit 0
    }

    # Пользовательскую часть устанавливает исходный процесс, а не UAC-пользователь.
    $sourceApp = Join-Path $StageDirectory 'SilentFilmCadence'
    $hasApp = Test-Path -LiteralPath $sourceApp
    if ($hasApp) {
        foreach ($relative in @('bin/silentfilm_cadence.exe', 'Start-Cadence.cmd', 'Start-Cadence.ps1',
                                'Install-Cadence-Menu.ps1', 'cadence_runtime.json', 'scripts/cadence_app.py')) {
            if (!(Test-Path -LiteralPath (Join-Path $sourceApp $relative))) { throw "Incomplete Cadence app: $relative" }
        }
        if (Get-Process -Name silentfilm_cadence -ErrorAction SilentlyContinue) {
            throw 'Close Cadence and wait for active renders to finish before installing.'
        }
        $guiProcesses = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
            Where-Object { $_.CommandLine -match 'cadence_app\.py' })
        if ($guiProcesses.Count -gt 0) { throw 'Close the Cadence GUI before installing.' }
        $configuration = Get-Content -LiteralPath (Join-Path $sourceApp 'cadence_runtime.json') -Raw | ConvertFrom-Json
        if ($configuration.backend -notin @('cpu', 'cuda')) { throw 'Invalid packaged Cadence backend.' }
        Write-Host "Cadence default backend: $($configuration.backend)"
    }

    if ($administrator) { Install-Prerequisites $StageDirectory; Install-OfxBundle $sourceBundle }
    else {
        # Повышается только копирование OFX; аргументы путей заключены в кавычки.
        $elevatedLog = Join-Path $env:ProgramData ('ALEXZSilentFilm/install-logs/ofx-' + [guid]::NewGuid().ToString('N') + '.log')
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'RemoteSigned', '-File',
            ('"' + $PSCommandPath + '"'), '-StageDirectory', ('"' + $StageDirectory.TrimEnd('\') + '"'), '-OfxOnly',
            '-LogFile', ('"' + $elevatedLog + '"'))
        Write-Host "UAC host: $powerShell64; working directory: $StageDirectory; script: $PSCommandPath; log: $elevatedLog"
        try {
            $process = Start-Process -FilePath $powerShell64 -WorkingDirectory $StageDirectory `
                -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        } catch {
            throw "UAC start failed: host='$powerShell64'; directory='$StageDirectory'; script='$PSCommandPath'. $($_.Exception.Message)"
        }
        if ($process.ExitCode -ne 0) {
            if (Test-Path -LiteralPath $elevatedLog) { Get-Content -LiteralPath $elevatedLog -Tail 40 -ErrorAction Continue | ForEach-Object { Write-Host $_ } }
            throw "OFX installation failed (exit $($process.ExitCode)). Log: $elevatedLog. Cadence was not changed."
        }
        Write-Host ('OFX installed: ' + (Join-Path $commonFiles64 'OFX/Plugins/ALEXZSilentFilm.ofx.bundle'))
    }

    if ($hasApp) {
        Assert-ResolveClosed
        $root = Join-Path $env:LOCALAPPDATA 'Programs/ALEXZSilentFilm'
        $destination = Join-Path $root 'SilentFilmCadence'
        $identifier = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
        $incoming = Join-Path $root ('.cadence-install-' + $identifier)
        $backup = Join-Path $root ('backups/' + $identifier + '/SilentFilmCadence')
        $saved = $false
        $menuPath = Join-Path $env:APPDATA 'Blackmagic Design/DaVinci Resolve/Support/Fusion/Scripts/Utility/SilentFilm Cadence.py'
        $menuExisted = Test-Path -LiteralPath $menuPath
        $previousMenu = $null
        if ($menuExisted) { $previousMenu = [IO.File]::ReadAllBytes($menuPath) }
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        try {
            # Не переносим осиротевшие .runtime-* и backup-каталоги из stage.
            New-Item -ItemType Directory -Path $incoming | Out-Null
            foreach ($directory in @('bin', 'scripts', 'presets', 'runtime')) {
                $sourceDirectory = Join-Path $sourceApp $directory
                if (Test-Path -LiteralPath $sourceDirectory) {
                    Copy-Item -LiteralPath $sourceDirectory -Destination (Join-Path $incoming $directory) -Recurse
                }
            }
            foreach ($file in @('Start-Cadence.ps1', 'Start-Cadence.cmd', 'Install-Cadence-Menu.ps1', 'cadence_runtime.json', 'WINDOWS_CADENCE_APP.md')) {
                $sourceFile = Join-Path $sourceApp $file
                if (Test-Path -LiteralPath $sourceFile) { Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $incoming $file) }
            }
            if (Test-Path -LiteralPath (Join-Path $incoming 'runtime/ucrt64/bin/python.exe')) {
                & $powerShell64 -NoProfile -ExecutionPolicy RemoteSigned -File (Join-Path $incoming 'Start-Cadence.ps1') -CheckRuntime
                if ($LASTEXITCODE -ne 0) { throw 'The bundled GTK/Python runtime failed its target-machine check. Previous Cadence was kept.' }
            }
            if (Test-Path -LiteralPath $destination) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
                Move-Item -LiteralPath $destination -Destination $backup
                $saved = $true
            }
            try {
                Move-Item -LiteralPath $incoming -Destination $destination
                & (Join-Path $destination 'Install-Cadence-Menu.ps1') -AppDirectory $destination -MsysRoot $MsysRoot
            } catch {
                if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
                if ($saved) { Move-Item -LiteralPath $backup -Destination $destination }
                if ($menuExisted) { [IO.File]::WriteAllBytes($menuPath, [byte[]]$previousMenu) }
                elseif (Test-Path -LiteralPath $menuPath) { Remove-Item -LiteralPath $menuPath -Force }
                throw
            }
            Write-Host "Cadence installed: $destination"
            if ($saved) { Write-Host "Previous Cadence: $backup" }
        } finally {
            if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
        }
    }
    Write-Host 'Installation complete. Start Resolve manually.'
    if ($hasApp) { Write-Host 'Workspace > Scripts > Utility > SilentFilm Cadence' }
} catch {
    Write-Error -ErrorAction Continue $_
    exit 1
} finally {
    if ($transcript) { Stop-Transcript | Out-Null }
}
