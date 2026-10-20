param([Parameter(Mandatory=$true)][string]$QtPrefix, [string]$BuildDirectory = 'build-release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path (Join-Path $root $BuildDirectory) 'bin'
if (!(Test-Path (Join-Path $destination 'OpenXRFovManager.exe'))) { throw 'Build the manager first' }
$deploy = Join-Path $QtPrefix 'bin\windeployqt.exe'
if (Test-Path $deploy) {
    & $deploy --release --no-translations (Join-Path $destination 'OpenXRFovManager.exe')
    if ($LASTEXITCODE) { throw 'Qt deployment failed' }
} else {
    # Some SDK distributions omit windeployqt. This app uses only these modules.
    foreach ($module in @('Core','Gui','Widgets','Network')) {
        Copy-Item -LiteralPath (Join-Path $QtPrefix "bin\Qt6$module.dll") -Destination $destination -Force
    }
    foreach ($plugin in @('platforms\qwindows.dll','platforms\qminimal.dll','styles\qmodernwindowsstyle.dll')) {
        $source = Join-Path (Join-Path $QtPrefix 'plugins') $plugin
        if (Test-Path $source) {
            $folder = Join-Path $destination (Split-Path $plugin -Parent)
            New-Item -ItemType Directory -Force $folder | Out-Null
            Copy-Item -LiteralPath $source -Destination $folder -Force
        }
    }
}
Copy-Item -LiteralPath (Join-Path $root 'README.md'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse -Force
$licenseDirectory = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Force $licenseDirectory | Out-Null
$qtLicenses = Join-Path $QtPrefix 'licenses'
if (Test-Path $qtLicenses) { Copy-Item -LiteralPath $qtLicenses -Destination (Join-Path $licenseDirectory 'Qt') -Recurse -Force }
$openxrLicense = Join-Path $root 'external\OpenXR-SDK\LICENSE'
if (Test-Path $openxrLicense) { Copy-Item -LiteralPath $openxrLicense -Destination (Join-Path $licenseDirectory 'OpenXR.txt') -Force }
Write-Host "Package is ready in $destination. Microsoft Visual C++ x64 runtime is required."
