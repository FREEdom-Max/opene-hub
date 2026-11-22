#Requires -Version 5.1
[CmdletBinding()]
param([ValidateSet('Window','Status','Quiet','Restore')][string]$Action = 'Window')
$ErrorActionPreference = 'Stop'

# Test-MateBookE checks the model before loading vendor code.
$probe = Join-Path $PSScriptRoot 'Test-MateBookE.ps1'
$sample = & $probe
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
            if (scheme != Active()) throw new InvalidOperationException("Active power scheme changed.");
            Check(PowerWriteDCValueIndex(IntPtr.Zero, ref scheme, ref processor, ref boost, value));
            if (scheme == Active()) Check(PowerSetActiveScheme(IntPtr.Zero, ref scheme));
            if (Read(scheme) != value) throw new InvalidOperationException("Power policy readback failed.");
        }
    }
}
'@
}
$profileDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'opene-hub\profiles'
function Set-BatteryProfile([string]$requested) {
    $mutex = [Threading.Mutex]::new($false, 'Global\OpenEHub-BatteryBoost')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(5000) }
        catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'Another profile change is in progress.' }
        $scheme = [OpenE.BatteryPolicy]::Active()
        $current = [OpenE.BatteryPolicy]::Read($scheme)
        $saved = Join-Path $profileDirectory "$scheme.txt"
        if ($requested -eq 'Quiet') {
            if (-not [IO.File]::Exists($saved)) {
                if ($current -eq 0) { return }
                [void][IO.Directory]::CreateDirectory($profileDirectory)
                $temp = "$saved.$PID.tmp"
                try {
                    [IO.File]::WriteAllText($temp, [string]$current)
                    [IO.File]::Move($temp, $saved)
                } finally { [IO.File]::Delete($temp) }
            } elseif ($current -ne 0 -and $current -ne [uint32]([IO.File]::ReadAllText($saved))) {
                throw 'Power policy changed outside opene-hub. Restore first to keep the newer value.'
            }
            [OpenE.BatteryPolicy]::Apply($scheme, 0)
        } elseif ([IO.File]::Exists($saved)) {
            $original = [uint32]([IO.File]::ReadAllText($saved))
            if ($original -gt 6) { throw 'Unrecognized saved boost policy.' }
            # Preserve a newer setting selected elsewhere.
            if ($current -eq 0 -or $current -eq $original) {
                [OpenE.BatteryPolicy]::Apply($scheme, $original)
            }
            [IO.File]::Delete($saved)
        }
    } finally {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

function Get-PolicyStatus {
    $scheme = [OpenE.BatteryPolicy]::Active()
    [pscustomobject]@{
        Scheme = $scheme
        BatteryBoostIndex = [OpenE.BatteryPolicy]::Read($scheme)
        RestoreAvailable = [IO.File]::Exists((Join-Path $profileDirectory "$scheme.txt"))
    }
}

if ($Action -ne 'Window') {
    if ($Action -in 'Quiet','Restore') { Set-BatteryProfile $Action }
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
foreach ($entry in ([ordered]@{Quiet='电池静音 · 关闭睿频';Restore='恢复此前的睿频设置';Refresh='刷新读数'}).GetEnumerator()) {
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
        "电池睿频策略索引  $($policy.BatteryBoostIndex)（0 为禁用）"
    ) -join [Environment]::NewLine
    $buttons.Restore.Enabled = $policy.RestoreAvailable
}
foreach ($button in $buttons.Values) {
    $button.Add_Click({
        try {
            if ($this.Tag -ne 'Refresh') { Set-BatteryProfile $this.Tag }
            Show-Metrics (& $probe)
        } catch {
            [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'opene-hub')
        }
    })
}
try {
    Show-Metrics $sample
    [void]$form.ShowDialog()
} finally { $form.Dispose() }
