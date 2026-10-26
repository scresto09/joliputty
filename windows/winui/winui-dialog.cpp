/*
 * winui-dialog.cpp: the C++/WinRT half of the WinUI 3 PuTTY
 * configuration box, providing the window and widgets described in
 * wuidlg.h. Like winui-host.cpp, it knows nothing about PuTTY.
 *
 * The window is laid out like the Windows 11 Settings app: Mica
 * behind everything, a NavigationView of the top-level categories on
 * the left, and on the right the page, with a breadcrumb trail for a
 * title and a card for each group of settings, with a row for each
 * setting. Bits of chrome that need theme
 * resources (so that they follow the light and dark themes) are
 * built from XAML markup at run time, which needs no XAML compiler.
 *
 * Programmatic changes to a widget's value mustn't be reported back
 * to the C side as if the user had made them. Most XAML controls
 * raise their change events synchronously, so a 'quiet' counter deals
 * with those; but TextBox raises TextChanged later, so for that we
 * note that one is due and swallow it when it comes.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
/* winbase.h's GetCurrentTime macro breaks the XAML animation API */
#undef GetCurrentTime

#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wuidlg.h"
#include "wui-dwm.h"
#include "wui-flyout.h"

namespace wf = winrt::Windows::Foundation;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace muw = winrt::Microsoft::UI::Windowing;
namespace wadt = winrt::Windows::ApplicationModel::DataTransfer;

using winrt::check_hresult;
using winrt::com_ptr;
using winrt::hstring;

/* Size of the window when it opens, in XAML units */
static const double DLG_WIDTH = 1000, DLG_HEIGHT = 720;
/* Height of a row in a list box, or one with buttons in its rows */
static const double LIST_ROW = 32, ACTION_ROW = 48;
/* Width of the controls on the right of setting rows */
static const double CONTROL_WIDTH = 300;

static hstring hs(const wchar_t *s)
{
    return s ? hstring(s) : hstring();
}

static bool has_text(const wchar_t *s)
{
    return s && *s;
}

/* A label as a setting's name: without the colon that ends it in the
 * classic dialog box, where it stood before its control */
static hstring setting_name(const wchar_t *s)
{
    std::wstring t = s ? s : L"";
    while (!t.empty() && (t.back() == L':' || t.back() == L' '))
        t.pop_back();
    return hstring(t);
}

/* ----------------------------------------------------------------------
 * Pieces of XAML markup.
 */

static const wchar_t XMLNS[] =
    L" xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' ";

template <typename T> static T xaml(const wchar_t *tag, const wchar_t *attrs)
{
    std::wstring s = L"<";
    s += tag;
    s += XMLNS;
    s += attrs;
    s += L"/>";
    return mux::Markup::XamlReader::Load(s).as<T>();
}

static muxc::TextBlock styled_text(const wchar_t *style, hstring const &text,
                                   bool secondary = false)
{
    std::wstring attrs = L"Style='{StaticResource ";
    attrs += style;
    attrs += L"}' TextWrapping='Wrap' ";
    if (secondary)
        attrs += L"Foreground='{ThemeResource TextFillColorSecondaryBrush}'";
    auto t = xaml<muxc::TextBlock>(L"TextBlock", attrs.c_str());
    t.Text(text);
    return t;
}

/* The card that holds a group of settings */
static muxc::Border group_card()
{
    return xaml<muxc::Border>(
        L"Border", L"BorderThickness='1' CornerRadius='8' "
        L"Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' "
        L"BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'");
}

static muxm::Brush theme_brush(const wchar_t *name)
{
    return mux::Application::Current().Resources().Lookup(
        winrt::box_value(name)).as<muxm::Brush>();
}

/* A row in a group card, with a line above it if it isn't the first */
static muxc::Border row_border(bool first)
{
    auto row = xaml<muxc::Border>(
        L"Border", L"Padding='24,6,24,6' MinHeight='48' "
        L"BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'");
    row.BorderThickness(mux::ThicknessHelper::FromLengths(0, first ? 0 : 1,
                                                          0, 0));
    return row;
}

/* An icon from the theme's symbol font */
static muxc::FontIcon symbol(unsigned glyph, double size = 0)
{
    muxc::FontIcon icon;
    try {
        icon.FontFamily(mux::Application::Current().Resources().Lookup(
                            winrt::box_value(L"SymbolThemeFontFamily"))
                        .as<muxm::FontFamily>());
    } catch (...) {
        icon.FontFamily(muxm::FontFamily(
                            L"Segoe Fluent Icons,Segoe MDL2 Assets"));
    }
    wchar_t s[2] = { (wchar_t)glyph, L'\0' };
    icon.Glyph(s);
    if (size > 0)
        icon.FontSize(size);
    return icon;
}

/* An image of an icon, drawn at its displayed size in pixels. (Also
 * used for the main window's icon, by winui-host.cpp.) */
