#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "GaokunPower.h"
#include "power/ChargeLimit.h"
#include "power/OemWmi.h"
#include "MateBookHwChannel.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

using namespace MateBookHw;

constexpr wchar_t kVendorChargeKey[] = L"SOFTWARE\\PCManager\\MBAPowerManager";
constexpr wchar_t kVendorStatusName[] = L"PowerSafeManagerStatus";
constexpr wchar_t kVendorCapacityName[] = L"CustomChargeCapacity";
constexpr wchar_t kVendorBackupKey[] = L"SOFTWARE\\openehub\\VendorChargeBackup";
constexpr wchar_t kBackupCapturedName[] = L"Captured";

bool ReadRegString(HKEY root, const wchar_t* subKey, const wchar_t* name, std::wstring& out) noexcept {
    wchar_t buf[64]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return false;
    }
    out.assign(buf);
    return true;
}

bool WriteRegString(HKEY key, const wchar_t* name, const std::wstring& value) noexcept {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

void CaptureVendorChargeBackupOnce() noexcept {
    DWORD captured = 0;
    DWORD size = sizeof(captured);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVendorBackupKey, kBackupCapturedName,
                     RRF_RT_REG_DWORD, nullptr, &captured, &size) == ERROR_SUCCESS &&
        captured != 0) {
        return;
    }
    HKEY backup = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVendorBackupKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &backup,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    std::wstring value;
    if (ReadRegString(HKEY_LOCAL_MACHINE, kVendorChargeKey, kVendorStatusName, value)) {
        (void)WriteRegString(backup, kVendorStatusName, value);
    }
    if (ReadRegString(HKEY_LOCAL_MACHINE, kVendorChargeKey, kVendorCapacityName, value)) {
        (void)WriteRegString(backup, kVendorCapacityName, value);
    }
    const DWORD one = 1;
    (void)RegSetValueExW(backup, kBackupCapturedName, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(backup);
}

void MirrorChargeToVendor(bool manual, uint8_t stopPercent) noexcept {
    HKEY vendor = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVendorChargeKey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &vendor) != ERROR_SUCCESS) {
        return;
    }
    CaptureVendorChargeBackupOnce();
    (void)WriteRegString(vendor, kVendorStatusName, manual ? L"1" : L"0");
    if (manual) {
        (void)WriteRegString(vendor, kVendorCapacityName, std::to_wstring(stopPercent));
    }
    RegCloseKey(vendor);
}

class VendorSensors {
public:
    ~VendorSensors() {
        if (m_dll) FreeLibrary(m_dll);
    }

    void EnsureLoaded() noexcept {
        if (m_attempted) return;
        m_attempted = true;
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return;
        const auto path = std::filesystem::path(folder) / L"Huawei" / L"PCManager" / L"WmiUtil.dll";
        CoTaskMemFree(folder);
        m_dll = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_dll) return;
        m_getFan = reinterpret_cast<ReadSensorFn>(
            GetProcAddress(m_dll, "?GetFanSpeed@BiosWmi@HwSDK@@SAHAEAHW4FanSpeedLocateNumber@2@@Z"));
        m_getTemp = reinterpret_cast<ReadSensorFn>(
            GetProcAddress(m_dll, "?GetSensorTemperature@BiosWmi@HwSDK@@SAHAEAHW4QuerySensorTemperatureIndex@2@@Z"));
    }

    bool ReadFanRpm(int32_t& outRpm) noexcept {
        EnsureLoaded();
        if (m_getFan) {
            int rpm = INT_MAX;
            const int status = m_getFan(rpm, 0);
            if (status == 0 && rpm >= 0 && rpm <= 20000 && rpm != INT_MAX) {
                outRpm = rpm;
                return true;
            }
        }
        // Fallback directly to OemWMIMethod::OemWMIfun (02 08 00).
        const uint8_t req[]{0x02, 0x08, 0x00};
        uint8_t resp[8]{};
        HRESULT hr = S_OK;
        if (Gaokun::Power::Oem::Invoke(req, sizeof(req), resp, sizeof(resp), false, hr) ==
            Gaokun::Power::Result::Ok && resp[0] == 0) {
            const int rpm = static_cast<int>(resp[1]) | (static_cast<int>(resp[2]) << 8);
            if (rpm >= 0 && rpm <= 20000) {
                outRpm = rpm;
                return true;
            }
        }
        return false;
    }

    bool ReadCpuTempC(int32_t& outTempC) noexcept {
        EnsureLoaded();
        auto tryIndex = [&](int idx) -> bool {
            if (m_getTemp) {
                int temp = INT_MAX;
                const int status = m_getTemp(temp, idx);
                if (status == 0 && temp > 0 && temp < 130 && temp != INT_MAX) {
                    outTempC = temp;
                    return true;
                }
            }
            const uint8_t req[]{0x02, 0x02, static_cast<uint8_t>(idx)};
            uint8_t resp[8]{};
            HRESULT hr = S_OK;
            if (Gaokun::Power::Oem::Invoke(req, sizeof(req), resp, sizeof(resp), false, hr) ==
                Gaokun::Power::Result::Ok && resp[0] == 0 && resp[1] == 1) {
                const int temp = static_cast<int>(static_cast<int8_t>(resp[2]));
                if (temp > 0 && temp < 130) {
                    outTempC = temp;
                    return true;
                }
            }
            return false;
        };

        if (m_tempSensorIndex >= 0 && tryIndex(m_tempSensorIndex)) return true;
        for (int idx = 0; idx < 6; ++idx) {
            if (tryIndex(idx)) {
                m_tempSensorIndex = idx;
                return true;
            }
        }
        return false;
    }

