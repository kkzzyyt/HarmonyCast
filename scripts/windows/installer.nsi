; ============================================================
; HarmonyCast Setup Installer (NSIS Script)
; Produces a modern 1-click Windows installer with Desktop icon,
; Start Menu shortcuts, and Uninstaller.
; ============================================================

Unicode True
RequestExecutionLevel user

!include "MUI2.nsh"

Name "鸿蒙投屏 (HarmonyCast)"
OutFile "${OUTPUT_SETUP}"
InstallDir "$LOCALAPPDATA\Programs\HarmonyCast"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICON_PATH}"
!define MUI_UNICON "${ICON_PATH}"

; Installer Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\HarmonyCast.exe"
!define MUI_FINISHPAGE_RUN_TEXT "立即运行 鸿蒙投屏"
!insertmacro MUI_PAGE_FINISH

; Uninstaller Pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_PAGE_INSTFILES

; Languages
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${STAGING_DIR}\*"
  
  ; Desktop & Start Menu Shortcuts
  CreateShortcut "$DESKTOP\鸿蒙投屏.lnk" "$INSTDIR\HarmonyCast.exe" "" "$INSTDIR\resources\app-icon.ico"
  CreateDirectory "$SMPROGRAMS\鸿蒙投屏"
  CreateShortcut "$SMPROGRAMS\鸿蒙投屏\鸿蒙投屏.lnk" "$INSTDIR\HarmonyCast.exe" "" "$INSTDIR\resources\app-icon.ico"
  CreateShortcut "$SMPROGRAMS\鸿蒙投屏\鸿蒙投屏 (软件渲染模式).lnk" "$INSTDIR\HarmonyCast.exe" "--software-rendering" "$INSTDIR\resources\app-icon.ico"
  CreateShortcut "$SMPROGRAMS\鸿蒙投屏\卸载鸿蒙投屏.lnk" "$INSTDIR\Uninstall.exe"
  
  ; Generate Uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\鸿蒙投屏.lnk"
  Delete "$DESKTOP\鸿蒙投屏 (软件渲染模式).lnk"
  Delete "$SMPROGRAMS\鸿蒙投屏\鸿蒙投屏.lnk"
  Delete "$SMPROGRAMS\鸿蒙投屏\鸿蒙投屏 (软件渲染模式).lnk"
  Delete "$SMPROGRAMS\鸿蒙投屏\卸载鸿蒙投屏.lnk"
  RMDir /r "$SMPROGRAMS\鸿蒙投屏"
  RMDir /r "$INSTDIR"
SectionEnd
