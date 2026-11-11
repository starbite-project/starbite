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
//
// Exit code 0, or 1 when the action failed (the line says why).
//
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

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
    if (!strcmp(argv[1], "frame")) {
        HANDLE h = CreateFileA("C:\\ProgramData\\Starbite\\umdlog\\dwmframe", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            printf("sbshow: dwmframe not written (%lu)\n", GetLastError());
            return 1;
        }
        char b[32];
        int n = snprintf(b, sizeof b, "%02u:%02u:%02u\r\n", t.wHour, t.wMinute, t.wSecond);
        DWORD wr;
        WriteFile(h, b, n, &wr, nullptr);
        CloseHandle(h);
        nudge();
        printf("sbshow: dwmframe rewritten, DWM nudged\n");
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
