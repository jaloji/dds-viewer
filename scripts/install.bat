@echo off
rem Installs the DDS WIC codec (Windows Explorer thumbnails for every DDS format).
rem Run as administrator from the release folder (DdsWic.dll next to this file).
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist DdsWic.dll (
  echo DdsWic.dll not found next to install.bat. Build the project first, then run dist\install.bat.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\DdsViewer"

if not exist "%APPDIR%" mkdir "%APPDIR%"
if exist "%APPDIR%\DdsWic.dll" regsvr32 /s /u "%APPDIR%\DdsWic.dll"
copy /y DdsWic.dll "%APPDIR%\DdsWic.dll" >nul || (
  echo Could not replace "%APPDIR%\DdsWic.dll": it is probably in use by Explorer.
  echo Run uninstall.bat, sign out or restart, then run install.bat again.
  exit /b 1
)
regsvr32 /s "%APPDIR%\DdsWic.dll" || (
  echo Codec registration failed. If Microsoft Defender attack surface reduction rules are
  echo enforced on this PC, allow the DLL first ^(admin PowerShell^):
  echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%APPDIR%\DdsWic.dll"
  exit /b 1
)
echo DDS codec installed: %APPDIR%\DdsWic.dll
echo.
echo Done. If .dds files still show blank icons, run clear-thumbnails.bat.