muxm::ImageSource wui_icon_image(HICON icon, int px)
{
    if (!icon || px < 1)
        return nullptr;
    com_ptr<IWICImagingFactory> wic;
    com_ptr<IWICBitmap> src;
    com_ptr<IWICBitmapScaler> scaler;
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateBitmapFromHICON(icon, src.put())) ||
        FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(src.get(), px, px,
                                  WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(
                   scaler.get(), GUID_WICPixelFormat32bppPBGRA,
                   WICBitmapDitherTypeNone, nullptr, 0,
                   WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    muxm::Imaging::WriteableBitmap bmp(px, px);
    auto buf = bmp.PixelBuffer();
    if (FAILED(conv->CopyPixels(nullptr, px * 4, buf.Capacity(),
                                buf.data())))
        return nullptr;
    bmp.Invalidate();
    return bmp;
}

/* ----------------------------------------------------------------------
 * Data structures.
 */

enum class Kind {
    Group, Text, Edit, Radio, Check, Button, List, FileSel, FontSel, Steps
};

/* A row of a table of steps (see wui_w_steps) */
struct StepRow {
    muxc::ListViewItem item{nullptr};
    muxc::TextBlock number{nullptr};
    muxc::CheckBox active{nullptr};
    muxc::TextBox expect{nullptr};
    muxc::PasswordBox command{nullptr};
    muxc::CheckBox hide{nullptr};
};

struct WuiWidget {
    WuiDlg *dlg;
    Kind kind;
    void *wctx = nullptr;
    bool footer = false;          /* one of the buttons that end the
                                   * dialog, not on the page */
    bool in_row = false;          /* in a group's one row (see
                                   * wui_dlg_group_one_row) */
    bool stretch = false;         /* ... taking up what's left of it */
    bool flush = false;           /* filling its card edge to edge (see
                                   * wui_dlg_group_flush) */
    bool bare = false;            /* ... or with no card (see
                                   * wui_dlg_group_bare) */

    mux::FrameworkElement root{nullptr};  /* the whole widget */

    /* For a group */
    muxc::StackPanel stack{nullptr};      /* its cards */
    muxc::StackPanel bar{nullptr};        /* the strip of buttons at the
                                           * end, if that's where we are */
    int nrows = 0;                        /* rows in it so far */
    muxc::StackPanel right{nullptr};      /* where the footer's go */
    muxc::Grid one_row{nullptr};          /* the row its settings share,
                                           * if they do */
    muxc::StackPanel tail{nullptr};       /* an edit row's control, and
                                           * any buttons beside it */

    muxc::TextBlock label{nullptr};
    muxc::TextBlock text{nullptr};        /* Text, or FontSel's description */
    muxc::TextBox edit{nullptr};          /* Edit, FileSel, or a combo's */
    muxc::PasswordBox pass{nullptr};
    muxc::NumberBox number{nullptr};
    bool integer = false;                 /* a NumberBox's are whole */
    muxc::ComboBox combo{nullptr};        /* combo box, drop-down list,
                                           * or radio buttons */
    muxc::ListView list{nullptr};
    muxc::ToggleSwitch toggle{nullptr};
    muxc::Button button{nullptr};         /* Button, or a selector's */
    muxc::Flyout flyout{nullptr};         /* a prompt's pop-up */

    std::vector<int> ids;                 /* per list item */
    std::vector<hstring> texts;           /* ... and their text */
    std::vector<std::shared_ptr<StepRow>> steps;  /* a table's rows */
    unsigned item_glyph = 0;              /* an icon in each list item */
    struct RowAction {
        hstring label;
        unsigned glyph;
        bool primary;                     /* a button, not in the menu */
        void *target;
    };
    std::vector<RowAction> actions;       /* for each item of a list */
    /* For rows (see wui_w_rows) */
    struct RowInfo {
        int id;
        WuiRowKind kind;
        bool marked;
        muxc::ListViewItem item{nullptr};  /* (not for a heading) */
        muxc::ListView list{nullptr};      /* the one it's in */
    };
    std::vector<RowInfo> rowinfo;
    muxc::StackPanel sections{nullptr};   /* rows' headings and cards */
    std::vector<muxc::ListView> lists;    /* ... a list in each card */
    struct RowsAction {
        int on;                           /* ROWS_ON_* */
        hstring label, alt_label;
        unsigned glyph, alt_glyph;
        void *target;
        int key;
    };
    std::vector<RowsAction> rows_actions;
    bool draggable = false;               /* rows can be dragged */
    int drag_id = -1;                     /* the row being dragged */
    int drop_what = -1, drop_where = -1;  /* see wui_w_rows_dropped */
    int activated = -1;                   /* see wui_w_rows_activated */
    muxc::TextBlock empty{nullptr};       /* rows' "there aren't any" */
    WuiWidget *next = nullptr;            /* a search box's rows */
    muxc::Button reset{nullptr};          /* a setting's Reset button... */
    muxc::MenuFlyoutItem reset_item{nullptr};  /* ... and menu item */
    muxc::MenuFlyout menu{nullptr};       /* a page menu button's */
    int ncols = 0;                        /* list columns */
    int height = 0;                       /* list rows to show */
    std::vector<int> percentages;

    int quiet = 0;                /* making a programmatic change */
    bool text_pending = false;    /* swallow the next TextChanged */
    bool dead = false;            /* see retire() */

    WuiWidget(WuiDlg *dlg, Kind kind) : dlg(dlg), kind(kind) {}

    /*
     * Once a widget has gone from its page (or its dialog has gone),
     * its XAML controls may still raise events for a while (losing
     * the focus, say), and their handlers point at this. So it stays
     * in existence, marked dead, for the handlers to see and ignore;
     * but it lets go of its controls, so that they can go.
     */
    void retire()
    {
        dead = true;
        root = nullptr;
        stack = bar = right = tail = nullptr;
        one_row = nullptr;
        label = text = nullptr;
        edit = nullptr;
        pass = nullptr;
        number = nullptr;
        combo = nullptr;
        list = nullptr;
        toggle = nullptr;
        button = nullptr;
        flyout = nullptr;
        steps.clear();
        rowinfo.clear();
        sections = nullptr;
        lists.clear();
        empty = nullptr;
        reset = nullptr;
        reset_item = nullptr;
        menu = nullptr;
    }
};

/* Retired widgets (see WuiWidget::retire) */
static std::vector<std::unique_ptr<WuiWidget>> retired;

struct WuiDlg {
    void *ctx;
    HWND owner = nullptr, hwnd = nullptr;
    mux::Window window{nullptr};
    muw::AppWindow appwindow{nullptr};
    muxc::NavigationView nav{nullptr};
    muxc::StackPanel context{nullptr};    /* what's being edited... */
    muxc::FontIcon context_icon{nullptr};
    muxc::TextBlock context_text{nullptr}, context_dot{nullptr},
        context_detail{nullptr};
    muxc::Button help{nullptr};           /* "?", at the title bar's end */
    std::wstring help_url;
    muxc::StackPanel heading{nullptr};    /* the page title, above... */
    /*
     * Beside it, the buttons: in that order, the footer's ordinary ones
     * (Apply, when it's not the main button), the page's own, the main
     * one (the page's or the footer's), and last, the page's menu, as
     * in Windows 11 apps. (See wui_w_set_accent, which moves a button
     * between these.)
     */
    muxc::StackPanel actions{nullptr};
    muxc::StackPanel footer_lead{nullptr}, page_actions{nullptr},
        page_main{nullptr}, footer_main{nullptr}, menu_actions{nullptr};
    muxc::StackPanel page{nullptr};       /* ... the rest of the page */
    muxc::StackPanel subpages{nullptr};   /* rows in the card of them */
    muxc::ScrollViewer scroller{nullptr};
    double scale = 1;

    std::vector<std::unique_ptr<WuiWidget>> widgets;
    std::unordered_map<int, muxc::NavigationViewItem> categories;
    int nav_quiet = 0;

    WuiWidget *default_button = nullptr, *cancel_button = nullptr;

    bool prompt_open = false;             /* (see wui_dlg_prompt) */
    hstring prompt_text;
    hstring choices_label;                /* (see wui_dlg_prompt_choices) */
    std::vector<hstring> choices;
    size_t choices_sel = 0;
    std::optional<std::wstring> prompt_folder;  /* the one chosen */
    std::vector<mud::DispatcherQueueTimer> timers;  /* notices' */
    muxc::InfoBar notice{nullptr};        /* the one showing, if any */

    bool running = false, stop_requested = false, manual_loop = false;
    bool close_ok = false;

    explicit WuiDlg(void *ctx) : ctx(ctx) {}
};

/* Dialogs that exist, so that work queued for later can tell whether
 * its dialog still does */
static std::vector<WuiDlg *> live_dlgs;

static bool is_live(WuiDlg *dlg, WuiWidget *w = nullptr)
{
    if (std::find(live_dlgs.begin(), live_dlgs.end(), dlg) == live_dlgs.end())
        return false;
    if (!w)
        return true;
    for (auto &p : dlg->widgets)
        if (p.get() == w)
            return true;
    return false;
}

/* The parts of the title bar that take the pointer themselves rather
 * than dragging the window: the line saying what's being edited (for
 * its tooltip), and the help button */
static void update_passthrough(WuiDlg *dlg)
{
    if (!is_live(dlg))
        return;
    try {
        namespace mui = winrt::Microsoft::UI::Input;
        auto src = mui::InputNonClientPointerSource::GetForWindowId(
            dlg->appwindow.Id());
        /* (never over the caption buttons, whatever's left over from a
         * layout not finished yet: that would take their clicks) */
        int32_t limit = dlg->appwindow.ClientSize().Width -
            dlg->appwindow.TitleBar().RightInset();
        std::vector<winrt::Windows::Graphics::RectInt32> rects;
        for (mux::FrameworkElement e :
                 { mux::FrameworkElement(dlg->context),
                   mux::FrameworkElement(dlg->help) }) {
            if (!e || e.Visibility() != mux::Visibility::Visible ||
                e.ActualWidth() <= 0)
                continue;
            auto p = e.TransformToVisual(nullptr)
                .TransformPoint(wf::Point{0, 0});
            winrt::Windows::Graphics::RectInt32 r{
                (int32_t)std::lround(p.X * dlg->scale),
                (int32_t)std::lround(p.Y * dlg->scale),
                (int32_t)std::lround(e.ActualWidth() * dlg->scale),
                (int32_t)std::lround(e.ActualHeight() * dlg->scale)};
            if (r.X + r.Width > limit)
                r.Width = limit - r.X;
            if (r.Width > 0)
                rects.push_back(r);
        }
        if (rects.empty())
            src.ClearRegionRects(mui::NonClientRegionKind::Passthrough);
        else
            src.SetRegionRects(mui::NonClientRegionKind::Passthrough,
                               rects);
    } catch (...) {
    }
}

/*
 * An exception escaping from an event handler takes the whole
 * application down, so say what it was instead. (These should never
 * happen; if one does, this at least tells us where to look.)
 */
static void report_exception(WuiDlg *dlg, const wchar_t *what)
{
    std::wstring msg = L"Internal error in the configuration box (";
    msg += what;
    msg += L")";
    try {
        throw;
    } catch (winrt::hresult_error const &e) {
        wchar_t code[16];
        swprintf(code, 16, L"0x%08X", (unsigned)e.code());
        msg += L": ";
        msg += code;
        msg += L" ";
        msg += e.message().c_str();
    } catch (std::exception const &e) {
        std::string m = e.what();
        msg += L": ";
        msg += std::wstring(m.begin(), m.end());
    } catch (...) {
    }
    MessageBoxW(is_live(dlg) ? dlg->hwnd : nullptr, msg.c_str(), WUI_APP_NAME,
                MB_ICONERROR | MB_OK);
}

static void fire(WuiWidget *w, WuiWidgetEvent ev)
{
    if (w->dead)
        return;
    WuiDlg *dlg = w->dlg;
    try {
        wui_dlg_cb_widget(dlg->ctx, w->wctx, ev);
    } catch (...) {
        report_exception(dlg, L"handling a control's event");
    }
}

/*
 * For events whose handlers may well open modal windows (buttons,
 * double-clicks): run them once XAML has finished dispatching the
 * input event, rather than from inside it.
 */
static void fire_later(WuiWidget *w, WuiWidgetEvent ev)
{
    if (w->dead)
        return;
    WuiDlg *dlg = w->dlg;
    dlg->window.DispatcherQueue().TryEnqueue([dlg, w, ev]() {
        if (is_live(dlg, w))
            fire(w, ev);
    });
}

/* Go to a category. (Later, because that replaces the page, which may
 * well contain the thing that was clicked.) */
static void goto_category(WuiDlg *dlg, int id)
{
    if (!is_live(dlg))
        return;
    dlg->window.DispatcherQueue().TryEnqueue([dlg, id]() {
        if (!is_live(dlg))
            return;
        try {
            wui_dlg_cb_category(dlg->ctx, id);
        } catch (...) {
            report_exception(dlg, L"changing page");
        }
    });
}

static WuiWidget *new_widget(WuiWidget *grp, Kind kind)
{
    WuiDlg *dlg = grp->dlg;
    dlg->widgets.push_back(std::make_unique<WuiWidget>(dlg, kind));
    WuiWidget *w = dlg->widgets.back().get();
    w->footer = grp->footer;
    w->in_row = (bool)grp->one_row;
    w->flush = grp->flush;
    return w;
}

/* Note programmatic text changes, to ignore the events they cause */
static void set_box_text(WuiWidget *w, muxc::TextBox box, hstring s)
{
    if (box.Text() != s) {
        w->text_pending = true;
        box.Text(s);
    }
}

/* ----------------------------------------------------------------------
 * The window.
 */

static void centre(WuiDlg *dlg, int w, int h)
{
    HMONITOR mon = MonitorFromWindow(dlg->owner ? dlg->owner : dlg->hwnd,
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return;
    RECT area = mi.rcWork;
    RECT ref = area;
    if (dlg->owner && IsWindowVisible(dlg->owner) && !IsIconic(dlg->owner))
        GetWindowRect(dlg->owner, &ref);
    int x = (ref.left + ref.right - w) / 2;
    int y = (ref.top + ref.bottom - h) / 2;
    x = std::max((int)area.left, std::min(x, (int)area.right - w));
    y = std::max((int)area.top, std::min(y, (int)area.bottom - h));
    dlg->appwindow.Move(winrt::Windows::Graphics::PointInt32{x, y});
}

static void press_button(WuiWidget *b)
{
    if (b && b->button && b->button.IsEnabled())
        fire_later(b, WUI_EV_ACTION);
}

extern "C" WuiDlg *wui_dlg_new(HWND owner, const wchar_t *title, HICON icon,
                               void *ctx)
{
    auto dlg = new WuiDlg(ctx);
    live_dlgs.push_back(dlg);
    dlg->owner = owner;

    dlg->window = mux::Window();
    check_hresult(dlg->window.as<IWindowNative>()->get_WindowHandle(
                      &dlg->hwnd));
    UINT dpi = GetDpiForWindow(dlg->hwnd);
    dlg->scale = (dpi ? dpi : 96) / 96.0;
    dlg->appwindow = dlg->window.AppWindow();
    auto presenter = muw::OverlappedPresenter::Create();
    presenter.IsMinimizable(false);
    dlg->appwindow.SetPresenter(presenter);
    /* Keep it above its owner, as a dialog box is */
    if (owner)
        SetWindowLongPtrW(dlg->hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);

    dlg->window.Title(hs(title));
    if (icon) {
        SendMessageW(dlg->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(dlg->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
    muxm::MicaBackdrop mica;
    dlg->window.SystemBackdrop(mica);

    /*
     * The layout: our own title bar (so that Mica runs right up to
     * the top), the navigation view and the page, and the button bar.
     */
    muxc::Grid root;
    for (int i = 0; i < 2; i++) {
        muxc::RowDefinition r;
        r.Height(i == 1 ? mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star) :
                 mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(r);
    }

    muxc::Grid titlebar;
    titlebar.Height(48);
    titlebar.Background(muxm::SolidColorBrush(
                            winrt::Microsoft::UI::Colors::Transparent()));
    muxc::StackPanel tbcontent;
    tbcontent.Orientation(muxc::Orientation::Horizontal);
    tbcontent.VerticalAlignment(mux::VerticalAlignment::Center);
    tbcontent.Margin(mux::ThicknessHelper::FromLengths(16, 0, 0, 0));
    tbcontent.Spacing(12);
    muxc::Image tbicon;
    tbicon.Width(16);
    tbicon.Height(16);
    try {
        tbicon.Source(wui_icon_image(
                          icon, (int)std::lround(16 * dlg->scale)));
    } catch (...) {
    }
    tbcontent.Children().Append(tbicon);
    auto tbtext = styled_text(L"CaptionTextBlockStyle", hs(title));
    tbtext.VerticalAlignment(mux::VerticalAlignment::Center);
    tbcontent.Children().Append(tbtext);
    titlebar.Children().Append(tbcontent);
    muxc::Grid::SetRow(titlebar, 0);
    root.Children().Append(titlebar);

    dlg->nav = muxc::NavigationView();
    dlg->nav.PaneDisplayMode(muxc::NavigationViewPaneDisplayMode::Left);
    dlg->nav.IsSettingsVisible(false);
    dlg->nav.IsBackButtonVisible(
        muxc::NavigationViewBackButtonVisible::Collapsed);
    dlg->nav.IsPaneToggleButtonVisible(false);
    dlg->nav.IsTitleBarAutoPaddingEnabled(false);
    dlg->nav.OpenPaneLength(280);
    /*
     * The page sits on the Mica, like the navigation pane, as in
     * Settings: NavigationView would put it in a lighter area of its
     * own, so we make that invisible. The title stays put above the
     * part that scrolls.
     */
    {
        muxm::SolidColorBrush clear(
            winrt::Microsoft::UI::Colors::Transparent());
        auto res = dlg->nav.Resources();
        res.Insert(winrt::box_value(L"NavigationViewContentBackground"),
                   clear);
        res.Insert(winrt::box_value(L"NavigationViewContentGridBorderBrush"),
                   clear);
    }
    muxc::Grid content;
    for (int i = 0; i < 2; i++) {
        muxc::RowDefinition r;
        r.Height(i ? mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star) :
                 mux::GridLengthHelper::Auto());
        content.RowDefinitions().Append(r);
    }
    /* The title, with the main button (Open, or Apply) on the right
     * of it, where it's always there to be pressed, whichever page
     * you're on: the place for an app's main action in Windows 11 */
    muxc::Grid top;
    top.Padding(mux::ThicknessHelper::FromLengths(40, 8, 40, 16));
    top.HorizontalAlignment(mux::HorizontalAlignment::Left);
    top.ColumnSpacing(24);
    {
        muxc::ColumnDefinition c0, c1;
        c0.Width(mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star));
        c1.Width(mux::GridLengthHelper::Auto());
        top.ColumnDefinitions().Append(c0);
        top.ColumnDefinitions().Append(c1);
    }
    dlg->heading = muxc::StackPanel();
    dlg->actions = muxc::StackPanel();
    dlg->actions.Orientation(muxc::Orientation::Horizontal);
    dlg->actions.Spacing(8);
    dlg->actions.VerticalAlignment(mux::VerticalAlignment::Top);
    dlg->actions.Margin(mux::ThicknessHelper::FromLengths(0, 6, 0, 0));
    for (auto *panel : { &dlg->footer_lead, &dlg->page_actions,
                         &dlg->page_main, &dlg->footer_main,
                         &dlg->menu_actions }) {
        *panel = muxc::StackPanel();
        panel->Orientation(muxc::Orientation::Horizontal);
        panel->Spacing(8);
        dlg->actions.Children().Append(*panel);
    }
    /* (in the title bar, after the title and a dash, what's being
     * edited) */
    muxc::StackPanel titlecol;
    dlg->context = muxc::StackPanel();
    dlg->context.Orientation(muxc::Orientation::Horizontal);
    dlg->context.Spacing(8);
    dlg->context.Visibility(mux::Visibility::Collapsed);
    dlg->context.Children().Append(
        styled_text(L"CaptionTextBlockStyle", hstring(L"-"), true));
    dlg->context_icon = symbol(0xE70F, 12);              /* Edit */
    dlg->context_icon.VerticalAlignment(mux::VerticalAlignment::Center);
    dlg->context_text = styled_text(L"BodyStrongTextBlockStyle", hstring());
    dlg->context_text.FontSize(12);
    dlg->context_text.TextWrapping(mux::TextWrapping::NoWrap);
    dlg->context_text.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    dlg->context_dot = xaml<muxc::TextBlock>(
        L"TextBlock", L"Text='\x25CF' FontSize='10' "
        L"VerticalAlignment='Center' "
        L"Foreground='{ThemeResource AccentTextFillColorPrimaryBrush}'");
    muxc::ToolTipService::SetToolTip(
        dlg->context_dot, winrt::box_value(hstring(L"Not saved")));
    dlg->context_detail = styled_text(L"CaptionTextBlockStyle", hstring(),
                                      true);
    /* (each with a limit, as the title bar doesn't make them fit, and
     * they'd run under the caption buttons) */
    dlg->context_text.MaxWidth(240);
    dlg->context_detail.MaxWidth(360);
    dlg->context_detail.TextWrapping(mux::TextWrapping::NoWrap);
    dlg->context_detail.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    for (mux::FrameworkElement e : { mux::FrameworkElement(dlg->context_icon),
                                     mux::FrameworkElement(dlg->context_text),
                                     mux::FrameworkElement(dlg->context_dot),
                                     mux::FrameworkElement(
                                         dlg->context_detail) }) {
        e.VerticalAlignment(mux::VerticalAlignment::Center);
        dlg->context.Children().Append(e);
    }
    for (auto e : dlg->context.Children())
        e.as<mux::FrameworkElement>().VerticalAlignment(
            mux::VerticalAlignment::Center);
    tbcontent.Children().Append(dlg->context);
    /* (and as the title bar is where the window's dragged from, which
     * takes the pointer, that part of it isn't, so that its tooltip
     * still works: see update_passthrough) */
    dlg->context.SizeChanged([dlg](auto &&, auto &&) {
        update_passthrough(dlg);
    });

    /* The help button, at the end of the title bar, before the caption
     * buttons (hidden until there's help to show: wui_dlg_set_help) */
    /* (looking like one of them: as wide, as high, square, and only
     * showing a background as the pointer's over it) */
    dlg->help = xaml<muxc::Button>(
        L"Button", L"Width='46' Height='32' Padding='0' CornerRadius='0' "
        L"Background='Transparent' BorderThickness='0' "
        L"HorizontalAlignment='Right' VerticalAlignment='Top'");
    {
        auto res = dlg->help.Resources();
        auto brush = [](const wchar_t *key) {
            return mux::Application::Current().Resources().Lookup(
                winrt::box_value(hstring(key)));
        };
        res.Insert(winrt::box_value(L"ButtonBackgroundPointerOver"),
                   brush(L"SubtleFillColorSecondaryBrush"));
        res.Insert(winrt::box_value(L"ButtonBackgroundPressed"),
                   brush(L"SubtleFillColorTertiaryBrush"));
        for (const wchar_t *key : { L"ButtonBorderBrushPointerOver",
                                    L"ButtonBorderBrushPressed" })
            res.Insert(winrt::box_value(key),
                       muxm::SolidColorBrush(
                           winrt::Microsoft::UI::Colors::Transparent()));
    }
    dlg->help.Content(symbol(0xE9CE, 12));        /* Unknown: "?" in a ring */
    muxc::ToolTipService::SetToolTip(dlg->help,
                                     winrt::box_value(hstring(L"Help")));
    dlg->help.Visibility(mux::Visibility::Collapsed);
    dlg->help.Click([dlg](auto &&, auto &&) {
        if (is_live(dlg) && !dlg->help_url.empty())
            ShellExecuteW(dlg->hwnd, L"open", dlg->help_url.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
    });
    dlg->help.SizeChanged([dlg](auto &&, auto &&) {
        update_passthrough(dlg);
    });
    titlebar.Children().Append(dlg->help);
    /* (the caption buttons' room, which the title bar's content runs
     * under, is only known once the window's up) */
    titlebar.SizeChanged([dlg](auto &&, auto &&) {
        if (!is_live(dlg))
            return;
        try {
            double inset = dlg->appwindow.TitleBar().RightInset() /
                dlg->scale;
            dlg->help.Margin(
                mux::ThicknessHelper::FromLengths(0, 0, inset, 0));
        } catch (...) {
        }
        /* (once the help button has moved there, which changes its
         * position but not its size, so its own SizeChanged doesn't
         * say) */
        dlg->window.DispatcherQueue().TryEnqueue([dlg]() {
            update_passthrough(dlg);
        });
    });
    titlecol.Children().Append(dlg->heading);
    muxc::Grid::SetColumn(titlecol, 0);
    muxc::Grid::SetColumn(dlg->actions, 1);
    top.Children().Append(titlecol);
    top.Children().Append(dlg->actions);
    muxc::Grid::SetRow(top, 0);
    content.Children().Append(top);
    dlg->scroller = muxc::ScrollViewer();
    dlg->page = muxc::StackPanel();
    dlg->page.Padding(mux::ThicknessHelper::FromLengths(40, 0, 40, 40));
    dlg->page.HorizontalAlignment(mux::HorizontalAlignment::Left);
    dlg->page.Spacing(8);
    dlg->scroller.Content(dlg->page);
    muxc::Grid::SetRow(dlg->scroller, 1);
    content.Children().Append(dlg->scroller);
    /*
     * The title and the page are as wide as there's room for, up to a
     * limit, and against the navigation pane, however wide the window
     * is, as in Settings. (Centred, as they'd be with just MaxWidth,
     * they'd move away from it in a wide window; and apart from each
     * other, the page having a scroll bar beside it and the title not.)
     */
    content.SizeChanged([wtop = winrt::make_weak(top), dlg](
                            wf::IInspectable const &sender, auto &&) {
        if (!is_live(dlg))
            return;
        double w = std::min(1000.0,
                            sender.as<mux::FrameworkElement>().ActualWidth());
        if (auto t = wtop.get())
            t.Width(w);
        dlg->page.Width(w);
    });
    dlg->nav.Content(content);
    muxc::Grid::SetRow(dlg->nav, 1);
    root.Children().Append(dlg->nav);

    dlg->window.Content(root);
    dlg->window.ExtendsContentIntoTitleBar(true);
    dlg->window.SetTitleBar(titlebar);

    dlg->nav.SelectionChanged(
        [dlg](auto &&, muxc::NavigationViewSelectionChangedEventArgs const &e) {
            if (!is_live(dlg) || dlg->nav_quiet)
                return;
            auto item = e.SelectedItem().try_as<muxc::NavigationViewItem>();
            if (item)
                goto_category(dlg, winrt::unbox_value<int>(item.Tag()));
        });

    /* Return and Escape press the default and cancel buttons, as
     * they do in a dialog box, if no control wanted them itself */
    root.KeyDown([dlg](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        if (!is_live(dlg))
            return;
        if (e.Key() == VirtualKey::Enter && dlg->default_button) {
            press_button(dlg->default_button);
            e.Handled(true);
        } else if (e.Key() == VirtualKey::Escape && dlg->cancel_button) {
            press_button(dlg->cancel_button);
            e.Handled(true);
        }
    });

    dlg->appwindow.Closing(
        [dlg](muw::AppWindow const &, muw::AppWindowClosingEventArgs const &e) {
            if (is_live(dlg) && !dlg->close_ok) {
                e.Cancel(true);
                wui_dlg_cb_close(dlg->ctx);
            }
        });

    int w = (int)std::lround(DLG_WIDTH * dlg->scale);
    int h = (int)std::lround(DLG_HEIGHT * dlg->scale);
    dlg->appwindow.Resize(winrt::Windows::Graphics::SizeInt32{w, h});
    centre(dlg, w, h);
    return dlg;
}

extern "C" HWND wui_dlg_hwnd(WuiDlg *dlg)
{
    return dlg->hwnd;
}

extern "C" void wui_dlg_set_theme(WuiDlg *dlg, int theme)
{
    auto root = dlg->window.Content().try_as<mux::FrameworkElement>();
    if (!root)
        return;
    auto want = theme == 1 ? mux::ElementTheme::Light :
        theme == 2 ? mux::ElementTheme::Dark : mux::ElementTheme::Default;
    if (root.RequestedTheme() == want)
        return;
    root.RequestedTheme(want);
    wui_dwm_dark_frame(dlg->hwnd, wui_is_dark(root));
    try {
        dlg->appwindow.TitleBar().PreferredTheme(
            theme == 1 ? muw::TitleBarTheme::Light :
            theme == 2 ? muw::TitleBarTheme::Dark :
            muw::TitleBarTheme::UseDefaultAppMode);
    } catch (...) {
        /* (as in wui_set_theme) */
    }
}

extern "C" void wui_dlg_run(WuiDlg *dlg)
{
    if (dlg->owner)
        EnableWindow(dlg->owner, FALSE);
    wui_show_without_flash(dlg->hwnd, dlg->window);
    if (!dlg->stop_requested) {
        dlg->running = true;
        /*
         * A nested message loop. The dispatcher queue's own one does
         * the pre-translation XAML needs for keyboard input; but in
         * case it can't run here, fall back to a plain one.
         */
        try {
            dlg->window.DispatcherQueue().RunEventLoop(
                mud::DispatcherRunOptions::QuitOnlyLocalLoop, nullptr);
        } catch (...) {
            dlg->manual_loop = true;
            MSG msg;
            while (!dlg->stop_requested && GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        dlg->running = false;
    }
    if (dlg->owner) {
        /* Before the dialog goes, so that Windows activates the
         * owner rather than some other application */
        EnableWindow(dlg->owner, TRUE);
    }
}

extern "C" void wui_dlg_stop(WuiDlg *dlg)
{
    if (dlg->stop_requested)
        return;
    dlg->stop_requested = true;
    if (!dlg->running)
        return;
    if (dlg->manual_loop)
        PostMessageW(dlg->hwnd, WM_NULL, 0, 0);
    else
        dlg->window.DispatcherQueue().EnqueueEventLoopExit();
}

extern "C" void wui_dlg_free(WuiDlg *dlg)
{
    live_dlgs.erase(std::remove(live_dlgs.begin(), live_dlgs.end(), dlg),
                    live_dlgs.end());
    if (dlg->owner) {
        EnableWindow(dlg->owner, TRUE);
        SetActiveWindow(dlg->owner);
    }
    dlg->close_ok = true;
    /* (out of sight first: closing it takes its content away before
     * the window itself goes, which would show as a flash of white) */
    wui_dwm_set(dlg->hwnd, DWMWA_CLOAK, TRUE);
    try {
        dlg->window.Close();
    } catch (...) {
    }
    for (auto &w : dlg->widgets) {
        w->retire();
        retired.push_back(std::move(w));
    }
    delete dlg;
}

/* ---- Navigation ---- */

extern "C" void wui_dlg_add_category(WuiDlg *dlg, const wchar_t *label,
                                     unsigned glyph, int id)
{
    muxc::NavigationViewItem item;
    item.Content(winrt::box_value(hs(label)));
    item.Tag(winrt::box_value(id));
    if (glyph)
        item.Icon(symbol(glyph));
    dlg->nav.MenuItems().Append(item);
    dlg->categories[id] = item;
}

extern "C" void wui_dlg_add_footer_category(WuiDlg *dlg, const wchar_t *label,
                                            unsigned glyph, int id)
{
    muxc::NavigationViewItem item;
    item.Content(winrt::box_value(hs(label)));
    item.Tag(winrt::box_value(id));
    if (glyph)
        item.Icon(symbol(glyph));
    dlg->nav.FooterMenuItems().Append(item);
    dlg->categories[id] = item;
}

extern "C" void wui_dlg_set_context(WuiDlg *dlg, unsigned glyph,
                                    const wchar_t *text, bool dot,
                                    const wchar_t *detail)
{
    if (!has_text(text)) {
        dlg->context.Visibility(mux::Visibility::Collapsed);
        return;
    }
    wchar_t g[2] = { (wchar_t)(glyph ? glyph : 0xE70F), L'\0' };
    dlg->context_icon.Glyph(g);
    dlg->context_text.Text(hs(text));
    dlg->context_dot.Visibility(dot ? mux::Visibility::Visible :
                                mux::Visibility::Collapsed);
    dlg->context_detail.Text(hs(detail));
    dlg->context_detail.Visibility(has_text(detail) ?
                                   mux::Visibility::Visible :
                                   mux::Visibility::Collapsed);
    dlg->context.Visibility(mux::Visibility::Visible);
}

extern "C" void wui_dlg_set_help(WuiDlg *dlg, const wchar_t *url)
{
    dlg->help_url = has_text(url) ? url : L"";
    dlg->help.Visibility(has_text(url) ? mux::Visibility::Visible :
                         mux::Visibility::Collapsed);
    dlg->window.DispatcherQueue().TryEnqueue([dlg]() {
        update_passthrough(dlg);
    });
}

extern "C" void wui_dlg_set_context_tip(WuiDlg *dlg, const wchar_t *text,
                                        const wchar_t *italic)
{
    namespace muxd = winrt::Microsoft::UI::Xaml::Documents;
    if (!has_text(text) && !has_text(italic)) {
        muxc::ToolTipService::SetToolTip(dlg->context, nullptr);
        return;
    }
    muxc::TextBlock t;
    t.TextWrapping(mux::TextWrapping::Wrap);
    t.MaxWidth(360);
    if (has_text(text)) {
        muxd::Run r;
        r.Text(hs(text));
        t.Inlines().Append(r);
    }
    if (has_text(italic)) {
        if (has_text(text))
            t.Inlines().Append(muxd::LineBreak());
        muxd::Run r;
        r.Text(hs(italic));
        r.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
        t.Inlines().Append(r);
    }
    muxc::ToolTipService::SetToolTip(dlg->context, t);
}

extern "C" void wui_dlg_select_category(WuiDlg *dlg, int id)
{
    auto it = dlg->categories.find(id);
    if (it == dlg->categories.end())
        return;
    dlg->nav_quiet++;
    dlg->nav.SelectedItem(it->second);
    dlg->nav_quiet--;
}

/* ---- The page ---- */

extern "C" void wui_dlg_clear_page(WuiDlg *dlg)
{
    auto &ws = dlg->widgets;
    if (dlg->default_button && !dlg->default_button->footer)
        dlg->default_button = nullptr;
    if (dlg->cancel_button && !dlg->cancel_button->footer)
        dlg->cancel_button = nullptr;
    auto keep = std::stable_partition(ws.begin(), ws.end(),
                                      [](auto &w) { return w->footer; });
    for (auto it = keep; it != ws.end(); ++it) {
        (*it)->retire();
        retired.push_back(std::move(*it));
    }
    ws.erase(keep, ws.end());
    dlg->heading.Children().Clear();
    dlg->page_actions.Children().Clear();
    dlg->page_main.Children().Clear();
    dlg->menu_actions.Children().Clear();
    dlg->page.Children().Clear();
    for (auto &t : dlg->timers)
        t.Stop();
    dlg->timers.clear();
    dlg->notice = nullptr;
    dlg->subpages = nullptr;
    dlg->scroller.ChangeView(nullptr, 0.0, nullptr, true);
}

extern "C" void wui_dlg_page_title(WuiDlg *dlg, int n,
                                   const wchar_t *const *crumbs,
                                   const int *ids, const wchar_t *subtitle)
{
    muxc::BreadcrumbBar trail;
    /* Big, like a page title, as in Settings */
    auto res = trail.Resources();
    res.Insert(winrt::box_value(L"BreadcrumbBarItemThemeFontSize"),
               winrt::box_value(28.0));
    res.Insert(winrt::box_value(L"BreadcrumbBarChevronFontSize"),
               winrt::box_value(16.0));
    res.Insert(winrt::box_value(L"BreadcrumbBarItemFontWeight"),
               winrt::box_value(winrt::Microsoft::UI::Text::FontWeights::
                                SemiBold()));
    auto items = winrt::single_threaded_observable_vector<
        wf::IInspectable>();
    std::vector<int> idv;
    for (int i = 0; i < n; i++) {
        items.Append(winrt::box_value(hs(crumbs[i])));
        idv.push_back(ids[i]);
    }
    trail.ItemsSource(items);
    trail.ItemClicked(
        [dlg, idv](auto &&, muxc::BreadcrumbBarItemClickedEventArgs const &e) {
            int i = e.Index();
            if (i >= 0 && i + 1 < (int)idv.size())   /* not the last */
                goto_category(dlg, idv[i]);
        });
    dlg->heading.Children().Append(trail);

    if (has_text(subtitle)) {
        auto s = styled_text(L"BodyTextBlockStyle", hs(subtitle), true);
        s.Margin(mux::ThicknessHelper::FromLengths(0, 4, 0, 0));
        dlg->heading.Children().Append(s);
    }
}

extern "C" WuiWidget *wui_dlg_group(WuiDlg *dlg, bool footer,
                                    const wchar_t *title)
{
    dlg->widgets.push_back(std::make_unique<WuiWidget>(dlg, Kind::Group));
    WuiWidget *g = dlg->widgets.back().get();
    g->footer = footer;

    if (footer) {
        g->right = dlg->actions;
        g->root = dlg->actions;
        return g;
    }

    /* One card for the whole group: its title at the top, then a row
     * for each setting */
    auto card = group_card();
    g->stack = muxc::StackPanel();
    card.Child(g->stack);
    if (has_text(title)) {
        auto t = styled_text(L"BodyStrongTextBlockStyle", hs(title));
        t.Margin(mux::ThicknessHelper::FromLengths(24, 10, 24, 6));
        g->stack.Children().Append(t);
        g->nrows++;
    }
    dlg->page.Children().Append(card);
    g->root = card;
    return g;
}

extern "C" void wui_dlg_subpage(WuiDlg *dlg, const wchar_t *label, int id)
{
    bool first = !dlg->subpages;
    if (first) {
        auto card = group_card();
        dlg->subpages = muxc::StackPanel();
        card.Child(dlg->subpages);
        dlg->page.Children().Append(card);
    }
    /* A row you can click on, with a line above it if it isn't the
     * first, like those in Settings */
    auto b = xaml<muxc::Button>(
        L"Button", L"HorizontalAlignment='Stretch' "
        L"HorizontalContentAlignment='Stretch' Padding='24,0,24,0' "
        L"MinHeight='44' CornerRadius='0' Background='Transparent' "
        L"BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'");
    b.BorderThickness(mux::ThicknessHelper::FromLengths(0, first ? 0 : 1,
                                                        0, 0));
    muxc::Grid g;
    muxc::ColumnDefinition c0, c1;
    c0.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    c1.Width(mux::GridLengthHelper::Auto());
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    muxc::TextBlock t;
    t.Text(hs(label));
    t.VerticalAlignment(mux::VerticalAlignment::Center);
    auto chevron = symbol(0xE76C, 12);           /* ChevronRight */
    chevron.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(t, 0);
    muxc::Grid::SetColumn(chevron, 1);
    g.Children().Append(t);
    g.Children().Append(chevron);
    b.Content(g);
    b.Click([dlg, id](auto &&, auto &&) { goto_category(dlg, id); });
    dlg->subpages.Children().Append(b);
}

/* ---- Widgets ---- */

/* Put something in a new row at the end of a group */
static void add_row(WuiWidget *grp, mux::UIElement content)
{
    auto row = row_border(grp->nrows++ == 0);
    if (grp->bare) {
        row.Padding(mux::ThicknessHelper::FromUniformLength(0));
        row.MinHeight(0);
    } else if (grp->flush) {
        row.Padding(mux::ThicknessHelper::FromLengths(0, 4, 0, 4));
    }
    row.Child(content);
    grp->stack.Children().Append(row);
}

extern "C" void wui_dlg_group_flush(WuiWidget *grp)
{
    grp->flush = true;
}

extern "C" void wui_dlg_group_bare(WuiWidget *grp)
{
    grp->bare = true;
    if (auto card = grp->root.try_as<muxc::Border>()) {
        card.Background(nullptr);
        card.BorderThickness(mux::ThicknessHelper::FromUniformLength(0));
    }
}

extern "C" void wui_dlg_group_one_row(WuiWidget *grp)
{
    grp->one_row = muxc::Grid();
    grp->one_row.ColumnSpacing(12);
    add_row(grp, grp->one_row);
}

extern "C" void wui_dlg_group_end_row(WuiWidget *grp)
{
    grp->one_row = nullptr;
}

/* Add a widget to the end of its group (or of its one row), and track
 * its focus */
static void add(WuiWidget *grp, WuiWidget *w)
{
    grp->bar = nullptr;                /* buttons after this start anew */
    if (grp->one_row) {
        muxc::ColumnDefinition c;
        c.Width(w->stretch ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        auto cols = grp->one_row.ColumnDefinitions();
        muxc::Grid::SetColumn(w->root, (int)cols.Size());
        cols.Append(c);
        grp->one_row.Children().Append(w->root);
    } else {
        add_row(grp, w->root);
    }
    w->root.GotFocus([w](auto &&, auto &&) { fire(w, WUI_EV_FOCUS); });
    w->root.LostFocus([w](auto &&, auto &&) { fire(w, WUI_EV_UNFOCUS); });
}

/*
 * The content of a setting's row: the label (and any description
 * under it) on the left and the control on the right, or (if 'below',
 * or it shares a row with others) the label above the control, which
 * takes the whole width.
 */
static mux::FrameworkElement setting_row(WuiWidget *w, const wchar_t *label,
                                          mux::FrameworkElement control,
                                          bool below, bool description = false)
{
    w->label = muxc::TextBlock();
    w->label.Text(setting_name(label));
    w->label.TextWrapping(mux::TextWrapping::Wrap);
    if (!has_text(label))
        w->label.Visibility(mux::Visibility::Collapsed);

    /* (beside it, a button to reset it, if it's been changed from the
     * default: see wui_w_set_modified) */
    muxc::StackPanel labelrow;
    labelrow.Orientation(muxc::Orientation::Horizontal);
    labelrow.Spacing(4);
    labelrow.Children().Append(w->label);
    w->reset = xaml<muxc::Button>(
        L"Button", L"Width='26' Height='26' Padding='0' "
        L"Background='Transparent' BorderThickness='0' "
        L"VerticalAlignment='Center' "
        L"Foreground='{ThemeResource AccentTextFillColorPrimaryBrush}'");
    w->reset.Content(symbol(0xE7A7, 12));                /* Undo */
    muxc::ToolTipService::SetToolTip(
        w->reset, winrt::box_value(hstring(
                      L"Not the default: click to reset it")));
    w->reset.Visibility(mux::Visibility::Collapsed);
    w->reset.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_RESET); });
    labelrow.Children().Append(w->reset);

    muxc::StackPanel names;
    names.VerticalAlignment(mux::VerticalAlignment::Center);
    names.Spacing(2);
    names.Children().Append(labelrow);
    if (description) {
        w->text = styled_text(L"CaptionTextBlockStyle", hstring(), true);
        names.Children().Append(w->text);
    }

    if (below || w->in_row) {
        muxc::StackPanel sp;
        sp.Spacing(w->in_row ? 6 : 10);
        sp.Children().Append(names);
        sp.Children().Append(control);
        return sp;
    }

    muxc::Grid g;
    g.ColumnSpacing(24);
    muxc::ColumnDefinition c0, c1;
    c0.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    c1.Width(mux::GridLengthHelper::Auto());
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    control.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(names, 0);
    muxc::Grid::SetColumn(control, 1);
    g.Children().Append(names);
    g.Children().Append(control);
    return g;
}

static void hook_text_box(WuiWidget *w, muxc::TextBox box)
{
    box.TextChanged([w](auto &&, auto &&) {
        if (w->dead)
            return;
        if (w->text_pending) {
            w->text_pending = false;
            return;
        }
        fire(w, WUI_EV_VALCHANGE);
    });
}

extern "C" WuiWidget *wui_w_text(WuiWidget *grp, void *wctx,
                                 const wchar_t *text)
{
    WuiWidget *w = new_widget(grp, Kind::Text);
    w->wctx = wctx;
    w->text = styled_text(L"BodyTextBlockStyle", hs(text), true);
    w->text.IsTextSelectionEnabled(true);
    w->text.VerticalAlignment(mux::VerticalAlignment::Center);
    w->root = w->text;
    add(grp, w);
    return w;
}

/* Find a named part inside a control's template */
static mux::DependencyObject find_part(mux::DependencyObject const &o,
                                       const wchar_t *name)
{
    int n = muxm::VisualTreeHelper::GetChildrenCount(o);
    for (int i = 0; i < n; i++) {
        auto c = muxm::VisualTreeHelper::GetChild(o, i);
        if (auto fe = c.try_as<mux::FrameworkElement>();
            fe && fe.Name() == name)
            return c;
        if (auto r = find_part(c, name))
            return r;
    }
    return nullptr;
}

/* Is the keyboard focus somewhere inside this element? */
static bool has_focus_within(mux::UIElement const &e)
{
    if (!e.XamlRoot())
        return false;
    auto f = muxi::FocusManager::GetFocusedElement(e.XamlRoot())
        .try_as<mux::DependencyObject>();
    while (f) {
        if (f == e)
            return true;
        f = muxm::VisualTreeHelper::GetParent(f);
    }
    return false;
}

extern "C" WuiWidget *wui_w_edit(WuiWidget *grp, void *wctx,
                                 const wchar_t *label, bool password,
                                 bool combo, int numeric, double step)
{
    WuiWidget *w = new_widget(grp, Kind::Edit);
    w->wctx = wctx;
    mux::FrameworkElement ctl{nullptr};
    if (numeric) {
        namespace nf = winrt::Windows::Globalization::NumberFormatting;
        w->number = muxc::NumberBox();
        w->integer = (numeric == 1);
        /* (wide enough for 5 digits, a port number, say, beside the
         * spin buttons and the clear button it gets with the focus; but
         * sharing a row, as the port does with the host name, with no
         * spin buttons, which nobody would step a port number with) */
        w->number.Width(w->in_row ? 110 : 200);
        w->number.Minimum(0);
        w->number.SmallChange(step > 0 ? step : 1);
        w->number.LargeChange((step > 0 ? step : 1) * 10);
        w->number.SpinButtonPlacementMode(
            w->in_row ? muxc::NumberBoxSpinButtonPlacementMode::Hidden :
            muxc::NumberBoxSpinButtonPlacementMode::Inline);
        /* Something that isn't a number goes back to what was there */
        w->number.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        nf::DecimalFormatter fmt;
        fmt.IntegerDigits(1);
        fmt.FractionDigits(0);
        fmt.IsGrouped(false);
        if (w->integer) {
            nf::IncrementNumberRounder rounder;
            rounder.Increment(1);
            rounder.RoundingAlgorithm(nf::RoundingAlgorithm::RoundHalfUp);
            fmt.NumberRounder(rounder);
        }
        w->number.NumberFormatter(fmt);
        w->number.ValueChanged([w](auto &&, auto &&) {
            if (w->dead || w->quiet)
                return;
            fire(w, WUI_EV_VALCHANGE);
        });
        ctl = w->number;
    } else if (combo) {
        w->combo = muxc::ComboBox();
        w->combo.IsEditable(true);
        w->combo.Width(CONTROL_WIDTH);
        /*
         * The classic box reports every keystroke in the edit part,
         * so we want the text box inside the combo box, which only
         * exists once its template has been applied.
         */
        w->combo.Loaded([w](auto &&, auto &&) {
            if (w->dead)
                return;
            if (w->edit)
                return;
            if (auto tb = find_part(w->combo, L"EditableText")
                .try_as<muxc::TextBox>()) {
                w->edit = tb;
                hook_text_box(w, tb);
            }
        });
        w->combo.SelectionChanged([w](auto &&, auto &&) {
            if (w->dead)
                return;
            if (w->quiet)
                return;
            int i = w->combo.SelectedIndex();
            if (i < 0)
                return;
            auto s = winrt::unbox_value_or<hstring>(
                w->combo.Items().GetAt(i), hstring());
            /* (The text box first, so that its change is the one we
             * expect, not a stray one caused by the combo box) */
            w->quiet++;
            if (w->edit)
                set_box_text(w, w->edit, s);
            w->combo.Text(s);
            w->quiet--;
            fire(w, WUI_EV_VALCHANGE);
        });
        w->combo.TextSubmitted([w](auto &&, auto &&) {
            if (w->dead)
                return;
            fire(w, WUI_EV_VALCHANGE);
        });
        w->combo.LostFocus([w](auto &&, auto &&) {
            if (w->dead)
                return;
            /* (Focus moves around inside the combo box, and into its
             * drop-down, so wait and see where it ends up) */
            WuiDlg *dlg = w->dlg;
            dlg->window.DispatcherQueue().TryEnqueue([dlg, w]() {
                if (is_live(dlg, w) && !w->combo.IsDropDownOpen() &&
                    !has_focus_within(w->combo))
                    fire(w, WUI_EV_COMMIT);
            });
        });
        ctl = w->combo;
    } else if (password) {
        w->pass = muxc::PasswordBox();
        w->pass.Width(CONTROL_WIDTH);
        w->pass.PasswordChanged([w](auto &&, auto &&) {
            if (w->dead)
                return;
            if (w->text_pending) {
                w->text_pending = false;
                return;
            }
            fire(w, WUI_EV_VALCHANGE);
        });
        ctl = w->pass;
    } else {
        w->edit = muxc::TextBox();
        if (w->in_row)
            w->stretch = true;          /* (the widest, in a shared row) */
        else
            w->edit.Width(CONTROL_WIDTH);
        hook_text_box(w, w->edit);
        ctl = w->edit;
    }
    if (w->in_row) {
        /* (no room for buttons beside it) */
        w->root = setting_row(w, label, ctl, false);
        add(grp, w);
        return w;
    }
    /* (with room for buttons beside it: see wui_w_button_beside) */
    w->tail = muxc::StackPanel();
    w->tail.Orientation(muxc::Orientation::Horizontal);
    w->tail.Spacing(8);
    w->tail.Children().Append(ctl);
    w->root = setting_row(w, label, w->tail, false);
    add(grp, w);
    return w;
}

extern "C" WuiWidget *wui_w_button_beside(WuiWidget *edit, void *wctx,
                                          const wchar_t *label)
{
    WuiWidget *w = new_widget(edit, Kind::Button);
    w->wctx = wctx;
    w->button = muxc::Button();
    w->button.Content(winrt::box_value(hs(label)));
    w->button.MinWidth(100);
    w->button.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_ACTION); });
    w->root = w->button;
    if (edit->tail)
        edit->tail.Children().Append(w->button);
    w->root.GotFocus([w](auto &&, auto &&) { fire(w, WUI_EV_FOCUS); });
    w->root.LostFocus([w](auto &&, auto &&) { fire(w, WUI_EV_UNFOCUS); });
    return w;
}

/* Radio buttons become a drop-down list of the choices */
extern "C" WuiWidget *wui_w_radio(WuiWidget *grp, void *wctx,
                                  const wchar_t *label, int nbuttons,
                                  const wchar_t *const *buttons)
{
    WuiWidget *w = new_widget(grp, Kind::Radio);
    w->wctx = wctx;
    w->combo = muxc::ComboBox();
    w->combo.MinWidth(w->in_row ? 120 : 200);
    w->combo.MaxWidth(CONTROL_WIDTH * 1.5);
    for (int i = 0; i < nbuttons; i++)
        w->combo.Items().Append(winrt::box_value(hs(buttons[i])));
    w->combo.SelectionChanged([w](auto &&, auto &&) {
        if (w->dead)
            return;
        if (!w->quiet && w->combo.SelectedIndex() >= 0)
            fire(w, WUI_EV_VALCHANGE);
    });
    w->root = setting_row(w, label, w->combo, false);
    add(grp, w);
    return w;
}

/* Check boxes become on/off switches */
extern "C" WuiWidget *wui_w_check(WuiWidget *grp, void *wctx,
                                  const wchar_t *label)
{
    WuiWidget *w = new_widget(grp, Kind::Check);
    w->wctx = wctx;
    w->toggle = muxc::ToggleSwitch();
    w->toggle.MinWidth(0);
    w->toggle.Toggled([w](auto &&, auto &&) {
        if (w->dead)
            return;
        if (!w->quiet)
            fire(w, WUI_EV_VALCHANGE);
    });
    w->root = setting_row(w, label, w->toggle, false);
    add(grp, w);
    return w;
}

/* A push button, which raises 'ev' when pressed */
static WuiWidget *make_button(WuiWidget *grp, Kind kind, void *wctx,
                              const wchar_t *label, bool isdefault,
                              bool iscancel, WuiWidgetEvent ev)
{
    WuiWidget *w = new_widget(grp, kind);
    w->wctx = wctx;
    if (isdefault)
        w->button = xaml<muxc::Button>(
            L"Button", L"Style='{StaticResource AccentButtonStyle}'");
    else
        w->button = muxc::Button();
    w->button.Content(winrt::box_value(hs(label)));
    w->button.MinWidth(100);
    w->button.Click([w, ev](auto &&, auto &&) { fire_later(w, ev); });
    if (isdefault)
        grp->dlg->default_button = w;
    if (iscancel)
        grp->dlg->cancel_button = w;
    w->root = w->button;

    /* Buttons in a row share a strip */
    if (grp->footer) {
        /* The main button goes beside the page title. A cancel button
         * isn't shown, as Windows 11 apps don't have them: Escape and
         * the close button do its job. */
        if (!iscancel || isdefault) {
            w->button.MinWidth(120);
            (isdefault ? grp->dlg->footer_main : grp->dlg->footer_lead)
                .Children().Append(w->button);
        }
    } else {
        if (!grp->bar) {
            grp->bar = muxc::StackPanel();
            grp->bar.Orientation(muxc::Orientation::Horizontal);
            grp->bar.Spacing(8);
            grp->bar.VerticalAlignment(mux::VerticalAlignment::Center);
            add_row(grp, grp->bar);
        }
        grp->bar.Children().Append(w->button);
    }
    w->root.GotFocus([w](auto &&, auto &&) { fire(w, WUI_EV_FOCUS); });
    w->root.LostFocus([w](auto &&, auto &&) { fire(w, WUI_EV_UNFOCUS); });
    return w;
}

extern "C" WuiWidget *wui_w_button(WuiWidget *grp, void *wctx,
                                   const wchar_t *label,
                                   bool isdefault, bool iscancel)
{
    return make_button(grp, Kind::Button, wctx, label, isdefault, iscancel,
                       WUI_EV_ACTION);
}

/* Raise an event for some other widget's wctx (once XAML has finished
 * with the input event that led to it) */
static void fire_for(WuiWidget *w, void *target, WuiWidgetEvent ev)
{
    WuiDlg *dlg = w->dlg;
    dlg->window.DispatcherQueue().TryEnqueue([dlg, w, target, ev]() {
        if (!is_live(dlg, w))
            return;
        try {
            wui_dlg_cb_widget(dlg->ctx, target, ev);
        } catch (...) {
            report_exception(dlg, L"handling a control's event");
        }
    });
}

extern "C" WuiWidget *wui_w_page_prompt(WuiDlg *dlg, void *wctx,
                                        const wchar_t *label,
                                        const wchar_t *prompt,
                                        const wchar_t *confirm, void *target)
{
    dlg->widgets.push_back(std::make_unique<WuiWidget>(dlg, Kind::Edit));
    WuiWidget *w = dlg->widgets.back().get();
    w->wctx = wctx;

    w->edit = muxc::TextBox();
    w->edit.Width(CONTROL_WIDTH);
    w->edit.Header(winrt::box_value(hs(prompt)));
    hook_text_box(w, w->edit);
    auto ok = xaml<muxc::Button>(
        L"Button", L"Style='{StaticResource AccentButtonStyle}'");
    ok.Content(winrt::box_value(hs(confirm)));
    ok.MinWidth(100);
    ok.HorizontalAlignment(mux::HorizontalAlignment::Right);
    muxc::StackPanel sp;
    sp.Spacing(12);
    sp.Children().Append(w->edit);
    sp.Children().Append(ok);
    w->flyout = muxc::Flyout();
    w->flyout.Content(sp);
    w->flyout.Placement(muxp::FlyoutPlacementMode::BottomEdgeAlignedRight);

    auto done = [w, target]() {
        if (w->dead)
            return;
        w->flyout.Hide();
        fire_for(w, target, WUI_EV_ACTION);
    };
    ok.Click([done](auto &&, auto &&) { done(); });
    w->edit.KeyDown([w, done](auto &&, muxi::KeyRoutedEventArgs const &e) {
        if (w->dead)
            return;
        if (e.Key() == winrt::Windows::System::VirtualKey::Enter) {
            e.Handled(true);
            done();
        }
    });
    w->flyout.Opened([w](auto &&, auto &&) {
        if (w->dead)
            return;
        w->edit.Focus(mux::FocusState::Programmatic);
        w->edit.SelectAll();
    });

    w->button = muxc::Button();
    w->button.Content(winrt::box_value(hs(label)));
    w->button.MinWidth(100);
    w->button.Flyout(w->flyout);
    w->root = w->button;
    dlg->page_actions.Children().Append(w->button);
    return w;
}

/* Raise an event for a wctx, once XAML has finished with the input
 * event that led to it */
static void fire_ctx(WuiDlg *dlg, void *wctx, WuiWidgetEvent ev)
{
    dlg->window.DispatcherQueue().TryEnqueue([dlg, wctx, ev]() {
        if (!is_live(dlg))
            return;
        try {
            wui_dlg_cb_widget(dlg->ctx, wctx, ev);
        } catch (...) {
            report_exception(dlg, L"handling an event");
        }
    });
}

/* ---- Buttons beside the page title ---- */

static muxc::StackPanel icon_and_label(unsigned glyph, muxc::TextBlock t)
{
    muxc::StackPanel sp;
    sp.Orientation(muxc::Orientation::Horizontal);
    sp.Spacing(8);
    if (glyph) {
        auto icon = symbol(glyph, 14);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        sp.Children().Append(icon);
    }
    t.VerticalAlignment(mux::VerticalAlignment::Center);
    sp.Children().Append(t);
    return sp;
}

extern "C" WuiWidget *wui_dlg_page_button(WuiDlg *dlg, void *wctx,
                                          const wchar_t *label,
                                          unsigned glyph, bool accent)
{
    dlg->widgets.push_back(std::make_unique<WuiWidget>(dlg, Kind::Button));
    WuiWidget *w = dlg->widgets.back().get();
    w->wctx = wctx;
    w->button = accent ? xaml<muxc::Button>(
        L"Button", L"Style='{StaticResource AccentButtonStyle}'") :
        muxc::Button();
    w->label = muxc::TextBlock();
    w->label.Text(hs(label));
    w->button.Content(icon_and_label(glyph, w->label));
    w->button.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_ACTION); });
    w->root = w->button;
    (accent ? dlg->page_main : dlg->page_actions).Children().Append(
        w->button);
    return w;
}

