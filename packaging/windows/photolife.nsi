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

; Windows locks a running program's files, so updating while PhotoLife is open
; fails part-way with "Error opening file for writing". Opening the installed
; PhotoLife.exe for writing tells whether it's running; ask until it isn't.
Function EnsureNotRunning
  IfFileExists "$INSTDIR\PhotoLife.exe" 0 done
  retry:
    ClearErrors
    FileOpen $0 "$INSTDIR\PhotoLife.exe" a
    IfErrors locked
    FileClose $0
    Goto done
  locked:
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "PhotoLife is still running.$\r$\n$\r$\nClose it, then click Retry. If its window is already closed, end PhotoLife.exe in Task Manager first." \
      IDRETRY retry
    Abort
  done:
FunctionEnd

Section "PhotoLife" SecMain
  SectionIn RO
  Call EnsureNotRunning
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
