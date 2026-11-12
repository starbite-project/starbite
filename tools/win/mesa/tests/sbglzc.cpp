// SPDX-License-Identifier: GPL-2.0-only
//
// sbglzc (build/win/gl-zerocopy/DESIGN.md, sb-patches/0150-0151): how fast does our GL ICD present, GDI against
// zero-copy? Geometry Dash: ~5-10 fps, most of each frame in the GDI present's CPU read of the back buffer.
//
//   sbglzc.exe [LOG]            A/B: runs itself twice, SB_GL_PRESENT=gdi then the default (zero-copy), and compares
//   sbglzc.exe one [LOG]        one run in this process, with the environment as it is
//   SBGLZC_SECS=N               (T54) a timed run of N seconds instead of 300 frames, for screenshots
//
// One run: a 1280x720 window (client area), a double-buffered RGBA8 GL context (no vsync if wglSwapIntervalEXT
// exists), and a known frame: 8 vertical colour bars, a white 64x64 square moving 8 px a frame (a camera sees motion
// when frames reach the panel). Before the first swap the back buffer is read back (glReadPixels: with zero-copy the
// back buffer is the shared D3D11 texture, so this checks GL renders into the import right). Then 300 frames are
// drawn and presented; it reports fps, ms per frame (mean, slowest), and from the KMD (sbwddm escape
// MW_ESCAPE_SB_RATES) the CPU time its present path spent per frame and GPU busy, plus which present the ICD took
// (SB_GL_ZC_STATE, set by the ICD) and whether d3d11.dll / sbumd.dll are in the process. After the run the back
// buffer is read again (the frame drawn after the 300th swap: buffers still render right after many swaps).
// Lines:  "RESULT <mode>: <frames> frames in <s> s = <fps> fps; per frame avg <ms> ms, max <ms> ms; kmd present cpu
//          <ms>/frame, gpu busy <pct> %; icd <state>; d3d11 <yes|no> sbumd <yes|no>"
//         "CHECK name: PASS|FAIL ..."
// Exit code: one run = failed checks (100 + n for setup failures); A/B = the two runs' codes added, 0 = both clean.
// A 120 s watchdog ends a run (exit 90).
//
#include <windows.h>
#include <d3dkmthk.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define W 1280
#define H 720
#define FRAMES 300

#define MW_ESCAPE_SB_RATES 0x5B02
struct Rates {
    uint32_t Code, Presents, JobsDone, Renders;
    uint64_t GpuBusy100ns, PresentWait100ns, PresentCpu100ns, Now100ns;
    uint32_t ThermalHolds, Reserved;
};

static FILE *logfile;
static int fails;

static void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt); vprintf(fmt, a); va_end(a);
    fflush(stdout);
    if (logfile) { va_start(a, fmt); vfprintf(logfile, fmt, a); va_end(a); fflush(logfile); }
}

// ---- the KMD's accumulators ----
static D3DKMT_HANDLE kmt_adapter;

static bool rates_on(D3DKMT_HANDLE ad, Rates *r)
{
    memset(r, 0, sizeof(*r));
    r->Code = MW_ESCAPE_SB_RATES;
    D3DKMT_ESCAPE e = {};
    e.hAdapter = ad;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = r;
    e.PrivateDriverDataSize = sizeof(*r);
    return D3DKMTEscape(&e) >= 0 && r->Now100ns != 0;
}

static void open_kmt(void)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = {};
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return;
    for (ULONG i = 0; i < en.NumAdapters && !kmt_adapter; i++) {
        Rates r;
        if (rates_on(info[i].hAdapter, &r))
            kmt_adapter = info[i].hAdapter;   // only sbwddm answers this escape
    }
}

static bool rates(Rates *r) { return kmt_adapter && rates_on(kmt_adapter, r); }

// ---- the known frame ----
static const unsigned char bars[8][3] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}};

