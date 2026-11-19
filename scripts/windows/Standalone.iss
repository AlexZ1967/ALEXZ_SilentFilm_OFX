; Оболочка одного EXE; копирование выполняет общий установщик с резервными копиями.
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
Source: "{#PayloadRoot}\*"; DestDir: "{tmp}\SilentFilmPayload"; Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  Status: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then begin
    Parameters := '-NoProfile -ExecutionPolicy RemoteSigned -File "' +
      ExpandConstant('{tmp}\SilentFilmPayload\Install-Windows.ps1') +
      '" -StageDirectory "' + ExpandConstant('{tmp}\SilentFilmPayload') +
      '" -LogFile "' + ExpandConstant('{localappdata}\ALEXZSilentFilm\install.log') + '"';
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
      '', SW_SHOW, ewWaitUntilTerminated, Status) then
      RaiseException('Cannot start the SilentFilm installer.');
    if Status <> 0 then
      RaiseException('SilentFilm installation failed. Close Resolve and Cadence, then retry. Log: ' +
        ExpandConstant('{localappdata}\ALEXZSilentFilm\install.log'));
  end;
end;
