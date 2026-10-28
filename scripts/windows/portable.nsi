; ============================================================
; HarmonyCast Single-File Portable Executable (NSIS Script)
; Produces a 100% self-contained single .exe file.
; Extracts silently to %TEMP%, launches HarmonyCast.exe,
; and purges temporary files when closed.
; ============================================================

Unicode True
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
ShowInstDetails nevershow

Name "HarmonyCast"
Caption "HarmonyCast"
Icon "${ICON_PATH}"
OutFile "${OUTPUT_EXE}"

Section
  ; Create a secure, isolated temporary directory in %TEMP%
  InitPluginsDir
  SetOutPath "$PLUGINSDIR\app"
  
  ; Bundle all deployed binaries, Qt DLLs, FFmpeg, HDC, and resources
  File /r "${STAGING_DIR}\*"
  
!ifdef SERVER2012
  ; Preconfigure software rasterizer environment for Server 2012
  System::Call 'Kernel32::SetEnvironmentVariable(t "QT_OPENGL", t "software")'
  System::Call 'Kernel32::SetEnvironmentVariable(t "QSG_RHI_BACKEND", t "software")'
  System::Call 'Kernel32::SetEnvironmentVariable(t "QT_QUICK_BACKEND", t "software")'
!endif

  ; Launch the application and wait until user exits
  ExecWait '"$PLUGINSDIR\app\HarmonyCast.exe"'
  
  ; When ExecWait finishes, NSIS automatically purges $PLUGINSDIR
SectionEnd
