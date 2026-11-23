#pragma once
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>

// Uses Huawei HwLcdEnhancement's OneControlApi.dll for Natural Color, Eye Comfort,
// Color Temperature, and Gamut status on Intel MateBook E (DiracR).
class HuaweiNaturalColor {
    struct ColorTemperature3DInfo {
        double r;
        double g;
        double b;
        uint32_t colorBlue;
        uint32_t pad;
        double protectValue;
    };
    static_assert(sizeof(ColorTemperature3DInfo) == 40);

    using CallInt = int(__cdecl*)(int);
    using CallVoid = int(__cdecl*)();
    using CallDouble = double(__cdecl*)(double);
    using CallSet3D = int(__cdecl*)(const ColorTemperature3DInfo*);

    static constexpr int kFeatureEyeProtect = 1;
    static constexpr int kFeatureNaturalColor = 4;

    HMODULE library{};
    CallInt getNature{}, setNature{}, notify{}, finiSync{};
    CallInt getEye{}, setEye{}, getGamut{};
    CallDouble getRhine{};
    CallVoid initFunc{}, finiFunc{};
    CallSet3D set3D{};
    bool syncNatureReady{};
    bool syncEyeReady{};
    bool funcReady{};
    bool dllChecked{};
    bool dllPresent{};

    static std::filesystem::path ResolveDllPath() {
        PWSTR folder{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return {};
        auto dll = std::filesystem::path(folder) / L"Huawei" / L"HwLcdEnhancement" / L"OneControlApi.dll";
        CoTaskMemFree(folder);
        return dll;
    }

    bool EnsureLoaded() {
        if (library) return true;
        const auto dll = ResolveDllPath();
        if (dll.empty()) return false;
        library = LoadLibraryExW(
            dll.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return false;

        auto symInt = [this](const char* name) {
            return reinterpret_cast<CallInt>(GetProcAddress(library, name));
        };
        const auto initSync = symInt("InitConfigSyncObject");
        finiSync = symInt("FiniConfigSyncObject");
        notify = symInt("NotifyConfigChange");
        getNature = symInt("GetNatureColorStatus");
        setNature = symInt("SetNatureColorStatus");
        getEye = symInt("GetEyeProtectStatus");
        setEye = symInt("SetEyeProtectStatus");
        getGamut = symInt("GetColorFieldCurrentMode");
        getRhine = reinterpret_cast<CallDouble>(GetProcAddress(library, "GetRhineValue"));
        initFunc = reinterpret_cast<CallVoid>(GetProcAddress(library, "Init"));
        finiFunc = reinterpret_cast<CallVoid>(GetProcAddress(library, "Fini"));
        set3D = reinterpret_cast<CallSet3D>(GetProcAddress(library, "Set3DColorTemperature"));

        if (initSync && finiSync && notify) {
            if (getNature && setNature && initSync(kFeatureNaturalColor) == 0) {
                syncNatureReady = true;
            }
            if (getEye && setEye && initSync(kFeatureEyeProtect) == 0) {
                syncEyeReady = true;
            }
        }
        return true;
    }

    bool EnsureFuncReady() {
        if (!EnsureLoaded()) return false;
        if (!funcReady && initFunc && finiFunc && set3D) {
            funcReady = (initFunc() == 0);
        }
        return funcReady;
    }

    static void KelvinToDiracRRgb(int kelvin, double& r, double& g, double& b) noexcept {
        if (kelvin <= 0) {
            r = g = b = 1.0;
            return;
        }
        // Factory calibration table from Util_WPF.dll (DiracR <naturalColor><algoInfo>).
        static constexpr double kTable[9][4] = {
            {2000.0,  1.0,            0.912727272727, 0.84},
            {3000.0,  1.0,            0.936802973978, 0.895910780669},
            {4000.0,  1.0,            0.961977186312, 0.958174904943},
            {5000.0,  1.0,            0.973076923077, 0.996153846154},
            {6000.0,  0.969811320755, 0.954716981132, 1.0},
            {7000.0,  0.940740740741, 0.940740740741, 1.0},
            {8000.0,  0.919708029197, 0.92700729927,  1.0},
            {9000.0,  0.89928057554,  0.913669064748, 1.0},
            {10000.0, 0.876325088339, 0.897526501767, 1.0},
        };
        const double cct = static_cast<double>(std::clamp(kelvin, 2000, 10000));
        if (cct <= kTable[0][0]) {
            r = kTable[0][1]; g = kTable[0][2]; b = kTable[0][3];
            return;
        }
        if (cct >= kTable[8][0]) {
            r = kTable[8][1]; g = kTable[8][2]; b = kTable[8][3];
            return;
        }
        for (int i = 0; i < 8; ++i) {
            if (cct > kTable[i + 1][0]) continue;
            const double a = (cct - kTable[i][0]) / (kTable[i + 1][0] - kTable[i][0]);
            r = kTable[i][1] + a * (kTable[i + 1][1] - kTable[i][1]);
            g = kTable[i][2] + a * (kTable[i + 1][2] - kTable[i][2]);
            b = kTable[i][3] + a * (kTable[i + 1][3] - kTable[i][3]);
            return;
        }
        r = g = b = 1.0;
    }

public:
    HuaweiNaturalColor() = default;
    HuaweiNaturalColor(const HuaweiNaturalColor&) = delete;
    ~HuaweiNaturalColor() {
        if (funcReady && finiFunc) finiFunc();
        if (syncEyeReady && finiSync) finiSync(kFeatureEyeProtect);
        if (syncNatureReady && finiSync) finiSync(kFeatureNaturalColor);
        if (library) FreeLibrary(library);
    }

    int Read() {
        if (!EnsureLoaded() || !syncNatureReady || !getNature) return -1;
        return getNature(-1);
    }

    bool Set(bool enabled) {
        if (!EnsureLoaded() || !syncNatureReady || !setNature || !notify) return false;
        return setNature(enabled ? 1 : 0) == 0 &&
               notify(kFeatureNaturalColor) == 0 &&
               Read() == static_cast<int>(enabled);
    }

    int ReadGamut() {
        if (!EnsureLoaded() || !getGamut) return -1;
        return getGamut(-1);
    }

    int ReadEyeComfort() {
        if (!EnsureLoaded() || !getEye) return -1;
        return getEye(-1);
    }

    bool IsDisplayControlAvailable() {
        if (library) return getEye && setEye && initFunc && finiFunc && set3D;
        if (!dllChecked) {
            dllChecked = true;
            const auto dll = ResolveDllPath();
            std::error_code ec;
            dllPresent = !dll.empty() && std::filesystem::exists(dll, ec);
        }
        return dllPresent;
    }

    bool ApplyEyeAndTemperature(bool eyeComfort, int kelvin) {
        if (!EnsureFuncReady()) return false;
        double rhine = getRhine ? getRhine(0.72) : 0.72;
        if (rhine < 0.3 || rhine > 1.0) rhine = 0.72;

        ColorTemperature3DInfo info{};
        KelvinToDiracRRgb(kelvin, info.r, info.g, info.b);
        info.colorBlue = eyeComfort ? 255u : 0u;
        info.protectValue = rhine;

        if (set3D(&info) != 0) return false;
        if (setEye) {
            (void)setEye(eyeComfort ? 1 : 0);
            if (syncEyeReady && notify) (void)notify(kFeatureEyeProtect);
        }
        return true;
    }
};