private:
    using ReadSensorFn = int(__cdecl*)(int&, int);
    HMODULE m_dll = nullptr;
    ReadSensorFn m_getFan = nullptr;
    ReadSensorFn m_getTemp = nullptr;
    bool m_attempted = false;
    int m_tempSensorIndex = 0;
};

struct SecurityDescriptorHolder {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

    bool Init(const wchar_t* sddl) noexcept {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, nullptr)) {
            return false;
        }
        sa.lpSecurityDescriptor = sd;
        return true;
    }

    ~SecurityDescriptorHolder() {
        if (sd) LocalFree(sd);
    }
};

class HwHost {
public:
    explicit HwHost(bool serviceMode) : m_serviceMode(serviceMode) {
        InitializeCriticalSection(&m_lock);
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~HwHost() {
        Stop();
        if (m_view) UnmapViewOfFile(m_view);
        if (m_mapping) CloseHandle(m_mapping);
        if (m_wakeEvent) CloseHandle(m_wakeEvent);
        if (m_stopEvent) CloseHandle(m_stopEvent);
        DeleteCriticalSection(&m_lock);
    }

    bool Initialize() noexcept {
        SecurityDescriptorHolder mapSec;
        if (!mapSec.Init(L"D:(A;;GR;;;AU)(A;;GA;;;BA)(A;;GA;;;SY)")) return false;

        const wchar_t* mapName = m_serviceMode ? kGlobalMappingName : kLocalMappingName;
        m_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, &mapSec.sa, PAGE_READWRITE, 0, sizeof(SharedState), mapName);
        if (!m_mapping) return false;

        m_view = static_cast<SharedState*>(
            MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedState)));
        if (!m_view) return false;

        ZeroMemory(m_view, sizeof(SharedState));
        m_view->magic = kMagic;
        m_view->version = kVersion;
        m_view->hostPid = GetCurrentProcessId();
        if (m_serviceMode) m_snapshot.flags |= Flag::kServiceMode;
        m_snapshot.hostPid = GetCurrentProcessId();

        // Take an initial reading on startup so data is immediately available.
        SampleChargeLocked();
        SampleSensorsLocked();
        PublishSnapshotLocked();

        m_activeUntilTick = GetTickCount64() + 10000;
        return true;
    }

    void Run(HANDLE parentProcess = nullptr) noexcept {
        m_pipeThread = CreateThread(nullptr, 0, &PipeThreadEntry, this, 0, nullptr);

        HANDLE waits[3]{m_stopEvent, m_wakeEvent, parentProcess};
        const DWORD waitCount = parentProcess ? 3u : 2u;

        while (true) {
            const ULONGLONG now = GetTickCount64();
            const bool active = now < m_activeUntilTick;
            const DWORD timeout = active ? 1500u : INFINITE;
            const DWORD rc = WaitForMultipleObjects(waitCount, waits, FALSE, timeout);
            if (rc == WAIT_OBJECT_0) break;
            if (parentProcess && rc == WAIT_OBJECT_0 + 2) break;

            if (rc == WAIT_OBJECT_0 + 1 || rc == WAIT_TIMEOUT) {
                EnterCriticalSection(&m_lock);
                SampleSensorsLocked();
                PublishSnapshotLocked();
                LeaveCriticalSection(&m_lock);
            }
        }

        Stop();
    }

    void SignalStop() noexcept {
        if (m_stopEvent) SetEvent(m_stopEvent);
    }

    void Stop() noexcept {
        SignalStop();
        if (m_pipeThread) {
            WaitForSingleObject(m_pipeThread, 3000);
            CloseHandle(m_pipeThread);
            m_pipeThread = nullptr;
        }
    }

