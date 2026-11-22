#Requires -Version 5.1
param([Parameter(Mandatory)][ValidateRange(0,2)][int]$Mode)
$ErrorActionPreference = 'Stop'
$vendor = Join-Path $env:ProgramFiles 'Huawei\HwLcdEnhancement'
Set-Location -LiteralPath $vendor
[Environment]::CurrentDirectory = $vendor
Add-Type -Namespace OpenEHub -Name Kernel -MemberDefinition '[DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern bool SetDllDirectory(string path);'
if (![OpenEHub.Kernel]::SetDllDirectory($vendor)) { exit 1 }
Add-Type -Path (Join-Path $vendor 'Util_WPF.dll')
if ([int][LCDUtil.CSharpAdapter.FunctionApi]::Init() -ne 0) { exit 1 }
try {
    # The x64 window calls this adapter only for DRR-WXX with its installed factory calibration.
    foreach ($file in 'native.icc','sRGBICC.icc','displayP3.icc') {
        if (!(Test-Path -LiteralPath (Join-Path $vendor "config\icc\DiracR\$file"))) { exit 1 }
    }
    $type = [LCDUtil.CSharpAdapter.BasicApi]
    $fields = @($type.GetFields([Reflection.BindingFlags]'NonPublic,Static') | Where-Object { $_.FieldType -eq [string] -and $_.Name -match 'deviceType' })
    if ($fields.Count -ne 1) { exit 1 }
    $fields[0].SetValue($null,'DiracR')
    if (![LCDUtil.FeatureManager]::IsSupportColorField()) { exit 1 }
    $field = [LCDUtil.ColorField]::Instance()
    $original = $field.SyncSysColorFieldMode()
    if ([int]$original -lt 0) { exit 1 }
    $rc = $field.ColorFieldSwitch([LCDUtil.ColorFieldMode]$Mode, $original)
    if ([int]$rc -ne 0 -or [int]$field.SyncSysColorFieldMode() -ne $Mode) { exit 1 }
} finally {
    [void][LCDUtil.CSharpAdapter.FunctionApi]::Fini()
}
