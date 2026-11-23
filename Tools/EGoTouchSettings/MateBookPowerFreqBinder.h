#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
#include <objbase.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>

#include "PowerOverlay.h"

// Binds Intel hybrid-core frequency ceilings (PROCFREQMAX / PROCFREQMAX1) and
// turbo boost policy (PERFBOOSTMODE) to Windows' three Power Modes on both AC
// and DC while OpenE hub is running, and restores the original Windows values
// automatically on exit.
class MateBookPowerFreqBinder {
public:
    struct Caps {
        DWORD maxFreq1MHz = 0; // P-core (Class 1) ceiling, 0 = unlimited
        DWORD maxFreqMHz = 0;  // E-core (Class 0) ceiling, 0 = unlimited
        DWORD boostMode = 1;   // PERFBOOSTMODE
        DWORD maxThrottle = 100;
        DWORD maxThrottle1 = 100;
    };

    static constexpr Caps CapsForMode(PowerOverlay::Mode mode) noexcept {
        switch (mode) {
        case PowerOverlay::Mode::BestEfficiency:
            return {2000u, 1500u, 1u, 100u, 100u};
        case PowerOverlay::Mode::Balanced:
            return {2800u, 2200u, 1u, 100u, 100u};
        case PowerOverlay::Mode::BestPerformance:
        default:
            return {0u, 0u, 2u, 100u, 100u};
        }
    }

    static std::wstring SettingsHintText(PowerOverlay::Mode mode, bool energySaverActive) {
        if (energySaverActive) {
            return L"节能模式已开启，已绑定频率上限：P 核 2.0 GHz · E 核 1.5 GHz（退出后自动恢复系统默认）。";
        }
        switch (mode) {
        case PowerOverlay::Mode::BestEfficiency:
            return L"已绑定频率上限：P 核 2.0 GHz · E 核 1.5 GHz（插电与电池通用，退出后自动恢复系统默认）。";
        case PowerOverlay::Mode::Balanced:
            return L"已绑定频率上限：P 核 2.8 GHz · E 核 2.2 GHz（插电与电池通用，退出后自动恢复系统默认）。";
        case PowerOverlay::Mode::BestPerformance:
        default:
            return L"当前不限频，并自动联动华为高能模式（退出后自动恢复系统默认）。";
        }
    }

    static std::wstring FlyoutHintText(PowerOverlay::Mode mode, bool energySaverActive) {
        if (energySaverActive) {
            return L"频率绑定：P 核 2.0 GHz · E 核 1.5 GHz（退出后恢复）";
        }
        switch (mode) {
        case PowerOverlay::Mode::BestEfficiency:
            return L"频率绑定：P 核 2.0 GHz · E 核 1.5 GHz（退出后恢复）";
        case PowerOverlay::Mode::Balanced:
            return L"频率绑定：P 核 2.8 GHz · E 核 2.2 GHz（退出后恢复）";
        case PowerOverlay::Mode::BestPerformance:
        default:
            return L"频率绑定：不限频，由系统动态调度（退出后恢复）";
        }
    }

    MateBookPowerFreqBinder() = default;
    MateBookPowerFreqBinder(const MateBookPowerFreqBinder&) = delete;
    MateBookPowerFreqBinder& operator=(const MateBookPowerFreqBinder&) = delete;
    ~MateBookPowerFreqBinder() { StopAndRestore(); }

    static void RestoreStaleSessionIfPresent() {
        if (const auto existing = LoadSessionBackupFile()) {
            RestoreBaselineLocked(*existing);
            DeleteSessionBackupFile();
        }
    }

