/*
 * winui-host.cpp: the C++/WinRT half of the WinUI 3 PuTTY front end.
 *
 * This owns the XAML application and window, renders the terminal
 * with Direct2D and DirectWrite into a SwapChainPanel, and turns XAML
 * input events into calls to the C side (winui-window.c) through the
 * interface in wui.h. It knows nothing about PuTTY itself.
 *
 * The XAML is built in code rather than markup, so that we need no
 * XAML compiler: the Application subclass provides the XAML type
 * metadata by delegating to the standard WinUI controls provider.
 *
 * The window holds a TabView with one tab per session. The TabView is
 * only the tab strip: the tabs' contents live in a grid below it,
 * where only the selected one is visible, because the TabView's own
 * content area doesn't stretch them to its full height. Each tab has
 * its own SwapChainPanel and scroll bar; all of them share one
 * Direct3D / Direct2D device. A tab's terminal image is kept in an
 * offscreen Direct2D bitmap, which the terminal code draws into
 * incrementally (as it would into a window DC) and which we copy to
 * the tab's swap chain after each update. That carries on while the
 * tab is hidden, so its picture is up to date when it's shown.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <unknwn.h>
/* winbase.h's GetCurrentTime macro breaks the XAML animation API */
#undef GetCurrentTime

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>

#include <microsoft.ui.xaml.window.h>
#include <microsoft.ui.xaml.media.dxinterop.h>

#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <exception>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "wui.h"
#include "wui-dwm.h"
#include "wui-flyout.h"

/* In winui-dialog.cpp */
winrt::Microsoft::UI::Xaml::Media::ImageSource wui_icon_image(HICON icon,
                                                              int px);

namespace wf = winrt::Windows::Foundation;
namespace mui = winrt::Microsoft::UI;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muw = winrt::Microsoft::UI::Windowing;

using winrt::com_ptr;
using winrt::check_hresult;

/* Width of the scroll bar, in XAML units. A multiple of 4, so that
 * it's a whole number of pixels at every standard scale factor. */
static const double SCROLLBAR_WIDTH = 16;

static D2D1_COLOR_F colour(COLORREF c)
{
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                        GetBValue(c) / 255.0f, 1.0f);
}

static bool is_variation_selector(unsigned c)
{
    return (c >= 0x180B && c <= 0x180D) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0xE0100 && c <= 0xE01EF);
}

/* ----------------------------------------------------------------------
 * The graphics device, shared by all the tabs.
 */

struct Gpu {
    com_ptr<ID2D1Factory1> d2d_factory;
    com_ptr<IDWriteFactory> dw_factory;
    com_ptr<IWICImagingFactory> wic_factory;

    com_ptr<ID3D11Device> d3d;
    com_ptr<ID2D1Device> d2d_device;
    com_ptr<ID2D1DeviceContext> dc;
    com_ptr<ID2D1SolidColorBrush> brush;
    bool lost = false;                 /* needs recreating */

    void init_factories()
    {
        D2D1_FACTORY_OPTIONS opts{};
        check_hresult(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &opts, d2d_factory.put_void()));
        check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown **>(dw_factory.put())));
        check_hresult(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wic_factory.put())));
    }

    void create_device()
    {
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
            D3D_FEATURE_LEVEL_9_1,
        };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        lost = false;
        d3d = nullptr;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
            ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.put(), nullptr,
            nullptr);
        if (FAILED(hr)) {
            /* No usable GPU: fall back to the software rasteriser */
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.put(), nullptr,
                nullptr);
        }
        check_hresult(hr);

        auto dxgi_device = d3d.as<IDXGIDevice>();
        d2d_device = nullptr;
        dc = nullptr;
        check_hresult(d2d_factory->CreateDevice(
            dxgi_device.get(), d2d_device.put()));
        check_hresult(d2d_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
        /* Work in physical pixels throughout */
        dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        dc->SetDpi(96, 96);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        brush = nullptr;
        check_hresult(dc->CreateSolidColorBrush(
            D2D1::ColorF(0, 0, 0), brush.put()));
    }
};

/* ----------------------------------------------------------------------
 * The renderer for one tab.
 */

struct Renderer {
    Gpu &gpu;
    com_ptr<IDXGISwapChain1> swapchain;
    com_ptr<ID2D1Bitmap1> target;      /* the swap chain's back buffer */
    com_ptr<ID2D1Bitmap1> backing;     /* the terminal image */
    std::unordered_map<HICON, com_ptr<ID2D1Bitmap>> icons;

    int width = 0, height = 0;
    float scale_x = 1, scale_y = 1;
    bool drawing = false;

    explicit Renderer(Gpu &gpu) : gpu(gpu) {}

    ~Renderer()
    {
        if (drawing)
            gpu.dc->EndDraw();
        gpu.dc->SetTarget(nullptr);
    }

    /* Font state */
    com_ptr<IDWriteFontFace> faces[2];        /* normal, bold */
    com_ptr<IDWriteTextFormat> formats[2];    /* for font fallback */
    float em = 0;
    float font_xscale = 1;
    UINT16 units_per_em = 1;
    int cell_w = 0, cell_h = 0, ascent = 0;
    D2D1_TEXT_ANTIALIAS_MODE aa = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;

    /* Forget everything that belongs to the old device */
    void reset()
    {
        drawing = false;
        icons.clear();
        swapchain = nullptr;
        target = nullptr;
        backing = nullptr;
        width = height = 0;
    }

