// SPDX-License-Identifier: GPL-2.0-only
//
// sbshow: puts the shell windows the user reported broken on screen for sbumd's DWM frame dumps
// (docs/WIN11-GPU.md 6.44, sb-patches/0128). Run by sb-gputest.cmd; prints one line per action to stdout.
//
//   sbshow mincon             minimise the console it runs in (the gputest's), so other windows come to the front
//   sbshow max CLASS SEC      wait up to SEC s for a visible top-level window of CLASS, then maximise it and
//                             try to bring it to the front (CabinetWClass = File Explorer, ApplicationFrameWindow =
//                             Settings and other UWP apps)
//   sbshow kbd                toggle the touch keyboard through TabTip's ITipInvocation (TabTip.exe must run)
//   sbshow list               every visible top-level window: class, title, rectangle, process
//   sbshow frame              rewrite C:\ProgramData\Starbite\umdlog\dwmframe (DWM dumps its next composed frame),
//                             then flash a 1x1 window so DWM presents at all
//   sbshow transparency 0|1|restore
//                             T36: HKCU ...\Themes\Personalize EnableTransparency (Settings > Personalisation >
//                             Colours > Transparency effects) and the broadcast Settings sends; the first change saves
//                             the user's value to C:\starbite\auto\gpu\transparency.orig, restore puts it back
//   sbshow hit                T36: which window a touch would land in (WindowFromPoint) on a grid over the lower half
//                             of the screen, where the touch keyboard sits
//   sbshow key start|esc      T36: press the Windows key (the Start menu) or Escape
// T37, the sweep (docs/WIN11-GPU.md 6.46):
//   sbshow shot NAME          the screen through GDI (BitBlt of the desktop DC, what any screenshot tool sees), at
//                             quarter size, to C:\ProgramData\Starbite\umdlog\shot-NAME.ppm, with a one-line summary
//   sbshow frame [NAME]       as above; NAME names DWM's dump (dump-dwm-NAME-p<n>.ppm, sb-patches/0130)
//   sbshow snap NAME          shot NAME, then frame NAME (one surface, both views)
//   sbshow keys SPEC          a key chord, e.g. win+a, win+n, ctrl+l, shift+f10, alt+f4, enter, esc
//   sbshow alttab NAME        hold Alt, Tab, then snap NAME while the switcher is up, then release
//   sbshow type TEXT          type TEXT (Unicode key events)
//   sbshow click|rclick XP YP a (right) click at XP%/YP% of the screen
//   sbshow close CLASS        WM_CLOSE to every visible top-level window of CLASS
//   sbshow dialog SEC         a standard message box for SEC seconds (start it with `start ""`)
//   sbshow image PATH         write a 1440x1440 test picture (BMP) for Photos and Paint
//   sbshow disk               free space on C:
//   sbshow diskok MIB         T39: exit 1 below MIB MiB free on C: (the sweep's heavy steps are skipped then)
//
// Exit code 0, or 1 when the action failed (the line says why).
//
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

struct __declspec(uuid("37c994e7-432b-4834-a2f7-dce1f13b834b")) ITipInvocation : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0;
};
static const CLSID CLSID_UIHostNoLaunch = {
    0x4ce576fa, 0x83dc, 0x4f88, { 0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76 }
};

static void exe_of(DWORD pid, char *out, size_t n)
{
    snprintf(out, n, "?");
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32 pe = { sizeof(pe) };
    for (BOOL ok = Process32First(s, &pe); ok; ok = Process32Next(s, &pe))
        if (pe.th32ProcessID == pid) {
            snprintf(out, n, "%s", pe.szExeFile);
            break;
        }
    CloseHandle(s);
}

static BOOL CALLBACK list_one(HWND w, LPARAM)
{
    if (!IsWindowVisible(w))
        return TRUE;
    RECT r;
    GetWindowRect(w, &r);
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1)
        return TRUE;
    char cls[128] = "", title[128] = "", exe[64];
    GetClassNameA(w, cls, sizeof cls);
    GetWindowTextA(w, title, sizeof title);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    exe_of(pid, exe, sizeof exe);
    BOOL cloaked = FALSE;
    /* DWMWA_CLOAKED = 14: UWP windows that are "visible" but not drawn */
    typedef HRESULT(WINAPI * GetAttr)(HWND, DWORD, PVOID, DWORD);
    static GetAttr get = (GetAttr)GetProcAddress(LoadLibraryA("dwmapi.dll"), "DwmGetWindowAttribute");
    if (get)
        get(w, 14, &cloaked, sizeof cloaked);
    printf("sbshow:   window %p class '%s' title '%s' %ld,%ld-%ld,%ld %s pid %lu%s%s\n", (void *)w, cls, title,
           r.left, r.top, r.right, r.bottom, exe, pid, cloaked ? " CLOAKED" : "",
           GetForegroundWindow() == w ? " FOREGROUND" : "");
    return TRUE;
}

