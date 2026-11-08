/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd as a Windows service ("StarbiteLink", LocalSystem): sblinkd.exe --service [ARGS]
 *
 * Why: started as the logged-on user (Startup folder), SendInput is subject to UIPI: no clicks into
 * elevated windows, and none into the shell's UIAccess/higher-band windows (the quick settings flyout,
 * parts of Start). uiAccess=true in the manifest would lift UIPI, but only for a binary signed by a
 * certificate the machine trusts (LocalMachine Root, not just testsigning) in a secure location, still
 * runs only after logon and still cannot see or drive the secure desktop (UAC, lock and logon screen).
 * A SYSTEM process in the console session can do all of it (this is how remote desktop tools work).
 *
 * So the service itself (session 0, which has no desktop) only supervises: it starts
 *   sblinkd.exe --session-helper --quit-event H ARGS
 * in the active console session with a copy of its own SYSTEM token (TokenSessionId set to that
 * session) on winsta0\default, restarts it when it exits (backoff 1 s .. 30 s) and moves it when the
 * console session changes (logon screen -> user, fast user switching). The helper is the normal sender;
 * its threads follow the input desktop, so the Mac also works at the logon screen and on UAC prompts.
 *
 * At every start the service also sets the active power scheme's sleep, hibernate, unattended-sleep and
 * display-off/dim/lock-display-off timeouts to 0 (never) on AC and DC, and HibernateEnabled = 0: this
 * phone's Windows has no power button driver and does not wake once asleep. --keep-power-settings skips it.
 * It also configures its own recovery: restart 5 s after a crash.
 *
 * Session lifecycle (round 7: three sign-outs in a row): the service logs every control with its name and
 * session id, never starts a helper into a session that is being logged off (WTS_SESSION_LOGOFF seen, or
 * the helper was killed by the end of its session: exit 0x40010004 DBG_TERMINATE_PROCESS, or it exited
 * with 4 "session ending"), and only into a session that is connected to the console (WTSActive or
 * WTSConnected). Before, it relaunched into the dying session (killed again within 0.1-0.4 s) and into
 * the next one before it was connected (SetProcessDpiAwarenessContext error 5, DuplicateOutput denied).
 * It also holds Global\StarbiteLinkUsbConfigured, the helpers' "the USB bus is configured" marker
 * (usbfn_win.c), so it outlives each helper.
 */
#include "sender.h"

#include <powrprof.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SB_BUILD_ID
#  define SB_BUILD_ID "dev"
#endif

#define SVC_NAME L"StarbiteLink"

static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_st;
static HANDLE g_stop_ev, g_session_ev;
/* a session Windows is logging off (WTS_SESSION_LOGOFF): no helper goes there until it logs on again */
static volatile DWORD g_ending_sid = 0xFFFFFFFFu;

#define EXIT_SESSION_KILLED 0x40010004u   /* DBG_TERMINATE_PROCESS: csrss ended the helper with its session */
#define EXIT_SESSION_ENDING 4u            /* the helper saw SM_SHUTTINGDOWN at start (main.c) */

static const char *wts_name(DWORD t)
{
    switch (t) {
    case WTS_CONSOLE_CONNECT: return "CONSOLE_CONNECT";
    case WTS_CONSOLE_DISCONNECT: return "CONSOLE_DISCONNECT";
    case WTS_REMOTE_CONNECT: return "REMOTE_CONNECT";
    case WTS_REMOTE_DISCONNECT: return "REMOTE_DISCONNECT";
    case WTS_SESSION_LOGON: return "LOGON";
    case WTS_SESSION_LOGOFF: return "LOGOFF";
    case WTS_SESSION_LOCK: return "LOCK";
    case WTS_SESSION_UNLOCK: return "UNLOCK";
    case WTS_SESSION_REMOTE_CONTROL: return "REMOTE_CONTROL";
    default: return "?";
    }
}

static const char *state_name(int st)
{
    switch (st) {
    case WTSActive: return "active";
    case WTSConnected: return "connected";
    case WTSConnectQuery: return "connecting";
    case WTSShadow: return "shadow";
    case WTSDisconnected: return "disconnected";
    case WTSIdle: return "idle";
    case WTSListen: return "listen";
    case WTSReset: return "reset";
    case WTSDown: return "down";
    case WTSInit: return "init";
    default: return "?";
    }
}

/* the session's connect state, -1 if it cannot be read */
static int session_state(DWORD sid)
{
    WTS_CONNECTSTATE_CLASS *p = NULL;
    DWORD n = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sid, WTSConnectState, (LPWSTR *)&p, &n) || !p)
        return -1;
    int st = (int)*p;
    WTSFreeMemory(p);
    return st;
}
static int g_argc;
static wchar_t **g_argv;

