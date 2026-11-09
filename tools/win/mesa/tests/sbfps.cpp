// SPDX-License-Identifier: GPL-2.0-only
//
// sbfps: how fast does a windowed D3D11 app run on the Mali through sbumd + sbwddm (docs/WIN11-GPU.md 6.21)?
// A full-screen window with a DXGI swap chain; every frame clears to a changing colour and draws a moving
// quad, then Present(0, 0) (no vsync wait). DWM composes the window, the KMD shows DWM's frames. Logs the
// frames, the mean rate and the slowest frame for each swap effect tried, to sbfps.log.
//
//   sbfps.exe hw [SECONDS] [LOG]       the Starbite/Mali adapter (default 20 s per swap effect)
//
// The camera sees a colour cycle with a moving square: if it moves smoothly, DWM's frames reach the panel.
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static const char kHlsl[] =
    "cbuffer X : register(b0) { float4 xf; float4 col; };\n"
    "float4 vs(float2 p : POSITION) : SV_Position { return float4(p * xf.xy + xf.zw, 0, 1); }\n"
    "float4 ps() : SV_Target { return col; }\n";

static FILE *logfile;
/* T14c: T14b's first run stopped somewhere after 512+ good frames and never reported. What each frame is doing,
 * and a watchdog that names the call a stuck frame is in, logs it and ends the process. */
static volatile LONG g_phase, g_frames, g_run;
static const char *const kPhase[] = { "idle", "clear", "map", "draw", "present", "create", "release" };
static volatile LONG64 g_last;          /* QPC of the last completed frame */
static LARGE_INTEGER g_f;
/* T14g: T14b's and T14f's blt runs stopped after ~500-600 frames and even the watchdog's lines never came: the
 * watchdog now writes with WriteFile on its own handle (no CRT locks a stuck main thread could hold) and, when a
 * frame is stuck, suspends the main thread and logs where it is (PC, LR and the frame-pointer chain). */
static HANDLE g_raw = INVALID_HANDLE_VALUE, g_main;

static void raw(const char *fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(b, sizeof(b) - 1, fmt, a);
    va_end(a);
    if (n < 0)
        n = (int)strlen(b);
    DWORD w;
    if (g_raw != INVALID_HANDLE_VALUE)
        WriteFile(g_raw, b, (DWORD)n, &w, nullptr);
}

static void where(const char *what, DWORD64 pc)
{
    HMODULE m = nullptr;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)pc, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof(mod));
        const char *base = strrchr(mod, '\\');
        raw("  %s %s+0x%llx\r\n", what, base ? base + 1 : mod, (unsigned long long)(pc - (DWORD64)(ULONG_PTR)m));
    } else {
        raw("  %s 0x%llx\r\n", what, (unsigned long long)pc);
    }
}

static void dump_main(void)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(g_main) == (DWORD)-1 || !GetThreadContext(g_main, &c)) {
        raw("  (could not read the main thread)\r\n");
        return;
    }
    where("pc", c.Pc);
    where("lr", c.Lr);
    DWORD64 fp = c.Fp;
    for (int i = 0; i < 20 && fp; i++) {
        DWORD64 frame[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)fp, frame, sizeof(frame), &got) ||
            got != sizeof(frame))
            break;
        if (frame[1])
            where("fp->lr", frame[1]);
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
}

static void out(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (logfile) {
        va_start(a, fmt);
        vfprintf(logfile, fmt, a);
        va_end(a);
        fflush(logfile);
    }
}

static DWORD WINAPI Watchdog(LPVOID)
{
    LONG lastFrames = -1;
    LARGE_INTEGER now;
    for (;;) {
        Sleep(2000);
        QueryPerformanceCounter(&now);
        LONG fr = g_frames;
        double idle = (double)(now.QuadPart - g_last) / g_f.QuadPart;
        if (fr != lastFrames && g_phase != 0)
            raw("  run %ld: %ld frames so far\r\n", g_run, fr);
        lastFrames = fr;
        if (g_phase != 0 && idle > 5.0) {
            raw("STUCK: run %ld, %ld frames done, in '%s' for %d s; the main thread:\r\n", g_run, fr,
                kPhase[g_phase < 7 ? g_phase : 0], (int)idle);
            dump_main();
            TerminateProcess(GetCurrentProcess(), 90);
        }
    }
}

