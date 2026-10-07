; Windows installer for the Maki Plugins (Inno Setup 6). Installs each plugin's .vst3 bundle into the
; standard VST3 folder (C:\Program Files\Common Files\VST3), with a page to pick which plugins to install
; and an uninstaller in Windows' "Apps" list.
;
; Build with build-installer.ps1, or directly:
;   ISCC.exe /DPluginDir="C:\gen plugins" /DAppVersion=1.0.0 MakiPlugins.iss
; Only the plugins found in PluginDir are offered.

#ifndef PluginDir
  #define PluginDir "C:\gen plugins"
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define Has(Name) FileExists(PluginDir + "\" + Name + ".vst3\Contents\x86_64-win\" + Name + ".vst3")

[Setup]
AppId={{6E1B7A52-3C1D-4D8E-9A57-4D61B2C0F1A7}
AppName=Maki Plugins
AppVersion={#AppVersion}
AppPublisher=Maki Plugins
AppPublisherURL=https://github.com/MaczorLuksemburg/klaudiuszoweVST
DefaultDirName={commoncf64}\VST3
DirExistsWarning=no
DisableProgramGroupPage=yes
UninstallFilesDir={autopf}\Maki Plugins
UninstallDisplayName=Maki Plugins (VST3)
LicenseFile=..\..\LICENSE
OutputDir=output
OutputBaseFilename=MakiPlugins-Setup-{#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2
SolidCompression=yes

[Messages]
SelectDirLabel3=The plugins go into this VST3 folder. The standard one (Common Files\VST3) is found by every DAW automatically.

[Types]
Name: "full"; Description: "All plugins"
Name: "custom"; Description: "Choose plugins"; Flags: iscustom

[Components]
#if Has("DynMap")
Name: "dynmap"; Description: "DynMap - multiband dynamic mapping (drawn compressor curves)"; Types: full custom
#endif
#if Has("MSC")
Name: "msc"; Description: "MSC - dynamic pan, Haas, chorus and stereo image"; Types: full custom
#endif
#if Has("StereoScale")
Name: "stereoscale"; Description: "StereoScale - stereo width"; Types: full custom
#endif
#if Has("FloorMatch")
Name: "floormatch"; Description: "FloorMatch - dialogue noise floor matcher"; Types: full custom
#endif

[Files]
#if Has("DynMap")
Source: "{#PluginDir}\DynMap.vst3\*"; DestDir: "{app}\DynMap.vst3"; Components: dynmap; Flags: recursesubdirs createallsubdirs ignoreversion
#endif
#if Has("MSC")
Source: "{#PluginDir}\MSC.vst3\*"; DestDir: "{app}\MSC.vst3"; Components: msc; Flags: recursesubdirs createallsubdirs ignoreversion
#endif
#if Has("StereoScale")
Source: "{#PluginDir}\StereoScale.vst3\*"; DestDir: "{app}\StereoScale.vst3"; Components: stereoscale; Flags: recursesubdirs createallsubdirs ignoreversion
#endif
#if Has("FloorMatch")
Source: "{#PluginDir}\FloorMatch.vst3\*"; DestDir: "{app}\FloorMatch.vst3"; Components: floormatch; Flags: recursesubdirs createallsubdirs ignoreversion
#endif

[Code]
// A DAW keeps loaded .vst3 files locked, so installing over them would fail half way: ask first.
function InitializeSetup(): Boolean;
begin
  Result := MsgBox('Please close your DAW (Reaper, FL Studio, Ableton, Cubase...) before continuing, ' +
                   'so the plugin files are not in use.' + #13#10#13#10 + 'Continue?',
                   mbConfirmation, MB_YESNO) = IDYES;
end;
