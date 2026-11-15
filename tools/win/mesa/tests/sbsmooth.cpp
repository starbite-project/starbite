// SPDX-License-Identifier: GPL-2.0-only
//
// sbsmooth: how smooth is this Windows desktop? (docs/WIN11-GPU.md 6.96). Numbers for the things a user feels:
// the compositor's frame pace, the time from an input event to the composed frame, the scheduler's wake-up time,
// the timer's grain, and what each CPU core delivers right now.
//
//   sbsmooth.exe TAG [LOG]
//
// Lines (all start with "SMOOTH TAG:"):
//   cpus      logical CPUs, and per CPU: MHz now / max (CallNtPowerInformation) and the time of a fixed integer loop
//             pinned to that CPU (a throttled or slow core shows as a longer time)
//   timer     NtQueryTimerResolution (current / finest / coarsest, ms); Sleep(1) measured as-is and with
//             timeBeginPeriod(1) (a game's frame limiter that sleeps 1 ms really sleeps this long)
//   wake      SetEvent -> the waiting thread runs: avg / p99 / max in microseconds over 300 wake-ups
//   dwm       DwmGetCompositionTimingInfo: refresh rate and period
//   pace W    a WxH topmost GDI window repaints a moving bar and waits for DWM's next composition (DwmFlush), 6 s:
//             frames, fps, the interval's avg / p50 / p95 / p99 / max, how many intervals were over 25 / 50 / 100 ms,
//             DWM's own frame counters over the run, the KMD's rates (GPU busy, present wait and copy, presents,
//             jobs), and each CPU's busy share. Twice: a 1200x700 window, and one that fills the screen.
//   input     SendInput (absolute mouse move into the window) -> WM_MOUSEMOVE arrives (avg / p95 / max ms), and
//             -> repainted -> DWM has composed it (avg / p95 / max ms), 60 samples each
// A 120 s watchdog ends it (exit 90). Exit 0 when it ran.
//
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <d3dkmthk.h>
#include <mmsystem.h>
#include <powrprof.h>
#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "ntdll.lib")

extern "C" {
NTSYSAPI LONG NTAPI NtQueryTimerResolution(PULONG Coarsest, PULONG Finest, PULONG Current);
NTSYSAPI LONG NTAPI NtQuerySystemInformation(ULONG Class, PVOID Info, ULONG Length, PULONG Returned);
}

#define MW_ESCAPE_SB_RATES 0x5B02
struct Rates {
    uint32_t Code, Presents, JobsDone, Renders;
    uint64_t GpuBusy100ns, PresentWait100ns, PresentCpu100ns, Now100ns;
    uint32_t ThermalHolds, Reserved;
};

struct PPI {   // PROCESSOR_POWER_INFORMATION
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

struct SPPI {  // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION (class 8)
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
};

static FILE *logfile;
static const char *tag = "?";
static LARGE_INTEGER qpf;
static D3DKMT_HANDLE kmt_adapter;
static unsigned ncpu;

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

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart;
}

static double pct(std::vector<double> &v, double p)
{
    if (v.empty())
        return 0;
    std::vector<double> s(v);
    std::sort(s.begin(), s.end());
    size_t i = (size_t)(p * (double)(s.size() - 1) + 0.5);
    return s[i < s.size() ? i : s.size() - 1];
}

static double avg(std::vector<double> &v)
{
    double a = 0;
    for (double x : v)
        a += x;
    return v.empty() ? 0 : a / (double)v.size();
}

static bool open_adapter(void)
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

static void cpu_times(std::vector<SPPI> &v)
{
    v.assign(ncpu, SPPI{});
    NtQuerySystemInformation(8, v.data(), (ULONG)(sizeof(SPPI) * ncpu), nullptr);
}

// "cpu0 41%, cpu1 12% ..." between two samples (KernelTime includes IdleTime); also the DPC + interrupt share
static void cpu_busy_line(const std::vector<SPPI> &a, const std::vector<SPPI> &b, char *s, size_t n)
{
    size_t o = 0;
    s[0] = 0;
    for (unsigned i = 0; i < ncpu && o + 40 < n; i++) {
        double tot = (double)((b[i].KernelTime.QuadPart - a[i].KernelTime.QuadPart) +
                              (b[i].UserTime.QuadPart - a[i].UserTime.QuadPart));
        double idle = (double)(b[i].IdleTime.QuadPart - a[i].IdleTime.QuadPart);
        double dpc = (double)((b[i].DpcTime.QuadPart - a[i].DpcTime.QuadPart) +
                              (b[i].InterruptTime.QuadPart - a[i].InterruptTime.QuadPart));
        o += (size_t)snprintf(s + o, n - o, "%scpu%u %.0f%% (dpc+irq %.1f%%)", i ? ", " : "", i,
                              tot > 0 ? 100.0 * (tot - idle) / tot : 0.0, tot > 0 ? 100.0 * dpc / tot : 0.0);
    }
}

