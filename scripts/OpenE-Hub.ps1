#Requires -Version 5.1
[CmdletBinding()]
param([ValidateSet('Window','Status','Quiet','Daily','Performance','Restore')][string]$Action = 'Window',
      [switch]$PolicyOnly)
$ErrorActionPreference = 'Stop'

# Test-MateBookE checks the model before loading vendor code.
$probe = Join-Path $PSScriptRoot 'Test-MateBookE.ps1'
$sample = & $probe -ModelOnly:$PolicyOnly
if (-not ('OpenE.BatteryPolicy' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace OpenE {
    public static class BatteryPolicy {
        static Guid processor = new Guid("54533251-82be-4824-96c1-47b60b740d00");
        static Guid boost = new Guid("be337238-0d82-4146-a960-4f3749d470c7");
        [DllImport("powrprof.dll")]
        static extern uint PowerGetActiveScheme(IntPtr root, out IntPtr scheme);
        [DllImport("kernel32.dll")]
        static extern IntPtr LocalFree(IntPtr ptr);
        [DllImport("powrprof.dll")]
        static extern uint PowerReadDCValueIndex(IntPtr root, ref Guid scheme, ref Guid group, ref Guid setting, out uint value);
        [DllImport("powrprof.dll")]
        static extern uint PowerWriteDCValueIndex(IntPtr root, ref Guid scheme, ref Guid group, ref Guid setting, uint value);
        [DllImport("powrprof.dll")]
        static extern uint PowerSetActiveScheme(IntPtr root, ref Guid scheme);
        [DllImport("powrprof.dll")]
        static extern uint PowerGetUserConfiguredDCPowerMode(out Guid mode);
        [DllImport("powrprof.dll")]
        static extern uint PowerSetUserConfiguredDCPowerMode(ref Guid mode);
        public static Guid ReadMode() {
            Guid mode;
            Check(PowerGetUserConfiguredDCPowerMode(out mode));
            return mode;
        }
        public static void SetMode(Guid mode) {
            Check(PowerSetUserConfiguredDCPowerMode(ref mode));
            if (ReadMode() != mode) throw new InvalidOperationException("Battery mode readback failed.");
        }
        static void Check(uint result) {
            if (result != 0) throw new Win32Exception((int)result);
        }
        public static Guid Active() {
            IntPtr ptr;
            Check(PowerGetActiveScheme(IntPtr.Zero, out ptr));
            try { return (Guid)Marshal.PtrToStructure(ptr, typeof(Guid)); }
            finally { LocalFree(ptr); }
        }
        public static uint Read(Guid scheme) {
            uint value;
            Check(PowerReadDCValueIndex(IntPtr.Zero, ref scheme, ref processor, ref boost, out value));
            return value;
        }
        public static void Apply(Guid scheme, uint value) {
            Check(PowerWriteDCValueIndex(IntPtr.Zero, ref scheme, ref processor, ref boost, value));
            if (scheme == Active()) Check(PowerSetActiveScheme(IntPtr.Zero, ref scheme));
            if (Read(scheme) != value) throw new InvalidOperationException("Power policy readback failed.");
        }
    }
}
'@
}
$profileDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'opene-hub\profiles'
$profiles = [ordered]@{
    Quiet = @{ Label='静音'; Boost=0; Mode='961cc777-2547-4f9d-8174-7d86181b8a7a' }
    Daily = @{ Label='日常'; Boost=1; Mode='961cc777-2547-4f9d-8174-7d86181b8a7a' }
    Performance = @{ Label='性能'; Boost=2; Mode='ded574b5-45a0-4f42-8737-46345c09c238' }
}
function Save-ProfileState($path, $state) {
    [void][IO.Directory]::CreateDirectory($profileDirectory)
    $temp = "$path.$PID.tmp"
    try {
        [IO.File]::WriteAllText($temp, ($state | ConvertTo-Json -Compress))
        if ([IO.File]::Exists($path)) {
            [IO.File]::Replace($temp, $path, "$temp.bak")
        } else { [IO.File]::Move($temp, $path) }
    } finally {
        [IO.File]::Delete($temp)
        [IO.File]::Delete("$temp.bak")
    }
}
function Set-BatteryProfile([string]$requested) {
    $mutex = [Threading.Mutex]::new($false, 'Global\OpenEHub-BatteryBoost')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(5000) }
        catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'Another profile change is in progress.' }
        $scheme = [OpenE.BatteryPolicy]::Active()
        $saved = Join-Path $profileDirectory 'battery-profile.json'
        $state = $null
        if ([IO.File]::Exists($saved)) {
            $state = [IO.File]::ReadAllText($saved) | ConvertFrom-Json
            if ($state.PSObject.Properties["MaxPerformance"] -or $state.PSObject.Properties["MaxPerformance1"]) {
                throw "This backup includes processor limits. Use the OpenE hub Battery page to change or restore it."
            }
            if ($requested -ne 'Restore' -and $scheme -ne [guid]$state.Scheme) {
                throw '电源方案已切换。请先恢复此前设置，再选择新档位。'
            }
            $scheme = [guid]$state.Scheme
        }
        $legacy = Join-Path $profileDirectory "$scheme.txt"
        $current = [OpenE.BatteryPolicy]::Read($scheme)
        $mode = [OpenE.BatteryPolicy]::ReadMode().ToString()
        if ($null -eq $state) {
            $original = $current
            if ([IO.File]::Exists($legacy) -and $current -eq 0) {
                $original = [uint32]([IO.File]::ReadAllText($legacy))
                if ($original -gt 6) { throw 'Unrecognized saved boost policy.' }
            }
            $state = [pscustomobject]@{ Scheme="$scheme"; Boost=$original; Mode=$mode; OwnedBoost=@($current); OwnedMode=@($mode) }
        }
        if ($requested -eq 'Restore') {
            if ($current -in $state.OwnedBoost -and $current -ne $state.Boost) {
                [OpenE.BatteryPolicy]::Apply($scheme, $state.Boost)
            }
            if ($mode -in $state.OwnedMode -and $mode -ne $state.Mode) { [OpenE.BatteryPolicy]::SetMode($state.Mode) }
            if (Test-Path -LiteralPath $legacy) { [IO.File]::Delete($legacy) }
            if (Test-Path -LiteralPath $saved) { [IO.File]::Delete($saved) }
            return
        }
        if ($current -notin $state.OwnedBoost -or $mode -notin $state.OwnedMode) {
            throw '电源设置已在其他位置更改。请先点击恢复，再选择档位。'
        }
        $target = $profiles[$requested]
        # Record both endpoints first so Restore can recover an interrupted change.
        $state.OwnedBoost = @($current, $target.Boost)
        $state.OwnedMode = @($mode, $target.Mode)
        Save-ProfileState $saved $state
        [OpenE.BatteryPolicy]::Apply($scheme, $target.Boost)
        if ($mode -ne $target.Mode) { [OpenE.BatteryPolicy]::SetMode($target.Mode) }
        $state.OwnedBoost = @($target.Boost)
        $state.OwnedMode = @($target.Mode)
        Save-ProfileState $saved $state
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

