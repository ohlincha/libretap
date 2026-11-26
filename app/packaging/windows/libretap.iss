; Inno Setup script for LibreTap (CI: iscc /DAppVersion=x.y.z packaging\windows\libretap.iss, from app/)
; Installs the deployed folder dist\LibreTap (program, Qt, ffmpeg.exe) per user by default.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{5E1C7A93-2B64-4C0F-9D8E-3A7B6F1C2D58}
AppName=LibreTap
AppVersion={#AppVersion}
AppPublisher=LibreTap contributors
AppPublisherURL=https://github.com/ohlincha/libretap
DefaultDirName={autopf}\LibreTap
DefaultGroupName=LibreTap
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\..\dist
OutputBaseFilename=LibreTap-{#AppVersion}-Windows-x64-Setup
SetupIconFile=..\..\resources\icon.ico
UninstallDisplayIcon={app}\LibreTap.exe
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\..\dist\LibreTap\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\LibreTap"; Filename: "{app}\LibreTap.exe"
Name: "{group}\LibreTap (demo, no hardware)"; Filename: "{app}\LibreTap.exe"; Parameters: "--demo"
Name: "{autodesktop}\LibreTap"; Filename: "{app}\LibreTap.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\LibreTap.exe"; Description: "Start LibreTap"; Flags: nowait postinstall skipifsilent