    com_ptr<ID2D1Bitmap1> new_backing(int w, int h)
    {
        com_ptr<ID2D1Bitmap1> bmp;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE), 96, 96);
        check_hresult(gpu.dc->CreateBitmap(
            D2D1::SizeU(w, h), nullptr, 0, props, bmp.put()));
        return bmp;
    }

    void create_target()
    {
        com_ptr<IDXGISurface> surface;
        check_hresult(swapchain->GetBuffer(0, IID_PPV_ARGS(surface.put())));
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE), 96, 96);
        target = nullptr;
        check_hresult(gpu.dc->CreateBitmapFromDxgiSurface(
            surface.get(), &props, target.put()));
    }

    void update_transform()
    {
        /* The swap chain is in physical pixels; tell the compositor to
         * scale it back down to XAML units. */
        if (auto sc2 = swapchain.try_as<IDXGISwapChain2>()) {
            DXGI_MATRIX_3X2_F m{};
            m._11 = 1.0f / scale_x;
            m._22 = 1.0f / scale_y;
            sc2->SetMatrixTransform(&m);
        }
    }

    /* Returns true if the panel needs a swap chain set on it. */
    bool resize(int w, int h, COLORREF bg)
    {
        bool new_swapchain = false;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w == width && h == height && swapchain)
            return false;

        if (!swapchain) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = w;
            desc.Height = h;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc.Scaling = DXGI_SCALING_STRETCH;

            auto dxgi_device = gpu.d3d.as<IDXGIDevice>();
            com_ptr<IDXGIAdapter> adapter;
            check_hresult(dxgi_device->GetAdapter(adapter.put()));
            com_ptr<IDXGIFactory2> factory;
            check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
            check_hresult(factory->CreateSwapChainForComposition(
                gpu.d3d.get(), &desc, nullptr, swapchain.put()));
            new_swapchain = true;
        } else {
            gpu.dc->SetTarget(nullptr);
            target = nullptr;
            check_hresult(swapchain->ResizeBuffers(
                2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        }
        create_target();
        update_transform();

        /* Make a new backing bitmap, keeping as much of the old
         * picture as still fits, to avoid flicker while the terminal
         * redraws. */
        auto nb = new_backing(w, h);
        gpu.dc->SetTarget(nb.get());
        gpu.dc->BeginDraw();
        gpu.dc->Clear(colour(bg));
        gpu.dc->EndDraw();
        if (backing) {
            D2D1_POINT_2U origin{0, 0};
            D2D1_RECT_U src{0, 0, (UINT32)std::min(w, width),
                            (UINT32)std::min(h, height)};
            nb->CopyFromBitmap(&origin, backing.get(), &src);
        }
        backing = nb;
        width = w;
        height = h;
        present();
        return new_swapchain;
    }

    void present()
    {
        if (!swapchain || !backing || !target)
            return;
        gpu.dc->SetTarget(target.get());
        gpu.dc->BeginDraw();
        gpu.dc->DrawBitmap(backing.get(), nullptr, 1.0f,
                       D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        HRESULT hr = gpu.dc->EndDraw();
        if (SUCCEEDED(hr))
            hr = swapchain->Present(1, 0);
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET)
            gpu.lost = true;
    }

    bool begin_draw()
    {
        if (!backing || drawing)
            return false;
        gpu.dc->SetTarget(backing.get());
        gpu.dc->BeginDraw();
        gpu.dc->SetTextAntialiasMode(aa);
        drawing = true;
        return true;
    }

    void end_draw()
    {
        if (!drawing)
            return;
        drawing = false;
        HRESULT hr = gpu.dc->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
            gpu.lost = true;
        else
            present();
    }

    void fill_rect(int x, int y, int w, int h, COLORREF c)
    {
        gpu.brush->SetColor(colour(c));
        gpu.dc->FillRectangle(D2D1::RectF((float)x, (float)y,
                                      (float)(x + w), (float)(y + h)),
                          gpu.brush.get());
    }

    void draw_icon(HICON icon, int x, int y, int w, int h)
    {
        auto it = icons.find(icon);
        if (it == icons.end()) {
            com_ptr<IWICBitmap> wicbmp;
            com_ptr<IWICFormatConverter> conv;
            com_ptr<ID2D1Bitmap> bmp;
            if (FAILED(gpu.wic_factory->CreateBitmapFromHICON(
                           icon, wicbmp.put())) ||
                FAILED(gpu.wic_factory->CreateFormatConverter(conv.put())) ||
                FAILED(conv->Initialize(
                           wicbmp.get(), GUID_WICPixelFormat32bppPBGRA,
                           WICBitmapDitherTypeNone, nullptr, 0,
                           WICBitmapPaletteTypeMedianCut)) ||
                FAILED(gpu.dc->CreateBitmapFromWicBitmap(
                           conv.get(), nullptr, bmp.put())))
                return;
            it = icons.emplace(icon, bmp).first;
        }
        gpu.dc->DrawBitmap(it->second.get(),
                       D2D1::RectF((float)x, (float)y,
                                   (float)(x + w), (float)(y + h)),
                       1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    }

    /* ---- Fonts ---- */

    com_ptr<IDWriteFont> find_font(const wchar_t *name, bool bold,
                                   int charset)
    {
        com_ptr<IDWriteGdiInterop> interop;
        check_hresult(gpu.dw_factory->GetGdiInterop(interop.put()));

        /* Try the requested font, then some that are always there */
        const wchar_t *names[] = { name, L"Consolas", L"Courier New" };
        for (const wchar_t *n : names) {
            if (!n || !*n)
                continue;
            LOGFONTW lf{};
            wcsncpy_s(lf.lfFaceName, n, _TRUNCATE);
            lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
            lf.lfCharSet = (BYTE)charset;
            com_ptr<IDWriteFont> font;
            if (SUCCEEDED(interop->CreateFontFromLOGFONT(&lf, font.put())))
                return font;
            /* The charset might be what it couldn't match */
            lf.lfCharSet = DEFAULT_CHARSET;
            if (SUCCEEDED(interop->CreateFontFromLOGFONT(&lf, font.put())))
                return font;
        }
        return nullptr;
    }

    static std::wstring family_name(IDWriteFontFamily *family)
    {
        com_ptr<IDWriteLocalizedStrings> names;
        if (FAILED(family->GetFamilyNames(names.put())))
            return L"";
        UINT32 index = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"en-us", &index, &exists);
        if (!exists)
            index = 0;
        UINT32 len = 0;
        if (FAILED(names->GetStringLength(index, &len)))
            return L"";
        std::wstring s(len + 1, L'\0');
        if (FAILED(names->GetString(index, s.data(), len + 1)))
            return L"";
        s.resize(len);
        return s;
    }

    float design_advance(IDWriteFontFace *face, UINT32 codepoint)
    {
        UINT16 glyph = 0;
        if (FAILED(face->GetGlyphIndices(&codepoint, 1, &glyph)) || !glyph)
            return 0;
        DWRITE_GLYPH_METRICS gm{};
        if (FAILED(face->GetDesignGlyphMetrics(&glyph, 1, &gm, FALSE)))
            return 0;
        return (float)gm.advanceWidth;
    }

    bool set_font(const WuiFontRequest *req, WuiFontMetrics *m)
    {
        com_ptr<IDWriteFont> font = find_font(req->name, req->bold,
                                              req->charset);
        if (!font)
            return false;

        com_ptr<IDWriteFontFamily> family;
        check_hresult(font->GetFontFamily(family.put()));
        com_ptr<IDWriteFont> boldfont;
        if (FAILED(family->GetFirstMatchingFont(
                       req->bold ? DWRITE_FONT_WEIGHT_HEAVY :
                       DWRITE_FONT_WEIGHT_BOLD, font->GetStretch(),
                       font->GetStyle(), boldfont.put())))
            boldfont = font;

        faces[0] = nullptr;
        faces[1] = nullptr;
        check_hresult(font->CreateFontFace(faces[0].put()));
        check_hresult(boldfont->CreateFontFace(faces[1].put()));

        DWRITE_FONT_METRICS fm{};
        font->GetMetrics(&fm);
        units_per_em = fm.designUnitsPerEm ? fm.designUnitsPerEm : 1;
        float upem = units_per_em;

        /* Natural cell width: the widest digit, as window.c does for
         * variable-pitch fonts (and for fixed-pitch ones, every glyph
         * is that wide anyway) */
        float maxadv = 0;
        for (UINT32 c = '0'; c <= '9'; c++)
            maxadv = std::max(maxadv, design_advance(faces[0].get(), c));
        if (maxadv <= 0)
            maxadv = upem / 2;

        if (req->cell_height > 0) {
            em = req->cell_height * upem / (float)(fm.ascent + fm.descent);
            cell_h = req->cell_height;
            ascent = (int)std::lround(fm.ascent * em / upem);
        } else {
            em = (float)(req->em_height > 0 ? req->em_height : 13);
            ascent = (int)std::lround(fm.ascent * em / upem);
            cell_h = ascent + (int)std::lround(fm.descent * em / upem);
        }
        if (cell_h < 1)
            cell_h = 1;

        float natural_w = maxadv * em / upem;
        if (req->cell_width > 0) {
            cell_w = req->cell_width;
            font_xscale = cell_w / natural_w;
        } else {
            cell_w = std::max(1, (int)std::lround(natural_w));
            font_xscale = 1;
        }

        bool mono = false;
        if (auto face1 = faces[0].try_as<IDWriteFontFace1>())
            mono = face1->IsMonospacedFont() != FALSE;

        switch (req->quality) {
          case WUI_QUALITY_ANTIALIASED:
            aa = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
            break;
          case WUI_QUALITY_NONANTIALIASED:
            aa = D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
            break;
          case WUI_QUALITY_CLEARTYPE:
            aa = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
            break;
          default:
            aa = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;
            break;
        }

        /* Text formats used to draw characters the font hasn't got,
         * which get DirectWrite's system font fallback. */
        std::wstring fname = family_name(family.get());
        for (int i = 0; i < 2; i++) {
            formats[i] = nullptr;
            IDWriteFont *f = i ? boldfont.get() : font.get();
            if (SUCCEEDED(gpu.dw_factory->CreateTextFormat(
                    fname.c_str(), nullptr, f->GetWeight(), f->GetStyle(),
                    f->GetStretch(), em, L"", formats[i].put()))) {
                formats[i]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                formats[i]->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                formats[i]->SetLineSpacing(
                    DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)cell_h,
                    (float)ascent);
            }
        }

        icons.clear();  /* the trust sigil is reloaded at each font size */

        m->cell_width = cell_w;
        m->cell_height = cell_h;
        m->ascent = ascent;
        m->strikethrough_y =
            ascent - (int)std::lround(fm.strikethroughPosition * em / upem);
        if (m->strikethrough_y <= 0 || m->strikethrough_y >= cell_h)
            m->strikethrough_y = ascent - ascent * 3 / 8;
        m->monospaced = mono;
        return true;
    }

    int char_width(unsigned codepoint)
    {
        if (!faces[0])
            return 0;
        float adv = design_advance(faces[0].get(), codepoint);
        if (adv <= 0)
            return 0;
        return std::max(1, (int)std::lround(adv * em / units_per_em *
                                            font_xscale));
    }

    struct Cluster {
        UINT32 codepoint;
        int start, len;            /* range of UTF-16 units */
        float pos, advance;        /* in pixels from the text origin */
        UINT16 glyph;
    };

    void draw_text(const WuiTextRequest *r)
    {
        IDWriteFontFace *face = faces[r->bold ? 1 : 0].get();
        IDWriteTextFormat *format = formats[r->bold ? 1 : 0].get();
        if (!face)
            return;

        /* Split the text into one cluster per character cell */
        std::vector<Cluster> clusters;
        float pos = 0;
        for (int i = 0; i < r->len;) {
            UINT32 c = r->text[i];
            int n = 1;
            if (IS_HIGH_SURROGATE(r->text[i]) && i + 1 < r->len &&
                IS_LOW_SURROGATE(r->text[i+1])) {
                c = 0x10000 + (((c & 0x3FF) << 10) | (r->text[i+1] & 0x3FF));
                n = 2;
            }
            float adv = 0;
            for (int j = 0; j < n; j++)
                adv += (float)r->dx[i + j];
            if (is_variation_selector(c)) {
                /* No glyph of its own; its cell belongs to whatever it
                 * modifies */
                if (!clusters.empty())
                    clusters.back().advance += adv;
            } else {
                clusters.push_back(Cluster{c, i, n, pos, adv, 0});
            }
            pos += adv;
            i += n;
        }
        if (clusters.empty())
            return;

        std::vector<UINT32> cps(clusters.size());
        std::vector<UINT16> glyphs(clusters.size());
        for (size_t i = 0; i < clusters.size(); i++)
            cps[i] = clusters[i].codepoint;
        if (FAILED(face->GetGlyphIndices(cps.data(), (UINT32)cps.size(),
                                         glyphs.data())))
            return;
        for (size_t i = 0; i < clusters.size(); i++)
            clusters[i].glyph = glyphs[i];

        /*
         * Draw in a coordinate system with the origin at the top left
         * of the first cell, scaled for double-width / double-height
         * lines and for any horizontal stretching of the font.
         */
        float sx = r->scale_x * font_xscale, sy = r->scale_y;
        D2D1_MATRIX_3X2_F old;
        gpu.dc->GetTransform(&old);
        gpu.dc->PushAxisAlignedClip(
            D2D1::RectF((float)r->clip.left, (float)r->clip.top,
                        (float)r->clip.right, (float)r->clip.bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
        gpu.dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) *
                         D2D1::Matrix3x2F::Translation((float)r->x,
                                                       (float)r->y));
        gpu.brush->SetColor(colour(r->fg));

        for (int pass = 0; pass < (r->shadow_bold ? 2 : 1); pass++) {
            float shift = pass ? -1.0f / sx : 0;
            size_t i = 0;
            while (i < clusters.size()) {
                if (clusters[i].glyph == 0 && clusters[i].codepoint != ' ') {
                    /* Missing glyph: let DirectWrite find a font */
                    const Cluster &cl = clusters[i];
                    if (format) {
                        float left = cl.pos / sx + shift;
                        gpu.dc->DrawText(
                            r->text + cl.start, cl.len, format,
                            D2D1::RectF(left, 0, left + cl.advance / sx,
                                        (float)cell_h),
                            gpu.brush.get(),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT |
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    i++;
                    continue;
                }

                /* A run of characters the font does have */
                size_t j = i;
                while (j < clusters.size() &&
                       !(clusters[j].glyph == 0 &&
                         clusters[j].codepoint != ' '))
                    j++;
                size_t n = j - i;
                std::vector<UINT16> gi(n);
                std::vector<FLOAT> adv(n);
                std::vector<DWRITE_GLYPH_OFFSET> offs(n);
                for (size_t k = 0; k < n; k++) {
                    const Cluster &cl = clusters[i + k];
                    gi[k] = cl.glyph;
                    adv[k] = cl.advance / sx;
                    offs[k].advanceOffset = 0;
                    offs[k].ascenderOffset = 0;
                }
                if (r->centre) {
                    std::vector<DWRITE_GLYPH_METRICS> gm(n);
                    if (SUCCEEDED(face->GetDesignGlyphMetrics(
                                      gi.data(), (UINT32)n, gm.data(),
                                      FALSE))) {
                        for (size_t k = 0; k < n; k++) {
                            float w = gm[k].advanceWidth * em / units_per_em;
                            offs[k].advanceOffset = (adv[k] - w) / 2;
                        }
                    }
                }

                DWRITE_GLYPH_RUN run{};
                run.fontFace = face;
                run.fontEmSize = em;
                run.glyphCount = (UINT32)n;
                run.glyphIndices = gi.data();
                run.glyphAdvances = adv.data();
                run.glyphOffsets = offs.data();
                run.isSideways = FALSE;
                run.bidiLevel = 0;
                gpu.dc->DrawGlyphRun(
                    D2D1::Point2F(clusters[i].pos / sx + shift,
                                  (float)ascent),
                    &run, gpu.brush.get(), DWRITE_MEASURING_MODE_NATURAL);
                i = j;
            }
        }

        gpu.dc->SetTransform(old);
        gpu.dc->PopAxisAlignedClip();
    }
};

/* ----------------------------------------------------------------------
 * The terminal control. We derive from UserControl only so that we
 * can set the mouse pointer shape, which XAML only lets a control do
 * for itself.
 */

struct TermControl : muxc::UserControlT<TermControl> {
    void set_cursor(WuiCursor c)
    {
        mui::Input::InputSystemCursorShape shape;
        switch (c) {
          case WUI_CURSOR_ARROW:
            shape = mui::Input::InputSystemCursorShape::Arrow;
            break;
          case WUI_CURSOR_APPSTARTING:
            shape = mui::Input::InputSystemCursorShape::AppStarting;
            break;
          case WUI_CURSOR_WAIT:
            shape = mui::Input::InputSystemCursorShape::Wait;
            break;
          default:
            shape = mui::Input::InputSystemCursorShape::IBeam;
            break;
        }
        ProtectedCursor(mui::Input::InputSystemCursor::Create(shape));
    }
};

/* The bar between two panes, which can be dragged to move it, and
 * which shows the mouse pointer for that (which, again, only a control
 * can set for itself) */
struct SplitterControl : muxc::UserControlT<SplitterControl> {
    void set_cursor(bool side_by_side)
    {
        ProtectedCursor(mui::Input::InputSystemCursor::Create(
                            side_by_side ?
                            mui::Input::InputSystemCursorShape::SizeWestEast :
                            mui::Input::InputSystemCursorShape::SizeNorthSouth));
    }
};

/* Width of that bar, in XAML units */
static const double SPLITTER_WIDTH = 6;


/* ----------------------------------------------------------------------
 * Tabs and the window.
 */

/* Our guess at the height of the tab strip (which is also the title
 * bar), in XAML units, until we can measure it. It's only used to size
 * the window before its first layout pass; after that we correct it. */
static const double TAB_STRIP_ESTIMATE = 41;
/* (anything less isn't the tab strip yet, but the window's first layout,
 * before the tabs are in it: see content_changed) */
static const double TAB_STRIP_MIN = 32;

/* Space to leave free for dragging the window, to the right of the
 * tabs, however many tabs there are (XAML units) */
static const double DRAG_SPACE = 48;

/* Width of the saved sessions button, beside the '+' button (XAML
 * units) */
/* (narrower than the '+' before it, so that the two make a pair, as
 * in Windows Terminal) */
static const double MENU_BUTTON_WIDTH = 24;

struct WuiHost;
static muxc::IconElement menu_icon(unsigned glyph);

/* A tooltip: what something does, and underneath, in italics, a hint
 * (a variant, or a shortcut) */
static muxc::TextBlock hint_tooltip(const wchar_t *what, const wchar_t *hint)
{
    /* (each '\n' of either starting a new line; the hint, keyboard
     * shortcuts mostly, in italics; and wrapped, rather than cut off at
     * the tooltip's width) */
    namespace muxd = winrt::Microsoft::UI::Xaml::Documents;
    muxc::TextBlock tip;
    tip.TextWrapping(mux::TextWrapping::Wrap);
    auto add = [&tip](const wchar_t *text, bool italic) {
        for (const wchar_t *p = text; ; ) {
            const wchar_t *nl = wcschr(p, L'\n');
            size_t len = nl ? (size_t)(nl - p) : wcslen(p);
            if (tip.Inlines().Size())
                tip.Inlines().Append(muxd::LineBreak());
            muxd::Run run;
            run.Text(winrt::hstring(p, (uint32_t)len));
            if (italic)
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            tip.Inlines().Append(run);
            if (!nl)
                break;
            p = nl + 1;
        }
    };
    add(what, false);
    if (hint && *hint)
        add(hint, true);
    return tip;
}

/* The first icon inside an element */
static muxc::FontIcon find_icon(mux::DependencyObject const &obj)
{
    int n = mux::Media::VisualTreeHelper::GetChildrenCount(obj);
    for (int i = 0; i < n; i++) {
        auto child = mux::Media::VisualTreeHelper::GetChild(obj, i);
        if (auto icon = child.try_as<muxc::FontIcon>())
            return icon;
        if (auto found = find_icon(child))
            return found;
    }
    return nullptr;
}

/* An element inside a control's template, by name */
static mux::FrameworkElement find_named(mux::DependencyObject const &obj,
                                        winrt::hstring const &name)
{
    int n = mux::Media::VisualTreeHelper::GetChildrenCount(obj);
    for (int i = 0; i < n; i++) {
        auto child = mux::Media::VisualTreeHelper::GetChild(obj, i);
        auto fe = child.try_as<mux::FrameworkElement>();
        if (fe && fe.Name() == name)
            return fe;
        if (auto found = find_named(child, name))
            return found;
    }
    return nullptr;
}

/*
 * One tab, holding one session's terminal. Event handlers hold only
 * weak references to it, so that once it's closed, any events still
 * on their way to it are dropped.
 */
struct WuiTab;

struct WuiTerm : std::enable_shared_from_this<WuiTerm> {
    WuiHost *win;
    void *ctx;
    WuiTab *tab = nullptr;             /* the tab it's a pane of... */
    muxc::TabViewItem item{nullptr};   /* ... and that tab's header */
    muxc::Border frame{nullptr};       /* around root: shows which pane
                                        * has the focus, if there are more
                                        * than one in the tab */
    int est_w = 0, est_h = 0;          /* a new pane's size, until it's
                                        * been laid out */
    int refusals = 0;                  /* see WuiHost::set_panel_size */
    muxc::Grid root{nullptr};
    muxc::UserControl term{nullptr};
    com_ptr<TermControl> term_impl;
    muxc::Grid area{nullptr};          /* behind the swap chain panel */
    muxc::SwapChainPanel panel{nullptr};
    muxp::ScrollBar scrollbar{nullptr};
    Renderer renderer;
    COLORREF bg = RGB(0, 0, 0);        /* the terminal's default bg */
    winrt::hstring title, status;      /* for the tab's tooltip */
    winrt::hstring label, session;     /* (see wui_term_set_label) */
    winrt::hstring hint;               /* ... (see wui_term_set_hint) */
    bool scrollbar_visible = true;
    bool term_focused = false;
    bool focus_on_load = false;        /* focus() came before Loaded */
    int panel_w = 0, panel_h = 0;      /* last size we told the C side */

    /* Find's search box, over the top right of the terminal, once
     * it's been asked for (see show_search) */
    muxc::Border search_bar{nullptr};
    muxc::TextBox search_box{nullptr};
    muxc::TextBlock search_count{nullptr};
    muxp::ToggleButton search_case{nullptr}, search_regex{nullptr};

    /* Send File's and Receive File's bar, likewise (see open_bar) */
    muxc::Border send_bar{nullptr};
    muxc::FontIcon send_icon{nullptr}, send_arrow{nullptr},
        send_warning{nullptr};
    muxc::TextBlock send_name{nullptr}, send_status{nullptr};
    muxc::AutoSuggestBox send_dest{nullptr};
    muxc::Button send_recent_button{nullptr}, send_button{nullptr},
        send_close{nullptr};
    muxc::ProgressBar send_progress{nullptr};
    muxc::StackPanel send_status_row{nullptr};
    muxc::HyperlinkButton send_help{nullptr}, send_show{nullptr};
    mui::Dispatching::DispatcherQueueTimer send_timer{nullptr};
    std::vector<winrt::hstring> send_recent;
    WuiSendState send_state = WUI_SEND_READY;
    bool bar_receive = false;          /* (receiving, not sending) */
    bool send_know_cwd = false;
    bool send_dest_typed = false;
    bool suggesting_recent = false;    /* (not what's in list_dir) */
    /* What's in a remote directory, for completing what's typed */
    std::wstring list_dir;
    std::vector<std::wstring> list_names;
    bool list_asked = false, list_known = false;
    bool list_wanted = false, list_all = false;

    /* (where those go: down the top right of the terminal) */
    muxc::StackPanel bars{nullptr};

    WuiTerm(WuiHost *win, void *ctx, Gpu &gpu)
        : win(win), ctx(ctx), renderer(gpu) {}

    void create();
    void hook_events();
    int scrollbar_px();
    void current_size(int &w, int &h);
    void update_size(bool notify);
    void set_swapchain();
    void report_focus();
    void focus();
    void focus_if_pending();
    WuiPointerEvent make_event(muxi::PointerRoutedEventArgs const &e,
                               mui::Input::PointerPoint const &pt);
    void pointer_event(muxi::PointerRoutedEventArgs const &e);
    void show_menu(int x, int y);
    void show_tab_menu(muxi::ContextRequestedEventArgs const &e);
    void update_tab_colours();
    void make_search_bar();
    void show_search();
    void hide_search();
    void search(int dir);
    muxc::StackPanel &get_bars();
    void make_send_bar();
    void open_bar(bool receive, const wchar_t *dest, bool know_cwd);
    void show_send(const wchar_t *filename, const wchar_t *dest,
                   const wchar_t *const *recent, size_t nrecent,
                   bool know_cwd);
    void show_receive(const wchar_t *path, bool know_cwd);
    void hide_send();
    void set_send_state(WuiSendState state, const wchar_t *text,
                        double fraction);
    void go();
    void show_recent();
    void complete(bool all);
};

/*
 * A tab holds one or more panes, each a terminal, arranged as a tree:
 * each split has two halves side by side or one above the other, with
 * a bar between them that can be dragged, and each half is a pane or
 * another split. One of the panes has the focus (the one the tab's
 * header is about, and that keystrokes go to when the tab's selected).
 */
struct PaneNode {
    PaneNode *parent = nullptr;
    WuiTerm *term = nullptr;           /* a pane's, or NULL for a split */
    bool side_by_side = false;         /* a split's halves */
    double ratio = 0.5;                /* ... and how much the first has */
    std::unique_ptr<PaneNode> a, b;
    muxc::Grid grid{nullptr};          /* a split's */

    mux::FrameworkElement element() const
    {
        return term ? mux::FrameworkElement(term->frame) :
            mux::FrameworkElement(grid);
    }
};

struct WuiTab {
    WuiHost *win;
    muxc::TabViewItem item{nullptr};
    muxc::Grid container{nullptr};     /* in WuiHost::contents */
    std::unique_ptr<PaneNode> tree;
    WuiTerm *focused = nullptr;
    winrt::hstring custom;             /* a title given it by its user,
                                        * instead of the panes' own (see
                                        * start_rename) */
    bool renaming = false;

    explicit WuiTab(WuiHost *win) : win(win) {}

    void start_rename();
    void end_rename(winrt::hstring const *text);

    bool single() const { return !tree || tree->term; }

    void panes(std::vector<WuiTerm *> &out, PaneNode *n = nullptr)
    {
        if (!n)
            n = tree.get();
        if (!n)
            return;
        if (n->term) {
            out.push_back(n->term);
            return;
        }
        panes(out, n->a.get());
        panes(out, n->b.get());
    }

    PaneNode *find_leaf(WuiTerm *t, PaneNode *n = nullptr)
    {
        if (!n)
            n = tree.get();
        if (!n)
            return nullptr;
        if (n->term)
            return n->term == t ? n : nullptr;
        if (auto r = find_leaf(t, n->a.get()))
            return r;
        return find_leaf(t, n->b.get());
    }

    void create();
    void update_frames();
    void refresh_header();
    void set_focused(WuiTerm *t);
    muxc::Grid make_split(PaneNode *n);
};

struct WuiHost {
    mux::Window window{nullptr};
    muw::AppWindow appwindow{nullptr};
    muw::OverlappedPresenter overlapped{nullptr};
    HWND hwnd = nullptr;
    muxc::Grid layout{nullptr};        /* tab strip above, contents below */
    muxc::TabView tabs{nullptr};
    muxc::Grid contents{nullptr};      /* every tab's root, one visible */
    muxc::Grid drag_region{nullptr};   /* the title bar, beside the tabs */
    muxc::Grid strip_footer{nullptr};  /* menu button, drag region, inset */
    muxc::Button menu_button{nullptr};
    mux::FrameworkElement add_button{nullptr};  /* the TabView's '+' */
    muxc::Grid icon_box{nullptr};      /* the window icon, before the tabs */
    muxc::Image icon_image{nullptr};
    HICON icon = nullptr;
    bool shown = false;                /* (see wui_show) */
    std::vector<std::shared_ptr<WuiTerm>> terms;
    std::vector<std::unique_ptr<WuiTab>> tablist;
    WuiTerm *active = nullptr;
    Gpu gpu;

    bool tinted_tabs = false;          /* the selected tab takes on its
                                        * terminal's background colour */
    double scale = 1;                  /* pixels per XAML unit */
    /* The area each tab's content gets, in pixels: the terminal area
     * plus the scroll bar, if the tab has one. Shared by all tabs. */
    int content_w = 0, content_h = 0;
    /* Window client area minus that (i.e. mostly the tab strip),
     * once we've been able to measure it */
    int chrome_w = 0, chrome_h = 0;
    bool chrome_known = false;
    /* A size the C side asked for that we haven't achieved yet */
    std::weak_ptr<WuiTerm> pending;
    int pending_w = 0, pending_h = 0, pending_tries = 0;

    bool window_active = false;
    bool close_confirmed = false;
    bool moving_tab = false;           /* ignore selection changes */
    bool size_update_queued = false;
    WuiWindowState last_state = WUI_STATE_NORMAL;

    void create()
    {
        gpu.init_factories();
        gpu.create_device();

        window = mux::Window();
        check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
        UINT dpi = GetDpiForWindow(hwnd);
        scale = (dpi ? dpi : 96) / 96.0;
        appwindow = window.AppWindow();
        overlapped = muw::OverlappedPresenter::Create();
        appwindow.SetPresenter(overlapped);

        tabs = muxc::TabView();
        tabs.IsAddTabButtonVisible(true);
        /* Reordering by dragging needs all three. (A tab dropped
         * outside the strip just stays where it was.) */
        tabs.CanReorderTabs(true);
        tabs.CanDragTabs(true);
        tabs.AllowDropTabs(true);
        tabs.TabWidthMode(muxc::TabViewWidthMode::Equal);
        tabs.CloseButtonOverlayMode(muxc::TabViewCloseButtonOverlayMode::Auto);
        /* The TabView has keyboard accelerators of its own (Ctrl+F4,
         * for one), and XAML would show them in a tooltip over the
         * whole tab strip, which is the whole title bar. (They still
         * work; the close buttons' tooltips say so.) */
        tabs.KeyboardAcceleratorPlacementMode(
            muxi::KeyboardAcceleratorPlacementMode::Hidden);

        /*
         * Put the tabs in the title bar, as Notepad does: the window
         * content extends up into it, and the empty part of the tab
         * strip, after the tabs and the '+' button, becomes the area
         * you drag the window by. (Right-clicking it, or Alt+Space,
         * brings up the window icon's menu: see TermWndProc.)
         */
        drag_region = muxc::Grid();
        drag_region.Background(
            mux::Media::SolidColorBrush(mui::Colors::Transparent()));
        drag_region.MinWidth(DRAG_SPACE);

        /*
         * Before it, just after the '+' button (which is where the
         * footer starts), a button with a down arrow on that lists the
         * saved sessions, as in Windows Terminal. It can't be inside
         * the drag region, which wouldn't let it be clicked, so the
         * footer has three columns: the button, the drag region, and
         * the space under the system's caption buttons.
         */
        menu_button = muxc::Button();
        auto arrow = menu_icon(0xE70D).as<muxc::FontIcon>();  /* ChevronDown */
        arrow.FontSize(12);
        menu_button.Content(arrow);
        menu_button.Width(MENU_BUTTON_WIDTH);
        /* (with none of a button's usual padding, which in a button this
         * narrow would leave no room for the arrow) */
        menu_button.Padding(mux::ThicknessHelper::FromUniformLength(0));
        menu_button.VerticalAlignment(mux::VerticalAlignment::Center);
        menu_button.Background(
            mux::Media::SolidColorBrush(mui::Colors::Transparent()));
        menu_button.BorderThickness(mux::ThicknessHelper::FromUniformLength(0));
        menu_button.IsTabStop(false);
        muxc::ToolTipService::SetToolTip(
            menu_button, hint_tooltip(L"Open a saved session, or type an "
                                      L"address",
                                      L"Ctrl+Shift+Space\n"
                                      L"Shift+click to open in new window"));

        strip_footer = muxc::Grid();
        for (int i = 0; i < 3; i++) {
            auto c = muxc::ColumnDefinition();
            c.Width(i == 1 ? mux::GridLengthHelper::FromValueAndType(
                                 1, mux::GridUnitType::Star) :
                    mux::GridLengthHelper::Auto());
            strip_footer.ColumnDefinitions().Append(c);
        }
        muxc::Grid::SetColumn(menu_button, 0);
        strip_footer.Children().Append(menu_button);
        muxc::Grid::SetColumn(drag_region, 1);
        strip_footer.Children().Append(drag_region);
        tabs.TabStripFooter(strip_footer);

        /*
         * The window icon goes at the left of the tab strip, where it
         * would be in an ordinary title bar, and clicking it brings up
         * the window menu, as it would there.
         */
        icon_box = muxc::Grid();
        icon_box.Background(
            mux::Media::SolidColorBrush(mui::Colors::Transparent()));
        icon_box.Padding(mux::ThicknessHelper::FromLengths(12, 0, 8, 0));
        icon_image = muxc::Image();
        icon_image.Width(16);
        icon_image.Height(16);
        icon_image.VerticalAlignment(mux::VerticalAlignment::Center);
        icon_box.Children().Append(icon_image);
        tabs.TabStripHeader(icon_box);

        layout = muxc::Grid();
        auto r0 = muxc::RowDefinition();
        r0.Height(mux::GridLengthHelper::Auto());
        auto r1 = muxc::RowDefinition();
        r1.Height(mux::GridLengthHelper::FromValueAndType(
                      1, mux::GridUnitType::Star));
        layout.RowDefinitions().Append(r0);
        layout.RowDefinitions().Append(r1);
        muxc::Grid::SetRow(tabs, 0);
        layout.Children().Append(tabs);
        contents = muxc::Grid();
        muxc::Grid::SetRow(contents, 1);
        layout.Children().Append(contents);
        window.Content(layout);
        window.ExtendsContentIntoTitleBar(true);
        window.SetTitleBar(drag_region);
        /* Tall caption buttons (48 units) would stick out below the
         * tab strip, which is shorter than that */
        appwindow.TitleBar().PreferredHeightOption(
            muw::TitleBarHeightOption::Standard);
        /* The tab strip is transparent; with Mica behind it, it looks
         * like Notepad's. (The darker 'Alt' kind is the one meant for
         * tabbed title bars. Where Mica isn't supported, this falls
         * back to a plain theme colour.) */
        mux::Media::MicaBackdrop mica;
        mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::
                  MicaKind::BaseAlt);
        window.SystemBackdrop(mica);
        update_title_bar();

        hook_events();
    }

    /* Keep the drag area clear of the minimise / maximise / close
     * buttons, which the system draws over our content */
    void update_title_bar()
    {
        /* While the window is minimised, the caption buttons aren't
         * anywhere, and the inset can come out negative, which a
         * column width can't be (XAML throws, and that would be the
         * end of us). There's no title bar to lay out then anyway. */
        if (get_state() == WUI_STATE_MINIMISED)
            return;
        double inset = appwindow.TitleBar().RightInset() / scale;
        if (!(inset >= 0 && inset < 10000))
            inset = 0;
        try {
            strip_footer.ColumnDefinitions().GetAt(2).Width(
                mux::GridLengthHelper::FromPixels(inset));
            strip_footer.MinWidth(DRAG_SPACE + MENU_BUTTON_WIDTH + inset);
        } catch (...) {
        }
    }

    /*
     * Put the saved sessions button (with the down arrow) level with
     * the '+' button just before it, and the same height: the '+' is
     * placed by the TabView's template, not centred in the strip as
     * the footer is, so centring this one leaves it out of line.
     */
    void align_menu_button()
    {
        if (!add_button || add_button.ActualHeight() <= 0)
            return;
        try {
            auto p = add_button.TransformToVisual(strip_footer)
                .TransformPoint(wf::Point{0, 0});
            double top = std::max(0.0, (double)p.Y);
            double h = add_button.ActualHeight();
            /* (only if it's changed, as this is called on layout, and
             * moving it lays out again) */
            if (std::fabs(menu_button.Margin().Top - top) < 0.5 &&
                std::fabs(menu_button.Height() - h) < 0.5)
                return;
            menu_button.VerticalAlignment(mux::VerticalAlignment::Top);
            menu_button.Margin(mux::ThicknessHelper::FromLengths(0, top, 0, 0));
            menu_button.Height(h);
            if (auto c = add_button.try_as<muxc::Control>()) {
                menu_button.CornerRadius(c.CornerRadius());
                /* (in the same colour) */
                menu_button.Foreground(c.Foreground());
            }
            /* (and the arrow the same size as the '+') */
            if (auto plus = find_icon(add_button))
                if (auto arrow = menu_button.Content()
                        .try_as<muxc::FontIcon>())
                    arrow.FontSize(plus.FontSize());
        } catch (...) {
        }
    }

    /* Draw the window icon at the size it's displayed at, in pixels */
    void update_icon()
    {
        if (icon)
            icon_image.Source(wui_icon_image(
                                  icon, (int)std::lround(16 * scale)));
    }

    /*
     * The window icon's menu: the system menu's items, drawn by XAML,
     * with icons. Windows would do this itself for a real title bar;
     * here we have to say which of them apply. (The session's commands
     * are in the tab's and the terminal's menus.)
     */
    void show_window_menu(bool at_pointer = false);
    void show_sessions_menu();

    void hook_events()
    {
        icon_box.PointerPressed(
            [this](auto &&, muxi::PointerRoutedEventArgs const &e) {
                window.DispatcherQueue().TryEnqueue(
                    [this]() { show_window_menu(); });
                e.Handled(true);
            });
        menu_button.Click([this](auto &&, auto &&) {
            show_sessions_menu();
        });
        /* The '+' button: a new tab, or with Shift, a new window (as
         * in Windows Terminal) */
        tabs.AddTabButtonClick([](auto &&, auto &&) {
            if (GetKeyState(VK_SHIFT) < 0)
                wui_cb_new_window_requested();
            else
                wui_cb_new_tab_requested();
        });
        /* ... which its tooltip says. (The button is in the TabView's
         * template, so it's there once the TabView has loaded.) */
        tabs.Loaded([this](auto &&, auto &&) {
            if (auto add = find_named(tabs, L"AddButton")) {
                muxc::ToolTipService::SetToolTip(
                    add, hint_tooltip(L"New session\u2026\n"
                                      L"Type an address to connect to, or "
                                      L"go to Sessions to pick one",
                                      L"Shift+click to open in new window"));
                /* (and the arrow button kept in line with it) */
                add_button = add;
                add.SizeChanged([this](auto &&, auto &&) {
                    align_menu_button();
                });
                strip_footer.SizeChanged([this](auto &&, auto &&) {
                    align_menu_button();
                });
                align_menu_button();
            }
        });
        tabs.TabCloseRequested(
            [this](muxc::TabView const &,
                   muxc::TabViewTabCloseRequestedEventArgs const &e) {
                if (WuiTerm *t = find(e.Tab()))
                    wui_cb_tab_close_requested(t->ctx);
            });
        tabs.SelectionChanged([this](auto &&, auto &&) { selection_changed(); });

        appwindow.Closing([this](muw::AppWindow const &,
                                 muw::AppWindowClosingEventArgs const &e) {
            if (!close_confirmed) {
                /* The C side asks the user, and calls back with
                 * wui_close_window if they agree */
                e.Cancel(true);
                wui_cb_close_requested();
            }
        });
        window.Closed([](auto &&, auto &&) { wui_cb_closed(); });
        appwindow.Changed([this](muw::AppWindow const &,
                                 muw::AppWindowChangedEventArgs const &e) {
            if (e.DidSizeChange() || e.DidPresenterChange())
                update_title_bar();
            WuiWindowState state = get_state();
            if (state != last_state) {
                last_state = state;
                wui_cb_state_changed(state);
            }
        });
        window.Activated([this](auto &&, mux::WindowActivatedEventArgs const &e) {
            window_active = (e.WindowActivationState() !=
                             mux::WindowActivationState::Deactivated);
            if (window_active && active)
                active->focus();
            if (active)
                active->report_focus();
        });
    }

    /* The pane with the focus in a tab, given its header */
    WuiTerm *find(winrt::Windows::Foundation::IInspectable const &obj)
    {
        if (!obj)
            return nullptr;
        auto item = obj.try_as<muxc::TabViewItem>();
        for (auto &tab : tablist)
            if (tab->item == item)
                return tab->focused;
        return nullptr;
    }

    void selection_changed()
    {
        if (moving_tab)
            return;
        WuiTerm *t = find(tabs.SelectedItem());
        if (!t)
            return;
        if (t == active) {
            /* (e.g. a new tab, which we made active already) */
            if (!t->renderer.swapchain && t->root.ActualWidth() > 0)
                content_changed(t);
            return;
        }
        WuiTerm *old = active;
        active = t;
        show_active();
        if (old)
            old->report_focus();
        wui_cb_tab_activated(t->ctx);
        if (t->root.ActualWidth() > 0 && t->root.ActualHeight() > 0)
            content_changed(t);

        /* Clicking a tab focuses the tab header; once XAML has finished
         * doing that, move the focus on to the terminal. */
        std::weak_ptr<WuiTerm> weak = t->weak_from_this();
        window.DispatcherQueue().TryEnqueue([weak]() {
            if (auto t = weak.lock())
                t->focus();
        });
    }

    void show_active()
    {
        for (auto &tab : tablist) {
            tab->container.Visibility(
                active && active->tab == tab.get() ?
                mux::Visibility::Visible : mux::Visibility::Collapsed);
            /* (the colour it's dragged in depends on whether it's this
             * one: see update_tab_colours) */
            if (tinted_tabs && tab->focused)
                tab->focused->update_tab_colours();
        }
    }

    /* The selected tab's content area has been laid out at a new size */
    void content_changed(WuiTerm *t)
    {
        double sx = t->panel.CompositionScaleX();
        if (sx > 0 && std::fabs(sx - scale) > 1e-6)
            scale_changed(sx);

        /* (the whole of the tab, whatever panes it has) */
        int w = (int)std::lround(t->tab->container.ActualWidth() * scale);
        int h = (int)std::lround(t->tab->container.ActualHeight() * scale);
        if (w <= 0 || h <= 0)
            return;                    /* minimised, or not laid out yet */
        content_w = w;
        content_h = h;

        RECT cr;
        bool chrome_ok = false;
        if (GetClientRect(hwnd, &cr)) {
            int ch = cr.bottom - content_h;
            if (ch >= (int)std::lround(TAB_STRIP_MIN * scale)) {
                chrome_w = cr.right - content_w;
                chrome_h = ch;
                chrome_known = true;
                chrome_ok = true;
            }
        }

        /* If we've just been given the wrong size for a requested
         * terminal area, because we guessed the size of the tab strip,
         * have another go now we know it */
        if (auto p = pending.lock()) {
            /* (not with what the tab strip measures before it's been
             * laid out: wait for the layout that it's in) */
            if (!chrome_ok && get_state() == WUI_STATE_NORMAL) {
                queue_size_update();
                return;
            }
            int want_w = pending_w + p->scrollbar_px();
            if ((content_w != want_w || content_h != pending_h) &&
                pending_tries++ < 3 && get_state() == WUI_STATE_NORMAL) {
                resize_client(want_w, pending_h);
                /* That should cause another layout, and another call
                 * here; but in case the window can't actually take
                 * that size, don't rely on it */
                queue_size_update();
                return;
            }
        }
        pending.reset();

        update_all_sizes();
    }

    void scale_changed(double s)
    {
        scale = s;
        update_title_bar();
        update_icon();
        for (auto &t : terms) {
            t->renderer.scale_x = t->renderer.scale_y = (float)scale;
            if (t->renderer.swapchain)
                t->renderer.update_transform();
        }
        wui_cb_dpi_changed((int)std::lround(scale * 96));
    }

    void update_all_sizes()
    {
        for (auto &t : terms)
            t->update_size(true);
    }

    /* For changes that don't trigger a layout of their own, or that we
     * don't want to report back to the C side while it's still in the
     * middle of making them */
    void queue_size_update()
    {
        if (size_update_queued)
            return;
        size_update_queued = true;
        window.DispatcherQueue().TryEnqueue([this]() {
            size_update_queued = false;
            update_all_sizes();
        });
    }

    void resize_client(int content_w_wanted, int content_h_wanted)
    {
        int cw = chrome_known ? chrome_w : 0;
        int ch = chrome_known ? chrome_h :
            (int)std::lround(TAB_STRIP_ESTIMATE * scale);
        /*
         * (Not with AppWindow.ResizeClient, whose idea of the client
         * area, with the content extended into the title bar, leaves the
         * title bar out: it makes the window that much taller than asked
         * for, 31 pixels or so. So by the window's own rectangles: the
         * window as it is, plus what the client area's to change by.)
         */
        RECT wr0, cr0;
        if (GetWindowRect(hwnd, &wr0) && GetClientRect(hwnd, &cr0)) {
            int frame_w = (wr0.right - wr0.left) - cr0.right;
            int frame_h = (wr0.bottom - wr0.top) - cr0.bottom;
            appwindow.Resize(winrt::Windows::Graphics::SizeInt32{
                    content_w_wanted + cw + frame_w,
                    content_h_wanted + ch + frame_h});
        } else {
            appwindow.ResizeClient(winrt::Windows::Graphics::SizeInt32{
                    content_w_wanted + cw, content_h_wanted + ch});
        }
        /*
         * Layout happens later, so until we hear otherwise, assume the
         * content is the size the window now is. Not necessarily the
         * size asked for: Windows can give it another (just after it's
         * been restored from maximised, say), and if it then stays that
         * size, there's no layout to tell us, and we'd go on thinking
         * the terminal had less room than it has, which leaves a band
         * at the bottom with nothing drawn in it.
         */
        content_w = content_w_wanted;
        content_h = content_h_wanted;
        RECT cr;
        if (chrome_known && GetClientRect(hwnd, &cr) &&
            cr.right - cw > 0 && cr.bottom - ch > 0) {
            content_w = cr.right - cw;
            content_h = cr.bottom - ch;
        }
    }

    void set_panel_size(WuiTerm *t, int w, int h)
    {
        if (!t->tab->single()) {
            /*
             * A pane can't make the window fit it (with others sharing
             * it), so tell it the size it's got, to fit itself to that.
             * (Not without end, though, in case that leads to its
             * asking again: see update_size.)
             */
            if (t->refusals++ < 2) {
                std::weak_ptr<WuiTerm> weak = t->weak_from_this();
                window.DispatcherQueue().TryEnqueue([weak]() {
                    auto t = weak.lock();
                    if (!t)
                        return;
                    int pw, ph;
                    t->current_size(pw, ph);
                    if (pw > 0 && ph > 0)
                        wui_cb_panel_resized(t->ctx, pw, ph);
                });
            }
            return;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        pending = t->weak_from_this();
        pending_w = w;
        pending_h = h;
        pending_tries = 0;
        resize_client(w + t->scrollbar_px(), h);
    }

    void recover_device()
    {
        /* The GPU went away: start again from scratch */
        gpu.create_device();
        for (auto &t : terms) {
            t->renderer.reset();
            if (t->panel_w > 0 && t->panel_h > 0 &&
                t->renderer.resize(t->panel_w, t->panel_h, t->bg))
                t->set_swapchain();
            wui_cb_redraw(t->ctx);
        }
    }

    WuiTerm *make_term(void *ctx, WuiTab *tab)
    {
        auto t = std::make_shared<WuiTerm>(this, ctx, gpu);
        t->tab = tab;
        t->item = tab->item;
        t->renderer.scale_x = t->renderer.scale_y = (float)scale;
        t->create();
        terms.push_back(t);
        return t.get();
    }

    /* Make a pane the one that's active, if its tab's selected */
    void activate_pane(WuiTerm *t)
    {
        if (!active || active->tab != t->tab || active == t)
            return;
        WuiTerm *old = active;
        active = t;
        old->report_focus();
        wui_cb_tab_activated(t->ctx);
        if (tinted_tabs)
            t->update_tab_colours();
    }

    WuiTerm *new_term(void *ctx)
    {
        tablist.push_back(std::make_unique<WuiTab>(this));
        WuiTab *tab = tablist.back().get();
        tab->create();
        WuiTerm *t = make_term(ctx, tab);
        tab->tree = std::make_unique<PaneNode>();
        tab->tree->term = t;
        tab->focused = t;
        tab->container.Children().Append(t->frame);
        tab->update_frames();
        t->update_size(false);
        contents.Children().Append(tab->container);
        tabs.TabItems().Append(tab->item);
        /* It's the visible tab from now on, even if the TabView doesn't
         * get round to telling us so until after it's been laid out */
        WuiTerm *old = active;
        active = t;
        show_active();
        if (old)
            old->report_focus();
        tabs.SelectedItem(tab->item);
        return t;
    }

    /* A new pane, beside (or below) another, in its tab */
    WuiTerm *new_split(void *ctx, WuiTerm *beside, bool side_by_side)
    {
        WuiTab *tab = beside->tab;
        PaneNode *leaf = tab->find_leaf(beside);
        if (!leaf)
            return new_term(ctx);

        /* How big each half will be, to start with */
        int pw = (int)std::lround(beside->frame.ActualWidth() * scale);
        int ph = (int)std::lround(beside->frame.ActualHeight() * scale);
        if (pw <= 0 || ph <= 0) {
            pw = content_w;
            ph = content_h;
        }
        int bar = (int)std::lround(SPLITTER_WIDTH * scale);
        WuiTerm *t = make_term(ctx, tab);
        t->est_w = side_by_side ? (pw - bar) / 2 : pw;
        t->est_h = side_by_side ? ph : (ph - bar) / 2;

        /* The pane's place in its parent, which the split takes over */
        mux::FrameworkElement old = beside->frame;
        auto parent = leaf->parent ? leaf->parent->grid : tab->container;
        uint32_t index = 0;
        parent.Children().IndexOf(old, index);
        int row = muxc::Grid::GetRow(old), col = muxc::Grid::GetColumn(old);
        parent.Children().RemoveAt(index);

        leaf->a = std::make_unique<PaneNode>();
        leaf->a->term = beside;
        leaf->a->parent = leaf;
        leaf->b = std::make_unique<PaneNode>();
        leaf->b->term = t;
        leaf->b->parent = leaf;
        leaf->term = nullptr;
        leaf->side_by_side = side_by_side;
        leaf->ratio = 0.5;
        leaf->grid = tab->make_split(leaf);
        muxc::Grid::SetRow(leaf->grid, row);
        muxc::Grid::SetColumn(leaf->grid, col);
        parent.Children().InsertAt(index, leaf->grid);

        /* The new pane has the focus */
        tab->focused = t;
        tab->update_frames();
        tab->refresh_header();
        t->update_size(false);
        if (active == beside) {
            active = t;
            beside->report_focus();
        }
        return t;
    }

    void free_term(WuiTerm *t)
    {
        auto it = std::find_if(terms.begin(), terms.end(),
                               [t](auto &p) { return p.get() == t; });
        if (it == terms.end())
            return;
        WuiTab *tab = t->tab;
        bool was_active = (active == t);
        if (was_active)
            active = nullptr;

        /* Destroy the pane object before taking it out of the XAML
         * tree, so that the events that causes (losing focus, etc)
         * find nothing to deliver to: the C side has already freed
         * the session they'd be about. */
        auto frame = t->frame;
        auto panel = t->panel;
        PaneNode *leaf = tab->find_leaf(t);
        terms.erase(it);
        t = nullptr;
        try {
            panel.as<ISwapChainPanelNative>()->SetSwapChain(nullptr);
        } catch (...) {
        }

        if (leaf && leaf->parent) {
            /* One of several panes: the other half of its split takes
             * the split's place */
            PaneNode *split = leaf->parent;
            std::unique_ptr<PaneNode> &other_ptr =
                split->a.get() == leaf ? split->b : split->a;
            std::unique_ptr<PaneNode> other = std::move(other_ptr);
            mux::FrameworkElement other_el = other->element();
            auto parent = split->parent ? split->parent->grid :
                tab->container;
            uint32_t index = 0;
            parent.Children().IndexOf(split->grid, index);
            int row = muxc::Grid::GetRow(split->grid);
            int col = muxc::Grid::GetColumn(split->grid);
            split->grid.Children().Clear();
            parent.Children().RemoveAt(index);
            other->parent = split->parent;
            muxc::Grid::SetRow(other_el, row);
            muxc::Grid::SetColumn(other_el, col);
            parent.Children().InsertAt(index, other_el);
            if (split->parent) {
                (split->parent->a.get() == split ? split->parent->a :
                 split->parent->b) = std::move(other);
            } else {
                tab->tree = std::move(other);
            }
            /* (split, and leaf in it, have gone now) */

            if (!tab->find_leaf(tab->focused) || tab->focused == nullptr) {
                std::vector<WuiTerm *> rest;
                tab->panes(rest);
                tab->focused = rest.empty() ? nullptr : rest.front();
            }
            tab->update_frames();
            tab->refresh_header();
            if (was_active && tab->focused) {
                active = tab->focused;
                wui_cb_tab_activated(active->ctx);
                std::weak_ptr<WuiTerm> weak = active->weak_from_this();
                window.DispatcherQueue().TryEnqueue([weak]() {
                    if (auto t = weak.lock())
                        t->focus();
                });
            }
            queue_size_update();
            return;
        }

        /* The only pane: the tab goes */
        auto item = tab->item;
        auto container = tab->container;
        tablist.erase(std::find_if(tablist.begin(), tablist.end(),
                                   [tab](auto &p) { return p.get() == tab; }));
        uint32_t index;
        if (contents.Children().IndexOf(container, index))
            contents.Children().RemoveAt(index);
        auto items = tabs.TabItems();
        if (items.IndexOf(item, index)) {
            items.RemoveAt(index);
            if (!tabs.SelectedItem() && items.Size() > 0)
                tabs.SelectedIndex((int)std::min(index, items.Size() - 1));
        }
        /* In case the removal didn't raise SelectionChanged */
        selection_changed();
    }

    /* Where a tab would go if moved one place left or right, or -1 */
    int move_target(WuiTerm *t, int dir)
    {
        auto items = tabs.TabItems();
        uint32_t index;
        if (!items.IndexOf(t->item, index))
            return -1;
        int to = (int)index + (dir > 0 ? 1 : -1);
        return (to >= 0 && to < (int)items.Size()) ? to : -1;
    }

    void move_tab(WuiTerm *t, int dir)
    {
        int to = move_target(t, dir);
        if (to < 0)
            return;
        auto items = tabs.TabItems();
        uint32_t index;
        items.IndexOf(t->item, index);
        /* Taking the item out may make the TabView select another one
         * for a moment; that's not a real change of tab, so the
         * session isn't told, and then the selection is put back. */
        auto selected = tabs.SelectedItem();
        moving_tab = true;
        try {
            items.RemoveAt(index);
            items.InsertAt((uint32_t)to, t->item);
            tabs.SelectedItem(selected);
        } catch (...) {
        }
        moving_tab = false;
        selection_changed();
        if (active) {
            std::weak_ptr<WuiTerm> weak = active->weak_from_this();
            window.DispatcherQueue().TryEnqueue([weak]() {
                if (auto t = weak.lock())
                    t->focus();
            });
        }
    }

    /* The n-th tab (from 1), or with 9, the last, as in browsers */
    void select_number(int n)
    {
        int count = (int)tabs.TabItems().Size();
        if (count < 1)
            return;
        int i = n >= 9 ? count - 1 : n - 1;
        if (i >= 0 && i < count)
            tabs.SelectedIndex(i);
    }

    void select_adjacent(int dir)
    {
        int n = (int)tabs.TabItems().Size();
        if (n < 2)
            return;
        int i = tabs.SelectedIndex();
        tabs.SelectedIndex(((i + (dir > 0 ? 1 : -1)) % n + n) % n);
    }

    WuiWindowState get_state()
    {
        auto presenter = appwindow.Presenter();
        if (presenter.Kind() == muw::AppWindowPresenterKind::FullScreen)
            return WUI_STATE_FULLSCREEN;
        if (auto op = presenter.try_as<muw::OverlappedPresenter>()) {
            switch (op.State()) {
              case muw::OverlappedPresenterState::Minimized:
                return WUI_STATE_MINIMISED;
              case muw::OverlappedPresenterState::Maximized:
                return WUI_STATE_MAXIMISED;
              default:
                break;
            }
        }
        return WUI_STATE_NORMAL;
    }

    void set_state(WuiWindowState state)
    {
        if (state == WUI_STATE_FULLSCREEN) {
            appwindow.SetPresenter(muw::AppWindowPresenterKind::FullScreen);
            return;
        }
        if (appwindow.Presenter().Kind() !=
            muw::AppWindowPresenterKind::Overlapped)
            appwindow.SetPresenter(overlapped);
        switch (state) {
          case WUI_STATE_MINIMISED:
            overlapped.Minimize();
            break;
          case WUI_STATE_MAXIMISED:
            overlapped.Maximize();
            break;
          default:
            if (overlapped.State() != muw::OverlappedPresenterState::Restored)
                overlapped.Restore();
            break;
        }
    }
};

void WuiTerm::create()
{
    root = muxc::Grid();
    auto c0 = muxc::ColumnDefinition();
    c0.Width(mux::GridLengthHelper::FromValueAndType(
                 1, mux::GridUnitType::Star));
    auto c1 = muxc::ColumnDefinition();
    c1.Width(mux::GridLengthHelper::Auto());
    root.ColumnDefinitions().Append(c0);
    root.ColumnDefinitions().Append(c1);

    /* Keep hold of the implementation pointer from make_self:
     * get_self on the projected UserControl would find the inner
     * (framework) object instead, since we're a composed class */
    term_impl = winrt::make_self<TermControl>();
    term = term_impl.as<muxc::UserControl>();
    term.IsTabStop(true);
    term.UseSystemFocusVisuals(false);
    term.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    term.VerticalContentAlignment(mux::VerticalAlignment::Stretch);
    panel = muxc::SwapChainPanel();
    /* The background makes the whole area respond to the pointer, not
     * just the parts with XAML content in. It's also what shows while
     * the window is being resized, before the swap chain catches up,
     * so it's the terminal's background colour. */
    area = muxc::Grid();
    area.Background(mux::Media::SolidColorBrush(mui::Colors::Black()));
    /* (and behind the scroll bar too, which is mostly transparent, so
     * that the window's backdrop doesn't show through it beside the
     * terminal: see wui_term_set_background) */
    root.Background(area.Background());
    area.Children().Append(panel);
    term.Content(area);
    muxc::Grid::SetColumn(term, 0);
    root.Children().Append(term);

    scrollbar = muxp::ScrollBar();
    scrollbar.Orientation(muxc::Orientation::Vertical);
    scrollbar.IndicatorMode(muxp::ScrollingIndicatorMode::MouseIndicator);
    scrollbar.Width(SCROLLBAR_WIDTH);
    scrollbar.RequestedTheme(mux::ElementTheme::Dark);   /* (on black) */
    scrollbar.IsTabStop(false);
    scrollbar.AllowFocusOnInteraction(false);
    scrollbar.Minimum(0);
    scrollbar.Maximum(0);
    scrollbar.SmallChange(1);
    muxc::Grid::SetColumn(scrollbar, 1);
    root.Children().Append(scrollbar);

    frame = muxc::Border();
    frame.Child(root);

    hook_events();
}

/* ---- Tabs, and the panes in them ---- */

static void update_tab_tooltip(WuiTerm *term);
static void update_tab_icon(WuiTerm *term);

void WuiTab::create()
{
    item = muxc::TabViewItem();
    item.Header(winrt::box_value(winrt::hstring(WUI_APP_NAME)));
    item.IsClosable(true);
    /* (Its content goes in WuiHost::contents, not in the item) */
    container = muxc::Grid();

    WuiTab *tab = this;
    WuiHost *w = win;
    /* (A tab's events can come in after it's gone, so they find it
     * again, if it's still there, by its header) */
    auto find = [w](muxc::TabViewItem const &item) -> WuiTab * {
        for (auto &t : w->tablist)
            if (t->item == item)
                return t.get();
        return nullptr;
    };
    auto weak_item = winrt::make_weak(item);

    /* The close button's tooltip, with its shortcut. (The button is
     * in the item's template, so it's there once the item has loaded;
     * and the item sets its own tooltip then, which this replaces.) */
    item.Loaded([weak_item](auto &&, auto &&) {
        if (auto item = weak_item.get())
            if (auto close = find_named(item, L"CloseButton"))
                muxc::ToolTipService::SetToolTip(
                    close, hint_tooltip(L"Close tab", L"Ctrl+F4"));
    });

    /* Double-clicking the tab (but not its close button) */
    item.DoubleTapped(
        [find, weak_item](auto &&, muxi::DoubleTappedRoutedEventArgs const &e) {
            auto item = weak_item.get();
            WuiTab *t = item ? find(item) : nullptr;
            if (!t || !t->focused)
                return;
            auto o = e.OriginalSource().try_as<mux::DependencyObject>();
            for (; o && o != item; o = mux::Media::VisualTreeHelper::
                     GetParent(o))
                if (o.try_as<muxp::ButtonBase>())
                    return;
            e.Handled(true);
            wui_cb_tab_double_clicked(t->focused->ctx);
        });

    /* (Right-click, Shift+F10 or the menu key on the tab) */
    item.ContextRequested(
        [find, weak_item](auto &&, muxi::ContextRequestedEventArgs const &e) {
            auto item = weak_item.get();
            WuiTab *t = item ? find(item) : nullptr;
            if (t && t->focused)
                t->focused->show_tab_menu(e);
        });

    /* The tab's whole area has been laid out at a new size */
    container.SizeChanged([find, weak_item](auto &&, auto &&) {
        auto item = weak_item.get();
        WuiTab *t = item ? find(item) : nullptr;
        if (!t || !t->focused)
            return;
        WuiHost *w = t->win;
        if (!w->active || w->active->tab == t)
            w->content_changed(t->focused);
    });
    (void)tab;
}

/* With more than one pane, a line round each, in the accent colour for
 * the one with the focus */
void WuiTab::update_frames()
{
    std::vector<WuiTerm *> list;
    panes(list);
    bool several = list.size() > 1;
    mux::Media::Brush accent{nullptr};
    try {
        accent = mux::Media::SolidColorBrush(winrt::unbox_value<
            winrt::Windows::UI::Color>(
                mux::Application::Current().Resources().Lookup(
                    winrt::box_value(L"SystemAccentColor"))));
    } catch (...) {
        accent = mux::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 120, 212));
    }
    mux::Media::SolidColorBrush clear(mui::Colors::Transparent());
    for (WuiTerm *t : list) {
        t->frame.BorderThickness(mux::ThicknessHelper::FromUniformLength(
                                     several ? 1 : 0));
        t->frame.BorderBrush(several && t == focused ? accent :
                             mux::Media::Brush(clear));
    }
}

