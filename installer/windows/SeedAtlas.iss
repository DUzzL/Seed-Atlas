#ifndef MyAppVersion
  #define MyAppVersion "4.2.dev0"
#endif

#define MyAppName "Seed Atlas"
#define MyAppExeName "seed-atlas.exe"
#define MyPortableDir "..\..\dist\Seed-Atlas-" + MyAppVersion + "-Windows-x64-Portable"
#define MySourceArchive "..\..\dist\Seed-Atlas-" + MyAppVersion + "-Source.zip"

[Setup]
AppId={{9EA88549-5B8A-493B-AD00-AA2FB6BE7D02}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Seed Atlas Project
DefaultDirName={localappdata}\Programs\Seed Atlas
DefaultGroupName=Seed Atlas
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\..\dist
OutputBaseFilename=Seed-Atlas-{#MyAppVersion}-Windows-x64-Setup
SetupIconFile=..\..\rc\app_icon.ico
LicenseFile=..\..\LICENSE
InfoBeforeFile=..\..\LEGAL_NOTICE.md
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
CloseApplicationsFilter=seed-atlas.exe
RestartApplications=no
AppMutex=SeedAtlas_9EA88549_5B8A_493B_AD00_AA2FB6BE7D02
VersionInfoVersion=4.2.0.0
VersionInfoCompany=Seed Atlas Project
VersionInfoDescription=Seed Atlas Windows Installer
VersionInfoProductName=Seed Atlas
VersionInfoProductVersion=4.2.0.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#MyPortableDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceArchive}"; DestDir: "{app}\Source"; Flags: ignoreversion

[Icons]
Name: "{group}\Seed Atlas"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\Seed Atlas"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,Seed Atlas}"; Flags: nowait postinstall skipifsilent