private:
    static DWORD WINAPI PipeThreadEntry(LPVOID param) noexcept {
        static_cast<HwHost*>(param)->PipeLoop();
        return 0;
    }

    void PipeLoop() noexcept {
        SecurityDescriptorHolder pipeSec;
        if (!pipeSec.Init(L"D:(A;;GRGW;;;AU)(A;;GA;;;BA)(A;;GA;;;SY)")) return;

        const wchar_t* pipeName = m_serviceMode ? kGlobalPipeName : kLocalPipeName;
        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connectEvent) return;

        while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT) {
            HANDLE pipe = CreateNamedPipeW(
                pipeName,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                4, sizeof(CommandReply), sizeof(CommandRequest), 2000, &pipeSec.sa);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(250);
                continue;
            }

            ResetEvent(connectEvent);
            OVERLAPPED ov{};
            ov.hEvent = connectEvent;
            BOOL connected = ConnectNamedPipe(pipe, &ov);
            if (!connected) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE waits[2]{m_stopEvent, connectEvent};
                    const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (rc != WAIT_OBJECT_0 + 1) {
                        CancelIo(pipe);
                        CloseHandle(pipe);
                        break;
                    }
                    DWORD dummy = 0;
                    connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
                } else if (err == ERROR_PIPE_CONNECTED) {
                    connected = TRUE;
                }
            }

            if (connected) {
                HandlePipeClient(pipe);
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
            }
            CloseHandle(pipe);
        }

        CloseHandle(connectEvent);
    }

    void HandlePipeClient(HANDLE pipe) noexcept {
        CommandRequest req{};
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, &req, sizeof(req), &bytesRead, nullptr) ||
            bytesRead != sizeof(req) || req.magic != kMagic || req.version != kVersion) {
            return;
        }

        CommandReply reply{};
        const auto kind = static_cast<CommandKind>(req.kind);
        EnterCriticalSection(&m_lock);

        switch (kind) {
        case CommandKind::WakeSensors: {
            const ULONGLONG now = GetTickCount64();
            const bool stale = (m_snapshot.sampleTickMs == 0) || (now - m_snapshot.sampleTickMs >= 1400);
            m_activeUntilTick = now + 6000;
            if (stale) {
                SampleSensorsLocked();
                PublishSnapshotLocked();
            }
            SetEvent(m_wakeEvent);
            reply.ok = 1;
            break;
        }
        case CommandKind::RefreshCharge: {
            reply.ok = SampleChargeLocked() ? 1u : 0u;
            PublishSnapshotLocked();
            break;
        }
        case CommandKind::SetSmartCharge: {
            HRESULT hr = S_OK;
            const auto res = Gaokun::Power::Detail::SetSmartCharge(false, hr);
            if (res == Gaokun::Power::Result::Ok) {
                MirrorChargeToVendor(false, Gaokun::Power::kSmartChargeStopPercent);
                (void)SampleChargeLocked();
                PublishSnapshotLocked();
                reply.ok = 1;
            } else {
                reply.ok = 0;
                reply.errorCode = static_cast<uint32_t>(hr);
            }
            break;
        }
        case CommandKind::SetChargeLimit: {
            if (req.param < Gaokun::Power::kMinChargeLimit ||
                req.param > Gaokun::Power::kMaxChargeLimit) {
                reply.ok = 0;
                reply.errorCode = ERROR_INVALID_PARAMETER;
                break;
            }
            const int snapped = Gaokun::Power::SnapChargeLimit(static_cast<int>(req.param));
            HRESULT hr = S_OK;
            const auto res = Gaokun::Power::Detail::SetChargeLimit(snapped, false, hr);
            if (res == Gaokun::Power::Result::Ok) {
                MirrorChargeToVendor(true, static_cast<uint8_t>(snapped));
                (void)SampleChargeLocked();
                PublishSnapshotLocked();
                reply.ok = 1;
            } else {
                reply.ok = 0;
                reply.errorCode = static_cast<uint32_t>(hr);
            }
            break;
        }
        default:
            reply.ok = 0;
            reply.errorCode = ERROR_INVALID_FUNCTION;
            break;
        }

        reply.snapshot = m_snapshot;
        LeaveCriticalSection(&m_lock);

        DWORD bytesWritten = 0;
        (void)WriteFile(pipe, &reply, sizeof(reply), &bytesWritten, nullptr);
    }

    bool SampleChargeLocked() noexcept {
        Gaokun::Power::ChargeThreshold threshold{};
        HRESULT hr = S_OK;
        if (Gaokun::Power::Detail::ReadChargeThreshold(threshold, hr) != Gaokun::Power::Result::Ok) {
            return false;
        }
        m_snapshot.flags |= Flag::kHasCharge;
        if (threshold.IsManual()) {
            m_snapshot.flags |= Flag::kChargeManual;
        } else {
            m_snapshot.flags &= ~Flag::kChargeManual;
        }
        m_snapshot.chargeMode = threshold.mode;
        m_snapshot.chargeDelayHours = threshold.delay;
        m_snapshot.chargeStartPercent = threshold.startPercent;
        m_snapshot.chargeStopPercent = threshold.stopPercent;
        return true;
    }

    void SampleSensorsLocked() noexcept {
        int32_t rpm = 0;
        if (m_sensors.ReadFanRpm(rpm)) {
            m_snapshot.fanRpm = rpm;
            m_snapshot.flags |= Flag::kHasFan;
        } else {
            m_snapshot.flags &= ~Flag::kHasFan;
        }

        int32_t tempC = 0;
        if (m_sensors.ReadCpuTempC(tempC)) {
            m_snapshot.cpuTempC = tempC;
            m_snapshot.flags |= Flag::kHasCpuTemp;
        } else {
            m_snapshot.flags &= ~Flag::kHasCpuTemp;
        }
        m_snapshot.sampleTickMs = GetTickCount64();
    }

    void PublishSnapshotLocked() noexcept {
        if (!m_view) return;
        InterlockedIncrement(&m_view->seq); // odd
        MemoryBarrier();
        m_view->flags = m_snapshot.flags;
        m_view->fanRpm = m_snapshot.fanRpm;
        m_view->cpuTempC = m_snapshot.cpuTempC;
        m_view->chargeMode = m_snapshot.chargeMode;
        m_view->chargeDelayHours = m_snapshot.chargeDelayHours;
        m_view->chargeStartPercent = m_snapshot.chargeStartPercent;
        m_view->chargeStopPercent = m_snapshot.chargeStopPercent;
        m_view->sampleTickMs = m_snapshot.sampleTickMs;
        m_view->hostPid = m_snapshot.hostPid;
        MemoryBarrier();
        InterlockedIncrement(&m_view->seq); // even
    }

    bool m_serviceMode = false;
    CRITICAL_SECTION m_lock{};
    HANDLE m_stopEvent = nullptr;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_mapping = nullptr;
    HANDLE m_pipeThread = nullptr;
    SharedState* m_view = nullptr;
    VendorSensors m_sensors;
    Snapshot m_snapshot{};
    ULONGLONG m_activeUntilTick = 0;
};

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_serviceStatus{};
HwHost* g_serviceHost = nullptr;

