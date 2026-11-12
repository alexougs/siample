; Installeur Windows de Siample (Inno Setup 6).
; Compile depuis la racine du depot :  ISCC.exe /DAppVersion=0.5.0 packaging\Windows\Siample.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6C1F7D2A-52B4-4A0B-9E53-51A4D5E7A0C1}
AppName=Siample
AppVersion={#AppVersion}
AppPublisher=Siample
DefaultDirName={autopf}\Siample
DefaultGroupName=Siample
DisableProgramGroupPage=yes
DisableDirPage=yes
OutputDir=..\..\dist
OutputBaseFilename=Installer Siample
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Siample

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Types]
Name: "full"; Description: "Installation complete"
Name: "custom"; Description: "Installation personnalisee"; Flags: iscustom

[Components]
Name: "vst3"; Description: "Plugin VST3 (FL Studio et autres DAW)"; Types: full custom
Name: "standalone"; Description: "Version autonome (pour tester sans DAW)"; Types: full

[Files]
Source: "..\..\build\Siample_artefacts\Release\VST3\Siample.vst3\*"; DestDir: "{commoncf64}\VST3\Siample.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3
Source: "..\..\build\Siample_artefacts\Release\Standalone\Siample.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: standalone
Source: "LISEZMOI.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Siample"; Filename: "{app}\Siample.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Siample.vst3"
