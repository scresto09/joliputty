/*
 * wui-flyout.h: doing what was chosen from a menu once the menu has
 * closed, for winui-host.cpp and winui-dialog.cpp (so C++ only).
 *
 * A MenuFlyoutItem's Click comes while its menu is still open, and the
 * menu then takes a moment to close. Something done in the meantime
 * that opens a ContentDialog, or puts the focus somewhere (as renaming
 * a tab does), can have the menu's closing undo it: the dialog is
 * dismissed at once, or the focus is taken back. Waiting a turn of the
 * event loop isn't always enough; waiting for the menu's Closed is.
 */

#ifndef PUTTY_WINUI_WUI_FLYOUT_H
#define PUTTY_WINUI_WUI_FLYOUT_H

#include <functional>
#include <memory>

/* Do 'action' once 'menu' has closed (or now, if it has already, or has
 * gone). The menu is referred to weakly, as its items, whose Click this
 * is for, belong to it. */
inline void wui_after_flyout(
    winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::Primitives::
                    FlyoutBase> const &menu,
    std::function<void()> action)
{
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase;
    auto f = menu.get();
    if (!f || !f.IsOpen()) {
        action();
        return;
    }
    /* (just once: the handler takes itself off) */
    auto token = std::make_shared<winrt::event_token>();
    *token = f.Closed([token, action](
                          winrt::Windows::Foundation::IInspectable const &
                          sender, auto &&) {
        sender.as<FlyoutBase>().Closed(*token);
        action();
    });
}

#endif
