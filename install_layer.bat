@echo off
setlocal
if not exist "%~dp0FovLayerControl.exe" (
  echo ERROR: Extract the complete Windows archive before running this script.
  exit /b 2
)
"%~dp0FovLayerControl.exe" --check
if errorlevel 1 exit /b %errorlevel%
"%~dp0FovLayerControl.exe" --enable
set "result=%errorlevel%"
if not "%result%"=="0" echo ERROR: Installation failed with code %result%. See the diagnostic message above.
if "%result%"=="0" echo Layer registered. Restart your VR application.
exit /b %result%
