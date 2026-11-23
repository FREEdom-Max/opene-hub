#include "pch.h"

#include "QuickFlyoutWindow.xaml.h"
#include "AccessoryBatteryState.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

#include <cmath>
#include <cstdio>

#if __has_include("QuickFlyoutWindow.g.cpp")
#include "QuickFlyoutWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;

namespace winrt::EGoTouchSettings::implementation {

namespace {

constexpr int kFlyoutWidthDip = 344;
constexpr int kScreenMarginDip = 12;
constexpr int kAnchorGapDip = 12;
constexpr int kMinFlyoutHeightDip = 180;
constexpr int kEditModeHeightDip = 376;
constexpr int kEnterSlideDip = 12;
constexpr int kEnterAnimMs = 150;
constexpr int kDefaultColorTempKelvin = 6500;
constexpr int kMinColorTempKelvin = 2000;
constexpr int kMaxColorTempKelvin = 10000;
constexpr int kColorTempStepKelvin = 500;
constexpr UINT kDwmwaWindowCornerPreference = 33;
constexpr int kDwmCornerRound = 2;
constexpr UINT kDwmwaUseImmersiveDarkMode = 20;
constexpr UINT kDwmwaBorderColor = 34;
constexpr COLORREF kBorderColorDark = 0x00303030;
constexpr COLORREF kBorderColorLight = 0x00DFDFDF;

enum class TaskbarEdge { Bottom, Top, Left, Right };

bool ClientAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)
        ? (enabled != FALSE)
        : true;
}

bool IsTileChecked(ToggleButton const& button) {
    return button.IsChecked() && button.IsChecked().Value();
}

int SnapColorTempKelvin(double raw) {
    const int rounded = static_cast<int>(
        std::lround(raw / static_cast<double>(kColorTempStepKelvin))) * kColorTempStepKelvin;
    return std::clamp(rounded, kMinColorTempKelvin, kMaxColorTempKelvin);
}

std::wstring FormatWatts(int32_t milliWatt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%.1f W",
                  std::abs(static_cast<double>(milliWatt)) / 1000.0);
    return buf;
}

void SubtractVisibleTrayWindow(
        HWND trayHwnd, HMONITOR targetMonitor, const RECT& monitorRect,
        RECT& work, TaskbarEdge& edge) noexcept {
    if (!trayHwnd || !IsWindowVisible(trayHwnd)) return;
    if (MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONULL) != targetMonitor) return;

    RECT tr{};
    if (!GetWindowRect(trayHwnd, &tr)) return;

    RECT clipped{};
    if (!IntersectRect(&clipped, &tr, &monitorRect)) return;

    const int w = clipped.right - clipped.left;
    const int h = clipped.bottom - clipped.top;
    if (w <= 0 || h <= 0) return;

    const int monMidX = (monitorRect.left + monitorRect.right) / 2;
    const int monMidY = (monitorRect.top + monitorRect.bottom) / 2;
    if (w >= h) {
        if ((clipped.top + clipped.bottom) / 2 >= monMidY) {
            work.bottom = (std::min)(work.bottom, clipped.top);
            edge = TaskbarEdge::Bottom;
        } else {
            work.top = (std::max)(work.top, clipped.bottom);
            edge = TaskbarEdge::Top;
        }
    } else {
        if ((clipped.left + clipped.right) / 2 <= monMidX) {
            work.left = (std::max)(work.left, clipped.right);
            edge = TaskbarEdge::Left;
        } else {
            work.right = (std::min)(work.right, clipped.left);
            edge = TaskbarEdge::Right;
        }
    }
}

} // namespace

QuickFlyoutWindow::QuickFlyoutWindow() {
    InitializeComponent();
    m_initialized = true;

    FlyoutHostBattery().Configure(false, false, false);
    FlyoutPenBattery().Configure(true, false, true);
    FlyoutKeyboardBattery().Configure(true, false, true);

    ConfigureWindow();

    FlyoutRoot().ActualThemeChanged(
        [this](FrameworkElement const&, IInspectable const&) { SyncFrameTheme(); });
    FlyoutRoot().Loaded(
        [this](IInspectable const&, RoutedEventArgs const&) {
            m_loaded = true;
            SyncFrameTheme();
            if (!m_visible || m_shutdown) return;
            RelayoutWindowHeight();
            if (m_pendingInitialFocus) {
                m_pendingInitialFocus = false;
                FocusDefaultElement();
            }
        });

    m_activatedToken = Activated(
        [this](IInspectable const&, WindowActivatedEventArgs const& args) {
            if (!m_visible || m_shutdown) return;
            if (args.WindowActivationState() == WindowActivationState::Deactivated) {
                const HWND self = WindowHandle();
                const HWND fg = GetForegroundWindow();
                const HWND active = GetActiveWindow();
                if ((fg && (fg == self || IsChild(self, fg))) ||
                    (active && (active == self || IsChild(self, active)))) {
                    return;
                }
                // 记录失焦隐藏时刻，防止点击托盘图标关闭飞出窗时任务栏先抢焦，
                // 随后到达的 WM_LBUTTONUP 又误判为“当前已关闭”而立刻重新弹出。
                m_hiddenByDeactivateTick = GetTickCount64();
                HideFlyout();
            }
        });

    m_colorTempTimer = this->DispatcherQueue().CreateTimer();
    m_colorTempTimer.Interval(std::chrono::milliseconds(60));
    m_colorTempTimer.IsRepeating(false);
    m_colorTempTickToken = m_colorTempTimer.Tick(
        [this](Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender, IInspectable const&) {
            sender.Stop();
            FlushPendingColorTemperature();
        });

    m_animTimer = this->DispatcherQueue().CreateTimer();
    m_animTimer.Interval(std::chrono::milliseconds(8));
    m_animTimer.IsRepeating(true);
    m_animTickToken = m_animTimer.Tick(
        [this](Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender, IInspectable const&) {
            if (m_shutdown || !m_visible || !m_animRunning) {
                m_animRunning = false;
                sender.Stop();
                return;
            }
            const HWND hwnd = WindowHandle();
            const double elapsed = static_cast<double>(GetTickCount64() - m_animStartTick);
            const double t = std::clamp(elapsed / static_cast<double>(kEnterAnimMs), 0.0, 1.0);
            const double eased = 1.0 - std::pow(1.0 - t, 3.0);

            const int currentTop = static_cast<int>(std::lround(
                m_animStartTop + (m_animTargetBounds.top - m_animStartTop) * eased));
            const uint8_t alpha = static_cast<uint8_t>(std::lround(255.0 * eased));
            const int widthPx = m_animTargetBounds.right - m_animTargetBounds.left;
            const int heightPx = m_animTargetBounds.bottom - m_animTargetBounds.top;

            SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
            SetWindowPos(hwnd, HWND_TOPMOST, m_animTargetBounds.left, currentTop,
                         widthPx, heightPx, SWP_NOACTIVATE | SWP_NOZORDER);

            if (t >= 1.0) {
                m_animRunning = false;
                sender.Stop();
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                SetWindowPos(hwnd, HWND_TOPMOST, m_animTargetBounds.left, m_animTargetBounds.top,
                             widthPx, heightPx, SWP_NOACTIVATE | SWP_NOZORDER);
            }
        });
}

