#pragma once
#include "GaokunPen.h"
#include "GaokunKeyboard.h"
#include "GaokunHostExit.h"
#include "PenStatusChannel.h"
#include <filesystem>

// The native hosts use the installed Huawei accessory plug-ins in the interactive session.
class MateBookAccessories {
public:
    void Start(const std::filesystem::path& directory) {
        penHost.Start((directory / L"openehubPen.exe").wstring(), L"");
        keyboardHost.Start((directory / L"openehubKeyboard.exe").wstring(), L"");
    }

    bool SetDetachSupport(bool enabled) {
        Gaokun::Keyboard::CommandWriter writer;
        return keyboardHost.IsRunning() && writer.Open() && writer.SetDetachSupport(enabled);
    }

    void Read(PenStatus::State& out) {
        Gaokun::Pen::Snapshot pen{};
        Gaokun::Keyboard::Snapshot kbd{};
        const bool penRunning = penHost.IsRunning();
        const bool keyboardRunning = keyboardHost.IsRunning();
        if (penRunning && !penOpen) penOpen = penReader.Open();
        if (keyboardRunning && !keyboardOpen) keyboardOpen = keyboardReader.Open();
        const bool penRead = penRunning && penOpen && penReader.Read(pen);
        const bool kbdRead = keyboardRunning && keyboardOpen && keyboardReader.Read(kbd);
        out.hasHostHealth = true;
        out.penHostHealthy = penRead && pen.flags != 0;
        out.kbdHostHealthy = kbdRead && kbd.flags != 0;
        out.penVendorMissing = penHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
        out.kbdVendorMissing = keyboardHost.ExitCode() == Gaokun::kHostExitVendorComponentsMissing;
        if (penRead) {
            using F = Gaokun::Pen::Flag;
            const auto has = [&](F f) { return (pen.flags & static_cast<uint32_t>(f)) != 0; };
            out.hasBatteryLevel = has(F::HasBattery);
            out.batteryLevel = pen.battery;
            out.hasChargingState = has(F::HasCharging);
            out.charging = has(F::Charging);
            out.hasStylusLink = has(F::HasConnected);
            out.stylusLinked = has(F::Connected);
            out.hasDeviceAttached = has(F::HasConnected);
            out.deviceAttached = has(F::Connected);
            out.modelId = pen.moduleId;
            out.hasPenFirmware = has(F::HasFirmware);
            strncpy_s(out.penFirmware, pen.firmware, _TRUNCATE);
            out.hasPenHardware = has(F::HasHardware);
            strncpy_s(out.penHardware, pen.hardware, _TRUNCATE);

            out.hasPenSerial = has(F::HasSerial);
            strncpy_s(out.penSerial, pen.serial, _TRUNCATE);
            // 产品名由 hal 按模组 ID 查表填好，这里直接转发。本仓库不再自带型号表：先前那份
            // 把 282 当成 CD52，因而把 M-Pen 2 显示为「第一代 M-Pencil」。
            strncpy_s(out.modelName, pen.modelName, _TRUNCATE);
        }

        if (kbdRead) {
            using F = Gaokun::Keyboard::Flag;
            const auto has = [&](F f) { return (kbd.flags & static_cast<uint32_t>(f)) != 0; };
            out.hasKbdPresent = has(F::HasConnected);
            out.kbdPresent = has(F::Connected);
            out.hasKbdDetached = has(F::HasDetached);
            out.kbdDetached = has(F::Detached);
            out.hasKbdBattery = has(F::HasBattery);
            out.kbdBatteryLevel = kbd.battery;
            out.hasKbdCharging = has(F::HasCharging);
            out.kbdCharging = has(F::Charging);
            out.hasKbdDetachSupport = has(F::HasDetachSupport);
            out.kbdDetachSupport = has(F::DetachSupport);
            strncpy_s(out.kbdFirmware, kbd.firmware, _TRUNCATE);
            strncpy_s(out.kbdModelName, kbd.modelName, _TRUNCATE);
        }
    }

private:
    Gaokun::Pen::HostController penHost;
    Gaokun::Keyboard::HostController keyboardHost;
    Gaokun::Pen::SnapshotReader penReader;
    Gaokun::Keyboard::SnapshotReader keyboardReader;
    bool penOpen = false;
    bool keyboardOpen = false;
};
