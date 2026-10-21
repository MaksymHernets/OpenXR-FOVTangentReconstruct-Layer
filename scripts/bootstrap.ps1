$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
if (!(Test-Path external/OpenXR-SDK/include/openxr/openxr.h)) {
    git clone --depth 1 --branch release-1.1.53 https://github.com/KhronosGroup/OpenXR-SDK.git external/OpenXR-SDK
    if ($LASTEXITCODE) { throw 'SDK download failed' }
}
if (!(Test-Path external/Vulkan-Headers/include/vulkan/vulkan.h)) {
    git clone --depth 1 --branch v1.3.290 https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers
    if ($LASTEXITCODE) { throw 'Vulkan headers download failed' }
}
if (!(Test-Path external/glslang/bin/glslang.exe)) {
    $compilerArchive = Join-Path $PWD 'external/glslang-16.6.0.zip'
    Invoke-WebRequest 'https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip' -OutFile $compilerArchive
    if ((Get-FileHash -LiteralPath $compilerArchive -Algorithm SHA256).Hash -ne '82BF434E69B9BB4829DE7E2B4BC2C5E7A7861E53D66CF75E5CC70F5F694A8D9B') {
        throw 'Shader compiler archive checksum mismatch'
    }
    Expand-Archive -LiteralPath $compilerArchive -DestinationPath external/glslang -Force
}

& (Join-Path $PSScriptRoot 'bootstrap-nvapi.ps1')

& (Join-Path $PSScriptRoot 'bootstrap-minhook.ps1')
