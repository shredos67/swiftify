#include "system/windows/WindowsIntegration.h"
#include "system/MediaTimeline.h"

#include "playback/PlaybackController.h"
#include "theme/Theme.h"

#include <QApplication>
#include <QImage>
#include <QPlatformSurfaceEvent>
#include <QQuickWindow>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QWindow>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>

#ifdef SWIFTIFY_HAVE_SMTC
// mingw-w64's windows.foundation.h specialises IReference for BYTE and for
// boolean, which it also makes an unsigned char; nothing here uses either
#ifdef __MINGW32__
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <functional>

namespace swiftify {

namespace {

constexpr UINT kPrevious = 1;
constexpr UINT kPlayPause = 2;
constexpr UINT kNext = 3;
constexpr DWORD kImmersiveDarkMode = 20;
constexpr DWORD kCornerPreference = 33;
constexpr DWORD kBorderColor = 34;
constexpr DWORD kCaptionColor = 35;
constexpr DWORD kTextColor = 36;

COLORREF nativeColor(const QColor &color)
{
    return RGB(color.red(), color.green(), color.blue());
}

#ifdef SWIFTIFY_HAVE_SMTC
// the media buttons' handler, written out by hand: wrl's Callback lives in
// <wrl/event.h>, which only msvc's sdk ships, and mingw has to build this too
using ButtonPressedHandler = ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Media::SystemMediaTransportControls *,
    ABI::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs *>;

class ButtonPressedCallback final : public ButtonPressedHandler
{
public:
    using Function = std::function<void(ABI::Windows::Media::SystemMediaTransportControlsButton)>;

    explicit ButtonPressedCallback(Function function)
        : m_function(std::move(function))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAgileObject)
            || IsEqualIID(riid, __uuidof(ButtonPressedHandler))) {
            *object = static_cast<ButtonPressedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_references;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ABI::Windows::Media::ISystemMediaTransportControls *,
                                     ABI::Windows::Media::ISystemMediaTransportControlsButtonPressedEventArgs *args) override
    {
        ABI::Windows::Media::SystemMediaTransportControlsButton button;
        if (args && SUCCEEDED(args->get_Button(&button))) {
            m_function(button);
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_references {1};
    Function m_function;
};
#endif

} // namespace

struct WindowsIntegration::Native {
    ITaskbarList3 *taskbar = nullptr;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    bool ownsCom = false;
    bool buttonsAdded = false;
    QSet<quintptr> readyWindows;
    HWND handle = nullptr;
    std::array<HICON, 4> icons {};

#ifdef SWIFTIFY_HAVE_SMTC
    bool ownsWinRt = false;
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControls> controls;
#ifdef SWIFTIFY_HAVE_SMTC_TIMELINE
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControls2> timelineControls;
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControlsTimelineProperties> timeline;
#endif
    EventRegistrationToken buttonToken {};
#endif