/* The header shows the pane with the focus: its title and status */
void WuiTab::refresh_header()
{
    if (!focused)
        return;
    WuiTerm *t = focused;
    std::vector<WuiTerm *> list;
    panes(list);
    winrt::hstring header = !custom.empty() ? custom :
        t->label.empty() ? t->title : t->label;
    if (renaming)
        return;                        /* (the box is there instead) */
    /* (and how many more panes it has, whatever it's called, so that
     * a tab given a name of its own still says it's split) */
    if (list.size() > 1) {
        wchar_t more[32];
        swprintf(more, 32, L" (+%u)", (unsigned)(list.size() - 1));
        header = header + more;
    }
    item.Header(winrt::box_value(header));
    update_tab_tooltip(t);
    update_tab_icon(t);
    if (win->tinted_tabs)
        t->update_tab_colours();
}

/*
 * Renaming the tab, for now (it's not saved anywhere): a box to type
 * the name in, in place of the title on the tab, starting as what it
 * says now. Return, or clicking away, keeps what's typed; Escape keeps
 * what it was; and nothing typed goes back to the title the tab would
 * have anyway.
 */
void WuiTab::start_rename()
{
    if (renaming || !focused)
        return;
    winrt::hstring now = !custom.empty() ? custom :
        focused->label.empty() ? focused->title : focused->label;
    renaming = true;
    muxc::TextBox box;
    box.Text(now);
    box.MinWidth(120);
    box.Padding(mux::ThicknessHelper::FromLengths(6, 2, 6, 2));
    box.MinHeight(0);
    box.VerticalAlignment(mux::VerticalAlignment::Center);

    WuiHost *w = win;
    auto weak_item = winrt::make_weak(item);
    auto find = [w, weak_item]() -> WuiTab * {
        auto item = weak_item.get();
        for (auto &t : w->tablist)
            if (item && t->item == item)
                return t.get();
        return nullptr;
    };
    box.Loaded([](wf::IInspectable const &sender, auto &&) {
        auto b = sender.as<muxc::TextBox>();
        b.Focus(mux::FocusState::Programmatic);
        b.SelectAll();
    });
    box.KeyDown([find](wf::IInspectable const &sender,
                       muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        WuiTab *tab = find();
        if (!tab)
            return;
        if (e.Key() == VirtualKey::Enter) {
            e.Handled(true);
            winrt::hstring text = sender.as<muxc::TextBox>().Text();
            tab->end_rename(&text);
        } else if (e.Key() == VirtualKey::Escape) {
            e.Handled(true);
            tab->end_rename(nullptr);
        }
    });
    box.LostFocus([find](wf::IInspectable const &sender, auto &&) {
        if (WuiTab *tab = find(); tab && tab->renaming) {
            winrt::hstring text = sender.as<muxc::TextBox>().Text();
            tab->end_rename(&text);
        }
    });
    item.Header(box);
}