static void draw(int frame)
{
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    for (int b = 0; b < 8; b++) {
        glScissor(b * W / 8, 0, W / 8, H);
        glClearColor(bars[b][0] / 255.0f, bars[b][1] / 255.0f, bars[b][2] / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    int x = (frame * 8) % (W - 64);
    glScissor(x, H / 2 - 32, 64, 64);
    glClearColor(0.5f, 0.5f, 0.5f, 1.0f);   // grey: distinct from every bar
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static void check_frame(const char *when, int frame)
{
    GLenum which = GL_BACK;
    glReadBuffer(which);
    int bad = 0;
    for (int b = 0; b < 8; b++) {
        unsigned char p[4] = {0};
        glReadPixels(b * W / 8 + W / 16, H / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        if (abs(p[0] - bars[b][0]) > 8 || abs(p[1] - bars[b][1]) > 8 || abs(p[2] - bars[b][2]) > 8) {
            say("  bar %d: got %d,%d,%d want %d,%d,%d\n", b, p[0], p[1], p[2], bars[b][0], bars[b][1], bars[b][2]);
            bad++;
        }
    }
    unsigned char q[4] = {0};
    int x = (frame * 8) % (W - 64);
    glReadPixels(x + 32, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
    if (abs(q[0] - 128) > 8 || abs(q[1] - 128) > 8 || abs(q[2] - 128) > 8) {
        say("  square: got %d,%d,%d want 128,128,128\n", q[0], q[1], q[2]);
        bad++;
    }
    say("CHECK readback %s: %s (%d of 9 samples wrong)\n", when, bad ? "FAIL" : "PASS", bad);
    if (bad)
        fails++;
}

static DWORD WINAPI watchdog(void *)
{
    Sleep(120000);
    say("FAIL watchdog: the run took over 120 s\n");
    if (logfile) fflush(logfile);
    ExitProcess(90);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) return 0;
    return DefWindowProcA(h, m, w, l);
}

typedef BOOL (WINAPI *PFN_SWAPINTERVAL)(int);

static int run_one(void)
{
    char mode[64] = "", state[64] = "";
    if (!GetEnvironmentVariableA("SB_GL_PRESENT", mode, sizeof mode)) strcpy(mode, "default");
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    open_kmt();
    say("sbglzc run: SB_GL_PRESENT=%s, KMD rates %s\n", mode, kmt_adapter ? "available" : "NOT available");

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "sbglzc";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rc = {0, 0, W, H};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowA("sbglzc", "sbglzc", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, rc.right - rc.left,
                             rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { say("FAIL CreateWindow %lu\n", GetLastError()); return 101; }
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    RECT cr; GetClientRect(wnd, &cr);
    if (cr.right != W || cr.bottom != H)
        say("note: client area %ldx%ld (the screen is smaller?); drawing %dx%d\n", cr.right, cr.bottom, W, H);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    pfd.cAlphaBits = 8; pfd.cDepthBits = 24; pfd.cStencilBits = 8; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { say("FAIL pixel format %d (%lu)\n", pf, GetLastError()); return 102; }
    HGLRC rcx = wglCreateContext(dc);
    if (!rcx || !wglMakeCurrent(dc, rcx)) { say("FAIL wglCreateContext/MakeCurrent (%lu)\n", GetLastError()); return 103; }
    const char *ren = (const char *)glGetString(GL_RENDERER), *ver = (const char *)glGetString(GL_VERSION);
    say("GL_RENDERER %s, GL_VERSION %s\n", ren ? ren : "-", ver ? ver : "-");
    bool sw = !ren || strstr(ren, "softpipe") || strstr(ren, "llvmpipe") || strstr(ren, "GDI Generic");
    if (sw) { say("CHECK renderer: FAIL not the Mali\n"); fails++; }
    PFN_SWAPINTERVAL si = (PFN_SWAPINTERVAL)wglGetProcAddress("wglSwapIntervalEXT");
    say("wglSwapIntervalEXT(0): %s\n", si ? (si(0) ? "ok" : "failed") : "not available");

    draw(0);
    check_frame("before the first swap", 0);
    if (!GetEnvironmentVariableA("SB_GL_ZC_STATE", state, sizeof state)) strcpy(state, "(unset: older ICD?)");
    say("icd present state after setup: %s\n", state);

    Rates r0 = {}, r1 = {};
    bool have = rates(&r0);
    LARGE_INTEGER f, t0, ta, tb, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double slowest = 0;
    int frames = 0;
    ta = t0;
    // T54: SBGLZC_SECS=N runs for N seconds instead of FRAMES frames (screenshots of the window while it runs)
    char sv[16] = "";
    double want_secs = GetEnvironmentVariableA("SBGLZC_SECS", sv, sizeof sv) ? atof(sv) : 0.0;
    if (want_secs > 100.0) want_secs = 100.0;   // under the 120 s watchdog
    if (want_secs > 0) say("timed run: %.0f s\n", want_secs);
    for (; want_secs > 0 ? (double)(ta.QuadPart - t0.QuadPart) < want_secs * (double)f.QuadPart : frames < FRAMES;
         frames++) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        draw(frames);
        if (!SwapBuffers(dc)) { say("FAIL SwapBuffers at frame %d (%lu)\n", frames, GetLastError()); fails++; break; }
        QueryPerformanceCounter(&tb);
        double ms = (double)(tb.QuadPart - ta.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (ms > slowest) slowest = ms;
        ta = tb;
    }
    QueryPerformanceCounter(&t1);
    have = have && rates(&r1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    draw(frames);
    check_frame("after the run", frames);

    if (!GetEnvironmentVariableA("SB_GL_ZC_STATE", state, sizeof state)) strcpy(state, "(unset: older ICD?)");
    bool d3d = GetModuleHandleA("d3d11.dll") != NULL, umd = GetModuleHandleA("sbumd.dll") != NULL;
    char kmd[160] = "kmd rates n/a";
    if (have && frames) {
        double cpu = (double)(r1.PresentCpu100ns - r0.PresentCpu100ns) / 10000.0 / frames;
        double wall = (double)(r1.Now100ns - r0.Now100ns);
        double busy = wall > 0 ? 100.0 * (double)(r1.GpuBusy100ns - r0.GpuBusy100ns) / wall : 0;
        snprintf(kmd, sizeof kmd, "kmd present cpu %.2f ms/frame (%u presents), gpu busy %.1f %%", cpu,
                 r1.Presents - r0.Presents, busy);
    }
    say("RESULT %s: %d frames in %.2f s = %.1f fps; per frame avg %.2f ms, max %.1f ms; %s; icd %s; d3d11 %s sbumd %s\n",
        mode, frames, secs, secs > 0 ? frames / secs : 0.0, frames ? secs * 1000.0 / frames : 0.0, slowest, kmd, state,
        d3d ? "yes" : "no", umd ? "yes" : "no");
    if (want_secs <= 0 && frames < FRAMES) fails++;

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rcx);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    say("sbglzc run end: %d failed\n", fails);
    return fails;
}

static int child(const char *self, const char *mode, const char *log)
{
    if (mode) SetEnvironmentVariableA("SB_GL_PRESENT", mode);
    else SetEnvironmentVariableA("SB_GL_PRESENT", NULL);
    SetEnvironmentVariableA("SB_GL_ZC_STATE", NULL);
    char cmd[MAX_PATH * 2 + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" one \"%s\"", self, log);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (logfile) { fclose(logfile); logfile = NULL; }   // the child appends to the same log
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        logfile = fopen(log, "a");
        say("FAIL CreateProcess %lu\n", GetLastError());
        return 110;
    }
    DWORD code = 99;
    if (WaitForSingleObject(pi.hProcess, 150000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 91);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    logfile = fopen(log, "a");
    say("sbglzc: run SB_GL_PRESENT=%s exited %lu\n", mode ? mode : "(default)", code);
    return (int)code;
}

int main(int argc, char **argv)
{
    bool one = argc > 1 && !strcmp(argv[1], "one");
    const char *log = one ? (argc > 2 ? argv[2] : "sbglzc.log") : (argc > 1 ? argv[1] : "sbglzc.log");
    logfile = fopen(log, "a");
    if (one) {
        int r = run_one();
        if (logfile) fclose(logfile);
        return r;
    }
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    say("sbglzc A/B start (%s)\n", self);
    int a = child(self, "gdi", log);
    int b = child(self, NULL, log);
    say("sbglzc A/B end: gdi run %d, zero-copy run %d (compare the two RESULT lines)\n", a, b);
    if (logfile) fclose(logfile);
    return a + b;
}
