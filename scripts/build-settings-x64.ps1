#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ build tools and Windows SDK 10.0.26100 first.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild and the x64 C++ toolset are required.' }
$project = Join-Path $root 'Tools\EGoTouchSettings\EGoTouchSettings.vcxproj'
$output = Join-Path $root 'build\settings-x64'
$intermediate = Join-Path $root 'build\obj-settings-x64'
& $msbuild $project /restore /nologo /verbosity:minimal /p:Platform=x64 /p:Configuration=Release "/p:OutDir=$output/" "/p:IntDir=$intermediate/" /p:WindowsAppSDKSelfContained=true
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$helpers = Join-Path $root 'Tools\EGoTouchSettings\MateBookHelpers.vcxproj'
foreach ($helper in 'Pen', 'Keyboard', 'Services') {
    & $msbuild $helpers /nologo /verbosity:minimal /p:Platform=x64 /p:Configuration=Release "/p:Helper=$helper" "/p:OutDir=$output/" "/p:IntDir=$intermediate/$helper/"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
Get-Item -LiteralPath (Join-Path $output 'openehub.exe')