    void Start(std::function<void()> onModeChanged) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_started) return;
            m_stopping.store(false, std::memory_order_release);
            m_onModeChanged = std::move(onModeChanged);
            m_started = true;
            CaptureOrLoadBaselineLocked();
        }

        // Register for OS-level effective power mode notifications outside m_mutex
        // because Windows invokes the callback immediately during registration.
        const auto& api = PowerOverlay::detail::Api::Instance();
        if (api.module) {
            auto regFn = reinterpret_cast<RegisterEffectiveFn>(
                GetProcAddress(api.module, "PowerRegisterForEffectivePowerModeNotifications"));
            m_unregisterFn = reinterpret_cast<UnregisterEffectiveFn>(
                GetProcAddress(api.module, "PowerUnregisterFromEffectivePowerModeNotifications"));
            if (regFn && m_unregisterFn) {
                PVOID handle = nullptr;
                if (SUCCEEDED(regFn(EFFECTIVE_POWER_MODE_V2, &OnEffectiveModeChanged, this, &handle)) && handle) {
                    m_regHandle = handle;
                }
            }
        }

        SyncNow();
    }

    void SyncNow() {
        SyncInternal(std::nullopt, false);
    }

    void StopAndRestore() {
        m_stopping.store(true, std::memory_order_release);
        PVOID regHandle = m_regHandle;
        m_regHandle = nullptr;
        if (regHandle && m_unregisterFn) {
            (void)m_unregisterFn(regHandle);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        m_started = false;
        m_onModeChanged = nullptr;

        if (m_hasBackup) {
            RestoreBaselineLocked(m_backup);
            DeleteSessionBackupFile();
            m_hasBackup = false;
        }
        m_bound = false;
    }

    bool IsBound() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_bound;
    }

    std::wstring DiagnosticsSummary() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bound) return L"未绑定";
        auto fmtCaps = [](const Caps& c) -> std::wstring {
            if (c.maxFreq1MHz == 0 && c.maxFreqMHz == 0) {
                return L"不限频 (Boost=" + std::to_wstring(c.boostMode) + L")";
            }
            return L"P=" + std::to_wstring(c.maxFreq1MHz) + L"MHz / E=" +
                   std::to_wstring(c.maxFreqMHz) + L"MHz (Boost=" + std::to_wstring(c.boostMode) + L")";
        };
        return L"已绑定 | AC: " + fmtCaps(m_lastAcCaps) + L" | DC: " + fmtCaps(m_lastDcCaps);
    }