static DWORD WINAPI watchdog(LPVOID)
{
    Sleep(120000);
    out("SMOOTH %s: watchdog: still running after 120 s\n", tag);
    ExitProcess(90);
}

// ---- cpus ----

static volatile uint64_t sink;

static double spin_ms(void)
{
    uint64_t x = 88172645463325252ull, s = 0;
    double t0 = now_ms();
    for (unsigned i = 0; i < 40000000u; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        s += x & 0xff;
    }
    sink = s;
    return now_ms() - t0;
}

static void phase_cpus(void)
{
    std::vector<PPI> p(ncpu);
    bool have = CallNtPowerInformation(ProcessorInformation, nullptr, 0, p.data(), (ULONG)(sizeof(PPI) * ncpu)) == 0;
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    out("SMOOTH %s: cpus %u (affinity 0x%llx of 0x%llx)\n", tag, ncpu, (unsigned long long)proc, (unsigned long long)sys);
    for (unsigned i = 0; i < ncpu && i < 16; i++) {
        double ms = -1;
        if (proc & ((DWORD_PTR)1 << i)) {
            DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << i);
            Sleep(0);
            ms = spin_ms();
            double ms2 = spin_ms();
            if (ms2 < ms)
                ms = ms2;
            if (old)
                SetThreadAffinityMask(GetCurrentThread(), old);
        }
        out("SMOOTH %s: cpu%u %lu MHz now, %lu max, limit %lu; fixed loop %.0f ms\n", tag, i,
            have ? p[i].CurrentMhz : 0, have ? p[i].MaxMhz : 0, have ? p[i].MhzLimit : 0, ms);
    }
}

// ---- timer ----

static void sleep1(const char *what)
{
    std::vector<double> v;
    for (int i = 0; i < 40; i++) {
        double t0 = now_ms();
        Sleep(1);
        v.push_back(now_ms() - t0);
    }
    out("SMOOTH %s: timer Sleep(1) %s: avg %.2f ms, max %.2f ms\n", tag, what, avg(v), pct(v, 1.0));
}

static void phase_timer(void)
{
    ULONG coarse = 0, fine = 0, cur = 0;
    NtQueryTimerResolution(&coarse, &fine, &cur);
    out("SMOOTH %s: timer resolution now %.3f ms (finest %.3f, coarsest %.3f)\n", tag, cur / 10000.0, fine / 10000.0,
        coarse / 10000.0);
    sleep1("as found");
    timeBeginPeriod(1);
    Sleep(20);
    NtQueryTimerResolution(&coarse, &fine, &cur);
    out("SMOOTH %s: timer resolution with timeBeginPeriod(1): %.3f ms\n", tag, cur / 10000.0);
    sleep1("with timeBeginPeriod(1)");
    timeEndPeriod(1);
}

// ---- wake ----

static HANDLE wake_ev, wake_done;
static volatile LONGLONG wake_t0;
static std::vector<double> wake_us;
static volatile LONG wake_stop;

static DWORD WINAPI wake_thread(LPVOID)
{
    for (;;) {
        WaitForSingleObject(wake_ev, INFINITE);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (wake_stop)
            return 0;
        wake_us.push_back((double)(t.QuadPart - wake_t0) * 1e6 / (double)qpf.QuadPart);
        SetEvent(wake_done);
    }
}

