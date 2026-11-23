#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace MateBookHw {

inline constexpr wchar_t kServiceName[] = L"OpenEHubHwService";
inline constexpr wchar_t kServiceDisplayName[] = L"OpenE hub Hardware Service";
inline constexpr wchar_t kGlobalMappingName[] = L"Global\\OpenEHubHwState";
inline constexpr wchar_t kLocalMappingName[] = L"Local\\OpenEHubHwState";
inline constexpr wchar_t kGlobalPipeName[] = L"\\\\.\\pipe\\OpenEHubHwControl";
inline constexpr wchar_t kLocalPipeName[] = L"\\\\.\\pipe\\OpenEHubHwControlLocal";

inline constexpr uint32_t kMagic = 0x4F454857u; // "OEHW"
inline constexpr uint32_t kVersion = 1u;

namespace Flag {
inline constexpr uint32_t kHasFan      = 1u << 0;
inline constexpr uint32_t kHasCpuTemp  = 1u << 1;
inline constexpr uint32_t kHasCharge   = 1u << 2;
inline constexpr uint32_t kChargeManual = 1u << 3;
inline constexpr uint32_t kServiceMode = 1u << 4;
} // namespace Flag

struct SharedState {
    uint32_t magic;
    uint32_t version;
    volatile LONG seq; // odd while writing, even when stable
    uint32_t flags;
    int32_t fanRpm;
    int32_t cpuTempC;
    uint8_t chargeMode;
    uint8_t chargeDelayHours;
    uint8_t chargeStartPercent;
    uint8_t chargeStopPercent;
    uint64_t sampleTickMs;
    uint32_t hostPid;
    uint32_t reserved;
};

struct Snapshot {
    uint32_t flags = 0;
    int32_t fanRpm = 0;
    int32_t cpuTempC = 0;
    uint8_t chargeMode = 0;
    uint8_t chargeDelayHours = 72;
    uint8_t chargeStartPercent = 65;
    uint8_t chargeStopPercent = 70;
    uint64_t sampleTickMs = 0;
    uint32_t hostPid = 0;

    [[nodiscard]] bool HasFan() const noexcept { return (flags & Flag::kHasFan) != 0; }
    [[nodiscard]] bool HasCpuTemp() const noexcept { return (flags & Flag::kHasCpuTemp) != 0; }
    [[nodiscard]] bool HasCharge() const noexcept { return (flags & Flag::kHasCharge) != 0; }
    [[nodiscard]] bool ChargeManual() const noexcept { return (flags & Flag::kChargeManual) != 0; }
    [[nodiscard]] bool IsServiceMode() const noexcept { return (flags & Flag::kServiceMode) != 0; }
};

enum class CommandKind : uint32_t {
    WakeSensors = 1,
    RefreshCharge = 2,
    SetSmartCharge = 3,
    SetChargeLimit = 4,
};

struct CommandRequest {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t kind = 0;
    uint32_t param = 0;
};

struct CommandReply {
    uint32_t ok = 0;
    uint32_t errorCode = 0;
    Snapshot snapshot{};
};