private:
    using RegisterEffectiveFn = HRESULT(WINAPI*)(ULONG, EFFECTIVE_POWER_MODE_CALLBACK*, PVOID, PVOID*);
    using UnregisterEffectiveFn = HRESULT(WINAPI*)(PVOID);

    struct SideValues {
        DWORD maxFreq = 0;
        DWORD maxFreq1 = 0;
        DWORD boost = 1;
        DWORD maxThrottle = 100;
        DWORD maxThrottle1 = 100;
    };

    struct SessionBackup {
        GUID scheme{};
        SideValues ac{};
        SideValues dc{};
        std::optional<DWORD> effDcEpp;
        std::optional<DWORD> effDcEpp1;
    };

    static inline constexpr GUID kProcessorSubgroup{
        0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
    static inline constexpr GUID kBoostSetting{
        0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
    static inline constexpr GUID kMaxThrottle{
        0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
    static inline constexpr GUID kMaxThrottle1{
        0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}};
    static inline constexpr GUID kMaxFreq{
        0x75b0ae3f, 0xbce0, 0x45a7, {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00}};
    static inline constexpr GUID kMaxFreq1{
        0x75b0ae3f, 0xbce0, 0x45a7, {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x01}};
    static inline constexpr GUID kPerfEpp{
        0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};
    static inline constexpr GUID kPerfEpp1{
        0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64}};

    static VOID WINAPI OnEffectiveModeChanged(EFFECTIVE_POWER_MODE mode, PVOID context) {
        auto* self = static_cast<MateBookPowerFreqBinder*>(context);
        if (!self || self->m_stopping.load(std::memory_order_acquire)) return;
        self->SyncInternal(mode, true);
    }

    static PowerOverlay::Mode MapEffectiveMode(EFFECTIVE_POWER_MODE mode) noexcept {
        switch (mode) {
        case EffectivePowerModeBatterySaver:
        case EffectivePowerModeBetterBattery:
            return PowerOverlay::Mode::BestEfficiency;
        case EffectivePowerModeHighPerformance:
        case EffectivePowerModeMaxPerformance:
        case EffectivePowerModeGameMode:
        case EffectivePowerModeMixedReality:
            return PowerOverlay::Mode::BestPerformance;
        case EffectivePowerModeBalanced:
        default:
            return PowerOverlay::Mode::Balanced;
        }
    }

    static bool GetActiveScheme(GUID& scheme) noexcept {
        const auto& api = PowerOverlay::detail::Api::Instance();
        GUID* ptr = nullptr;
        if (!api.getActiveScheme || api.getActiveScheme(nullptr, &ptr) != ERROR_SUCCESS || !ptr) {
            return false;
        }
        scheme = *ptr;
        LocalFree(ptr);
        return true;
    }

    static bool ReadSideValues(const GUID& scheme, bool ac, SideValues& out) noexcept {
        const auto& api = PowerOverlay::detail::Api::Instance();
        auto fn = ac ? api.readAcValue : api.readDcValue;
        if (!fn) return false;
        auto rd = [&](const GUID& setting, DWORD& val) {
            return fn(nullptr, &scheme, &kProcessorSubgroup, &setting, &val) == ERROR_SUCCESS;
        };
        return rd(kMaxFreq, out.maxFreq) &&
               rd(kMaxFreq1, out.maxFreq1) &&
               rd(kBoostSetting, out.boost) &&
               rd(kMaxThrottle, out.maxThrottle) &&
               rd(kMaxThrottle1, out.maxThrottle1);
    }

    static bool WriteSideValuesIfChanged(const GUID& scheme, bool ac, const SideValues& desired, bool& changed) noexcept {
        SideValues cur{};
        const bool haveCur = ReadSideValues(scheme, ac, cur);
        if (haveCur &&
            cur.maxFreq == desired.maxFreq &&
            cur.maxFreq1 == desired.maxFreq1 &&
            cur.boost == desired.boost &&
            cur.maxThrottle == desired.maxThrottle &&
            cur.maxThrottle1 == desired.maxThrottle1) {
            return true;
        }
        const auto& api = PowerOverlay::detail::Api::Instance();
        auto fn = ac ? api.writeAcValue : api.writeDcValue;
        if (!fn) return false;
        auto wr = [&](const GUID& setting, DWORD val) {
            return fn(nullptr, &scheme, &kProcessorSubgroup, &setting, val) == ERROR_SUCCESS;
        };
        const bool ok =
            wr(kMaxFreq, desired.maxFreq) &&
            wr(kMaxFreq1, desired.maxFreq1) &&
            wr(kBoostSetting, desired.boost) &&
            wr(kMaxThrottle, desired.maxThrottle) &&
            wr(kMaxThrottle1, desired.maxThrottle1);
        if (ok) changed = true;
        return ok;
    }

    static bool ReadBestEfficiencyDcEpp(DWORD& epp0, DWORD& epp1) noexcept {
        const auto& api = PowerOverlay::detail::Api::Instance();
        if (!api.readDcValue) return false;
        const GUID& eff = PowerOverlay::detail::kBestEfficiencyGuid;
        return api.readDcValue(nullptr, &eff, &kProcessorSubgroup, &kPerfEpp, &epp0) == ERROR_SUCCESS &&
               api.readDcValue(nullptr, &eff, &kProcessorSubgroup, &kPerfEpp1, &epp1) == ERROR_SUCCESS;
    }

    static bool WriteBestEfficiencyDcEppIfChanged(DWORD target0, DWORD target1, bool& changed) noexcept {
        DWORD cur0 = 0, cur1 = 0;
        if (ReadBestEfficiencyDcEpp(cur0, cur1) && cur0 == target0 && cur1 == target1) {
            return true;
        }
        const auto& api = PowerOverlay::detail::Api::Instance();
        if (!api.writeDcValue) return false;
        const GUID& eff = PowerOverlay::detail::kBestEfficiencyGuid;
        const bool ok =
            api.writeDcValue(nullptr, &eff, &kProcessorSubgroup, &kPerfEpp, target0) == ERROR_SUCCESS &&
            api.writeDcValue(nullptr, &eff, &kProcessorSubgroup, &kPerfEpp1, target1) == ERROR_SUCCESS;
        if (ok) changed = true;
        return ok;
    }

    static std::filesystem::path SessionBackupPath() {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) return {};
        std::filesystem::path dir{raw};
        CoTaskMemFree(raw);
        return dir / L"opene-hub" / L"profiles" / L"freq-bind-session.json";
    }

    static std::string GuidToString(const GUID& g) {
        char buf[64]{};
        std::snprintf(
            buf, sizeof(buf),
            "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return buf;
    }

    static bool ParseGuidString(const std::string& text, GUID& out) {
        std::wstring wide(text.begin(), text.end());
        if (!wide.empty() && wide.front() != L'{') wide = L"{" + wide + L"}";
        return SUCCEEDED(CLSIDFromString(wide.c_str(), &out));
    }

    static std::optional<std::string> ExtractJsonString(const std::string& json, const char* key) {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t pos = json.find(needle);
        if (pos == std::string::npos) return std::nullopt;
        const size_t colon = json.find(':', pos + needle.size());
        if (colon == std::string::npos) return std::nullopt;
        const size_t q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return std::nullopt;
        const size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) return std::nullopt;
        return json.substr(q1 + 1, q2 - q1 - 1);
    }

    static std::optional<DWORD> ExtractJsonDword(const std::string& json, const char* key) {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t pos = json.find(needle);
        if (pos == std::string::npos) return std::nullopt;
        const size_t colon = json.find(':', pos + needle.size());
        if (colon == std::string::npos) return std::nullopt;
        size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] < '0' || json[i] > '9') return std::nullopt;
        DWORD val = 0;
        while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
            const auto d = static_cast<DWORD>(json[i] - '0');
            if (val > (MAXDWORD - d) / 10) return std::nullopt;
            val = val * 10 + d;
            ++i;
        }
        return val;
    }

    static std::optional<SessionBackup> LoadSessionBackupFile() {
        const auto path = SessionBackupPath();
        if (path.empty()) return std::nullopt;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto schemeStr = ExtractJsonString(json, "Scheme");
        const auto acFreq = ExtractJsonDword(json, "AcMaxFreq");
        const auto acFreq1 = ExtractJsonDword(json, "AcMaxFreq1");
        const auto acBoost = ExtractJsonDword(json, "AcBoost");
        const auto acThr = ExtractJsonDword(json, "AcMaxThrottle");
        const auto acThr1 = ExtractJsonDword(json, "AcMaxThrottle1");
        const auto dcFreq = ExtractJsonDword(json, "DcMaxFreq");
        const auto dcFreq1 = ExtractJsonDword(json, "DcMaxFreq1");
        const auto dcBoost = ExtractJsonDword(json, "DcBoost");
        const auto dcThr = ExtractJsonDword(json, "DcMaxThrottle");
        const auto dcThr1 = ExtractJsonDword(json, "DcMaxThrottle1");
        if (!schemeStr || !acFreq || !acFreq1 || !acBoost || !acThr || !acThr1 ||
            !dcFreq || !dcFreq1 || !dcBoost || !dcThr || !dcThr1) {
            return std::nullopt;
        }
        SessionBackup b{};
        if (!ParseGuidString(*schemeStr, b.scheme)) return std::nullopt;
        b.ac = {*acFreq, *acFreq1, *acBoost, *acThr, *acThr1};
        b.dc = {*dcFreq, *dcFreq1, *dcBoost, *dcThr, *dcThr1};
        b.effDcEpp = ExtractJsonDword(json, "EffDcEpp");
        b.effDcEpp1 = ExtractJsonDword(json, "EffDcEpp1");
        return b;
    }

    static void SaveSessionBackupFile(const SessionBackup& b) {
        const auto path = SessionBackupPath();
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return;
        const auto tmp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        std::string json =
            "{\"Scheme\":\"" + GuidToString(b.scheme) +
            "\",\"AcMaxFreq\":" + std::to_string(b.ac.maxFreq) +
            ",\"AcMaxFreq1\":" + std::to_string(b.ac.maxFreq1) +
            ",\"AcBoost\":" + std::to_string(b.ac.boost) +
            ",\"AcMaxThrottle\":" + std::to_string(b.ac.maxThrottle) +
            ",\"AcMaxThrottle1\":" + std::to_string(b.ac.maxThrottle1) +
            ",\"DcMaxFreq\":" + std::to_string(b.dc.maxFreq) +
            ",\"DcMaxFreq1\":" + std::to_string(b.dc.maxFreq1) +
            ",\"DcBoost\":" + std::to_string(b.dc.boost) +
            ",\"DcMaxThrottle\":" + std::to_string(b.dc.maxThrottle) +
            ",\"DcMaxThrottle1\":" + std::to_string(b.dc.maxThrottle1);
        if (b.effDcEpp && b.effDcEpp1) {
            json += ",\"EffDcEpp\":" + std::to_string(*b.effDcEpp) +
                    ",\"EffDcEpp1\":" + std::to_string(*b.effDcEpp1);
        }
        json += "}";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(json.data(), static_cast<std::streamsize>(json.size()));
            out.close();
            if (!out) {
                std::filesystem::remove(tmp, ec);
                return;
            }
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(tmp, ec);
        }
    }

    static void DeleteSessionBackupFile() {
        const auto path = SessionBackupPath();
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    void CaptureOrLoadBaselineLocked() {
        GUID scheme{};
        if (!GetActiveScheme(scheme)) return;

        // If an unclean previous exit left a backup for this scheme, preserve that
        // true pre-binding Windows baseline instead of snapshots of our capped values.
        if (const auto existing = LoadSessionBackupFile()) {
            if (PowerOverlay::detail::GuidEquals(existing->scheme, scheme)) {
                m_backup = *existing;
                // If upgrading from a previous build that overrode Best Efficiency DC EPP,
                // restore that baseline immediately and drop the legacy EPP fields from the backup.
                if (m_backup.effDcEpp && m_backup.effDcEpp1) {
                    bool eppChanged = false;
                    (void)WriteBestEfficiencyDcEppIfChanged(*m_backup.effDcEpp, *m_backup.effDcEpp1, eppChanged);
                    m_backup.effDcEpp.reset();
                    m_backup.effDcEpp1.reset();
                    SaveSessionBackupFile(m_backup);
                }
                m_hasBackup = true;
                return;
            }
            RestoreBaselineLocked(*existing);
            DeleteSessionBackupFile();
        }

        SessionBackup fresh{};
        fresh.scheme = scheme;
        if (ReadSideValues(scheme, true, fresh.ac) && ReadSideValues(scheme, false, fresh.dc)) {
            m_backup = fresh;
            m_hasBackup = true;
            SaveSessionBackupFile(fresh);
        }
    }

    static void RestoreBaselineLocked(const SessionBackup& b) noexcept {
        bool changed = false;
        bool eppChanged = false;
        (void)WriteSideValuesIfChanged(b.scheme, true, b.ac, changed);
        (void)WriteSideValuesIfChanged(b.scheme, false, b.dc, changed);
        if (b.effDcEpp && b.effDcEpp1) {
            (void)WriteBestEfficiencyDcEppIfChanged(*b.effDcEpp, *b.effDcEpp1, eppChanged);
        }
        if (changed || eppChanged) {
            GUID active{};
            const auto& api = PowerOverlay::detail::Api::Instance();
            if (GetActiveScheme(active) &&
                PowerOverlay::detail::GuidEquals(active, b.scheme) &&
                api.setActiveScheme) {
                (void)api.setActiveScheme(nullptr, &b.scheme);
            }
            if (eppChanged && api.setActive) {
                SYSTEM_POWER_STATUS sps{};
                const bool onDc = GetSystemPowerStatus(&sps) && sps.ACLineStatus == 0;
                if (onDc && PowerOverlay::Query().mode == PowerOverlay::Mode::BestEfficiency) {
                    (void)api.setActive(&PowerOverlay::detail::kBestEfficiencyGuid);
                }
            }
        }
    }

    void SyncInternal(std::optional<EFFECTIVE_POWER_MODE> effectiveOverride, bool notifyUi) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started || m_stopping.load(std::memory_order_acquire)) return;

            GUID scheme{};
            if (!GetActiveScheme(scheme)) return;
            if (!m_hasBackup) {
                CaptureOrLoadBaselineLocked();
                if (!m_hasBackup) return;
            } else if (!PowerOverlay::detail::GuidEquals(m_backup.scheme, scheme)) {
                RestoreBaselineLocked(m_backup);
                DeleteSessionBackupFile();
                m_hasBackup = false;
                CaptureOrLoadBaselineLocked();
                if (!m_hasBackup) return;
            }

            const auto& api = PowerOverlay::detail::Api::Instance();
            SYSTEM_POWER_STATUS sps{};
            const bool acOnline = GetSystemPowerStatus(&sps) ? (sps.ACLineStatus == 1) : true;
            const bool energySaver = PowerOverlay::detail::IsEnergySaverActiveNow();

            auto queryUserMode = [&](bool ac) -> PowerOverlay::Mode {
                GUID guid{};
                auto fn = ac ? api.getUserAc : api.getUserDc;
                if (fn && fn(&guid) == ERROR_SUCCESS) {
                    return PowerOverlay::detail::ModeFromGuid(guid);
                }
                return PowerOverlay::Query().mode;
            };

            PowerOverlay::Mode acMode = queryUserMode(true);
            PowerOverlay::Mode dcMode = queryUserMode(false);
            if (effectiveOverride) {
                const auto mapped = MapEffectiveMode(*effectiveOverride);
                if (acOnline) acMode = mapped;
                else dcMode = mapped;
            }
            if (energySaver) {
                if (acOnline) acMode = PowerOverlay::Mode::BestEfficiency;
                else dcMode = PowerOverlay::Mode::BestEfficiency;
            }

            const Caps acCaps = CapsForMode(acMode);
            const Caps dcCaps = CapsForMode(dcMode);
            const SideValues acDesired{acCaps.maxFreqMHz, acCaps.maxFreq1MHz, acCaps.boostMode,
                                       acCaps.maxThrottle, acCaps.maxThrottle1};
            const SideValues dcDesired{dcCaps.maxFreqMHz, dcCaps.maxFreq1MHz, dcCaps.boostMode,
                                       dcCaps.maxThrottle, dcCaps.maxThrottle1};

            bool changed = false;
            const bool okAc = WriteSideValuesIfChanged(scheme, true, acDesired, changed);
            const bool okDc = WriteSideValuesIfChanged(scheme, false, dcDesired, changed);
            if (changed && api.setActiveScheme) {
                (void)api.setActiveScheme(nullptr, &scheme);
            }
            if (okAc && okDc) {
                m_bound = true;
                m_lastAcCaps = acCaps;
                m_lastDcCaps = dcCaps;
            }
            if (notifyUi && m_onModeChanged) {
                callback = m_onModeChanged;
            }
        }
        if (callback) callback();
    }

    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopping{false};
    bool m_started = false;
    bool m_hasBackup = false;
    bool m_bound = false;
    SessionBackup m_backup{};
    Caps m_lastAcCaps{};
    Caps m_lastDcCaps{};
    PVOID m_regHandle = nullptr;
    UnregisterEffectiveFn m_unregisterFn = nullptr;
    std::function<void()> m_onModeChanged;
};
