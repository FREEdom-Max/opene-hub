#Requires -Version 5.1
param(
    [string]$Version = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$binDir = Join-Path $root 'build\settings-x64'
$stageDir = Join-Path $root 'build\msi-stage-x64'
$runtimeDir = Join-Path $stageDir 'runtime'
$outMsi = Join-Path $root 'build\OpenEHubSetup-x64.msi'

if (-not $Version) {
    $appVer = Get-Content -LiteralPath (Join-Path $root 'Common\include\AppVersion.h') -Raw
    if ($appVer -match 'OPENEGO_VERSION_STRING\s+"([^"]+)"') {
        $Version = $Matches[1]
    } else {
        $Version = '1.0.0'
    }
}

Write-Host "=== Packaging OpenE hub x64 MSI (v$Version) ===" -ForegroundColor Cyan

if (-not $SkipBuild) {
    Write-Host "[1/3] Building x64 binaries..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-settings-x64.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$coreFiles = @(
    'openehub.exe',
    'openehubHw.exe',
    'openehubPen.exe',
    'openehubKeyboard.exe',
    'openehubServices.exe'
)
foreach ($f in $coreFiles) {
    $full = Join-Path $binDir $f
    if (-not (Test-Path -LiteralPath $full)) {
        throw "Missing required binary: $full"
    }
}

Write-Host "[2/3] Staging clean runtime files (excluding PDB/ILK/LIB/EXP and unused locales)..."
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDir, $runtimeDir, (Join-Path $stageDir 'scripts') -Force | Out-Null

# Core executables (declared explicitly in WXS)
foreach ($f in $coreFiles) {
    Copy-Item -LiteralPath (Join-Path $binDir $f) -Destination (Join-Path $stageDir $f) -Force
}
Copy-Item -LiteralPath (Join-Path $root 'scripts\Set-OpenEHubGamut.ps1') `
          -Destination (Join-Path $stageDir 'scripts\Set-OpenEHubGamut.ps1') -Force

# Runtime files in root of build\settings-x64 (DLL/XBF/PRI/WINMD/JSON/PNG/RestartAgent)
$skipExtensions = @('.pdb', '.ilk', '.lib', '.exp', '.obj', '.log')
$skipExact = @(
    'openehub.exe',
    'openehubHw.exe',
    'openehubPen.exe',
    'openehubKeyboard.exe',
    'openehubServices.exe',
    'openehubFan.exe'
)
Get-ChildItem -LiteralPath $binDir -File | ForEach-Object {
    if ($skipExtensions -contains $_.Extension.ToLowerInvariant()) { return }
    if ($skipExact -contains $_.Name.ToLowerInvariant()) { return }
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $runtimeDir $_.Name) -Force
}

# Keep only essential subdirectories: Microsoft.UI.Xaml, NpuDetect, zh-CN, zh-TW, en-us
$keepDirs = @('Microsoft.UI.Xaml', 'NpuDetect', 'zh-CN', 'zh-TW', 'en-us')
foreach ($d in $keepDirs) {
    $src = Join-Path $binDir $d
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $runtimeDir $d) -Recurse -Force
    }
}

$stagedBytes = (Get-ChildItem -LiteralPath $stageDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("    Staged payload: {0:N1} MB" -f ($stagedBytes / 1MB))

Write-Host "[3/3] Compiling MSI with WiX Toolset..."
$wix = (Get-Command wix -ErrorAction SilentlyContinue).Source
if (-not $wix) {
    $candidate = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    if (Test-Path -LiteralPath $candidate) { $wix = $candidate }
}
if (-not $wix) {
    throw 'WiX Toolset v5 CLI not found. Install via: dotnet tool install --global wix --version 5.0.2'
}

Push-Location $root
try {
    & $wix build `
        -ext WixToolset.UI.wixext `
        -ext WixToolset.Util.wixext `
        -arch x64 `
        -d "BuildVersion=$Version" `
        -d "StageDir=$stageDir" `
        -d "RuntimeDir=$runtimeDir" `
        -loc 'scripts\zh-CN-x64.wxl' `
        'scripts\OpenEHubSetup-x64.wxs' `
        -out $outMsi
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

$msiItem = Get-Item -LiteralPath $outMsi
Write-Host ("=== Done: {0} ({1:N1} MB) ===" -f $msiItem.FullName, ($msiItem.Length / 1MB)) -ForegroundColor Green