struct Find {
    const char *cls;
    HWND found;
};

static BOOL CALLBACK find_one(HWND w, LPARAM p)
{
    Find *f = (Find *)p;
    char cls[128] = "";
    GetClassNameA(w, cls, sizeof cls);
    if (IsWindowVisible(w) && !_stricmp(cls, f->cls)) {
        f->found = w;
        return FALSE;
    }
    return TRUE;
}

static void front(HWND w)
{
    /* the usual dance: a process may set the foreground only if the foreground thread lets it */
    HWND fg = GetForegroundWindow();
    DWORD a = GetCurrentThreadId(), b = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    if (b && b != a)
        AttachThreadInput(a, b, TRUE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    if (b && b != a)
        AttachThreadInput(a, b, FALSE);
}

static LRESULT CALLBACK nudge_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

static void nudge(void)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = nudge_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sbshow_nudge";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "sbshow_nudge", "", WS_POPUP, 0,
                             0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w)
        return;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 30; i++) {     /* ~600 ms: move the pixel so every checked present differs */
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageA(&m);
        SetWindowPos(w, HWND_TOPMOST, i & 1, 0, 1, 1, SWP_NOACTIVATE);
        Sleep(20);
    }
    DestroyWindow(w);
}

static const char *PERSONALIZE = "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
static const char *ORIG = "C:\\starbite\\auto\\gpu\\transparency.orig";

static int set_transparency(DWORD v)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, PERSONALIZE, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr)) {
        printf("sbshow: Personalize key not opened\n");
        return 1;
    }
    DWORD cur = 1, len = sizeof(cur), type = 0;
    bool had = RegQueryValueExA(k, "EnableTransparency", nullptr, &type, (BYTE *)&cur, &len) == 0 && type == REG_DWORD;
    FILE *f = fopen(ORIG, "r");
    if (f)
        fclose(f);
    else if ((f = fopen(ORIG, "w")) != nullptr) {    /* the user's own value, once */
        fprintf(f, "%s %lu\n", had ? "set" : "absent", had ? cur : 1UL);
        fclose(f);
    }
    LONG r = RegSetValueExA(k, "EnableTransparency", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
    DWORD_PTR res;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 3000, &res);
    printf("sbshow: EnableTransparency %lu -> %lu (%s), broadcast sent\n", had ? cur : 1UL, v, r ? "FAILED" : "set");
    return r ? 1 : 0;
}

static int restore_transparency(void)
{
    char what[16] = "";
    unsigned long v = 1;
    FILE *f = fopen(ORIG, "r");
    if (!f) {
        printf("sbshow: nothing to restore (no %s)\n", ORIG);
        return 0;
    }
    int n = fscanf(f, "%15s %lu", what, &v);
    fclose(f);
    if (n != 2)
        return 1;
    if (!strcmp(what, "absent")) {
        HKEY k;
        if (!RegOpenKeyExA(HKEY_CURRENT_USER, PERSONALIZE, 0, KEY_WRITE, &k)) {
            RegDeleteValueA(k, "EnableTransparency");
            RegCloseKey(k);
        }
        DWORD_PTR res;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 3000,
                            &res);
        printf("sbshow: EnableTransparency restored (absent)\n");
    } else {
        set_transparency((DWORD)v);
        printf("sbshow: EnableTransparency restored to %lu\n", v);
    }
    DeleteFileA(ORIG);
    return 0;
}

static void hit_grid(void)
{
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    printf("sbshow: screen %dx%d\n", w, h);
    for (int ry = 0; ry < 6; ry++) {
        int y = h / 2 + (h / 2) * ry / 6 + h / 24;
        printf("sbshow:   y %4d:", y);
        for (int rx = 0; rx < 5; rx++) {
            POINT pt = { w / 10 + (w * 8 / 10) * rx / 4, y };
            HWND hw = WindowFromPoint(pt), top = hw ? GetAncestor(hw, GA_ROOT) : nullptr;
            char cls[48] = "", exe[40] = "?";
            DWORD pid = 0;
            if (top) {
                GetClassNameA(top, cls, sizeof cls);
                GetWindowThreadProcessId(top, &pid);
                exe_of(pid, exe, sizeof exe);
            }
            printf(" [%d: %s %s]", (int)pt.x, top ? exe : "-", cls);
        }
        printf("\n");
    }
}

