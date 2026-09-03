; NSIS installer for PhotoLife.
;
; Expects a windeployqt-populated staging tree passed in via /DSTAGE_DIR and a
; version via /DAPP_VERSION, e.g.:
;   makensis /DSTAGE_DIR=dist /DAPP_VERSION=0.1.0 packaging/windows/photolife.nsi

Unicode true
!ifndef APP_VERSION
  !define APP_VERSION "0.1.0"
!endif
!ifndef STAGE_DIR
  !define STAGE_DIR "dist"
!endif

Name "PhotoLife"
OutFile "PhotoLife-${APP_VERSION}-Setup.exe"
InstallDir "$PROGRAMFILES64\PhotoLife"
InstallDirRegKey HKLM "Software\PhotoLife" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!define MUI_ICON "photolife.ico"
!define MUI_UNICON "photolife.ico"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\PhotoLife.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "PhotoLife" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"

  CreateShortcut "$SMPROGRAMS\PhotoLife.lnk" "$INSTDIR\PhotoLife.exe"
  CreateShortcut "$DESKTOP\PhotoLife.lnk" "$INSTDIR\PhotoLife.exe"

  WriteRegStr HKLM "Software\PhotoLife" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhotoLife" \
    "DisplayName" "PhotoLife"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhotoLife" \
    "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhotoLife" \
    "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhotoLife" \
    "Publisher" "PhotoLife"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\PhotoLife.lnk"
  Delete "$DESKTOP\PhotoLife.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\PhotoLife"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhotoLife"
SectionEnd