extern "C" void wui_w_set_tooltip(WuiWidget *w, const wchar_t *tip)
{
    if (w->root)
        muxc::ToolTipService::SetToolTip(w->root,
                                         winrt::box_value(hs(tip)));
}

extern "C" void wui_w_set_accent(WuiWidget *w, bool accent)
{
    if (!w->button)
        return;
    if (!accent) {
        w->button.ClearValue(mux::FrameworkElement::StyleProperty());
    } else {
        try {
            w->button.Style(mux::Application::Current().Resources().Lookup(
                                winrt::box_value(L"AccentButtonStyle"))
                            .as<mux::Style>());
        } catch (...) {
        }
    }

    /* (and to where the main button goes, or out of there) */
    WuiDlg *dlg = w->dlg;
    auto parent = muxm::VisualTreeHelper::GetParent(w->button)
        .try_as<muxc::StackPanel>();
    muxc::StackPanel to{nullptr};
    if (parent == dlg->footer_lead || parent == dlg->footer_main)
        to = accent ? dlg->footer_main : dlg->footer_lead;
    else if (parent == dlg->page_actions || parent == dlg->page_main)
        to = accent ? dlg->page_main : dlg->page_actions;
    if (!to || to == parent)
        return;
    uint32_t i;
    if (parent.Children().IndexOf(w->button, i))
        parent.Children().RemoveAt(i);
    to.Children().Append(w->button);
}

