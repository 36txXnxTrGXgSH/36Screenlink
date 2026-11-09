; Installateur de 36Screenlink : un seul fichier, une seule validation administrateur.
#ifndef Version
  #define Version "1.7.0"
#endif
#define Root "..\.."
#define PlatformToolsUrl "https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip"
#define PlatformToolsSha256 "45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99"

[Setup]
AppId={{36C1E2A7-5B7D-4F36-9E36-5C2E36F1A736}
AppName=36Screenlink
AppVersion={#Version}
AppPublisher=36txXnxTrGXgSH
AppPublisherURL=https://github.com/36txXnxTrGXgSH/36Screenlink
DefaultDirName={autopf}\36Screenlink
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
OutputDir={#Root}\dist\release
OutputBaseFilename=36Screenlink-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=36Screenlink
UninstallDisplayIcon={app}\36Screenlink.exe
SetupIconFile=..\agent\res\36Screenlink.ico
LicenseFile={#Root}\LICENSE
CloseApplications=no

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "usb"; Description: "Prise en charge USB : télécharger Android Platform Tools chez Google (facultatif, le Wi-Fi suffit)"; Flags: unchecked
Name: "secure"; Description: "Permettre à la tablette de valider les invites UAC et de déverrouiller ce PC (risque : à réserver à une tablette protégée par un code)"; Flags: unchecked

[Files]
Source: "{#Root}\windows\build\Release\36Screenlink.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\windows\build\Release\36ScreenlinkService.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\windows\driver\build\36ScreenlinkDevice.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\windows\driver\dist\36ScreenlinkDriver.dll"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "{#Root}\windows\driver\dist\36ScreenlinkDriver.inf"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "{#Root}\windows\driver\dist\36ScreenlinkDriver.cat"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "setup.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "remove.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "driver.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\windows\driver\LICENSE-MS-PL.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\36Screenlink"; Filename: "{app}\36Screenlink.exe"; Parameters: "--open"; Comment: "Partager l'écran vers la tablette"

[Run]
Filename: "{app}\36Screenlink.exe"; Parameters: "--open"; Description: "Lancer 36Screenlink"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\remove.ps1"""; Flags: runhidden waituntilterminated; RunOnceId: "Remove36Screenlink"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Messages]
FinishedLabel=36Screenlink est installé et démarre avec Windows.%n%nInstallez 36Screenlink.apk sur la tablette, ouvrez l'application, puis comparez le code affiché sur le PC et sur la tablette.

[Code]
var
  DownloadPage: TDownloadWizardPage;

function PowerShell(): String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Prise en charge USB', 'Téléchargement d''Android Platform Tools chez Google', nil);
end;

// Une mise à jour ne pourrait pas remplacer des fichiers en cours d'utilisation.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop 36Screenlink', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(2500);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im 36Screenlink.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im adb.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('usb') then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#PlatformToolsUrl}', 'platform-tools.zip', '{#PlatformToolsSha256}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        // Sans ces outils, l'application fonctionne en Wi-Fi : l'échec n'arrête pas l'installation.
        Log('Téléchargement impossible : ' + GetExceptionMessage);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Arguments: String;
begin
  if CurStep <> ssPostInstall then Exit;
  WizardForm.StatusLabel.Caption := 'Installation du pilote d''écran et du service...';
  Arguments := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\setup.ps1') + '"';
  if WizardIsTaskSelected('secure') then Arguments := Arguments + ' -SecureScreens';
  if FileExists(ExpandConstant('{tmp}\platform-tools.zip')) then
    Arguments := Arguments + ' -PlatformTools "' + ExpandConstant('{tmp}\platform-tools.zip') + '"';
  if not Exec(PowerShell(), Arguments, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
    MsgBox('L''installation du pilote a échoué. Consultez C:\ProgramData\36Screenlink\logs\setup.log.', mbError, MB_OK);
end;