function Get-PolicyStatus {
    $scheme = [OpenE.BatteryPolicy]::Active()
    $boost = [OpenE.BatteryPolicy]::Read($scheme)
    $mode = [OpenE.BatteryPolicy]::ReadMode().ToString()
    $label = '自定义'
    foreach ($profile in $profiles.Values) {
        if ($profile.Boost -eq $boost -and $profile.Mode -eq $mode) { $label = $profile.Label }
    }
    [pscustomobject]@{
        Scheme = $scheme
        BatteryBoostIndex = $boost
        BatteryPowerMode = $mode
        BatteryProfile = $label
        RestoreAvailable = [IO.File]::Exists((Join-Path $profileDirectory "$scheme.txt")) -or [IO.File]::Exists((Join-Path $profileDirectory 'battery-profile.json'))
    }
}

if ($Action -ne 'Window') {
    if ($Action -ne 'Status') { Set-BatteryProfile $Action }
    $sample
    Get-PolicyStatus
    return
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$form = [Windows.Forms.Form]::new()
$form.Text = 'opene-hub · MateBook E 2023'
$form.Font = [Drawing.Font]::new('Microsoft YaHei UI', 11)
$form.AutoScaleMode = 'Dpi'
$form.AutoSize = $true
$form.AutoSizeMode = 'GrowAndShrink'
$form.StartPosition = 'CenterScreen'
$layout = [Windows.Forms.FlowLayoutPanel]::new()
$layout.FlowDirection = 'TopDown'
$layout.WrapContents = $false
$layout.AutoSize = $true
$layout.Padding = [Windows.Forms.Padding]::new(16)
$form.Controls.Add($layout)
$metrics = [Windows.Forms.Label]::new()
$metrics.AutoSize = $true
$layout.Controls.Add($metrics)
$buttons = @{}
foreach ($entry in ([ordered]@{Quiet='静音 · 关闭睿频';Daily='日常 · 节能优先，允许睿频';Performance='性能 · 性能优先，允许睿频';Restore='恢复此前的电池设置';Refresh='刷新读数'}).GetEnumerator()) {
    $button = [Windows.Forms.Button]::new()
    $button.AutoSize = $true
    $button.Text = $entry.Value
    $button.Tag = $entry.Key
    $layout.Controls.Add($button)
    $buttons[$entry.Key] = $button
}
function Show-Metrics($reading) {
    $policy = Get-PolicyStatus
    $rate = if ($null -eq $reading.ChargeRate_mW) { '暂无读数' } else { '{0:N2} W' -f ($reading.ChargeRate_mW / 1000) }
    $rpm = if ($reading.Fan0_RPM -is [int]) { "$($reading.Fan0_RPM) RPM" } else { '读取失败（可尝试管理员权限）' }
    $temp = if ($reading.Sensor0_C -is [int]) { "$($reading.Sensor0_C) °C" } else { '读取失败（可尝试管理员权限）' }
    $metrics.Text = @(
        "电池净充电功率（负值为放电）  $rate"
        "风扇  $rpm"
        "温度传感器 0  $temp"
        "当前电池档位  $($policy.BatteryProfile)"
    ) -join [Environment]::NewLine
    $buttons.Restore.Enabled = $policy.RestoreAvailable
}
foreach ($button in $buttons.Values) {
    $button.Add_Click({
        try {
            if ($this.Tag -ne 'Refresh') { Set-BatteryProfile $this.Tag }
            Show-Metrics (& $probe)
        } catch {
            $buttons.Restore.Enabled = $true
            [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'opene-hub')
        }
    })
}
try {
    Show-Metrics $sample
    [void]$form.ShowDialog()
} finally { $form.Dispose() }