QuickFlyoutWindow::~QuickFlyoutWindow() {
    if (m_colorTempTimer) {
        m_colorTempTimer.Stop();
        if (m_colorTempTickToken.value) {
            m_colorTempTimer.Tick(m_colorTempTickToken);
            m_colorTempTickToken = {};
        }
    }
    if (m_animTimer) {
        m_animTimer.Stop();
        if (m_animTickToken.value) {
            m_animTimer.Tick(m_animTickToken);
            m_animTickToken = {};
        }
    }
    if (m_activatedToken.value) Activated(m_activatedToken);
    RestoreWndProc();
}

void QuickFlyoutWindow::SetCallbacks(QuickFlyoutCallbacks callbacks) {
    m_callbacks = std::move(callbacks);
}

HWND QuickFlyoutWindow::WindowHandle() {
    winrt::EGoTouchSettings::QuickFlyoutWindow projected = *this;
    const auto windowNative = projected.as<IWindowNative>();
    HWND hwnd = nullptr;
    check_hresult(windowNative->get_WindowHandle(&hwnd));
    return hwnd;
}

void QuickFlyoutWindow::RestoreWndProc() noexcept {
    if (!m_originalWndProc) return;
    winrt::EGoTouchSettings::QuickFlyoutWindow projected = *this;
    if (const auto windowNative = projected.try_as<IWindowNative>()) {
        HWND hwnd = nullptr;
        if (SUCCEEDED(windowNative->get_WindowHandle(&hwnd)) && hwnd && IsWindow(hwnd)) {
            SetWindowLongPtrW(
                hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_originalWndProc));
            RemovePropW(hwnd, L"OpenEGoHub.QuickFlyout");
        }
    }
    m_originalWndProc = nullptr;
}

LRESULT CALLBACK QuickFlyoutWindow::FlyoutWndProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = static_cast<QuickFlyoutWindow*>(
        GetPropW(hwnd, L"OpenEGoHub.QuickFlyout"));
    const WNDPROC prev = self ? self->m_originalWndProc : nullptr;
    if (message == WM_ACTIVATEAPP && wParam == FALSE && self &&
        self->m_visible && !self->m_shutdown) {
        self->m_hiddenByDeactivateTick = GetTickCount64();
        self->HideFlyout();
    }
    return prev ? CallWindowProcW(prev, hwnd, message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

void QuickFlyoutWindow::ConfigureWindow() {
    const HWND hwnd = WindowHandle();
    const WindowId id = GetWindowIdFromWindow(hwnd);
    const Microsoft::UI::Windowing::AppWindow appWindow =
        Microsoft::UI::Windowing::AppWindow::GetFromWindowId(id);
    if (const auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>()) {
        presenter.SetBorderAndTitleBar(false, false);
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    exStyle &= ~WS_EX_NOACTIVATE;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

    const int corner = kDwmCornerRound;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));

    if (!m_originalWndProc) {
        SetPropW(hwnd, L"OpenEGoHub.QuickFlyout", this);
        m_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FlyoutWndProc)));
    }
}

void QuickFlyoutWindow::ApplyTheme(ElementTheme theme) {
    FlyoutRoot().RequestedTheme(theme);
}

void QuickFlyoutWindow::SyncFrameTheme() {
    const bool dark = FlyoutRoot().ActualTheme() == ElementTheme::Dark;
    const HWND hwnd = WindowHandle();
    const BOOL immersive = dark ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &immersive, sizeof(immersive));
    const COLORREF border = dark ? kBorderColorDark : kBorderColorLight;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border, sizeof(border));
}

int QuickFlyoutWindow::EstimateHeightDip() const noexcept {
    if (m_editMode) {
        return kEditModeHeightDip + (m_naturalColorSupported ? 32 : 0) +
               (m_batteryProfileSupported ? 32 : 0) + (m_thermalSupported ? 32 : 0);
    }

    int height = 78; // 上下内边距 + 底栏 + 底部分隔线
    int tileCount = 0;
    if (!m_hideTouchTile && (m_itemsMask & QuickFlyoutItems::kTouchTile)) ++tileCount;
    if (m_itemsMask & QuickFlyoutItems::kEyeComfortTile) ++tileCount;
    if (m_naturalColorSupported && (m_itemsMask & QuickFlyoutItems::kNaturalColorTile)) ++tileCount;
    if (m_itemsMask & QuickFlyoutItems::kSmartChargeTile) ++tileCount;
    if (m_itemsMask & QuickFlyoutItems::kKbdWirelessTile) ++tileCount;
    if (m_itemsMask & QuickFlyoutItems::kEnergySaverTile) ++tileCount;

    const bool hasTiles = tileCount > 0;
    const bool hasColorTemp = (m_itemsMask & QuickFlyoutItems::kColorTempSlider) != 0;
    const bool hasHostPower = (m_itemsMask & QuickFlyoutItems::kHostPowerCard) != 0;
    const bool hasBatteryProfile =
        m_batteryProfileSupported && (m_itemsMask & QuickFlyoutItems::kBatteryProfileRow) != 0;
    const bool hasThermal = m_thermalSupported && (m_itemsMask & QuickFlyoutItems::kThermalRow) != 0;
    const bool hasAccessories = (m_itemsMask & QuickFlyoutItems::kAccessories) != 0 &&
                                (m_penRowVisible || m_kbdRowVisible);

    if (hasTiles) {
        const int rows = (tileCount + 1) / 2;
        height += rows * 60 + (std::max)(0, rows - 1) * 8 + 12;
    }
    if (hasColorTemp) height += 46;
    if ((hasTiles || hasColorTemp) && (hasHostPower || hasBatteryProfile || hasThermal || hasAccessories)) height += 13;
    if (hasHostPower) height += 74;
    if (hasBatteryProfile) height += 44;
    if (hasThermal) height += 36;
    if ((hasHostPower || hasBatteryProfile || hasThermal) && hasAccessories) height += 13;
    if (hasAccessories) {
        if (m_penRowVisible) height += 30;
        if (m_kbdRowVisible) height += 30;
        if (m_penRowVisible && m_kbdRowVisible) height += 6;
        height += 6;
    }
    if (!hasTiles && !hasColorTemp && !hasHostPower && !hasBatteryProfile && !hasThermal && !hasAccessories) {
        height += 40;
    }
    return (std::max)(kMinFlyoutHeightDip, height);
}