int sbl_service_is_running(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!m) return 0;
    SC_HANDLE sv = OpenServiceW(m, SVC_NAME, SERVICE_QUERY_STATUS);
    int run = 0;
    if (sv) {
        SERVICE_STATUS st;
        if (QueryServiceStatus(sv, &st))
            run = st.dwCurrentState == SERVICE_RUNNING || st.dwCurrentState == SERVICE_START_PENDING;
        CloseServiceHandle(sv);
    }
    CloseServiceHandle(m);
    return run;
}

static void set_state(DWORD state, DWORD wait_hint)
{
    g_st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_st.dwCurrentState = state;
    g_st.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    g_st.dwWin32ExitCode = NO_ERROR;
    g_st.dwWaitHint = wait_hint;
    g_st.dwCheckPoint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : g_st.dwCheckPoint + 1;
    SetServiceStatus(g_ssh, &g_st);
}

static DWORD WINAPI ctrl(DWORD code, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)data; (void)ctx;
    switch (code) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        sbl_log("service: %s", code == SERVICE_CONTROL_STOP ? "STOP" : code == SERVICE_CONTROL_SHUTDOWN ? "SHUTDOWN"
                                                                                                      : "PRESHUTDOWN (Windows is shutting down or restarting)");
        set_state(SERVICE_STOP_PENDING, 5000);
        SetEvent(g_stop_ev);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE: {
        DWORD sid = data ? ((WTSSESSION_NOTIFICATION *)data)->dwSessionId : 0xFFFFFFFFu;
        int st = sid != 0xFFFFFFFFu ? session_state(sid) : -1;
        sbl_log("service: session change %lu %s (session %lu, now %s; console session %lu)", type, wts_name(type),
                sid, st >= 0 ? state_name(st) : "?", WTSGetActiveConsoleSessionId());
        if (type == WTS_SESSION_LOGOFF) g_ending_sid = sid;
        else if ((type == WTS_SESSION_LOGON || type == WTS_CONSOLE_CONNECT) && sid == g_ending_sid) g_ending_sid = 0xFFFFFFFFu;
        SetEvent(g_session_ev);
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void enable_priv(const wchar_t *name)
{
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return;
    TOKEN_PRIVILEGES tp;
    memset(&tp, 0, sizeof tp);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(t, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(t);
}

/* ---- power: never sleep, never turn the display off (the phone cannot wake up again) ----------------- */

static const GUID G_SUB_SLEEP = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
static const GUID G_SUB_VIDEO = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
static const struct { const GUID *sub; GUID set; const char *name; } POWER_ZERO[] = {
    { &G_SUB_SLEEP, { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } }, "sleep after" },
    { &G_SUB_SLEEP, { 0x9d7815a6, 0x7ee4, 0x497e, { 0x88, 0x88, 0x51, 0x5a, 0x05, 0xf0, 0x23, 0x64 } }, "hibernate after" },
    { &G_SUB_SLEEP, { 0x7bc4a2f9, 0xd8fc, 0x4469, { 0xb0, 0x7b, 0x33, 0xeb, 0x78, 0x5a, 0xac, 0xa0 } }, "unattended sleep" },
    { &G_SUB_VIDEO, { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } }, "display off after" },
    { &G_SUB_VIDEO, { 0x17aaa29b, 0x8b43, 0x4b94, { 0xaa, 0xfe, 0x35, 0xf6, 0x4d, 0xaa, 0xf1, 0xee } }, "dim display after" },
    { &G_SUB_VIDEO, { 0x8ec4b3a5, 0x6868, 0x48c2, { 0xbe, 0x75, 0x4f, 0x30, 0x44, 0xbe, 0x88, 0xa7 } }, "lock screen display off" },
};

static void power_never_sleep(void)
{
    GUID *scheme = NULL;
    DWORD r = PowerGetActiveScheme(NULL, &scheme);
    if (r != ERROR_SUCCESS || !scheme) { sbl_log("power: PowerGetActiveScheme %lu", r); return; }
    char done[256] = "";
    for (size_t i = 0; i < sizeof POWER_ZERO / sizeof POWER_ZERO[0]; i++) {
        DWORD a = PowerWriteACValueIndex(NULL, scheme, POWER_ZERO[i].sub, &POWER_ZERO[i].set, 0);
        DWORD d = PowerWriteDCValueIndex(NULL, scheme, POWER_ZERO[i].sub, &POWER_ZERO[i].set, 0);
        size_t k = strlen(done);
        snprintf(done + k, sizeof done - k, "%s%s%s", k ? ", " : "", POWER_ZERO[i].name, a || d ? " (FAILED)" : "");
    }
    r = PowerSetActiveScheme(NULL, scheme);
    LocalFree(scheme);
    DWORD zero = 0;
    LSTATUS h = RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled",
                                REG_DWORD, &zero, sizeof zero);
    sbl_log("power: active scheme, AC and DC set to never: %s; re-applied %lu; HibernateEnabled = 0: %ld", done, r, (long)h);
}

