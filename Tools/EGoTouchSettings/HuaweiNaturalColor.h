#pragma once
#include <windows.h>
#include <shlobj.h>
#include <filesystem>

// MonitorManage uses the Natural Color feature (4) of the installed OneControl API.
class HuaweiNaturalColor {
    using Call = int (__cdecl *)(int);
    static constexpr int feature = 4;
    HMODULE library{};
    Call get{}, set{}, notify{}, fini{};
    bool ready{};
public:
    HuaweiNaturalColor() = default;
    HuaweiNaturalColor(const HuaweiNaturalColor&) = delete;
    ~HuaweiNaturalColor() {
        if (ready) fini(feature);
        if (library) FreeLibrary(library);
    }
    int Read() {
        if (!ready && !library) {
            PWSTR folder{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return -1;
            const auto dll = std::filesystem::path(folder) / L"Huawei/HwLcdEnhancement/OneControlApi.dll";
            CoTaskMemFree(folder);
            library = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library) return -1;
            auto symbol = [this](const char* name) { return reinterpret_cast<Call>(GetProcAddress(library, name)); };
            const auto init = symbol("InitConfigSyncObject");
            fini = symbol("FiniConfigSyncObject");
            get = symbol("GetNatureColorStatus");
            set = symbol("SetNatureColorStatus");
            notify = symbol("NotifyConfigChange");
            ready = init && fini && get && set && notify && init(feature) == 0;
        }
        return ready ? get(-1) : -1;
    }
    bool Set(bool enabled) {
        return ready && set(enabled) == 0 && notify(feature) == 0 && Read() == static_cast<int>(enabled);
    }
};
