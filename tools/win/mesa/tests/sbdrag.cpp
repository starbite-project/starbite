// SPDX-License-Identifier: GPL-2.0-only
//
// sbdrag: how fast DWM composes while a window is dragged (docs/WIN11-GPU.md 6.30). The user, T22: "extremely slow,
// around 2 fps at absolute most" moving one window.
//
//   sbdrag.exe TAG [LOG]
//
// A 600x800 GDI window (text, filled rectangles) is moved 300 times by SetWindowPos (8 px steps across the screen),
// and after each move the test waits for DWM's next composition (DwmFlush), as a drag paces itself on DWM. It reads
// DWM's own counters (DwmGetCompositionTimingInfo: frames composed, dropped, missed) and the KMD's time accumulators
// (sbwddm escape MW_ESCAPE_SB_RATES: GPU busy, the present path's queue wait and CPU copy, presents, GPU jobs) before
// and after, and prints one line:
//   DRAG TAG: N moves in T s = M moves/s; DWM F frames = X fps (dropped D, missed S); per move avg A ms, max B ms;
//   GPU busy G %, present wait W ms/s, present copy C ms/s, P presents, J jobs (J/P per present), thermal holds H
// A 60 s watchdog ends it (exit 90). Exit 0 when it ran.
//
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <d3dkmthk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define MW_ESCAPE_SB_RATES 0x5B02
struct Rates {
    uint32_t Code, Presents, JobsDone, Renders;
    uint64_t GpuBusy100ns, PresentWait100ns, PresentCpu100ns, Now100ns;
    uint32_t ThermalHolds, Reserved;
};

static FILE *logfile;

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

static D3DKMT_HANDLE kmt_adapter;

static bool open_kmt(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *a = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return false;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        a->Release();
        if (d.VendorId != 0x42525453)
            continue;
        D3DKMT_OPENADAPTERFROMLUID o = {};
        o.AdapterLuid = d.AdapterLuid;
        if (D3DKMTOpenAdapterFromLuid(&o) >= 0) {
            kmt_adapter = o.hAdapter;
            return true;
        }
    }
    return false;
}

static bool rates(Rates *r)
{
    memset(r, 0, sizeof(*r));
    r->Code = MW_ESCAPE_SB_RATES;
    if (!kmt_adapter)
        return false;
    D3DKMT_ESCAPE e = {};
    e.hAdapter = kmt_adapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = r;
    e.PrivateDriverDataSize = sizeof(*r);
    return D3DKMTEscape(&e) >= 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        for (int i = 0; i < 8; i++) {
            RECT r = { 20 + i * 60, 100 + i * 70, 200 + i * 60, 160 + i * 70 };
            HBRUSH b = CreateSolidBrush(RGB(30 * i, 80, 255 - 30 * i));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        TextOutA(dc, 20, 20, "sbdrag: Starbite window drag test 0123", 38);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI watchdog(LPVOID)
{
    Sleep(60000);
    out("sbdrag: watchdog: still running after 60 s\n");
    ExitProcess(90);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "drag";
    if (argc > 2)
        logfile = fopen(argv[2], "a");
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    SetProcessDPIAware();
    bool kmt = open_kmt();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sbdrag";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "sbdrag", "sbdrag", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 200, 600, 800, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (!w) {
        out("DRAG %s: FAIL no window\n", tag);
        return 1;
    }
    for (int i = 0; i < 30; i++) {       /* let it appear and settle */
        pump();
        DwmFlush();
    }

    DWM_TIMING_INFO t0 = {}, t1 = {};
    t0.cbSize = t1.cbSize = sizeof(DWM_TIMING_INFO);
    Rates r0, r1;
    bool ok0 = kmt && rates(&r0);
    HRESULT h0 = DwmGetCompositionTimingInfo(nullptr, &t0);
    LARGE_INTEGER f, s, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&s);
    double maxms = 0;
    int moves = 0, sw = GetSystemMetrics(SM_CXSCREEN);
    for (int i = 0; i < 300; i++) {
        int x = 40 + (i * 8) % (sw > 700 ? sw - 640 : 200);
        QueryPerformanceCounter(&a);
        SetWindowPos(w, nullptr, x, 200 + (i % 50) * 4, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        DwmFlush();
        QueryPerformanceCounter(&b);
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms > maxms)
            maxms = ms;
        moves++;
        if ((double)(b.QuadPart - s.QuadPart) / (double)f.QuadPart > 45.0)
            break;                        /* 45 s at most: the watchdog is 60 */
    }
    double secs = (double)(b.QuadPart - s.QuadPart) / (double)f.QuadPart;
    HRESULT h1 = DwmGetCompositionTimingInfo(nullptr, &t1);
    bool ok1 = kmt && rates(&r1);

    unsigned frames = (unsigned)(t1.cFrame - t0.cFrame);
    out("DRAG %s: %d moves in %.2f s = %.1f moves/s; DWM %u frames = %.1f fps (dropped %u, missed %u)%s; per move avg "
        "%.1f ms, max %.1f ms\n", tag, moves, secs, moves / secs, frames, frames / secs,
        (unsigned)(t1.cFramesDropped - t0.cFramesDropped), (unsigned)(t1.cFramesMissed - t0.cFramesMissed),
        SUCCEEDED(h0) && SUCCEEDED(h1) ? "" : " (DWM timing unavailable)", secs * 1000.0 / moves, maxms);
    if (ok0 && ok1) {
        double span = (double)(r1.Now100ns - r0.Now100ns) / 1e7;
        unsigned pres = r1.Presents - r0.Presents, jobs = r1.JobsDone - r0.JobsDone;
        out("DRAG %s: GPU busy %.1f %%, present wait %.1f ms/s, present copy %.1f ms/s, %u presents, %u GPU jobs "
            "(%.1f per present), %u renders, thermal holds %u\n", tag,
            (double)(r1.GpuBusy100ns - r0.GpuBusy100ns) / 1e5 / span,
            (double)(r1.PresentWait100ns - r0.PresentWait100ns) / 1e4 / span,
            (double)(r1.PresentCpu100ns - r0.PresentCpu100ns) / 1e4 / span, pres, jobs, pres ? (double)jobs / pres : 0.0,
            r1.Renders - r0.Renders, r1.ThermalHolds - r0.ThermalHolds);
    } else {
        out("DRAG %s: KMD rates unavailable (adapter %s)\n", tag, kmt ? "open, escape failed" : "not found");
    }
    DestroyWindow(w);
    pump();
    if (logfile)
        fclose(logfile);
    return 0;
}