extern "C" WuiWidget *wui_dlg_page_menu(WuiDlg *dlg, const wchar_t *tooltip)
{
    dlg->widgets.push_back(std::make_unique<WuiWidget>(dlg, Kind::Button));
    WuiWidget *w = dlg->widgets.back().get();
    w->button = muxc::Button();
    w->button.Content(symbol(0xE712, 14));               /* More */
    w->button.MinHeight(32);
    muxc::ToolTipService::SetToolTip(w->button,
                                     winrt::box_value(hs(tooltip)));
    w->menu = muxc::MenuFlyout();
    w->button.Flyout(w->menu);
    w->root = w->button;
    dlg->menu_actions.Children().Append(w->button);
    return w;
}

extern "C" void wui_w_menu_add(WuiWidget *m, void *wctx, const wchar_t *label,
                               unsigned glyph)
{
    if (!m->menu)
        return;
    if (!label) {
        m->menu.Items().Append(muxc::MenuFlyoutSeparator());
        return;
    }
    muxc::MenuFlyoutItem mi;
    mi.Text(hs(label));
    if (glyph)
        mi.Icon(symbol(glyph));
    WuiDlg *dlg = m->dlg;
    /* (once the menu has closed, as New folder... opens a ContentDialog:
     * see wui-flyout.h) */
    winrt::weak_ref<muxp::FlyoutBase> wmenu = winrt::make_weak(
        m->menu.as<muxp::FlyoutBase>());
    mi.Click([dlg, wctx, wmenu](auto &&, auto &&) {
        wui_after_flyout(wmenu, [dlg, wctx]() {
            if (is_live(dlg))
                fire_ctx(dlg, wctx, WUI_EV_ACTION);
        });
    });
    m->menu.Items().Append(mi);
}

extern "C" void wui_w_set_visible(WuiWidget *w, bool visible)
{
    if (w->root)
        w->root.Visibility(visible ? mux::Visibility::Visible :
                           mux::Visibility::Collapsed);
}

extern "C" void wui_w_enable(WuiWidget *w, bool enabled)
{
    muxc::Control c{nullptr};
    if (w->button)
        c = w->button;
    else if (w->edit)
        c = w->edit;
    else if (w->combo)
        c = w->combo;
    else if (w->toggle)
        c = w->toggle;
    else if (w->list)
        c = w->list;
    else if (w->number)
        c = w->number;
    if (c)
        c.IsEnabled(enabled);
}

/* ---- Notices, and asking for some text ---- */

extern "C" void wui_dlg_notice(WuiDlg *dlg, const wchar_t *text,
                               const wchar_t *action, void *wctx,
                               int seconds, bool error)
{
    muxc::InfoBar bar;
    bar.Message(hs(text));
    bar.Severity(error ? muxc::InfoBarSeverity::Error :
                 muxc::InfoBarSeverity::Success);
    bar.IsClosable(true);
    bar.IsOpen(true);
    auto weak = winrt::make_weak(bar);
    if (has_text(action)) {
        muxc::Button b;
        b.Content(winrt::box_value(hs(action)));
        b.Click([dlg, wctx, weak](auto &&, auto &&) {
            if (auto bar = weak.get())
                bar.IsOpen(false);
            fire_ctx(dlg, wctx, WUI_EV_ACTION);
        });
        bar.ActionButton(b);
    }
    /* (one at a time: a new one replaces the last) */
    if (dlg->notice) {
        uint32_t index;
        if (dlg->page.Children().IndexOf(dlg->notice, index))
            dlg->page.Children().RemoveAt(index);
        for (auto &t : dlg->timers)
            t.Stop();
        dlg->timers.clear();
    }
    dlg->notice = bar;
    dlg->page.Children().InsertAt(0, bar);
    if (seconds > 0) {
        auto timer = dlg->window.DispatcherQueue().CreateTimer();
        timer.Interval(std::chrono::seconds(seconds));
        timer.IsRepeating(false);
        timer.Tick([weak](auto &&, auto &&) {
            if (auto bar = weak.get())
                bar.IsOpen(false);
        });
        timer.Start();
        dlg->timers.push_back(timer);
    }
}

/* Take the characters in 'no' out of whatever's typed or pasted in a
 * text box, keeping the caret where it was among what's left */
static void refuse_chars(muxc::TextBox const &box, std::wstring no)
{
    box.TextChanging([no](muxc::TextBox const &b, auto &&) {
        std::wstring t(b.Text());
        if (t.find_first_of(no) == std::wstring::npos)
            return;
        size_t caret = b.SelectionStart(), kept = 0;
        std::wstring out;
        for (size_t i = 0; i < t.size(); i++) {
            if (no.find(t[i]) != std::wstring::npos)
                continue;
            if (i < caret)
                kept++;
            out += t[i];
        }
        b.Text(out);
        b.Select(kept, 0);
    });
}

extern "C" void wui_w_refuse_chars(WuiWidget *w, const wchar_t *chars)
{
    if (w->edit && has_text(chars))
        refuse_chars(w->edit, chars);
}

/* The tree of folders in a prompt (see wui_dlg_prompt_choices): its
 * nodes and their paths ("" for the top), and (weakly, as they have
 * handlers that refer to this) the controls */
struct PromptFolders {
    std::vector<muxc::TreeViewNode> nodes;
    std::vector<std::wstring> paths;
    size_t start = 0;
    winrt::weak_ref<muxc::TreeView> tree;
    winrt::weak_ref<muxc::TextBox> name;
    winrt::weak_ref<mux::FrameworkElement> row, add;

    bool empty() const { return nodes.empty(); }
    size_t index(muxc::TreeViewNode const &n) const
    {
        auto it = std::find(nodes.begin(), nodes.end(), n);
        return it == nodes.end() ? start : size_t(it - nodes.begin());
    }
    /* The path of the folder chosen (and with nothing chosen, still
     * where it started) */
    std::optional<std::wstring> chosen() const
    {
        if (empty())
            return std::nullopt;
        auto t = tree.get();
        return paths[t ? index(t.SelectedNode()) : start];
    }
    muxc::TreeViewNode add_node(size_t parent, std::wstring const &path,
                                std::wstring const &leaf)
    {
        muxc::TreeViewNode node;
        node.Content(winrt::box_value(hstring(leaf)));
        if (parent < nodes.size())
            nodes[parent].Children().Append(node);
        nodes.push_back(node);
        paths.push_back(path);
        return node;
    }
    void editing(bool on)
    {
        if (auto r = row.get())
            r.Visibility(on ? mux::Visibility::Visible :
                         mux::Visibility::Collapsed);
        if (auto a = add.get())
            a.Visibility(on ? mux::Visibility::Collapsed :
                         mux::Visibility::Visible);
        if (auto n = name.get(); n && on) {
            n.Text(L"");
            n.Focus(mux::FocusState::Programmatic);
        }
    }
    /* Make the folder typed in the one chosen (or choose it, if it's
     * there already) */
    void create()
    {
        auto t = tree.get();
        auto n = name.get();
        if (!t || !n)
            return;
        std::wstring leaf(n.Text());
        leaf.erase(0, leaf.find_first_not_of(L" \t"));
        leaf.erase(leaf.find_last_not_of(L" \t") + 1);
        if (leaf.empty())
            return;
        size_t parent = index(t.SelectedNode());
        std::wstring path = paths[parent].empty() ? leaf :
            paths[parent] + L"/" + leaf;
        muxc::TreeViewNode node{nullptr};
        for (size_t i = 0; i < paths.size() && !node; i++)
            if (!_wcsicmp(paths[i].c_str(), path.c_str()))
                node = nodes[i];
        if (!node)
            node = add_node(parent, path, leaf);
        nodes[parent].IsExpanded(true);
        t.SelectedNode(node);
        editing(false);
        t.Focus(mux::FocusState::Programmatic);
    }
};

/* Put the tree of folders in a prompt, with a way to make a new one */
static void prompt_folders(muxc::StackPanel const &sp,
                           std::shared_ptr<PromptFolders> const &f,
                           std::vector<hstring> const &choices,
                           hstring const &label, size_t sel)
{
    std::unordered_map<std::wstring, size_t> by_path;
    for (size_t i = 0; i < choices.size(); i++) {
        std::wstring path(choices[i]);
        size_t slash = path.rfind(L'/');
        if (i == 0) {
            f->add_node(SIZE_MAX, L"", path).IsExpanded(true);
            continue;
        }
        auto parent = slash == std::wstring::npos ? by_path.end() :
            by_path.find(path.substr(0, slash));
        f->add_node(parent != by_path.end() ? parent->second : 0, path,
                    slash == std::wstring::npos ? path :
                    path.substr(slash + 1));
        by_path.emplace(path, i);
    }
    /* (with the way down to the one chosen open) */
    f->start = std::min(sel, f->nodes.size() - 1);
    auto start = f->nodes[f->start];
    for (auto p = start.Parent(); p; p = p.Parent())
        p.IsExpanded(true);

    sp.Children().Append(styled_text(L"BodyTextBlockStyle", label));
    muxc::TreeView tree;
    tree.SelectionMode(muxc::TreeViewSelectionMode::Single);
    tree.RootNodes().Append(f->nodes[0]);
    tree.MaxHeight(240);
    tree.MinHeight(120);
    tree.Loaded([start](wf::IInspectable const &sender, auto &&) {
        sender.as<muxc::TreeView>().SelectedNode(start);
    });
    muxc::Border frame;
    frame.BorderThickness(mux::ThicknessHelper::FromUniformLength(1));
    frame.BorderBrush(theme_brush(L"CardStrokeColorDefaultBrush"));
    frame.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(4));
    frame.Child(tree);
    sp.Children().Append(frame);
    f->tree = winrt::make_weak(tree);

    /* (a button for a new folder, in the one chosen, which turns into a
     * box to type its name in) */
    muxc::Button add;
    muxc::StackPanel content;
    content.Orientation(muxc::Orientation::Horizontal);
    content.Spacing(8);
    content.Children().Append(symbol(0xE8F4, 14));         /* NewFolder */
    muxc::TextBlock text;
    text.Text(L"New folder");
    content.Children().Append(text);
    add.Content(content);
    add.Click([f](auto &&, auto &&) { f->editing(true); });

    muxc::Grid row;
    row.ColumnSpacing(8);
    for (int i = 0; i < 3; i++) {
        muxc::ColumnDefinition c;
        c.Width(i == 0 ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c);
    }
    muxc::TextBox name;
    name.PlaceholderText(L"New folder's name, in the one chosen");
    refuse_chars(name, L"#/\\");      /* (see NAME_REFUSED_W) */
    /* (Return makes it, and Escape doesn't make it; neither does
     * anything to the dialog) */
    name.PreviewKeyDown([f](auto &&, muxi::KeyRoutedEventArgs const &e) {
        if (e.Key() == winrt::Windows::System::VirtualKey::Enter) {
            e.Handled(true);
            f->create();
        } else if (e.Key() == winrt::Windows::System::VirtualKey::Escape) {
            e.Handled(true);
            f->editing(false);
        }
    });
    muxc::Grid::SetColumn(name, 0);
    row.Children().Append(name);
    muxc::Button create;
    create.Content(winrt::box_value(hstring(L"Create")));
    create.Click([f](auto &&, auto &&) { f->create(); });
    muxc::Grid::SetColumn(create, 1);
    row.Children().Append(create);
    muxc::Button cancel;
    cancel.Content(winrt::box_value(hstring(L"Cancel")));
    cancel.Click([f](auto &&, auto &&) { f->editing(false); });
    muxc::Grid::SetColumn(cancel, 2);
    row.Children().Append(cancel);
    row.Visibility(mux::Visibility::Collapsed);

    sp.Children().Append(add);
    sp.Children().Append(row);
    f->name = winrt::make_weak(name);
    f->row = winrt::make_weak(row.as<mux::FrameworkElement>());
    f->add = winrt::make_weak(add.as<mux::FrameworkElement>());
}

extern "C" void wui_dlg_prompt(WuiDlg *dlg, void *wctx, const wchar_t *title,
                               const wchar_t *text, const wchar_t *initial,
                               const wchar_t *refused, const wchar_t *confirm)
{
    /* (the list to choose from is just for this one) */
    auto choices = std::move(dlg->choices);
    dlg->choices.clear();
    if (dlg->prompt_open)
        return;
    auto root = dlg->window.Content().as<mux::FrameworkElement>();
    muxc::ContentDialog cd;
    try {
        cd.Style(mux::Application::Current().Resources().Lookup(
                     winrt::box_value(L"DefaultContentDialogStyle"))
                 .as<mux::Style>());
    } catch (...) {
    }
    cd.XamlRoot(root.XamlRoot());
    cd.RequestedTheme(root.ActualTheme());
    cd.Title(winrt::box_value(hs(title)));
    muxc::StackPanel sp;
    sp.Spacing(12);
    if (has_text(text)) {
        auto t = styled_text(L"BodyTextBlockStyle", hs(text));
        sp.Children().Append(t);
    }
    muxc::TextBox box;
    box.Text(hs(initial));
    box.MinWidth(320);
    /* (all of it selected, to type over; but if it's a folder to put
     * something in, "Clients/", just the place after it) */
    bool in_folder = has_text(initial) && initial[wcslen(initial) - 1] == L'/';
    box.Loaded([box, in_folder](auto &&, auto &&) {
        box.Focus(mux::FocusState::Programmatic);
        if (in_folder)
            box.Select(box.Text().size(), 0);
        else
            box.SelectAll();
    });
    if (has_text(refused))
        refuse_chars(box, refused);
    sp.Children().Append(box);
    auto f = std::make_shared<PromptFolders>();
    if (!choices.empty())
        prompt_folders(sp, f, choices, dlg->choices_label, dlg->choices_sel);
    cd.Content(sp);
    cd.PrimaryButtonText(hs(confirm));
    cd.CloseButtonText(L"Cancel");
    cd.DefaultButton(muxc::ContentDialogButton::Primary);
    dlg->prompt_open = true;
    try {
        auto op = cd.ShowAsync();
        op.Completed([dlg, wctx, box, f](
                         wf::IAsyncOperation<muxc::ContentDialogResult>
                         const &o, wf::AsyncStatus status) {
            if (!is_live(dlg))
                return;
            dlg->prompt_open = false;
            if (status != wf::AsyncStatus::Completed ||
                o.GetResults() != muxc::ContentDialogResult::Primary)
                return;
            dlg->prompt_text = box.Text();
            dlg->prompt_folder = f->chosen();
            fire_ctx(dlg, wctx, WUI_EV_ACTION);
        });
    } catch (...) {
        dlg->prompt_open = false;
        report_exception(dlg, L"asking for some text");
    }
}

extern "C" int wui_dlg_ask(WuiDlg *dlg, const wchar_t *title,
                           const wchar_t *text, const wchar_t *yes,
                           const wchar_t *no)
{
    auto root = dlg->window.Content().try_as<mux::FrameworkElement>();
    if (dlg->prompt_open || !root || !root.XamlRoot())
        return -1;
    struct Wait {
        bool done = false;
        int answer = 0;
    };
    auto wait = std::make_shared<Wait>();
    muxc::ContentDialog cd{nullptr};
    try {
        cd = muxc::ContentDialog();
        try {
            cd.Style(mux::Application::Current().Resources().Lookup(
                         winrt::box_value(L"DefaultContentDialogStyle"))
                     .as<mux::Style>());
        } catch (...) {
        }
        cd.XamlRoot(root.XamlRoot());
        cd.RequestedTheme(root.ActualTheme());
        cd.Title(winrt::box_value(hs(title)));
        auto t = styled_text(L"BodyTextBlockStyle", hs(text));
        cd.Content(t);
        cd.PrimaryButtonText(hs(yes));
        if (has_text(no))
            cd.SecondaryButtonText(hs(no));
        cd.CloseButtonText(L"Cancel");
        cd.DefaultButton(muxc::ContentDialogButton::Primary);

        /* (and wait for it, as a message box would) */
        auto queue = dlg->window.DispatcherQueue();
        auto op = cd.ShowAsync();
        dlg->prompt_open = true;
        op.Completed([wait, queue](
                         wf::IAsyncOperation<muxc::ContentDialogResult>
                         const &o, wf::AsyncStatus status) {
            if (status == wf::AsyncStatus::Completed) {
                auto r = o.GetResults();
                wait->answer = r == muxc::ContentDialogResult::Primary ? 1 :
                    r == muxc::ContentDialogResult::Secondary ? 2 : 0;
            }
            wait->done = true;
            queue.EnqueueEventLoopExit();
        });
        while (!wait->done)
            queue.RunEventLoop(mud::DispatcherRunOptions::QuitOnlyLocalLoop,
                               nullptr);
        dlg->prompt_open = false;
    } catch (...) {
        dlg->prompt_open = false;
        if (!wait->done) {
            try {
                if (cd)
                    cd.Hide();
            } catch (...) {
            }
            return -1;
        }
    }
    return wait->answer;
}

extern "C" bool wui_dlg_pick_font(WuiDlg *dlg, const wchar_t *title,
                                  const wchar_t *const *families,
                                  size_t nfamilies, wchar_t *family,
                                  size_t size, bool *bold, int *points)
{
    using winrt::Windows::UI::Text::FontWeights;
    auto root = dlg->window.Content().try_as<mux::FrameworkElement>();
    if (dlg->prompt_open || !root || !root.XamlRoot())
        return false;

    /*
     * The families, each twice, normal and then bold, each in itself
     * (as in vim's WinUI font dialog, and GTK's): entry n is family
     * n / 2, bold if n is odd.
     */
    muxc::ListView list;
    list.SelectionMode(muxc::ListViewSelectionMode::Single);
    list.MinHeight(100);
    int sel = 0;
    for (size_t i = 0; i < nfamilies; i++) {
        hstring fam(families[i]);
        for (int b = 0; b < 2; b++) {
            muxc::TextBlock t;
            t.Text(b ? fam + L" Bold" : fam);
            try {
                t.FontFamily(muxm::FontFamily(fam));
            } catch (...) {
            }
            t.FontSize(16);
            t.FontWeight(b ? FontWeights::Bold() : FontWeights::Normal());
            list.Items().Append(t);
        }
        if (!_wcsicmp(families[i], family))
            sel = (int)i * 2 + (*bold ? 1 : 0);
    }
    if (nfamilies)
        list.SelectedIndex(sel);

    /* (an editable list of the usual sizes, as in Notepad) */
    muxc::ComboBox sizes;
    sizes.IsEditable(true);
    sizes.Width(110);
    static const int presets[] = {
        6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 48, 72
    };
    int match = -1;
    for (size_t k = 0; k < sizeof(presets) / sizeof(*presets); k++) {
        sizes.Items().Append(winrt::box_value(winrt::to_hstring(presets[k])));
        if (presets[k] == *points)
            match = (int)k;
    }
    if (match >= 0)
        sizes.SelectedIndex(match);
    else
        sizes.Text(winrt::to_hstring(*points));

    muxc::TextBlock preview;
    preview.Text(L"The quick brown fox jumps over the lazy dog\n"
                 L"0123456789  {}[]()<>  ~|\\/  il1I O0");
    preview.TextWrapping(mux::TextWrapping::NoWrap);
    auto chosen_size = [sizes]() {
        int sz = (int)wcstol(sizes.Text().c_str(), nullptr, 10);
        return sz < 1 ? 0 : sz > 200 ? 200 : sz;
    };
    auto refresh = [list, preview, chosen_size, families, nfamilies]() {
        try {
            int i = list.SelectedIndex();
            if (i >= 0 && (size_t)(i / 2) < nfamilies) {
                preview.FontFamily(muxm::FontFamily(hstring(families[i / 2])));
                preview.FontWeight((i & 1) ? FontWeights::Bold() :
                                   FontWeights::Normal());
            }
            int sz = chosen_size();
            /* (points, as near as XAML's units are to them) */
            preview.FontSize((sz ? sz : 10) * 96.0 / 72.0);
        } catch (...) {
        }
    };
    refresh();
    list.SelectionChanged([refresh](auto &&, auto &&) { refresh(); });
    sizes.SelectionChanged([refresh](auto &&, auto &&) { refresh(); });
    sizes.TextSubmitted([refresh](auto &&, auto &&) { refresh(); });

    /* The list, taking what room the rest leaves; the size; and the
     * preview, in a card */
    muxc::Grid content;
    content.RowSpacing(12);
    content.Width(480);
    content.Height(440);
    for (int k = 0; k < 3; k++) {
        muxc::RowDefinition r;
        r.Height(k == 0 ? mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star) :
                 mux::GridLengthHelper::Auto());
        content.RowDefinitions().Append(r);
    }
    muxc::Grid::SetRow(list, 0);
    content.Children().Append(list);
    muxc::StackPanel sizecol;
    sizecol.Spacing(4);
    sizecol.Children().Append(styled_text(L"CaptionTextBlockStyle",
                                          hstring(L"Size (points)"), true));
    sizecol.Children().Append(sizes);
    muxc::Grid::SetRow(sizecol, 1);
    content.Children().Append(sizecol);
    auto card = group_card();
    card.Padding(mux::ThicknessHelper::FromLengths(12, 10, 12, 10));
    card.MaxHeight(140);
    card.Child(preview);
    muxc::Grid::SetRow(card, 2);
    content.Children().Append(card);

    struct Wait {
        bool done = false, ok = false;
    };
    auto wait = std::make_shared<Wait>();
    muxc::ContentDialog cd{nullptr};
    try {
        cd = muxc::ContentDialog();
        try {
            cd.Style(mux::Application::Current().Resources().Lookup(
                         winrt::box_value(L"DefaultContentDialogStyle"))
                     .as<mux::Style>());
        } catch (...) {
        }
        cd.XamlRoot(root.XamlRoot());
        cd.RequestedTheme(root.ActualTheme());
        cd.Title(winrt::box_value(hs(title)));
        cd.Content(content);
        cd.PrimaryButtonText(L"OK");
        cd.CloseButtonText(L"Cancel");
        cd.DefaultButton(muxc::ContentDialogButton::Primary);
        /* (with the font chosen already in sight, and the keyboard in
         * the list) */
        cd.Opened([list, sel](auto &&, auto &&) {
            try {
                if ((uint32_t)sel < list.Items().Size())
                    list.ScrollIntoView(list.Items().GetAt(sel));
                list.Focus(mux::FocusState::Programmatic);
            } catch (...) {
            }
        });

        auto queue = dlg->window.DispatcherQueue();
        auto op = cd.ShowAsync();
        dlg->prompt_open = true;
        op.Completed([wait, queue](
                         wf::IAsyncOperation<muxc::ContentDialogResult>
                         const &o, wf::AsyncStatus status) {
            wait->ok = status == wf::AsyncStatus::Completed &&
                o.GetResults() == muxc::ContentDialogResult::Primary;
            wait->done = true;
            queue.EnqueueEventLoopExit();
        });
        while (!wait->done)
            queue.RunEventLoop(mud::DispatcherRunOptions::QuitOnlyLocalLoop,
                               nullptr);
        dlg->prompt_open = false;
    } catch (...) {
        dlg->prompt_open = false;
        if (!wait->done) {
            try {
                if (cd)
                    cd.Hide();
            } catch (...) {
            }
        }
        return false;
    }

    int i = list.SelectedIndex();
    int sz = chosen_size();
    if (!wait->ok || i < 0 || (size_t)(i / 2) >= nfamilies || !sz)
        return false;
    wcsncpy(family, families[i / 2], size - 1);
    family[size - 1] = L'\0';
    *bold = (i & 1) != 0;
    *points = sz;
    return true;
}

