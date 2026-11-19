; По умолчанию payload находится рядом с Setup.exe; SingleExe встраивает архив.
[Setup]
AppId=ALEXZSilentFilmStandalone
AppName=ALEXZ SilentFilm OFX and Cadence
AppVersion={#PackageVersion}
DefaultDirName={localappdata}\Programs\ALEXZSilentFilm
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputRoot}
OutputBaseFilename={#SetupName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
#ifndef ExternalPayload
Source: "{#PayloadRoot}\ALEXZSilentFilm.ofx.bundle\*"; DestDir: "{tmp}\SilentFilmPayload\ALEXZSilentFilm.ofx.bundle"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\bin\*"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence\bin"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\scripts\cadence_*.py"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence\scripts"; Flags: ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\presets\*.drx"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence\presets"; Flags: ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\runtime\*"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence\runtime"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\*.ps1"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence"; Flags: ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\*.cmd"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence"; Flags: ignoreversion
Source: "{#PayloadRoot}\SilentFilmCadence\cadence_runtime.json"; DestDir: "{tmp}\SilentFilmPayload\SilentFilmCadence"; Flags: ignoreversion
Source: "{#PayloadRoot}\Install-Windows.*"; DestDir: "{tmp}\SilentFilmPayload"; Flags: ignoreversion
Source: "{#PayloadRoot}\package-info.json"; DestDir: "{tmp}\SilentFilmPayload"; Flags: ignoreversion
Source: "{#PayloadRoot}\prerequisites\vc_redist.x64.exe"; DestDir: "{tmp}\SilentFilmPayload\prerequisites"; Flags: ignoreversion
#endif

[Code]
function PayloadDirectory: String;
begin
#ifdef ExternalPayload
  Result := ExpandConstant('{src}\files');
#else
  Result := ExpandConstant('{tmp}\SilentFilmPayload');
#endif
end;

function InitializeSetup: Boolean;
begin
  Result := True;
#ifdef ExternalPayload
  if FileExists(ExpandConstant('{src}\.packaging-in-progress')) then begin
    MsgBox('This installer folder is still being prepared. Finish packaging first.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if not FileExists(PayloadDirectory + '\Install-Windows.ps1') or
     not FileExists(PayloadDirectory + '\SilentFilmCadence\runtime\ucrt64\bin\python.exe') or
     not FileExists(PayloadDirectory + '\ALEXZSilentFilm.ofx.bundle\Contents\Win64\ALEXZSilentFilm.ofx') or
     not FileExists(PayloadDirectory + '\prerequisites\vc_redist.x64.exe') then begin
    MsgBox('The files directory must stay next to Setup.exe. Copy the entire installer folder.', mbError, MB_OK);
    Result := False;
  end;
#endif
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Status: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then begin
    Parameters := '-NoProfile -ExecutionPolicy RemoteSigned -File "' +
      PayloadDirectory + '\Install-Windows.ps1' +
      '" -StageDirectory "' + PayloadDirectory +
      '" -LogFile "' + ExpandConstant('{localappdata}\ALEXZSilentFilm\install.log') + '"';
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
      '', SW_SHOW, ewWaitUntilTerminated, Status) then
      RaiseException('Cannot start the SilentFilm installer.');
    if Status <> 0 then
      RaiseException('SilentFilm installation failed. Close Resolve and Cadence, then retry. Log: ' +
        ExpandConstant('{localappdata}\ALEXZSilentFilm\install.log'));
  end;
end;
