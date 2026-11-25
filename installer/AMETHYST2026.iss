; AMETHYST 2026 - Windows installer (Inno Setup 6)
; Built by tools\make-release.ps1, which passes /DAppVersion=x.y.z
;
; - Installs per user (no admin prompt) to %LOCALAPPDATA%\Programs\AMETHYST 2026,
;   so the built-in updater can replace the app without UAC.
; - Start-menu shortcut, optional desktop shortcut, uninstaller, .amethyst file association.
; - When run silently by the updater it closes AMETHYST, updates, and starts it again.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define AppName "AMETHYST 2026"
#define AppExe "AMETHYST 2026.exe"
#define BuildDir "..\build\VocalDAW_artefacts"

[Setup]
AppId={{6E2B7C1A-4F8D-4B3E-9A61-2D7C5E8F1A90}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=AMETHYST
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=AMETHYST-2026-Setup-{#AppVersion}
SetupIconFile={#BuildDir}\JuceLibraryCode\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=force
RestartApplications=no
ChangesAssociations=yes
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#BuildDir}\Release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; double-click a .amethyst project to open it
Root: HKCU; Subkey: "Software\Classes\.amethyst"; ValueType: string; ValueName: ""; ValueData: "AMETHYST2026.Project"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\AMETHYST2026.Project"; ValueType: string; ValueName: ""; ValueData: "AMETHYST project"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\AMETHYST2026.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKCU; Subkey: "Software\Classes\AMETHYST2026.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

[Run]
; normal install: offer to start it
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent
; silent update from inside the app: start it again
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: WizardSilent
