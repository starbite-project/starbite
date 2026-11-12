// SPDX-License-Identifier: GPL-2.0-only
//
// sbcomp (docs/WIN11-GPU.md 6.61, T56): which ways of putting D3D11 frames into a window reach the screen?
// T54 (VIS1): sbfps's topmost full-screen window, with a FLIP_SEQUENTIAL or a DISCARD swap chain for its HWND, never
// appeared in the screen shots (wallpaper and consoles only), and DWM's umdlog never opens an app swap chain buffer;
// Explorer's DirectComposition surfaces do reach DWM. One mode per run, a 1200x700 topmost window at 60,60:
//   sbcomp.exe gdi SECS [LOG]        control: GDI FillRect, yellow
//   sbcomp.exe hwnd SECS [LOG]       D3D11 + CreateSwapChainForHwnd (FLIP_SEQUENTIAL, 2 buffers), red
//   sbcomp.exe hwndblt SECS [LOG]    D3D11 + CreateSwapChainForHwnd (DISCARD, 1 buffer), magenta
//   sbcomp.exe dcsc SECS [LOG]       D3D11 + DirectComposition: CreateSwapChainForComposition on a DComp visual, green
//   sbcomp.exe dcsurf SECS [LOG]     D3D11 + DirectComposition: IDCompositionSurface BeginDraw/EndDraw, blue
// Each frame clears the window to the mode's colour, its brightness pulsing (a camera or two shots see motion),
// Present(1, 0) (DComp: Commit). Log: "sbcomp MODE: N frames in S s; failures F; first failure ..." Exit: 0 ok,
// 1 a step failed, 2 usage.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static FILE *logf;
static unsigned fails;
static char first_fail[160];

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (logf) { va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap); fflush(logf); }
}