static void key(WORD vk)
{
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    printf("sbshow: key 0x%02x sent %u\n", vk, SendInput(2, in, sizeof(INPUT)));
}

/* ---- T37: the sweep ------------------------------------------------------------------------------------ */

static int write_dwmframe(const char *name, const SYSTEMTIME &t)
{
    HANDLE h = CreateFileA("C:\\ProgramData\\Starbite\\umdlog\\dwmframe", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("sbshow: dwmframe not written (%lu)\n", GetLastError());
        return 1;
    }
    char b[96];
    int n = snprintf(b, sizeof b, "%s %02u:%02u:%02u\r\n", name && *name ? name : "frame", t.wHour, t.wMinute, t.wSecond);
    DWORD wr;
    WriteFile(h, b, n, &wr, nullptr);
    CloseHandle(h);
    nudge();
    printf("sbshow: dwmframe rewritten (%s), DWM nudged\n", name && *name ? name : "-");
    return 0;
}

/* T39: free space on C: (MiB) and the memory figures, printed with every snap; shots stop below 300 MiB */
static unsigned long long free_mib(void)
{
    ULARGE_INTEGER fr, tot;
    return GetDiskFreeSpaceExA("C:\\", &fr, &tot, nullptr) ? fr.QuadPart >> 20 : 0;
}

static void mem_line(void)
{
    MEMORYSTATUSEX m = { sizeof(m) };
    GlobalMemoryStatusEx(&m);
    printf("sbshow:   memory load %lu%%, RAM free %llu of %llu MiB, commit free %llu of %llu MiB; C: %llu MiB free\n",
           m.dwMemoryLoad, m.ullAvailPhys >> 20, m.ullTotalPhys >> 20, m.ullAvailPageFile >> 20,
           m.ullTotalPageFile >> 20, free_mib());
}

static int shot(const char *name)
{
    if (free_mib() < 600) {
        printf("sbshow: shot %s SKIPPED: C: has %llu MiB free (< 600)\n", name, free_mib());
        return 1;
    }
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        printf("sbshow: shot %s: no bitmap\n", name);
        return 1;
    }
    HGDIOBJ old = SelectObject(mdc, bmp);
    BOOL ok = BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    const unsigned char *px = (const unsigned char *)bits;
    int w4 = w / 4, h4 = h / 4;
    unsigned long long sum = 0, black = 0, n = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "C:\\ProgramData\\Starbite\\umdlog\\shot-%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w4, h4);
        unsigned char *line = (unsigned char *)malloc((size_t)w4 * 3);
        for (int y = 0; line && y < h4; y++) {
            for (int x = 0; x < w4; x++) {
                unsigned r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++) {
                        const unsigned char *p = px + ((size_t)(4 * y + dy) * w + 4 * x + dx) * 4;
                        b += p[0], g += p[1], r += p[2];
                    }
                line[3 * x] = (unsigned char)(r / 16);
                line[3 * x + 1] = (unsigned char)(g / 16);
                line[3 * x + 2] = (unsigned char)(b / 16);
                unsigned l = (r + g + b) / 48;
                sum += l, n++;
                black += l < 8;
            }
            fwrite(line, 1, (size_t)w4 * 3, f);
        }
        free(line);
        fclose(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    printf("sbshow: shot %s %dx%d (BitBlt %s): mean %llu, near-black %llu%% -> %s\n", name, w4, h4, ok ? "ok" : "FAILED",
           n ? sum / n : 0, n ? black * 100 / n : 0, f ? path : "NOT WRITTEN");
    return ok && f ? 0 : 1;
}

static WORD vk_of(const char *k)
{
    static const struct { const char *n; WORD vk; } t[] = {
        { "win", VK_LWIN }, { "ctrl", VK_CONTROL }, { "shift", VK_SHIFT }, { "alt", VK_MENU }, { "esc", VK_ESCAPE },
        { "enter", VK_RETURN }, { "tab", VK_TAB }, { "space", VK_SPACE }, { "del", VK_DELETE }, { "up", VK_UP },
        { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }, { "f4", VK_F4 }, { "f5", VK_F5 },
        { "f10", VK_F10 }, { "f11", VK_F11 }, { "home", VK_HOME }, { "end", VK_END },
    };
    for (auto &e : t)
        if (!_stricmp(k, e.n))
            return e.vk;
    if (strlen(k) == 1 && isalnum((unsigned char)k[0]))
        return (WORD)toupper((unsigned char)k[0]);
    return 0;
}