    Native()
    {
        ownsCom = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
#ifdef SWIFTIFY_HAVE_SMTC
        ownsWinRt = SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED));
#endif
        const std::array<QStyle::StandardPixmap, 4> symbols {
            QStyle::SP_MediaSkipBackward, QStyle::SP_MediaPlay,
            QStyle::SP_MediaPause, QStyle::SP_MediaSkipForward
        };
        for (size_t index = 0; index < symbols.size(); ++index) {
            icons[index] = QApplication::style()->standardIcon(symbols[index]).pixmap(24).toImage().toHICON();
        }
    }

    void cleanupMediaControls()
    {
#ifdef SWIFTIFY_HAVE_SMTC
        if (controls) {
            if (buttonToken.value != 0) {
                controls->remove_ButtonPressed(buttonToken);
                buttonToken.value = 0;
            }
            controls->put_IsEnabled(false);
#ifdef SWIFTIFY_HAVE_SMTC_TIMELINE
            timeline.Reset();
            timelineControls.Reset();
#endif
            controls.Reset();
        }
#endif
    }

    ~Native()
    {
        if (handle) RemoveWindowSubclass(handle, &Native::windowProcedure, reinterpret_cast<UINT_PTR>(this));
        if (taskbar) {
            taskbar->Release();
        }
        for (HICON icon : icons) {
            if (icon) DestroyIcon(icon);
        }
        cleanupMediaControls();
#ifdef SWIFTIFY_HAVE_SMTC
        if (ownsWinRt) RoUninitialize();
#endif
        if (ownsCom) CoUninitialize();
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR identity, DWORD_PTR context)
    {
        auto *integration = reinterpret_cast<WindowsIntegration *>(context);
        if (message == WM_THEMECHANGED || message == WM_DWMCOMPOSITIONCHANGED || message == WM_STYLECHANGED) {
            QTimer::singleShot(0, integration, &WindowsIntegration::styleWindow);
        }
        if (integration->m_window && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION)) {
            if (message == WM_NCCALCSIZE) {
                if (IsZoomed(window)) {
                    auto *bounds = wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam)->rgrc[0]
                                         : reinterpret_cast<RECT *>(lParam);
                    MONITORINFO monitor {sizeof(MONITORINFO)};
                    if (GetMonitorInfoW(MonitorFromRect(bounds, MONITOR_DEFAULTTONEAREST), &monitor)) {
                        *bounds = monitor.rcWork;
                    }
                }
                return 0;
            }
            if (message == WM_NCHITTEST && !IsZoomed(window) && integration->m_window->visibility() != QWindow::FullScreen) {
                RECT bounds {};
                if (GetWindowRect(window, &bounds)) {
                    using WindowDpi = UINT (WINAPI *)(HWND);
                    using MetricsForDpi = int (WINAPI *)(int, UINT);
                    static const auto windowDpi = reinterpret_cast<WindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
                    static const auto metricsForDpi = reinterpret_cast<MetricsForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
                    const UINT dpi = windowDpi ? windowDpi(window) : qRound(96 * integration->m_window->devicePixelRatio());
                    const int border = metricsForDpi
                        ? metricsForDpi(SM_CXSIZEFRAME, dpi) + metricsForDpi(SM_CXPADDEDBORDER, dpi)
                        : GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    const int horizontal = GET_X_LPARAM(lParam);
                    const int vertical = GET_Y_LPARAM(lParam);
                    const bool left = horizontal >= bounds.left && horizontal < bounds.left + border;
                    const bool right = horizontal < bounds.right && horizontal >= bounds.right - border;
                    const bool top = vertical >= bounds.top && vertical < bounds.top + border;
                    const bool bottom = vertical < bounds.bottom && vertical >= bounds.bottom - border;
                    if (top && left) return HTTOPLEFT;
                    if (top && right) return HTTOPRIGHT;
                    if (bottom && left) return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                }
            }
        }
        if (message == WM_COMMAND && HIWORD(wParam) == THBN_CLICKED) {
            MSG native {};
            native.hwnd = window;
            native.message = message;
            native.wParam = wParam;
            native.lParam = lParam;
            qintptr result = 0;
            if (integration->nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), &native, &result)) return result;
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, &Native::windowProcedure, identity);
            integration->m_native->cleanupMediaControls();
            integration->m_native->handle = nullptr;
            integration->m_native->buttonsAdded = false;
            integration->m_native->readyWindows.remove(reinterpret_cast<quintptr>(window));
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
};

void WindowsIntegration::initializeProcess()
{
    SetCurrentProcessExplicitAppUserModelID(L"dev.addenator.Swiftify.Qt");
}

