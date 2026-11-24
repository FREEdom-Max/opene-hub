<p align="center">
  <img src="Assets/brand/openego-hub-256.png" alt="OpenE hub" width="88">
</p>

<h1 align="center">OpenE hub</h1>

<p align="center">A lightweight hardware control centre for HUAWEI MateBook E 2023 (Intel x64)</p>

<p align="center">
  <a href="#supported-hardware"><img src="https://img.shields.io/badge/Platform-Windows%2011%20x64-317AF7?style=flat-square&labelColor=334155" alt="Platform"></a>
  <a href="https://github.com/NihilDigit/openego-hub"><img src="https://img.shields.io/badge/Forked%20from-OpenEGo%20Hub-64748B?style=flat-square&labelColor=334155" alt="Forked from OpenEGo Hub"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/FREEdom-Max/opene-hub?style=flat-square&label=License&labelColor=334155&color=64748B" alt="License"></a>
</p>

<p align="center"><sub><a href="README.md">中文</a> | <strong>English</strong></sub></p>

**OpenE hub** is a fork of [OpenEGo Hub](https://github.com/NihilDigit/openego-hub) (MateBook E Go · ARM64) adapted and extended for the **HUAWEI MateBook E 2023 (`DRR-WXX`, Intel x64)**. With Huawei PC Manager's resident background services disabled, it provides battery and charge-limit management, Intel hybrid-core frequency ceiling binding, `Fn + P` Turbo mode takeover with a Fluent HUD, on-demand fan/temperature telemetry, display colour and refresh-rate controls, and stylus / smart magnetic keyboard monitoring.

> [!IMPORTANT]
> Keep the factory Huawei drivers and Huawei PC Manager installation directory on the system. Display calibration, accessory telemetry, charge limits, and BIOS/EC sensors load local DLLs shipped with PC Manager on demand, so uninstalling PC Manager breaks those features. PC Manager's resident background services can be disabled in one click on the Services page.

---

## Features

- **Overview & zero-polling tray flyout.** Summarizes hardware specs, live battery wattage, voltage, capacity, cycle count, and health, reads fan RPM and CPU temperature on demand, and shows stylus and magnetic keyboard telemetry. First launch starts silently in the system tray — left-click for the quick flyout, or launch again / use the context menu for the main window.
- **3-tier Power Mode × hybrid-core frequency binding × Huawei Turbo sync.**
  - While running, bidirectionally binds Windows' three Power Modes (on both AC and battery) to Intel hybrid-core frequency ceilings:
    - **Best power efficiency (including Energy Saver):** P-cores `2.0 GHz` · E-cores `1.5 GHz` (preserves Windows default EPP)
    - **Balanced:** P-cores `2.8 GHz` · E-cores `2.2 GHz`
    - **Best performance:** unlimited clocks, automatically engaging Huawei's BIOS/EC **High-Performance (Turbo)** mode
  - Changes made from Windows Settings, the tray flyout, the main window, or hotkeys stay in sync and cleanly restore Windows defaults and disengage Turbo mode on exit, shutdown, or uninstall.
- **`Fn + P` hotkey takeover & Fluent HUD.**
  - When Huawei PC Manager services are disabled, `OpenEHubHwService` intercepts `Fn + P` via zero-polling WMI `OemWMIEvent` subscriptions to cycle through Best efficiency → Balanced → Best performance (Turbo).
  - Displays a light/dark-adaptive **Fluent acrylic HUD** at the top-centre of the screen and briefly suppresses Huawei's duplicate `OSD_Daemon.exe` performance popup without affecting volume, brightness, microphone, or Wi-Fi OSDs.
- **Battery & charge limits.** Integrates Windows 11 Energy Saver and Power Mode alongside vendor smart charging and six manual charge limits (50%–100%) synced on demand via `OpenEHubHwService`.
- **Display & colour.** Switch internal panel refresh rate (120 Hz / 60 Hz), factory-calibrated colour gamuts (Native, sRGB, Display P3), Natural Tone, colour temperature, and Eye Comfort mode.
- **Stylus & smart magnetic keyboard.** Event-driven helper hosts (`openehubPen.exe`, `openehubKeyboard.exe`) using overlapped IPC report HUAWEI M-Pencil and split magnetic keyboard connection, attach, charging, battery level, and firmware version, toggle wireless-on-detach, and show top-centre connection prompts.
- **Service management.** Disable or restore Huawei PC Manager's 6 resident services (`MBAMainService`, `HwPCClientService`, `HiConnectivityService`, `HwDistributedMainService`, `MouseCrossingDaemon`, `WUCSProxy`) in one click while keeping low-level touch/pen and hotkey services (`HuaweiThpService`, `OSD_Daemon`) intact. Automatically restored on uninstall.

---

## Supported hardware

- **Machine:** HUAWEI MateBook E 2023 (`DRR-WXX`, Intel Core i5-1230U / i7-1260U, Windows 11 x64; verified on i5-1230U / BIOS 1.29)
- **Keyboard:** HUAWEI Smart Magnetic Keyboard (RX0I split keyboard)
- **Pen:** HUAWEI M-Pencil

The included read-only diagnostic script [scripts/Test-MateBookE.ps1](scripts/Test-MateBookE.ps1) verifies battery, fan speed, and temperature sensors without modifying system settings.

---

## Installation & usage

### Option 1: MSI installer (recommended)

1. Build `OpenEHubSetup-x64.msi` with `scripts/pack-msi-x64.ps1` or download it from Releases.
2. Run the installer (administrator privileges are required to register `OpenEHubHwService`).
3. Launch from the Start menu or system tray. Uninstall cleanly from Windows Settings → Apps to remove the hardware service and restore any disabled Huawei background services and default power settings.

### Option 2: Portable run

- Double-click [opene-hub.cmd](opene-hub.cmd) in the repository root (builds automatically if needed), or run `build/settings-x64/openehub.exe` directly.

---

## Build from source

### Requirements

- Windows 11 x64
- Visual Studio 2022 (MSVC x64 toolchain and MSBuild)
- Windows SDK `10.0.26100`
- NuGet CLI
- WiX Toolset v5 (required only for MSI packaging)

### Build and package

```powershell
# 1. Build the x64 app, hardware service, and accessory hosts (outputs to build\settings-x64)
.\scripts\build-settings-x64.ps1

# 2. Package the standard MSI installer (outputs to build\OpenEHubSetup-x64.msi)
.\scripts\pack-msi-x64.ps1
```

---

## Layout

- `Tools/EGoTouchSettings/`: WinUI 3 app (`openehub.exe`), tray flyout and Fluent HUD, hybrid-core frequency binder (`MateBookPowerFreqBinder.h`), hardware service (`MateBookHw.cpp` → `openehubHw.exe` / `OpenEHubHwService`), and event-driven accessory hosts (`openehubPen.exe`, `openehubKeyboard.exe`).
- `scripts/`: x64 build script (`build-settings-x64.ps1`), WiX MSI packaging (`pack-msi-x64.ps1` / `OpenEHubSetup-x64.wxs`), and read-only diagnostics (`Test-MateBookE.ps1`).
- `Common/`: cross-process shared-memory definitions and shared utilities.
- `docs/`: reverse-engineered protocol notes for the keyboard MCU, stylus, and Huawei components.
- `EGoTouchService/`, `hal/`: upstream ARM64 service and HAL components inherited from OpenEGo Hub.

---

## Credits

Derived from **[OpenEGo Hub](https://github.com/NihilDigit/openego-hub)** (for MateBook E Go ARM64, itself derived from **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**, MIT License, © Detach2233); see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for full licence notices.

The following community projects were consulted during development:

- [OpenEGo Hub](https://github.com/NihilDigit/openego-hub) (by NihilDigit)
- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen) (by eiyooooo)
- [goodies](https://github.com/matebook-e-go/goodies) (by dantmnf)
- [EgoTools](https://github.com/SaKongA/EgoTools) (by SaKongA)
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService) (by qwqVictor)

---

## Licence

Released under the [MIT License](LICENSE). Any redistribution in source or binary form must retain the copyright notices in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

---

## Disclaimers

- **Trademarks.** This project has no official affiliation with, or endorsement from, HUAWEI or any other rights holder. Product names, trademarks, and company names belong to their respective owners.
- **Third-party assets.** At runtime the app loads device renders and vendor modules from a locally installed Huawei PC Manager when present, falling back to built-in icons otherwise. Copyrighted vendor artwork is included in neither this repository nor installers.
- **Use at your own risk.** This project interacts with low-level hardware interfaces, WMI/EC controls, power settings, and Windows services for interoperability research and technical exchange. You assume all risk of hardware faults, data loss, or system instability; the authors and contributors accept no liability.