int QuickFlyoutWindow::MeasureDesiredHeightDip() {
    const int fallback = EstimateHeightDip();
    if (!m_loaded || !FlyoutRoot().XamlRoot()) {
        return fallback;
    }
    try {
        FlyoutRoot().UpdateLayout();
        FlyoutRoot().Measure(
            Windows::Foundation::Size{static_cast<float>(kFlyoutWidthDip), 2000.0f});
        const int measured =
            static_cast<int>(std::ceil(FlyoutRoot().DesiredSize().Height)) + 2;
        return (std::max)(kMinFlyoutHeightDip, measured);
    } catch (...) {
        return fallback;
    }
}

RECT QuickFlyoutWindow::ComputeAnchorBounds(
        bool hasAnchor, int anchorX, int anchorY, int heightDip, bool& openDownward) {
    const HWND hwnd = WindowHandle();
    const POINT monitorProbe = hasAnchor ? POINT{anchorX, anchorY} : POINT{0, 0};
    const HMONITOR monitor = MonitorFromPoint(monitorProbe, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    RECT work{};
    RECT monitorRect{};
    if (GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
        monitorRect = info.rcMonitor;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        monitorRect = work;
    }

    // 展开态平板任务栏不会改写 SPI/MONITORINFO.rcWork，需扣除可见 Shell_TrayWnd 矩形避免遮挡。
    TaskbarEdge edge = TaskbarEdge::Bottom;
    SubtractVisibleTrayWindow(FindWindowW(L"Shell_TrayWnd", nullptr), monitor, monitorRect, work, edge);
    HWND secondaryTray = nullptr;
    while ((secondaryTray = FindWindowExW(nullptr, secondaryTray, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        SubtractVisibleTrayWindow(secondaryTray, monitor, monitorRect, work, edge);
    }

    UINT dpiX = 0;
    UINT dpiY = 0;
    UINT dpi = 0;
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
        dpi = dpiX;
    } else {
        const UINT rawDpi = GetDpiForWindow(hwnd);
        dpi = rawDpi ? rawDpi : 96;
    }
    const int widthPx = MulDiv(kFlyoutWidthDip, dpi, 96);
    const int heightPx = MulDiv(heightDip, dpi, 96);
    const int marginPx = MulDiv(kScreenMarginDip, dpi, 96);

    const int minLeft = static_cast<int>(work.left) + marginPx;
    const int workRight = static_cast<int>(work.right) - marginPx;
    const int maxLeft = (std::max)(minLeft, workRight - widthPx);
    const int minTop = static_cast<int>(work.top) + marginPx;
    const int workBottom = static_cast<int>(work.bottom) - marginPx;
    const int maxTop = (std::max)(minTop, workBottom - heightPx);

    int left = maxLeft;
    int top = maxTop;
    openDownward = false;
    switch (edge) {
    case TaskbarEdge::Top:
        left = maxLeft;
        top = minTop;
        openDownward = true;
        break;
    case TaskbarEdge::Left:
        left = minLeft;
        top = maxTop;
        openDownward = false;
        break;
    case TaskbarEdge::Right:
    case TaskbarEdge::Bottom:
    default:
        left = maxLeft;
        top = maxTop;
        openDownward = false;
        break;
    }

    if (hasAnchor) {
        const int gapPx = MulDiv(kAnchorGapDip, dpi, 96);

        // 四个象限候选均同时约束在工作区内并保留与点按点的安全间距，优先取左上方。
        const int leftCandidate = (std::min)(anchorX - gapPx, workRight) - widthPx;
        const int rightCandidate = (std::max)(anchorX + gapPx, minLeft);
        const int aboveCandidate = (std::min)(anchorY - gapPx, workBottom) - heightPx;
        const int belowCandidate = (std::max)(anchorY + gapPx, minTop);

        const bool fitsLeft = leftCandidate >= minLeft;
        const bool fitsRight = (rightCandidate + widthPx) <= workRight;
        const bool fitsAbove = aboveCandidate >= minTop;
        const bool fitsBelow = (belowCandidate + heightPx) <= workBottom;

        if (fitsLeft && fitsAbove) {
            left = leftCandidate;
            top = aboveCandidate;
            openDownward = false;
        } else if (fitsRight && fitsAbove) {
            left = rightCandidate;
            top = aboveCandidate;
            openDownward = false;
        } else if (fitsLeft && fitsBelow) {
            left = leftCandidate;
            top = belowCandidate;
            openDownward = true;
        } else if (fitsRight && fitsBelow) {
            left = rightCandidate;
            top = belowCandidate;
            openDownward = true;
        } else if (fitsAbove) {
            left = std::clamp(anchorX - widthPx / 2, minLeft, maxLeft);
            top = aboveCandidate;
            openDownward = false;
        } else if (fitsBelow) {
            left = std::clamp(anchorX - widthPx / 2, minLeft, maxLeft);
            top = belowCandidate;
            openDownward = true;
        } else if (fitsLeft) {
            left = leftCandidate;
            top = std::clamp(anchorY - heightPx / 2, minTop, maxTop);
            openDownward = false;
        } else if (fitsRight) {
            left = rightCandidate;
            top = std::clamp(anchorY - heightPx / 2, minTop, maxTop);
            openDownward = false;
        }
    }

    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);
    return RECT{left, top, left + widthPx, top + heightPx};
}

void QuickFlyoutWindow::StartWholeWindowEnterAnimation(
        const RECT& targetBounds, bool openDownward) {
    if (m_animTimer) m_animTimer.Stop();
    m_animRunning = false;
    m_animTargetBounds = targetBounds;

    const HWND hwnd = WindowHandle();
    const int widthPx = targetBounds.right - targetBounds.left;
    const int heightPx = targetBounds.bottom - targetBounds.top;

    if (!ClientAnimationsEnabled()) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(hwnd, HWND_TOPMOST, targetBounds.left, targetBounds.top,
                     widthPx, heightPx, SWP_SHOWWINDOW);
        return;
    }

    const UINT rawDpi = GetDpiForWindow(hwnd);
    const UINT dpi = rawDpi ? rawDpi : 96;
    const int slidePx = MulDiv(kEnterSlideDip, dpi, 96);
    m_animStartTop = openDownward ? targetBounds.top - slidePx : targetBounds.top + slidePx;
    m_animStartTick = GetTickCount64();
    m_animRunning = true;

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, targetBounds.left, m_animStartTop,
                 widthPx, heightPx, SWP_SHOWWINDOW);
    m_animTimer.Start();
}