static int chord(const char *spec, bool down, bool up)
{
    char b[64];
    WORD vks[6];
    int n = 0;
    snprintf(b, sizeof b, "%s", spec);
    for (char *tok = strtok(b, "+"); tok && n < 6; tok = strtok(nullptr, "+")) {
        WORD vk = vk_of(tok);
        if (!vk) {
            printf("sbshow: unknown key '%s'\n", tok);
            return 1;
        }
        vks[n++] = vk;
    }
    INPUT in[12] = {};
    int m = 0;
    if (down)
        for (int i = 0; i < n; i++) {
            in[m].type = INPUT_KEYBOARD;
            in[m++].ki.wVk = vks[i];
        }
    if (up)
        for (int i = n - 1; i >= 0; i--) {
            in[m].type = INPUT_KEYBOARD;
            in[m].ki.wVk = vks[i];
            in[m++].ki.dwFlags = KEYEVENTF_KEYUP;
        }
    UINT sent = SendInput((UINT)m, in, sizeof(INPUT));
    printf("sbshow: keys %s%s sent %u/%d\n", spec, down && up ? "" : down ? " (down)" : " (up)", sent, m);
    return sent == (UINT)m ? 0 : 1;
}

static void type_text(const char *s)
{
    for (; *s; s++) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = (WORD)(unsigned char)*s;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(15);
    }
    printf("sbshow: typed %zu characters\n", strlen(s));
}