extern "C" bool wui_dlg_pick_colour(WuiDlg *dlg, const wchar_t *title,
                                    unsigned *rgb)
{
    auto root = dlg->window.Content().try_as<mux::FrameworkElement>();
    if (dlg->prompt_open || !root || !root.XamlRoot())
        return false;
    struct Wait {
        bool done = false, ok = false;
    };
    auto wait = std::make_shared<Wait>();
    muxc::ContentDialog cd{nullptr};
    muxc::ColorPicker picker;
    picker.IsAlphaEnabled(false);
    picker.IsMoreButtonVisible(false);
    picker.IsColorChannelTextInputVisible(true);
    picker.IsHexInputVisible(true);
    picker.Color(winrt::Windows::UI::ColorHelper::FromArgb(
                     255, (*rgb >> 16) & 0xFF, (*rgb >> 8) & 0xFF,
                     *rgb & 0xFF));
    try {
        cd = muxc::ContentDialog();
        try {
            cd.Style(mux::Application::Current().Resources().Lookup(
                         winrt::box_value(L"DefaultContentDialogStyle"))
                     .as<mux::Style>());
        } catch (...) {
        }
        cd.XamlRoot(root.XamlRoot());
        cd.RequestedTheme(root.ActualTheme());
        cd.Title(winrt::box_value(hs(title)));
        muxc::ScrollViewer sv;
        sv.Content(picker);
        cd.Content(sv);
        cd.PrimaryButtonText(L"OK");
        cd.CloseButtonText(L"Cancel");
        cd.DefaultButton(muxc::ContentDialogButton::Primary);

        /* (and wait for it, as the colour dialog box would) */
        auto queue = dlg->window.DispatcherQueue();
        auto op = cd.ShowAsync();
        dlg->prompt_open = true;
        op.Completed([wait, queue](
                         wf::IAsyncOperation<muxc::ContentDialogResult>
                         const &o, wf::AsyncStatus status) {
            wait->ok = status == wf::AsyncStatus::Completed &&
                o.GetResults() == muxc::ContentDialogResult::Primary;
            wait->done = true;
            queue.EnqueueEventLoopExit();
        });
        while (!wait->done)
            queue.RunEventLoop(mud::DispatcherRunOptions::QuitOnlyLocalLoop,
                               nullptr);
        dlg->prompt_open = false;
    } catch (...) {
        dlg->prompt_open = false;
        if (!wait->done) {
            try {
                if (cd)
                    cd.Hide();
            } catch (...) {
            }
        }
        return false;
    }
    if (!wait->ok)
        return false;
    auto c = picker.Color();
    *rgb = ((unsigned)c.R << 16) | ((unsigned)c.G << 8) | c.B;
    return true;
}

extern "C" void wui_dlg_prompt_choices(WuiDlg *dlg, const wchar_t *label,
                                       const wchar_t *const *items, size_t n,
                                       size_t sel)
{
    dlg->choices_label = hs(label);
    dlg->choices.clear();
    for (size_t i = 0; i < n; i++)
        dlg->choices.push_back(hs(items[i]));
    dlg->choices_sel = sel;
}

extern "C" size_t wui_dlg_prompt_folder(WuiDlg *dlg, wchar_t *buf,
                                       size_t size)
{
    std::wstring s = dlg->prompt_folder.value_or(L"");
    if (buf && size > 0) {
        size_t n = std::min(s.size(), size - 1);
        std::copy(s.begin(), s.begin() + n, buf);
        buf[n] = L'\0';
    }
    return s.size();
}

extern "C" size_t wui_dlg_prompt_text(WuiDlg *dlg, wchar_t *buf, size_t size)
{
    hstring const &s = dlg->prompt_text;
    if (buf && size > 0) {
        size_t n = std::min((size_t)s.size(), size - 1);
        std::copy(s.begin(), s.begin() + n, buf);
        buf[n] = L'\0';
    }
    return s.size();
}

/* ---- A search box, and rows ---- */

static void rows_focus(WuiWidget *rows);

extern "C" WuiWidget *wui_w_search(WuiWidget *grp, void *wctx,
                                   const wchar_t *placeholder,
                                   const wchar_t *button)
{
    WuiWidget *w = new_widget(grp, Kind::Edit);
    w->wctx = wctx;
    w->edit = muxc::TextBox();
    w->edit.PlaceholderText(hs(placeholder));
    w->edit.VerticalAlignment(mux::VerticalAlignment::Center);
    hook_text_box(w, w->edit);
    w->edit.KeyDown([w](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        if (w->dead)
            return;
        if (e.Key() == VirtualKey::Enter) {
            e.Handled(true);
            fire_later(w, WUI_EV_ACTION);
        } else if (e.Key() == VirtualKey::Down && w->next) {
            e.Handled(true);
            rows_focus(w->next);
        }
    });
    w->button = xaml<muxc::Button>(
        L"Button", L"Style='{StaticResource AccentButtonStyle}'");
    w->button.Content(winrt::box_value(hs(button)));
    w->button.MinWidth(100);
    w->button.VerticalAlignment(mux::VerticalAlignment::Center);
    w->button.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_ACTION); });

    muxc::Grid g;
    g.ColumnSpacing(8);
    muxc::ColumnDefinition c0, c1;
    c0.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    c1.Width(mux::GridLengthHelper::Auto());
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    muxc::Grid::SetColumn(w->edit, 0);
    muxc::Grid::SetColumn(w->button, 1);
    g.Children().Append(w->edit);
    g.Children().Append(w->button);
    w->root = g;
    add(grp, w);
    return w;
}

extern "C" void wui_w_search_next(WuiWidget *search, WuiWidget *next)
{
    search->next = next;
}

static WuiWidget::RowInfo *find_row(WuiWidget *w, wf::IInspectable const &o)
{
    for (auto &r : w->rowinfo)
        if (r.item && (r.item == o || r.item.Content() == o))
            return &r;
    return nullptr;
}

static WuiWidget::RowInfo *find_row_id(WuiWidget *w, int id)
{
    for (auto &r : w->rowinfo)
        if (r.id == id && r.item)
            return &r;
    return nullptr;
}

/* The selected row (in whichever card it's in), or null */
static WuiWidget::RowInfo *rows_sel(WuiWidget *w)
{
    for (auto &l : w->lists)
        if (auto sel = l.SelectedItem())
            return find_row(w, sel);
    return nullptr;
}

/* Select a row (and nothing in the other cards) */
static void rows_select(WuiWidget *w, int id)
{
    auto r = find_row_id(w, id);
    if (!r)
        return;
    w->quiet++;
    for (auto &l : w->lists)
        if (l != r->list)
            l.SelectedIndex(-1);
    r->list.SelectedItem(r->item);
    w->quiet--;
}

static void rows_focus_row(WuiWidget *w, WuiWidget::RowInfo &r)
{
    rows_select(w, r.id);
    r.item.Focus(mux::FocusState::Keyboard);
}

/* Put the focus on the selected row, or the first there is */
static void rows_focus(WuiWidget *rows)
{
    if (rows->dead)
        return;
    if (auto r = rows_sel(rows)) {
        r->item.Focus(mux::FocusState::Keyboard);
        return;
    }
    for (auto &r : rows->rowinfo) {
        if (r.item) {
            rows_focus_row(rows, r);
            return;
        }
    }
}

/* What a rows widget's action is for */
enum { ROWS_ON_ITEM, ROWS_ON_FOLDER, ROWS_ON_LIST };

static int rows_on(WuiRowKind kind)
{
    return kind == WUI_ROW_ITEM ? ROWS_ON_ITEM :
        kind == WUI_ROW_FOLDER ? ROWS_ON_FOLDER : -1;
}

static bool rows_have_actions(WuiWidget *w, int on)
{
    for (auto &a : w->rows_actions)
        if (a.on == on)
            return true;
    return false;
}

/* A row's menu of actions (or, for ROWS_ON_LIST, the list's, with no
 * row to select, and id -1) */
static muxc::MenuFlyout rows_menu(WuiWidget *w, int on, int id, bool marked)
{
    muxc::MenuFlyout menu;
    winrt::weak_ref<muxp::FlyoutBase> wmenu = winrt::make_weak(
        menu.as<muxp::FlyoutBase>());
    for (auto &a : w->rows_actions) {
        if (a.on != on)
            continue;
        if (a.label.empty()) {
            menu.Items().Append(muxc::MenuFlyoutSeparator());
            continue;
        }
        muxc::MenuFlyoutItem mi;
        bool alt = marked && !a.alt_label.empty();
        mi.Text(alt ? a.alt_label : a.label);
        unsigned glyph = alt && a.alt_glyph ? a.alt_glyph : a.glyph;
        if (glyph)
            mi.Icon(symbol(glyph));
        if (a.key == VK_F2)
            mi.KeyboardAcceleratorTextOverride(L"F2");
        else if (a.key == VK_DELETE)
            mi.KeyboardAcceleratorTextOverride(L"Del");
        void *target = a.target;
        /* (once the menu has closed, as Duplicate... and the like open
         * a ContentDialog: see wui-flyout.h) */
        mi.Click([w, id, target, wmenu](auto &&, auto &&) {
            if (w->dead)
                return;
            if (id >= 0)
                rows_select(w, id);
            wui_after_flyout(wmenu, [w, target]() {
                if (!w->dead)
                    fire_for(w, target, WUI_EV_ACTION);
            });
        });
        menu.Items().Append(mi);
    }
    return menu;
}

/* The next row after (dir > 0) or before the selected one that isn't a
 * heading, whichever card it's in, or null */
static WuiWidget::RowInfo *rows_step(WuiWidget *w, int dir)
{
    auto sel = rows_sel(w);
    if (!sel)
        return nullptr;
    int n = (int)w->rowinfo.size();
    int i = (int)(sel - w->rowinfo.data());
    for (i += dir; i >= 0 && i < n; i += dir)
        if (w->rowinfo[i].item)
            return &w->rowinfo[i];
    return nullptr;
}

/*
 * The list in a card of rows. Each card has its own, but they work as
 * one: one row is selected in them all, and the arrow keys go from
 * the end of one to the start of the next.
 */
static muxc::ListView rows_new_list(WuiWidget *w)
{
    auto list = xaml<muxc::ListView>(
        L"ListView", L"BorderThickness='0' Padding='4,4,4,4' "
        L"Background='Transparent'");
    list.SelectionMode(muxc::ListViewSelectionMode::Single);
    list.IsItemClickEnabled(true);
    list.CanDragItems(w->draggable);
    /* (all of it shows, and the page scrolls, not the list, so that
     * the mouse wheel scrolls the page even over the list) */
    muxc::ScrollViewer::SetVerticalScrollMode(list,
                                              muxc::ScrollMode::Disabled);
    muxc::ScrollViewer::SetVerticalScrollBarVisibility(
        list, muxc::ScrollBarVisibility::Disabled);
    list.ItemClick([w](auto &&, muxc::ItemClickEventArgs const &e) {
        if (w->dead)
            return;
        auto r = find_row(w, e.ClickedItem());
        if (!r)
            return;
        rows_select(w, r->id);
        /* (an item is just selected by a click; it's double-clicking
         * it, or Return, that does it) */
        if (r->kind != WUI_ROW_ITEM) {
            w->activated = r->id;
            fire_later(w, WUI_EV_ACTION);
        }
    });
    list.SelectionChanged([w](wf::IInspectable const &sender, auto &&) {
        if (w->dead || w->quiet)
            return;
        auto list = sender.as<muxc::ListView>();
        if (!list.SelectedItem())
            return;
        w->quiet++;
        for (auto &l : w->lists)
            if (l != list)
                l.SelectedIndex(-1);
        w->quiet--;
    });
    list.DoubleTapped([w](auto &&, muxi::DoubleTappedRoutedEventArgs
                          const &e) {
        if (w->dead)
            return;
        /* (the item it's in, unless it's in a button in that, such as
         * its menu's) */
        auto o = e.OriginalSource().try_as<mux::DependencyObject>();
        for (; o; o = muxm::VisualTreeHelper::GetParent(o)) {
            if (o.try_as<muxp::ButtonBase>())
                return;
            if (auto item = o.try_as<muxc::ListViewItem>()) {
                auto r = find_row(w, item);
                if (r && r->kind == WUI_ROW_ITEM) {
                    e.Handled(true);
                    rows_select(w, r->id);
                    w->activated = r->id;
                    fire_later(w, WUI_EV_ACTION);
                }
                return;
            }
        }
    });
    list.PreviewKeyDown([w](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        if (w->dead)
            return;
        auto r = rows_sel(w);
        if (!r)
            return;
        if (e.Key() == VirtualKey::Enter && r->kind == WUI_ROW_ITEM) {
            e.Handled(true);
            w->activated = r->id;
            fire_later(w, WUI_EV_ACTION);
        } else if (e.Key() == VirtualKey::Down ||
                   e.Key() == VirtualKey::Up) {
            /* (on into the next card, or back to the one before) */
            int dir = e.Key() == VirtualKey::Down ? 1 : -1;
            auto next = rows_step(w, dir);
            if (next && next->list != r->list) {
                e.Handled(true);
                rows_focus_row(w, *next);
            }
        }
    });
    list.KeyDown([w](auto &&, muxi::KeyRoutedEventArgs const &e) {
        if (w->dead)
            return;
        int key = (int)e.Key();
        auto r = rows_sel(w);
        if (!r || rows_on(r->kind) < 0)
            return;
        for (auto &a : w->rows_actions) {
            if (a.on == rows_on(r->kind) && a.key && a.key == key) {
                e.Handled(true);
                fire_for(w, a.target, WUI_EV_ACTION);
                return;
            }
        }
    });

    /* Dragging a row (if wui_w_rows_draggable says it can be) onto
     * another (see rows_drop_target), or anywhere else in the widget */
    list.DragItemsStarting([w](auto &&,
                               muxc::DragItemsStartingEventArgs const &e) {
        auto items = e.Items();
        auto r = !w->dead && w->draggable && items.Size() == 1 ?
            find_row(w, items.GetAt(0)) : nullptr;
        if (!r || rows_on(r->kind) < 0) {
            e.Cancel(true);
            return;
        }
        w->drag_id = r->id;
        e.Data().SetText(L"");
        e.Data().RequestedOperation(wadt::DataPackageOperation::Move);
    });
    list.DragItemsCompleted([w](auto &&, auto &&) { w->drag_id = -1; });

    auto card = group_card();
    card.Child(list);
    w->sections.Children().Append(card);
    w->lists.push_back(list);
    return list;
}

extern "C" WuiWidget *wui_w_rows(WuiWidget *grp, void *wctx,
                                 const wchar_t *empty)
{
    WuiWidget *w = new_widget(grp, Kind::List);
    w->wctx = wctx;
    w->sections = muxc::StackPanel();

    w->empty = styled_text(L"BodyTextBlockStyle", hs(empty), true);
    w->empty.Margin(mux::ThicknessHelper::FromLengths(24, 12, 24, 12));
    muxc::StackPanel sp;
    /* (with a background, so that all of it can be right-clicked, or
     * dropped on) */
    sp.Background(muxm::SolidColorBrush(
                      winrt::Microsoft::UI::Colors::Transparent()));
    sp.Padding(mux::ThicknessHelper::FromLengths(0, 0, 0, 8));
    sp.Children().Append(w->sections);
    sp.Children().Append(w->empty);
    sp.AllowDrop(true);
    sp.DragOver([w](auto &&, mux::DragEventArgs const &e) {
        if (w->dead || w->drag_id < 0)
            return;
        e.AcceptedOperation(wadt::DataPackageOperation::Move);
        e.DragUIOverride().Caption(L"Move to the top level");
        e.Handled(true);
    });
    sp.Drop([w](auto &&, mux::DragEventArgs const &e) {
        if (w->dead || w->drag_id < 0)
            return;
        e.Handled(true);
        w->drop_what = w->drag_id;
        w->drop_where = -1;
        fire_later(w, WUI_EV_DROP);
    });
    w->root = sp;
    add(grp, w);
    return w;
}

extern "C" void wui_w_rows_action(WuiWidget *w, const wchar_t *label,
                                  const wchar_t *alt_label, unsigned glyph,
                                  unsigned alt_glyph, void *target, int key)
{
    w->rows_actions.push_back({ROWS_ON_ITEM, hs(label), hs(alt_label),
                               glyph, alt_glyph, target, key});
}

extern "C" void wui_w_rows_folder_action(WuiWidget *w, const wchar_t *label,
                                         unsigned glyph, void *target,
                                         int key)
{
    w->rows_actions.push_back({ROWS_ON_FOLDER, hs(label), hstring(), glyph,
                               0, target, key});
}

extern "C" void wui_w_rows_list_action(WuiWidget *w, const wchar_t *label,
                                       unsigned glyph, void *target)
{
    w->rows_actions.push_back({ROWS_ON_LIST, hs(label), hstring(), glyph,
                               0, target, 0});
    w->root.ContextFlyout(rows_menu(w, ROWS_ON_LIST, -1, false));
}

extern "C" int wui_w_rows_activated(WuiWidget *w)
{
    return w->activated;
}

extern "C" void wui_w_rows_draggable(WuiWidget *w, bool draggable)
{
    w->draggable = draggable;
    for (auto &l : w->lists)
        l.CanDragItems(draggable);
}

extern "C" void wui_w_rows_dropped(WuiWidget *w, int *what, int *where)
{
    *what = w->drop_what;
    *where = w->drop_where;
}

/* A row that rows can be dropped on (highlighted while one's over it) */
static void rows_drop_target(WuiWidget *w, muxc::ListViewItem const &item,
                             int id, WuiRowKind kind, hstring const &title)
{
    item.AllowDrop(true);
    auto over = [w, id]() {
        return !w->dead && w->drag_id >= 0 && w->drag_id != id;
    };
    auto unlight = [](wf::IInspectable const &sender) {
        sender.as<muxc::ListViewItem>().ClearValue(
            muxc::Control::BackgroundProperty());
    };
    item.DragEnter([over](wf::IInspectable const &sender, auto &&) {
        if (over())
            sender.as<muxc::ListViewItem>().Background(
                theme_brush(L"SubtleFillColorSecondaryBrush"));
    });
    item.DragLeave([unlight](wf::IInspectable const &sender, auto &&) {
        unlight(sender);
    });
    item.DragOver([kind, title, over](auto &&, mux::DragEventArgs const &e) {
        e.Handled(true);
        if (!over()) {
            e.AcceptedOperation(wadt::DataPackageOperation::None);
            return;
        }
        e.AcceptedOperation(wadt::DataPackageOperation::Move);
        e.DragUIOverride().Caption(kind == WUI_ROW_FOLDER ?
                                   hstring(L"Move into ") + title :
                                   hstring(L"Move beside ") + title);
    });
    item.Drop([w, id, over, unlight](wf::IInspectable const &sender,
                                     mux::DragEventArgs const &e) {
        unlight(sender);
        e.Handled(true);
        if (!over())
            return;
        w->drop_what = w->drag_id;
        w->drop_where = id;
        fire_later(w, WUI_EV_DROP);
    });
}

extern "C" void wui_w_rows_clear(WuiWidget *w)
{
    w->quiet++;
    w->sections.Children().Clear();
    w->lists.clear();
    w->rowinfo.clear();
    w->quiet--;
    w->empty.Visibility(mux::Visibility::Visible);
}

