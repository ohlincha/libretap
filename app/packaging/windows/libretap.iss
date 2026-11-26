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
; Ethernet capture needs Wireshark's dumpcap and the Npcap driver. Npcap's licence does not allow bundling it, so this
; downloads Wireshark's official installer, which offers Npcap; the user clicks through it (it asks for administrator
; rights). Shown only when Wireshark is not installed yet.
Name: "wireshark"; Description: "Install Wireshark with Npcap, for Ethernet capture (downloads it from wireshark.org; you click through its installer)"; GroupDescription: "Network capture:"; Check: not WiresharkInstalled

[Files]
Source: "..\..\dist\LibreTap\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\LibreTap"; Filename: "{app}\LibreTap.exe"
Name: "{group}\LibreTap (demo, no hardware)"; Filename: "{app}\LibreTap.exe"; Parameters: "--demo"
Name: "{autodesktop}\LibreTap"; Filename: "{app}\LibreTap.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\LibreTap.exe"; Description: "Start LibreTap"; Flags: nowait postinstall skipifsilent

[Code]
function WiresharkInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf64}\Wireshark\dumpcap.exe')) or
            FileExists(ExpandConstant('{commonpf32}\Wireshark\dumpcap.exe'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('wireshark') then
  begin
    try
      DownloadTemporaryFile('https://www.wireshark.org/download/win64/Wireshark-latest-x64.exe', 'wireshark-setup.exe', '', nil);
      // ShellExec, not Exec: Wireshark's installer asks for administrator rights (UAC)
      if not ShellExec('open', ExpandConstant('{tmp}\wireshark-setup.exe'), '', '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then
        ShellExec('open', 'https://www.wireshark.org/download.html', '', '', SW_SHOW, ewNoWait, ResultCode);
    except
      ShellExec('open', 'https://www.wireshark.org/download.html', '', '', SW_SHOW, ewNoWait, ResultCode);
    end;
  end;
end;
