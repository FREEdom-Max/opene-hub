#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "PowerOverlay.h"

namespace MateBookBatteryProfile {

enum class Profile : uint8_t {
    Custom = 0,
    Quiet = 1,       // Boost = 1, BestEfficiency
    Daily = 2,       // Boost = 1, Balanced
    Performance = 3, // Boost = 2, BestPerformance
};

struct Status {
    bool supported = false;
    Profile profile = Profile::Custom;
    DWORD boostIndex = 0;
    bool hasBackup = false;
};

namespace detail {

inline constexpr GUID kProcessorSubgroup{
    0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
inline constexpr GUID kBoostSetting{
    0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
inline constexpr GUID kMaxPerformance{
    0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
inline constexpr GUID kMaxPerformance1{
    0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}};
inline constexpr DWORD kFullProcessorState = 100;

struct Preset {
    Profile profile;
    DWORD boost;
    GUID mode;
};

inline constexpr Preset kPresets[] = {
    {Profile::Quiet, 1, PowerOverlay::detail::kBestEfficiencyGuid},
    {Profile::Daily, 1, PowerOverlay::detail::kBalancedGuid},
    {Profile::Performance, 2, PowerOverlay::detail::kBestPerformanceGuid},
};

inline bool SameMode(const GUID& a, const GUID& b) noexcept {
    using PowerOverlay::detail::GuidEquals;
    if (GuidEquals(a, b)) return true;
    const auto balanced = [](const GUID& mode) {
        return GuidEquals(mode, PowerOverlay::detail::kBalancedGuid) ||
               GuidEquals(mode, PowerOverlay::detail::kBalancedLegacyGuid);
    };
    return balanced(a) && balanced(b);
}

struct ProfileLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Global\\OpenEHub-BatteryBoost");
    bool locked = false;
    ProfileLock() {
        const auto result = handle ? WaitForSingleObject(handle, 0) : WAIT_FAILED;
        locked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ProfileLock(const ProfileLock&) = delete;
    ~ProfileLock() {
        if (locked) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
};

inline std::filesystem::path ProfileDir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) return {};
    std::filesystem::path dir{raw};
    CoTaskMemFree(raw);
    return dir / L"opene-hub" / L"profiles";
}

inline std::filesystem::path BackupFilePath() {
    const auto dir = ProfileDir();
    return dir.empty() ? std::filesystem::path{} : (dir / L"battery-profile.json");
}

inline std::string GuidToString(const GUID& g) {
    char buf[64]{};
    std::snprintf(
        buf, sizeof(buf),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

inline bool ParseGuidString(const std::string& text, GUID& out) {
    std::wstring wide(text.begin(), text.end());
    if (!wide.empty() && wide.front() != L'{') {
        wide = L"{" + wide + L"}";
    }
    return SUCCEEDED(CLSIDFromString(wide.c_str(), &out));
}

struct BackupData {
    GUID scheme{};
    DWORD boost = 0;
    GUID mode{};
    // Older backup files contain only Scheme, Boost and Mode.
    std::optional<DWORD> maxPerformance;
    std::optional<DWORD> maxPerformance1;
};

inline std::optional<std::string> ExtractJsonString(const std::string& json, const char* key) {
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

inline std::optional<DWORD> ExtractJsonDword(const std::string& json, const char* key) {
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
        const auto digit = static_cast<DWORD>(json[i] - '0');
        if (val > (MAXDWORD - digit) / 10) return std::nullopt;
        val = val * 10 + digit;
        ++i;
    }
    return val;
}

inline std::optional<BackupData> LoadBackup() {
    const auto path = BackupFilePath();
    if (path.empty()) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto schemeStr = ExtractJsonString(content, "Scheme");
    const auto modeStr = ExtractJsonString(content, "Mode");
    const auto boostVal = ExtractJsonDword(content, "Boost");
    if (!schemeStr || !modeStr || !boostVal || *boostVal > 6) return std::nullopt;

    BackupData data{};
    if (!ParseGuidString(*schemeStr, data.scheme) || !ParseGuidString(*modeStr, data.mode)) {
        return std::nullopt;
    }
    data.boost = *boostVal;
    data.maxPerformance = ExtractJsonDword(content, "MaxPerformance");
    data.maxPerformance1 = ExtractJsonDword(content, "MaxPerformance1");
    if ((content.find("\"MaxPerformance\"") != std::string::npos &&
         (!data.maxPerformance || *data.maxPerformance > 100)) ||
        (content.find("\"MaxPerformance1\"") != std::string::npos &&
         (!data.maxPerformance1 || *data.maxPerformance1 > 100)) ||
        data.maxPerformance.has_value() != data.maxPerformance1.has_value()) return std::nullopt;
    return data;
}

inline bool SaveBackup(const BackupData& data) {
    const auto dir = ProfileDir();
    if (dir.empty() || !data.maxPerformance || !data.maxPerformance1) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    const auto path = dir / L"battery-profile.json";
    const auto tmp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const std::string json =
        "{\"Scheme\":\"" + GuidToString(data.scheme) +
        "\",\"Boost\":" + std::to_string(data.boost) +
        ",\"Mode\":\"" + GuidToString(data.mode) +
        "\",\"MaxPerformance\":" + std::to_string(*data.maxPerformance) +
        ",\"MaxPerformance1\":" + std::to_string(*data.maxPerformance1) + "}";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(tmp, ec);
    return false;
}

inline bool DeleteBackupFiles(const GUID& scheme) {
    std::error_code ec;
    const auto path = BackupFilePath();
    const auto dir = ProfileDir();
    if (!dir.empty()) {
        const auto schemeStr = GuidToString(scheme);
        std::wstring wide(schemeStr.begin(), schemeStr.end());
        std::filesystem::remove(dir / (wide + L".txt"), ec);
        if (ec) return false;
    }
    return !path.empty() && std::filesystem::remove(path, ec) && !ec;
}

inline bool GetActiveScheme(GUID& scheme) noexcept {
    const auto& api = PowerOverlay::detail::Api::Instance();
    GUID* ptr = nullptr;
    if (!api.getActiveScheme || api.getActiveScheme(nullptr, &ptr) != ERROR_SUCCESS || !ptr) return false;
    scheme = *ptr;
    LocalFree(ptr);
    return true;
}

inline bool ReadBatteryPolicy(const GUID& scheme, BackupData& state, bool readMax = true) noexcept {
    const auto& api = PowerOverlay::detail::Api::Instance();
    if (!api.readDcValue || !api.getUserDc) return false;
    const auto read = [&](const GUID& setting, DWORD& value) {
        return api.readDcValue(nullptr, &scheme, &kProcessorSubgroup, &setting, &value) == ERROR_SUCCESS;
    };
    DWORD max0 = 0, max1 = 0;
    state = {scheme};
    if (!read(kBoostSetting, state.boost) || api.getUserDc(&state.mode) != ERROR_SUCCESS) return false;
    if (readMax) {
        if (!read(kMaxPerformance, max0) || !read(kMaxPerformance1, max1)) return false;
        state.maxPerformance = max0;
        state.maxPerformance1 = max1;
    }
    return true;
}

inline bool WriteBatteryPolicy(const BackupData& desired) noexcept {
    const auto& api = PowerOverlay::detail::Api::Instance();
    if (!api.setActiveScheme || !api.writeDcValue || !api.setUserDc) return false;

    const auto write = [&](const GUID& setting, DWORD value) {
        return api.writeDcValue(nullptr, &desired.scheme, &kProcessorSubgroup, &setting, value) == ERROR_SUCCESS;
    };
    if (!write(kBoostSetting, desired.boost) ||
        (desired.maxPerformance && !write(kMaxPerformance, *desired.maxPerformance)) ||
        (desired.maxPerformance1 && !write(kMaxPerformance1, *desired.maxPerformance1))) return false;

    GUID active{};
    if (!GetActiveScheme(active)) return false;
    if (PowerOverlay::detail::GuidEquals(active, desired.scheme) &&
        api.setActiveScheme(nullptr, &desired.scheme) != ERROR_SUCCESS) return false;

    if (api.setUserDc(&desired.mode) != ERROR_SUCCESS) return false;
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power) && power.ACLineStatus == 0 && api.setActive) {
        (void)api.setActive(&desired.mode);
    }
    BackupData actual{};
    return ReadBatteryPolicy(desired.scheme, actual, desired.maxPerformance.has_value()) &&
           actual.boost == desired.boost && SameMode(actual.mode, desired.mode) &&
           actual.maxPerformance == desired.maxPerformance &&
           actual.maxPerformance1 == desired.maxPerformance1;
}

} // namespace detail

inline Status Query() noexcept {
    Status status{};
    try {
        GUID scheme{};
        detail::BackupData current{};
        if (!detail::GetActiveScheme(scheme) || !detail::ReadBatteryPolicy(scheme, current)) return status;
        status.supported = true;
        status.boostIndex = current.boost;
        status.hasBackup = detail::LoadBackup().has_value();
        if (current.maxPerformance == detail::kFullProcessorState &&
            current.maxPerformance1 == detail::kFullProcessorState) {
            for (const auto& preset : detail::kPresets) {
                if (current.boost == preset.boost && detail::SameMode(current.mode, preset.mode)) {
                    status.profile = preset.profile;
                    break;
                }
            }
        }
    } catch (...) {
        return {};
    }
    return status;
}

inline bool Apply(Profile target) noexcept {
    try {
        const auto preset = std::find_if(std::begin(detail::kPresets), std::end(detail::kPresets),
                                         [target](const auto& entry) { return entry.profile == target; });
        if (preset == std::end(detail::kPresets)) return false;
        const detail::ProfileLock lock;
        if (!lock.locked) return false;
        GUID scheme{};
        detail::BackupData current{};
        if (!detail::GetActiveScheme(scheme) || !detail::ReadBatteryPolicy(scheme, current)) return false;

        auto backup = detail::LoadBackup();
        if (backup && !PowerOverlay::detail::GuidEquals(backup->scheme, scheme)) return false;
        if (!backup) {
            const auto path = detail::BackupFilePath();
            if (path.empty() || std::filesystem::exists(path)) return false;
            if (!detail::SaveBackup(current)) return false;
        } else if (!backup->maxPerformance) {
            // Preserve the old boost and mode baseline when first managing the two DC caps.
            backup->maxPerformance = current.maxPerformance;
            backup->maxPerformance1 = current.maxPerformance1;
            if (!detail::SaveBackup(*backup)) return false;
        }

        const detail::BackupData desired{
            scheme, preset->boost, preset->mode, detail::kFullProcessorState, detail::kFullProcessorState};
        if (detail::WriteBatteryPolicy(desired)) return true;
        (void)detail::WriteBatteryPolicy(current);
    } catch (...) {}
    return false;
}

inline bool Restore() noexcept {
    try {
        const detail::ProfileLock lock;
        if (!lock.locked) return false;
        const auto backup = detail::LoadBackup();
        if (!backup) return false;
        detail::BackupData current{};
        if (!detail::ReadBatteryPolicy(backup->scheme, current, backup->maxPerformance.has_value())) return false;
        if (detail::WriteBatteryPolicy(*backup)) return detail::DeleteBackupFiles(backup->scheme);
        (void)detail::WriteBatteryPolicy(current);
    } catch (...) {}
    return false;
}

} // namespace MateBookBatteryProfile
