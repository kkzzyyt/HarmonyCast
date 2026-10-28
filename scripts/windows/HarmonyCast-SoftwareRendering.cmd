@echo off
setlocal
rem Launch HarmonyCast forced to software rasterization fallback
rem Ideal for Windows Server 2012 / 2012 R2 VMs or environments without hardware D3D12/OpenGL drivers
set "QT_OPENGL=software"
set "QSG_RHI_BACKEND=software"
set "QT_QUICK_BACKEND=software"
set "HARMONYCAST_SOFTWARE_RENDERING=1"
start "" "%~dp0HarmonyCast.exe" %*
endlocal
