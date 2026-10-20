@echo off
setlocal
if not exist "%~dp0FovLayerControl.exe" (
  echo ERROR: FovLayerControl.exe must be beside this script.
  exit /b 2
)
"%~dp0FovLayerControl.exe" --uninstall
set "result=%errorlevel%"
if not "%result%"=="0" echo ERROR: Uninstallation failed with code %result%.
if "%result%"=="0" echo Layer unregistered. User profiles and history are preserved.
exit /b %result%