void WuiTab::end_rename(winrt::hstring const *text)
{
    if (!renaming)
        return;
    renaming = false;
    if (text) {
        std::wstring s(*text);
        s.erase(0, s.find_first_not_of(L" \t"));
        s.erase(s.find_last_not_of(L" \t") + 1);
        custom = winrt::hstring(s);
    }
    refresh_header();
    /* (and back to typing in the terminal) */
    if (focused)
        focused->focus();
}

/* A pane's had the focus put in it */
void WuiTab::set_focused(WuiTerm *t)
{
    if (focused == t)
        return;
    focused = t;
    update_frames();
    refresh_header();
    win->activate_pane(t);
}

/* The grid for a split: its two halves, with the bar between them */
muxc::Grid WuiTab::make_split(PaneNode *n)
{
    muxc::Grid g;
    bool sbs = n->side_by_side;
    auto star = [](double v) {
        return mux::GridLengthHelper::FromValueAndType(
            v, mux::GridUnitType::Star);
    };
    for (int i = 0; i < 3; i++) {
        auto len = i == 1 ? mux::GridLengthHelper::FromPixels(SPLITTER_WIDTH) :
            star(i == 0 ? n->ratio : 1 - n->ratio);
        if (sbs) {
            muxc::ColumnDefinition c;
            c.Width(len);
            g.ColumnDefinitions().Append(c);
        } else {
            muxc::RowDefinition r;
            r.Height(len);
            g.RowDefinitions().Append(r);
        }
    }
    auto place = [&](mux::FrameworkElement e, int i) {
        muxc::Grid::SetRow(e, sbs ? 0 : i);
        muxc::Grid::SetColumn(e, sbs ? i : 0);
        g.Children().Append(e);
    };
    place(n->a->element(), 0);
    place(n->b->element(), 2);

    /* The bar: drag it to move it, double-click it to put it back in
     * the middle */
    auto impl = winrt::make_self<SplitterControl>();
    auto bar = impl.as<muxc::UserControl>();
    muxc::Border fill;
    fill.Background(mux::Media::SolidColorBrush(mui::Colors::Transparent()));
    bar.Content(fill);
    bar.IsTabStop(false);
    impl->set_cursor(sbs);
    auto weak_grid = winrt::make_weak(g);
    auto set_ratio = [weak_grid, sbs, star](double ratio) {
        auto g = weak_grid.get();
        if (!g)
            return;
        ratio = std::clamp(ratio, 0.1, 0.9);
        if (sbs) {
            g.ColumnDefinitions().GetAt(0).Width(star(ratio));
            g.ColumnDefinitions().GetAt(2).Width(star(1 - ratio));
        } else {
            g.RowDefinitions().GetAt(0).Height(star(ratio));
            g.RowDefinitions().GetAt(2).Height(star(1 - ratio));
        }
    };
    auto dragging = std::make_shared<bool>(false);
    bar.PointerPressed([bar, dragging](auto &&, muxi::PointerRoutedEventArgs
                                       const &e) {
        *dragging = bar.CapturePointer(e.Pointer());
        e.Handled(true);
    });
    bar.PointerMoved([weak_grid, sbs, dragging, set_ratio](
                         auto &&, muxi::PointerRoutedEventArgs const &e) {
        auto g = weak_grid.get();
        if (!*dragging || !g)
            return;
        auto pos = e.GetCurrentPoint(g).Position();
        double total = (sbs ? g.ActualWidth() : g.ActualHeight()) -
            SPLITTER_WIDTH;
        if (total > 0)
            set_ratio(((sbs ? pos.X : pos.Y) - SPLITTER_WIDTH / 2) / total);
        e.Handled(true);
    });
    auto stop = [bar, dragging](auto &&, muxi::PointerRoutedEventArgs
                                const &e) {
        if (*dragging)
            bar.ReleasePointerCapture(e.Pointer());
        *dragging = false;
    };
    bar.PointerReleased(stop);
    bar.PointerCaptureLost([dragging](auto &&, auto &&) { *dragging = false; });
    bar.DoubleTapped([set_ratio](auto &&, auto &&) { set_ratio(0.5); });
    place(bar, 1);
    return g;
}

void WuiTerm::hook_events()
{
    std::weak_ptr<WuiTerm> weak = weak_from_this();

    /* (The tab's own events are WuiTab's: see WuiTab::create) */
    root.SizeChanged([weak](auto &&, auto &&) {
        auto t = weak.lock();
        if (!t)
            return;
        /* A pane among others has a size of its own; otherwise it's the
         * tab's (see WuiTerm::current_size), which the tab looks after */
        if (!t->tab->single())
            t->update_size(true);
        /* (A tab that's just been made visible gets one of these) */
        t->focus_if_pending();
    });
    panel.CompositionScaleChanged([weak](auto &&, auto &&) {
        if (auto t = weak.lock(); t && t->win->active == t.get())
            t->win->content_changed(t.get());
    });

    term.Loaded([weak](auto &&, auto &&) {
        if (auto t = weak.lock())
            t->focus_if_pending();
    });
    term.GotFocus([weak](auto &&, auto &&) {
        if (auto t = weak.lock()) {
            t->focus_on_load = false;
            t->term_focused = true;
            t->tab->set_focused(t.get());
            t->report_focus();
        }
    });
    term.LostFocus([weak](auto &&, auto &&) {
        if (auto t = weak.lock()) {
            t->term_focused = false;
            t->report_focus();
        }
    });
    term.CharacterReceived(
        [weak](auto &&, muxi::CharacterReceivedRoutedEventArgs const &e) {
            if (auto t = weak.lock())
                wui_cb_char(t->ctx, (wchar_t)e.Character());
            e.Handled(true);
        });

    term.PointerPressed([weak](auto &&, muxi::PointerRoutedEventArgs const &e) {
        if (auto t = weak.lock()) {
            t->term.Focus(mux::FocusState::Pointer);
            t->term.CapturePointer(e.Pointer());
            t->pointer_event(e);
        }
    });
    term.PointerMoved([weak](auto &&, muxi::PointerRoutedEventArgs const &e) {
        if (auto t = weak.lock())
            t->pointer_event(e);
    });
    term.PointerReleased([weak](auto &&, muxi::PointerRoutedEventArgs const &e) {
        if (auto t = weak.lock()) {
            t->pointer_event(e);
            t->term.ReleasePointerCapture(e.Pointer());
        }
    });
    term.PointerWheelChanged(
        [weak](auto &&, muxi::PointerRoutedEventArgs const &e) {
            auto t = weak.lock();
            if (!t)
                return;
            auto pt = e.GetCurrentPoint(t->panel);
            auto props = pt.Properties();
            WuiPointerEvent ev = t->make_event(e, pt);
            ev.kind = WUI_POINTER_WHEEL;
            ev.wheel_delta = props.MouseWheelDelta();
            ev.wheel_horizontal = props.IsHorizontalMouseWheel();
            wui_cb_pointer(t->ctx, &ev);
            e.Handled(true);
        });

    scrollbar.Scroll([weak](auto &&, muxp::ScrollEventArgs const &e) {
        auto t = weak.lock();
        if (!t)
            return;
        using muxp::ScrollEventType;
        WuiScrollKind kind;
        switch (e.ScrollEventType()) {
          case ScrollEventType::SmallDecrement:
            kind = WUI_SCROLL_LINEUP;
            break;
          case ScrollEventType::SmallIncrement:
            kind = WUI_SCROLL_LINEDOWN;
            break;
          case ScrollEventType::LargeDecrement:
            kind = WUI_SCROLL_PAGEUP;
            break;
          case ScrollEventType::LargeIncrement:
            kind = WUI_SCROLL_PAGEDOWN;
            break;
          case ScrollEventType::First:
            kind = WUI_SCROLL_TOP;
            break;
          case ScrollEventType::Last:
            kind = WUI_SCROLL_BOTTOM;
            break;
          case ScrollEventType::ThumbPosition:
          case ScrollEventType::ThumbTrack:
            kind = WUI_SCROLL_POSITION;
            break;
          default:
            return;
        }
        wui_cb_scroll(t->ctx, kind, (int)std::lround(e.NewValue()));
    });
}

/*
 * The tab's colours when it's selected: its terminal's background, so
 * that the two run together, with text that shows up on it; or (if
 * the window's tabs aren't tinted) the theme's. While it's being
 * dragged, the tab shows a background of its own instead, which has
 * to match too, but only for the selected tab: another one is dragged
 * with its unselected text, which needn't show up on that colour.
 */
void WuiTerm::update_tab_colours()
{
    static const wchar_t *const bg_keys[] = {
        L"TabViewItemHeaderBackgroundSelected",
        L"TabViewSelectedItemBorderBrush",
    };
    static const wchar_t *const fg_keys[] = {
        L"TabViewItemHeaderForegroundSelected",
        L"TabViewItemIconForegroundSelected",
        L"TabViewItemHeaderSelectedCloseButtonForeground",
    };
    auto res = item.Resources();
    auto set = [&](const wchar_t *key, mux::Media::Brush const &brush) {
        auto k = winrt::box_value(winrt::hstring(key));
        if (brush)
            res.Insert(k, brush);
        else if (res.HasKey(k))
            res.Remove(k);
    };
    mux::Media::SolidColorBrush bg_brush{nullptr}, fg_brush{nullptr};
    if (win->tinted_tabs) {
        bg_brush = mux::Media::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(
                255, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
        int luma = (GetRValue(bg) * 299 + GetGValue(bg) * 587 +
                    GetBValue(bg) * 114) / 1000;
        fg_brush = mux::Media::SolidColorBrush(
            luma < 128 ? mui::Colors::White() : mui::Colors::Black());
    }
    for (auto key : bg_keys)
        set(key, bg_brush);
    for (auto key : fg_keys)
        set(key, fg_brush);
    set(L"TabViewItemHeaderDragBackground",
        win->active == this ? bg_brush : nullptr);

    /* The template only looks its theme resources up again when the
     * theme changes, so make it */
    auto theme = item.RequestedTheme();
    item.RequestedTheme(theme == mux::ElementTheme::Dark ?
                        mux::ElementTheme::Light : mux::ElementTheme::Dark);
    item.RequestedTheme(theme);
}

int WuiTerm::scrollbar_px()
{
    return scrollbar_visible ?
        (int)std::lround(SCROLLBAR_WIDTH * win->scale) : 0;
}

/* Every tab's terminal area is the shared content area, less its own
 * scroll bar if it has one; except that a pane among others has its own
 * part of that */
void WuiTerm::current_size(int &w, int &h)
{
    if (tab && !tab->single()) {
        double aw = root.ActualWidth(), ah = root.ActualHeight();
        if (aw > 0 && ah > 0) {
            w = (int)std::lround(aw * win->scale) - scrollbar_px();
            h = (int)std::lround(ah * win->scale);
        } else {
            w = est_w - scrollbar_px();
            h = est_h;
        }
        return;
    }
    w = win->content_w - scrollbar_px();
    h = win->content_h;
}

void WuiTerm::set_swapchain()
{
    check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(
                      renderer.swapchain.get()));
}

void WuiTerm::update_size(bool notify)
{
    int w, h;
    current_size(w, h);
    if (w <= 0 || h <= 0)
        return;

    bool resized = (w != renderer.width || h != renderer.height ||
                    !renderer.swapchain);
    if (resized && renderer.resize(w, h, bg))
        set_swapchain();
    if (w != panel_w || h != panel_h) {
        panel_w = w;
        panel_h = h;
        refusals = 0;
        if (notify)
            wui_cb_panel_resized(ctx, w, h);
    }
    if (resized && notify)
        wui_cb_redraw(ctx);
}

void WuiTerm::report_focus()
{
    wui_cb_focus(ctx, win->window_active && term_focused &&
                 win->active == this);
}

/*
 * Focus() does nothing to an element that isn't loaded and visible,
 * which is the case for a new tab, or one just selected, until it's
 * been laid out. The focus then stays on the tab header, so keystrokes
 * never reach the terminal. So if it's too early, try again when the
 * terminal is loaded or shown.
 */
void WuiTerm::focus()
{
    if (!term.Focus(mux::FocusState::Programmatic))
        focus_on_load = true;
}

void WuiTerm::focus_if_pending()
{
    if (!focus_on_load || win->active != this)
        return;
    if (term.Focus(mux::FocusState::Programmatic))
        focus_on_load = false;
}

WuiPointerEvent WuiTerm::make_event(muxi::PointerRoutedEventArgs const &e,
                                    mui::Input::PointerPoint const &pt)
{
    WuiPointerEvent ev{};
    auto props = pt.Properties();
    auto pos = pt.Position();
    ev.x = (int)std::floor(pos.X * win->scale);
    ev.y = (int)std::floor(pos.Y * win->scale);
    ev.left_down = props.IsLeftButtonPressed();
    ev.middle_down = props.IsMiddleButtonPressed();
    ev.right_down = props.IsRightButtonPressed();
    auto mods = e.KeyModifiers();
    using winrt::Windows::System::VirtualKeyModifiers;
    ev.shift = (mods & VirtualKeyModifiers::Shift) ==
        VirtualKeyModifiers::Shift;
    ev.ctrl = (mods & VirtualKeyModifiers::Control) ==
        VirtualKeyModifiers::Control;
    ev.time_ms = (unsigned long)(pt.Timestamp() / 1000);
    return ev;
}

/*
 * XAML only raises PointerPressed for the first button to go down,
 * and PointerReleased for the last to come up; changes to the other
 * buttons in between arrive as PointerMoved. So we decide what
 * happened from the update kind in every case.
 */
void WuiTerm::pointer_event(muxi::PointerRoutedEventArgs const &e)
{
    auto pt = e.GetCurrentPoint(panel);
    WuiPointerEvent ev = make_event(e, pt);
    using mui::Input::PointerUpdateKind;
    switch (pt.Properties().PointerUpdateKind()) {
      case PointerUpdateKind::LeftButtonPressed:
        ev.kind = WUI_POINTER_PRESS;
        ev.button = WUI_BUTTON_LEFT;
        break;
      case PointerUpdateKind::MiddleButtonPressed:
        ev.kind = WUI_POINTER_PRESS;
        ev.button = WUI_BUTTON_MIDDLE;
        break;
      case PointerUpdateKind::RightButtonPressed:
        ev.kind = WUI_POINTER_PRESS;
        ev.button = WUI_BUTTON_RIGHT;
        break;
      case PointerUpdateKind::LeftButtonReleased:
        ev.kind = WUI_POINTER_RELEASE;
        ev.button = WUI_BUTTON_LEFT;
        break;
      case PointerUpdateKind::MiddleButtonReleased:
        ev.kind = WUI_POINTER_RELEASE;
        ev.button = WUI_BUTTON_MIDDLE;
        break;
      case PointerUpdateKind::RightButtonReleased:
        ev.kind = WUI_POINTER_RELEASE;
        ev.button = WUI_BUTTON_RIGHT;
        break;
      default:
        ev.kind = WUI_POINTER_MOVE;
        break;
    }
    wui_cb_pointer(ctx, &ev);
    e.Handled(true);
}

/* XAML menus don't use '&' accelerator markers */
static winrt::hstring menu_label(const wchar_t *label)
{
    std::wstring s;
    for (const wchar_t *p = label ? label : L""; *p; p++) {
        if (*p == L'&') {
            if (p[1] == L'&')
                s += *++p;
            continue;
        }
        s += *p;
    }
    return winrt::hstring(s);
}

/* A menu icon from the theme's symbol font. (FontIcon doesn't use
 * that by default: the glyphs are in the Private Use Area, so in an
 * ordinary text font they come out blank.) */
static muxc::IconElement menu_icon(unsigned glyph)
{
    muxc::FontIcon icon;
    mux::Media::FontFamily font{nullptr};
    try {
        font = mux::Application::Current().Resources().Lookup(
            winrt::box_value(L"SymbolThemeFontFamily"))
            .as<mux::Media::FontFamily>();
    } catch (...) {
        font = mux::Media::FontFamily(L"Segoe Fluent Icons,Segoe MDL2 Assets");
    }
    icon.FontFamily(font);
    wchar_t s[2] = { (wchar_t)glyph, L'\0' };
    icon.Glyph(s);
    return icon;
}

/* ---- Find ---- */

/*
 * The search box, as in Windows Terminal: the text, which match is
 * selected out of how many, buttons to go up (to the previous one,
 * which is what Enter does, as the search starts from the bottom) and
 * down, to match case, to take the text as a regular expression, and
 * to close it. The searching is done by the C side.
 */
void WuiTerm::make_search_bar()
{
    std::weak_ptr<WuiTerm> weak = weak_from_this();

    search_box = muxc::TextBox();
    search_box.Width(200);
    search_box.PlaceholderText(L"Find");
    search_box.VerticalAlignment(mux::VerticalAlignment::Center);
    search_box.TextChanged([weak](auto &&, auto &&) {
        if (auto t = weak.lock())
            t->search(0);
    });
    search_box.KeyDown([weak](auto &&, muxi::KeyRoutedEventArgs const &e) {
        auto t = weak.lock();
        if (!t || e.Key() != winrt::Windows::System::VirtualKey::Enter)
            return;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        t->search(shift ? +1 : -1);
        e.Handled(true);
    });

    search_count = muxc::TextBlock();
    search_count.MinWidth(48);
    search_count.TextAlignment(mux::TextAlignment::Center);
    search_count.VerticalAlignment(mux::VerticalAlignment::Center);
    search_count.Margin(mux::ThicknessHelper::FromLengths(4, 0, 4, 0));

    auto transparent = mux::Media::SolidColorBrush(mui::Colors::Transparent());
    auto style = [&](muxc::Control const &b, muxc::TextBlock tip) {
        b.Background(transparent);
        b.BorderBrush(transparent);
        b.MinWidth(32);
        b.Height(32);
        b.Padding(mux::ThicknessHelper::FromLengths(6, 0, 6, 0));
        b.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::ToolTipService::SetToolTip(b, tip);
    };
    auto tip = [](const wchar_t *what) {
        muxc::TextBlock t;
        t.Text(what);
        return t;
    };
    auto button = [&](unsigned glyph, muxc::TextBlock t, int dir) {
        muxc::Button b;
        b.Content(menu_icon(glyph));
        style(b, t);
        b.Click([weak, dir](auto &&, auto &&) {
            if (auto t = weak.lock()) {
                if (dir)
                    t->search(dir);
                else
                    t->hide_search();
            }
        });
        return b;
    };
    auto toggle = [&](const wchar_t *label, muxc::TextBlock t) {
        muxp::ToggleButton b;
        b.Content(winrt::box_value(winrt::hstring(label)));
        style(b, t);
        b.Click([weak](auto &&, auto &&) {
            if (auto t = weak.lock())
                t->search(0);
        });
        return b;
    };
    search_case = toggle(L"Aa", tip(L"Match case"));
    search_regex = toggle(L"^$", tip(L"Regular expression"));

    muxc::StackPanel sp;
    sp.Orientation(muxc::Orientation::Horizontal);
    sp.Spacing(4);
    sp.Children().Append(search_box);
    sp.Children().Append(search_count);
    sp.Children().Append(button(0xE74A,                   /* Up */
                                hint_tooltip(L"Previous match", L"Enter"),
                                -1));
    sp.Children().Append(button(0xE74B,                   /* Down */
                                hint_tooltip(L"Next match", L"Shift+Enter"),
                                +1));
    sp.Children().Append(search_case);
    sp.Children().Append(search_regex);
    sp.Children().Append(button(0xE711,                   /* Cancel */
                                hint_tooltip(L"Close", L"Esc"), 0));

    /* (in XAML, so that its colours follow the window's theme) */
    search_bar = mux::Markup::XamlReader::Load(
        L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/"
        L"presentation' BorderThickness='1' CornerRadius='8' "
        L"Padding='8,6,6,6' HorizontalAlignment='Right' "
        L"Background='{ThemeResource AcrylicInAppFillColorDefaultBrush}' "
        L"BorderBrush='{ThemeResource SurfaceStrokeColorFlyoutBrush}'/>")
        .as<muxc::Border>();
    search_bar.Child(sp);
    /* Escape from anywhere in it */
    search_bar.KeyDown([weak](auto &&, muxi::KeyRoutedEventArgs const &e) {
        auto t = weak.lock();
        if (t && e.Key() == winrt::Windows::System::VirtualKey::Escape) {
            t->hide_search();
            e.Handled(true);
        }
    });
    get_bars().Children().Append(search_bar);
}

