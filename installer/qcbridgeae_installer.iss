; QCBridgeAE — Windows installer (Inno Setup 6), unsigned by decision
; (2026-09-24). Copies the Transmit device into Adobe's shared MediaCore
; folder, which After Effects and Premiere Pro both load from. Admin is
; required because that folder is under Program Files.
;
; Build on the Windows box after cmake --build build-release:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\qcbridgeae_installer.iss
; → dist\QCBridgeAE-<version>-Setup-x64.exe
#define MyAppName "QCBridgeAE"
#define MyAppVersion "0.2.0"
#define MyAppPublisher "cbkow"
#define MyAppURL "https://github.com/cbkow/QCBridgeAE"
#define PluginFile "QCBridgeAE-Transmit.prm"

[Setup]
AppId={{3B9D7E62-0C4A-4F5B-8E17-6D2C9A1B4F58}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 {#MyAppPublisher}
; The device lives in MediaCore; nothing else is installed, so the
; "app" folder is the plug-in folder itself and no directory page shows.
DefaultDirName={commonpf}\Adobe\Common\Plug-ins\7.0\MediaCore
DisableDirPage=yes
DisableProgramGroupPage=yes
CreateAppDir=no
OutputDir=..\dist
OutputBaseFilename=QCBridgeAE-{#MyAppVersion}-Setup-x64
Compression=lzma2/max
SolidCompression=yes
UninstallFilesDir={commonappdata}\QCBridgeAE
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; Shown on the welcome page under the title.
WelcomeLabel2=This will install the QCBridgeAE → QCView video preview device for After Effects and Premiere Pro.%n%nQuit After Effects and Premiere Pro before continuing; they load devices at launch.

[Files]
Source: "..\build-release\{#PluginFile}"; DestDir: "{commonpf}\Adobe\Common\Plug-ins\7.0\MediaCore"; Flags: ignoreversion

[Code]
// A running host holds the device file open, and the copy fails with
// access denied; say so up front instead. tasklist is the one process
// list Inno's Pascal can reach without a DLL.
function HostRunning(const ExeName: String): Boolean;
var
  R: Integer;
  Lines: TArrayOfString;
  TmpFile: String;
  I: Integer;
begin
  Result := False;
  TmpFile := ExpandConstant('{tmp}\tasklist.txt');
  if Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq ' + ExeName + '" /NH > "' + TmpFile + '"', '', SW_HIDE, ewWaitUntilTerminated, R) then
    if LoadStringsFromFile(TmpFile, Lines) then
      for I := 0 to GetArrayLength(Lines) - 1 do
        if Pos(Lowercase(ExeName), Lowercase(Lines[I])) > 0 then
          Result := True;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not DirExists(ExpandConstant('{commonpf}\Adobe\Common\Plug-ins\7.0\MediaCore')) then
  begin
    MsgBox('Adobe''s MediaCore plug-in folder was not found (Program Files\Adobe\Common\Plug-ins\7.0\MediaCore). Install After Effects or Premiere Pro first.', mbError, MB_OK);
    Result := False;
    exit;
  end;
  if HostRunning('AfterFX.exe') or HostRunning('Adobe Premiere Pro.exe') then
  begin
    MsgBox('Quit After Effects and Premiere Pro first: they hold the device file open while they run. Then run this installer again.', mbError, MB_OK);
    Log('refused: an Adobe host is running');
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MsgBox('Installed. In After Effects: Settings > Video Preview - tick Enable Mercury Transmit and QCBridgeAE > QCView, and untick "Disable video output when in the background". In Premiere Pro the same ticks are under Settings > Playback. Then in QCView: File > Connect to After Effects (or Premiere Pro).', mbInformation, MB_OK);
end;