static void phase_wake(void)
{
    wake_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    wake_done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, wake_thread, nullptr, 0, nullptr);
    Sleep(50);
    for (int i = 0; i < 300; i++) {
        Sleep(i % 3);                 // the waiter is really asleep, not spinning on a hot core
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        wake_t0 = t.QuadPart;
        SetEvent(wake_ev);
        WaitForSingleObject(wake_done, 1000);
    }
    wake_stop = 1;
    SetEvent(wake_ev);
    WaitForSingleObject(th, 1000);
    unsigned over1 = 0, over5 = 0;
    for (double u : wake_us) {
        over1 += u > 1000;
        over5 += u > 5000;
    }
    out("SMOOTH %s: wake %zu wake-ups: avg %.0f us, p50 %.0f, p99 %.0f, max %.0f; over 1 ms %u, over 5 ms %u\n", tag,
        wake_us.size(), avg(wake_us), pct(wake_us, 0.5), pct(wake_us, 0.99), pct(wake_us, 1.0), over1, over5);
}

// ---- the window ----

static int bar_x;
static volatile LONGLONG move_seen;   // QPC when WM_MOUSEMOVE arrived (0: none yet)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        RECT bar = {bar_x, 0, bar_x + 80, rc.bottom};
        HBRUSH b = CreateSolidBrush(RGB(20, 90, 220));
        FillRect(dc, &bar, b);
        DeleteObject(b);
        TextOutA(dc, 20, 20, "sbsmooth: Starbite desktop smoothness test", 42);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        move_seen = t.QuadPart;
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static HWND make_window(int x, int y, int w, int h, bool popup)
{
    HWND wnd = CreateWindowExA(WS_EX_TOPMOST, "sbsmooth", "sbsmooth", popup ? WS_POPUP : WS_OVERLAPPEDWINDOW, x, y, w, h,
                               nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!wnd)
        return nullptr;
    ShowWindow(wnd, SW_SHOWNORMAL);
    SetForegroundWindow(wnd);
    UpdateWindow(wnd);
    for (int i = 0; i < 20; i++) {
        pump();
        DwmFlush();
    }
    return wnd;
}

static void phase_pace(const char *name, int x, int y, int w, int h, bool popup)
{
    HWND wnd = make_window(x, y, w, h, popup);
    if (!wnd) {
        out("SMOOTH %s: pace %s: no window (%lu)\n", tag, name, GetLastError());
        return;
    }
    RECT rc;
    GetClientRect(wnd, &rc);
    DWM_TIMING_INFO d0 = {}, d1 = {};
    d0.cbSize = d1.cbSize = sizeof(d0);
    Rates r0, r1;
    std::vector<SPPI> c0, c1;
    std::vector<double> iv;
    bool have_d = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &d0));
    bool have_r = rates(&r0);
    cpu_times(c0);
    double t0 = now_ms(), last = t0, t = t0;
    unsigned flush_fail = 0;
    while (t - t0 < 6000.0) {
        bar_x = (bar_x + 12) % (rc.right > 100 ? rc.right - 80 : 1);
        InvalidateRect(wnd, nullptr, FALSE);
        UpdateWindow(wnd);
        if (FAILED(DwmFlush()))
            flush_fail++;
        pump();
        t = now_ms();
        iv.push_back(t - last);
        last = t;
    }
    cpu_times(c1);
    have_r = have_r && rates(&r1);
    have_d = have_d && SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &d1));
    double secs = (t - t0) / 1000.0;
    unsigned o25 = 0, o50 = 0, o100 = 0;
    for (double v : iv) {
        o25 += v > 25;
        o50 += v > 50;
        o100 += v > 100;
    }
    out("SMOOTH %s: pace %s (%ldx%ld client): %zu frames in %.1f s = %.1f fps; interval avg %.1f ms, p50 %.1f, p95 %.1f, "
        "p99 %.1f, max %.1f; over 25 ms %u, over 50 ms %u, over 100 ms %u; DwmFlush failures %u\n", tag, name, rc.right,
        rc.bottom, iv.size(), secs, (double)iv.size() / secs, avg(iv), pct(iv, 0.5), pct(iv, 0.95), pct(iv, 0.99),
        pct(iv, 1.0), o25, o50, o100, flush_fail);
    if (have_d)
        out("SMOOTH %s: pace %s DWM: %llu frames = %.1f fps, refreshes %llu, late %llu, dropped %llu, missed %llu\n", tag,
            name, (unsigned long long)(d1.cFrame - d0.cFrame), (double)(d1.cFrame - d0.cFrame) / secs,
            (unsigned long long)(d1.cRefresh - d0.cRefresh), (unsigned long long)(d1.cFramesLate - d0.cFramesLate),
            (unsigned long long)(d1.cFramesDropped - d0.cFramesDropped),
            (unsigned long long)(d1.cFramesMissed - d0.cFramesMissed));
    if (have_r && r1.Now100ns > r0.Now100ns) {
        double span = (double)(r1.Now100ns - r0.Now100ns);
        unsigned pres = r1.Presents - r0.Presents, jobs = r1.JobsDone - r0.JobsDone;
        out("SMOOTH %s: pace %s KMD: GPU busy %.1f %%, present wait %.0f ms/s, present copy %.0f ms/s, %u presents "
            "(%.1f/s), %u jobs (%.1f per present), thermal holds %u\n", tag, name,
            100.0 * (double)(r1.GpuBusy100ns - r0.GpuBusy100ns) / span,
            1000.0 * (double)(r1.PresentWait100ns - r0.PresentWait100ns) / span,
            1000.0 * (double)(r1.PresentCpu100ns - r0.PresentCpu100ns) / span, pres, (double)pres / secs, jobs,
            pres ? (double)jobs / pres : 0.0, r1.ThermalHolds - r0.ThermalHolds);
    }
    char line[640];
    cpu_busy_line(c0, c1, line, sizeof(line));
    out("SMOOTH %s: pace %s CPU: %s\n", tag, name, line);
    DestroyWindow(wnd);
    pump();
}

