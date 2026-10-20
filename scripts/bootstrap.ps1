$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
if (!(Test-Path external/OpenXR-SDK/include/openxr/openxr.h)) {
    git clone --depth 1 --branch release-1.1.53 https://github.com/KhronosGroup/OpenXR-SDK.git external/OpenXR-SDK
    if ($LASTEXITCODE) { throw 'SDK download failed' }
}
