#define NOMINMAX
#include <windows.h>
#include <cwchar>
#include <vector>
#include "MateBookServices.h"

namespace {
struct Service {
    SC_HANDLE handle;
    ~Service() { if (handle) CloseServiceHandle(handle); }
};

DWORD WaitForState(SC_HANDLE svc, DWORD desired) {
    const auto deadline = GetTickCount64() + 15000;
    SERVICE_STATUS status{};
    do {
        if (!QueryServiceStatus(svc, &status)) return GetLastError();
        if (status.dwCurrentState == desired) return ERROR_SUCCESS;
        Sleep(200);
    } while (GetTickCount64() < deadline);
    return ERROR_TIMEOUT;
}

struct SavedState {
    DWORD startType;
    DWORD running;
    DWORD delayed;
};

DWORD Change(HKEY backups, SC_HANDLE scm, const wchar_t* name, bool disable, bool startPhase) {
    SavedState saved{};
    DWORD size = sizeof(saved);
    LONG rc = RegGetValueW(backups, nullptr, name, RRF_RT_REG_BINARY, nullptr, &saved, &size);
    const bool missing = rc == ERROR_FILE_NOT_FOUND;
    if (rc != ERROR_SUCCESS && !missing) return rc;
    if (missing && !disable) return ERROR_SUCCESS;
    if (!missing && (size != sizeof(saved) || saved.startType < SERVICE_AUTO_START ||
                     saved.startType > SERVICE_DISABLED || saved.running > 1 || saved.delayed > 1))
        return ERROR_INVALID_DATA;

    const DWORD access = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG |
                         (disable ? SERVICE_STOP : SERVICE_START);
    Service svc{OpenServiceW(scm, name, access)};
    if (!svc.handle) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) return error;
        return missing || disable ? ERROR_SUCCESS : RegDeleteValueW(backups, name);
    }
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(svc.handle, &status)) return GetLastError();
    if (disable && missing) {
        DWORD needed{};
        QueryServiceConfigW(svc.handle, nullptr, 0, &needed);
        if (!needed) return GetLastError();
        std::vector<BYTE> buffer(needed);
        const auto cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (!QueryServiceConfigW(svc.handle, cfg, needed, &needed)) return GetLastError();
        if (cfg->dwStartType == SERVICE_DISABLED) return ERROR_SUCCESS;
        if (cfg->dwStartType < SERVICE_AUTO_START || cfg->dwStartType > SERVICE_DISABLED) return ERROR_INVALID_DATA;
        SERVICE_DELAYED_AUTO_START_INFO delayed{};
        if (cfg->dwStartType == SERVICE_AUTO_START &&
            !QueryServiceConfig2W(svc.handle, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                  reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &needed)) return GetLastError();
        saved = {cfg->dwStartType, status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING ? 1u : 0u,
                 delayed.fDelayedAutostart ? 1u : 0u};
        rc = RegSetValueExW(backups, name, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&saved), sizeof(saved));
        if (rc != ERROR_SUCCESS) return rc;
    }
    if (!startPhase) {
        if (!ChangeServiceConfigW(svc.handle, SERVICE_NO_CHANGE, disable ? SERVICE_DISABLED : saved.startType,
                                  SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return GetLastError();
        if (!disable && saved.startType == SERVICE_AUTO_START) {
            SERVICE_DELAYED_AUTO_START_INFO delayed{static_cast<BOOL>(saved.delayed)};
            if (!ChangeServiceConfig2W(svc.handle, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) return GetLastError();
        }
        if (disable && status.dwCurrentState != SERVICE_STOPPED) {
            if (status.dwCurrentState != SERVICE_STOP_PENDING &&
                !ControlService(svc.handle, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
                return GetLastError();
            return WaitForState(svc.handle, SERVICE_STOPPED);
        }
        return ERROR_SUCCESS;
    }
    if (saved.running && status.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(svc.handle, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return GetLastError();
        if (const auto error = WaitForState(svc.handle, SERVICE_RUNNING)) return error;
    }
    return RegDeleteValueW(backups, name);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || (wcscmp(argv[1], L"disable") && wcscmp(argv[1], L"restore"))) return ERROR_INVALID_PARAMETER;
    const bool disable = wcscmp(argv[1], L"disable") == 0;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\openehub.ServiceChange");
    if (!mutex) return GetLastError();
    const auto acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return ERROR_BUSY;
    Service scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.handle) return GetLastError();
    HKEY backups{};
    auto rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, MateBookServices::backupKey, 0, nullptr, 0,
                              KEY_READ | KEY_SET_VALUE, nullptr, &backups, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    DWORD result{};
    bool configured[_countof(MateBookServices::names)]{};
    for (size_t i = 0; i < _countof(MateBookServices::names); ++i) {
        auto error = Change(backups, scm.handle, MateBookServices::names[i], disable, false);
        configured[i] = error == ERROR_SUCCESS;
        if (error) result = error;
    }
    if (!disable) {
        for (size_t i = _countof(MateBookServices::names); i-- > 0;) {
            if (!configured[i]) continue;
            if (auto error = Change(backups, scm.handle, MateBookServices::names[i], false, true)) result = error;
        }
    }
    RegCloseKey(backups);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}