static void phase_input(void)
{
    HWND wnd = make_window(200, 200, 1200, 700, false);
    if (!wnd) {
        out("SMOOTH %s: input: no window\n", tag);
        return;
    }
    RECT rc;
    GetClientRect(wnd, &rc);
    POINT c = {rc.right / 2, rc.bottom / 2};
    ClientToScreen(wnd, &c);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    std::vector<double> arrive, composed;
    unsigned lost = 0;
    for (int i = 0; i < 60; i++) {
        Sleep(30 + (i % 4) * 7);       // not in step with the refresh
        pump();
        move_seen = 0;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)(((c.x + (i % 20) * 6 - 60) * 65535LL) / (sw > 1 ? sw - 1 : 1));
        in.mi.dy = (LONG)(((c.y + (i % 7) * 5) * 65535LL) / (sh > 1 ? sh - 1 : 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        if (SendInput(1, &in, sizeof(in)) != 1) {
            lost++;
            continue;
        }
        double deadline = now_ms() + 500.0;
        while (!move_seen && now_ms() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
            pump();
        }
        if (!move_seen) {
            lost++;
            continue;
        }
        arrive.push_back((double)(move_seen - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart);
        bar_x = (bar_x + 40) % (rc.right > 100 ? rc.right - 80 : 1);
        InvalidateRect(wnd, nullptr, FALSE);
        UpdateWindow(wnd);
        DwmFlush();
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        composed.push_back((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart);
    }
    out("SMOOTH %s: input %zu moves (%u not delivered): to the window avg %.1f ms, p95 %.1f, max %.1f; to the composed "
        "frame avg %.1f ms, p95 %.1f, max %.1f\n", tag, arrive.size(), lost, avg(arrive), pct(arrive, 0.95),
        pct(arrive, 1.0), avg(composed), pct(composed, 0.95), pct(composed, 1.0));
    DestroyWindow(wnd);
    pump();
}

int main(int argc, char **argv)
{
    tag = argc > 1 ? argv[1] : "run";
    if (argc > 2)
        logfile = fopen(argv[2], "a");
    QueryPerformanceFrequency(&qpf);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    open_adapter();

    phase_cpus();
    phase_timer();
    phase_wake();

    DWM_TIMING_INFO d = {};
    d.cbSize = sizeof(d);
    BOOL comp = FALSE;
    DwmIsCompositionEnabled(&comp);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &d)))
        out("SMOOTH %s: dwm composition %d, refresh %u/%u Hz, period %.2f ms; screen %dx%d\n", tag, (int)comp,
            d.rateRefresh.uiNumerator, d.rateRefresh.uiDenominator ? d.rateRefresh.uiDenominator : 1,
            (double)d.qpcRefreshPeriod * 1000.0 / (double)qpf.QuadPart, GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN));
    else
        out("SMOOTH %s: dwm composition %d, no timing info\n", tag, (int)comp);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sbsmooth";
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);

    phase_pace("window", 200, 200, 1200, 700, false);
    phase_pace("fullscreen", 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), true);
    phase_input();
    out("SMOOTH %s: done\n", tag);
    return 0;
}
