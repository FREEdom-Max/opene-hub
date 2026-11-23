#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include "MateBookServices.h"

#pragma comment(lib, "advapi32.lib")

namespace {

struct ServiceHandle {
    SC_HANDLE handle = nullptr;
    ~ServiceHandle() { if (handle) CloseServiceHandle(handle); }
};

struct SavedState {
    DWORD startType;
    DWORD running;
    DWORD delayed;
};

// Mirror upstream EGoTouchService/source/VendorServices.cpp:
// Terminate PC Manager / HiView user-mode watchdog processes first so they cannot
// re-enable or restart HiConnectivityService / HwDistributedMainService mid-disable.
// HwLcdEnhancement and HW_OSDServer are outside these two directories and remain untouched.
void TerminateVendorProcesses() noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return;
    }

    do {
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
        if (!process) continue;

        wchar_t path[MAX_PATH]{};
        DWORD length = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &length)) {
            std::wstring lowered(path, length);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (lowered.find(L"\\huawei\\pcmanager\\") != std::wstring::npos ||
                lowered.find(L"\\huawei\\hiview\\") != std::wstring::npos) {
                (void)TerminateProcess(process, 0);
            }
        }
        CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
}

bool QueryServiceProcessStatus(SC_HANDLE svc, SERVICE_STATUS_PROCESS& out) noexcept {
    DWORD needed = 0;
    return QueryServiceStatusEx(
               svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&out), sizeof(out), &needed) != FALSE;
}

bool QueryStartTypeAndDelayed(SC_HANDLE svc, DWORD& startType, DWORD& delayed) noexcept {
    DWORD needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (!needed) return false;
    std::vector<BYTE> buffer(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(svc, cfg, needed, &needed)) return false;
    startType = cfg->dwStartType;
    delayed = 0;
    if (startType == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO info{};
        if (QueryServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                 reinterpret_cast<BYTE*>(&info), sizeof(info), &needed)) {
            delayed = info.fDelayedAutostart ? 1u : 0u;
        }
    }
    return true;
}