/* T14d: T14c's flip run ended the process so suddenly its watchdog never logged: a crash. Name it. */
static LONG WINAPI Crash(EXCEPTION_POINTERS *e)
{
    HMODULE m = nullptr;
    char mod[MAX_PATH] = "?";
    void *addr = e->ExceptionRecord->ExceptionAddress;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)addr, &m);
    if (m)
        GetModuleFileNameA(m, mod, sizeof(mod));
    out("CRASH: run %ld, %ld frames, in '%s': exception 0x%08lx at %p (%s+0x%llx), info %llx %llx\n", g_run,
        g_frames, kPhase[g_phase < 7 ? g_phase : 0], (unsigned long)e->ExceptionRecord->ExceptionCode, addr, mod,
        (unsigned long long)((char *)addr - (char *)m), (unsigned long long)e->ExceptionRecord->ExceptionInformation[0],
        (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    if (logfile)
        fflush(logfile);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static IDXGIAdapter1 *pick_adapter(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *pick = nullptr, *a = nullptr;
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

/* One run with one swap effect: frames for `secs` seconds. 0 = ran. */
static int run(HWND wnd, int w, int h, DXGI_SWAP_EFFECT effect, UINT buffers, const char *name, double secs)
{
    LARGE_INTEGER c0;
    QueryPerformanceCounter(&c0);
    g_last = c0.QuadPart;
    g_frames = 0;
    InterlockedIncrement(&g_run);
    g_phase = 5;                          /* create */
    IDXGIAdapter1 *ad = pick_adapter();
    if (!ad) {
        out("%s: no Starbite/Mali adapter\n", name);
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = buffers;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = effect;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain *sc = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd,
                                               &sc, &dev, &got, &ctx);
    if (FAILED(hr)) {
        out("%s: D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", name, (unsigned long)hr);
        return 1;
    }
    ID3D11Texture2D *bb = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) ||
        FAILED(dev->CreateRenderTargetView(bb, nullptr, &rtv))) {
        out("%s: back buffer failed\n", name);
        return 1;
    }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11InputLayout *il = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "fps", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "fps", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err)) ||
        FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) {
        out("%s: shaders failed\n", name);
        return 1;
    }
    const float quad[] = { -1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, -1 };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
    ID3D11Buffer *vb = nullptr, *cb = nullptr;
    dev->CreateBuffer(&bd, &init, &vb);
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 32;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, &cb);
    D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);

    LARGE_INTEGER f, t0, prev, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    prev = t0;
    g_last = t0.QuadPart;
    unsigned frames = 0, slow = 0;
    double worst = 0;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / f.QuadPart;
        if (t >= secs)
            break;
        double dt = (double)(now.QuadPart - prev.QuadPart) / f.QuadPart;
        prev = now;
        if (frames && dt > worst)
            worst = dt;
        if (frames && dt > 0.1)
            slow++;
        const float clear[4] = { 0.5f + 0.5f * (float)sin(t), 0.5f + 0.5f * (float)sin(t * 1.3 + 2), 0.5f, 1.0f };
        g_phase = 1;
        ctx->ClearRenderTargetView(rtv, clear);
        g_phase = 2;
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float *p = (float *)map.pData;
            p[0] = 0.25f;
            p[1] = 0.25f * w / h;
            p[2] = 0.6f * (float)sin(t * 2.0);
            p[3] = 0.6f * (float)cos(t * 1.7);
            p[4] = 1.0f;
            p[5] = 1.0f;
            p[6] = 1.0f;
            p[7] = 1.0f;
            ctx->Unmap(cb, 0);
        }
        g_phase = 3;
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        UINT stride = 8, off = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->IASetInputLayout(il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(6, 0);
        g_phase = 4;
        hr = sc->Present(0, 0);
        if (FAILED(hr)) {
            out("%s: Present failed 0x%08lx after %u frames\n", name, (unsigned long)hr, frames);
            break;
        }
        frames++;
        g_frames = (LONG)frames;
        LARGE_INTEGER done;
        QueryPerformanceCounter(&done);
        g_last = done.QuadPart;
    }
    g_phase = 6;                          /* release */
    QueryPerformanceCounter(&now);
    double total = (double)(now.QuadPart - t0.QuadPart) / f.QuadPart;
    out("%s: %u frames in %.1f s = %.1f fps; slowest frame %.0f ms; %u frames over 100 ms\n", name, frames, total,
        frames / total, worst * 1000.0, slow);
    sc->Release();
    ctx->Release();
    dev->Release();
    g_phase = 0;
    return 0;
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    double secs = argc > 2 ? atof(argv[2]) : 15.0;
    if (secs < 2 || secs > 300)
        secs = 20.0;
    logfile = fopen(argc > 3 ? argv[3] : "sbfps.log", "a");
    SetUnhandledExceptionFilter(Crash);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    g_raw = CreateFileA(argc > 3 ? "sbfps-watchdog.log" : "sbfps-watchdog.log", FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    QueryPerformanceFrequency(&g_f);
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    SetProcessDPIAware();
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    out("sbfps: screen %dx%d, %.0f s per run\n", w, h, secs);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sbfps";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND wnd = CreateWindowExA(WS_EX_TOPMOST, "sbfps", "sbfps", WS_POPUP | WS_VISIBLE, 0, 0, w, h, nullptr, nullptr,
                               wc.hInstance, nullptr);
    if (!wnd) {
        out("CreateWindow failed %lu\n", GetLastError());
        return 100;
    }
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    int r = 0;
    /* T14g: flip first (T14f's blt stall kept the flip run from ever starting) */
    r += run(wnd, w, h, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, "flip (FLIP_SEQUENTIAL, 2 buffers)", secs);
    r += run(wnd, w, h, DXGI_SWAP_EFFECT_DISCARD, 1, "blt (DISCARD, 1 buffer)", secs);
    DestroyWindow(wnd);
    out("sbfps: done (%d run(s) failed)\n", r);
    if (logfile)
        fclose(logfile);
    return r;
}
