// SPDX-License-Identifier: GPL-2.0-only
//
// sbtext: DirectWrite text, a WIC image and Direct2D shapes on the Mali, against WARP (docs/WIN11-GPU.md 6.23-6.24).
// The user's T15 session: XAML apps drew their shapes but no text and no pictures. Direct2D draws text from a glyph
// atlas sampled next to another texture through one sampler (sb-patches/0114) and images through WIC bitmaps.
//
//   sbtext.exe hw [LOG]
//
// The same 512x320 scene is drawn by Direct2D on a D3D11 device of the Mali adapter and on WARP:
//   gray        "Starbite 0123 Text" in Segoe UI, grayscale antialiasing         (rows 0-79)
//   cleartype   the same with ClearType                                          (rows 80-159)
//   image       a 64x64 WIC bitmap (a colour gradient) drawn scaled to 128x128   (rows 160-319, x 0-127)
//   shapes      a filled rounded rectangle and an ellipse outline                (rows 160-319, x 160-511)
// Per region: the dark-pixel count (text) or the mean difference against WARP (image, shapes). The two frames are
// written as sbtext-hw.ppm and sbtext-warp.ppm. Lines "TEST name: PASS|FAIL ..."; exit code = failures
// (100 + n: setup failed).
//
// T33 (sb-patches/0127): "transparent": white grayscale text on a transparent clear (0,0,0,0), as the shell's XAML
// windows draw it (TextInputHost, Explorer, Settings in T32c's dumps: inside each glyph box RGB 255 with alpha = the
// coverage, i.e. not premultiplied). All four channels against WARP, plus the pixels whose colour exceeds alpha.
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define W 512
#define H 320

static FILE *logfile;
static int failures;

static void out(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    fflush(stdout);
    if (logfile) {
        va_start(a, fmt);
        vfprintf(logfile, fmt, a);
        va_end(a);
        fflush(logfile);
    }
}

static IDXGIAdapter1 *mali_adapter(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *a = nullptr, *pick = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (d.VendorId == 0x42525453 || wcsstr(d.Description, L"Mali") || wcsstr(d.Description, L"Starbite")))
            pick = a;
        else
            a->Release();
    }
    return pick;
}

/* Draws the scene with Direct2D on `dev` and reads it back into px (BGRA). */
/* T22: `mask` picks the elements (1 gray text, 2 ClearType text, 4 bitmap, 8 rounded rectangle, 16 ellipse);
 * the white clear is always drawn. T33: 32 = clear transparent instead and draw the text white. */
static bool render(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID2D1Factory *d2d, IDWriteFactory *dw,
                   IWICImagingFactory *wic, UINT *px, const char *who, unsigned mask = 31)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) {
        out("%s: CreateTexture2D 0x%08lx\n", who, (unsigned long)hr);
        return false;
    }
    IDXGISurface *surf = nullptr;
    tex->QueryInterface(__uuidof(IDXGISurface), (void **)&surf);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1RenderTarget *rt = nullptr;
    hr = d2d->CreateDxgiSurfaceRenderTarget(surf, &rp, &rt);
    if (FAILED(hr)) {
        out("%s: CreateDxgiSurfaceRenderTarget 0x%08lx\n", who, (unsigned long)hr);
        return false;
    }
    ID2D1SolidColorBrush *black = nullptr, *blue = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.3f, 0.9f, 1), &blue);
    IDWriteTextFormat *fmt = nullptr;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us", &fmt);

    /* a 64x64 gradient as a WIC bitmap (premultiplied BGRA, opaque) */
    static BYTE grad[64 * 64 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            BYTE *p = grad + (y * 64 + x) * 4;
            p[0] = (BYTE)(x * 4);           /* B */
            p[1] = (BYTE)(y * 4);           /* G */
            p[2] = (BYTE)(255 - x * 4);     /* R */
            p[3] = 255;
        }
    IWICBitmap *wb = nullptr;
    ID2D1Bitmap *bmp = nullptr;
    hr = wic->CreateBitmapFromMemory(64, 64, GUID_WICPixelFormat32bppPBGRA, 64 * 4, sizeof(grad), grad, &wb);
    if (SUCCEEDED(hr))
        hr = rt->CreateBitmapFromWicBitmap(wb, nullptr, &bmp);
    if (FAILED(hr))
        out("%s: WIC bitmap 0x%08lx\n", who, (unsigned long)hr);

    rt->BeginDraw();
    ID2D1SolidColorBrush *white = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->Clear((mask & 32) ? D2D1::ColorF(0, 0, 0, 0) : D2D1::ColorF(1, 1, 1, 1));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (mask & 1)
        rt->DrawText(L"Starbite 0123 Text", 18, fmt, D2D1::RectF(8, 8, W - 8, 80), (mask & 32) ? white : black);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    if (mask & 2)
        rt->DrawText(L"Starbite 0123 Text", 18, fmt, D2D1::RectF(8, 88, W - 8, 160), black);
    if (bmp && (mask & 4))
        rt->DrawBitmap(bmp, D2D1::RectF(0, 176, 128, 304), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (mask & 8)
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(176, 176, 336, 304), 16, 16), blue);
    if (mask & 16)
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(424, 240), 64, 56), black, 6.0f);
    hr = rt->EndDraw();
    if (FAILED(hr))
        out("%s: EndDraw 0x%08lx\n", who, (unsigned long)hr);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st));
    if (ok) {
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m;
        ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            for (int y = 0; y < H; y++)
                memcpy(px + y * W, (const BYTE *)m.pData + y * m.RowPitch, W * 4);
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (bmp) bmp->Release();
    if (wb) wb->Release();
    fmt->Release();
    black->Release();
    blue->Release();
    white->Release();
    rt->Release();
    surf->Release();
    tex->Release();
    return ok && SUCCEEDED(hr);
}