DWORD ReadBackup(HKEY backups, const wchar_t* name, SavedState& saved, bool& missing) noexcept {
    DWORD size = sizeof(saved);
    const LONG rc = RegGetValueW(backups, nullptr, name, RRF_RT_REG_BINARY, nullptr, &saved, &size);
    missing = (rc == ERROR_FILE_NOT_FOUND);
    if (missing) return ERROR_SUCCESS;
    if (rc != ERROR_SUCCESS) return static_cast<DWORD>(rc);
    if (size != sizeof(saved) || saved.startType < SERVICE_AUTO_START ||
        saved.startType > SERVICE_DISABLED || saved.running > 1 || saved.delayed > 1) {
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

// Phase 1 (Disable): Back up current config (once) and switch all services to SERVICE_DISABLED
// before stopping any of them, preventing mutual restart races.
DWORD DisableConfigureAll(HKEY backups, SC_HANDLE scm) noexcept {
    DWORD firstError = ERROR_SUCCESS;
    for (const wchar_t* name : MateBookServices::names) {
        ServiceHandle svc{OpenServiceW(
            scm, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG)};
        if (!svc.handle) {
            const DWORD err = GetLastError();
            if (err != ERROR_SERVICE_DOES_NOT_EXIST && firstError == ERROR_SUCCESS) firstError = err;
            continue;
        }

        DWORD startType = 0;
        DWORD delayed = 0;
        if (!QueryStartTypeAndDelayed(svc.handle, startType, delayed)) {
            const DWORD err = GetLastError();
            if (firstError == ERROR_SUCCESS) firstError = err;
            continue;
        }

        if (startType != SERVICE_DISABLED) {
            SavedState existing{};
            bool missing = false;
            const DWORD backupRc = ReadBackup(backups, name, existing, missing);
            if (backupRc == ERROR_SUCCESS && missing) {
                SERVICE_STATUS_PROCESS st{};
                const bool running = QueryServiceProcessStatus(svc.handle, st) &&
                                     st.dwCurrentState != SERVICE_STOPPED &&
                                     st.dwCurrentState != SERVICE_STOP_PENDING;
                const SavedState toSave{startType, running ? 1u : 0u, delayed};
                const LONG writeRc = RegSetValueExW(
                    backups, name, 0, REG_BINARY,
                    reinterpret_cast<const BYTE*>(&toSave), sizeof(toSave));
                if (writeRc != ERROR_SUCCESS && firstError == ERROR_SUCCESS) {
                    firstError = static_cast<DWORD>(writeRc);
                    continue;
                }
            }

            if (!ChangeServiceConfigW(svc.handle, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                                      SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr)) {
                const DWORD err = GetLastError();
                if (firstError == ERROR_SUCCESS) firstError = err;
            }
        }
    }
    return firstError;
}

// Phase 2 (Disable): Issue SERVICE_CONTROL_STOP to all active services concurrently, wait up to
// 10 s for them to stop, then forcefully terminate any wedged service process (e.g.
// HwDistributedMainService stuck in STOP_PENDING) and wait briefly for SCM to observe STOPPED.
void StopAllAndCleanUp(SC_HANDLE scm) noexcept {
    std::vector<SC_HANDLE> handles;
    handles.reserve(_countof(MateBookServices::names));

    for (const wchar_t* name : MateBookServices::names) {
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!svc) continue;
        SERVICE_STATUS_PROCESS st{};
        if (QueryServiceProcessStatus(svc, st) && st.dwCurrentState != SERVICE_STOPPED) {
            if (st.dwCurrentState != SERVICE_STOP_PENDING) {
                SERVICE_STATUS ignored{};
                (void)ControlService(svc, SERVICE_CONTROL_STOP, &ignored);
            }
            handles.push_back(svc);
        } else {
            CloseServiceHandle(svc);
        }
    }

    auto waitUntilStopped = [&](DWORD timeoutMs) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!handles.empty() && GetTickCount64() < deadline) {
            bool anyAlive = false;
            for (size_t i = 0; i < handles.size();) {
                SERVICE_STATUS_PROCESS st{};
                if (!QueryServiceProcessStatus(handles[i], st) ||
                    st.dwCurrentState == SERVICE_STOPPED) {
                    CloseServiceHandle(handles[i]);
                    handles.erase(handles.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    anyAlive = true;
                    ++i;
                }
            }
            if (!anyAlive) break;
            Sleep(150);
        }
    };

    waitUntilStopped(10000);

    // If any service is still stuck in STOP_PENDING / RUNNING after 10 s, terminate its host PID.
    if (!handles.empty()) {
        for (SC_HANDLE svc : handles) {
            SERVICE_STATUS_PROCESS st{};
            if (QueryServiceProcessStatus(svc, st) && st.dwCurrentState != SERVICE_STOPPED &&
                st.dwProcessId != 0) {
                if (HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, st.dwProcessId)) {
                    (void)TerminateProcess(proc, 0);
                    CloseHandle(proc);
                }
            }
        }
        waitUntilStopped(3000);
    }

    for (SC_HANDLE svc : handles) {
        CloseServiceHandle(svc);
    }
}

// Phase 3 (Disable): Verify no service was flipped back to AUTO_START during shutdown.
DWORD DisableReverifyAll(SC_HANDLE scm) noexcept {
    DWORD firstError = ERROR_SUCCESS;
    bool needSecondStop = false;
    for (const wchar_t* name : MateBookServices::names) {
        ServiceHandle svc{OpenServiceW(scm, name, SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG)};
        if (!svc.handle) continue;
        DWORD startType = 0;
        DWORD delayed = 0;
        if (!QueryStartTypeAndDelayed(svc.handle, startType, delayed)) continue;
        if (startType == SERVICE_DISABLED) continue;

        if (ChangeServiceConfigW(svc.handle, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr)) {
            needSecondStop = true;
        } else if (firstError == ERROR_SUCCESS) {
            firstError = GetLastError();
        }
    }
    if (needSecondStop) StopAllAndCleanUp(scm);
    return firstError;
}

