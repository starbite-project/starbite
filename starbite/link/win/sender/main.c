/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: the Starbite Link sender. Shows the phone's Windows desktop (the "Starbite Display" when the
 * IddCx virtual monitor is there, else the primary screen) in the Mac app and takes the Mac's keyboard
 * and mouse.
 *
 *   sblinkd.exe                      USB (GenericUsbFn, the phone in device mode), as the logged-on user
 *                                    (Startup folder / Run key / logon task: the fallback mode)
 *   sblinkd.exe --tcp [PORT]         also TCP (default 47474) on 127.0.0.1; --bind 0.0.0.0 for the network
 *   sblinkd.exe --service [ARGS]     the "StarbiteLink" LocalSystem service (service_win.c): starts
 *                                    "sblinkd.exe --session-helper ARGS" in the active console session with
 *                                    a SYSTEM token, so input reaches elevated windows, the flyouts, UAC and
 *                                    the lock/logon screen, and restarts it on session changes or exits
 * A watchdog thread restarts the process when the USB driver or a loop is stuck (sbl_watchdog).
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
#ifdef _WIN32
static int g_helper;             /* --session-helper: started by the service as SYSTEM */
static HANDLE g_quit_ev;         /* --quit-event: the service asks the helper to say BYE and exit */
static HANDLE g_one;             /* the single-instance mutex */
int sbl_service_main(int argc, wchar_t **wargv);   /* service_win.c */
int sbl_service_is_running(void);
#endif

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
        "  --diag           log every OS frame's move/dirty rects (capture diagnostics)\n"
        "  --stall-ms N     forget unacknowledged frames after N ms and send a keyframe (default 3000)\n"
        "  --no-keep-awake  let Windows sleep while the Mac is connected\n"
#ifdef _WIN32
        "  --service [ARGS] run as the StarbiteLink service (LocalSystem); ARGS go to the session helper\n"
        "  --unsafe-tcp     allow --tcp in the SYSTEM session helper (any local program could drive UAC!)\n"
#endif
#ifndef _WIN32
        "  host build: --synth-auto FPS (animate the synthetic desktop), --synth-size WxH, --print-input\n"
#endif
        );
}

#ifdef _WIN32
#define SHUTDOWN_JOIN_MS 1500   /* how long an exit waits for the USB teardown (well under Windows' 5 s) */
#define EXIT_SESSION_ENDING 4   /* service_win.c: this helper's session is ending, start no new one in it */

/* set before the session-end teardown so that every exit path (this handler, or sbl_main returning when the
 * main loop sees stop) reports EXIT_SESSION_ENDING: an exit code 0 made the service relaunch a helper into
 * the dying session 1 s later, before its WTS_SESSION_LOGOFF arrived (review, round 8) */
static volatile LONG g_session_ending;

/* WM_QUERYENDSESSION / WM_ENDSESSION lParam: a sign-out or a shutdown/restart (round 7: the logs could not
 * tell the three session ends apart) */
static const char *end_kind(LPARAM lp, char *b, size_t n)
{
    snprintf(b, n, "%s%s%s (lParam 0x%llx, SM_SHUTTINGDOWN %d)",
             (lp & ENDSESSION_LOGOFF) ? "the user is signing out" : "Windows is shutting down or restarting",
             (lp & ENDSESSION_CRITICAL) ? ", critical" : "", (lp & ENDSESSION_CLOSEAPP) ? ", close app" : "",
             (unsigned long long)(ULONG_PTR)lp, GetSystemMetrics(SM_SHUTTINGDOWN));
    return b;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    char k[160];
    switch (msg) {
    case WM_QUERYENDSESSION:
        sbl_log("Windows asks to end the session: %s", end_kind(lp, k, sizeof k));
        return TRUE;
    case WM_ENDSESSION:
        sbl_log("Windows %s the session: %s", wp ? "is ending" : "cancelled ending", end_kind(lp, k, sizeof k));
        if (wp) {
            InterlockedExchange(&g_session_ending, 1);
            sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, (lp & ENDSESSION_LOGOFF) ? "Windows user is signing out"
                                                                              : "Windows is shutting down");
            /* the process is killed soon after this returns (exit 0x40010004): tear USB down first */
            sbl_sender_shutdown(&g_s, SHUTDOWN_JOIN_MS);
            sbl_log("session end: USB torn down, exiting (code %u: the service starts no helper in this session)",
                    EXIT_SESSION_ENDING);
            ExitProcess(EXIT_SESSION_ENDING);
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
    if (t == CTRL_LOGOFF_EVENT || t == CTRL_SHUTDOWN_EVENT) InterlockedExchange(&g_session_ending, 1);
    sbl_log("console control %lu (%s): BYE(SHUTDOWN)", t, t == CTRL_LOGOFF_EVENT ? "logoff" : t == CTRL_SHUTDOWN_EVENT
                                                            ? "shutdown" : t == CTRL_CLOSE_EVENT ? "close" : "break");
    sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, "sblinkd stopped");
    sbl_sender_shutdown(&g_s, SHUTDOWN_JOIN_MS);
    return TRUE;
}