/* A heading above a card of rows: just its title, or (if it has an
 * id) something to click on, to show or hide what's under it */
static mux::FrameworkElement rows_heading(WuiWidget *w, int id,
                                          unsigned glyph,
                                          const wchar_t *title,
                                          const wchar_t *detail)
{
    auto t = styled_text(L"BodyStrongTextBlockStyle", hs(title));
    auto margin = mux::ThicknessHelper::FromLengths(
        0, w->sections.Children().Size() ? 16 : 4, 0, 6);
    /* (with no menu: not even the list's, on right-clicking it) */
    auto no_menu = [](auto &&, muxi::ContextRequestedEventArgs const &e) {
        e.Handled(true);
    };
    if (id < 0) {
        t.Margin(margin);
        t.Padding(mux::ThicknessHelper::FromLengths(8, 0, 8, 0));
        t.ContextRequested(no_menu);
        return t;
    }

    muxc::Grid g;
    g.ColumnSpacing(8);
    for (int i = 0; i < 3; i++) {
        muxc::ColumnDefinition c;
        c.Width(i == 1 ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c);
    }
    auto place = [&](mux::FrameworkElement e, int col) {
        e.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(e, col);
        g.Children().Append(e);
    };
    if (glyph)
        place(symbol(glyph, 10), 0);
    place(t, 1);
    if (has_text(detail)) {
        auto dt = styled_text(L"CaptionTextBlockStyle", hs(detail), true);
        dt.TextWrapping(mux::TextWrapping::NoWrap);
        place(dt, 2);
    }
    auto b = xaml<muxc::Button>(
        L"Button", L"HorizontalAlignment='Stretch' "
        L"HorizontalContentAlignment='Stretch' Padding='8,2,8,2' "
        L"Background='Transparent' BorderThickness='0'");
    b.Margin(margin);
    b.Content(g);
    b.ContextRequested(no_menu);
    b.Click([w, id](auto &&, auto &&) {
        if (w->dead)
            return;
        w->activated = id;
        fire_later(w, WUI_EV_ACTION);
    });
    return b;
}

extern "C" void wui_w_rows_add(WuiWidget *w, int id, WuiRowKind kind,
                               int indent, unsigned glyph,
                               const wchar_t *title, const wchar_t *subtitle,
                               const wchar_t *detail, bool marked)
{
    if (kind == WUI_ROW_HEADING) {
        w->sections.Children().Append(
            rows_heading(w, id, glyph, title, detail));
        w->rowinfo.push_back({id, kind, marked, nullptr, nullptr});
        return;
    }

    muxc::ListViewItem item;
    item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    item.Padding(mux::ThicknessHelper::FromLengths(8 + 24 * indent, 0,
                                                   8, 0));
    item.Tag(winrt::box_value(id));

    muxc::Grid g;
    g.ColumnSpacing(16);
    for (int i = 0; i < 4; i++) {
        muxc::ColumnDefinition c;
        c.Width(i == 1 ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c);
    }
    auto place = [&](mux::FrameworkElement e, int col) {
        e.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(e, col);
        g.Children().Append(e);
    };
    if (glyph)
        place(symbol(glyph, 16), 0);
    muxc::StackPanel text;
    muxc::TextBlock t;
    t.Text(hs(title));
    t.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    text.Children().Append(t);
    if (has_text(subtitle)) {
        auto st = styled_text(L"CaptionTextBlockStyle", hs(subtitle), true);
        st.TextWrapping(mux::TextWrapping::NoWrap);
        st.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        text.Children().Append(st);
    }
    place(text, 1);
    if (has_text(detail)) {
        auto dt = styled_text(L"CaptionTextBlockStyle", hs(detail), true);
        dt.TextWrapping(mux::TextWrapping::NoWrap);
        place(dt, 2);
    }
    int on = rows_on(kind);
    if (on >= 0 && rows_have_actions(w, on)) {
        auto more = xaml<muxc::Button>(
            L"Button", L"Width='28' Height='24' Padding='0' "
            L"Background='Transparent' BorderThickness='0'");
        more.Content(symbol(0xE712, 14));            /* More */
        muxc::ToolTipService::SetToolTip(
            more, winrt::box_value(hstring(L"More options")));
        more.Flyout(rows_menu(w, on, id, marked));
        place(more, 3);
        item.ContextFlyout(rows_menu(w, on, id, marked));
    }
    if (on >= 0)
        rows_drop_target(w, item, id, kind, hs(title));
    item.Content(g);
    item.MinHeight(has_text(subtitle) ? 40 : 30);

    /* (in the card after the last heading, or a new one if there's
     * been a heading since) */
    bool new_card = w->rowinfo.empty() ||
        w->rowinfo.back().kind == WUI_ROW_HEADING;
    auto list = new_card ? rows_new_list(w) : w->rowinfo.back().list;
    w->quiet++;
    w->rowinfo.push_back({id, kind, marked, item, list});
    list.Items().Append(item);
    w->quiet--;
    w->empty.Visibility(mux::Visibility::Collapsed);
}

/* (a new one each time: an element can only be in one tooltip) */
static muxc::TextBlock italic_text(const wchar_t *text)
{
    muxc::TextBlock t;
    t.Text(hs(text));
    t.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
    return t;
}

extern "C" void wui_w_rows_tooltip(WuiWidget *w, int id, const wchar_t *tip)
{
    auto r = find_row_id(w, id);
    if (!r || !r->item)
        return;
    /* (on what's in the row, too, which is what the pointer is over;
     * with a background, so the gaps between its parts count) */
    if (auto g = r->item.Content().try_as<muxc::Panel>()) {
        if (!g.Background())
            g.Background(muxm::SolidColorBrush(
                winrt::Microsoft::UI::Colors::Transparent()));
        muxc::ToolTipService::SetToolTip(g, italic_text(tip));
    }
    muxc::ToolTipService::SetToolTip(r->item, italic_text(tip));
}

extern "C" int wui_w_rows_selected(WuiWidget *w)
{
    auto r = rows_sel(w);
    return r ? r->id : -1;
}

extern "C" void wui_w_rows_select(WuiWidget *w, int id)
{
    rows_select(w, id);
    if (auto r = find_row_id(w, id))
        r->list.ScrollIntoView(r->item);
}

/* ---- Settings that aren't their defaults ---- */

extern "C" void wui_w_set_modified(WuiWidget *w, bool modified)
{
    if (w->reset)
        w->reset.Visibility(modified ? mux::Visibility::Visible :
                            mux::Visibility::Collapsed);
    if (w->reset_item)
        w->reset_item.IsEnabled(modified);
}

extern "C" void wui_w_setting_menu(WuiWidget *w, bool apply_all)
{
    if (!w->root)
        return;
    muxc::MenuFlyout menu;
    w->reset_item = muxc::MenuFlyoutItem();
    w->reset_item.Text(L"Reset to default");
    w->reset_item.Icon(symbol(0xE7A7));                   /* Undo */
    w->reset_item.IsEnabled(w->reset &&
                            w->reset.Visibility() == mux::Visibility::Visible);
    w->reset_item.Click([w](auto &&, auto &&) {
        fire_later(w, WUI_EV_RESET);
    });
    menu.Items().Append(w->reset_item);
    if (apply_all) {
        muxc::MenuFlyoutItem mi;
        mi.Text(L"Use this in all saved sessions...");
        mi.Icon(symbol(0xE8C8));                          /* Copy */
        mi.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_APPLYALL); });
        menu.Items().Append(mi);
    }
    w->root.ContextFlyout(menu);
}

/* The content of a list item: split at tabs, into columns */
static mux::UIElement list_item_columns(WuiWidget *w, const wchar_t *text);

/* ... after an icon, if the list has them */
static mux::UIElement list_item_content(WuiWidget *w, const wchar_t *text)
{
    auto content = list_item_columns(w, text);
    if (!w->item_glyph)
        return content;
    muxc::Grid g;
    g.ColumnSpacing(16);
    muxc::ColumnDefinition c0, c1;
    c0.Width(mux::GridLengthHelper::Auto());
    c1.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    auto icon = symbol(w->item_glyph, 16);
    icon.VerticalAlignment(mux::VerticalAlignment::Center);
    content.as<mux::FrameworkElement>().VerticalAlignment(
        mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(icon, 0);
    muxc::Grid::SetColumn(content.as<mux::FrameworkElement>(), 1);
    g.Children().Append(icon);
    g.Children().Append(content);
    return g;
}

static mux::UIElement list_item_columns(WuiWidget *w, const wchar_t *text)
{
    std::wstring s = text ? text : L"";
    if (w->ncols <= 1 || s.find(L'\t') == std::wstring::npos) {
        muxc::TextBlock t;
        t.Text(hstring(s));
        t.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        return t;
    }
    muxc::Grid g;
    g.ColumnSpacing(8);
    for (int i = 0; i < w->ncols; i++) {
        muxc::ColumnDefinition c;
        int p = i < (int)w->percentages.size() ? w->percentages[i] : 1;
        c.Width(mux::GridLengthHelper::FromValueAndType(
                    std::max(p, 1), mux::GridUnitType::Star));
        g.ColumnDefinitions().Append(c);
    }
    size_t start = 0;
    for (int i = 0; i < w->ncols; i++) {
        size_t tab = s.find(L'\t', start);
        std::wstring piece = (i == w->ncols - 1 || tab == std::wstring::npos) ?
            s.substr(start) : s.substr(start, tab - start);
        muxc::TextBlock t;
        t.Text(hstring(piece));
        t.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        muxc::Grid::SetColumn(t, i);
        g.Children().Append(t);
        if (tab == std::wstring::npos)
            break;
        start = tab + 1;
    }
    return g;
}

/* Move the selected item of a drag list up (dir < 0) or down */
static void list_move(WuiWidget *w, int dir)
{
    if (w->dead)
        return;
    int i = w->list.SelectedIndex();
    int j = i + dir;
    int n = (int)w->list.Items().Size();
    if (i < 0 || j < 0 || j >= n)
        return;
    w->quiet++;
    auto items = w->list.Items();
    auto item = items.GetAt(i);
    items.RemoveAt(i);
    items.InsertAt(j, item);
    std::swap(w->ids[i], w->ids[j]);
    std::swap(w->texts[i], w->texts[j]);
    w->list.SelectedIndex(j);
    w->quiet--;
    w->list.ScrollIntoView(item);
    fire(w, WUI_EV_VALCHANGE);
}

extern "C" WuiWidget *wui_w_list(WuiWidget *grp, void *wctx,
                                 const wchar_t *label, int height,
                                 bool multisel, bool draglist,
                                 int ncols, const int *percentages)
{
    WuiWidget *w = new_widget(grp, Kind::List);
    w->wctx = wctx;
    w->ncols = ncols;
    if (percentages)
        w->percentages.assign(percentages, percentages + ncols);

    if (height == 0) {
        w->combo = muxc::ComboBox();
        /* (in a shared row, just as wide as its choice needs) */
        w->combo.MinWidth(w->in_row ? 120 : 200);
        w->combo.MaxWidth(CONTROL_WIDTH * 1.5);
        w->combo.SelectionChanged([w](auto &&, auto &&) {
            if (w->dead)
                return;
            if (!w->quiet)
                fire(w, WUI_EV_SELCHANGE);
        });
        w->root = setting_row(w, label, w->combo, false);
        add(grp, w);
        return w;
    }

    /* (In a card of its own, the card is its frame, and its items line
     * up with the card's title) */
    w->list = w->flush ?
        xaml<muxc::ListView>(
            L"ListView", L"BorderThickness='0' Padding='8,0,8,0' "
            L"Background='Transparent'") :
        xaml<muxc::ListView>(
            L"ListView", L"BorderThickness='1' CornerRadius='4' "
            L"BorderBrush='{ThemeResource ControlStrokeColorDefaultBrush}' "
            L"Background='{ThemeResource ControlFillColorDefaultBrush}'");
    w->height = height;
    w->list.Height(height * LIST_ROW + 8);
    w->list.SelectionMode(multisel ? muxc::ListViewSelectionMode::Extended :
                          muxc::ListViewSelectionMode::Single);
    w->list.SelectionChanged([w](auto &&, auto &&) {
        if (w->dead)
            return;
        if (!w->quiet)
            fire(w, WUI_EV_SELCHANGE);
    });
    w->list.DoubleTapped([w](auto &&, auto &&) {
        if (w->dead)
            return;
        if (w->list.SelectedIndex() >= 0)
            fire_later(w, WUI_EV_ACTION);
    });
    mux::FrameworkElement ctl = w->list;

    if (draglist) {
        muxc::Grid g;
        g.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1;
        c0.Width(mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star));
        c1.Width(mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c0);
        g.ColumnDefinitions().Append(c1);
        muxc::StackPanel buttons;
        buttons.Spacing(8);
        const unsigned glyphs[2] = {0xE70E, 0xE70D};   /* ChevronUp, Down */
        const wchar_t *names[2] = {L"Move up", L"Move down"};
        for (int d = 0; d < 2; d++) {
            muxc::Button b;
            b.Content(symbol(glyphs[d], 12));
            muxc::ToolTipService::SetToolTip(
                b, winrt::box_value(hstring(names[d])));
            int dir = d ? +1 : -1;
            b.Click([w, dir](auto &&, auto &&) { list_move(w, dir); });
            buttons.Children().Append(b);
        }
        muxc::Grid::SetColumn(w->list, 0);
        muxc::Grid::SetColumn(buttons, 1);
        g.Children().Append(w->list);
        g.Children().Append(buttons);
        ctl = g;
    }
    w->root = setting_row(w, label, ctl, true);
    add(grp, w);
    return w;
}

extern "C" WuiWidget *wui_w_filesel(WuiWidget *grp, void *wctx,
                                    const wchar_t *label, bool just_button)
{
    if (just_button) {
        /* Just a button, and in a row with any others */
        return make_button(grp, Kind::FileSel, wctx, label, false, false,
                           WUI_EV_BROWSE);
    }

    WuiWidget *w = new_widget(grp, Kind::FileSel);
    w->wctx = wctx;
    w->button = muxc::Button();
    w->button.Content(winrt::box_value(hstring(L"Browse...")));
    w->button.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_BROWSE); });
    w->edit = muxc::TextBox();
    w->edit.Width(CONTROL_WIDTH);
    hook_text_box(w, w->edit);
    muxc::StackPanel sp;
    sp.Orientation(muxc::Orientation::Horizontal);
    sp.Spacing(8);
    sp.Children().Append(w->edit);
    sp.Children().Append(w->button);
    w->root = setting_row(w, label, sp, false);
    add(grp, w);
    return w;
}

extern "C" WuiWidget *wui_w_fontsel(WuiWidget *grp, void *wctx,
                                    const wchar_t *label)
{
    WuiWidget *w = new_widget(grp, Kind::FontSel);
    w->wctx = wctx;
    w->button = muxc::Button();
    w->button.Content(winrt::box_value(hstring(L"Change...")));
    w->button.MinWidth(100);
    w->button.Click([w](auto &&, auto &&) { fire_later(w, WUI_EV_BROWSE); });
    w->root = setting_row(w, label, w->button, false, true);
    add(grp, w);
    return w;
}

/* ---- A table of steps ---- */

/* The columns: number, Active, Expected, Command, Hide, and the
 * buttons to move and delete the row (negative for a share of what's
 * left) */
static const double STEP_COLUMNS[] = {28, 56, -1, -1, 48, 112};

static muxc::Grid step_grid()
{
    muxc::Grid g;
    g.ColumnSpacing(8);
    for (double width : STEP_COLUMNS) {
        muxc::ColumnDefinition c;
        c.Width(width < 0 ? mux::GridLengthHelper::FromValueAndType(
                                -width, mux::GridUnitType::Star) :
                mux::GridLengthHelper::FromPixels(width));
        g.ColumnDefinitions().Append(c);
    }
    return g;
}

static void step_cell(muxc::Grid g, mux::FrameworkElement e, int col)
{
    e.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(e, col);
    g.Children().Append(e);
}

static void steps_changed(WuiWidget *w)
{
    if (!w->dead && !w->quiet)
        fire(w, WUI_EV_VALCHANGE);
}

/* Put w->steps in the order the list shows them (which dragging can
 * change), and number them */
static void steps_renumber(WuiWidget *w)
{
    std::vector<std::shared_ptr<StepRow>> order;
    auto items = w->list.Items();
    for (uint32_t i = 0; i < items.Size(); i++) {
        auto item = items.GetAt(i);
        for (auto &r : w->steps)
            if (r->item == item) {
                order.push_back(r);
                break;
            }
    }
    w->steps = order;
    for (size_t i = 0; i < w->steps.size(); i++)
        w->steps[i]->number.Text(winrt::to_hstring((int)i + 1));
}

static void step_move(WuiWidget *w, std::weak_ptr<StepRow> wr, int dir)
{
    auto r = wr.lock();
    if (w->dead || !r)
        return;
    auto items = w->list.Items();
    uint32_t i;
    if (!items.IndexOf(r->item, i))
        return;
    int j = (int)i + dir;
    if (j < 0 || j >= (int)items.Size())
        return;
    items.RemoveAt(i);
    items.InsertAt((uint32_t)j, r->item);
    steps_renumber(w);
    steps_changed(w);
}

static void step_remove(WuiWidget *w, std::weak_ptr<StepRow> wr)
{
    auto r = wr.lock();
    if (w->dead || !r)
        return;
    uint32_t i;
    if (w->list.Items().IndexOf(r->item, i))
        w->list.Items().RemoveAt(i);
    steps_renumber(w);
    steps_changed(w);
}

static muxc::Button step_button(unsigned glyph, const wchar_t *tip)
{
    auto b = xaml<muxc::Button>(
        L"Button", L"Width='32' Height='32' Padding='0' "
        L"Background='Transparent' BorderThickness='0'");
    b.Content(symbol(glyph, 12));
    muxc::ToolTipService::SetToolTip(b, winrt::box_value(hstring(tip)));
    return b;
}

static void step_hide(StepRow *r)
{
    r->command.PasswordRevealMode(
        r->hide.IsChecked().Value() ? muxc::PasswordRevealMode::Peek :
        muxc::PasswordRevealMode::Visible);
}

extern "C" void wui_w_steps_add(WuiWidget *w, bool active,
                                const wchar_t *expect,
                                const wchar_t *command, bool hide)
{
    auto r = std::make_shared<StepRow>();
    std::weak_ptr<StepRow> wr = r;
    auto g = step_grid();
    g.Padding(mux::ThicknessHelper::FromLengths(0, 4, 0, 4));

    r->number = muxc::TextBlock();
    r->number.HorizontalAlignment(mux::HorizontalAlignment::Center);
    step_cell(g, r->number, 0);

    r->active = muxc::CheckBox();
    r->active.MinWidth(0);
    r->active.IsChecked(active);
    r->active.HorizontalAlignment(mux::HorizontalAlignment::Center);
    r->active.Click([w](auto &&, auto &&) { steps_changed(w); });
    step_cell(g, r->active, 1);

    r->expect = muxc::TextBox();
    r->expect.Text(hs(expect));
    r->expect.TextChanged([w](auto &&, auto &&) { steps_changed(w); });
    step_cell(g, r->expect, 2);

    /* A password box, so that a hidden command can be hidden here
     * too (but looked at, with its reveal button) */
    r->command = muxc::PasswordBox();
    r->command.Password(hs(command));
    r->command.PasswordChanged([w](auto &&, auto &&) { steps_changed(w); });
    step_cell(g, r->command, 3);

    r->hide = muxc::CheckBox();
    r->hide.MinWidth(0);
    r->hide.IsChecked(hide);
    r->hide.HorizontalAlignment(mux::HorizontalAlignment::Center);
    r->hide.Click([w, wr](auto &&, auto &&) {
        if (auto r = wr.lock(); r && !w->dead)
            step_hide(r.get());
        steps_changed(w);
    });
    step_cell(g, r->hide, 4);
    step_hide(r.get());

    muxc::StackPanel buttons;
    buttons.Orientation(muxc::Orientation::Horizontal);
    auto up = step_button(0xE70E, L"Move up");                 /* ChevronUp */
    up.Click([w, wr](auto &&, auto &&) { step_move(w, wr, -1); });
    auto down = step_button(0xE70D, L"Move down");             /* ChevronDown */
    down.Click([w, wr](auto &&, auto &&) { step_move(w, wr, +1); });
    auto del = step_button(0xE74D, L"Delete");                 /* Delete */
    del.Click([w, wr](auto &&, auto &&) { step_remove(w, wr); });
    buttons.Children().Append(up);
    buttons.Children().Append(down);
    buttons.Children().Append(del);
    step_cell(g, buttons, 5);

    r->item = muxc::ListViewItem();
    /* (The row itself isn't somewhere to stop: its fields are) */
    r->item.IsTabStop(false);
    r->item.Padding(mux::ThicknessHelper::FromLengths(8, 0, 8, 0));
    r->item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    r->item.Content(g);

    w->quiet++;
    w->steps.push_back(r);
    w->list.Items().Append(r->item);
    steps_renumber(w);
    w->quiet--;
}