void WindowsIntegration::setNativeTitleBar(QWindow *window, bool enabled)
{
    if (!window) return;
    const Qt::WindowFlags flags = enabled
        ? Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint
        : Qt::Window | Qt::FramelessWindowHint;
    if (window->flags() == flags) return;
    if (!window->handle() || QGuiApplication::platformName() != QLatin1StringView("windows")) {
        window->setFlags(flags);
        return;
    }
    WINDOWPLACEMENT placement {sizeof(WINDOWPLACEMENT)};
    const bool placed = GetWindowPlacement(reinterpret_cast<HWND>(window->winId()), &placement);
    const auto visibility = window->visibility();
    const auto states = window->windowStates();
    const bool active = window->isActive();
    window->hide();
    window->destroy();
    window->setWindowStates(Qt::WindowNoState);
    window->setFlags(flags);
    window->create();
    if (placed) {
        placement.showCmd = SW_HIDE;
        SetWindowPlacement(reinterpret_cast<HWND>(window->winId()), &placement);
    }
    window->setWindowStates(states);
    window->setVisibility(visibility);
    if (active && window->isVisible()) window->requestActivate();
}

WindowsIntegration::WindowsIntegration(PlaybackController *player, Theme *theme, QObject *parent)
    : QObject(parent), m_player(player), m_theme(theme)
{
    if (QGuiApplication::platformName() != QLatin1StringView("windows")) {
        return;
    }
    m_native = std::make_unique<Native>();
    qApp->installNativeEventFilter(this);
    connect(m_theme, &Theme::schemeChanged, this, &WindowsIntegration::styleWindow);
    connect(m_player, &PlaybackController::playingChanged, this, &WindowsIntegration::updateButtons);
    connect(m_player, &PlaybackController::playingChanged, this, &WindowsIntegration::updateMediaControls);
    connect(m_player, &PlaybackController::queueChanged, this, &WindowsIntegration::updateButtons);
    connect(m_player, &PlaybackController::queueChanged, this, &WindowsIntegration::updateMediaControls);
    connect(m_player, &PlaybackController::currentSongChanged, this, &WindowsIntegration::updateButtons);
    connect(m_player, &PlaybackController::currentSongChanged, this, &WindowsIntegration::updateMediaControls);
    connect(m_player, &PlaybackController::currentSongChanged, this, &WindowsIntegration::updateMediaMetadata);
    connect(m_player, &PlaybackController::mediaStateChanged, this, &WindowsIntegration::updateMediaControls);
    connect(m_player, &PlaybackController::mediaStateChanged, this, &WindowsIntegration::updateMediaMetadata);
    connect(m_player, &PlaybackController::loadingChanged, this, &WindowsIntegration::updateProgress);
    connect(m_player, &PlaybackController::livePositionChanged, this, &WindowsIntegration::updateProgress);
    connect(m_player, &PlaybackController::durationChanged, this, &WindowsIntegration::updateProgress);
    connect(m_player, &PlaybackController::durationChanged, this, &WindowsIntegration::updateMediaTimeline);
    // mediaStateChanged already covers the one-second clock, seeks and pauses.
    connect(m_player, &PlaybackController::mediaStateChanged, this, &WindowsIntegration::updateMediaTimeline);
}

WindowsIntegration::~WindowsIntegration()
{
    if (m_native) {
        qApp->removeNativeEventFilter(this);
        m_native->cleanupMediaControls();
        if (m_window && m_native->taskbar) {
            m_native->taskbar->SetProgressState(reinterpret_cast<HWND>(m_window->winId()), TBPF_NOPROGRESS);
        }
    }
}

void WindowsIntegration::setWindow(QWindow *window)
{
    if (m_window == window || !m_native) {
        return;
    }
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    if (m_native->handle) {
        RemoveWindowSubclass(m_native->handle, &Native::windowProcedure, reinterpret_cast<UINT_PTR>(m_native.get()));
        m_native->cleanupMediaControls();
        m_native->handle = nullptr;
    }
    m_window = window;
    m_native->buttonsAdded = false;
    if (!m_window) return;
    m_window->installEventFilter(this);
    m_window->create();
    attachWindow();
    connect(m_window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (visible) {
            styleWindow();
            QTimer::singleShot(0, this, &WindowsIntegration::initializeTaskbar);
            QTimer::singleShot(0, this, &WindowsIntegration::initializeMediaControls);
        }
    });
    if (auto *quick = qobject_cast<QQuickWindow *>(m_window.data())) {
        connect(quick, &QQuickWindow::colorChanged, this, &WindowsIntegration::styleWindow);
    }
    styleWindow();
    QTimer::singleShot(0, this, &WindowsIntegration::initializeTaskbar);
    QTimer::singleShot(0, this, &WindowsIntegration::initializeMediaControls);
}