/* the helper: the service set the quit event (stop, shutdown, console session moved) */
static void quit_thread(void *arg)
{
    (void)arg;
    WaitForSingleObject(g_quit_ev, INFINITE);
    sbl_log("the service asks this helper to stop: BYE(SHUTDOWN)");
    sbl_sender_bye(&g_s, SBLK_BYE_SHUTDOWN, "Starbite Link service stopping");
    sbl_sender_shutdown(&g_s, SHUTDOWN_JOIN_MS);
}

/* Something is stuck beyond what the loops can recover from (sbl_watchdog): a new process. The helper
 * just exits (the service starts a new one at once); a Startup-folder instance starts its own copy. */
static void restart_self(const char *why)
{
    sbl_log("watchdog: %s: restarting sblinkd%s", why, g_helper ? " (the service starts a new helper)" : "");
    if (g_helper) ExitProcess(3);
    if (g_one) { ReleaseMutex(g_one); CloseHandle(g_one); g_one = NULL; }
    wchar_t exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    wchar_t *cl = _wcsdup(GetCommandLineW());
    if (cl && GetModuleFileNameW(NULL, exe, MAX_PATH) &&
        CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else sbl_log("watchdog: could not start a new sblinkd (%lu)", GetLastError());
    ExitProcess(3);
}

static void watchdog_thread(void *arg)
{
    (void)arg;
    for (;;) {
        Sleep(2000);
        char why[128];
        if (sbl_watchdog(&g_s, plat_now_us(), why, sizeof why)) restart_self(why);
    }
}

/* one sender per machine (USB has one interface): Global\ so the SYSTEM helper and a Startup-folder
 * instance in the same session see each other. Returns 0 = ours. */
static int single_instance(void)
{
    const wchar_t *name = L"Global\\StarbiteLinkSender";
    g_one = CreateMutexW(NULL, TRUE, name);
    DWORD e = GetLastError();
    if (g_one && e != ERROR_ALREADY_EXISTS) return 0;
    if (!g_helper) {
        /* ERROR_ACCESS_DENIED: the SYSTEM helper holds it */
        sbl_log("another sblinkd is running (%s): exiting", e == ERROR_ACCESS_DENIED ? "the service's helper" : "this or another session");
        return -1;
    }
    /* the helper: a Startup-folder instance got there first; take over when it exits */
    if (!g_one) g_one = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!g_one) { sbl_log("single instance mutex: %lu", GetLastError()); return -1; }
    sbl_log("a Startup-folder sblinkd is running: waiting for it to exit");
    HANDLE w[2] = { g_one, g_quit_ev };
    DWORD r = WaitForMultipleObjects(g_quit_ev ? 2 : 1, w, FALSE, INFINITE);
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0 ? 0 : -1;
}
#endif

