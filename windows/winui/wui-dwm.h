/*
 * wui-dwm.h: showing a XAML window without a flash of white, for
 * winui-host.cpp and winui-dialog.cpp (so C++ only).
 *
 * A XAML window appears before XAML has drawn anything in it, so for
 * a moment it's white, which in the dark theme is a flash. So (as
 * Windows Terminal does) we have the window 'cloaked' (there, but not
 * shown on the screen) until XAML has drawn its first frame. We also
 * tell the window manager which theme the window's frame should be.
 *
 * The window manager's functions are looked up at run time, so that
 * nothing more needs linking.
 */

#ifndef PUTTY_WINUI_WUI_DWM_H
#define PUTTY_WINUI_WUI_DWM_H

#include <chrono>
#include <memory>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#define WUI_DWMWA_USE_IMMERSIVE_DARK_MODE 20

inline void wui_dwm_set(HWND hwnd, DWORD attr, BOOL value)
{
    typedef HRESULT (WINAPI *fn_t)(HWND, DWORD, LPCVOID, DWORD);
    static fn_t fn;
    static bool tried;
    if (!tried) {
        tried = true;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
            fn = (fn_t)(void (*)(void))GetProcAddress(
                dwm, "DwmSetWindowAttribute");
    }
    if (fn)
        fn(hwnd, attr, &value, sizeof(value));
}

/* Whether an element (the window's content) is in the dark theme */
inline bool wui_is_dark(winrt::Microsoft::UI::Xaml::FrameworkElement const &e)
{
    namespace mux = winrt::Microsoft::UI::Xaml;
    switch (e.RequestedTheme()) {
      case mux::ElementTheme::Dark:
        return true;
      case mux::ElementTheme::Light:
        return false;
      default:
        try {
            return mux::Application::Current().RequestedTheme() ==
                mux::ApplicationTheme::Dark;
        } catch (...) {
            return false;
        }
    }
}

inline void wui_dwm_dark_frame(HWND hwnd, bool dark)
{
    wui_dwm_set(hwnd, WUI_DWMWA_USE_IMMERSIVE_DARK_MODE, dark);
}

/* Show a window: cloak it, activate it, and uncloak it once XAML has
 * drawn in it (or after a moment anyway, so that it can't get lost) */
inline void wui_show_without_flash(
    HWND hwnd, winrt::Microsoft::UI::Xaml::Window const &window)
{
    namespace mux = winrt::Microsoft::UI::Xaml;
    auto root = window.Content().try_as<mux::FrameworkElement>();
    if (root)
        wui_dwm_dark_frame(hwnd, wui_is_dark(root));
    wui_dwm_set(hwnd, DWMWA_CLOAK, TRUE);

    auto queue = window.DispatcherQueue();
    auto shown = std::make_shared<bool>(false);
    auto uncloak = [hwnd, shown]() {
        if (!*shown) {
            *shown = true;
            wui_dwm_set(hwnd, DWMWA_CLOAK, FALSE);
            /* (and to the front again, now it can be seen: activating
             * it while it was cloaked doesn't always bring it there,
             * and then it comes up behind whatever was in front) */
            SetForegroundWindow(hwnd);
        }
    };
    /* (A frame is drawn after the Rendering event for it; so after the
     * first one, wait for the queue to come round again) */
    auto token = std::make_shared<winrt::event_token>();
    *token = mux::Media::CompositionTarget::Rendering(
        [token, queue, uncloak](auto &&, auto &&) {
            mux::Media::CompositionTarget::Rendering(*token);
            queue.TryEnqueue(
                winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                uncloak);
        });
    auto timer = queue.CreateTimer();
    timer.Interval(std::chrono::milliseconds(500));
    timer.IsRepeating(false);
    timer.Tick([timer, uncloak](auto &&, auto &&) { uncloak(); });
    timer.Start();

    window.Activate();
}

#endif /* PUTTY_WINUI_WUI_DWM_H */
