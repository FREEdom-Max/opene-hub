#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include <shellapi.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::EGoTouchSettings::implementation {

namespace {

bool HasCommandLineSwitch(const wchar_t* name) {
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < count && !found; ++i) {
        found = _wcsicmp(argv[i], name) == 0;
    }
    LocalFree(argv);
    return found;
}

} // namespace

App::App() {
    InitializeComponent();
    m_singleton = CreateMutexW(nullptr, FALSE, EGoTouchTrayIpc::kSettingsMutexName);
    m_secondaryInstance = m_singleton && GetLastError() == ERROR_ALREADY_EXISTS;

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& args) {
        if (IsDebuggerPresent()) {
            [[maybe_unused]] const auto message = args.Message();
            __debugbreak();
        }
    });
#endif
}

App::~App() {
    if (m_singleton) CloseHandle(m_singleton);
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    if (HasCommandLineSwitch(L"--quit")) {
        if (HWND bridge = FindWindowExW(
                HWND_MESSAGE, nullptr, EGoTouchTrayIpc::kSettingsBridgeWindowClass, nullptr)) {
            DWORD ownerPid = 0;
            GetWindowThreadProcessId(bridge, &ownerPid);
            HANDLE proc = (ownerPid && ownerPid != GetCurrentProcessId())
                ? OpenProcess(SYNCHRONIZE, FALSE, ownerPid)
                : nullptr;
            PostMessageW(bridge, EGoTouchTrayIpc::kSettingsLifecycleMessage,
                         static_cast<WPARAM>(EGoTouchTrayIpc::SettingsLifecycle::InstallStarted), 0);
            if (proc) {
                (void)WaitForSingleObject(proc, 4000);
                CloseHandle(proc);
            }
        }
#if defined(_M_X64) && !defined(_M_ARM64EC)
        MateBookPowerFreqBinder::RestoreStaleSessionIfPresent();
#endif
        Exit();
        return;
    }

    const bool background = HasCommandLineSwitch(L"--background");
    if (m_secondaryInstance) {
        HWND bridge = nullptr;
        for (int attempt = 0; attempt < 10 && !bridge; ++attempt) {
            bridge = FindWindowExW(
                HWND_MESSAGE, nullptr, EGoTouchTrayIpc::kSettingsBridgeWindowClass, nullptr);
            if (!bridge) Sleep(50);
        }
        if (bridge && !background) {
            PostMessageW(bridge, EGoTouchTrayIpc::kActivateMessage, 0, 0);
        }
        Exit();
        return;
    }

    auto mainWin = make_self<MainWindow>();
    m_window = *mainWin;
    if (!mainWin->StartsHidden()) {
        m_window.Activate();
    }
}

} // namespace winrt::EGoTouchSettings::implementation
