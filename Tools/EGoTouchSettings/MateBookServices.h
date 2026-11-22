#pragma once

namespace MateBookServices {
// Display enhancement, OSD and input services must stay available to the OEM drivers.
inline constexpr const wchar_t* names[] = {
    L"HiConnectivityService", L"HwDistributedMainService", L"MouseCrossingDaemon",
    L"MBAMainService", L"WUCSProxy", L"HwPCCoreService"
};
inline constexpr wchar_t backupKey[] = L"SOFTWARE\\openehub\\ServiceBackup";
}
