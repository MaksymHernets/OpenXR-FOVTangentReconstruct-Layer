param([string]$QtPrefix = $env:Qt6_DIR, [string]$BuildDirectory = 'build-release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install the Visual Studio Desktop development with C++ workload' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force (Join-Path $root $BuildDirectory) | Out-Null
$environmentScript = Join-Path (Join-Path $root $BuildDirectory) 'compiler-environment.cmd'
@("@echo off", "call `"$devcmd`" -arch=x64 -host_arch=x64 >nul", 'if errorlevel 1 exit /b 1', 'set') | Set-Content -LiteralPath $environmentScript
$environment = & cmd.exe /d /c $environmentScript
if ($LASTEXITCODE) { throw 'Compiler environment initialization failed' }
$compilerPaths = @()
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        if ($matches[1] -ieq 'Path') { $compilerPaths += $matches[2] }
        else { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
}
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:Path = "$ninja;$($compilerPaths -join ';')"
Set-Location $root
$arguments = @('-S','.', '-B',$BuildDirectory,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release')
if ($QtPrefix) { $arguments += "-DCMAKE_PREFIX_PATH=$QtPrefix" }
& cmake @arguments
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $BuildDirectory -j 4
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $BuildDirectory --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
