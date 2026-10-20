@echo off
setlocal
if not exist "%~dp0FovLayerControl.exe" (
  echo ERROR: Build the project and run this script from the bin\Release directory.
  exit /b 2
)
"%~dp0FovLayerControl.exe" --enable
set "result=%errorlevel%"
if not "%result%"=="0" echo ERROR: Installation failed with code %result%. Administrator approval is required.
if "%result%"=="0" echo Layer registered. Restart your VR application.
exit /b %result%