/* The panel for Find's and Send File's bars */
muxc::StackPanel &WuiTerm::get_bars()
{
    if (!bars) {
        bars = muxc::StackPanel();
        bars.Spacing(8);
        bars.HorizontalAlignment(mux::HorizontalAlignment::Right);
        bars.VerticalAlignment(mux::VerticalAlignment::Top);
        bars.Margin(mux::ThicknessHelper::FromLengths(0, 8, 16, 0));
        /* (over the terminal, not the scroll bar) */
        muxc::Grid::SetColumn(bars, 0);
        root.Children().Append(bars);
    }
    return bars;
}

void WuiTerm::show_search()
{
    if (!search_bar)
        make_search_bar();
    search_bar.Visibility(mux::Visibility::Visible);
    /* (and again once the menu this came from has closed, which puts
     * the focus back where it was) */
    std::weak_ptr<WuiTerm> weak = weak_from_this();
    auto grab = [weak]() {
        if (auto t = weak.lock()) {
            t->search_box.Focus(mux::FocusState::Programmatic);
            t->search_box.SelectAll();
        }
    };
    grab();
    win->window.DispatcherQueue().TryEnqueue(grab);
}

void WuiTerm::hide_search()
{
    search_bar.Visibility(mux::Visibility::Collapsed);
    wui_cb_search_closed(ctx);
    focus();
}

void WuiTerm::search(int dir)
{
    auto checked = [](muxp::ToggleButton const &b) {
        auto c = b.IsChecked();
        return c && c.Value();
    };
    winrt::hstring text = search_box.Text();
    wui_cb_search(ctx, text.c_str(), checked(search_case),
                  checked(search_regex), dir);
}

extern "C" void wui_term_show_search(WuiTerm *term)
{
    term->show_search();
}

extern "C" void wui_term_search_result(WuiTerm *term, int which, int total)
{
    if (!term->search_count)
        return;
    wchar_t buf[64];
    if (total < 0)
        wcscpy(buf, L"Invalid");
    else if (term->search_box.Text().empty())
        buf[0] = L'\0';
    else
        swprintf(buf, sizeof(buf) / sizeof(*buf), L"%d/%d", which, total);
    term->search_count.Text(buf);
}

/* ---- Send File and Receive File ---- */

/*
 * The bar for sending a file to the server (with the file's name, and
 * where to, with the places files went lately) or receiving one (with
 * where from), with what's in the remote directory offered as you
 * type; and a button to do it; then how it's going. The transferring
 * is done by the C side.
 */

/* What to put in the shell's startup file for it to say where it is */
static const wchar_t osc7_bash[] =
    L"PROMPT_COMMAND='printf \"\\e]7;file://%s%s\\a\" \"$HOSTNAME\" \"$PWD\"'";
static const wchar_t osc7_zsh[] =
    L"autoload -Uz add-zsh-hook; "
    L"_osc7() { printf '\\e]7;file://%s%s\\a' \"$HOST\" \"$PWD\"; }; "
    L"add-zsh-hook precmd _osc7";

static void copy_text(HWND hwnd, const wchar_t *text)
{
    size_t size = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem)
        return;
    memcpy(GlobalLock(mem), text, size);
    GlobalUnlock(mem);
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, mem))
            mem = nullptr;
        CloseClipboard();
    }
    if (mem)
        GlobalFree(mem);
}

/* The explanation of how to make the shell say where it is */
static muxc::Flyout osc7_flyout(HWND hwnd)
{
    muxc::StackPanel sp;
    sp.Spacing(8);
    sp.MaxWidth(520);

    muxc::TextBlock intro;
    intro.TextWrapping(mux::TextWrapping::Wrap);
    intro.Text(L"If the shell on the server says which directory it's in, "
               L"files are sent to there, and received from there. To make "
               L"it, add this to the end of the startup file on the "
               L"server, and log in again:");
    sp.Children().Append(intro);

    auto snippet = [&](const wchar_t *file, const wchar_t *code) {
        muxc::TextBlock label;
        label.Text(file);
        label.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
        sp.Children().Append(label);

        muxc::Grid row;
        row.ColumnSpacing(8);
        muxc::ColumnDefinition c1, c2;
        c1.Width(mux::GridLengthHelper::FromValueAndType(
                     1, mux::GridUnitType::Star));
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::TextBox box;
        box.Text(code);
        box.IsReadOnly(true);
        box.TextWrapping(mux::TextWrapping::Wrap);
        box.FontFamily(mux::Media::FontFamily(L"Cascadia Mono, Consolas"));
        muxc::Grid::SetColumn(box, 0);
        row.Children().Append(box);

        muxc::Button copy;
        copy.Content(menu_icon(0xE8C8));               /* Copy */
        copy.VerticalAlignment(mux::VerticalAlignment::Top);
        muxc::ToolTipService::SetToolTip(
            copy, winrt::box_value(winrt::hstring(L"Copy")));
        std::wstring text = code;
        copy.Click([hwnd, text](auto &&, auto &&) {
            copy_text(hwnd, text.c_str());
        });
        muxc::Grid::SetColumn(copy, 1);
        row.Children().Append(copy);

        sp.Children().Append(row);
    };
    snippet(L"bash: ~/.bashrc", osc7_bash);
    snippet(L"zsh: ~/.zshrc", osc7_zsh);

    muxc::TextBlock note;
    note.TextWrapping(mux::TextWrapping::Wrap);
    note.Opacity(0.7);
    note.Text(L"(fish does it already.)");
    sp.Children().Append(note);

    muxc::Flyout flyout;
    flyout.Content(sp);
    return flyout;
}

void WuiTerm::make_send_bar()
{
    std::weak_ptr<WuiTerm> weak = weak_from_this();
    auto transparent = mux::Media::SolidColorBrush(mui::Colors::Transparent());
    auto icon_button = [&](unsigned glyph, muxc::TextBlock tip) {
        muxc::Button b;
        b.Content(menu_icon(glyph));
        b.Background(transparent);
        b.BorderBrush(transparent);
        b.MinWidth(32);
        b.Height(32);
        b.Padding(mux::ThicknessHelper::FromLengths(6, 0, 6, 0));
        b.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::ToolTipService::SetToolTip(b, tip);
        return b;
    };
    auto tip = [](const wchar_t *what) {
        muxc::TextBlock t;
        t.Text(what);
        return t;
    };
    /* (after a change of mind, forget about replacing, what went
     * wrong, or what was received) */
    auto changed = [](WuiTerm *t) {
        if (t->send_state == WUI_SEND_EXISTS ||
            t->send_state == WUI_SEND_ERROR ||
            (t->send_state == WUI_SEND_DONE && t->bar_receive))
            t->set_send_state(WUI_SEND_READY, L"", -1);
    };

    /* What, and where to or from */
    send_icon = muxc::FontIcon();
    send_icon.FontSize(16);
    send_icon.VerticalAlignment(mux::VerticalAlignment::Center);

    send_name = muxc::TextBlock();
    send_name.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    send_name.MaxWidth(200);
    send_name.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    send_name.VerticalAlignment(mux::VerticalAlignment::Center);

    send_arrow = muxc::FontIcon();
    send_arrow.Glyph(L"\xE72A");                        /* Forward */
    send_arrow.FontSize(12);
    send_arrow.Opacity(0.7);
    send_arrow.VerticalAlignment(mux::VerticalAlignment::Center);

    send_dest = muxc::AutoSuggestBox();
    send_dest.Width(320);
    send_dest.UpdateTextOnSelect(false);
    send_dest.VerticalAlignment(mux::VerticalAlignment::Center);
    send_dest.TextChanged([weak, changed](
                              muxc::AutoSuggestBox const &,
                              muxc::AutoSuggestBoxTextChangedEventArgs
                              const &e) {
        auto t = weak.lock();
        if (!t || e.Reason() !=
            muxc::AutoSuggestionBoxTextChangeReason::UserInput)
            return;
        t->send_dest_typed = true;
        changed(t.get());
        t->complete(false);
    });
    /* (choosing one, by clicking it or going to it with the arrow
     * keys, just puts it in the box) */
    send_dest.SuggestionChosen([weak, changed](
                                   muxc::AutoSuggestBox const &box,
                                   muxc::AutoSuggestBoxSuggestionChosenEventArgs
                                   const &e) {
        auto t = weak.lock();
        if (!t)
            return;
        auto item = winrt::unbox_value<winrt::hstring>(e.SelectedItem());
        box.Text(t->suggesting_recent ? item :
                 winrt::hstring(t->list_dir + item.c_str()));
        t->send_dest_typed = true;
        changed(t.get());
    });
    /* (and then Enter or clicking it goes into a directory; Enter
     * otherwise is the button) */
    send_dest.QuerySubmitted([weak](
                                 muxc::AutoSuggestBox const &,
                                 muxc::AutoSuggestBoxQuerySubmittedEventArgs
                                 const &e) {
        auto t = weak.lock();
        if (!t)
            return;
        if (e.ChosenSuggestion()) {
            std::wstring text = t->send_dest.Text().c_str();
            if (!text.empty() && text.back() == L'/')
                t->complete(true);
            return;
        }
        t->go();
    });

    send_recent_button = icon_button(0xE70D, tip(L""));  /* ChevronDown */
    send_recent_button.Click([weak](auto &&, auto &&) {
        auto t = weak.lock();
        if (!t)
            return;
        t->send_dest.Focus(mux::FocusState::Programmatic);
        if (t->bar_receive)
            t->complete(true);
        else
            t->show_recent();
    });

    send_button = mux::Markup::XamlReader::Load(
        L"<Button xmlns='http://schemas.microsoft.com/winfx/2006/xaml/"
        L"presentation' Style='{StaticResource AccentButtonStyle}'/>")
        .as<muxc::Button>();
    send_button.MinWidth(80);
    send_button.VerticalAlignment(mux::VerticalAlignment::Center);
    send_button.Click([weak](auto &&, auto &&) {
        if (auto t = weak.lock())
            t->go();
    });

    send_close = icon_button(0xE711, hint_tooltip(L"Close", L"Esc"));
    send_close.Click([weak](auto &&, auto &&) {
        if (auto t = weak.lock())
            t->hide_send();
    });

    muxc::StackPanel top;
    top.Orientation(muxc::Orientation::Horizontal);
    top.Spacing(8);
    top.Children().Append(send_icon);
    top.Children().Append(send_name);
    top.Children().Append(send_arrow);
    muxc::StackPanel dest_box;                /* (the box and its list) */
    dest_box.Orientation(muxc::Orientation::Horizontal);
    dest_box.Children().Append(send_dest);
    dest_box.Children().Append(send_recent_button);
    top.Children().Append(dest_box);
    top.Children().Append(send_button);
    top.Children().Append(send_close);

    /* How it's going */
    send_progress = muxc::ProgressBar();
    send_progress.Width(160);
    send_progress.Minimum(0);
    send_progress.Maximum(100);
    send_progress.VerticalAlignment(mux::VerticalAlignment::Center);

    send_warning = muxc::FontIcon();
    send_warning.Glyph(L"\xE7BA");                      /* Warning */
    send_warning.FontSize(14);
    send_warning.VerticalAlignment(mux::VerticalAlignment::Center);

    send_status = muxc::TextBlock();
    send_status.MaxWidth(520);
    send_status.TextWrapping(mux::TextWrapping::Wrap);
    send_status.VerticalAlignment(mux::VerticalAlignment::Center);

    send_show = muxc::HyperlinkButton();
    send_show.Content(winrt::box_value(winrt::hstring(L"Show in folder")));
    send_show.Padding(mux::ThicknessHelper::FromLengths(4, 2, 4, 2));
    send_show.VerticalAlignment(mux::VerticalAlignment::Center);
    send_show.Click([weak](auto &&, auto &&) {
        if (auto t = weak.lock())
            wui_cb_show_received(t->ctx);
    });

    send_status_row = muxc::StackPanel();
    send_status_row.Orientation(muxc::Orientation::Horizontal);
    send_status_row.Spacing(8);
    send_status_row.Margin(mux::ThicknessHelper::FromLengths(24, 0, 0, 0));
    send_status_row.Children().Append(send_progress);
    send_status_row.Children().Append(send_warning);
    send_status_row.Children().Append(send_status);
    send_status_row.Children().Append(send_show);

    /* How to make it start where the shell is */
    send_help = muxc::HyperlinkButton();
    send_help.Content(winrt::box_value(winrt::hstring(
                          L"Start from the shell's current directory...")));
    send_help.Padding(mux::ThicknessHelper::FromLengths(4, 2, 4, 2));
    send_help.Margin(mux::ThicknessHelper::FromLengths(20, 0, 0, 0));
    {
        muxc::Flyout flyout = osc7_flyout(win->hwnd);
        send_help.Click([flyout](auto &&sender, auto &&) {
            flyout.ShowAt(sender.template as<mux::FrameworkElement>());
        });
    }

    muxc::StackPanel sp;
    sp.Spacing(6);
    sp.Children().Append(top);
    sp.Children().Append(send_status_row);
    sp.Children().Append(send_help);

    /* (in XAML, so that its colours follow the window's theme) */
    send_bar = mux::Markup::XamlReader::Load(
        L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/"
        L"presentation' BorderThickness='1' CornerRadius='8' "
        L"Padding='10,6,6,6' HorizontalAlignment='Right' "
        L"Background='{ThemeResource AcrylicInAppFillColorDefaultBrush}' "
        L"BorderBrush='{ThemeResource SurfaceStrokeColorFlyoutBrush}'/>")
        .as<muxc::Border>();
    send_bar.Child(sp);
    send_bar.KeyDown([weak](auto &&, muxi::KeyRoutedEventArgs const &e) {
        auto t = weak.lock();
        if (t && e.Key() == winrt::Windows::System::VirtualKey::Escape) {
            t->hide_send();
            e.Handled(true);
        }
    });
    get_bars().Children().Append(send_bar);

    /* Once a file's been sent, the bar goes too, after a moment */
    send_timer = win->window.DispatcherQueue().CreateTimer();
    send_timer.Interval(std::chrono::milliseconds(3000));
    send_timer.IsRepeating(false);
    send_timer.Tick([weak](auto &&, auto &&) {
        auto t = weak.lock();
        if (t && t->send_state == WUI_SEND_DONE && !t->bar_receive)
            t->hide_send();
    });
}

/* The button (or Enter): send the file, or receive it (or, if what's
 * typed is a directory, look in it) */
void WuiTerm::go()
{
    if (send_state == WUI_SEND_BUSY ||
        (send_state == WUI_SEND_DONE && !bar_receive))
        return;
    winrt::hstring text = send_dest.Text();
    if (!bar_receive) {
        wui_cb_send(ctx, text.c_str(), send_state == WUI_SEND_EXISTS);
    } else if (text.empty() || text.back() == L'/') {
        complete(true);
    } else {
        send_dest.IsSuggestionListOpen(false);
        wui_cb_receive(ctx, text.c_str());
    }
}

/* The places files went lately */
void WuiTerm::show_recent()
{
    auto items = winrt::single_threaded_vector<wf::IInspectable>();
    for (auto const &r : send_recent)
        items.Append(winrt::box_value(r));
    suggesting_recent = true;
    send_dest.ItemsSource(items);
    send_dest.IsSuggestionListOpen(items.Size() > 0);
}

/*
 * Offer what's in the remote directory that's been typed (up to its
 * last '/'), starting with what's been typed after that (or, if 'all',
 * everything there). When sending, that's just directories. If we
 * don't know what's there yet, ask, and offer it when we find out.
 */
void WuiTerm::complete(bool all)
{
    std::wstring text = send_dest.Text().c_str();
    size_t slash = text.rfind(L'/');
    std::wstring dir = slash == std::wstring::npos ? L"" :
        text.substr(0, slash + 1);
    std::wstring prefix = text.substr(dir.size());

    list_wanted = true;
    list_all = all;
    if (dir != list_dir || !list_asked) {
        list_dir = dir;
        list_names.clear();
        list_known = false;
        list_asked = true;
        send_dest.IsSuggestionListOpen(false);
        wui_cb_list(ctx, dir.c_str());
        return;
    }
    if (!list_known)
        return;                        /* (it'll come) */

    auto items = winrt::single_threaded_vector<wf::IInspectable>();
    bool dots = !prefix.empty() && prefix[0] == L'.';
    for (auto const &name : list_names) {
        bool is_dir = name.back() == L'/';
        if (!bar_receive && !is_dir)
            continue;
        if (name[0] == L'.' && !dots)
            continue;                  /* (hidden, unless asked for) */
        if (!all && (name.size() < prefix.size() ||
                     _wcsnicmp(name.c_str(), prefix.c_str(),
                               prefix.size())))
            continue;
        items.Append(winrt::box_value(winrt::hstring(name)));
        if (items.Size() >= 500)
            break;
    }
    suggesting_recent = false;
    send_dest.ItemsSource(items);
    send_dest.IsSuggestionListOpen(items.Size() > 0);
}

void WuiTerm::set_send_state(WuiSendState state, const wchar_t *text,
                             double fraction)
{
    auto show = [](mux::UIElement const &e, bool visible) {
        e.Visibility(visible ? mux::Visibility::Visible :
                     mux::Visibility::Collapsed);
    };
    send_state = state;
    bool busy = (state == WUI_SEND_BUSY ||
                 (state == WUI_SEND_DONE && !bar_receive));

    send_dest.IsEnabled(!busy);
    send_recent_button.IsEnabled(!busy &&
                                 (bar_receive || !send_recent.empty()));
    send_button.IsEnabled(!busy);
    send_button.Content(winrt::box_value(winrt::hstring(
                            state == WUI_SEND_EXISTS ? L"Replace" :
                            bar_receive ? L"Receive" : L"Send")));
    muxc::ToolTipService::SetToolTip(
        send_close, hint_tooltip(state == WUI_SEND_BUSY ? L"Cancel" :
                                 L"Close", L"Esc"));

    show(send_progress, busy);
    if (busy) {
        send_progress.IsIndeterminate(fraction < 0);
        if (fraction >= 0)
            send_progress.Value(fraction * 100);
    }
    show(send_warning, state == WUI_SEND_EXISTS || state == WUI_SEND_ERROR);
    send_status.Text(text ? text : L"");
    show(send_show, state == WUI_SEND_DONE && bar_receive);
    show(send_status_row, busy || (text && *text));
    show(send_help, !send_know_cwd && !busy);

    if (state == WUI_SEND_DONE && !bar_receive)
        send_timer.Start();
    else
        send_timer.Stop();
}

/* Set up the bar, for sending or receiving */
void WuiTerm::open_bar(bool receive, const wchar_t *dest, bool know_cwd)
{
    if (!send_bar)
        make_send_bar();
    bar_receive = receive;
    send_know_cwd = know_cwd;
    send_icon.Glyph(receive ? L"\xE896" : L"\xE898");  /* Download, Upload */
    send_dest.PlaceholderText(receive ? L"Remote file" :
                              L"Remote directory");
    muxc::ToolTipService::SetToolTip(
        send_recent_button, winrt::box_value(winrt::hstring(
                                receive ? L"What's there" :
                                L"Recent directories")));
    list_dir.clear();
    list_names.clear();
    list_known = list_asked = list_wanted = false;
    send_dest.IsSuggestionListOpen(false);
    send_dest.Text(dest);
    send_dest_typed = false;
    set_send_state(WUI_SEND_READY, L"", -1);
    send_bar.Visibility(mux::Visibility::Visible);

    /* (and again once the menu this came from has closed) */
    std::weak_ptr<WuiTerm> weak = weak_from_this();
    auto grab = [weak]() {
        if (auto t = weak.lock())
            t->send_dest.Focus(mux::FocusState::Programmatic);
    };
    grab();
    win->window.DispatcherQueue().TryEnqueue(grab);
}

void WuiTerm::show_send(const wchar_t *filename, const wchar_t *dest,
                        const wchar_t *const *recent, size_t nrecent,
                        bool know_cwd)
{
    send_recent.assign(recent, recent + nrecent);
    open_bar(false, dest, know_cwd);
    send_name.Text(filename);
    muxc::ToolTipService::SetToolTip(
        send_name, winrt::box_value(winrt::hstring(filename)));
    send_name.Visibility(mux::Visibility::Visible);
    send_arrow.Visibility(mux::Visibility::Visible);
}

void WuiTerm::show_receive(const wchar_t *path, bool know_cwd)
{
    open_bar(true, path, know_cwd);
    send_name.Visibility(mux::Visibility::Collapsed);
    send_arrow.Visibility(mux::Visibility::Collapsed);
    if (*path)
        complete(true);                /* (what's there) */
}

void WuiTerm::hide_send()
{
    if (!send_bar || send_bar.Visibility() == mux::Visibility::Collapsed)
        return;
    send_timer.Stop();
    send_bar.Visibility(mux::Visibility::Collapsed);
    send_dest.IsSuggestionListOpen(false);
    wui_cb_send_closed(ctx);
    focus();
}

