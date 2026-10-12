#ifndef PayloadDir
  #error PayloadDir must be supplied by BuildInstaller.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be supplied by BuildInstaller.ps1
#endif
#ifndef AppVersion
  #error AppVersion must be supplied by BuildInstaller.ps1
#endif

[Setup]
AppId={{C252A97B-7C30-4572-87D3-3DAC6BAF931A}
AppName=LamaPon
AppVersion={#AppVersion}
AppPublisher=LamaPon contributors
AppPublisherURL=https://github.com/Timiratz/LamaPon-Engine
DefaultDirName={localappdata}\Programs\LamaPon
DefaultGroupName=LamaPon
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
AllowNoIcons=yes
UsePreviousTasks=yes
OutputDir={#OutputDir}
OutputBaseFilename=LamaPon-{#AppVersion}-windows-x64-Setup
SetupIconFile=..\..\assets\icons\LamaPon.ico
UninstallDisplayIcon={app}\LamaPonHub.exe
LicenseFile=License.ja.txt
ShowLanguageDialog=no
LanguageDetectionMethod=none
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[CustomMessages]
japanese.DesktopIcons=デスクトップに起動ショートカットを作成する
japanese.BuildEnvironment=自動ビルド環境セットアップ
japanese.AutoBuild=LamaPon 自動ビルド
japanese.SetupBuildEnvironment=C++自動ビルドに必要なソフトを確認・セットアップする（不足時はダウンロードと管理者権限が必要）
japanese.LaunchHub=LamaPon Hubを起動する
japanese.InstallHelp=インストールと使い方

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcons}"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\LamaPon Hub"; Filename: "{app}\LamaPonHub.exe"; WorkingDir: "{app}"
Name: "{group}\LamaPon Engine"; Filename: "{app}\LamaPonEditor.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:AutoBuild}"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\StartAutoBuild.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\LamaPonEditor.exe"
Name: "{group}\{cm:BuildEnvironment}"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\SetupBuildEnvironment.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\LamaPonHub.exe"
Name: "{group}\{cm:InstallHelp}"; Filename: "{app}\docs\installation.md"
Name: "{autodesktop}\LamaPon Hub"; Filename: "{app}\LamaPonHub.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\LamaPon Engine"; Filename: "{app}\LamaPonEditor.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\{cm:AutoBuild}"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\StartAutoBuild.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\LamaPonEditor.exe"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\SetupBuildEnvironment.ps1"""; Description: "{cm:SetupBuildEnvironment}"; Flags: postinstall skipifsilent
Filename: "{app}\LamaPonHub.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchHub}"; Flags: postinstall nowait skipifsilent
