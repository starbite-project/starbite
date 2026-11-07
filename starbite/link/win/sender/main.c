/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: the Starbite Link sender. Shows the phone's Windows desktop (the "Starbite Display" when the
 * IddCx virtual monitor is there, else the primary screen) in the Mac app and takes the Mac's keyboard
 * and mouse. Runs in the user's session (Startup folder or a logon task; README.md), no admin needed.
 *
 *   sblinkd.exe                      USB (GenericUsbFn, the phone in device mode)
 *   sblinkd.exe --tcp [PORT]         also TCP (default 47474) on 127.0.0.1; --bind 0.0.0.0 for the network
 * Host build (Mac): sblinkd-host serves a synthetic desktop over TCP for developing the viewer.
 */
#include "sender.h"
#ifndef _WIN32
#  include "host.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SB_BUILD_ID
#  define SB_BUILD_ID "dev"
#endif

static sbl_sender g_s;

static void usage(void)
{
    fprintf(stderr,
        "sblinkd (Starbite Link sender, SBLINK v1) build " SB_BUILD_ID "\n"
        "  --tcp [PORT]     listen on TCP (default port 47474)\n"
        "  --bind ADDR      TCP bind address (default 127.0.0.1; 0.0.0.0 = network, no authentication!)\n"
        "  --usb / --no-usb serve the GenericUsbFn interface (Windows default: on)\n"
        "  --output X       capture X: \"primary\", \\\\.\\DISPLAYn, or a monitor name (default: \"Starbite Display\", else primary)\n"
        "  --gdi            capture with GDI BitBlt instead of DXGI desktop duplication\n"
        "  --fps N          frame rate cap (default 30)   --gdi-fps N  GDI poll rate (default 10)\n"
        "  --lz4-accel N    LZ4 acceleration (default 1; higher = faster, bigger)\n"
        "  --usb-chunk N    bytes per USB IN transfer (default 262144)\n"
        "  --name NAME      name in HELLO (default \"Starbite S9\")\n"
        "  --log PATH       log file (default C:\\starbite\\link\\sblinkd.log)\n"
        "  --console        also log to a console\n"
        "  -v               log every frame and key\n"
#ifndef _WIN32
        "  host build: --synth-auto FPS (animate the synthetic desktop), --synth-size WxH, --print-input\n"
#endif
        );
}

#ifdef _WIN32
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            sbl_log("Windows is ending the session: BYE(SHUTDOWN)");
            sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, "Windows is shutting down");
            sbl_sender_stop(&g_s);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            sbl_log("Windows is going to sleep: BYE(SHUTDOWN)");
            sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, "Windows is going to sleep");
        }
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void msg_thread(void *arg)
{
    (void)arg;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"StarbiteLinkSender";
    RegisterClassW(&wc);
    /* a hidden top-level window (message-only windows get no WM_ENDSESSION / power broadcasts) */
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Starbite Link", WS_POPUP, 0, 0, 0, 0, NULL,
                             NULL, wc.hInstance, NULL);
    if (!h) { sbl_log("message window: CreateWindowEx failed (%lu)", GetLastError()); return; }
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
}

static BOOL WINAPI ctrl_handler(DWORD t)
{
    (void)t;
    sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, "sblinkd stopped");
    sbl_sender_stop(&g_s);
    return TRUE;
}
#endif

