; Inno Setup template for the signed Windows Libera Link package.
; GitHub Actions supplies paths and version values with /D defines.

#ifndef MyAppVersion
  #error "MyAppVersion define is required"
#endif
#ifndef MyAppBuildNumber
  #error "MyAppBuildNumber define is required"
#endif
#ifndef MyAppSourceDir
  #error "MyAppSourceDir define is required"
#endif
#ifndef MyVcRedistPath
  #error "MyVcRedistPath define is required"
#endif
#ifndef MyOutputDir
  #error "MyOutputDir define is required"
#endif
#ifndef MySetupIconFile
  #error "MySetupIconFile define is required"
#endif

#define MyAppName "Libera Link"
#define MyAppPublisher "Seb.ly ltd"
#define MyAppURL "https://github.com/sebleedelisle/libera-link"
#define MyAppExeName "Libera Link.exe"
#define MyCliExeName "libera-link-cli.exe"
#define MyInstallerAppId "{{1F7AD3E7-D94A-42DE-92A4-C681EA8CA45C}"

[Setup]
AppId={#MyInstallerAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion} (build {#MyAppBuildNumber})
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#MyOutputDir}
OutputBaseFilename=libera-link-windows-setup
SetupIconFile={#MySetupIconFile}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyAppSourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppSourceDir}\{#MyCliExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppSourceDir}\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyVcRedistPath}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Check: NeedsVCAndAdminInstallMode
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: shellexec waituntilterminated; Verb: "runas"; Check: NeedsVCAndNonAdminInstallMode
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsVC: Boolean;
var
  Installed: Cardinal;
begin
  Result := not RegQueryDWordValue(
    HKLM,
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'Installed',
    Installed
  ) or (Installed <> 1);
end;

function NeedsVCAndAdminInstallMode: Boolean;
begin
  Result := NeedsVC and IsAdminInstallMode;
end;

function NeedsVCAndNonAdminInstallMode: Boolean;
begin
  Result := NeedsVC and not IsAdminInstallMode;
end;