static bool ok(HRESULT hr, const char *what)
{
    if (SUCCEEDED(hr)) return true;
    if (!fails++) snprintf(first_fail, sizeof first_fail, "%s 0x%08lx", what, (unsigned long)hr);
    if (fails <= 5) say("sbcomp: %s failed 0x%08lx\n", what, (unsigned long)hr);
    return false;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

static float pulse(unsigned frame) { return 0.55f + 0.45f * (float)((frame / 15) % 2); }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: sbcomp gdi|hwnd|hwndblt|dcsc|dcsurf SECS [LOG]\n"); return 2; }
    const char *mode = argv[1];
    double secs = atof(argv[2]);
    logf = fopen(argc > 3 ? argv[3] : "sbcomp.log", "a");
    float col[3];
    bool gdi = !strcmp(mode, "gdi"), hwnd = !strcmp(mode, "hwnd"), hwndblt = !strcmp(mode, "hwndblt"),
         dcsc = !strcmp(mode, "dcsc"), dcsurf = !strcmp(mode, "dcsurf");
    if (gdi) { col[0] = 1; col[1] = 1; col[2] = 0; }
    else if (hwnd) { col[0] = 1; col[1] = 0; col[2] = 0; }
    else if (hwndblt) { col[0] = 1; col[1] = 0; col[2] = 1; }
    else if (dcsc) { col[0] = 0; col[1] = 1; col[2] = 0; }
    else if (dcsurf) { col[0] = 0; col[1] = 0; col[2] = 1; }
    else { printf("unknown mode %s\n", mode); return 2; }

    const int W = 1200, H = 700;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "sbcomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    DWORD ex = WS_EX_TOPMOST | ((dcsc || dcsurf) ? WS_EX_NOREDIRECTIONBITMAP : 0);
    HWND wnd = CreateWindowExA(ex, "sbcomp", "sbcomp", WS_POPUP | WS_VISIBLE, 60, 60, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { say("sbcomp %s: CreateWindow failed %lu\n", mode, GetLastError()); return 1; }
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    pump();
    say("sbcomp %s: window %dx%d at 60,60, colour %.0f,%.0f,%.0f, %.0f s\n", mode, W, H, col[0] * 255, col[1] * 255,
        col[2] * 255, secs);

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain1 *sc = NULL;
    IDCompositionDevice *dc = NULL;
    IDCompositionTarget *target = NULL;
    IDCompositionVisual *visual = NULL;
    IDCompositionSurface *surf = NULL;
    if (!gdi) {
        D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got;
        if (!ok(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                  D3D11_SDK_VERSION, &dev, &got, &ctx), "D3D11CreateDevice"))
            goto out;
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory2 *fac = NULL;
        DXGI_ADAPTER_DESC desc = {};
        if (!ok(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd), "QueryInterface IDXGIDevice")) goto out;
        if (ok(xd->GetAdapter(&ad), "GetAdapter")) {
            ad->GetDesc(&desc);
            ok(ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fac), "GetParent IDXGIFactory2");
        }
        say("sbcomp %s: device feature level 0x%x on \"%ls\" (vendor 0x%x); sbumd %s\n", mode, (unsigned)got,
            desc.Description, desc.VendorId, GetModuleHandleA("sbumd.dll") ? "loaded" : "NOT loaded");
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = W; d.Height = H; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        if (hwnd || hwndblt) {
            d.BufferCount = hwnd ? 2 : 1;
            d.SwapEffect = hwnd ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_DISCARD;
            d.Scaling = DXGI_SCALING_STRETCH;
            if (fac) ok(fac->CreateSwapChainForHwnd(dev, wnd, &d, NULL, NULL, &sc), "CreateSwapChainForHwnd");
        } else {
            if (!ok(DCompositionCreateDevice(xd, __uuidof(IDCompositionDevice), (void **)&dc), "DCompositionCreateDevice"))
                goto out;
            ok(dc->CreateTargetForHwnd(wnd, TRUE, &target), "CreateTargetForHwnd");
            ok(dc->CreateVisual(&visual), "CreateVisual");
            if (dcsc) {
                d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.Scaling = DXGI_SCALING_STRETCH;
                d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                if (fac && ok(fac->CreateSwapChainForComposition(dev, &d, NULL, &sc), "CreateSwapChainForComposition") &&
                    visual)
                    ok(visual->SetContent(sc), "SetContent(swap chain)");
            } else {
                if (ok(dc->CreateSurface(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &surf), "CreateSurface") &&
                    visual)
                    ok(visual->SetContent(surf), "SetContent(surface)");
            }
            if (target && visual) ok(target->SetRoot(visual), "SetRoot");
            ok(dc->Commit(), "Commit");
        }
        if (fac) fac->Release();
        if (ad) ad->Release();
        xd->Release();
        if (fails) goto out;
    }

    {
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        unsigned frames = 0;
        HBRUSH br[2] = {CreateSolidBrush(RGB((int)(col[0] * 140), (int)(col[1] * 140), (int)(col[2] * 140))),
                        CreateSolidBrush(RGB((int)(col[0] * 255), (int)(col[1] * 255), (int)(col[2] * 255)))};
        for (;; frames++) {
            QueryPerformanceCounter(&t);
            if ((double)(t.QuadPart - t0.QuadPart) >= secs * (double)f.QuadPart) break;
            pump();
            float p = pulse(frames);
            float c[4] = {col[0] * p, col[1] * p, col[2] * p, 1.0f};
            if (gdi) {
                HDC hdc = GetDC(wnd);
                RECT r = {0, 0, W, H};
                FillRect(hdc, &r, br[(frames / 15) % 2]);
                ReleaseDC(wnd, hdc);
                GdiFlush();
                Sleep(16);
            } else if (sc && !dcsurf) {
                ID3D11Texture2D *bb = NULL;
                ID3D11RenderTargetView *rtv = NULL;
                if (ok(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb), "GetBuffer") &&
                    ok(dev->CreateRenderTargetView(bb, NULL, &rtv), "CreateRenderTargetView")) {
                    ctx->ClearRenderTargetView(rtv, c);
                    ok(sc->Present(1, 0), "Present");
                }
                if (rtv) rtv->Release();
                if (bb) bb->Release();
            } else if (surf) {
                ID3D11Texture2D *tex = NULL;
                POINT off = {};
                if (ok(surf->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&tex, &off), "BeginDraw")) {
                    ID3D11RenderTargetView *rtv = NULL;
                    if (ok(dev->CreateRenderTargetView(tex, NULL, &rtv), "CreateRenderTargetView(surface)")) {
                        ctx->ClearRenderTargetView(rtv, c);   // the whole atlas texture: covers our W x H at off
                        rtv->Release();
                    }
                    tex->Release();
                    ok(surf->EndDraw(), "EndDraw");
                    ok(dc->Commit(), "Commit");
                }
                Sleep(16);
            }
            if (fails > 50) break;
        }
        double s = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        say("sbcomp %s: %u frames in %.1f s (%.1f fps); failures %u%s%s\n", mode, frames, s, s > 0 ? frames / s : 0.0,
            fails, fails ? "; first: " : "", fails ? first_fail : "");
        DeleteObject(br[0]);
        DeleteObject(br[1]);
    }
out:
    if (fails && !strstr(first_fail, "Present"))
        say("sbcomp %s: setup failed: %s\n", mode, first_fail);
    if (surf) surf->Release();
    if (visual) visual->Release();
    if (target) target->Release();
    if (dc) dc->Release();
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(wnd);
    return fails ? 1 : 0;
}