static void click(int xp, int yp, bool right)
{
    INPUT in[3] = {};
    for (auto &i : in)
        i.type = INPUT_MOUSE;
    in[0].mi.dx = in[1].mi.dx = in[2].mi.dx = xp * 65535 / 100;
    in[0].mi.dy = in[1].mi.dy = in[2].mi.dy = yp * 65535 / 100;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | (right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    in[2].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | (right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
    printf("sbshow: %sclick at %d%%,%d%% sent %u\n", right ? "right " : "", xp, yp, SendInput(3, in, sizeof(INPUT)));
}

static BOOL CALLBACK close_one(HWND w, LPARAM p)
{
    char cls[128] = "";
    GetClassNameA(w, cls, sizeof cls);
    if (IsWindowVisible(w) && !_stricmp(cls, (const char *)p)) {
        PostMessageA(w, WM_CLOSE, 0, 0);
        printf("sbshow: WM_CLOSE to %p\n", (void *)w);
    }
    return TRUE;
}

static int write_image(const char *path)
{
    const int W = 1440, H = 1440;
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih;
    ih.biWidth = W;
    ih.biHeight = -H;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + W * H * 3;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    unsigned char *row = (unsigned char *)malloc(W * 3);
    for (int y = 0; row && y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char r = (unsigned char)(x * 255 / W), g = (unsigned char)(y * 255 / H), b = (unsigned char)(255 - r);
            if (((x / 90) + (y / 90)) % 2 && y > H * 3 / 4)
                r = g = b = 255;              /* a checkerboard band: scaling and filtering show here */
            if (abs(x - W / 2) < 3 || abs(y - H / 2) < 3)
                r = g = b = 0;                /* a centre cross */
            row[3 * x] = b, row[3 * x + 1] = g, row[3 * x + 2] = r;
        }
        fwrite(row, 1, W * 3, f);
    }
    free(row);
    fclose(f);
    printf("sbshow: test picture written to %s\n", path);
    return 0;
}

static DWORD WINAPI dialog_close(LPVOID p)
{
    Sleep((DWORD)(UINT_PTR)p * 1000);
    ExitProcess(0);
    return 0;
}

int main(int argc, char **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   /* physical pixels */
    if (argc < 2) {
        printf("usage: sbshow mincon | max CLASS SEC | kbd | list | frame\n");
        return 2;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("sbshow %s at %02u:%02u:%02u\n", argv[1], t.wHour, t.wMinute, t.wSecond);

    if (!strcmp(argv[1], "mincon")) {
        HWND c = GetConsoleWindow();
        printf("sbshow: console %p %s\n", (void *)c, c && ShowWindow(c, SW_MINIMIZE) ? "minimised" : "left");
        return 0;
    }
    if (!strcmp(argv[1], "max") && argc >= 4) {
        Find f = { argv[2], nullptr };
        int sec = atoi(argv[3]);
        for (int i = 0; i < sec * 4 && !f.found; i++) {
            EnumWindows(find_one, (LPARAM)&f);
            if (!f.found)
                Sleep(250);
        }
        if (!f.found) {
            printf("sbshow: no visible '%s' window within %d s\n", argv[2], sec);
            return 1;
        }
        ShowWindow(f.found, SW_RESTORE);
        ShowWindow(f.found, SW_SHOWMAXIMIZED);
        front(f.found);
        printf("sbshow: '%s' window %p maximised, foreground %s\n", argv[2], (void *)f.found,
               GetForegroundWindow() == f.found ? "yes" : "no");
        return 0;
    }
    if (!strcmp(argv[1], "kbd")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ITipInvocation *tip = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_UIHostNoLaunch, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER,
                                      __uuidof(ITipInvocation), (void **)&tip);
        if (FAILED(hr)) {
            printf("sbshow: ITipInvocation not available 0x%08lx (is TabTip.exe running?)\n", (unsigned long)hr);
            return 1;
        }
        hr = tip->Toggle(GetDesktopWindow());
        tip->Release();
        printf("sbshow: touch keyboard toggled 0x%08lx\n", (unsigned long)hr);
        return FAILED(hr) ? 1 : 0;
    }
    if (!strcmp(argv[1], "list")) {
        EnumWindows(list_one, 0);
        return 0;
    }
    if (!strcmp(argv[1], "frame"))
        return write_dwmframe(argc >= 3 ? argv[2] : nullptr, t);
    if (!strcmp(argv[1], "shot") && argc >= 3)
        return shot(argv[2]);
    if (!strcmp(argv[1], "snap") && argc >= 3) {
        mem_line();
        int r = shot(argv[2]);
        if (free_mib() < 600)
            return 1;                     /* DWM's dump too (sbumd also checks) */
        return write_dwmframe(argv[2], t) | r;
    }
    if (!strcmp(argv[1], "keys") && argc >= 3)
        return chord(argv[2], true, true);
    if (!strcmp(argv[1], "alttab") && argc >= 3) {
        chord("alt", true, false);
        chord("tab", true, true);
        Sleep(1500);
        int r = shot(argv[2]) | write_dwmframe(argv[2], t);
        Sleep(1500);
        chord("alt", false, true);
        return r;
    }
    if (!strcmp(argv[1], "type") && argc >= 3) {
        type_text(argv[2]);
        return 0;
    }
    if ((!strcmp(argv[1], "click") || !strcmp(argv[1], "rclick")) && argc >= 4) {
        click(atoi(argv[2]), atoi(argv[3]), argv[1][0] == 'r');
        return 0;
    }
    if (!strcmp(argv[1], "close") && argc >= 3) {
        EnumWindows(close_one, (LPARAM)argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "image") && argc >= 3)
        return write_image(argv[2]);
    if (!strcmp(argv[1], "dialog")) {
        int sec = argc >= 3 ? atoi(argv[2]) : 10;
        CreateThread(nullptr, 0, dialog_close, (LPVOID)(UINT_PTR)(sec > 0 ? sec : 10), 0, nullptr);
        MessageBoxW(nullptr, L"Starbite sweep: a standard Windows message box (closes by itself).",
                    L"Starbite sweep", MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
        return 0;
    }
    if (!strcmp(argv[1], "diskok") && argc >= 3) {
        /* T39: exit 1 when C: has less than N MiB free (the sweep skips its heavy app steps then) */
        unsigned long long f = free_mib(), need = strtoull(argv[2], nullptr, 10);
        printf("sbshow: C: %llu MiB free, %s %llu\n", f, f >= need ? "at least" : "BELOW", need);
        return f >= need ? 0 : 1;
    }
    if (!strcmp(argv[1], "mem")) {
        mem_line();
        return 0;
    }
    if (!strcmp(argv[1], "disk")) {
        ULARGE_INTEGER fr, tot;
        if (GetDiskFreeSpaceExA("C:\\", &fr, &tot, nullptr))
            printf("sbshow: C: %llu MiB free of %llu MiB\n", fr.QuadPart >> 20, tot.QuadPart >> 20);
        return 0;
    }
    if (!strcmp(argv[1], "transparency") && argc >= 3) {
        if (!strcmp(argv[2], "restore"))
            return restore_transparency();
        return set_transparency((DWORD)atoi(argv[2]));
    }
    if (!strcmp(argv[1], "hit")) {
        hit_grid();
        return 0;
    }
    if (!strcmp(argv[1], "key") && argc >= 3) {
        key(!strcmp(argv[2], "start") ? VK_LWIN : VK_ESCAPE);
        return 0;
    }
    printf("sbshow: unknown command\n");
    return 2;
}