void ReportServiceState(DWORD state, DWORD win32Exit = NO_ERROR, DWORD waitHint = 0) noexcept {
    if (!g_statusHandle) return;
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = win32Exit;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void WINAPI ServiceCtrlHandler(DWORD control) noexcept {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        if (g_serviceHost) g_serviceHost->SignalStop();
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) noexcept {
    g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, &ServiceCtrlHandler);
    if (!g_statusHandle) return;
    ReportServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);

    HwHost host(true);
    g_serviceHost = &host;
    if (!host.Initialize()) {
        g_serviceHost = nullptr;
        ReportServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    ReportServiceState(SERVICE_RUNNING);
    host.Run();
    g_serviceHost = nullptr;
    ReportServiceState(SERVICE_STOPPED);
}

DWORD WaitServiceState(SC_HANDLE svc, DWORD desired, DWORD timeoutMs) noexcept {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS status{};
    do {
        if (!QueryServiceStatus(svc, &status)) return GetLastError();
        if (status.dwCurrentState == desired) return ERROR_SUCCESS;
        Sleep(150);
    } while (GetTickCount64() < deadline);
    return ERROR_TIMEOUT;
}

int InstallAndStartService() noexcept {
    wchar_t exePath[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (!len || len >= std::size(exePath)) return 1;
    const std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --service";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) return static_cast<int>(GetLastError());

    SC_HANDLE svc = OpenServiceW(
        scm, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        svc = CreateServiceW(
            scm, kServiceName, kServiceDisplayName,
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, cmdLine.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            const DWORD err = GetLastError();
            CloseServiceHandle(scm);
            return static_cast<int>(err);
        }
    } else {
        // Update binary path in case the folder was moved; restart if binary changed.
        (void)ChangeServiceConfigW(
            svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            cmdLine.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kServiceDisplayName);
    }

    SERVICE_DESCRIPTIONW desc{};
    wchar_t descText[] = L"Provides fan speed, CPU temperature and battery charge limit access for OpenE hub.";
    desc.lpDescription = descText;
    (void)ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    SERVICE_STATUS status{};
    DWORD result = ERROR_SUCCESS;
    if (QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(svc, 0, nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) result = err;
        }
        if (result == ERROR_SUCCESS) {
            result = WaitServiceState(svc, SERVICE_RUNNING, 10000);
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return static_cast<int>(result);
}

int UninstallService() noexcept {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return static_cast<int>(GetLastError());
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return err == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : static_cast<int>(err);
    }
    SERVICE_STATUS status{};
    (void)ControlService(svc, SERVICE_CONTROL_STOP, &status);
    (void)WaitServiceState(svc, SERVICE_STOPPED, 10000);
    const DWORD result = DeleteService(svc) ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return static_cast<int>(result);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && _wcsicmp(argv[1], L"--service") == 0) {
        SERVICE_TABLE_ENTRYW table[]{
            {const_cast<LPWSTR>(kServiceName), &ServiceMain},
            {nullptr, nullptr},
        };
        return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--install-service") == 0) {
        return InstallAndStartService();
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--uninstall-service") == 0) {
        return UninstallService();
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--session") == 0) {
        const DWORD parentPid = static_cast<DWORD>(wcstoul(argv[2], nullptr, 10));
        HANDLE parent = parentPid ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
        if (!parent) return 1;
        HwHost host(false);
        if (!host.Initialize()) {
            CloseHandle(parent);
            return 1;
        }
        host.Run(parent);
        CloseHandle(parent);
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--smart") == 0) {
        HRESULT hr = S_OK;
        if (Gaokun::Power::Detail::SetSmartCharge(false, hr) != Gaokun::Power::Result::Ok) {
            return 1;
        }
        MirrorChargeToVendor(false, Gaokun::Power::kSmartChargeStopPercent);
        return 0;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--limit") == 0) {
        const int raw = _wtoi(argv[2]);
        if (raw < Gaokun::Power::kMinChargeLimit || raw > Gaokun::Power::kMaxChargeLimit) return 2;
        const int snapped = Gaokun::Power::SnapChargeLimit(raw);
        HRESULT hr = S_OK;
        if (Gaokun::Power::Detail::SetChargeLimit(snapped, false, hr) != Gaokun::Power::Result::Ok) {
            return 1;
        }
        MirrorChargeToVendor(true, static_cast<uint8_t>(snapped));
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--query") == 0) {
        VendorSensors sensors;
        int32_t rpm = 0, temp = 0;
        const bool hasFan = sensors.ReadFanRpm(rpm);
        const bool hasTemp = sensors.ReadCpuTempC(temp);
        Gaokun::Power::ChargeThreshold th{};
        HRESULT hr = S_OK;
        const bool hasCharge =
            Gaokun::Power::Detail::ReadChargeThreshold(th, hr) == Gaokun::Power::Result::Ok;
        wprintf(L"fan=%ls (%d RPM)\n", hasFan ? L"ok" : L"err", rpm);
        wprintf(L"cpu_temp=%ls (%d C)\n", hasTemp ? L"ok" : L"err", temp);
        wprintf(L"charge=%ls (mode=%u, %u%%..%u%%)\n", hasCharge ? L"ok" : L"err",
                th.mode, th.startPercent, th.stopPercent);
        return (hasFan || hasTemp || hasCharge) ? 0 : 1;
    }
    return 1;
}