static void ppm(const char *name, const UINT *px)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int dark(const UINT *px, int y0, int y1, int x0, int x1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            UINT p = px[y * W + x];
            if (((p >> 16 & 255) + (p >> 8 & 255) + (p & 255)) < 3 * 128)
                n++;
        }
    return n;
}

static double diff(const UINT *a, const UINT *b, int y0, int y1, int x0, int x1)
{
    double s = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            UINT p = a[y * W + x], q = b[y * W + x];
            for (int c = 0; c < 24; c += 8)
                s += fabs((double)(p >> c & 255) - (double)(q >> c & 255));
        }
    return s / ((y1 - y0) * (x1 - x0) * 3);
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "hw") != 0) {
        printf("usage: sbtext hw [LOG]\n");
        return 2;
    }
    if (argc > 2)
        logfile = fopen(argv[2], "w");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID2D1Factory *d2d = nullptr;
    IDWriteFactory *dw = nullptr;
    IWICImagingFactory *wic = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dw)) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        out("FAIL factories\n");
        return 100;
    }
    IDXGIAdapter1 *mali = mali_adapter();
    if (!mali) {
        out("FAIL no Starbite/Mali adapter\n");
        return 101;
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    ID3D11Device *hw = nullptr, *sw = nullptr;
    ID3D11DeviceContext *hwc = nullptr, *swc = nullptr;
    HRESULT hr = D3D11CreateDevice(mali, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &hw, &got, &hwc);
    if (FAILED(hr)) {
        out("FAIL Mali device 0x%08lx\n", (unsigned long)hr);
        return 102;
    }
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                           D3D11_SDK_VERSION, &sw, &got, &swc);
    if (FAILED(hr)) {
        out("FAIL WARP device 0x%08lx\n", (unsigned long)hr);
        return 103;
    }
    static UINT a[W * H], b[W * H];
    bool okh = render(hw, hwc, d2d, dw, wic, a, "mali");
    bool oks = render(sw, swc, d2d, dw, wic, b, "warp");
    ppm("sbtext-hw.ppm", a);
    ppm("sbtext-warp.ppm", b);
    if (!okh || !oks) {
        out("FAIL render (mali %d, warp %d)\n", okh, oks);
        return 104;
    }
    /* T22: each element alone (and the clear alone): the whole frame against WARP's, and which 16x16 tiles differ */
    {
        static UINT ea[W * H], eb[W * H];
        struct { const char *name; unsigned mask; } el[] = {
            { "only-clear", 0 }, { "only-gray", 1 }, { "only-cleartype", 2 }, { "only-bitmap", 4 },
            { "only-roundrect", 8 }, { "only-ellipse", 16 },
        };
        for (auto &e : el) {
            bool k1 = render(hw, hwc, d2d, dw, wic, ea, "mali", e.mask), k2 = render(sw, swc, d2d, dw, wic, eb, "warp", e.mask);
            int bad_tiles = 0, black_tiles = 0;
            for (int ty = 0; ty < H / 16; ty++)
                for (int tx = 0; tx < W / 16; tx++) {
                    double dd = diff(ea, eb, ty * 16, ty * 16 + 16, tx * 16, tx * 16 + 16);
                    bad_tiles += dd >= 6.0;
                    bool all0 = true;
                    for (int y = ty * 16; y < ty * 16 + 16 && all0; y++)
                        for (int x = tx * 16; x < tx * 16 + 16 && all0; x++)
                            all0 = ea[y * W + x] == 0;
                    black_tiles += all0;
                }
            double dd = diff(ea, eb, 0, H, 0, W);
            bool ok = k1 && k2 && dd < 3.0;
            out("TEST %-14s: %s  frame difference %.2f, %d of %d tiles differ, %d tiles all 0x00000000\n", e.name,
                ok ? "PASS" : "FAIL", dd, bad_tiles, (W / 16) * (H / 16), black_tiles);
            if (e.mask == 0 || e.mask == 8) {
                char n[64];
                snprintf(n, sizeof(n), "sbtext-hw-%s.ppm", e.name);
                ppm(n, ea);
            }
        }
    }
    /* T33: white text on transparent, all four channels against WARP, and premultiplication (colour <= alpha) */
    {
        static UINT ea[W * H], eb[W * H];
        bool k1 = render(hw, hwc, d2d, dw, wic, ea, "mali", 32 | 1), k2 = render(sw, swc, d2d, dw, wic, eb, "warp", 32 | 1);
        double s = 0;
        int over_h = 0, over_s = 0, cov_h = 0, cov_s = 0;
        for (int i = 0; i < W * H; i++) {
            for (int c = 0; c < 32; c += 8)
                s += fabs((double)(ea[i] >> c & 255) - (double)(eb[i] >> c & 255));
            unsigned ah = ea[i] >> 24, as = eb[i] >> 24;
            unsigned mh = 0, ms = 0;
            for (int c = 0; c < 24; c += 8) {
                if ((ea[i] >> c & 255) > mh) mh = ea[i] >> c & 255;
                if ((eb[i] >> c & 255) > ms) ms = eb[i] >> c & 255;
            }
            over_h += mh > ah + 2;
            over_s += ms > as + 2;
            cov_h += ah != 0;
            cov_s += as != 0;
        }
        double dd = s / (W * H * 4.0);
        bool ok = k1 && k2 && cov_s > 200 && dd < 3.0 && over_h <= over_s + 16 && cov_h > cov_s * 7 / 10 &&
                  cov_h < cov_s * 13 / 10;
        out("TEST %-14s: %s  RGBA difference %.2f, colour>alpha mali %d warp %d, covered mali %d warp %d (centre "
            "mali 0x%08x warp 0x%08x)\n", "transparent", ok ? "PASS" : "FAIL", dd, over_h, over_s, cov_h, cov_s,
            ea[40 * W + 60], eb[40 * W + 60]);
        failures += !ok;
        ppm("sbtext-hw-transparent.ppm", ea);
    }
    struct { const char *name; int y0, y1, x0, x1; bool text; } r[] = {
        { "gray", 0, 80, 0, W, true }, { "cleartype", 80, 160, 0, W, true },
        { "image", 160, 320, 0, 128, false }, { "shapes", 160, 320, 160, W, false },
    };
    for (auto &q : r) {
        if (q.text) {
            int dh = dark(a, q.y0, q.y1, q.x0, q.x1), ds = dark(b, q.y0, q.y1, q.x0, q.x1);
            bool ok = ds > 200 && dh > ds * 7 / 10 && dh < ds * 13 / 10;
            out("TEST %-9s: %s  dark pixels mali %d, warp %d (diff %.1f)\n", q.name, ok ? "PASS" : "FAIL", dh, ds,
                diff(a, b, q.y0, q.y1, q.x0, q.x1));
            failures += !ok;
        } else {
            double dd = diff(a, b, q.y0, q.y1, q.x0, q.x1);
            bool ok = dd < 6.0;
            out("TEST %-9s: %s  mean difference to warp %.2f (centre mali 0x%08x warp 0x%08x)\n", q.name,
                ok ? "PASS" : "FAIL", dd, a[((q.y0 + q.y1) / 2) * W + (q.x0 + q.x1) / 2],
                b[((q.y0 + q.y1) / 2) * W + (q.x0 + q.x1) / 2]);
            failures += !ok;
        }
    }
    out("sbtext: %d failure(s)\n", failures);
    if (logfile)
        fclose(logfile);
    return failures;
}