DWORD RestoreAll(HKEY backups, SC_HANDLE scm) noexcept {
    DWORD firstError = ERROR_SUCCESS;
    struct RestoreItem {
        const wchar_t* name = nullptr;
        bool shouldStart = false;
        bool hasBackup = false;
        bool configured = false;
    };
    std::vector<RestoreItem> items;
    items.reserve(_countof(MateBookServices::names));

    // Phase 1: Restore start type for all services first.
    for (const wchar_t* name : MateBookServices::names) {
        SavedState saved{};
        bool missing = false;
        const DWORD rc = ReadBackup(backups, name, saved, missing);
        if (rc != ERROR_SUCCESS) {
            if (firstError == ERROR_SUCCESS) firstError = rc;
            continue;
        }

        ServiceHandle svc{OpenServiceW(
            scm, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG)};
        if (!svc.handle) {
            const DWORD err = GetLastError();
            if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
                if (!missing) (void)RegDeleteValueW(backups, name);
            } else if (firstError == ERROR_SUCCESS) {
                firstError = err;
            }
            continue;
        }

        DWORD currentStart = 0;
        DWORD currentDelayed = 0;
        if (!QueryStartTypeAndDelayed(svc.handle, currentStart, currentDelayed)) {
            if (firstError == ERROR_SUCCESS) firstError = GetLastError();
            continue;
        }

        if (missing && currentStart != SERVICE_DISABLED) {
            continue;
        }

        const DWORD targetStart = missing ? SERVICE_AUTO_START : saved.startType;
        const DWORD targetDelayed = missing ? 0u : saved.delayed;
        const bool shouldStart = missing ? (targetStart == SERVICE_AUTO_START) : (saved.running != 0);

        if (!ChangeServiceConfigW(svc.handle, SERVICE_NO_CHANGE, targetStart,
                                  SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr)) {
            const DWORD err = GetLastError();
            if (firstError == ERROR_SUCCESS) firstError = err;
            continue;
        }
        if (targetStart == SERVICE_AUTO_START) {
            SERVICE_DELAYED_AUTO_START_INFO info{static_cast<BOOL>(targetDelayed)};
            (void)ChangeServiceConfig2W(svc.handle, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info);
        }

        items.push_back(RestoreItem{name, shouldStart, !missing, true});
    }

    // Phase 2: Start services in reverse order (core services first) and wait briefly.
    std::vector<SC_HANDLE> starting;
    for (size_t i = items.size(); i-- > 0;) {
        const auto& item = items[i];
        if (!item.configured) continue;
        if (item.shouldStart) {
            SC_HANDLE svc = OpenServiceW(scm, item.name, SERVICE_START | SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS_PROCESS st{};
                if (QueryServiceProcessStatus(svc, st) && st.dwCurrentState == SERVICE_STOPPED) {
                    if (StartServiceW(svc, 0, nullptr) ||
                        GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                        starting.push_back(svc);
                    } else {
                        CloseServiceHandle(svc);
                    }
                } else {
                    CloseServiceHandle(svc);
                }
            }
        }
        if (item.hasBackup) {
            (void)RegDeleteValueW(backups, item.name);
        }
    }

    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (!starting.empty() && GetTickCount64() < deadline) {
        bool anyPending = false;
        for (size_t i = 0; i < starting.size();) {
            SERVICE_STATUS_PROCESS st{};
            if (!QueryServiceProcessStatus(starting[i], st) ||
                st.dwCurrentState == SERVICE_RUNNING ||
                st.dwCurrentState == SERVICE_STOPPED) {
                CloseServiceHandle(starting[i]);
                starting.erase(starting.begin() + static_cast<ptrdiff_t>(i));
            } else {
                anyPending = true;
                ++i;
            }
        }
        if (!anyPending) break;
        Sleep(150);
    }
    for (SC_HANDLE svc : starting) {
        CloseServiceHandle(svc);
    }

    return firstError;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || (wcscmp(argv[1], L"disable") && wcscmp(argv[1], L"restore"))) {
        return ERROR_INVALID_PARAMETER;
    }
    const bool disable = wcscmp(argv[1], L"disable") == 0;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\openehub.ServiceChange");
    if (!mutex) return static_cast<int>(GetLastError());
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return ERROR_BUSY;
    }

    ServiceHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.handle) {
        const DWORD err = GetLastError();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return static_cast<int>(err);
    }

    HKEY backups = nullptr;
    const LONG rc = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, MateBookServices::backupKey, 0, nullptr, 0,
        KEY_READ | KEY_SET_VALUE, nullptr, &backups, nullptr);
    if (rc != ERROR_SUCCESS) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return static_cast<int>(rc);
    }

    DWORD result = ERROR_SUCCESS;
    if (disable) {
        TerminateVendorProcesses();
        result = DisableConfigureAll(backups, scm.handle);
        StopAllAndCleanUp(scm.handle);
        const DWORD verifyErr = DisableReverifyAll(scm.handle);
        if (result == ERROR_SUCCESS) result = verifyErr;
    } else {
        result = RestoreAll(backups, scm.handle);
    }

    RegCloseKey(backups);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(result);
}