extern "C" void wui_term_show_send(WuiTerm *term, const wchar_t *filename,
                                   const wchar_t *dest,
                                   const wchar_t *const *recent,
                                   size_t nrecent, bool know_cwd)
{
    term->show_send(filename, dest, recent, nrecent, know_cwd);
}

extern "C" void wui_term_show_receive(WuiTerm *term, const wchar_t *path,
                                      bool know_cwd)
{
    term->show_receive(path, know_cwd);
}

extern "C" void wui_term_send_dest(WuiTerm *term, const wchar_t *dest)
{
    if (term->send_dest && !term->send_dest_typed &&
        term->send_dest.Text().empty() &&
        term->send_state == WUI_SEND_READY) {
        term->send_dest.Text(dest);
        if (term->bar_receive)
            term->complete(true);
    }
}

extern "C" void wui_term_send_state(WuiTerm *term, WuiSendState state,
                                    const wchar_t *text, double fraction)
{
    if (term->send_bar)
        term->set_send_state(state, text, fraction);
}

extern "C" void wui_term_listing(WuiTerm *term, const wchar_t *dir,
                                 const wchar_t *const *names, size_t n)
{
    if (!term->send_bar || term->list_dir != dir)
        return;                        /* (not what we want now) */
    term->list_names.assign(names, names + n);
    term->list_known = true;
    if (term->list_wanted &&
        term->send_bar.Visibility() == mux::Visibility::Visible &&
        term->send_state != WUI_SEND_BUSY)
        term->complete(term->list_all);
}

struct WuiRegex {
    std::wregex re;
};

extern "C" WuiRegex *wui_regex_new(const wchar_t *pattern, bool ignore_case)
{
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (ignore_case)
            flags |= std::regex_constants::icase;
        return new WuiRegex{std::wregex(pattern, flags)};
    } catch (...) {
        return nullptr;
    }
}

extern "C" bool wui_regex_find(WuiRegex *re, const wchar_t *text, size_t len,
                               size_t from, size_t *start, size_t *mlen)
{
    std::wcmatch m;
    try {
        /* (not the beginning of the line, if we're partway along it) */
        auto flags = from > 0 ? std::regex_constants::match_prev_avail :
            std::regex_constants::match_default;
        if (!std::regex_search(text + from, text + len, m, re->re, flags))
            return false;
    } catch (...) {
        return false;                  /* (e.g. too complex) */
    }
    *start = from + m.position(0);
    *mlen = m.length(0);
    return true;
}

extern "C" void wui_regex_free(WuiRegex *re)
{
    delete re;
}

/* A menu command, once the menu it was chosen from has closed (as some,
 * such as Rename Tab, put the focus somewhere, or open a ContentDialog:
 * see wui-flyout.h) */
static void menu_command(winrt::weak_ref<muxp::FlyoutBase> const &menu,
                         void *c, unsigned id)
{
    wui_after_flyout(menu, [c, id]() { wui_cb_menu_command(c, id); });
}

/* An item in a submenu, which is an ordinary MenuFlyout, in 'menu' */
static void add_flyout_item(
    std::vector<wf::Collections::IVector<muxc::MenuFlyoutItemBase>> &stack,
    const WuiMenuItem &item, void *c,
    winrt::weak_ref<muxp::FlyoutBase> const &menu)
{
    unsigned id = item.id;
    switch (item.kind) {
      case WUI_MENU_SEPARATOR:
        stack.back().Append(muxc::MenuFlyoutSeparator());
        break;
      case WUI_MENU_SUBMENU: {
        muxc::MenuFlyoutSubItem sub;
        sub.Text(menu_label(item.label));
        sub.IsEnabled(item.enabled);
        if (item.icon)
            sub.Icon(menu_icon(item.icon));
        stack.back().Append(sub);
        stack.push_back(sub.Items());
        break;
      }
      case WUI_MENU_ITEM: {
        muxc::MenuFlyoutItem mi{nullptr};
        if (item.checked) {
            muxc::ToggleMenuFlyoutItem t;
            t.IsChecked(true);
            mi = t;
        } else {
            mi = muxc::MenuFlyoutItem();
        }
        mi.Text(menu_label(item.label));
        mi.IsEnabled(item.enabled);
        if (item.icon)
            mi.Icon(menu_icon(item.icon));
        if (item.accel)
            mi.KeyboardAcceleratorTextOverride(item.accel);
        mi.Click([c, id, menu](auto &&, auto &&) {
            menu_command(menu, c, id);
        });
        stack.back().Append(mi);
        break;
      }
      default:
        break;
    }
}

/*
 * The context menu is laid out like Notepad's: a CommandBarFlyout,
 * with the items marked 'primary' as a row of icon buttons along the
 * top, and the rest in a list below. Submenus of that list are
 * ordinary MenuFlyouts.
 */
void WuiTerm::show_menu(int x, int y)
{
    size_t n;
    const WuiMenuItem *items = wui_cb_get_terminal_menu(ctx, &n);
    muxc::CommandBarFlyout flyout;
    flyout.AlwaysExpanded(true);
    auto primary = flyout.PrimaryCommands();
    auto secondary = flyout.SecondaryCommands();
    /* The submenus we're inside, if any */
    std::vector<wf::Collections::IVector<muxc::MenuFlyoutItemBase>> stack;
    void *c = ctx;
    winrt::weak_ref<muxp::FlyoutBase> menu = winrt::make_weak(
        flyout.as<muxp::FlyoutBase>());

    for (size_t i = 0; i < n; i++) {
        const WuiMenuItem &item = items[i];
        if (item.kind == WUI_MENU_END) {
            if (!stack.empty())
                stack.pop_back();
            continue;
        }
        if (!stack.empty()) {
            add_flyout_item(stack, item, c, menu);
            continue;
        }

        unsigned id = item.id;
        winrt::hstring label = menu_label(item.label);
        switch (item.kind) {
          case WUI_MENU_SEPARATOR:
            /* (None at the top of the list, where the separator
             * after the primary items would otherwise end up) */
            if (secondary.Size() > 0)
                secondary.Append(muxc::AppBarSeparator());
            break;
          case WUI_MENU_SUBMENU: {
            muxc::AppBarButton b;
            b.Label(label);
            b.IsEnabled(item.enabled);
            if (item.icon)
                b.Icon(menu_icon(item.icon));
            muxc::MenuFlyout sub;
            sub.Placement(muxp::FlyoutPlacementMode::RightEdgeAlignedTop);
            b.Flyout(sub);
            secondary.Append(b);
            stack.push_back(sub.Items());
            break;
          }
          case WUI_MENU_ITEM: {
            muxc::AppBarButton b{nullptr};
            muxc::AppBarToggleButton t{nullptr};
            muxc::ICommandBarElement elt{nullptr};
            muxp::ButtonBase button{nullptr};
            if (item.checked) {
                t = muxc::AppBarToggleButton();
                t.Label(label);
                t.IsChecked(true);
                if (item.icon)
                    t.Icon(menu_icon(item.icon));
                elt = t;
                button = t;
            } else {
                b = muxc::AppBarButton();
                b.Label(label);
                if (item.icon)
                    b.Icon(menu_icon(item.icon));
                elt = b;
                button = b;
            }
            button.IsEnabled(item.enabled);
            if (item.accel) {
                if (b)
                    b.KeyboardAcceleratorTextOverride(item.accel);
                else
                    t.KeyboardAcceleratorTextOverride(item.accel);
            }
            button.Click([c, id, menu](auto &&, auto &&) {
                menu_command(menu, c, id);
            });
            if (item.primary && item.icon) {
                /* Icon only, so say what it is when hovered over */
                muxc::ToolTipService::SetToolTip(button,
                                                 winrt::box_value(label));
                /* with a vertical bar between each one and the next */
                if (primary.Size() > 0)
                    primary.Append(muxc::AppBarSeparator());
                primary.Append(elt);
            } else {
                secondary.Append(elt);
            }
            break;
          }
          default:
            break;
        }
    }

    /*
     * The whole menu goes when anything in it is chosen, submenus' items
     * as well. (CommandBarFlyout does that itself only for some of its
     * commands, and not for what's in a submenu, which, as of the
     * submenus Copy to Pool brought, left it open after Copy or Paste.)
     */
    auto weak = winrt::make_weak(flyout);
    auto hide = [weak](auto &&, auto &&) {
        if (auto f = weak.get())
            f.Hide();
    };
    std::function<void(wf::Collections::IVector<muxc::MenuFlyoutItemBase>)>
        hide_in = [&](wf::Collections::IVector<muxc::MenuFlyoutItemBase>
                      items) {
            for (auto const &it : items) {
                if (auto sub = it.try_as<muxc::MenuFlyoutSubItem>())
                    hide_in(sub.Items());
                else if (auto mi = it.try_as<muxc::MenuFlyoutItem>())
                    mi.Click(hide);
            }
        };
    for (auto cmds : { primary, secondary })
        for (auto const &cmd : cmds) {
            auto b = cmd.try_as<muxc::AppBarButton>();
            if (b && b.Flyout()) {
                if (auto mf = b.Flyout().try_as<muxc::MenuFlyout>())
                    hide_in(mf.Items());
            } else if (auto bb = cmd.try_as<muxp::ButtonBase>()) {
                bb.Click(hide);
            }
        }

    muxp::FlyoutShowOptions opts;
    opts.Position(wf::Point((float)(x / win->scale),
                            (float)(y / win->scale)));
    /* Take the keyboard focus, as a context menu does */
    opts.ShowMode(muxp::FlyoutShowMode::Standard);
    flyout.ShowAt(term, opts);
}

/* The window icon's menu */
void WuiHost::show_window_menu(bool at_pointer)
{
    muxc::MenuFlyout flyout;
    flyout.Placement(muxp::FlyoutPlacementMode::BottomEdgeAlignedLeft);
    auto top = flyout.Items();

    WuiWindowState state = get_state();
    bool max = (state == WUI_STATE_MAXIMISED ||
                state == WUI_STATE_FULLSCREEN);
    bool min = (state == WUI_STATE_MINIMISED);
    HWND h = hwnd;
    auto sys = [&](UINT cmd, const wchar_t *label, unsigned glyph,
                   bool enabled) {
        muxc::MenuFlyoutItem mi;
        mi.Text(label);
        if (glyph)
            mi.Icon(menu_icon(glyph));
        mi.IsEnabled(enabled);
        /* (Posted, as Windows' own menu would: Move and Size go into
         * a modal loop) */
        mi.Click([h, cmd](auto &&, auto &&) {
            PostMessageW(h, WM_SYSCOMMAND, cmd, 0);
        });
        top.Append(mi);
    };
    sys(SC_RESTORE, L"Restore", 0xE923, max || min);       /* ChromeRestore */
    sys(SC_MOVE, L"Move", 0xE7C2, !max);                   /* Move */
    sys(SC_SIZE, L"Size", 0xE740,                          /* FullScreen */
        !max && overlapped.IsResizable());
    sys(SC_MINIMIZE, L"Minimize", 0xE921,                  /* ChromeMinimize */
        !min && overlapped.IsMinimizable());
    sys(SC_MAXIMIZE, L"Maximize", 0xE922,                  /* ChromeMaximize */
        !max && overlapped.IsMaximizable());
    sys(SC_CLOSE, L"Close", 0xE8BB, true);                 /* ChromeClose */

    /* Then ours (Full Screen) */
    if (active) {
        size_t n;
        void *ctx = active->ctx;
        const WuiMenuItem *items = wui_cb_get_window_menu(ctx, &n);
        top.Append(muxc::MenuFlyoutSeparator());
        std::vector<wf::Collections::IVector<muxc::MenuFlyoutItemBase>> stack;
        stack.push_back(top);
        auto menu = winrt::make_weak(flyout.as<muxp::FlyoutBase>());
        for (size_t i = 0; i < n; i++)
            if (items[i].kind != WUI_MENU_END)
                add_flyout_item(stack, items[i], ctx, menu);
    }

    if (at_pointer) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        muxp::FlyoutShowOptions opts;
        opts.Position(wf::Point{(float)(pt.x / scale),
                                (float)(pt.y / scale)});
        opts.ShowMode(muxp::FlyoutShowMode::Standard);
        flyout.Placement(muxp::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        flyout.ShowAt(layout, opts);
    } else {
        flyout.ShowAt(icon_box);
    }
}

extern "C" void wui_show_window_menu(WuiHost *host, bool at_pointer)
{
    /* (not from inside whatever window message this is for) */
    host->window.DispatcherQueue().TryEnqueue([host, at_pointer]() {
        try {
            host->show_window_menu(at_pointer);
        } catch (...) {
        }
    });
}

/*
 * The saved sessions button's pop-up: a box to filter them by, and
 * them as on the configuration box's Home page, the favorite ones, the
 * most recently used, and all of them, in their folders (which open
 * and close there too). Clicking one opens it in a new tab (or with
 * Shift, a new window), as Return in the box does the first.
 */
namespace {
struct SessionsPopup {
    struct Entry {
        std::wstring name, detail;
        bool favorite;
        long long last_used;
        unsigned id;
    };
    std::vector<Entry> sessions;
    void *ctx = nullptr;
    unsigned first = 0;            /* the first one showing's command */
    std::wstring quick;            /* a host name typed in, to connect
                                    * to if none of them match */
    muxc::Flyout flyout{nullptr};
    muxc::TextBox filter{nullptr};
    muxc::StackPanel body{nullptr};
};
}

static void sessions_popup_fill(std::shared_ptr<SessionsPopup> const &sp);

/* After the rows have been made again: stay on the one that was
 * clicked, in its new form (by its tag: "F" and a folder's path, or
 * "H" and a heading's section) */
static void popup_focus(SessionsPopup &p, std::wstring const &tag)
{
    for (auto c : p.body.Children()) {
        auto t = c.as<mux::FrameworkElement>().Tag();
        if (t && winrt::unbox_value_or<winrt::hstring>(t, L"") == tag) {
            c.as<muxc::Control>().Focus(mux::FocusState::Keyboard);
            return;
        }
    }
}

static muxc::TextBlock popup_text(std::wstring const &text,
                                  const wchar_t *style, bool secondary)
{
    muxc::TextBlock t;
    try {
        t.Style(mux::Application::Current().Resources().Lookup(
                    winrt::box_value(style)).as<mux::Style>());
        if (secondary)
            t.Foreground(mux::Application::Current().Resources().Lookup(
                             winrt::box_value(L"TextFillColorSecondaryBrush"))
                         .as<mux::Media::Brush>());
    } catch (...) {
    }
    t.Text(winrt::hstring(text));
    t.TextWrapping(mux::TextWrapping::NoWrap);
    t.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    return t;
}

/* A row: a button the width of the pop-up, with an icon, a title (and
 * perhaps something under it), and perhaps a detail on the right */
static muxc::Button popup_row(int indent, unsigned chevron, unsigned glyph,
                              std::wstring const &title,
                              std::wstring const &subtitle,
                              std::wstring const &detail)
{
    muxc::Button b;
    b.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    b.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    b.Background(mux::Media::SolidColorBrush(mui::Colors::Transparent()));
    b.BorderThickness(mux::ThicknessHelper::FromUniformLength(0));
    b.Padding(mux::ThicknessHelper::FromLengths(10 + 20 * indent, 4, 10, 4));

    muxc::Grid g;
    g.ColumnSpacing(10);
    for (int i = 0; i < 4; i++) {
        muxc::ColumnDefinition c;
        c.Width(i == 2 ? mux::GridLengthHelper::FromValueAndType(
                    1, mux::GridUnitType::Star) :
                mux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c);
    }
    auto place = [&](mux::FrameworkElement e, int col) {
        e.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(e, col);
        g.Children().Append(e);
    };
    if (chevron) {
        auto c = menu_icon(chevron).as<muxc::FontIcon>();
        c.FontSize(10);
        place(c, 0);
    }
    if (glyph) {
        auto icon = menu_icon(glyph).as<muxc::FontIcon>();
        icon.FontSize(16);
        place(icon, 1);
    }
    muxc::StackPanel text;
    /* (a heading, with no icon, in bold) */
    text.Children().Append(popup_text(title, glyph ? L"BodyTextBlockStyle" :
                                      L"BodyStrongTextBlockStyle", false));
    if (!subtitle.empty())
        text.Children().Append(popup_text(subtitle, L"CaptionTextBlockStyle",
                                          true));
    place(text, 2);
    if (!detail.empty())
        place(popup_text(detail, L"CaptionTextBlockStyle", true), 3);
    b.Content(g);
    return b;
}

/* A section's heading, which closes or opens it (and says how many
 * are in it while it's closed). Returns whether it's open. */
static bool popup_heading(std::shared_ptr<SessionsPopup> const &sp,
                          const wchar_t *title, unsigned section,
                          size_t count)
{
    bool open = !(wui_cb_sessions_hidden() & section);
    auto b = popup_row(0, open ? 0xE70D : 0xE76C,  /* ChevronDown, Right */
                       0, title, L"", open ? L"" : std::to_wstring(count));
    b.Margin(mux::ThicknessHelper::FromLengths(
                 0, sp->body.Children().Size() ? 8 : 0, 0, 0));
    std::wstring tag = L"H" + std::to_wstring(section);
    b.Tag(winrt::box_value(winrt::hstring(tag)));
    std::weak_ptr<SessionsPopup> weak = sp;
    b.Click([weak, section, tag](auto &&, auto &&) {
        auto p = weak.lock();
        if (!p || !p->body)
            return;
        wui_cb_sessions_set_hidden(wui_cb_sessions_hidden() ^ section);
        sessions_popup_fill(p);
        popup_focus(*p, tag);
    });
    sp->body.Children().Append(b);
    return open;
}

/* What separates the folders in a session's name, as PuTTY-ng has it
 * (and winui-sessions.h too); and the name as shown, with '/' there,
 * which reads better */
static const wchar_t POPUP_SEP = L'#';

static std::wstring shown_path(std::wstring s)
{
    std::replace(s.begin(), s.end(), POPUP_SEP, L'/');
    return s;
}

/* How popup_session shows a session's name: as it is (for what the
 * filter finds), or just the part after its folder (in the tree, under
 * the folder; or under Favorites or Recent, with the folder in the
 * tooltip, as on the configuration box's Home page) */
enum { POPUP_NAME_FULL, POPUP_NAME_IN_TREE, POPUP_NAME_IN_SECTION };

static void popup_session(std::shared_ptr<SessionsPopup> const &sp,
                          SessionsPopup::Entry const &e, int how,
                          int indent)
{
    std::wstring shown = e.name, folder;
    auto slash = shown.rfind(POPUP_SEP);
    if (how != POPUP_NAME_FULL && slash != std::wstring::npos) {
        folder = shown_path(shown.substr(0, slash));
        shown = shown.substr(slash + 1);
    } else {
        shown = shown_path(shown);
    }
    auto b = popup_row(indent, 0, e.favorite ? 0xE735 : 0xE756,
                       shown, L"", L"");  /* FavoriteStarFill,
                                           * CommandPrompt */
    /* (what it connects to, just when it's hovered over, to keep the
     * list short; and under Favorites or Recent, the folder it's in, in
     * italics) */
    if (how == POPUP_NAME_IN_SECTION) {
        namespace muxd = winrt::Microsoft::UI::Xaml::Documents;
        muxc::TextBlock tip;
        if (!e.detail.empty()) {
            muxd::Run r;
            r.Text(e.detail);
            tip.Inlines().Append(r);
            tip.Inlines().Append(muxd::LineBreak());
        }
        muxd::Run f;
        f.Text(folder.empty() ? L"(Top level)" : folder);
        f.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
        tip.Inlines().Append(f);
        muxc::ToolTipService::SetToolTip(b, tip);
    } else if (!e.detail.empty()) {
        muxc::ToolTipService::SetToolTip(
            b, winrt::box_value(winrt::hstring(e.detail)));
    }
    std::weak_ptr<SessionsPopup> weak = sp;
    unsigned id = e.id;
    b.Click([weak, id](auto &&, auto &&) {
        auto p = weak.lock();
        if (!p || !p->flyout)
            return;
        void *ctx = p->ctx;
        p->flyout.Hide();
        /* With Shift held down, in a new window instead of a tab */
        wui_cb_menu_command(ctx, GetKeyState(VK_SHIFT) < 0 ?
                            id | WUI_MENU_NEW_WINDOW : id);
    });
    if (!sp->first)
        sp->first = id;
    sp->body.Children().Append(b);
}

/* Connecting to what's been typed in, as a host name */
static void popup_quick(SessionsPopup &p)
{
    std::wstring target = p.quick;
    void *ctx = p.ctx;
    p.flyout.Hide();
    wui_cb_quick_connect(ctx, target.c_str(), GetKeyState(VK_SHIFT) < 0);
}

static void popup_quick_row(std::shared_ptr<SessionsPopup> const &sp)
{
    auto b = popup_row(0, 0, 0xE703,                     /* Connect */
                       L"Connect to " + sp->quick, L"", L"");
    muxc::ToolTipService::SetToolTip(
        b, winrt::box_value(winrt::hstring(
               L"As a new session, by SSH, unless it starts with "
               L"telnet://, rlogin://, raw://, supdup:// or serial://")));
    std::weak_ptr<SessionsPopup> weak = sp;
    b.Click([weak](auto &&, auto &&) {
        if (auto p = weak.lock(); p && p->flyout)
            popup_quick(*p);
    });
    sp->body.Children().Append(b);
}

static bool starts_with(std::wstring const &s, std::wstring const &prefix)
{
    return s.size() >= prefix.size() &&
        !s.compare(0, prefix.size(), prefix);
}