void QuickFlyoutWindow::RelayoutWindowHeight() {
    if (m_shutdown || !m_visible) return;
    const int nextHeightDip = MeasureDesiredHeightDip();
    if (std::abs(nextHeightDip - m_lastMeasuredHeightDip) < 2) return;

    m_lastMeasuredHeightDip = nextHeightDip;
    bool openDownward = m_openDownward;
    const RECT bounds = ComputeAnchorBounds(
        m_lastHasAnchor, m_lastAnchorX, m_lastAnchorY, nextHeightDip, openDownward);
    m_openDownward = openDownward;

    if (m_animRunning) {
        const int delta = bounds.top - m_animTargetBounds.top;
        m_animStartTop += delta;
        m_animTargetBounds = bounds;
    } else {
        SetWindowPos(WindowHandle(), HWND_TOPMOST, bounds.left, bounds.top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void QuickFlyoutWindow::FocusDefaultElement() {
    try {
        if (!m_editMode && FlyoutRoot().XamlRoot()) {
            for (UIElement const& candidate : {
                     TouchTile().as<UIElement>(),
                     EyeComfortTile().as<UIElement>(),
                     NaturalColorTile().as<UIElement>(),
                     SmartChargeTile().as<UIElement>(),
                     KbdWirelessTile().as<UIElement>(),
                     EnergySaverTile().as<UIElement>()}) {
                if (candidate.Visibility() == Visibility::Visible) {
                    if (auto ctrl = candidate.try_as<Control>(); ctrl && ctrl.IsEnabled()) {
                        ctrl.Focus(FocusState::Programmatic);
                        return;
                    }
                }
            }
            if (FlyoutPowerSection().Visibility() == Visibility::Visible &&
                PowerEnergySaverBtn().IsEnabled()) {
                PowerEnergySaverBtn().Focus(FocusState::Programmatic);
                return;
            }
        }
        if (EditFlyoutButton().XamlRoot()) {
            EditFlyoutButton().Focus(FocusState::Programmatic);
        }
    } catch (...) {
    }
}

void QuickFlyoutWindow::ToggleAt(bool hasAnchor, int anchorX, int anchorY) {
    if (m_shutdown) return;
    if (m_visible && IsWindowVisible(WindowHandle())) {
        HideFlyout();
        return;
    }
    if (GetTickCount64() - m_hiddenByDeactivateTick < 250) {
        return;
    }
    ShowAt(hasAnchor, anchorX, anchorY);
}

void QuickFlyoutWindow::ShowAt(bool hasAnchor, int anchorX, int anchorY) {
    if (m_shutdown) return;
    m_lastHasAnchor = hasAnchor;
    m_lastAnchorX = anchorX;
    m_lastAnchorY = anchorY;
    m_editMode = false;

    if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    } else {
        ApplyItemsLayout();
    }

    if (!SystemBackdrop()) {
        SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
    }
    SyncFrameTheme();

    const int heightDip = MeasureDesiredHeightDip();
    m_lastMeasuredHeightDip = heightDip;
    bool openDownward = false;
    const RECT bounds = ComputeAnchorBounds(hasAnchor, anchorX, anchorY, heightDip, openDownward);
    m_openDownward = openDownward;

    const HWND hwnd = WindowHandle();
    const int widthPx = bounds.right - bounds.left;
    const int heightPx = bounds.bottom - bounds.top;

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, bounds.left, bounds.top,
                 widthPx, heightPx, SWP_NOACTIVATE);
    AppWindow().Show(false);
    Activate();
    SetForegroundWindow(hwnd);
    m_visible = true;

    StartWholeWindowEnterAnimation(bounds, openDownward);

    if (m_loaded && FlyoutRoot().XamlRoot()) {
        m_pendingInitialFocus = false;
        FocusDefaultElement();
    } else {
        m_pendingInitialFocus = true;
    }
    if (m_callbacks.onVisibilityChanged) m_callbacks.onVisibilityChanged();
}

void QuickFlyoutWindow::HideFlyout() {
    if (m_colorTempTimer && m_colorTempTimer.IsRunning()) {
        m_colorTempTimer.Stop();
        FlushPendingColorTemperature();
    }
    if (m_animTimer) m_animTimer.Stop();
    m_animRunning = false;
    const bool wasVisible = m_visible;
    m_visible = false;
    m_editMode = false;
    m_pendingInitialFocus = false;
    const HWND hwnd = WindowHandle();
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    AppWindow().Hide();
    ShowWindow(hwnd, SW_HIDE);
    SystemBackdrop(nullptr);
    if (wasVisible && m_callbacks.onVisibilityChanged) {
        m_callbacks.onVisibilityChanged();
    }
}

void QuickFlyoutWindow::UpdateState(const QuickFlyoutState& state) {
    if (m_shutdown || !m_visible) return;
    ApplyStateToControls(state);
    RelayoutWindowHeight();
}

bool QuickFlyoutWindow::IsVisible() const noexcept {
    return m_visible && !m_shutdown;
}

void QuickFlyoutWindow::Shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    HideFlyout();
    RestoreWndProc();
    m_callbacks = {};
}