class Client {
public:
    Client() = default;
    ~Client() { Close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void Close() noexcept {
        if (m_view) {
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        m_usingGlobal = false;
    }

    [[nodiscard]] bool ReadSnapshot(Snapshot& out) noexcept {
        if (!EnsureMapped()) return false;
        if (!TryReadView(out)) {
            Close();
            return false;
        }
        // Verify host process is still alive if local session mode; for service mode, check freshness/mapping.
        if (!out.IsServiceMode() && out.hostPid != 0) {
            HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, out.hostPid);
            if (proc) {
                const bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
                CloseHandle(proc);
                if (!alive) {
                    Close();
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsConnected() noexcept {
        Snapshot snap{};
        return ReadSnapshot(snap);
    }

    static bool SendCommand(CommandKind kind, uint32_t param = 0, CommandReply* replyOut = nullptr,
                            DWORD timeoutMs = 3000) noexcept {
        CommandRequest req{};
        req.kind = static_cast<uint32_t>(kind);
        req.param = param;
        CommandReply reply{};
        DWORD readBytes = 0;

        auto tryPipe = [&](const wchar_t* pipeName) -> bool {
            if (!CallNamedPipeW(pipeName, &req, sizeof(req), &reply, sizeof(reply),
                                &readBytes, timeoutMs)) {
                return false;
            }
            if (readBytes != sizeof(reply)) return false;
            if (replyOut) *replyOut = reply;
            return reply.ok != 0;
        };

        if (tryPipe(kGlobalPipeName)) return true;
        return tryPipe(kLocalPipeName);
    }

    // Non-elevated readback from PC Manager's persisted charge record so the UI reflects
    // the current mode immediately even before the hardware service is installed.
    static bool ReadVendorChargeFallback(bool& manual, uint8_t& limitPercent) noexcept {
        auto readStr = [](const wchar_t* name, std::wstring& out) -> bool {
            wchar_t buf[64]{};
            DWORD size = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PCManager\\MBAPowerManager",
                             name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
                return false;
            }
            out.assign(buf);
            return true;
        };
        std::wstring statusStr;
        std::wstring capStr;
        const bool hasStatus = readStr(L"PowerSafeManagerStatus", statusStr);
        const bool hasCap = readStr(L"CustomChargeCapacity", capStr);
        if (!hasStatus && !hasCap) return false;

        const int statusVal = hasStatus ? _wtoi(statusStr.c_str()) : 0;
        manual = (statusVal == 1);
        int cap = hasCap ? _wtoi(capStr.c_str()) : 100;
        if (cap < 50 || cap > 100) cap = 100;
        limitPercent = static_cast<uint8_t>(cap);
        return true;
    }

    static bool IsServiceRunning() noexcept {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return false;
        SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
        bool running = false;
        if (svc) {
            SERVICE_STATUS status{};
            if (QueryServiceStatus(svc, &status)) {
                running = (status.dwCurrentState == SERVICE_RUNNING);
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
        return running;
    }

private:
    bool EnsureMapped() noexcept {
        if (m_view) {
            if (!m_usingGlobal) {
                // Upgrade to Global service mapping if the service came online later.
                HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, kGlobalMappingName);
                if (probe) {
                    Close();
                    m_mapping = probe;
                    m_view = static_cast<const SharedState*>(
                        MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(SharedState)));
                    if (m_view) {
                        m_usingGlobal = true;
                        return true;
                    }
                    Close();
                }
            }
            return true;
        }

        auto tryOpen = [&](const wchar_t* name, bool isGlobal) -> bool {
            HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
            if (!h) return false;
            auto* view = static_cast<const SharedState*>(
                MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(SharedState)));
            if (!view) {
                CloseHandle(h);
                return false;
            }
            m_mapping = h;
            m_view = view;
            m_usingGlobal = isGlobal;
            return true;
        };

        return tryOpen(kGlobalMappingName, true) || tryOpen(kLocalMappingName, false);
    }

    bool TryReadView(Snapshot& out) const noexcept {
        if (!m_view) return false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            // ReadAcquire emits a plain read with acquire barrier — safe on PAGE_READONLY views
            // (unlike InterlockedCompareExchange, which performs a write cycle and faults).
            const LONG s1 = ReadAcquire(&m_view->seq);
            if ((s1 & 1) != 0) {
                YieldProcessor();
                continue;
            }
            if (m_view->magic != kMagic || m_view->version != kVersion) return false;
            Snapshot copy{};
            copy.flags = m_view->flags;
            copy.fanRpm = m_view->fanRpm;
            copy.cpuTempC = m_view->cpuTempC;
            copy.chargeMode = m_view->chargeMode;
            copy.chargeDelayHours = m_view->chargeDelayHours;
            copy.chargeStartPercent = m_view->chargeStartPercent;
            copy.chargeStopPercent = m_view->chargeStopPercent;
            copy.sampleTickMs = m_view->sampleTickMs;
            copy.hostPid = m_view->hostPid;
            MemoryBarrier();
            const LONG s2 = ReadAcquire(&m_view->seq);
            if (s1 == s2) {
                out = copy;
                return true;
            }
        }
        return false;
    }

    HANDLE m_mapping = nullptr;
    const SharedState* m_view = nullptr;
    bool m_usingGlobal = false;
};

} // namespace MateBookHw