/* The sessions in a folder (or "", for all of them), as a tree */
static void popup_tree(std::shared_ptr<SessionsPopup> const &sp,
                       std::wstring const &prefix, int indent)
{
    std::vector<std::wstring> folders;
    for (auto &e : sp->sessions) {
        if (!starts_with(e.name, prefix))
            continue;
        auto slash = e.name.find(POPUP_SEP, prefix.size());
        if (slash == std::wstring::npos || slash == prefix.size())
            continue;
        auto f = e.name.substr(0, slash);
        if (std::find(folders.begin(), folders.end(), f) == folders.end())
            folders.push_back(f);
    }
    std::sort(folders.begin(), folders.end(),
              [](std::wstring const &a, std::wstring const &b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });

    for (auto &f : folders) {
        std::wstring path = f + POPUP_SEP;
        size_t count = 0;
        for (auto &e : sp->sessions)
            count += starts_with(e.name, path);
        bool open = wui_cb_session_folder_open(f.c_str());
        auto b = popup_row(indent, open ? 0xE70D : 0xE76C,  /* ChevronDown,
                                                             * Right */
                           open ? 0xE838 : 0xE8B7,   /* FolderOpen, Folder */
                           f.substr(prefix.size()), L"",
                           std::to_wstring(count));
        b.Tag(winrt::box_value(winrt::hstring(L"F" + f)));
        std::weak_ptr<SessionsPopup> weak = sp;
        b.Click([weak, f](auto &&, auto &&) {
            auto p = weak.lock();
            if (!p || !p->body)
                return;
            wui_cb_session_folder_toggle(f.c_str());
            sessions_popup_fill(p);
            popup_focus(*p, L"F" + f);
        });
        sp->body.Children().Append(b);
        if (open)
            popup_tree(sp, path, indent + 1);
    }

    for (auto &e : sp->sessions)
        if (starts_with(e.name, prefix) &&
            (e.name.find(POPUP_SEP, prefix.size()) == std::wstring::npos ||
             e.name[prefix.size()] == POPUP_SEP))
            popup_session(sp, e, POPUP_NAME_IN_TREE, indent);
}

static bool contains_ci(std::wstring const &hay, std::wstring const &needle)
{
    auto it = std::search(hay.begin(), hay.end(), needle.begin(),
                          needle.end(), [](wchar_t a, wchar_t b) {
                              return towlower(a) == towlower(b);
                          });
    return it != hay.end();
}

/*
 * The pools' pop-up: a box to filter them by, and them, under a heading
 * for each pool. Clicking one pastes it (as Return in the box does the
 * first); right-clicking it, the rest.
 */
namespace {
struct PoolPopup {
    struct Entry {
        int pool;
        std::wstring text;
    };
    std::vector<Entry> items;
    bool session = false;
    void *ctx = nullptr;
    int first = -1;                /* the first one showing */
    muxc::Flyout flyout{nullptr};
    muxc::TextBox filter{nullptr};
    muxc::StackPanel body{nullptr};
};
}

/* Do something with one, once the pop-up's gone */
static void pool_do(PoolPopup &p, int index, int action)
{
    void *ctx = p.ctx;
    if (p.flyout)
        p.flyout.Hide();
    wui_cb_pool_action(ctx, index, action);
}

static void pool_fill(std::shared_ptr<PoolPopup> const &sp)
{
    PoolPopup &p = *sp;
    p.body.Children().Clear();
    p.first = -1;
    std::wstring q(p.filter.Text());
    static const wchar_t *const headings[] = {
        L"This tab", L"This session", L"All sessions"
    };
    bool any = false;
    for (int pool = WUI_POOL_TAB; pool <= WUI_POOL_COMMON; pool++) {
        bool heading = false;
        for (size_t i = 0; i < p.items.size(); i++) {
            auto const &e = p.items[i];
            if (e.pool != pool || (!q.empty() && !contains_ci(e.text, q)))
                continue;
            if (!heading) {
                auto h = popup_text(headings[pool], L"CaptionTextBlockStyle",
                                    true);
                h.Margin(mux::ThicknessHelper::FromLengths(
                             10, any ? 8 : 2, 10, 2));
                p.body.Children().Append(h);
                heading = true;
            }
            any = true;
            if (p.first < 0)
                p.first = (int)i;
            /* (its first line, the whole of it, or most, in the
             * tooltip) */
            std::wstring line = e.text.substr(0, e.text.find_first_of(
                                                  L"\r\n"));
            if (line.size() < e.text.size())
                line += L" \u2026";
            auto b = popup_row(0, 0, 0, line, L"", L"");
            std::wstring tip = e.text.size() > 600 ?
                e.text.substr(0, 600) + L"\u2026" : e.text;
            muxc::ToolTipService::SetToolTip(
                b, winrt::box_value(winrt::hstring(tip)));
            std::weak_ptr<PoolPopup> weak = sp;
            int index = (int)i;
            b.Click([weak, index](auto &&, auto &&) {
                if (auto p = weak.lock(); p && p->flyout)
                    pool_do(*p, index, WUI_POOL_PASTE);
            });
            muxc::MenuFlyout menu;
            auto add = [&](const wchar_t *label, unsigned glyph, int action) {
                muxc::MenuFlyoutItem mi;
                mi.Text(label);
                mi.Icon(menu_icon(glyph));
                mi.Click([weak, index, action](auto &&, auto &&) {
                    if (auto p = weak.lock(); p && p->flyout)
                        pool_do(*p, index, action);
                });
                menu.Items().Append(mi);
            };
            add(L"Paste", 0xE77F, WUI_POOL_PASTE);              /* Paste */
            if (pool != WUI_POOL_SESSION && p.session)
                add(L"Keep for this session", 0xE74E,          /* Save */
                    WUI_POOL_KEEP_SESSION);
            if (pool != WUI_POOL_COMMON)
                add(L"Keep for all sessions", 0xE74E,          /* Save */
                    WUI_POOL_KEEP_COMMON);
            add(L"Remove", 0xE74D, WUI_POOL_REMOVE);           /* Delete */
            b.ContextFlyout(menu);
            p.body.Children().Append(b);
        }
    }
    if (!any) {
        auto t = popup_text(p.items.empty() ?
                            L"Nothing yet: what's copied in this tab goes "
                            L"here, and Copy to Pool keeps things for this "
                            L"session, or all of them" :
                            L"Nothing matches", L"BodyTextBlockStyle", true);
        t.TextWrapping(mux::TextWrapping::Wrap);
        t.Margin(mux::ThicknessHelper::FromLengths(10, 8, 10, 8));
        p.body.Children().Append(t);
    }
    bool tab = std::any_of(p.items.begin(), p.items.end(), [](auto &e) {
        return e.pool == WUI_POOL_TAB;
    });
    if (tab && q.empty()) {
        muxc::HyperlinkButton clear;
        clear.Content(winrt::box_value(
                          winrt::hstring(L"Clear this tab's history")));
        clear.Margin(mux::ThicknessHelper::FromLengths(4, 6, 4, 2));
        std::weak_ptr<PoolPopup> weak = sp;
        clear.Click([weak](auto &&, auto &&) {
            if (auto p = weak.lock(); p && p->flyout)
                pool_do(*p, -1, WUI_POOL_CLEAR_TAB);
        });
        p.body.Children().Append(clear);
    }
}

extern "C" void wui_term_show_pool(WuiTerm *term, int x, int y,
                                   const WuiPoolItem *items, size_t n,
                                   bool session)
{
    auto sp = std::make_shared<PoolPopup>();
    sp->ctx = term->ctx;
    sp->session = session;
    for (size_t i = 0; i < n; i++)
        sp->items.push_back({items[i].pool, items[i].text});

    sp->filter = muxc::TextBox();
    sp->filter.PlaceholderText(L"Filter");
    sp->filter.Margin(mux::ThicknessHelper::FromLengths(4, 4, 4, 4));
    sp->body = muxc::StackPanel();
    sp->body.XYFocusKeyboardNavigation(
        muxi::XYFocusKeyboardNavigationMode::Enabled);
    muxc::ScrollViewer scroll;
    scroll.MaxHeight(360);
    scroll.Content(sp->body);
    muxc::StackPanel root;
    root.Width(380);
    root.Children().Append(sp->filter);
    root.Children().Append(scroll);
    pool_fill(sp);

    std::weak_ptr<PoolPopup> weak = sp;
    sp->filter.TextChanged([weak](auto &&, auto &&) {
        if (auto p = weak.lock(); p && p->body)
            pool_fill(p);
    });
    /* (Return pastes the first that's showing; Down goes to them) */
    sp->filter.KeyDown([weak](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        auto p = weak.lock();
        if (!p || !p->flyout)
            return;
        if (e.Key() == VirtualKey::Enter && p->first >= 0) {
            e.Handled(true);
            pool_do(*p, p->first, WUI_POOL_PASTE);
        } else if (e.Key() == VirtualKey::Down) {
            for (auto c : p->body.Children())
                if (auto b = c.try_as<muxc::Button>()) {
                    e.Handled(true);
                    b.Focus(mux::FocusState::Keyboard);
                    break;
                }
        }
    });

    sp->flyout = muxc::Flyout();
    sp->flyout.ShouldConstrainToRootBounds(false);
    {
        mux::Style style(winrt::xaml_typename<muxc::FlyoutPresenter>());
        style.Setters().Append(mux::Setter(
            muxc::Control::PaddingProperty(),
            winrt::box_value(mux::ThicknessHelper::FromUniformLength(4))));
        style.Setters().Append(mux::Setter(
            mux::FrameworkElement::MaxWidthProperty(),
            winrt::box_value(480.0)));
        style.Setters().Append(mux::Setter(
            muxc::Control::CornerRadiusProperty(),
            winrt::box_value(mux::CornerRadiusHelper::FromUniformRadius(8))));
        sp->flyout.FlyoutPresenterStyle(style);
    }
    sp->flyout.Content(root);
    sp->flyout.Opened([weak](auto &&, auto &&) {
        if (auto p = weak.lock(); p && p->filter)
            p->filter.Focus(mux::FocusState::Programmatic);
    });
    /* (and nothing of it kept once it's gone, as its handlers refer to
     * it, and so to themselves) */
    sp->flyout.Closed([sp](auto &&, auto &&) {
        sp->flyout = nullptr;
        sp->filter = nullptr;
        sp->body = nullptr;
    });
    muxp::FlyoutShowOptions opts;
    opts.Position(wf::Point((float)(x / term->win->scale),
                            (float)(y / term->win->scale)));
    opts.ShowMode(muxp::FlyoutShowMode::Standard);
    sp->flyout.ShowAt(term->term, opts);
}


static void sessions_popup_fill(std::shared_ptr<SessionsPopup> const &sp)
{
    auto &p = *sp;
    p.body.Children().Clear();
    p.first = 0;
    p.quick.clear();
    std::wstring q(p.filter.Text());
    q.erase(0, q.find_first_not_of(L" \t"));
    q.erase(q.find_last_not_of(L" \t") + 1);

    if (!q.empty()) {
        /* (and if it could be a host name, connecting to it: first,
         * if nothing matches, which is what Return does then) */
        if (q.find_first_of(L" \t") == std::wstring::npos)
            p.quick = q;
        /* What matches, the ones whose names start with it first */
        for (int pass = 0; pass < 2; pass++)
            for (auto &e : p.sessions) {
                /* (with the names as shown) */
                std::wstring name = shown_path(e.name);
                bool starts = name.size() >= q.size() &&
                    !_wcsnicmp(name.c_str(), q.c_str(), q.size());
                if (pass == 0 ? starts :
                    !starts && (contains_ci(name, q) ||
                                contains_ci(e.detail, q)))
                    popup_session(sp, e, POPUP_NAME_FULL, 0);
            }
        if (!p.quick.empty())
            popup_quick_row(sp);
        if (!p.first && p.quick.empty()) {
            auto t = popup_text(L"No saved session matches",
                                L"BodyTextBlockStyle", true);
            t.Margin(mux::ThicknessHelper::FromLengths(10, 8, 10, 8));
            p.body.Children().Append(t);
        }
        return;
    }
    if (p.sessions.empty()) {
        auto t = popup_text(L"There are no saved sessions yet",
                            L"BodyTextBlockStyle", true);
        t.Margin(mux::ThicknessHelper::FromLengths(10, 8, 10, 8));
        p.body.Children().Append(t);
        return;
    }

    /* (leaving out what its right-click menu says to) */
    unsigned omitted = wui_cb_sessions_omitted();
    bool favorites = !(omitted & WUI_SECTION_FAVORITES);
    bool recent = !(omitted & WUI_SECTION_RECENT);
    bool all = !(omitted & WUI_SECTION_ALL);
    bool headings = false;
    std::vector<SessionsPopup::Entry const *> list;
    for (auto &e : p.sessions)
        if (e.favorite && favorites)
            list.push_back(&e);
    if (!list.empty()) {
        if (popup_heading(sp, L"Favorites", WUI_SECTION_FAVORITES,
                          list.size()))
            for (auto e : list)
                popup_session(sp, *e, POPUP_NAME_IN_SECTION, 0);
        headings = true;
    }
    list.clear();
    /* (not the favorite ones again, if they're there already) */
    for (auto &e : p.sessions)
        if (recent && e.last_used && !(e.favorite && favorites))
            list.push_back(&e);
    std::stable_sort(list.begin(), list.end(), [](auto a, auto b) {
        return a->last_used > b->last_used;
    });
    if (list.size() > 5)
        list.resize(5);
    if (!list.empty()) {
        if (popup_heading(sp, L"Recent", WUI_SECTION_RECENT, list.size()))
            for (auto e : list)
                popup_session(sp, *e, POPUP_NAME_IN_SECTION, 0);
        headings = true;
    }
    /* (and if there's nothing else, just the tree, with no heading to
     * close it with) */
    if (all && (!headings || popup_heading(sp, L"All sessions",
                                           WUI_SECTION_ALL,
                                           p.sessions.size())))
        popup_tree(sp, L"", 0);
    if (!all && !headings) {
        auto t = popup_text(L"Nothing to show: right-click to choose",
                            L"BodyTextBlockStyle", true);
        t.Margin(mux::ThicknessHelper::FromLengths(10, 8, 10, 8));
        p.body.Children().Append(t);
    }
}

/* The menu on right-clicking the pop-up: which sections to show */
static muxc::MenuFlyout popup_settings(
    std::shared_ptr<SessionsPopup> const &sp)
{
    static const struct {
        const wchar_t *label;
        unsigned section;
    } sections[] = {
        { L"Show Favorites", WUI_SECTION_FAVORITES },
        { L"Show Recent", WUI_SECTION_RECENT },
        { L"Show All sessions", WUI_SECTION_ALL },
    };
    unsigned omitted = wui_cb_sessions_omitted();
    std::weak_ptr<SessionsPopup> weak = sp;
    muxc::MenuFlyout menu;
    for (auto &sec : sections) {
        muxc::ToggleMenuFlyoutItem mi;
        mi.Text(sec.label);
        mi.IsChecked(!(omitted & sec.section));
        unsigned section = sec.section;
        mi.Click([weak, section](wf::IInspectable const &sender, auto &&) {
            bool on = sender.as<muxc::ToggleMenuFlyoutItem>().IsChecked();
            unsigned o = wui_cb_sessions_omitted();
            wui_cb_sessions_set_omitted(on ? o & ~section : o | section);
            if (auto p = weak.lock(); p && p->body)
                sessions_popup_fill(p);
        });
        menu.Items().Append(mi);
    }
    return menu;
}

void WuiHost::show_sessions_menu()
{
    if (!active)
        return;
    auto sp = std::make_shared<SessionsPopup>();
    sp->ctx = active->ctx;
    size_t n;
    const WuiSession *s = wui_cb_get_sessions(&n);
    for (size_t i = 0; i < n; i++)
        sp->sessions.push_back({s[i].name ? s[i].name : L"",
                                s[i].detail ? s[i].detail : L"",
                                s[i].favorite, s[i].last_used, s[i].id});

    sp->filter = muxc::TextBox();
    sp->filter.PlaceholderText(L"Filter sessions, or type a host name "
                               L"to connect to");
    sp->filter.Margin(mux::ThicknessHelper::FromLengths(4, 4, 4, 4));
    sp->body = muxc::StackPanel();
    sp->body.XYFocusKeyboardNavigation(
        muxi::XYFocusKeyboardNavigationMode::Enabled);
    muxc::ScrollViewer scroll;
    scroll.MaxHeight(480);
    scroll.Content(sp->body);
    muxc::StackPanel root;
    root.Width(360);
    root.Children().Append(sp->filter);
    /* (right-clicking anywhere in it but the box says what to show; with
     * a background, so that that's anywhere) */
    root.Background(mux::Media::SolidColorBrush(mui::Colors::Transparent()));
    root.ContextFlyout(popup_settings(sp));
    root.Children().Append(scroll);

    std::weak_ptr<SessionsPopup> weak = sp;
    sp->filter.TextChanged([weak](auto &&, auto &&) {
        if (auto p = weak.lock(); p && p->body)
            sessions_popup_fill(p);
    });
    sp->filter.KeyDown([weak](auto &&, muxi::KeyRoutedEventArgs const &e) {
        using winrt::Windows::System::VirtualKey;
        auto p = weak.lock();
        if (!p || !p->body)
            return;
        if (e.Key() == VirtualKey::Enter && p->first) {
            /* (Return opens the first one showing, or if there are
             * none, connects to what's been typed) */
            e.Handled(true);
            unsigned id = p->first;
            void *ctx = p->ctx;
            p->flyout.Hide();
            wui_cb_menu_command(ctx, GetKeyState(VK_SHIFT) < 0 ?
                                id | WUI_MENU_NEW_WINDOW : id);
        } else if (e.Key() == VirtualKey::Enter && !p->quick.empty()) {
            e.Handled(true);
            popup_quick(*p);
        } else if (e.Key() == VirtualKey::Down) {
            /* (and Down goes into the list) */
            for (auto c : p->body.Children()) {
                if (auto b = c.try_as<muxc::Button>()) {
                    e.Handled(true);
                    b.Focus(mux::FocusState::Keyboard);
                    break;
                }
            }
        }
    });

    sp->flyout = muxc::Flyout();
    /* (allowed out of the window, if it's bigger than that: WinUI puts
     * it in a window of its own then) */
    sp->flyout.ShouldConstrainToRootBounds(false);
    mux::Style style(winrt::xaml_typename<muxc::FlyoutPresenter>());
    style.Setters().Append(mux::Setter(
        muxc::Control::PaddingProperty(),
        winrt::box_value(mux::ThicknessHelper::FromUniformLength(4))));
    style.Setters().Append(mux::Setter(
        muxc::Control::CornerRadiusProperty(),
        winrt::box_value(mux::CornerRadiusHelper::FromUniformRadius(8))));
    style.Setters().Append(mux::Setter(
        mux::FrameworkElement::MaxWidthProperty(),
        winrt::box_value(480.0)));
    sp->flyout.FlyoutPresenterStyle(style);
    sp->flyout.Placement(muxp::FlyoutPlacementMode::BottomEdgeAlignedLeft);
    sp->flyout.Content(root);
    sp->flyout.Opened([weak](auto &&, auto &&) {
        if (auto p = weak.lock(); p && p->filter)
            p->filter.Focus(mux::FocusState::Programmatic);
    });
    /* (the pop-up keeps this until it's closed, and then this lets go
     * of the pop-up's controls, which have handlers that point back at
     * it) */
    sp->flyout.Closed([sp](auto &&, auto &&) {
        sp->filter = nullptr;
        sp->body = nullptr;
        sp->flyout = nullptr;
    });
    sessions_popup_fill(sp);
    sp->flyout.ShowAt(menu_button);
}

/* The tab's own menu, which is just a list */
void WuiTerm::show_tab_menu(muxi::ContextRequestedEventArgs const &e)
{
    size_t n;
    const WuiMenuItem *items = wui_cb_get_tab_menu(ctx, &n);
    muxc::MenuFlyout flyout;
    std::vector<wf::Collections::IVector<muxc::MenuFlyoutItemBase>> stack;
    stack.push_back(flyout.Items());
    auto menu = winrt::make_weak(flyout.as<muxp::FlyoutBase>());
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == WUI_MENU_END) {
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        add_flyout_item(stack, items[i], ctx, menu);
    }

    wf::Point pt;
    if (e.TryGetPosition(item, pt)) {
        muxp::FlyoutShowOptions opts;
        opts.Position(pt);
        flyout.ShowAt(item, opts);
    } else {
        flyout.ShowAt(item);
    }
    e.Handled(true);
}

/* ----------------------------------------------------------------------
 * The application object.
 */

struct App : mux::ApplicationT<App, mux::Markup::IXamlMetadataProvider> {
    void OnLaunched(mux::LaunchActivatedEventArgs const &)
    {
        Resources().MergedDictionaries().Append(
            muxc::XamlControlsResources());
        /* We say when to stop (wui_exit, once the main window has
         * closed). Otherwise the config box at startup, which comes
         * and goes before the main window has been shown, would count
         * as the last window closing. */
        try {
            DispatcherShutdownMode(mux::DispatcherShutdownMode::
                                   OnExplicitShutdown);
        } catch (...) {
        }
        host = new WuiHost();
        host->create();
        wui_cb_launched(host);
    }

    mux::Markup::IXamlType GetXamlType(
        winrt::Windows::UI::Xaml::Interop::TypeName const &type)
    {
        return provider.GetXamlType(type);
    }

    mux::Markup::IXamlType GetXamlType(winrt::hstring const &fullName)
    {
        return provider.GetXamlType(fullName);
    }

    winrt::com_array<mux::Markup::XmlnsDefinition> GetXmlnsDefinitions()
    {
        return provider.GetXmlnsDefinitions();
    }

  private:
    WuiHost *host = nullptr;
    mux::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
};

/* ----------------------------------------------------------------------
 * The C interface.
 */

static void report_exception(const wchar_t *what)
{
    std::wstring msg = what;
    try {
        throw;
    } catch (winrt::hresult_error const &e) {
        msg += L": ";
        msg += e.message().c_str();
    } catch (std::exception const &e) {
        msg += L": ";
        std::string s = e.what();
        msg += std::wstring(s.begin(), s.end());
    } catch (...) {
    }
    MessageBoxW(nullptr, msg.c_str(), WUI_APP_NAME L" Fatal Error",
                MB_ICONERROR | MB_OK);
}

extern "C" bool wui_packaged(void)
{
    /* (with a package identity, and the runtime in its package graph,
     * which the package's dependency on it puts there) */
    static int packaged = -1;
    if (packaged < 0) {
        UINT32 len = 0;
        packaged = GetCurrentPackageFullName(&len, nullptr) ==
            ERROR_INSUFFICIENT_BUFFER &&
            LoadLibraryExW(L"Microsoft.WindowsAppRuntime.dll", nullptr, 0);
    }
    return packaged;
}