void QuickFlyoutWindow::ApplyItemsLayout() {
    if (m_editMode) {
        NormalContentPanel().Visibility(Visibility::Collapsed);
        EditContentPanel().Visibility(Visibility::Visible);
        EditFlyoutIcon().Glyph(L"\xE8FB");
        EditFlyoutText().Text(L"完成");
        SyncEditCheckBoxes();
        return;
    }

    NormalContentPanel().Visibility(Visibility::Visible);
    EditContentPanel().Visibility(Visibility::Collapsed);
    EditFlyoutIcon().Glyph(L"\xE70F");
    EditFlyoutText().Text(L"编辑");

    struct TileEntry {
        uint32_t flag;
        bool allowed;
        ToggleButton button;
    };
    const TileEntry entries[] = {
        {QuickFlyoutItems::kTouchTile, !m_hideTouchTile, TouchTile()},
        {QuickFlyoutItems::kEyeComfortTile, true, EyeComfortTile()},
        {QuickFlyoutItems::kNaturalColorTile, m_naturalColorSupported, NaturalColorTile()},
        {QuickFlyoutItems::kSmartChargeTile, true, SmartChargeTile()},
        {QuickFlyoutItems::kKbdWirelessTile, true, KbdWirelessTile()},
        {QuickFlyoutItems::kEnergySaverTile, true, EnergySaverTile()},
    };

    int visibleTileCount = 0;
    for (const auto& entry : entries) {
        const bool show = entry.allowed && ((m_itemsMask & entry.flag) != 0);
        entry.button.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        if (show) {
            Controls::Grid::SetRow(entry.button, visibleTileCount / 2);
            Controls::Grid::SetColumn(entry.button, visibleTileCount % 2);
            ++visibleTileCount;
        } else {
            Controls::Grid::SetRow(entry.button, 0);
            Controls::Grid::SetColumn(entry.button, 0);
        }
    }

    const int visibleRows = (visibleTileCount + 1) / 2;
    TileGridRow0().Height(visibleRows > 0 ? GridLengthHelper::Auto() : GridLengthHelper::FromPixels(0));
    TileGridRow1().Height(visibleRows > 1 ? GridLengthHelper::Auto() : GridLengthHelper::FromPixels(0));
    TileGridRow2().Height(visibleRows > 2 ? GridLengthHelper::Auto() : GridLengthHelper::FromPixels(0));
    QuickTilesGrid().Visibility(visibleTileCount > 0 ? Visibility::Visible : Visibility::Collapsed);

    const bool showColorTemp = (m_itemsMask & QuickFlyoutItems::kColorTempSlider) != 0;
    FlyoutColorTempSection().Visibility(showColorTemp ? Visibility::Visible : Visibility::Collapsed);

    const bool showHostPower = (m_itemsMask & QuickFlyoutItems::kHostPowerCard) != 0;
    FlyoutPowerSection().Visibility(showHostPower ? Visibility::Visible : Visibility::Collapsed);

    const bool showBatteryProfile =
        m_batteryProfileSupported && ((m_itemsMask & QuickFlyoutItems::kBatteryProfileRow) != 0);
    FlyoutBatteryProfileSection().Visibility(showBatteryProfile ? Visibility::Visible : Visibility::Collapsed);

    const bool showThermal = m_thermalSupported && ((m_itemsMask & QuickFlyoutItems::kThermalRow) != 0);
    FlyoutThermalSection().Visibility(showThermal ? Visibility::Visible : Visibility::Collapsed);

    const bool showAccessories =
        ((m_itemsMask & QuickFlyoutItems::kAccessories) != 0) &&
        (m_penRowVisible || m_kbdRowVisible);
    FlyoutAccessorySection().Visibility(showAccessories ? Visibility::Visible : Visibility::Collapsed);

    const bool hasTop = visibleTileCount > 0 || showColorTemp;
    const bool hasMiddle = showHostPower || showBatteryProfile || showThermal;
    const bool hasBottom = hasMiddle || showAccessories;
    FlyoutTopDivider().Visibility((hasTop && hasBottom) ? Visibility::Visible : Visibility::Collapsed);
    FlyoutAccessoryDivider().Visibility(
        (hasMiddle && showAccessories) ? Visibility::Visible : Visibility::Collapsed);
    FlyoutEmptyHintText().Visibility(
        (!hasTop && !hasBottom) ? Visibility::Visible : Visibility::Collapsed);
}

void QuickFlyoutWindow::SyncEditCheckBoxes() {
    m_updatingControls = true;
    EditCheckTouch().Visibility(m_hideTouchTile ? Visibility::Collapsed : Visibility::Visible);
    EditCheckTouch().IsChecked((m_itemsMask & QuickFlyoutItems::kTouchTile) != 0);
    EditCheckEyeComfort().IsChecked((m_itemsMask & QuickFlyoutItems::kEyeComfortTile) != 0);
    EditCheckNaturalColor().Visibility(m_naturalColorSupported ? Visibility::Visible : Visibility::Collapsed);
    EditCheckNaturalColor().IsChecked((m_itemsMask & QuickFlyoutItems::kNaturalColorTile) != 0);
    EditCheckSmartCharge().IsChecked((m_itemsMask & QuickFlyoutItems::kSmartChargeTile) != 0);
    EditCheckKbdWireless().IsChecked((m_itemsMask & QuickFlyoutItems::kKbdWirelessTile) != 0);
    EditCheckEnergySaver().IsChecked((m_itemsMask & QuickFlyoutItems::kEnergySaverTile) != 0);
    EditCheckColorTemp().IsChecked((m_itemsMask & QuickFlyoutItems::kColorTempSlider) != 0);
    EditCheckHostPower().IsChecked((m_itemsMask & QuickFlyoutItems::kHostPowerCard) != 0);
    EditCheckBatteryProfile().Visibility(m_batteryProfileSupported ? Visibility::Visible : Visibility::Collapsed);
    EditCheckBatteryProfile().IsChecked((m_itemsMask & QuickFlyoutItems::kBatteryProfileRow) != 0);
    EditCheckThermal().Visibility(m_thermalSupported ? Visibility::Visible : Visibility::Collapsed);
    EditCheckThermal().IsChecked((m_itemsMask & QuickFlyoutItems::kThermalRow) != 0);
    EditCheckAccessories().IsChecked((m_itemsMask & QuickFlyoutItems::kAccessories) != 0);
    m_updatingControls = false;
}

void QuickFlyoutWindow::ApplyPowerModeSelection(PowerOverlay::Mode mode) {
    PowerModeEfficiencyBtn().IsChecked(mode == PowerOverlay::Mode::BestEfficiency);
    PowerModeBalancedBtn().IsChecked(mode == PowerOverlay::Mode::Balanced);
    PowerModePerformanceBtn().IsChecked(mode == PowerOverlay::Mode::BestPerformance);
}

void QuickFlyoutWindow::ApplyBatteryProfileSelection(MateBookBatteryProfile::Profile profile) {
    BatteryProfileQuietBtn().IsChecked(profile == MateBookBatteryProfile::Profile::Quiet);
    BatteryProfileDailyBtn().IsChecked(profile == MateBookBatteryProfile::Profile::Daily);
    BatteryProfilePerfBtn().IsChecked(profile == MateBookBatteryProfile::Profile::Performance);
}

