// sbrotate.cpp (docs/WIN11-GPU.md 6.57): screen orientation for Windows on the S9 (portrait-native 1440x2960 panel).
//   sbrotate.exe query                         prints the current mode and orientation
//   sbrotate.exe landscape|portrait|landscape-flipped|portrait-flipped|toggle
//   sbrotate.exe daemon                        for the Startup folder: once per user (marker), switch to landscape;
//                                              then Ctrl+Alt+Left/Right/Up/Down rotate (as Intel's hotkeys did)
// Uses Windows' own ChangeDisplaySettingsEx (CDS_UPDATEREGISTRY), so Windows saves the choice and Settings > Display >
// Display orientation shows it; the driver (sbwddm with Parameters\Rotation = 1) must offer the rotated path.
// On this portrait-native panel DMDO_90 or DMDO_270 is "landscape"; which one is right side up is set by
// SBROTATE_LANDSCAPE=90|270 (default 90) or C:\starbite\rotate\landscape-270.
// Log: C:\starbite\rotate\sbrotate.log. Exit 0 ok, 1 failed, 2 usage.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "user32.lib")

static const char *kDir = "C:\\starbite\\rotate";
static FILE *logf;

static void say(const char *fmt, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (logf) {
        fprintf(logf, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
        fflush(logf);
    }
}

static const char *oname(DWORD o)
{
    switch (o) {
    case DMDO_DEFAULT: return "portrait (0)";
    case DMDO_90: return "90";
    case DMDO_180: return "portrait-flipped (180)";
    case DMDO_270: return "270";
    default: return "?";
    }
}

static DWORD landscape_orientation(void)
{
    char v[16] = {0};
    if (GetEnvironmentVariableA("SBROTATE_LANDSCAPE", v, sizeof v) && !strcmp(v, "270"))
        return DMDO_270;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\landscape-270", kDir);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? DMDO_270 : DMDO_90;
}

static bool current(DEVMODEA *dm)
{
    ZeroMemory(dm, sizeof *dm);
    dm->dmSize = sizeof *dm;
    return EnumDisplaySettingsExA(NULL, ENUM_CURRENT_SETTINGS, dm, 0) != 0;
}

static int set_orientation(DWORD want)
{
    DEVMODEA dm;
    if (!current(&dm)) { say("sbrotate: EnumDisplaySettingsEx failed (%lu)\n", GetLastError()); return 1; }
    DWORD have = dm.dmDisplayOrientation;
    if (have == want) { say("sbrotate: already %s, %lux%lu\n", oname(want), dm.dmPelsWidth, dm.dmPelsHeight); return 0; }
    bool was_side = have == DMDO_90 || have == DMDO_270, to_side = want == DMDO_90 || want == DMDO_270;
    if (was_side != to_side) {                 // width and height swap between portrait-like and landscape-like
        DWORD w = dm.dmPelsWidth;
        dm.dmPelsWidth = dm.dmPelsHeight;
        dm.dmPelsHeight = w;
    }
    dm.dmDisplayOrientation = want;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL);
    say("sbrotate: %s -> %s, %lux%lu: %s (%ld)\n", oname(have), oname(want), dm.dmPelsWidth, dm.dmPelsHeight,
        r == DISP_CHANGE_SUCCESSFUL ? "ok" : r == DISP_CHANGE_BADMODE ? "the driver does not offer it" :
        r == DISP_CHANGE_RESTART ? "needs a restart" : "failed", r);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}

static int query(void)
{
    DEVMODEA dm;
    if (!current(&dm)) { say("sbrotate: EnumDisplaySettingsEx failed (%lu)\n", GetLastError()); return 1; }
    say("sbrotate: %lux%lu @%lu Hz, orientation %s\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
        oname(dm.dmDisplayOrientation));
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_HOTKEY) {
        DWORD land = landscape_orientation();
        DWORD flipped_land = land == DMDO_90 ? DMDO_270 : DMDO_90;
        switch (w) {
        case 1: set_orientation(land); break;            // Ctrl+Alt+Right: landscape
        case 2: set_orientation(flipped_land); break;    // Ctrl+Alt+Left: landscape, the other way
        case 3: set_orientation(DMDO_DEFAULT); break;    // Ctrl+Alt+Up: portrait
        case 4: set_orientation(DMDO_180); break;        // Ctrl+Alt+Down: portrait flipped
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int daemon(void)
{
    HANDLE one = CreateMutexA(NULL, TRUE, "Local\\sbrotate-daemon");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) { say("sbrotate: daemon already running\n"); return 0; }
    char mark[MAX_PATH];
    snprintf(mark, sizeof mark, "%s\\first-landscape-done", kDir);
    if (GetFileAttributesA(mark) == INVALID_FILE_ATTRIBUTES) {
        Sleep(5000);                                      // let the desktop settle after logon
        if (set_orientation(landscape_orientation()) == 0) {
            FILE *f = fopen(mark, "w");
            if (f) { fputs("done\n", f); fclose(f); }
        }
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "sbrotate";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("sbrotate", "sbrotate", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    int ok = RegisterHotKey(h, 1, mods, VK_RIGHT) + RegisterHotKey(h, 2, mods, VK_LEFT) +
             RegisterHotKey(h, 3, mods, VK_UP) + RegisterHotKey(h, 4, mods, VK_DOWN);
    say("sbrotate: daemon up, %d of 4 hotkeys (Ctrl+Alt+arrows)\n", ok);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}

int main(int argc, char **argv)
{
    CreateDirectoryA("C:\\starbite", NULL);
    CreateDirectoryA(kDir, NULL);
    char lp[MAX_PATH];
    snprintf(lp, sizeof lp, "%s\\sbrotate.log", kDir);
    logf = fopen(lp, "a");
    if (argc < 2) { printf("usage: sbrotate query|landscape|portrait|landscape-flipped|portrait-flipped|toggle|daemon\n"); return 2; }
    const char *c = argv[1];
    DWORD land = landscape_orientation();
    if (!strcmp(c, "query")) return query();
    if (!strcmp(c, "landscape")) return set_orientation(land);
    if (!strcmp(c, "landscape-flipped")) return set_orientation(land == DMDO_90 ? DMDO_270 : DMDO_90);
    if (!strcmp(c, "portrait")) return set_orientation(DMDO_DEFAULT);
    if (!strcmp(c, "portrait-flipped")) return set_orientation(DMDO_180);
    if (!strcmp(c, "toggle")) {
        DEVMODEA dm;
        if (!current(&dm)) return 1;
        bool side = dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270;
        return set_orientation(side ? DMDO_DEFAULT : land);
    }
    if (!strcmp(c, "daemon")) return daemon();
    printf("unknown command %s\n", c);
    return 2;
}