extern "C" int wui_run(void)
{
    /*
     * Unless we're running from the MSIX package, which depends on
     * it, we have to find the Windows App SDK runtime ourselves. If
     * it's not installed, this offers to send the user off to get it.
     */
    bool packaged = wui_packaged();
    PACKAGE_VERSION min_version{};
    min_version.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    HRESULT hr = packaged ? S_OK : MddBootstrapInitialize2(
        WINDOWSAPPSDK_RELEASE_MAJORMINOR, WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
        min_version, MddBootstrapInitializeOptions_OnNoMatch_ShowUI);
    if (FAILED(hr)) {
        wchar_t msg[256];
        swprintf_s(msg, L"Unable to load the Windows App SDK runtime "
                   L"(error 0x%08lX).", (unsigned long)hr);
        MessageBoxW(nullptr, msg, WUI_APP_NAME L" Fatal Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    int ret = 0;
    try {
        /* The C side has already done CoInitialize, which gives us the
         * single-threaded apartment XAML needs, so a complaint here
         * about COM being initialised already is harmless */
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (winrt::hresult_error const &) {
        }
        mux::Application::Start([](auto &&) { winrt::make<App>(); });
    } catch (...) {
        report_exception(L"Unable to start the user interface");
        ret = 1;
    }

    if (!packaged)
        MddBootstrapShutdown();
    return ret;
}

extern "C" void wui_exit(void)
{
    try {
        mux::Application::Current().Exit();
    } catch (...) {
        PostQuitMessage(0);
    }
}

/* ---- The window ---- */

extern "C" HWND wui_hwnd(WuiHost *host)
{
    return host->hwnd;
}

extern "C" void wui_show(WuiHost *host)
{
    if (host->shown) {
        host->window.Activate();
        return;
    }
    host->shown = true;
    wui_show_without_flash(host->hwnd, host->window);
}

extern "C" void wui_close_window(WuiHost *host)
{
    host->close_confirmed = true;
    /* (as for the config box: see wui_dlg_free) */
    wui_dwm_set(host->hwnd, DWMWA_CLOAK, TRUE);
    host->window.Close();
}

/*
 * A message in a ContentDialog over the window, waited for in a
 * nested event loop, like a message box. There can only be one
 * ContentDialog at a time, and the loop has to be the innermost one
 * when the dialog closes; so if one is already showing, or a modal
 * window (the config box, with its own loop) has the window disabled,
 * this falls back to an ordinary message box.
 */
struct MessageWait {
    bool done = false;
    int result = 0;
};
static bool message_showing = false;

static bool message_can_show(WuiHost *host)
{
    return host && !message_showing && host->layout &&
        host->layout.XamlRoot() && IsWindowEnabled(host->hwnd) &&
        IsWindowVisible(host->hwnd);
}

/* (how tall its content can be for the dialog, centred in the window,
 * to stay below the title bar, the tab strip, so that only the caption
 * buttons are over it: as much below it, and about 180 of the dialog's
 * title, margins and buttons) */
static double message_room(WuiHost *host)
{
    double top = 40, height = 0;
    try {
        if (host->tabs)
            top = std::max(top, host->tabs.ActualHeight());
        height = host->layout.ActualHeight();
    } catch (...) {
    }
    return std::max(120.0, height - 2 * top - 180);
}

/* (a ContentDialog over the window, with its title, for the content
 * and buttons to be added to) */
static muxc::ContentDialog message_dialog(WuiHost *host,
                                          const wchar_t *title)
{
    muxc::ContentDialog d;
    /* (The Windows 11 look, which a dialog made in code doesn't get by
     * default) */
    try {
        d.Style(mux::Application::Current().Resources().Lookup(
                    winrt::box_value(L"DefaultContentDialogStyle"))
                .as<mux::Style>());
    } catch (...) {
    }
    d.XamlRoot(host->layout.XamlRoot());
    /* (It's in a pop-up, which doesn't get the theme the window's been
     * given, so give it that too) */
    d.RequestedTheme(host->layout.ActualTheme());
    d.Title(winrt::box_value(winrt::hstring(title ? title : L"")));
    return d;
}

/* (shows it and waits for it: 1 for the primary button, 2 for the
 * secondary one, 0 for the close one, or -1 if it couldn't be shown) */
static int message_wait(WuiHost *host, muxc::ContentDialog const &d)
{
    auto wait = std::make_shared<MessageWait>();
    try {
        auto queue = host->window.DispatcherQueue();
        auto op = d.ShowAsync();
        message_showing = true;
        op.Completed([wait, queue](auto const &op, wf::AsyncStatus status) {
            if (status == wf::AsyncStatus::Completed) {
                auto r = op.GetResults();
                wait->result = r == muxc::ContentDialogResult::Primary ? 1 :
                    r == muxc::ContentDialogResult::Secondary ? 2 : 0;
            }
            wait->done = true;
            queue.EnqueueEventLoopExit();
        });
        while (!wait->done)
            queue.RunEventLoop(
                winrt::Microsoft::UI::Dispatching::DispatcherRunOptions::
                QuitOnlyLocalLoop, nullptr);
        message_showing = false;
    } catch (...) {
        message_showing = false;
        if (!wait->done) {
            try {
                d.Hide();
            } catch (...) {
            }
            return -1;
        }
    }
    /* Back to the terminal, which had the focus before */
    if (host->active)
        host->active->focus();
    return wait->result;
}

extern "C" bool wui_message(WuiHost *host, const wchar_t *title,
                            const wchar_t *text, const wchar_t *primary,
                            const wchar_t *close, bool warning)
{
    auto fallback = [&]() {
        UINT style = primary ? MB_OKCANCEL | MB_DEFBUTTON1 : MB_OK;
        style |= warning ? MB_ICONWARNING : MB_ICONINFORMATION;
        return MessageBoxW(host ? host->hwnd : nullptr, text, title,
                           style) == IDOK;
    };
    if (!message_can_show(host))
        return fallback();

    int result;
    try {
        auto d = message_dialog(host, title);
        muxc::TextBlock t;
        t.Text(text ? text : L"");
        t.TextWrapping(mux::TextWrapping::Wrap);
        t.IsTextSelectionEnabled(true);
        muxc::ScrollViewer sv;
        sv.MaxHeight(message_room(host));
        sv.Content(t);
        d.Content(sv);
        if (primary) {
            d.PrimaryButtonText(primary);
            d.DefaultButton(muxc::ContentDialogButton::Primary);
        } else {
            d.DefaultButton(muxc::ContentDialogButton::Close);
        }
        d.CloseButtonText(close ? close : L"OK");
        result = message_wait(host, d);
    } catch (...) {
        result = -1;
    }
    return result < 0 ? fallback() : result == 1;
}

extern "C" int wui_alert(WuiHost *host, const WuiAlert *alert)
{
    if (!message_can_show(host))
        return -1;

    try {
        auto d = message_dialog(host, alert->title);
        muxc::StackPanel body;
        body.Spacing(12);
        auto text = [](const wchar_t *s) {
            muxc::TextBlock t;
            t.Text(s ? s : L"");
            t.TextWrapping(mux::TextWrapping::Wrap);
            t.IsTextSelectionEnabled(true);
            return t;
        };
        auto mono = mux::Media::FontFamily(L"Cascadia Mono, Consolas");

        if (alert->heading) {
            /* (in XAML, so that its colour follows the theme) */
            auto h = mux::Markup::XamlReader::Load(
                L"<TextBlock xmlns='http://schemas.microsoft.com/winfx/2006/"
                L"xaml/presentation' TextWrapping='Wrap' FontWeight='SemiBold' "
                L"Style='{StaticResource BodyStrongTextBlockStyle}' "
                L"Foreground='{ThemeResource SystemFillColorCriticalBrush}'/>")
                .as<muxc::TextBlock>();
            h.Text(alert->heading);
            body.Children().Append(h);
        }

        /* (the "More information" ones, two columns of keys and values,
         * in an expander, after the rest) */
        muxc::Grid info;
        info.ColumnSpacing(12);
        info.RowSpacing(6);
        for (int i = 0; i < 2; i++) {
            muxc::ColumnDefinition c;
            c.Width(i ? mux::GridLengthHelper::FromValueAndType(
                            1, mux::GridUnitType::Star) :
                    mux::GridLengthHelper::Auto());
            info.ColumnDefinitions().Append(c);
        }
        int row = -1;
        for (size_t i = 0; i < alert->nitems; i++) {
            const WuiAlertItem &item = alert->items[i];
            switch (item.type) {
              case WUI_ALERT_PARA:
                body.Children().Append(text(item.text));
                break;
              case WUI_ALERT_DISPLAY: {
                auto t = text(item.text);
                t.FontFamily(mono);
                t.Margin(mux::ThicknessHelper::FromLengths(12, 0, 0, 0));
                body.Children().Append(t);
                break;
              }
              case WUI_ALERT_INFO_KEY: {
                muxc::RowDefinition r;
                r.Height(mux::GridLengthHelper::Auto());
                info.RowDefinitions().Append(r);
                row++;
                auto t = text(item.text);
                t.TextWrapping(mux::TextWrapping::NoWrap);
                muxc::Grid::SetRow(t, row);
                info.Children().Append(t);
                break;
              }
              case WUI_ALERT_INFO_VALUE:
              case WUI_ALERT_INFO_BLOB: {
                if (row < 0)
                    break;
                auto t = text(item.text);
                t.FontFamily(mono);
                if (item.type == WUI_ALERT_INFO_BLOB)
                    t.TextWrapping(mux::TextWrapping::WrapWholeWords);
                muxc::Grid::SetRow(t, row);
                muxc::Grid::SetColumn(t, 1);
                info.Children().Append(t);
                break;
              }
            }
        }
        /* (scrolling, if it's taller than the window has room for) */
        muxc::ScrollViewer sv;
        sv.MaxHeight(message_room(host));
        if (row >= 0) {
            /* (a link that shows them, or hides them again) */
            muxc::HyperlinkButton more;
            more.Content(winrt::box_value(winrt::hstring(L"More information")));
            more.Padding(mux::ThicknessHelper::FromLengths(0, 4, 0, 4));
            info.Visibility(mux::Visibility::Collapsed);
            info.Margin(mux::ThicknessHelper::FromLengths(0, 0, 0, 4));
            auto winfo = winrt::make_weak(info);
            auto wsv = winrt::make_weak(sv);
            more.Click([winfo, wsv](auto &&, auto &&) {
                auto i = winfo.get();
                if (!i)
                    return;
                bool show = i.Visibility() == mux::Visibility::Collapsed;
                i.Visibility(show ? mux::Visibility::Visible :
                             mux::Visibility::Collapsed);
                /* (and down to them, if they're below the bottom) */
                if (auto s = wsv.get(); s && show) {
                    s.UpdateLayout();
                    s.ChangeView(nullptr, s.ScrollableHeight(), nullptr);
                }
            });
            body.Children().Append(more);
            body.Children().Append(info);
        }
        sv.Content(body);
        /* (clear of the scroll bar, which sits over the text) */
        body.Padding(mux::ThicknessHelper::FromLengths(0, 0, 16, 0));
        d.Content(sv);

        if (alert->primary)
            d.PrimaryButtonText(alert->primary);
        if (alert->secondary)
            d.SecondaryButtonText(alert->secondary);
        d.CloseButtonText(alert->close ? alert->close : L"Cancel");
        /* (none the default, as in PuTTY's: Enter isn't to accept a
         * host key unread) */
        d.DefaultButton(muxc::ContentDialogButton::None);
        return message_wait(host, d);
    } catch (...) {
        return -1;
    }
}

extern "C" void wui_set_title(WuiHost *host, const wchar_t *title)
{
    host->window.Title(winrt::hstring(title ? title : L""));
}

extern "C" void wui_set_icon(WuiHost *host, HICON icon)
{
    host->icon = icon;
    try {
        host->update_icon();
    } catch (...) {
        /* not worth dying over */
    }
}

extern "C" int wui_get_dpi(WuiHost *host)
{
    return (int)std::lround(host->scale * 96);
}

extern "C" WuiWindowState wui_get_state(WuiHost *host)
{
    return host->get_state();
}

extern "C" void wui_set_state(WuiHost *host, WuiWindowState state)
{
    host->set_state(state);
}

extern "C" void wui_set_resizable(WuiHost *host, bool resizable,
                                  bool maximisable)
{
    host->overlapped.IsResizable(resizable);
    host->overlapped.IsMaximizable(maximisable);
}

extern "C" void wui_set_topmost(WuiHost *host, bool topmost)
{
    host->overlapped.IsAlwaysOnTop(topmost);
}

extern "C" void wui_set_theme(WuiHost *host, int theme, bool tinted_tab)
{
    if (tinted_tab != host->tinted_tabs) {
        host->tinted_tabs = tinted_tab;
        for (auto &tab : host->tablist)
            if (tab->focused)
                tab->focused->update_tab_colours();
    }
    /* (Mica follows the content's theme by itself; the caption
     * buttons, which the system draws, need telling) */
    host->layout.RequestedTheme(theme == 1 ? mux::ElementTheme::Light :
                                theme == 2 ? mux::ElementTheme::Dark :
                                mux::ElementTheme::Default);
    wui_dwm_dark_frame(host->hwnd, wui_is_dark(host->layout));
    try {
        host->appwindow.TitleBar().PreferredTheme(
            theme == 1 ? muw::TitleBarTheme::Light :
            theme == 2 ? muw::TitleBarTheme::Dark :
            muw::TitleBarTheme::UseDefaultAppMode);
    } catch (...) {
        /* (too old a Windows App Runtime: the buttons keep Windows'
         * theme, which is no great loss) */
    }
}

extern "C" void wui_select_adjacent_tab(WuiHost *host, int dir)
{
    host->select_adjacent(dir);
}

extern "C" void wui_select_tab_number(WuiHost *host, int n)
{
    host->select_number(n);
}

extern "C" void wui_show_sessions_popup(WuiHost *host)
{
    /* (not from inside whatever key message this is for) */
    host->window.DispatcherQueue().TryEnqueue([host]() {
        try {
            host->show_sessions_menu();
        } catch (...) {
        }
    });
}

/*
 * The pane next to a term's, in its tab, in a direction: of those whose
 * middle is that way of its middle, and which overlap it across that
 * direction, the nearest; and the keyboard focus put in it.
 */
extern "C" bool wui_term_focus_pane(WuiTerm *term, int dx, int dy)
{
    std::vector<WuiTerm *> list;
    term->tab->panes(list);
    if (list.size() < 2)
        return false;
    auto rect = [term](WuiTerm *t) {
        auto p = t->root.TransformToVisual(term->tab->container)
            .TransformPoint(wf::Point{0, 0});
        return wf::Rect{p.X, p.Y, (float)t->root.ActualWidth(),
                        (float)t->root.ActualHeight()};
    };
    try {
        wf::Rect from = rect(term);
        float fx = from.X + from.Width / 2, fy = from.Y + from.Height / 2;
        WuiTerm *best = nullptr;
        float bestd = 0;
        for (WuiTerm *t : list) {
            if (t == term)
                continue;
            wf::Rect r = rect(t);
            float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
            float along = dx ? (cx - fx) * dx : (cy - fy) * dy;
            bool overlaps = dx ?
                r.Y < from.Y + from.Height && from.Y < r.Y + r.Height :
                r.X < from.X + from.Width && from.X < r.X + r.Width;
            if (along <= 0 || !overlaps)
                continue;
            if (!best || along < bestd) {
                best = t;
                bestd = along;
            }
        }
        if (!best)
            return false;
        best->focus();
        return true;
    } catch (...) {
        return false;
    }
}

/* ---- The tabs ---- */

extern "C" WuiTerm *wui_term_new(WuiHost *host, void *ctx)
{
    return host->new_term(ctx);
}

extern "C" void wui_term_free(WuiTerm *term)
{
    term->win->free_term(term);
}

extern "C" void wui_term_activate(WuiTerm *term)
{
    term->tab->set_focused(term);
    term->win->tabs.SelectedItem(term->item);
}

extern "C" WuiTerm *wui_term_new_split(WuiHost *host, void *ctx,
                                       WuiTerm *beside, bool side_by_side)
{
    return host->new_split(ctx, beside, side_by_side);
}

extern "C" bool wui_term_is_split(WuiTerm *term)
{
    return !term->tab->single();
}

extern "C" size_t wui_term_tab_panes(WuiTerm *term, void **ctxs, size_t max)
{
    std::vector<WuiTerm *> list;
    term->tab->panes(list);
    for (size_t i = 0; i < list.size() && i < max; i++)
        ctxs[i] = list[i]->ctx;
    return list.size();
}

extern "C" bool wui_term_can_move(WuiTerm *term, int dir)
{
    return term->win->move_target(term, dir) >= 0;
}

extern "C" void wui_term_move(WuiTerm *term, int dir)
{
    term->win->move_tab(term, dir);
}

/* The tab's tooltip: its title, and what's wrong, if anything (for
 * the pane with the focus, which the tab's header shows) */
static void update_tab_tooltip(WuiTerm *term)
{
    namespace muxd = winrt::Microsoft::UI::Xaml::Documents;
    muxc::TextBlock tip;
    auto lines = tip.Inlines();
    auto line = [&](winrt::hstring const &text, bool italic) {
        if (lines.Size() > 0)
            lines.Append(muxd::LineBreak());
        muxd::Run run;
        run.Text(text);
        if (italic)
            run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
        lines.Append(run);
    };
    /* (the saved session it's from, if any, and then its title) */
    if (!term->session.empty())
        line(term->session, false);
    line(term->title, false);
    /* What's wrong, if anything; or else the hint */
    if (!term->status.empty())
        line(term->status, false);
    else
        line(term->hint.empty() ? winrt::hstring(
                 L"Double-click to change settings") : term->hint, true);
    muxc::ToolTipService::SetToolTip(term->item, tip);
}

extern "C" void wui_term_set_title(WuiTerm *term, const wchar_t *title)
{
    term->title = winrt::hstring(title ? title : L"");
    if (term->tab->focused == term)
        term->tab->refresh_header();
}

/* The warning icon in the header, if the pane's got a status */
static void update_tab_icon(WuiTerm *term)
{
    if (!term->status.empty()) {
        muxc::FontIconSource icon;
        try {
            icon.FontFamily(mux::Application::Current().Resources().Lookup(
                                winrt::box_value(L"SymbolThemeFontFamily"))
                            .as<mux::Media::FontFamily>());
        } catch (...) {
            icon.FontFamily(mux::Media::FontFamily(
                                L"Segoe Fluent Icons,Segoe MDL2 Assets"));
        }
        icon.Glyph(L"\xE7BA");                 /* Warning */
        try {
            icon.Foreground(mux::Application::Current().Resources().Lookup(
                                winrt::box_value(
                                    L"SystemFillColorCautionBrush"))
                            .as<mux::Media::Brush>());
        } catch (...) {
        }
        term->item.IconSource(icon);
    } else {
        term->item.IconSource(nullptr);
    }
}

extern "C" void wui_term_set_status(WuiTerm *term, const wchar_t *status)
{
    term->status = winrt::hstring(status ? status : L"");
    if (term->tab->focused == term)
        term->tab->refresh_header();
}

extern "C" void wui_term_rename_tab(WuiTerm *term)
{
    /* (once the menu that asked for it has gone) */
    std::weak_ptr<WuiTerm> weak = term->weak_from_this();
    term->win->window.DispatcherQueue().TryEnqueue([weak]() {
        if (auto t = weak.lock())
            t->tab->start_rename();
    });
}

extern "C" void wui_term_set_label(WuiTerm *term, const wchar_t *label,
                                   const wchar_t *session)
{
    term->label = winrt::hstring(label ? label : L"");
    term->session = winrt::hstring(session ? session : L"");
    if (term->tab->focused == term)
        term->tab->refresh_header();
}

extern "C" void wui_term_set_hint(WuiTerm *term, const wchar_t *hint)
{
    term->hint = winrt::hstring(hint ? hint : L"");
    if (term->tab->focused == term)
        term->tab->refresh_header();
}

extern "C" void wui_term_set_background(WuiTerm *term, COLORREF bg)
{
    term->bg = bg;
    mux::Media::SolidColorBrush brush(
        winrt::Windows::UI::ColorHelper::FromArgb(
            255, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
    term->area.Background(brush);
    term->root.Background(brush);
    /* The scroll bar goes on the same background, so it wants the
     * theme that shows up on it */
    int luma = (GetRValue(bg) * 299 + GetGValue(bg) * 587 +
                GetBValue(bg) * 114) / 1000;
    term->scrollbar.RequestedTheme(luma < 128 ? mux::ElementTheme::Dark :
                                   mux::ElementTheme::Light);
    if (term->win->tinted_tabs && term->tab->focused == term)
        term->update_tab_colours();
}

extern "C" void wui_get_panel_size(WuiTerm *term, int *w, int *h)
{
    term->current_size(*w, *h);
}

extern "C" void wui_set_panel_size(WuiTerm *term, int w, int h)
{
    term->win->set_panel_size(term, w, h);
}

extern "C" void wui_set_scrollbar_visible(WuiTerm *term, bool visible)
{
    if (visible == term->scrollbar_visible)
        return;
    term->scrollbar_visible = visible;
    term->scrollbar.Visibility(visible ? mux::Visibility::Visible :
                               mux::Visibility::Collapsed);
    /* The terminal area changes width, but the C side is busy */
    term->win->queue_size_update();
}

extern "C" void wui_set_scrollbar(WuiTerm *term, int total, int start,
                                  int page)
{
    auto &sb = term->scrollbar;
    sb.Maximum(std::max(0, total - page));
    sb.ViewportSize(page);
    sb.LargeChange(page);
    sb.Value(start);
    sb.IsEnabled(total > page);
}

extern "C" void wui_set_cursor(WuiTerm *term, WuiCursor cursor)
{
    term->term_impl->set_cursor(cursor);
}

extern "C" void wui_show_context_menu(WuiTerm *term, int x, int y)
{
    try {
        term->show_menu(x, y);
    } catch (...) {
        /* not worth dying over */
    }
}

extern "C" void wui_focus_terminal(WuiTerm *term)
{
    term->focus();
}

extern "C" bool wui_set_font(WuiTerm *term, const WuiFontRequest *req,
                             WuiFontMetrics *metrics)
{
    try {
        return term->renderer.set_font(req, metrics);
    } catch (...) {
        return false;
    }
}

extern "C" int wui_char_width(WuiTerm *term, unsigned int codepoint)
{
    return term->renderer.char_width(codepoint);
}

extern "C" bool wui_begin_draw(WuiTerm *term)
{
    if (term->win->gpu.lost)
        term->win->recover_device();
    return term->renderer.begin_draw();
}

extern "C" void wui_end_draw(WuiTerm *term)
{
    term->renderer.end_draw();
}

extern "C" void wui_fill_rect(WuiTerm *term, int x, int y, int w, int h,
                              COLORREF c)
{
    term->renderer.fill_rect(x, y, w, h, c);
}

extern "C" void wui_draw_text(WuiTerm *term, const WuiTextRequest *req)
{
    term->renderer.draw_text(req);
}

extern "C" void wui_draw_icon(WuiTerm *term, HICON icon, int x, int y,
                              int w, int h)
{
    term->renderer.draw_icon(icon, x, y, w, h);
}
