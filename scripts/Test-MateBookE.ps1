# Run in 64-bit Windows PowerShell 5.1. All hardware operations are queries.
# The WmiUtil entry points are documented in hal/docs/hardware-hal.md.
#Requires -Version 5.1
param([switch]$ModelOnly)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit Windows PowerShell.' }

$bios = Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS'
if ($bios.SystemManufacturer -ne 'HUAWEI' -or $bios.SystemProductName -ne 'DRR-WXX') {
    throw "This probe targets HUAWEI DRR-WXX. Detected $($bios.SystemProductName)."
}

if ($ModelOnly) { return }
$battery = [Windows.Devices.Power.Battery,Windows.Devices.Power,ContentType=WindowsRuntime]::AggregateBattery.GetReport()
$summary = [ordered]@{
    Model = $bios.SystemProductName
    BIOS = $bios.BIOSVersion
    BatteryStatus = [string]$battery.Status
    RemainingCapacity_mWh = $battery.RemainingCapacityInMilliwattHours
    FullChargeCapacity_mWh = $battery.FullChargeCapacityInMilliwattHours
    ChargeRate_mW = $battery.ChargeRateInMilliwatts
}

if (-not ('EHub.SensorLibrary' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace EHub {
    public sealed class SensorLibrary : IDisposable {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll")]
        private static extern bool FreeLibrary(IntPtr module);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int ReadSensor(ref int value, int index);
        private IntPtr module;
        public SensorLibrary(string path) {
            // Resolve dependencies from the vendor directory and System32.
            module = LoadLibraryEx(path, IntPtr.Zero, 0x100 | 0x800);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public int Read(bool fan, ref int value) {
            string name = fan ? "?GetFanSpeed@BiosWmi@HwSDK@@SAHAEAHW4FanSpeedLocateNumber@2@@Z"
                              : "?GetSensorTemperature@BiosWmi@HwSDK@@SAHAEAHW4QuerySensorTemperatureIndex@2@@Z";
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            return ((ReadSensor)Marshal.GetDelegateForFunctionPointer(address, typeof(ReadSensor)))(ref value, 0);
        }
        public void Dispose() {
            if (module != IntPtr.Zero) FreeLibrary(module);
            module = IntPtr.Zero;
        }
    }
}
'@
}
$dll = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Huawei\PCManager\WmiUtil.dll'
$library = $null
try {
    $library = [EHub.SensorLibrary]::new($dll)
    foreach ($field in 'Fan0_RPM', 'Sensor0_C') {
        try {
            $value = [int]::MaxValue
            $result = $library.Read(($field -eq 'Fan0_RPM'), [ref]$value)
            $summary[$field] = if ($result -ne 0) { "Unavailable (vendor status $result)" }
                elseif ($value -eq [int]::MaxValue) { 'Unavailable (no sample)' }
                else { $value }
        } catch { $summary[$field] = $_.Exception.GetBaseException().Message }
    }
} catch { $summary['ThermalQueryError'] = $_.Exception.GetBaseException().Message }
finally { if ($null -ne $library) { $library.Dispose() } }
[pscustomobject]$summary