extern "C" WuiWidget *wui_w_steps(WuiWidget *grp, void *wctx,
                                  const wchar_t *const *headers,
                                  const wchar_t *add_label)
{
    WuiWidget *w = new_widget(grp, Kind::Steps);
    w->wctx = wctx;

    auto head = step_grid();
    head.Padding(mux::ThicknessHelper::FromLengths(8, 0, 8, 4));
    for (int i = 0; i < 5; i++) {
        auto t = styled_text(L"CaptionTextBlockStyle", hs(headers[i]), true);
        if (i != 2 && i != 3)
            t.HorizontalAlignment(mux::HorizontalAlignment::Center);
        step_cell(head, t, i);
    }

    /* The rows can be dragged into a different order, as well as moved
     * with their buttons */
    w->list = muxc::ListView();
    w->list.SelectionMode(muxc::ListViewSelectionMode::None);
    w->list.CanReorderItems(true);
    w->list.CanDragItems(true);
    w->list.AllowDrop(true);
    /* Tab goes through every row's fields in turn, and then on out
     * of the table (rather than a list's usual once in, once out) */
    w->list.TabNavigation(muxi::KeyboardNavigationMode::Local);
    w->list.DragItemsCompleted([w](auto &&, auto &&) {
        if (w->dead)
            return;
        steps_renumber(w);
        steps_changed(w);
    });

    w->button = muxc::Button();
    {
        muxc::StackPanel sp;
        sp.Orientation(muxc::Orientation::Horizontal);
        sp.Spacing(8);
        sp.Children().Append(symbol(0xE710, 12));             /* Add */
        muxc::TextBlock t;
        t.Text(hs(add_label));
        sp.Children().Append(t);
        w->button.Content(sp);
    }
    w->button.Click([w](auto &&, auto &&) {
        if (w->dead)
            return;
        wui_w_steps_add(w, true, L"", L"", false);
        auto r = w->steps.empty() ? nullptr : w->steps.back();
        if (r)
            r->expect.Focus(mux::FocusState::Programmatic);
        steps_changed(w);
    });

    muxc::StackPanel sp;
    sp.Spacing(4);
    sp.Children().Append(head);
    sp.Children().Append(w->list);
    w->button.Margin(mux::ThicknessHelper::FromLengths(0, 8, 0, 0));
    sp.Children().Append(w->button);
    w->root = sp;
    add(grp, w);
    return w;
}

extern "C" int wui_w_steps_count(WuiWidget *w)
{
    return (int)w->steps.size();
}

extern "C" size_t wui_w_steps_text(WuiWidget *w, int row, int col,
                                   wchar_t *buf, size_t size)
{
    hstring s;
    if (row >= 0 && row < (int)w->steps.size())
        s = col ? w->steps[row]->command.Password() :
            w->steps[row]->expect.Text();
    if (buf && size > 0) {
        size_t n = std::min((size_t)s.size(), size - 1);
        std::copy(s.begin(), s.begin() + n, buf);
        buf[n] = L'\0';
    }
    return s.size();
}

extern "C" bool wui_w_steps_flag(WuiWidget *w, int row, int col)
{
    if (row < 0 || row >= (int)w->steps.size())
        return false;
    auto &r = w->steps[row];
    auto v = (col ? r->hide : r->active).IsChecked();
    return v && v.Value();
}

/* ---- Widget values ---- */

extern "C" void wui_w_set_text(WuiWidget *w, const wchar_t *text)
{
    hstring s = hs(text);
    if (w->number) {
        const wchar_t *start = text ? text : L"";
        wchar_t *end;
        double v = std::wcstod(start, &end);
        if (end == start)
            v = std::numeric_limits<double>::quiet_NaN();
        w->quiet++;
        w->number.Value(v);
        w->quiet--;
    } else if (w->combo && w->kind == Kind::Edit) {
        w->quiet++;
        if (w->edit)
            set_box_text(w, w->edit, s);
        w->combo.Text(s);
        w->quiet--;
    } else if (w->pass) {
        if (w->pass.Password() != s) {
            w->text_pending = true;
            w->pass.Password(s);
        }
    } else if (w->edit) {
        set_box_text(w, w->edit, s);
    } else if (w->text) {
        w->text.Text(s);
    }
}

extern "C" size_t wui_w_get_text(WuiWidget *w, wchar_t *buf, size_t size)
{
    hstring s;
    if (w->number) {
        double v = w->number.Value();
        wchar_t num[64] = L"";
        if (!std::isnan(v)) {
            if (w->integer)
                swprintf(num, 64, L"%lld", (long long)std::llround(v));
            else
                swprintf(num, 64, L"%.10g", v);
        }
        s = num;
    } else if (w->edit)
        s = w->edit.Text();
    else if (w->combo && w->kind == Kind::Edit)
        s = w->combo.Text();
    else if (w->pass)
        s = w->pass.Password();
    else if (w->text)
        s = w->text.Text();
    if (buf && size > 0) {
        size_t n = std::min((size_t)s.size(), size - 1);
        std::copy(s.begin(), s.begin() + n, buf);
        buf[n] = L'\0';
    }
    return s.size();
}

extern "C" void wui_w_select_range(WuiWidget *w, size_t start, size_t len)
{
    if (w->edit)
        w->edit.Select((int32_t)start, (int32_t)len);
}

extern "C" void wui_w_set_label(WuiWidget *w, const wchar_t *label)
{
    if (w->label) {
        w->label.Text(setting_name(label));
        w->label.Visibility(has_text(label) ? mux::Visibility::Visible :
                            mux::Visibility::Collapsed);
    } else if (w->button) {
        w->button.Content(winrt::box_value(hs(label)));
    }
}

extern "C" void wui_w_set_checked(WuiWidget *w, bool checked)
{
    if (w->toggle && w->toggle.IsOn() != checked) {
        w->quiet++;
        w->toggle.IsOn(checked);
        w->quiet--;
    }
}

extern "C" bool wui_w_get_checked(WuiWidget *w)
{
    return w->toggle && w->toggle.IsOn();
}

extern "C" void wui_w_set_radio(WuiWidget *w, int which)
{
    if (w->kind == Kind::Radio && which >= 0 &&
        which < (int)w->combo.Items().Size()) {
        w->quiet++;
        w->combo.SelectedIndex(which);
        w->quiet--;
    }
}

extern "C" int wui_w_get_radio(WuiWidget *w)
{
    if (w->kind != Kind::Radio)
        return 0;
    return std::max(0, w->combo.SelectedIndex());
}

extern "C" void wui_w_focus(WuiWidget *w)
{
    muxc::Control c{nullptr};
    if (w->kind == Kind::FontSel)
        c = w->button;
    else if (w->number)
        c = w->number;
    else if (w->edit)
        c = w->edit;
    else if (w->pass)
        c = w->pass;
    else if (w->combo)
        c = w->combo;
    else if (w->list)
        c = w->list;
    else if (w->toggle)
        c = w->toggle;
    else if (w->button)
        c = w->button;
    if (!c)
        return;
    if (c.IsLoaded()) {
        c.Focus(mux::FocusState::Programmatic);
        return;
    }
    /* Focus() does nothing until the control is in the live tree (as
     * when the window is still opening), so do it then */
    auto token = std::make_shared<winrt::event_token>();
    *token = c.Loaded([c, token](auto &&, auto &&) {
        c.Loaded(*token);
        c.Focus(mux::FocusState::Programmatic);
    });
}

/* ---- Lists ---- */

extern "C" void wui_w_list_clear(WuiWidget *w)
{
    w->quiet++;
    if (w->list)
        w->list.Items().Clear();
    else if (w->combo)
        w->combo.Items().Clear();
    w->ids.clear();
    w->texts.clear();
    w->quiet--;
}

extern "C" void wui_w_list_action(WuiWidget *w, const wchar_t *label,
                                  unsigned glyph, bool primary, void *target)
{
    w->actions.push_back({hs(label), glyph, primary, target});
    if (w->list)
        w->list.Height(w->height * ACTION_ROW + 8);
}

/* One of a list item's actions: select the item, as if it had been
 * clicked on, and then press the action's button */
static void row_action(WuiWidget *w, winrt::weak_ref<muxc::ListViewItem> wi,
                       void *target)
{
    if (w->dead)
        return;
    auto item = wi.get();
    uint32_t index;
    if (!item || !w->list.Items().IndexOf(item, index))
        return;
    w->list.SelectedIndex((int32_t)index);
    WuiDlg *dlg = w->dlg;
    dlg->window.DispatcherQueue().TryEnqueue([dlg, w, target]() {
        if (!is_live(dlg, w))
            return;
        try {
            wui_dlg_cb_widget(dlg->ctx, target, WUI_EV_ACTION);
        } catch (...) {
            report_exception(dlg, L"handling a list item's action");
        }
    });
}

/* A list item with actions: its text, then a button for each primary
 * action, then a menu of the rest */
static mux::UIElement action_item_content(
    WuiWidget *w, muxc::ListViewItem item, const wchar_t *text)
{
    muxc::Grid g;
    g.ColumnSpacing(8);
    muxc::ColumnDefinition c0;
    c0.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    auto t = list_item_content(w, text);
    t.as<mux::FrameworkElement>().VerticalAlignment(
        mux::VerticalAlignment::Center);
    g.Children().Append(t);

    auto wi = winrt::make_weak(item);
    int col = 1;
    auto place = [&](mux::FrameworkElement e) {
        muxc::ColumnDefinition c;
        c.Width(mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c);
        e.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(e, col++);
        g.Children().Append(e);
    };

    muxc::MenuFlyout menu{nullptr};
    for (auto &a : w->actions) {
        void *target = a.target;
        if (a.primary) {
            muxc::Button b;
            b.Content(winrt::box_value(a.label));
            b.MinWidth(80);
            b.Click([w, wi, target](auto &&, auto &&) {
                row_action(w, wi, target);
            });
            place(b);
        } else {
            if (!menu)
                menu = muxc::MenuFlyout();
            muxc::MenuFlyoutItem mi;
            mi.Text(a.label);
            if (a.glyph)
                mi.Icon(symbol(a.glyph));
            /* (once the menu has closed: see wui-flyout.h) */
            winrt::weak_ref<muxp::FlyoutBase> wmenu = winrt::make_weak(
                menu.as<muxp::FlyoutBase>());
            mi.Click([w, wi, target, wmenu](auto &&, auto &&) {
                wui_after_flyout(wmenu, [w, wi, target]() {
                    row_action(w, wi, target);
                });
            });
            menu.Items().Append(mi);
        }
    }
    if (menu) {
        muxc::Button more;
        more.Content(symbol(0xE712, 14));        /* More */
        muxc::ToolTipService::SetToolTip(
            more, winrt::box_value(hstring(L"More options")));
        more.Flyout(menu);
        place(more);
    }
    return g;
}

extern "C" void wui_w_list_add(WuiWidget *w, const wchar_t *text, int id)
{
    w->quiet++;
    if (w->list) {
        muxc::ListViewItem item;
        item.MinHeight(w->actions.empty() ? LIST_ROW : ACTION_ROW);
        item.Padding(mux::ThicknessHelper::FromLengths(12, 0, 12, 0));
        item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        item.Content(w->actions.empty() ? list_item_content(w, text) :
                     action_item_content(w, item, text));
        w->list.Items().Append(item);
    } else if (w->combo) {
        w->combo.Items().Append(winrt::box_value(hs(text)));
    }
    w->ids.push_back(id);
    w->texts.push_back(hs(text));
    w->quiet--;
}

extern "C" void wui_w_list_del(WuiWidget *w, int index)
{
    if (index < 0 || index >= (int)w->ids.size())
        return;
    w->quiet++;
    if (w->list)
        w->list.Items().RemoveAt(index);
    else if (w->combo)
        w->combo.Items().RemoveAt(index);
    w->ids.erase(w->ids.begin() + index);
    w->texts.erase(w->texts.begin() + index);
    w->quiet--;
}

extern "C" void wui_w_list_swatch(WuiWidget *w, int index, unsigned rgb)
{
    if (!w->list || index < 0 || index >= (int)w->list.Items().Size())
        return;
    auto item = w->list.Items().GetAt(index).try_as<muxc::ListViewItem>();
    if (!item)
        return;
    muxm::SolidColorBrush brush(winrt::Windows::UI::ColorHelper::FromArgb(
                                    255, (rgb >> 16) & 0xFF,
                                    (rgb >> 8) & 0xFF, rgb & 0xFF));
    /* (the square goes in front of what's there, the first time; after
     * that, it just changes colour) */
    auto panel = item.Content().try_as<muxc::StackPanel>();
    if (panel && panel.Tag() && winrt::unbox_value_or<int>(panel.Tag(), 0)) {
        if (auto sq = panel.Children().GetAt(0).try_as<muxc::Border>())
            sq.Background(brush);
        return;
    }
    auto old = item.Content();
    item.Content(nullptr);
    panel = muxc::StackPanel();
    panel.Tag(winrt::box_value(1));
    panel.Orientation(muxc::Orientation::Horizontal);
    panel.Spacing(12);
    muxc::Border sq;
    sq.Width(18);
    sq.Height(18);
    sq.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(4));
    sq.BorderThickness(mux::ThicknessHelper::FromUniformLength(1));
    sq.BorderBrush(theme_brush(L"ControlStrokeColorDefaultBrush"));
    sq.Background(brush);
    sq.VerticalAlignment(mux::VerticalAlignment::Center);
    panel.Children().Append(sq);
    if (auto e = old.try_as<mux::UIElement>()) {
        panel.Children().Append(e);
    } else {
        muxc::TextBlock t;
        t.Text(w->texts[index]);
        t.VerticalAlignment(mux::VerticalAlignment::Center);
        panel.Children().Append(t);
    }
    item.Content(panel);
}

extern "C" void wui_w_list_icon(WuiWidget *w, unsigned glyph)
{
    w->item_glyph = glyph;
}

extern "C" size_t wui_w_list_text(WuiWidget *w, int index, wchar_t *buf,
                                  size_t size)
{
    hstring s;
    if (index >= 0 && index < (int)w->texts.size())
        s = w->texts[index];
    if (buf && size > 0) {
        size_t n = std::min((size_t)s.size(), size - 1);
        std::copy(s.begin(), s.begin() + n, buf);
        buf[n] = L'\0';
    }
    return s.size();
}

extern "C" int wui_w_list_id(WuiWidget *w, int index)
{
    if (index < 0 || index >= (int)w->ids.size())
        return 0;
    return w->ids[index];
}

extern "C" int wui_w_list_selected(WuiWidget *w)
{
    if (w->list) {
        if (w->list.SelectedItems().Size() != 1)
            return -1;
        return w->list.SelectedIndex();
    }
    if (w->combo)
        return w->combo.SelectedIndex();
    return -1;
}

extern "C" bool wui_w_list_issel(WuiWidget *w, int index)
{
    if (w->list) {
        if (index < 0 || index >= (int)w->list.Items().Size())
            return false;
        uint32_t pos;
        return w->list.SelectedItems().IndexOf(
            w->list.Items().GetAt(index), pos);
    }
    return w->combo && w->combo.SelectedIndex() == index;
}

extern "C" void wui_w_list_select(WuiWidget *w, int index)
{
    w->quiet++;
    if (w->list) {
        w->list.SelectedIndex(index);
        if (index >= 0 && index < (int)w->list.Items().Size())
            w->list.ScrollIntoView(w->list.Items().GetAt(index));
    } else if (w->combo) {
        w->combo.SelectedIndex(index);
    }
    w->quiet--;
}

/* ---- The Event Log window ---- */

struct WuiLog {
    void *ctx;
    mux::Window window{nullptr};
    HWND hwnd = nullptr, owner = nullptr;
    muxc::ListView list{nullptr};
    muxc::TextBlock count{nullptr};
    std::vector<hstring> lines;
    bool closing = false;                 /* (by wui_log_free) */
    int anchor = -1;                      /* where Shift+arrows extend
                                           * the selection from */
    int moving = 0;                       /* (see log_move) */
};

static std::vector<WuiLog *> live_logs;

static bool log_live(WuiLog *log)
{
    return std::find(live_logs.begin(), live_logs.end(), log) !=
        live_logs.end();
}

/* A line of the log is its time, a tab, and what happened: shown as
 * two columns, the time quieter */
static muxc::ListViewItem log_item(hstring const &line)
{
    std::wstring s(line);
    size_t tab = s.find(L'\t');
    muxc::Grid g;
    g.ColumnSpacing(16);
    for (int i = 0; i < 2; i++) {
        muxc::ColumnDefinition c;
        c.Width(i ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c);
    }
    if (tab != std::wstring::npos) {
        auto t = styled_text(L"BodyTextBlockStyle",
                             hstring(s.substr(0, tab)), true);
        t.FontFamily(muxm::FontFamily(L"Cascadia Mono, Consolas"));
        t.FontSize(12);
        t.VerticalAlignment(mux::VerticalAlignment::Center);
        g.Children().Append(t);
        s = s.substr(tab + 1);
    }
    auto m = styled_text(L"BodyTextBlockStyle", hstring(s));
    m.TextWrapping(mux::TextWrapping::Wrap);
    m.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(m, 1);
    g.Children().Append(m);

    muxc::ListViewItem item;
    item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    item.MinHeight(28);
    item.Padding(mux::ThicknessHelper::FromLengths(12, 2, 12, 2));
    item.Content(g);
    return item;
}

static void log_count(WuiLog *log)
{
    size_t n = log->lines.size();
    log->count.Text(hstring(std::to_wstring(n) +
                            (n == 1 ? L" event" : L" events")));
}

/* Put the keyboard focus on an event, so that the arrow keys move
 * through them: the newest (if 'last'), or the one chosen, or the
 * first */
static void log_focus(WuiLog *log, bool last)
{
    uint32_t n = log->list.Items().Size();
    if (!n) {
        log->list.Focus(mux::FocusState::Keyboard);
        return;
    }
    int i = last ? int(n - 1) : log->list.SelectedIndex();
    if (i < 0)
        i = 0;
    auto c = log->list.ContainerFromIndex(i).try_as<muxc::Control>();
    if (!c) {
        /* (it's not been made yet, being out of sight) */
        log->list.ScrollIntoView(log->list.Items().GetAt(i));
        log->list.UpdateLayout();
        c = log->list.ContainerFromIndex(i).try_as<muxc::Control>();
    }
    if (c)
        c.Focus(mux::FocusState::Keyboard);
    else
        log->list.Focus(mux::FocusState::Keyboard);
}

/* The event with the keyboard focus, or -1 */
static int log_focused(WuiLog *log)
{
    auto root = log->window.Content().try_as<mux::UIElement>();
    if (!root || !root.XamlRoot())
        return -1;
    auto o = muxi::FocusManager::GetFocusedElement(root.XamlRoot())
        .try_as<mux::DependencyObject>();
    for (; o; o = muxm::VisualTreeHelper::GetParent(o))
        if (auto item = o.try_as<muxc::ListViewItem>())
            return log->list.IndexFromContainer(item);
    return -1;
}

/*
 * The arrow keys (and Page Up and Down, Home and End), done here rather
 * than by the ListView: its own way, which scrolls each step into view
 * with an animation, can't keep up with a key held down. This moves the
 * focus and the selection (or with Shift, extends it), and scrolls
 * straight there.
 */
static bool log_move(WuiLog *log, winrt::Windows::System::VirtualKey key)
{
    using winrt::Windows::System::VirtualKey;
    int n = (int)log->lines.size();
    if (!n)
        return false;
    int cur = log_focused(log);
    if (cur < 0)
        cur = log->list.SelectedIndex() >= 0 ?
            log->list.SelectedIndex() : n - 1;
    int page = std::max(1, (int)(log->list.ActualHeight() / 30) - 1);
    int to;
    switch (key) {
      case VirtualKey::Up: to = cur - 1; break;
      case VirtualKey::Down: to = cur + 1; break;
      case VirtualKey::PageUp: to = cur - page; break;
      case VirtualKey::PageDown: to = cur + page; break;
      case VirtualKey::Home: to = 0; break;
      case VirtualKey::End: to = n - 1; break;
      default: return false;
    }
    to = std::max(0, std::min(to, n - 1));

    log->moving++;
    if (GetKeyState(VK_SHIFT) < 0) {
        if (log->anchor < 0 || log->anchor >= n)
            log->anchor = cur;
        int lo = std::min(log->anchor, to), hi = std::max(log->anchor, to);
        log->list.SelectedItems().Clear();
        log->list.SelectRange(winrt::Microsoft::UI::Xaml::Data::
                              ItemIndexRange(lo, (uint32_t)(hi - lo + 1)));
    } else {
        log->anchor = to;
        log->list.SelectedItems().Clear();
        log->list.SelectedIndex(to);
    }
    log->moving--;

    auto item = log->list.Items().GetAt((uint32_t)to);
    log->list.ScrollIntoView(item);
    auto c = log->list.ContainerFromIndex(to).try_as<muxc::Control>();
    if (!c) {
        log->list.UpdateLayout();
        c = log->list.ContainerFromIndex(to).try_as<muxc::Control>();
    }
    if (c)
        c.Focus(mux::FocusState::Keyboard);
    return true;
}

/* Copy the lines chosen (or all of them, with none chosen) */
static void log_copy(WuiLog *log)
{
    std::wstring text;
    auto sel = log->list.SelectedRanges();
    bool any = sel.Size() > 0;
    for (size_t i = 0; i < log->lines.size(); i++) {
        bool in = !any;
        for (auto const &r : sel)
            if ((int)i >= r.FirstIndex() && (int)i <= r.LastIndex())
                in = true;
        if (in)
            text += std::wstring(log->lines[i]) + L"\r\n";
    }
    if (text.empty())
        return;
    namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
    try {
        dt::DataPackage pkg;
        pkg.SetText(hstring(text));
        dt::Clipboard::SetContent(pkg);
    } catch (...) {
    }
}

