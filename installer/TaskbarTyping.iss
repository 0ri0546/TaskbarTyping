; Discord Taskbar Typing - script Inno Setup (6.3 ou plus)
; Se compile avec build-release.ps1, ou a la main :
;   ISCC /DStagingDir=C:\chemin\staging TaskbarTyping.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StagingDir
  #define StagingDir "build\staging"
#endif

#define AppName "Discord Taskbar Typing"
#define AppPublisher "Lucas"
#define OverlayExe "DiscordTaskbarTyping.exe"

[Setup]
AppId={{E2D06FBB-4964-4548-AA8C-C8601489E004}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
; IMPORTANT : ce dossier doit rester identique au chemin par defaut cherche
; par le plugin (native.ts) : %LOCALAPPDATA%\Programs\DiscordTaskbarTyping
DefaultDirName={localappdata}\Programs\DiscordTaskbarTyping
DisableDirPage=yes
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=output
OutputBaseFilename=DiscordTaskbarTyping-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#OverlayExe}
InfoBeforeFile=LISEZ-MOI.txt
CloseApplications=no
RestartApplications=no

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Messages]
french.WelcomeLabel2=Cet assistant va installer [name] sur votre ordinateur.%n%nL'indicateur affiche la photo de profil de la personne qui vous écrit en message privé, au-dessus de la barre des tâches.%n%nDiscord sera fermé pendant l'installation, puis vous pourrez le relancer à la fin. Rien d'autre à configurer.

[InstallDelete]
; Nettoie l'ancienne version avant de copier la nouvelle
Type: filesandordirs; Name: "{app}\vencord\dist"

[Files]
Source: "{#StagingDir}\{#OverlayExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagingDir}\vencord\dist\*"; DestDir: "{app}\vencord\dist"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StagingDir}\vencord\VencordInstallerCli.exe"; DestDir: "{app}\vencord"; Flags: ignoreversion
Source: "{#StagingDir}\LICENSE-Vencord-GPL3.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagingDir}\BUILD-INFO.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "LISEZ-MOI.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "repair.cmd"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Réparer (après une mise à jour de Discord)"; Filename: "{app}\repair.cmd"; WorkingDir: "{app}"
Name: "{group}\Désinstaller {#AppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{localappdata}\Discord\Update.exe"; Parameters: "--processStart Discord.exe"; Description: "Lancer Discord maintenant"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\vencord"

[Code]
var
  DiscordWasClosed: Boolean;

function SetEnvironmentVariable(lpName: String; lpValue: String): Boolean;
  external 'SetEnvironmentVariableW@kernel32.dll stdcall';

function DiscordUpdateExe(): String;
begin
  Result := ExpandConstant('{localappdata}\Discord\Update.exe');
end;

// taskkill renvoie 0 si au moins un processus a ete ferme.
function KillProcess(const ImageName: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\taskkill.exe'),
    '/F /IM ' + ImageName + ' /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
    and (ResultCode = 0);
end;

// Lance le CLI officiel de Vencord en "dev install" :
//  - VENCORD_USER_DATA_DIR : dossier qui contient NOTRE dist\patcher.js
//  - VENCORD_DEV_INSTALL=1 : ne telecharge rien, n'ecrase pas notre dist
// Ne jamais utiliser -repair : il retelecharge le Vencord officiel.
function RunVencordCli(const Args: String): Boolean;
var
  Base: String;
  ResultCode: Integer;
begin
  Base := ExpandConstant('{app}\vencord');
  SetEnvironmentVariable('VENCORD_USER_DATA_DIR', Base);
  SetEnvironmentVariable('VENCORD_DEV_INSTALL', '1');
  Result := Exec(Base + '\VencordInstallerCli.exe', Args, Base,
    SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not FileExists(DiscordUpdateExe()) then
  begin
    MsgBox('Discord n''est pas installé sur ce PC (version classique de discord.com).' + #13#10 +
           'Installe-le, lance-le une fois, puis relance cet installateur.', mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  KillProcess('DiscordTaskbarTyping.exe');
  DiscordWasClosed := KillProcess('Discord.exe');
  Sleep(1500);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  DistDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    DistDir := ExpandConstant('{app}\vencord\dist');

    if not FileExists(DistDir + '\patcher.js') then
    begin
      MsgBox(
        'Installation incomplete.' + #13#10 + #13#10 +
        'Le fichier Vencord suivant est manquant :' + #13#10 +
        DistDir + '\patcher.js' + #13#10 + #13#10 +
        'Discord ne sera pas modifié.',
        mbError,
        MB_OK
      );
      Exit;
    end;

    if not FileExists(DistDir + '\preload.js') then
    begin
      MsgBox(
        'Installation incomplete : preload.js est manquant.' + #13#10 +
        'Discord ne sera pas modifié.',
        mbError,
        MB_OK
      );
      Exit;
    end;

    if not RunVencordCli('-install -branch stable') then
    begin
      MsgBox(
        'L''installation dans Discord a échoué.' + #13#10 + #13#10 +
        'Discord ne sera pas considéré comme installé avec TaskbarTyping.',
        mbError,
        MB_OK
      );
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    KillProcess('DiscordTaskbarTyping.exe');
    DiscordWasClosed := KillProcess('Discord.exe');
    Sleep(1500);
    if not RunVencordCli('-uninstall -branch stable') then
      MsgBox('Le patch de Discord n''a pas pu être retiré automatiquement.' + #13#10 +
             'Tu peux réinstaller Discord depuis discord.com pour repartir de zéro.',
             mbInformation, MB_OK);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    if DiscordWasClosed and (not UninstallSilent) and FileExists(DiscordUpdateExe()) then
      Exec(DiscordUpdateExe(), '--processStart Discord.exe', '', SW_SHOW, ewNoWait, ResultCode);
  end;
end;
