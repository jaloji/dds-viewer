@echo off
rem Removes the DDS WIC codec. Windows' own DDS decoder (BC1-BC3) stays in place.
rem Run as administrator.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\DdsViewer"

if exist "%APPDIR%\DdsWic.dll" (
  regsvr32 /s /u "%APPDIR%\DdsWic.dll"
  rem Explorer may still hold the DLL; it can then be deleted after a restart.
  del /f "%APPDIR%\DdsWic.dll" 2>nul
  if exist "%APPDIR%\DdsWic.dll" echo DdsWic.dll is in use, delete "%APPDIR%" after a restart.
  rmdir "%APPDIR%" 2>nul
  echo DDS codec removed.
)
