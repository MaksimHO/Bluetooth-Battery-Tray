#define AppName "Bluetooth Battery Tray"
#define AppVersion "1.0.0"
#define AppPublisher "Bluetooth Battery Tray"
#define AppExeName "BluetoothBatteryTray.exe"

[Setup]
AppId={{8B2E5A3C-6F1D-4C7E-9A3B-2D5F7C1E4A90}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Bluetooth Battery Tray
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=BluetoothBatteryTray-Setup-{#AppVersion}
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName} {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
RestartApplications=no
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoVersion=1.0.0.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
AutoStartTask=Start with Windows
russian.AutoStartTask=Запускать при входе в Windows

[Tasks]
Name: "autostart"; Description: "{cm:AutoStartTask}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\Release\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README-RU.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: autostart

[Registry]
Root: HKCU; Subkey: "Software\BluetoothBatteryTray"; ValueType: none; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\BluetoothBatteryTray"

[Code]
function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  // The tray app has no visible main window, so Windows Restart Manager cannot
  // always close it. Stop it explicitly before the uninstaller removes files.
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM BluetoothBatteryTray.exe', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := True;
end;