/* restart the service 5 s after a crash (three times, counter reset after a day) */
static void set_recovery(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!m) return;
    SC_HANDLE sv = OpenServiceW(m, SVC_NAME, SERVICE_CHANGE_CONFIG | SERVICE_START);
    if (sv) {
        SC_ACTION act[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
        SERVICE_FAILURE_ACTIONSW fa;
        memset(&fa, 0, sizeof fa);
        fa.dwResetPeriod = 86400;
        fa.cActions = 3;
        fa.lpsaActions = act;
        if (!ChangeServiceConfig2W(sv, SERVICE_CONFIG_FAILURE_ACTIONS, &fa))
            sbl_log("service: recovery actions not set (%lu)", GetLastError());
        CloseServiceHandle(sv);
    }
    CloseServiceHandle(m);
}

/* ---- the session helper --------------------------------------------------------------------------- */

/* "exe" --session-helper --quit-event 0xH ARGS (ARGS = what followed --service, minus service options) */
static wchar_t *helper_cmdline(const wchar_t *exe, HANDLE quit)
{
    size_t cap = wcslen(exe) + 96;
    for (int i = 2; i < g_argc; i++) cap += wcslen(g_argv[i]) * 2 + 4;
    wchar_t *c = calloc(cap, sizeof *c);
    if (!c) return NULL;
    _snwprintf(c, cap - 1, L"\"%ls\" --session-helper --quit-event 0x%llx", exe, (unsigned long long)(uintptr_t)quit);
    for (int i = 2; i < g_argc; i++) {
        if (!wcscmp(g_argv[i], L"--keep-power-settings")) continue;
        size_t k = wcslen(c);
        if (wcschr(g_argv[i], L' ')) _snwprintf(c + k, cap - k - 1, L" \"%ls\"", g_argv[i]);
        else _snwprintf(c + k, cap - k - 1, L" %ls", g_argv[i]);
    }
    return c;
}