void QuickFlyoutWindow::ApplyStateToControls(const QuickFlyoutState& state) {
    m_updatingControls = true;

    m_hideTouchTile = state.hideTouchTile;
    m_naturalColorSupported = state.naturalColorSupported;
    m_thermalSupported = state.thermalSupported;
    m_batteryProfileSupported = state.batteryProfileSupported;

    m_itemsMask = state.itemsMask | QuickFlyoutItems::kConfiguredBit |
                  QuickFlyoutItems::kSmartChargeDefaultMigratedBit;
    if (m_hideTouchTile) {
        m_itemsMask |= QuickFlyoutItems::kMateBookEDefaultMigratedBit |
                       QuickFlyoutItems::kBatteryProfileMigratedBit;
    }

    TouchTile().IsEnabled(state.trayConnected);
    TouchTile().IsChecked(state.touchDesired);

    EyeComfortTile().IsEnabled(state.trayConnected || state.eyeComfortAvailable);
    EyeComfortTile().IsChecked(state.eyeComfort);

    NaturalColorTile().IsEnabled(state.naturalColorAvailable);
    NaturalColorTile().IsChecked(state.naturalColor);

    const bool kbdDetected =
        state.hasState && state.status.hasKbdPresent && state.status.kbdPresent;
    const bool kbdChecked = state.kbdDetachPending
        ? state.kbdDetachPendingValue
        : (kbdDetected && state.status.hasKbdDetachSupport && state.status.kbdDetachSupport);
    KbdWirelessTile().IsEnabled((state.trayConnected || state.kbdWirelessAvailable) && kbdDetected);
    KbdWirelessTile().IsChecked(kbdChecked);

    SmartChargeTile().IsEnabled((state.trayConnected || state.smartChargeAvailable) && state.hasChargeLimit);
    SmartChargeTile().IsChecked(state.smartCharge);
    const hstring smartChargeTip = !state.hasChargeLimit
        ? hstring{L"正在读取充电保护状态"}
        : (state.smartCharge
              ? hstring{L"智能充电已开启（连续接电 72 小时后维持 70%）"}
              : hstring{L"手动充电上限 " +
                        to_hstring(static_cast<unsigned>(state.chargeLimitPercent)) + L"%"});
    ToolTipService::SetToolTip(SmartChargeTile(), winrt::box_value(smartChargeTip));

    const bool saverActive = state.powerOverlay.energySaverActive;
    PowerEnergySaverBtn().IsEnabled(true);
    PowerEnergySaverBtn().IsChecked(saverActive);
    EnergySaverTile().IsEnabled(true);
    EnergySaverTile().IsChecked(saverActive);

    const bool ctAvailable = state.trayConnected || state.colorTempAvailable;
    const bool ctOn = state.colorTemperatureKelvin > 0;
    ColorTempToggle().IsEnabled(ctAvailable);
    ColorTempToggle().IsChecked(ctOn);
    if (!m_colorTempTimer || !m_colorTempTimer.IsRunning()) {
        m_lastSliderKelvin = SnapColorTempKelvin(static_cast<double>(
            ctOn ? state.colorTemperatureKelvin : state.colorTemperatureSliderKelvin));
        ColorTempSlider().Value(static_cast<double>(m_lastSliderKelvin));
        ColorTempValueText().Text(
            ctOn ? (to_hstring(m_lastSliderKelvin) + L"K") : L"关闭");
    }
    ColorTempSlider().IsEnabled(ctAvailable && ctOn);

    if (m_batteryProfileSupported) {
        ApplyBatteryProfileSelection(state.batteryProfile);
        BatteryProfileQuietBtn().IsEnabled(true);
        BatteryProfileDailyBtn().IsEnabled(true);
        BatteryProfilePerfBtn().IsEnabled(true);
    }

    if (m_thermalSupported) {
        FlyoutCpuTempText().Text(
            state.hasCpuTemp ? (to_hstring(state.cpuTempC) + L" °C") : hstring{L"—"});
        FlyoutFanRpmText().Text(
            state.hasFanRpm ? (to_hstring(state.fanRpm) + L" RPM") : hstring{L"—"});
        const bool showAuth = state.canAuthorizeHw && !state.hasCpuTemp && !state.hasFanRpm;
        FlyoutAuthHwButton().Visibility(showAuth ? Visibility::Visible : Visibility::Collapsed);
    }

    const hstring saverTag = state.powerOverlay.energySaverActive ? hstring{L"（节能）"} : hstring{};
    if (state.hasHostPower && state.hostPower.batteryPresent && state.hostPower.percent <= 100) {
        FlyoutHostBattery().SetState(true, state.hostPower.percent, state.hostPower.charging);
        FlyoutHostPercentText().Text(to_hstring(static_cast<unsigned>(state.hostPower.percent)) + L"%");

        const bool hasWatt = state.hostPower.powerMilliWatt != 0;
        const std::wstring watts = FormatWatts(state.hostPower.powerMilliWatt);
        if (state.hostPower.charging) {
            FlyoutHostStateText().Text(L"正在充电" + saverTag);
            FlyoutHostWattText().Text(hasWatt ? hstring{L"充电 " + watts} : hstring{});
        } else if (state.hostPower.discharging) {
            FlyoutHostStateText().Text((state.hostPower.acOnline ? L"插电放电" : L"使用电池") + saverTag);
            FlyoutHostWattText().Text(hasWatt ? hstring{L"功耗 " + watts} : hstring{});
        } else if (state.hostPower.acOnline) {
            FlyoutHostStateText().Text(L"已接通电源" + saverTag);
            FlyoutHostWattText().Text(hasWatt ? hstring{L"功耗 " + watts} : hstring{L"外部供电"});
        } else {
            FlyoutHostStateText().Text(L"未在充放电" + saverTag);
            FlyoutHostWattText().Text(hasWatt ? hstring{L"功耗 " + watts} : hstring{});
        }
    } else {
        FlyoutHostBattery().SetState(false, 0, false);
        FlyoutHostPercentText().Text(L"—");
        FlyoutHostStateText().Text(L"本机电源" + saverTag);
        FlyoutHostWattText().Text(L"");
    }

    m_powerFreqBound = state.powerFreqBound;
    const Visibility modeVis =
        state.powerOverlay.supported ? Visibility::Visible : Visibility::Collapsed;
    FlyoutPowerModeDivider().Visibility(modeVis);
    FlyoutPowerModeGrid().Visibility(modeVis);
    if (state.powerOverlay.supported) {
        if (saverActive) {
            PowerModeEfficiencyBtn().IsChecked(false);
            PowerModeBalancedBtn().IsChecked(false);
            PowerModePerformanceBtn().IsChecked(false);
        } else {
            ApplyPowerModeSelection(state.powerOverlay.mode);
        }
        const bool modeInteractive = !saverActive;
        PowerModeEfficiencyBtn().IsEnabled(modeInteractive);
        PowerModeBalancedBtn().IsEnabled(modeInteractive);
        PowerModePerformanceBtn().IsEnabled(modeInteractive);
        if (state.powerFreqBound) {
            ToolTipService::SetToolTip(PowerModeEfficiencyBtn(), box_value(L"最佳能效（P 核 2.0 GHz · E 核 1.5 GHz）"));
            ToolTipService::SetToolTip(PowerModeBalancedBtn(), box_value(L"平衡（P 核 2.8 GHz · E 核 2.2 GHz）"));
            ToolTipService::SetToolTip(PowerModePerformanceBtn(), box_value(L"最佳性能（不限频）"));
        }
    }

    const bool showPen = state.hasState && (
        AccessoryBattery::HasPenBattery(state.status) ||
        (state.status.hasDeviceAttached && state.status.deviceAttached) ||
        (state.status.hasStylusLink && state.status.stylusLinked));
    m_penRowVisible = showPen;
    FlyoutPenRow().Visibility(showPen ? Visibility::Visible : Visibility::Collapsed);
    if (showPen) {
        FlyoutPenName().Text(
            state.status.modelName[0] ? to_hstring(state.status.modelName) : L"HUAWEI M-Pencil");
        const bool penCharging = state.status.hasChargingState && state.status.charging;
        if (AccessoryBattery::HasPenBattery(state.status)) {
            FlyoutPenBattery().SetState(true, state.status.batteryLevel, penCharging);
            FlyoutPenStatus().Visibility(Visibility::Collapsed);
        } else {
            FlyoutPenBattery().SetState(false, 0, false);
            FlyoutPenStatus().Text(
                (state.status.hasDeviceAttached && state.status.deviceAttached)
                    ? L"已吸附"
                    : L"已连接");
            FlyoutPenStatus().Visibility(Visibility::Visible);
        }
    }

    m_kbdRowVisible = kbdDetected;
    FlyoutKeyboardRow().Visibility(kbdDetected ? Visibility::Visible : Visibility::Collapsed);
    if (kbdDetected) {
        FlyoutKeyboardName().Text(
            state.status.kbdModelName[0]
                ? to_hstring(state.status.kbdModelName)
                : L"HUAWEI 智能磁吸键盘");
        const bool kbdCharging = state.status.hasKbdCharging && state.status.kbdCharging;
        if (AccessoryBattery::HasKeyboardBattery(state.status)) {
            FlyoutKeyboardBattery().SetState(true, state.status.kbdBatteryLevel, kbdCharging);
            FlyoutKeyboardStatus().Visibility(Visibility::Collapsed);
        } else {
            FlyoutKeyboardBattery().SetState(false, 0, false);
            FlyoutKeyboardStatus().Text(
                (state.status.hasKbdDetached && state.status.kbdDetached)
                    ? L"无线已连接"
                    : L"已吸附");
            FlyoutKeyboardStatus().Visibility(Visibility::Visible);
        }
    }

    ApplyItemsLayout();
    m_updatingControls = false;
}

