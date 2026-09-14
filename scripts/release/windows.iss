#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "tree"
#endif
#ifndef OutputDir
  #define OutputDir "dist"
#endif
[Setup]
AppId={{0942D704-4C41-49F8-91B0-A77957961666}
AppName=RetroSave Community
AppVersion={#AppVersion}
AppPublisher=RetroSave contributors
AppPublisherURL=https://github.com/pinpin-http/retrosave-community
DefaultDirName={localappdata}\Programs\RetroSave Community
DefaultGroupName=RetroSave Community
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=RetroSave-Community-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#SourceDir}\LICENSE
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\retrosave-desktop.exe
[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\RetroSave Community"; Filename: "{app}\retrosave-desktop.exe"
Name: "{autodesktop}\RetroSave Community"; Filename: "{app}\retrosave-desktop.exe"; Tasks: desktopicon
[Tasks]
Name: "desktopicon"; Description: "Créer un raccourci sur le bureau"; Flags: unchecked
[Run]
Filename: "{app}\retrosave-desktop.exe"; Description: "Ouvrir RetroSave Community"; Flags: nowait postinstall skipifsilent
