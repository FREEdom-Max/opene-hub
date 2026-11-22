#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <cwchar>
#include <climits>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const HWND window = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    DWORD pid{};
    if (!GetWindowThreadProcessId(window, &pid)) return 1;
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent) return 1;
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return 1;
    const auto path = std::filesystem::path(folder) / L"Huawei" / L"PCManager" / L"WmiUtil.dll";
    CoTaskMemFree(folder);
    HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    using ReadFan = int(__cdecl*)(int&, int);
    auto read = dll ? reinterpret_cast<ReadFan>(GetProcAddress(dll, "?GetFanSpeed@BiosWmi@HwSDK@@SAHAEAHW4FanSpeedLocateNumber@2@@Z")) : nullptr;
    do {
        DWORD owner{};
        if (!GetWindowThreadProcessId(window, &owner) || owner != pid) break;
        if (GetPropW(window, L"OpenEHub.ReadFan")) {
            int rpm = INT_MAX;
            const int status = read ? read(rpm, 0) : -1;
            const bool valid = status == 0 && rpm >= 0 && rpm != INT_MAX;
            PostMessageW(window, WM_APP + 253, valid ? 0 : (status == -5 ? 1 : 2), valid ? rpm : 0);
            if (!valid) break;
        }
    } while (WaitForSingleObject(parent, 2500) == WAIT_TIMEOUT);
    if (dll) FreeLibrary(dll);
    CloseHandle(parent);
    return 0;
}