void QuickFlyoutWindow::TouchTileClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setTouchProvider) return;
    const bool requested = IsTileChecked(TouchTile());
    if (!m_callbacks.setTouchProvider(requested)) {
        m_updatingControls = true;
        TouchTile().IsChecked(!requested);
        m_updatingControls = false;
    }
}

void QuickFlyoutWindow::EyeComfortTileClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setEyeComfort) return;
    const bool requested = IsTileChecked(EyeComfortTile());
    if (!m_callbacks.setEyeComfort(requested)) {
        m_updatingControls = true;
        EyeComfortTile().IsChecked(!requested);
        m_updatingControls = false;
    }
}

void QuickFlyoutWindow::NaturalColorTileClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setNaturalColor) return;
    const bool requested = IsTileChecked(NaturalColorTile());
    if (!m_callbacks.setNaturalColor(requested)) {
        m_updatingControls = true;
        NaturalColorTile().IsChecked(!requested);
        m_updatingControls = false;
    }
}

void QuickFlyoutWindow::KbdWirelessTileClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setKeyboardWireless) return;
    const bool requested = IsTileChecked(KbdWirelessTile());
    if (!m_callbacks.setKeyboardWireless(requested)) {
        m_updatingControls = true;
        KbdWirelessTile().IsChecked(!requested);
        m_updatingControls = false;
    }
}

void QuickFlyoutWindow::SmartChargeTileClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setSmartCharge) return;
    const bool requested = IsTileChecked(SmartChargeTile());
    if (!m_callbacks.setSmartCharge(requested)) {
        m_updatingControls = true;
        SmartChargeTile().IsChecked(!requested);
        m_updatingControls = false;
        return;
    }
    if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
}

void QuickFlyoutWindow::HandleEnergySaverToggle(ToggleButton const& source) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setEnergySaver) return;
    const bool requested = IsTileChecked(source);
    if (!m_callbacks.setEnergySaver(requested)) {
        m_updatingControls = true;
        source.IsChecked(!requested);
        m_updatingControls = false;
        return;
    }
    if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
    RelayoutWindowHeight();
}

void QuickFlyoutWindow::EnergySaverTileClicked(IInspectable const&, RoutedEventArgs const&) {
    HandleEnergySaverToggle(EnergySaverTile());
}

void QuickFlyoutWindow::PowerEnergySaverClicked(IInspectable const&, RoutedEventArgs const&) {
    HandleEnergySaverToggle(PowerEnergySaverBtn());
}

void QuickFlyoutWindow::PowerModeEfficiencyClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls) return;
    const auto mode = PowerOverlay::Mode::BestEfficiency;
    if (m_callbacks.setPowerMode && m_callbacks.setPowerMode(mode)) {
        m_updatingControls = true;
        ApplyPowerModeSelection(mode);
        m_updatingControls = false;
    } else if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
}

void QuickFlyoutWindow::PowerModeBalancedClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls) return;
    const auto mode = PowerOverlay::Mode::Balanced;
    if (m_callbacks.setPowerMode && m_callbacks.setPowerMode(mode)) {
        m_updatingControls = true;
        ApplyPowerModeSelection(mode);
        m_updatingControls = false;
    } else if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
}

void QuickFlyoutWindow::PowerModePerformanceClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls) return;
    const auto mode = PowerOverlay::Mode::BestPerformance;
    if (m_callbacks.setPowerMode && m_callbacks.setPowerMode(mode)) {
        m_updatingControls = true;
        ApplyPowerModeSelection(mode);
        m_updatingControls = false;
    } else if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
}