void WindowsIntegration::attachWindow()
{
    if (!m_window || !m_native || !m_window->handle()) return;
    const HWND handle = reinterpret_cast<HWND>(m_window->winId());
    if (m_native->handle == handle) return;
    if (m_native->handle) {
        RemoveWindowSubclass(m_native->handle, &Native::windowProcedure, reinterpret_cast<UINT_PTR>(m_native.get()));
    }
    m_native->handle = nullptr;
    m_native->buttonsAdded = false;
    if (SetWindowSubclass(handle, &Native::windowProcedure, reinterpret_cast<UINT_PTR>(m_native.get()),
                          reinterpret_cast<DWORD_PTR>(this))) {
        m_native->handle = handle;
    }
}

bool WindowsIntegration::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::PlatformSurface) {
        const auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
        if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
            styleWindow();
            QTimer::singleShot(0, this, &WindowsIntegration::initializeTaskbar);
            QTimer::singleShot(0, this, &WindowsIntegration::initializeMediaControls);
        }
    }
    return QObject::eventFilter(watched, event);
}

void WindowsIntegration::styleWindow()
{
    if (!m_window || !m_native || !m_window->handle()) return;
    attachWindow();
    const HWND handle = reinterpret_cast<HWND>(m_window->winId());
    if (m_window->flags().testFlag(Qt::FramelessWindowHint) && m_window->visibility() != QWindow::FullScreen) {
        const LONG_PTR current = GetWindowLongPtrW(handle, GWL_STYLE);
        const LONG_PTR desired = (current | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU) & ~WS_CAPTION;
        if (current != desired) {
            SetWindowLongPtrW(handle, GWL_STYLE, desired);
            SetWindowPos(handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    const auto *quick = qobject_cast<QQuickWindow *>(m_window.data());
    const QColor background = quick ? quick->color() : m_theme->colors()->layer0();
    const BOOL dark = background.lightnessF() < 0.5;
    const DWORD corners = 2;
    const COLORREF border = 0xfffffffe;
    const COLORREF caption = nativeColor(background);
    const COLORREF text = dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    DwmSetWindowAttribute(handle, kImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(handle, kCornerPreference, &corners, sizeof(corners));
    DwmSetWindowAttribute(handle, kBorderColor, &border, sizeof(border));
    DwmSetWindowAttribute(handle, kCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(handle, kTextColor, &text, sizeof(text));
}

void WindowsIntegration::initializeTaskbar()
{
    if (!m_window || !m_native || !m_window->isVisible()) return;
    if (!m_native->readyWindows.contains(quintptr(m_window->winId()))) return;
    if (!m_native->taskbar) {
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITaskbarList3, reinterpret_cast<void **>(&m_native->taskbar)))) {
            return;
        }
        if (FAILED(m_native->taskbar->HrInit())) {
            m_native->taskbar->Release();
            m_native->taskbar = nullptr;
            return;
        }
    }
    updateButtons();
}

void WindowsIntegration::updateButtons()
{
    if (!m_window || !m_native || !m_native->taskbar || !m_window->isVisible()) return;
    const HWND handle = reinterpret_cast<HWND>(m_window->winId());
    std::array<THUMBBUTTON, 3> buttons {};
    const std::array<HICON, 3> icons {
        m_native->icons[0], m_native->icons[m_player->isPlaying() ? 2 : 1], m_native->icons[3]
    };
    const std::array<const wchar_t *, 3> tips {
        L"Previous", m_player->isPlaying() ? L"Pause" : L"Play", L"Next"
    };
    const std::array<bool, 3> enabled {
        m_player->hasSong(), m_player->hasSong(), m_player->canSkipForward()
    };
    for (size_t index = 0; index < buttons.size(); ++index) {
        buttons[index].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[index].iId = UINT(index) + 1;
        buttons[index].hIcon = icons[index];
        buttons[index].dwFlags = enabled[index] ? THBF_ENABLED : THBF_DISABLED;
        std::wcsncpy(buttons[index].szTip, tips[index], std::size(buttons[index].szTip) - 1);
    }
    if (!m_native->buttonsAdded) {
        m_native->buttonsAdded = SUCCEEDED(m_native->taskbar->ThumbBarAddButtons(handle, UINT(buttons.size()), buttons.data()));
    } else {
        m_native->taskbar->ThumbBarUpdateButtons(handle, UINT(buttons.size()), buttons.data());
    }
    const Song song = m_player->currentSong();
    const QString tooltip = m_player->hasSong() ? song.name + QStringLiteral(" — ") + song.artists : QStringLiteral("Swiftify");
    m_native->taskbar->SetThumbnailTooltip(handle, reinterpret_cast<LPCWSTR>(tooltip.utf16()));
    updateProgress();
}

void WindowsIntegration::updateProgress()
{
    if (!m_window || !m_native || !m_native->taskbar || !m_window->isVisible()) return;
    const HWND handle = reinterpret_cast<HWND>(m_window->winId());
    if (!m_player->hasSong()) {
        m_native->taskbar->SetProgressState(handle, TBPF_NOPROGRESS);
    } else if (m_player->isLoading()) {
        m_native->taskbar->SetProgressState(handle, TBPF_INDETERMINATE);
    } else if (m_player->durationMs() > 0) {
        m_native->taskbar->SetProgressState(handle, m_player->isPlaying() ? TBPF_NORMAL : TBPF_PAUSED);
        m_native->taskbar->SetProgressValue(handle,
            std::min<quint64>(m_player->livePositionMs(), m_player->durationMs()), m_player->durationMs());
    } else {
        m_native->taskbar->SetProgressState(handle, TBPF_NOPROGRESS);
    }
}

bool WindowsIntegration::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (!m_native || (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")) return false;
    const auto *native = static_cast<MSG *>(message);
    if (m_native->taskbarCreated && native->message == m_native->taskbarCreated) {
        m_native->readyWindows.insert(reinterpret_cast<quintptr>(native->hwnd));
        if (!m_window || !m_window->handle() || native->hwnd != reinterpret_cast<HWND>(m_window->winId())) return false;
        m_native->buttonsAdded = false;
        if (m_native->taskbar) {
            m_native->taskbar->Release();
            m_native->taskbar = nullptr;
        }
        initializeTaskbar();
        return false;
    }
    if (!m_window || !m_window->handle() || native->hwnd != reinterpret_cast<HWND>(m_window->winId())) return false;
    if (native->message == WM_COMMAND && HIWORD(native->wParam) == THBN_CLICKED) {
        switch (LOWORD(native->wParam)) {
        case kPrevious: m_player->skipBackward(); break;
        case kPlayPause: m_player->togglePlayback(); break;
        case kNext: m_player->skipForward(); break;
        default: return false;
        }
        if (result) *result = 0;
        return true;
    }
    return false;
}

#ifdef SWIFTIFY_HAVE_SMTC
void WindowsIntegration::initializeMediaControls()
{
    if (!m_window || !m_native || !m_window->handle()) return;
    const HWND handle = reinterpret_cast<HWND>(m_window->winId());
    if (m_native->controls) return;

    using namespace ABI::Windows::Media;
    using namespace ABI::Windows::Foundation;
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Wrappers::HStringReference;

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HStringReference smtcId(RuntimeClass_Windows_Media_SystemMediaTransportControls);
    if (FAILED(RoGetActivationFactory(smtcId.Get(), IID_PPV_ARGS(&interop))) || !interop) {
        return;
    }

    if (FAILED(interop->GetForWindow(handle, IID_PPV_ARGS(&m_native->controls))) || !m_native->controls) {
        return;
    }

    m_native->controls->put_IsEnabled(true);
    m_native->controls->put_IsPlayEnabled(true);
    m_native->controls->put_IsPauseEnabled(true);
    m_native->controls->put_IsStopEnabled(true);
    m_native->controls->put_IsNextEnabled(true);
    m_native->controls->put_IsPreviousEnabled(true);

    m_native->buttonToken.value = 0;
    // the buttons are pressed on a thread of windows' own; the press is
    // queued onto this one
    ComPtr<ButtonPressedHandler> handler;
    handler.Attach(new ButtonPressedCallback([this](SystemMediaTransportControlsButton button) {
        QMetaObject::invokeMethod(this, [this, button] {
            handleMediaButton(int(button));
        }, Qt::QueuedConnection);
    }));
    m_native->controls->add_ButtonPressed(handler.Get(), &m_native->buttonToken);

#ifdef SWIFTIFY_HAVE_SMTC_TIMELINE
    ComPtr<IInspectable> timeline;
    HStringReference timelineId(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties);
    if (SUCCEEDED(m_native->controls.As(&m_native->timelineControls))
        && SUCCEEDED(RoActivateInstance(timelineId.Get(), &timeline)) && timeline) {
        timeline.As(&m_native->timeline);
    }
#endif
    updateMediaMetadata();
    updateMediaControls();
    updateMediaTimeline();
}

void WindowsIntegration::handleMediaButton(int button)
{
    using namespace ABI::Windows::Media;
    switch (button) {
    case SystemMediaTransportControlsButton_Play:
        m_player->resume();
        break;
    case SystemMediaTransportControlsButton_Pause:
    case SystemMediaTransportControlsButton_Stop:
        if (m_player->isPlaying() || m_player->isLoading()) {
            m_player->pause();
        }
        break;
    case SystemMediaTransportControlsButton_Next:
        m_player->skipForward();
        break;
    case SystemMediaTransportControlsButton_Previous:
        m_player->skipBackward();
        break;
    default:
        break;
    }
}

void WindowsIntegration::updateMediaControls()
{
    if (!m_native || !m_native->controls) return;
    using namespace ABI::Windows::Media;
    if (!m_player->hasSong()) {
        m_native->controls->put_PlaybackStatus(MediaPlaybackStatus_Closed);
        m_native->controls->put_IsPlayEnabled(false);
        m_native->controls->put_IsPauseEnabled(false);
        m_native->controls->put_IsNextEnabled(false);
        m_native->controls->put_IsPreviousEnabled(false);
    } else {
        m_native->controls->put_PlaybackStatus(m_player->isPlaying() ? MediaPlaybackStatus_Playing : MediaPlaybackStatus_Paused);
        m_native->controls->put_IsPlayEnabled(true);
        m_native->controls->put_IsPauseEnabled(true);
        m_native->controls->put_IsNextEnabled(m_player->canSkipForward());
        m_native->controls->put_IsPreviousEnabled(m_player->canSkipBackward());
    }
}

void WindowsIntegration::updateMediaMetadata()
{
    if (!m_native || !m_native->controls) return;
    using namespace ABI::Windows::Media;
    using namespace ABI::Windows::Foundation;
    using namespace ABI::Windows::Storage::Streams;
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Wrappers::HString;
    using Microsoft::WRL::Wrappers::HStringReference;

    ComPtr<ISystemMediaTransportControlsDisplayUpdater> updater;
    if (FAILED(m_native->controls->get_DisplayUpdater(&updater)) || !updater) {
        return;
    }

    if (!m_player->hasSong()) {
        updater->ClearAll();
        updater->Update();
        return;
    }

    const Song song = m_player->currentSong();
    updater->put_Type(MediaPlaybackType_Music);

    ComPtr<IMusicDisplayProperties> music;
    if (SUCCEEDED(updater->get_MusicProperties(&music)) && music) {
        HString title;
        title.Set(reinterpret_cast<const wchar_t *>(song.name.utf16()), UINT32(song.name.length()));
        music->put_Title(title.Get());

        HString artist;
        artist.Set(reinterpret_cast<const wchar_t *>(song.artists.utf16()), UINT32(song.artists.length()));
        music->put_Artist(artist.Get());

        // the album title came in the interface's second version
        ComPtr<IMusicDisplayProperties2> music2;
        if (!song.albumName.isEmpty() && SUCCEEDED(music.As(&music2)) && music2) {
            HString album;
            album.Set(reinterpret_cast<const wchar_t *>(song.albumName.utf16()), UINT32(song.albumName.length()));
            music2->put_AlbumTitle(album.Get());
        }
    }

    if (song.artworkUrl.isValid()) {
        ComPtr<IUriRuntimeClassFactory> uriFactory;
        HStringReference uriClassName(RuntimeClass_Windows_Foundation_Uri);
        if (SUCCEEDED(RoGetActivationFactory(uriClassName.Get(), IID_PPV_ARGS(&uriFactory))) && uriFactory) {
            const QString urlStr = song.artworkUrl.toString();
            HString uriHStr;
            uriHStr.Set(reinterpret_cast<const wchar_t *>(urlStr.utf16()), UINT32(urlStr.length()));
            ComPtr<IUriRuntimeClass> uri;
            if (SUCCEEDED(uriFactory->CreateUri(uriHStr.Get(), &uri)) && uri) {
                ComPtr<IRandomAccessStreamReferenceStatics> streamStatics;
                HStringReference streamClassName(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference);
                if (SUCCEEDED(RoGetActivationFactory(streamClassName.Get(), IID_PPV_ARGS(&streamStatics))) && streamStatics) {
                    ComPtr<IRandomAccessStreamReference> thumbnail;
                    if (SUCCEEDED(streamStatics->CreateFromUri(uri.Get(), &thumbnail)) && thumbnail) {
                        updater->put_Thumbnail(thumbnail.Get());
                    }
                }
            }
        }
    }

    updater->Update();
}
void WindowsIntegration::updateMediaTimeline()
{
#ifdef SWIFTIFY_HAVE_SMTC_TIMELINE
    if (!m_native || !m_native->timelineControls || !m_native->timeline) return;
    const auto value = MediaTimeline::fromMilliseconds(m_player->hasSong(), m_player->durationMs(),
        m_player->currentSong().durationMs, m_player->livePositionMs());
    ABI::Windows::Foundation::TimeSpan zero {};
    ABI::Windows::Foundation::TimeSpan end {value.end};
    ABI::Windows::Foundation::TimeSpan position {value.position};
    auto *timeline = m_native->timeline.Get();
    // Reset dependent bounds before shortening a track (or clearing a session).
    if (SUCCEEDED(timeline->put_Position(zero)) && SUCCEEDED(timeline->put_MaxSeekTime(zero))
        && SUCCEEDED(timeline->put_StartTime(zero)) && SUCCEEDED(timeline->put_EndTime(end))
        && SUCCEEDED(timeline->put_MinSeekTime(zero)) && SUCCEEDED(timeline->put_MaxSeekTime(end))
        && SUCCEEDED(timeline->put_Position(position))) {
        m_native->timelineControls->UpdateTimelineProperties(timeline);
    }
#endif
}
#else
// built without the media controls: the headers had no declarations for them
void WindowsIntegration::initializeMediaControls() {}
void WindowsIntegration::handleMediaButton(int) {}
void WindowsIntegration::updateMediaControls() {}
void WindowsIntegration::updateMediaMetadata() {}
void WindowsIntegration::updateMediaTimeline() {}
#endif

} // namespace swiftify