static int sbl_main(int argc, char **argv)
{
    sbl_cfg cfg;
    sbl_cfg_default(&cfg);
#ifdef _WIN32
    cfg.usb = 1;
#else
    cfg.tcp_port = SBLK_TCP_PORT;
    int synth_auto = 0, print_input = 0;
#endif
    const char *log_path = NULL;
    int console = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        int has_num = v && v[0] >= '0' && v[0] <= '9';
        if (!strcmp(a, "--tcp")) { cfg.tcp_port = has_num ? atoi(argv[++i]) : (int)SBLK_TCP_PORT; }
        else if (!strcmp(a, "--bind") && v) { snprintf(cfg.tcp_bind, sizeof cfg.tcp_bind, "%s", v); i++; }
        else if (!strcmp(a, "--usb")) cfg.usb = 1;
        else if (!strcmp(a, "--no-usb")) cfg.usb = 0;
        else if (!strcmp(a, "--output") && v) { snprintf(cfg.output, sizeof cfg.output, "%s", v); i++; }
        else if (!strcmp(a, "--gdi")) cfg.force_gdi = 1;
        else if (!strcmp(a, "--fps") && has_num) cfg.fps = atoi(argv[++i]);
        else if (!strcmp(a, "--gdi-fps") && has_num) cfg.gdi_fps = atoi(argv[++i]);
        else if (!strcmp(a, "--lz4-accel") && has_num) cfg.lz4_accel = atoi(argv[++i]);
        else if (!strcmp(a, "--usb-chunk") && has_num) cfg.usb_chunk = atoi(argv[++i]);
        else if (!strcmp(a, "--name") && v) { snprintf(cfg.name, sizeof cfg.name, "%s", v); i++; }
        else if (!strcmp(a, "--log") && v) { log_path = v; i++; }
        else if (!strcmp(a, "--console")) console = 1;
        else if (!strcmp(a, "-v")) cfg.verbose = 1;
#ifndef _WIN32
        else if (!strcmp(a, "--synth-auto") && has_num) synth_auto = atoi(argv[++i]);
        else if (!strcmp(a, "--synth-size") && v) {
            unsigned w, h;
            if (sscanf(v, "%ux%u", &w, &h) == 2) synth_set_size(w, h);
            i++;
        }
        else if (!strcmp(a, "--print-input")) print_input = 1;
#endif
        else { usage(); return 2; }
    }
    if (cfg.fps < 1) cfg.fps = 1;
    if (cfg.fps > 60) cfg.fps = 60;
    if (cfg.gdi_fps < 1) cfg.gdi_fps = 1;
    if (cfg.usb_chunk < 4096) cfg.usb_chunk = 4096;
    cfg.usb_chunk &= ~511;

#ifdef _WIN32
    if (console && AttachConsole(ATTACH_PARENT_PROCESS) == 0) AllocConsole();
    if (console) { FILE *f; freopen_s(&f, "CONOUT$", "w", stderr); }
    plat_log_open(log_path, console);
#else
    plat_log_open(log_path, 1);
    (void)console;
#endif

    sbl_log("sblinkd build %s starting (SBLINK v%u.%u)%s", SB_BUILD_ID, SBLK_PROTO_MAJOR, SBLK_PROTO_MINOR,
            *plat_log_path() ? "" : " (no log file)");

#ifdef _WIN32
    HANDLE one = CreateMutexW(NULL, TRUE, L"Local\\StarbiteLinkSender");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        sbl_log("another sblinkd is running in this session: exiting");
        return 1;
    }
    /* desktop coordinates in real pixels on every monitor (DXGI rects, SendInput, GDI) */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        sbl_log("SetProcessDpiAwarenessContext failed (%lu)", GetLastError());
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#endif

    if (sbl_sender_init(&g_s, &cfg) < 0) { sbl_log("init failed"); return 1; }
#ifdef _WIN32
    plat_thread(msg_thread, NULL);
#else
    if (synth_auto) synth_set_auto(synth_auto);
    rec_set_print(print_input);
#endif

    int transports = 0;
    if (cfg.tcp_port > 0 && sbl_tcp_start(&g_s, cfg.tcp_bind, cfg.tcp_port, NULL) == 0) transports++;
    if (cfg.usb && sbl_usb_start(&g_s) == 0) transports++;
    if (!transports) { sbl_log("no transport could start: exiting"); return 1; }

    sbl_sender_run(&g_s);
    sbl_log("sblinkd stopped");
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv)
{
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 1;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc(n > 0 ? (size_t)n : 1);
        if (!argv[i]) return 1;
        argv[i][0] = 0;
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    return sbl_main(argc, argv);
}
#else
int sbl_usb_start(sbl_sender *s) { (void)s; sbl_log("usb: not in the host build"); return -1; }
int main(int argc, char **argv) { return sbl_main(argc, argv); }
#endif