void QuickFlyoutWindow::HandleBatteryProfileClick(MateBookBatteryProfile::Profile profile) {
    if (!m_initialized || m_updatingControls) return;
    if (m_callbacks.setBatteryProfile && m_callbacks.setBatteryProfile(profile)) {
        if (m_callbacks.readState) {
            ApplyStateToControls(m_callbacks.readState());
        } else {
            m_updatingControls = true;
            ApplyBatteryProfileSelection(profile);
            m_updatingControls = false;
        }
    } else if (m_callbacks.readState) {
        ApplyStateToControls(m_callbacks.readState());
    }
}

void QuickFlyoutWindow::BatteryProfileQuietClicked(IInspectable const&, RoutedEventArgs const&) {
    HandleBatteryProfileClick(MateBookBatteryProfile::Profile::Quiet);
}

void QuickFlyoutWindow::BatteryProfileDailyClicked(IInspectable const&, RoutedEventArgs const&) {
    HandleBatteryProfileClick(MateBookBatteryProfile::Profile::Daily);
}

void QuickFlyoutWindow::BatteryProfilePerfClicked(IInspectable const&, RoutedEventArgs const&) {
    HandleBatteryProfileClick(MateBookBatteryProfile::Profile::Performance);
}

void QuickFlyoutWindow::ColorTempToggleClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls || !m_callbacks.setColorTemperature) return;
    const bool enabled = IsTileChecked(ColorTempToggle());
    const int targetKelvin = enabled
        ? SnapColorTempKelvin(ColorTempSlider().Value())
        : 0;
    if (!m_callbacks.setColorTemperature(targetKelvin)) {
        m_updatingControls = true;
        ColorTempToggle().IsChecked(!enabled);
        m_updatingControls = false;
        return;
    }
    ColorTempSlider().IsEnabled(enabled);
    ColorTempValueText().Text(enabled ? (to_hstring(targetKelvin) + L"K") : L"关闭");
}

void QuickFlyoutWindow::ColorTempSliderChanged(
        IInspectable const&,
        RangeBaseValueChangedEventArgs const& args) {
    if (!m_initialized || m_updatingControls) return;
    const int kelvin = SnapColorTempKelvin(args.NewValue());
    m_lastSliderKelvin = kelvin;
    m_pendingColorTemperature = kelvin;
    if (IsTileChecked(ColorTempToggle())) {
        ColorTempValueText().Text(to_hstring(kelvin) + L"K");
    }
    m_colorTempTimer.Stop();
    m_colorTempTimer.Start();
}

void QuickFlyoutWindow::EditFlyoutClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_shutdown) return;
    m_editMode = !m_editMode;
    if (EditFlyoutButton().XamlRoot()) {
        EditFlyoutButton().Focus(FocusState::Programmatic);
    }
    ApplyItemsLayout();
    if (!m_editMode) {
        FocusDefaultElement();
    }
    RelayoutWindowHeight();
}

void QuickFlyoutWindow::EditItemCheckClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_updatingControls) return;
    uint32_t mask = QuickFlyoutItems::kConfiguredBit |
                    QuickFlyoutItems::kSmartChargeDefaultMigratedBit;
    if (m_hideTouchTile) {
        mask |= QuickFlyoutItems::kMateBookEDefaultMigratedBit |
                QuickFlyoutItems::kBatteryProfileMigratedBit;
    }
    const auto isChecked = [](CheckBox const& box) {
        return box.IsChecked() && box.IsChecked().Value();
    };
    if (!m_hideTouchTile && isChecked(EditCheckTouch())) mask |= QuickFlyoutItems::kTouchTile;
    if (isChecked(EditCheckEyeComfort())) mask |= QuickFlyoutItems::kEyeComfortTile;
    if (m_naturalColorSupported && isChecked(EditCheckNaturalColor())) mask |= QuickFlyoutItems::kNaturalColorTile;
    if (isChecked(EditCheckSmartCharge())) mask |= QuickFlyoutItems::kSmartChargeTile;
    if (isChecked(EditCheckKbdWireless())) mask |= QuickFlyoutItems::kKbdWirelessTile;
    if (isChecked(EditCheckEnergySaver())) mask |= QuickFlyoutItems::kEnergySaverTile;
    if (isChecked(EditCheckColorTemp())) mask |= QuickFlyoutItems::kColorTempSlider;
    if (isChecked(EditCheckHostPower())) mask |= QuickFlyoutItems::kHostPowerCard;
    if (m_batteryProfileSupported && isChecked(EditCheckBatteryProfile())) mask |= QuickFlyoutItems::kBatteryProfileRow;
    if (m_thermalSupported && isChecked(EditCheckThermal())) mask |= QuickFlyoutItems::kThermalRow;
    if (isChecked(EditCheckAccessories())) mask |= QuickFlyoutItems::kAccessories;

    m_itemsMask = mask;
    if (m_callbacks.saveItemsMask) {
        m_callbacks.saveItemsMask(m_itemsMask);
    }
}

void QuickFlyoutWindow::ResetFlyoutItemsClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized) return;
    m_itemsMask = m_hideTouchTile ? QuickFlyoutItems::kMateBookEDefaultMask
                                  : QuickFlyoutItems::kDefaultMask;
    SyncEditCheckBoxes();
    if (m_callbacks.saveItemsMask) {
        m_callbacks.saveItemsMask(m_itemsMask);
    }
}

void QuickFlyoutWindow::AuthorizeHardwareClicked(IInspectable const&, RoutedEventArgs const&) {
    if (!m_initialized || m_shutdown) return;
    auto auth = m_callbacks.authorizeHardware;
    if (auth) auth();
}

void QuickFlyoutWindow::FlushPendingColorTemperature() {
    if (m_shutdown || !IsTileChecked(ColorTempToggle()) || !m_callbacks.setColorTemperature) {
        return;
    }
    (void)m_callbacks.setColorTemperature(m_pendingColorTemperature);
}

void QuickFlyoutWindow::OpenSettingsClicked(IInspectable const&, RoutedEventArgs const&) {
    auto open = m_callbacks.openSettings;
    HideFlyout();
    if (open) open();
}

void QuickFlyoutWindow::FlyoutRootKeyDown(
        IInspectable const&,
        Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    if (args.Key() == Windows::System::VirtualKey::Escape) {
        args.Handled(true);
        if (m_editMode) {
            m_editMode = false;
            if (EditFlyoutButton().XamlRoot()) {
                EditFlyoutButton().Focus(FocusState::Programmatic);
            }
            ApplyItemsLayout();
            FocusDefaultElement();
            RelayoutWindowHeight();
            return;
        }
        HideFlyout();
    }
}

} // namespace winrt::EGoTouchSettings::implementation
