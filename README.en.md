# OpenE hub

[中文](README.md)

**OpenE hub** is a Windows 11 hardware control app for the **HUAWEI MateBook E 2023 (Intel x64)**. Use the system tray to switch power modes and check battery and accessory status, or open the main window to adjust charging and display settings.

## Supported hardware

- **PC.** HUAWEI MateBook E 2023 (`DRR-WXX`) with an Intel Core i5-1230U or i7-1260U and Windows 11 x64. The recorded hardware test used an i5-1230U with BIOS 1.29.
- **Accessories.** HUAWEI M-Pencil and the RX0I detachable Smart Magnetic Keyboard.

Keep the original Huawei drivers and Huawei PC Manager installation files. Charge protection, display calibration, accessory status and some sensor readings rely on these components. Use the Services page in OpenE hub to disable or restore PC Manager's background services.

## Features

- **Power and performance.** Sync Windows power modes with separate frequency limits for Intel P-cores and E-cores on AC and battery power. With the OpenE hub hardware service enabled and Huawei PC Manager's power service disabled, `Fn + P` cycles through the three modes, shows an on-screen indicator and engages Huawei Turbo mode on Best performance.
- **Battery and sensors.** Check charge or discharge power, battery level, health and cycle count. Read fan speed and CPU temperature on demand. Choose smart charging or one of six manual charge limits from 50% to 100%.
- **Display.** Switch the built-in display between 60 Hz and 120 Hz and select Native, sRGB or Display P3. Adjust Natural Tone, colour temperature and Eye Comfort.
- **Pen and keyboard.** Check connection status, battery level and firmware version. Control the keyboard's wireless connection when detached.
- **Tray and services.** Left-click the tray icon for quick controls or use its context menu to open the main window. The Services page can disable or restore Huawei PC Manager's background services while keeping the low-level touch, pen and hotkey services available.

| Windows power mode | P-core limit | E-core limit |
| --- | ---: | ---: |
| Best power efficiency (including Energy Saver) | 2.0 GHz | 1.5 GHz |
| Balanced | 2.8 GHz | 2.2 GHz |
| Best performance | Uncapped | Uncapped |

On exit, OpenE hub restores the frequency settings that were in effect at startup and turns off Huawei Turbo mode if it enabled it.

## Build and install

Install **Visual Studio 2022 or Build Tools**, the MSVC x64 toolchain, MSBuild and **Windows SDK 10.0.26100**. MSBuild restores the NuGet dependencies during the build.

### Build the MSI installer

Install **WiX Toolset v5** and its `WixToolset.UI.wixext` and `WixToolset.Util.wixext` extensions. From the repository root, run the following command.

```powershell
.\scripts\pack-msi-x64.ps1
```

The script builds the app and produces `build\OpenEHubSetup-x64.msi`. Running the MSI requires administrator privileges to register the hardware service. Launch OpenE hub from the Start menu after installation. Uninstall it from Windows Settings → Apps to remove the hardware service and restore any Huawei background services that OpenE hub disabled.

Prebuilt installers will be listed here when available. [GitHub Releases](https://github.com/FREEdom-Max/opene-hub/releases)

### Run from source

Double-click [opene-hub.cmd](opene-hub.cmd) to build the app if needed and launch it. To build and launch manually, run these commands from the repository root.

```powershell
.\scripts\build-settings-x64.ps1
.\build\settings-x64\openehub.exe
```

OpenE hub appears in the system tray on first launch. Features such as fan speed, temperature and charge protection may prompt for administrator privileges to install the hardware service.

## Diagnostics and source

- The [hardware diagnostic script](scripts/Test-MateBookE.ps1) checks the battery, fan speed and temperature interfaces without changing system settings.
- [Tools/EGoTouchSettings](Tools/EGoTouchSettings) contains the WinUI 3 app, hardware service and accessory helpers.
- [scripts](scripts) contains the x64 build, MSI packaging and diagnostic scripts. [docs](docs) contains technical notes.

## Credits and licence

OpenE hub is derived from [OpenEGo Hub](https://github.com/NihilDigit/openego-hub), which is based on [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev). Development also referenced [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen), [goodies](https://github.com/matebook-e-go/goodies), [EgoTools](https://github.com/SaKongA/EgoTools) and [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService).

This project is released under the [MIT License](LICENSE). Include the applicable copyright and licence notices when redistributing source or binaries. See the [third-party notices](THIRD-PARTY-NOTICES.md) for details. OpenE hub is an independent community project with no official affiliation with Huawei.
