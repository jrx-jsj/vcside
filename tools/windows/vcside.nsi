; Windows installer of VCSIDE (paths read while compiling use "/", so makensis on Linux finds them), made with NSIS (makensis) by tools/release.sh.
;   makensis -DVERSION=0.1.0 -DSOURCE=<folder with vcside.exe and data> -DOUTFILE=<setup.exe> vcside.nsi
; It installs for the current user, in %LOCALAPPDATA%\Programs\VCSIDE (no administrator rights
; needed), with a Start menu shortcut, an optional desktop one and an uninstaller listed in
; "Installed apps". The settings (%APPDATA%\vcside) are not touched by the uninstaller.

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !error "VERSION is not set"
!endif
!ifndef SOURCE
  !error "SOURCE is not set"
!endif
!ifndef OUTFILE
  !define OUTFILE "vcside-${VERSION}-windows-x86_64-setup.exe"
!endif

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VCSIDE"

Name "VCSIDE ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\VCSIDE"
InstallDirRegKey HKCU "Software\VCSIDE" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "VCSIDE"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "VCSIDE installer"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!define MUI_ICON "../../assets/icon/vcside.ico"
!define MUI_UNICON "../../assets/icon/vcside.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\vcside.exe"

!insertmacro MUI_PAGE_LICENSE "${SOURCE}/LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Spanish"

LangString DesktopShortcut ${LANG_ENGLISH} "Desktop shortcut"
LangString DesktopShortcut ${LANG_SPANISH} "Acceso directo en el escritorio"

Function .onInit
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Section "VCSIDE" SectionProgram
  SectionIn RO
  SetOutPath "$INSTDIR"
  RMDir /r "$INSTDIR\data"  ; an update replaces the data folder as a whole
  File "${SOURCE}/vcside.exe"
  File "${SOURCE}/LICENSE.txt"
  File /r "${SOURCE}/data"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\VCSIDE.lnk" "$INSTDIR\vcside.exe"
  WriteRegStr HKCU "Software\VCSIDE" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "VCSIDE"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\vcside.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "VCSIDE"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section /o "$(DesktopShortcut)" SectionDesktop
  CreateShortcut "$DESKTOP\VCSIDE.lnk" "$INSTDIR\vcside.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\VCSIDE.lnk"
  Delete "$DESKTOP\VCSIDE.lnk"
  RMDir /r "$INSTDIR\data"
  Delete "$INSTDIR\vcside.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\VCSIDE"
SectionEnd