static HANDLE launch_helper(DWORD sid, HANDLE quit)
{
    HANDLE me = NULL, tok = NULL, proc = NULL;
    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return NULL;
    wcscpy(dir, exe);
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (sl) *sl = 0;
    wchar_t *cmd = helper_cmdline(exe, quit);
    if (!cmd) return NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &me) ||
        !DuplicateTokenEx(me, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &tok)) {
        sbl_log("service: token copy failed (%lu)", GetLastError());
        goto out;
    }
    if (!SetTokenInformation(tok, TokenSessionId, &sid, sizeof sid)) {
        sbl_log("service: TokenSessionId %lu failed (%lu)", sid, GetLastError());
        goto out;
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.lpDesktop = L"winsta0\\default";
    /* inherits only the quit event (the only inheritable handle this process has) */
    if (!CreateProcessAsUserW(tok, exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        sbl_log("service: CreateProcessAsUser in session %lu failed (%lu)", sid, GetLastError());
        goto out;
    }
    CloseHandle(pi.hThread);
    proc = pi.hProcess;
    sbl_log("service: helper pid %lu started in session %lu", pi.dwProcessId, sid);
out:
    if (tok) CloseHandle(tok);
    if (me) CloseHandle(me);
    free(cmd);
    return proc;
}

static void stop_helper(HANDLE *proc, HANDLE quit, const char *why)
{
    if (!*proc) return;
    sbl_log("service: stopping the helper (%s)", why);
    SetEvent(quit);
    if (WaitForSingleObject(*proc, 4000) != WAIT_OBJECT_0) {
        sbl_log("service: the helper did not exit: terminating it");
        TerminateProcess(*proc, 1);
        WaitForSingleObject(*proc, 2000);
    }
    CloseHandle(*proc);
    *proc = NULL;
    ResetEvent(quit);
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;
    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, ctrl, NULL);
    if (!g_ssh) return;
    set_state(SERVICE_START_PENDING, 3000);
    g_stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_session_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE quit = CreateEventW(&sa, TRUE, FALSE, NULL);   /* inherited by the helper */
    /* the helpers' "USB bus configured" marker: held here so it survives every helper (usbfn_win.c) */
    HANDLE usb_marker = CreateEventW(NULL, TRUE, FALSE, L"Global\\StarbiteLinkUsbConfigured");
    if (!usb_marker) sbl_log("service: USB marker event not created (%lu)", GetLastError());
    set_state(SERVICE_RUNNING, 0);

    enable_priv(L"SeTcbPrivilege");
    enable_priv(L"SeAssignPrimaryTokenPrivilege");
    enable_priv(L"SeIncreaseQuotaPrivilege");
    set_recovery();
    int keep_power = 0;
    for (int i = 2; i < g_argc; i++) if (!wcscmp(g_argv[i], L"--keep-power-settings")) keep_power = 1;
    if (!keep_power) power_never_sleep();

    HANDLE proc = NULL;
    DWORD proc_sid = 0xFFFFFFFFu;
    ULONGLONG started = 0, next_launch = 0;
    DWORD backoff = 1000;
    /* a helper killed with its session (0x40010004) or exiting with 4: that session is ending; skip it
     * for 20 s unless it is still active then (the shutdown was cancelled) or logs on again */
    DWORD soft_sid = 0xFFFFFFFFu;
    ULONGLONG soft_until = 0;
    DWORD wait_logged_sid = 0xFFFFFFFFu;
    int wait_logged_why = -1;
    for (;;) {
        DWORD sid = WTSGetActiveConsoleSessionId();
        ULONGLONG now = GetTickCount64();
        if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(proc, &code);
            CloseHandle(proc);
            proc = NULL;
            int ending = code == EXIT_SESSION_KILLED || code == EXIT_SESSION_ENDING;
            /* quick exits back off (1, 2, 4 .. 30 s); one that ran a while restarts at once */
            backoff = now - started < 10000 && !ending ? (backoff * 2 > 30000 ? 30000 : backoff * 2) : 1000;
            next_launch = now + (code == 3 ? 500 : backoff);   /* 3 = its watchdog asked for a restart */
            if (ending) {
                soft_sid = proc_sid;
                soft_until = now + 20000;
            }
            sbl_log("service: helper exited (code 0x%lx%s) after %llu s; %s", code,
                    code == EXIT_SESSION_KILLED ? ": killed by the end of its session" :
                    code == EXIT_SESSION_ENDING ? ": its session is ending" : code == 3 ? ": watchdog restart" : "",
                    (now - started) / 1000,
                    ending ? "no new helper in that session until it is active again or another one is on the console"
                           : "restarting it");
        }
        if (proc && sid != proc_sid) {
            stop_helper(&proc, quit, "the console session changed");
            next_launch = now;
        }
        if (!proc && sid != 0xFFFFFFFFu && now >= next_launch) {
            /* only into a session that is connected to the console and not being logged off */
            int st = session_state(sid);
            int why = -1;   /* -1 = go */
            if (sid == g_ending_sid) why = 0;
            else if (sid == soft_sid && now < soft_until) why = 1;
            else if (st >= 0 && st != WTSActive && st != WTSConnected) why = 2;   /* unreadable (-1): go anyway */
            if (why < 0 && sid == soft_sid) soft_sid = 0xFFFFFFFFu;
            if (why >= 0) {
                if (wait_logged_sid != sid || wait_logged_why != why) {
                    sbl_log("service: not starting a helper in session %lu yet: %s (state %s)", sid,
                            why == 0 ? "it is being logged off" : why == 1 ? "its last helper was ended with the session"
                                                                         : "not connected to the console",
                            st >= 0 ? state_name(st) : "?");
                    wait_logged_sid = sid;
                    wait_logged_why = why;
                }
            } else {
                wait_logged_sid = 0xFFFFFFFFu;
                wait_logged_why = -1;
                proc = launch_helper(sid, quit);
                proc_sid = sid;
                started = now;
                if (!proc) { backoff = backoff * 2 > 30000 ? 30000 : backoff * 2; next_launch = now + backoff; }
            }
        }
        HANDLE w[3] = { g_stop_ev, g_session_ev, proc };
        DWORD r = WaitForMultipleObjects(proc ? 3 : 2, w, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
    }
    stop_helper(&proc, quit, "service stopping");
    if (usb_marker) CloseHandle(usb_marker);
    sbl_log("service: stopped");
    set_state(SERVICE_STOPPED, 0);
}

int sbl_service_main(int argc, wchar_t **wargv)
{
    g_argc = argc;
    g_argv = wargv;
    CreateDirectoryW(L"C:\\starbite", NULL);
    CreateDirectoryW(L"C:\\starbite\\link", NULL);
    plat_log_open("C:\\starbite\\link\\sblinkd-service.log", 0);
    sbl_log("service: sblinkd build " SB_BUILD_ID " starting");
    SERVICE_TABLE_ENTRYW t[2] = { { (LPWSTR)SVC_NAME, service_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherW(t)) {
        DWORD e = GetLastError();
        sbl_log("service: StartServiceCtrlDispatcher failed (%lu)%s", e,
                e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? ": --service only works when started by the Service Control Manager" : "");
        return 1;
    }
    return 0;
}