extern "C" WuiLog *wui_log_new(HWND owner, const wchar_t *title, HICON icon,
                               int theme, void *ctx)
{
    auto log = new WuiLog;
    log->ctx = ctx;
    log->owner = owner;
    live_logs.push_back(log);

    log->window = mux::Window();
    check_hresult(log->window.as<IWindowNative>()->get_WindowHandle(
                      &log->hwnd));
    UINT dpi = GetDpiForWindow(log->hwnd);
    double scale = (dpi ? dpi : 96) / 96.0;
    auto appwindow = log->window.AppWindow();
    auto presenter = muw::OverlappedPresenter::Create();
    presenter.IsMinimizable(false);
    appwindow.SetPresenter(presenter);
    /* (above the window it's the log of, but not stopping it) */
    if (owner)
        SetWindowLongPtrW(log->hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);
    log->window.Title(hs(title));
    if (icon) {
        SendMessageW(log->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(log->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
    log->window.SystemBackdrop(muxm::MicaBackdrop());

    /* Our own title bar, as in the configuration box; then the count
     * of events and the buttons; then the events, in a card */
    muxc::Grid root;
    for (int i = 0; i < 3; i++) {
        muxc::RowDefinition r;
        r.Height(i == 2 ? mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star) :
                 mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(r);
    }

    muxc::Grid titlebar;
    titlebar.Height(48);
    titlebar.Background(muxm::SolidColorBrush(
                            winrt::Microsoft::UI::Colors::Transparent()));
    muxc::StackPanel tbcontent;
    tbcontent.Orientation(muxc::Orientation::Horizontal);
    tbcontent.VerticalAlignment(mux::VerticalAlignment::Center);
    tbcontent.Margin(mux::ThicknessHelper::FromLengths(16, 0, 0, 0));
    tbcontent.Spacing(12);
    muxc::Image tbicon;
    tbicon.Width(16);
    tbicon.Height(16);
    try {
        tbicon.Source(wui_icon_image(icon, (int)std::lround(16 * scale)));
    } catch (...) {
    }
    tbcontent.Children().Append(tbicon);
    auto tbtext = styled_text(L"CaptionTextBlockStyle", hs(title));
    tbtext.VerticalAlignment(mux::VerticalAlignment::Center);
    tbcontent.Children().Append(tbtext);
    titlebar.Children().Append(tbcontent);
    muxc::Grid::SetRow(titlebar, 0);
    root.Children().Append(titlebar);

    muxc::Grid top;
    top.Padding(mux::ThicknessHelper::FromLengths(24, 4, 24, 12));
    top.ColumnSpacing(8);
    for (int i = 0; i < 3; i++) {
        muxc::ColumnDefinition c;
        c.Width(i == 0 ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        top.ColumnDefinitions().Append(c);
    }
    muxc::StackPanel heading;
    heading.Children().Append(styled_text(L"SubtitleTextBlockStyle",
                                          hstring(L"Event Log")));
    log->count = styled_text(L"CaptionTextBlockStyle", hstring(), true);
    heading.Children().Append(log->count);
    top.Children().Append(heading);

    auto button = [](const wchar_t *label, unsigned glyph, bool accent) {
        auto b = accent ?
            xaml<muxc::Button>(L"Button", L"Style='{StaticResource "
                               L"AccentButtonStyle}'") : muxc::Button();
        muxc::StackPanel c;
        c.Orientation(muxc::Orientation::Horizontal);
        c.Spacing(8);
        if (glyph)
            c.Children().Append(symbol(glyph, 14));
        muxc::TextBlock t;
        t.Text(label);
        c.Children().Append(t);
        b.Content(c);
        b.VerticalAlignment(mux::VerticalAlignment::Center);
        return b;
    };
    auto copy = button(L"Copy", 0xE8C8, false);             /* Copy */
    muxc::ToolTipService::SetToolTip(
        copy, winrt::box_value(hstring(L"Copy the events chosen, or all "
                                       L"of them (Ctrl+C)")));
    copy.Click([log](auto &&, auto &&) {
        if (log_live(log))
            log_copy(log);
    });
    muxc::Grid::SetColumn(copy, 1);
    top.Children().Append(copy);
    auto close = button(L"Close", 0, true);
    close.Click([log](auto &&, auto &&) {
        if (log_live(log)) {
            wui_dwm_set(log->hwnd, DWMWA_CLOAK, TRUE);
            log->window.Close();
        }
    });
    muxc::Grid::SetColumn(close, 2);
    top.Children().Append(close);
    muxc::Grid::SetRow(top, 1);
    root.Children().Append(top);

    log->list = xaml<muxc::ListView>(
        L"ListView", L"BorderThickness='0' Padding='4' "
        L"Background='Transparent'");
    log->list.SelectionMode(muxc::ListViewSelectionMode::Extended);
    log->list.PreviewKeyDown([log](auto &&, muxi::KeyRoutedEventArgs const &e) {
        if (!log_live(log) || GetKeyState(VK_CONTROL) < 0)
            return;
        if (log_move(log, e.Key()))
            e.Handled(true);
    });
    /* (a click, or anything else that chooses just one, is where Shift
     * extends from next) */
    log->list.SelectionChanged([log](auto &&, auto &&) {
        if (log_live(log) && !log->moving &&
            log->list.SelectedItems().Size() == 1)
            log->anchor = log->list.SelectedIndex();
    });
    auto card = group_card();
    card.Margin(mux::ThicknessHelper::FromLengths(24, 0, 24, 24));
    card.Padding(mux::ThicknessHelper::FromUniformLength(0));
    card.Child(log->list);
    muxc::Grid::SetRow(card, 2);
    root.Children().Append(card);

    /* (Ctrl+C copies, and Escape closes, as in a dialog box; and the
     * arrow keys go to the events, from wherever the focus is) */
    root.KeyDown([log](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        if (!log_live(log))
            return;
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && e.Key() == VirtualKey::C) {
            log_copy(log);
            e.Handled(true);
        } else if (e.Key() == VirtualKey::Down ||
                   e.Key() == VirtualKey::Up ||
                   e.Key() == VirtualKey::PageDown ||
                   e.Key() == VirtualKey::PageUp ||
                   e.Key() == VirtualKey::Home ||
                   e.Key() == VirtualKey::End) {
            /* (the list does these itself, in log_move, when it has
             * the focus; so this is from elsewhere, e.g. a button) */
            if (log_move(log, e.Key()))
                e.Handled(true);
        } else if (e.Key() == VirtualKey::Escape) {
            wui_dwm_set(log->hwnd, DWMWA_CLOAK, TRUE);
            log->window.Close();
            e.Handled(true);
        }
    });

    auto theme_of = theme == 1 ? mux::ElementTheme::Light :
        theme == 2 ? mux::ElementTheme::Dark : mux::ElementTheme::Default;
    root.RequestedTheme(theme_of);
    log->window.Content(root);
    log->window.ExtendsContentIntoTitleBar(true);
    log->window.SetTitleBar(titlebar);
    wui_dwm_dark_frame(log->hwnd, wui_is_dark(root));

    /* (out of sight before it goes, as for the About box) */
    appwindow.Closing([hwnd = log->hwnd](auto &&, auto &&) {
        wui_dwm_set(hwnd, DWMWA_CLOAK, TRUE);
    });
    log->window.Closed([log](auto &&, auto &&) {
        if (!log_live(log) || log->closing)
            return;
        /* (closed by its user: wui_log_free, from that, finishes) */
        log->closing = true;
        if (log->owner)
            SetActiveWindow(log->owner);
        wui_log_cb_closed(log->ctx);
    });

    /* (beside its owner, about as wide as a terminal) */
    int w = (int)std::lround(760 * scale), h = (int)std::lround(520 * scale);
    appwindow.Resize(winrt::Windows::Graphics::SizeInt32{w, h});
    HMONITOR mon = MonitorFromWindow(owner ? owner : log->hwnd,
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT area = mi.rcWork, ref = area;
        if (owner && IsWindowVisible(owner) && !IsIconic(owner))
            GetWindowRect(owner, &ref);
        int x = (ref.left + ref.right - w) / 2;
        int y = (ref.top + ref.bottom - h) / 2;
        x = std::max((int)area.left, std::min(x, (int)area.right - w));
        y = std::max((int)area.top, std::min(y, (int)area.bottom - h));
        appwindow.Move(winrt::Windows::Graphics::PointInt32{x, y});
    }
    log_count(log);
    wui_show_without_flash(log->hwnd, log->window);
    return log;
}

extern "C" void wui_log_set(WuiLog *log, size_t n,
                            const wchar_t *const *lines)
{
    /* (just the new ones, if that's all that's changed, so that what's
     * chosen stays chosen; otherwise all of them again) */
    size_t old = log->lines.size();
    bool more = n >= old;
    for (size_t i = 0; i < old && more; i++)
        more = log->lines[i] == lines[i];
    if (!more) {
        log->list.Items().Clear();
        log->lines.clear();
        old = 0;
    }
    for (size_t i = old; i < n; i++) {
        log->lines.push_back(hs(lines[i]));
        log->list.Items().Append(log_item(log->lines.back()));
    }
    log_count(log);
    if (n > old && n)
        log->list.ScrollIntoView(log->list.Items().GetAt(
                                     (uint32_t)(n - 1)));
}

extern "C" void wui_log_show(WuiLog *log)
{
    ShowWindow(log->hwnd, SW_SHOWNORMAL);
    log->window.Activate();
    /* (the newest event focused, once it's been laid out, so that the
     * arrow keys work straight away) */
    log->window.DispatcherQueue().TryEnqueue(
        mud::DispatcherQueuePriority::Low, [log]() {
            if (log_live(log))
                log_focus(log, true);
        });
}

extern "C" void wui_log_set_theme(WuiLog *log, int theme)
{
    auto root = log->window.Content().try_as<mux::FrameworkElement>();
    if (!root)
        return;
    root.RequestedTheme(theme == 1 ? mux::ElementTheme::Light :
                        theme == 2 ? mux::ElementTheme::Dark :
                        mux::ElementTheme::Default);
    wui_dwm_dark_frame(log->hwnd, wui_is_dark(root));
}

extern "C" void wui_log_free(WuiLog *log)
{
    live_logs.erase(std::remove(live_logs.begin(), live_logs.end(), log),
                    live_logs.end());
    if (!log->closing) {
        log->closing = true;
        wui_dwm_set(log->hwnd, DWMWA_CLOAK, TRUE);
        try {
            log->window.Close();
        } catch (...) {
        }
    }
    delete log;
}

/* ---- The About box ---- */

extern "C" void wui_about(HWND owner, HICON icon, int theme,
                          const WuiAbout *about)
{
    mux::Window window;
    HWND hwnd = nullptr;
    check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
    UINT dpi = GetDpiForWindow(hwnd);
    double scale = (dpi ? dpi : 96) / 96.0;
    auto appwindow = window.AppWindow();
    auto presenter = muw::OverlappedPresenter::Create();
    presenter.IsMinimizable(false);
    presenter.IsMaximizable(false);
    appwindow.SetPresenter(presenter);
    if (owner)
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);
    window.Title(hs(about->title));
    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
    window.SystemBackdrop(muxm::MicaBackdrop());

    muxc::Grid root;
    for (int i = 0; i < 3; i++) {
        muxc::RowDefinition r;
        r.Height(i == 1 ? mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star) :
                 mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(r);
    }

    /* Our own title bar, with just the title (the icon's below) */
    muxc::Grid titlebar;
    titlebar.Height(40);
    titlebar.Background(muxm::SolidColorBrush(
                            winrt::Microsoft::UI::Colors::Transparent()));
    auto tbtext = styled_text(L"CaptionTextBlockStyle", hs(about->title));
    tbtext.VerticalAlignment(mux::VerticalAlignment::Center);
    tbtext.Margin(mux::ThicknessHelper::FromLengths(16, 0, 0, 0));
    titlebar.Children().Append(tbtext);
    muxc::Grid::SetRow(titlebar, 0);
    root.Children().Append(titlebar);

    /* Everything centred, as in a Windows 11 app's About page: the
     * icon, the name, the version, who it's by; then links to the
     * rest, which come up over it */
    muxc::StackPanel page;
    page.HorizontalAlignment(mux::HorizontalAlignment::Center);
    page.VerticalAlignment(mux::VerticalAlignment::Center);
    page.Padding(mux::ThicknessHelper::FromLengths(32, 0, 32, 8));
    page.Spacing(4);
    auto centred = [](mux::FrameworkElement const &e) {
        e.HorizontalAlignment(mux::HorizontalAlignment::Center);
        if (auto t = e.try_as<muxc::TextBlock>()) {
            t.TextAlignment(mux::TextAlignment::Center);
            t.TextWrapping(mux::TextWrapping::Wrap);
        }
        return e;
    };
    muxc::Image big;
    big.Width(96);
    big.Height(96);
    big.Margin(mux::ThicknessHelper::FromLengths(0, 0, 0, 12));
    try {
        big.Source(wui_icon_image(icon, (int)std::lround(96 * scale)));
    } catch (...) {
    }
    page.Children().Append(centred(big));
    page.Children().Append(centred(styled_text(L"TitleLargeTextBlockStyle",
                                               hs(about->name))));
    auto version = styled_text(L"BodyStrongTextBlockStyle",
                               hs(about->version));
    version.IsTextSelectionEnabled(true);
    page.Children().Append(centred(version));
    if (has_text(about->byline))
        page.Children().Append(centred(styled_text(
            L"BodyTextBlockStyle", hs(about->byline), true)));

    /* (a ContentDialog, for the licence and the build details) */
    /* (weakly: these are in handlers of things in it) */
    auto xroot = std::make_shared<winrt::weak_ref<muxc::Grid>>();
    auto show_text = [xroot](hstring title, hstring text, bool copy) {
        auto r = xroot->get();
        if (!r)
            return;
        muxc::ContentDialog cd;
        try {
            cd.Style(mux::Application::Current().Resources().Lookup(
                         winrt::box_value(L"DefaultContentDialogStyle"))
                     .as<mux::Style>());
        } catch (...) {
        }
        cd.XamlRoot(r.XamlRoot());
        cd.RequestedTheme(r.ActualTheme());
        cd.Title(winrt::box_value(title));
        muxc::ScrollViewer sv;
        /* (short enough for the dialog, centred in the window, to stay
         * clear of the title bar and its close button, which would be
         * over it: 40 above it and below it, and about 180 of the
         * dialog's title, margins and buttons) */
        sv.MaxHeight(std::clamp(r.ActualHeight() - 2 * 40 - 180, 120.0,
                                320.0));
        auto t = styled_text(L"BodyTextBlockStyle", text);
        t.TextWrapping(mux::TextWrapping::Wrap);
        /* (clear of the scroll bar, which sits over the text) */
        t.Margin(mux::ThicknessHelper::FromLengths(0, 0, 16, 0));
        t.IsTextSelectionEnabled(true);
        sv.Content(t);
        if (copy) {
            cd.Content(sv);
            cd.CloseButtonText(L"Close");
            cd.DefaultButton(muxc::ContentDialogButton::Close);
        } else {
            /* (our own Close button, as the dialog's would stretch the
             * whole width, alone along the bottom) */
            muxc::StackPanel body;
            body.Spacing(16);
            body.Children().Append(sv);
            auto close = xaml<muxc::Button>(
                L"Button", L"Style='{StaticResource AccentButtonStyle}' "
                L"MinWidth='100' HorizontalAlignment='Right'");
            close.Content(winrt::box_value(hstring(L"Close")));
            auto weak = winrt::make_weak(cd);
            close.Click([weak](auto &&, auto &&) {
                if (auto d = weak.get())
                    d.Hide();
            });
            body.Children().Append(close);
            cd.Content(body);
        }
        if (copy) {
            cd.PrimaryButtonText(L"Copy");
            cd.PrimaryButtonClick([text](auto &&, auto &&) {
                namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
                try {
                    dt::DataPackage pkg;
                    pkg.SetText(text);
                    dt::Clipboard::SetContent(pkg);
                } catch (...) {
                }
            });
        }
        try {
            cd.ShowAsync();
        } catch (...) {
        }
    };

    muxc::StackPanel links;
    links.Orientation(muxc::Orientation::Horizontal);
    links.Spacing(4);
    links.Margin(mux::ThicknessHelper::FromLengths(0, 16, 0, 0));
    auto link = [&](const wchar_t *label) {
        muxc::HyperlinkButton b;
        b.Content(winrt::box_value(hstring(label)));
        if (links.Children().Size()) {
            auto dot = styled_text(L"BodyTextBlockStyle", hstring(L"·"),
                                   true);
            dot.VerticalAlignment(mux::VerticalAlignment::Center);
            links.Children().Append(dot);
        }
        links.Children().Append(b);
        return b;
    };
    hstring licence = hs(about->licence);
    /* (the fork's copyright first, then the original's, then that it's
     * not the original, before the licence) */
    hstring copyright = hs(about->copyright);
    if (has_text(about->fork_copyright))
        copyright = hs(about->fork_copyright) + L"\n" + copyright;
    if (has_text(about->notice))
        copyright = copyright + L"\n\n" + hs(about->notice);
    link(L"Licence").Click([show_text, licence, copyright](auto &&, auto &&) {
        show_text(L"Licence", copyright + L"\n\n" + licence, false);
    });
    auto web = link(L"Web site");
    muxc::ToolTipService::SetToolTip(web,
                                     winrt::box_value(hs(about->website)));
    std::wstring url(about->website);
    web.Click([url, hwnd](auto &&, auto &&) {
        ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr,
                      SW_SHOWDEFAULT);
    });
    hstring details = hs(about->name) + L"\n" + hs(about->version) +
        L"\n\n" + hs(about->build);
    link(L"Build").Click([show_text, details](auto &&, auto &&) {
        show_text(L"Build", details, true);
    });
    page.Children().Append(centred(links));
    /* (the fork's copyright, if any, above the original's) */
    auto copyline = styled_text(L"CaptionTextBlockStyle",
        hs(has_text(about->fork_copyright) ? about->fork_copyright
                                           : about->copyright), true);
    copyline.Margin(mux::ThicknessHelper::FromLengths(0, 8, 0, 0));
    page.Children().Append(centred(copyline));
    if (has_text(about->fork_copyright))
        page.Children().Append(centred(styled_text(
            L"CaptionTextBlockStyle", hs(about->copyright), true)));
    muxc::Grid::SetRow(page, 1);
    root.Children().Append(page);

    /* Close, at the bottom right */
    auto close = xaml<muxc::Button>(
        L"Button", L"Style='{StaticResource AccentButtonStyle}' "
        L"MinWidth='100'");
    close.Content(winrt::box_value(hstring(L"Close")));
    close.HorizontalAlignment(mux::HorizontalAlignment::Right);
    close.Margin(mux::ThicknessHelper::FromLengths(0, 0, 24, 20));
    muxc::Grid::SetRow(close, 2);
    root.Children().Append(close);
    *xroot = winrt::make_weak(root);

    root.RequestedTheme(theme == 1 ? mux::ElementTheme::Light :
                        theme == 2 ? mux::ElementTheme::Dark :
                        mux::ElementTheme::Default);
    window.Content(root);
    window.ExtendsContentIntoTitleBar(true);
    window.SetTitleBar(titlebar);

    /* It's modal, like the dialog box it replaces: its owner's off
     * while it's open, and this waits for it to close */
    auto done = std::make_shared<bool>(false);
    auto queue = window.DispatcherQueue();
    auto finish = [done, queue]() {
        if (*done)
            return;
        *done = true;
        try {
            queue.EnqueueEventLoopExit();
        } catch (...) {
        }
    };
    auto shut = [window, hwnd, owner]() {
        /* (AppWindow's Closing isn't raised for this, so as there) */
        if (owner)
            EnableWindow(owner, TRUE);
        wui_dwm_set(hwnd, DWMWA_CLOAK, TRUE);
        try {
            window.Close();
        } catch (...) {
        }
    };
    close.Click([shut](auto &&, auto &&) { shut(); });
    root.KeyDown([shut](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        if (e.Key() == VirtualKey::Escape || e.Key() == VirtualKey::Enter) {
            e.Handled(true);
            shut();
        }
    });
    window.Closed([finish](auto &&, auto &&) { finish(); });
    /* (the owner back on before this goes, so that it's what Windows
     * activates next, and not some other application) */
    appwindow.Closing([owner, hwnd](auto &&, auto &&) {
        if (owner)
            EnableWindow(owner, TRUE);
        /* (out of sight first: closing it takes its content away before
         * the window itself goes, which shows as a flash of white, as
         * in wui_dlg_free) */
        wui_dwm_set(hwnd, DWMWA_CLOAK, TRUE);
    });
    close.Loaded([](wf::IInspectable const &sender, auto &&) {
        sender.as<muxc::Button>().Focus(mux::FocusState::Programmatic);
    });

    /* (wide enough for the licence's box to be its full width, and tall
     * enough for it to have some text, clear of the title bar) */
    int w = (int)std::lround(520 * scale), h = (int)std::lround(540 * scale);
    appwindow.Resize(winrt::Windows::Graphics::SizeInt32{w, h});
    HMONITOR mon = MonitorFromWindow(owner ? owner : hwnd,
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT area = mi.rcWork, ref = area;
        if (owner && IsWindowVisible(owner) && !IsIconic(owner))
            GetWindowRect(owner, &ref);
        int x = (ref.left + ref.right - w) / 2;
        int y = (ref.top + ref.bottom - h) / 2;
        x = std::max((int)area.left, std::min(x, (int)area.right - w));
        y = std::max((int)area.top, std::min(y, (int)area.bottom - h));
        appwindow.Move(winrt::Windows::Graphics::PointInt32{x, y});
    }

    if (owner)
        EnableWindow(owner, FALSE);
    wui_show_without_flash(hwnd, window);
    try {
        queue.RunEventLoop(mud::DispatcherRunOptions::QuitOnlyLocalLoop,
                           nullptr);
    } catch (...) {
        MSG msg;
        while (!*done && GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        /* (before it goes, so that Windows activates the owner) */
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}