static int sbl_main(int argc, char **argv)
{
#ifdef _WIN32
    /* desktop coordinates in real pixels on every monitor (DXGI rects, SendInput, GDI): first of all, before
     * any thread or user32 call. Logged below, once the log is open. */
    DWORD dpi_err = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ? 0 : GetLastError();
#endif
    sbl_cfg cfg;
    sbl_cfg_default(&cfg);
#ifdef _WIN32
    cfg.usb = 1;
#else
    cfg.tcp_port = SBLK_TCP_PORT;
    int synth_auto = 0, print_input = 0;
#endif
    const char *log_path = NULL;
    int console = 0, unsafe_tcp = 0;
    (void)unsafe_tcp;

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
        else if (!strcmp(a, "--diag")) cfg.diag = 1;
        else if (!strcmp(a, "--stall-ms") && has_num) cfg.stall_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--no-keep-awake")) cfg.keep_awake = 0;
#ifdef _WIN32
        else if (!strcmp(a, "--session-helper")) g_helper = 1;
        else if (!strcmp(a, "--quit-event") && v) { g_quit_ev = (HANDLE)(uintptr_t)strtoull(v, NULL, 0); i++; }
        else if (!strcmp(a, "--unsafe-tcp")) unsafe_tcp = 1;
#endif
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
    if (cfg.stall_ms < 500) cfg.stall_ms = 500;
    cfg.usb_chunk &= ~511;

#ifdef _WIN32
    if (console && AttachConsole(ATTACH_PARENT_PROCESS) == 0) AllocConsole();
    if (console) { FILE *f; freopen_s(&f, "CONOUT$", "w", stderr); }
    plat_log_open(log_path, console);
#else
    plat_log_open(log_path, 1);
    (void)console;
#endif

#ifdef _WIN32
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    sbl_log("sblinkd build %s starting (SBLINK v%u.%u), %s, session %lu%s", SB_BUILD_ID, SBLK_PROTO_MAJOR,
            SBLK_PROTO_MINOR, g_helper ? "service session helper (SYSTEM)" : "user mode", sid,
            *plat_log_path() ? "" : " (no log file)");
    if (g_helper && cfg.tcp_port > 0 && !unsafe_tcp) {
        sbl_log("--tcp ignored: this helper injects input as SYSTEM (UAC included); --unsafe-tcp allows it");
        cfg.tcp_port = 0;
    }
    if (dpi_err)
        sbl_log("SetProcessDpiAwarenessContext failed (%lu): the process is DPI awareness %d; injecting and capturing "
                "threads switch to per-monitor v2 themselves", dpi_err,
                (int)GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()));
    if (g_helper && GetSystemMetrics(SM_SHUTTINGDOWN)) {
        sbl_log("this session is ending (SM_SHUTTINGDOWN): not starting");
        return EXIT_SESSION_ENDING;   /* service_win.c: no new helper in this session */
    }
    if (!g_helper && sbl_service_is_running()) {
        sbl_log("the StarbiteLink service is running and serves this session: this instance exits");
        return 0;
    }
    if (single_instance() < 0) return 1;
    plat_thread(watchdog_thread, NULL);
#else
    sbl_log("sblinkd build %s starting (SBLINK v%u.%u)%s", SB_BUILD_ID, SBLK_PROTO_MAJOR, SBLK_PROTO_MINOR,
            *plat_log_path() ? "" : " (no log file)");
#endif

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#endif

    if (sbl_sender_init(&g_s, &cfg) < 0) { sbl_log("init failed"); return 1; }
#ifdef _WIN32
    plat_thread(msg_thread, NULL);
    if (g_quit_ev) plat_thread(quit_thread, NULL);
#else
    if (synth_auto) synth_set_auto(synth_auto);
    rec_set_print(print_input);
#endif

    int transports = 0;
    if (cfg.tcp_port > 0 && sbl_tcp_start(&g_s, cfg.tcp_bind, cfg.tcp_port, NULL) == 0) transports++;
    if (cfg.usb && sbl_usb_start(&g_s) == 0) transports++;
    if (!transports) { sbl_log("no transport could start: exiting"); return 1; }

    sbl_sender_run(&g_s);
    sbl_sender_shutdown(&g_s, 1500);   /* the USB thread cancels, deactivates and closes before we exit */
#ifdef _WIN32
    if (g_helper && g_session_ending) {
        sbl_log("sblinkd stopped: the session is ending (exit %u)", EXIT_SESSION_ENDING);
        return EXIT_SESSION_ENDING;
    }
#endif
    sbl_log("sblinkd stopped");
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv)
{
    if (argc >= 2 && !wcscmp(wargv[1], L"--service")) return sbl_service_main(argc, wargv);
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
