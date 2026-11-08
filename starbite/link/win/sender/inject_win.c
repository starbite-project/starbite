/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd input injection on Windows: SendInput. Keys by scan code (the Windows layout picks the
 * character), Pause/media keys by virtual key, text by KEYEVENTF_UNICODE, the pointer absolute over the
 * virtual desktop (MOUSEEVENTF_VIRTUALDESK) so it lands on the captured output wherever it is.
 * Run as the service's session helper (SYSTEM, main.c / service_win.c) the input reaches every window:
 * elevated ones, Start, the quick settings flyout (UIPI only blocks input into a HIGHER integrity level)
 * and, because each thread follows the input desktop (sbl_follow_input_desktop), the secure desktop too
 * (UAC prompts, the lock and logon screen). Started from the Startup folder as the user, UIPI still
 * blocks elevated and UIAccess windows and the secure desktop cannot be opened.
 */
#include "sender.h"

#include <string.h>

/* injection statistics (sbl_inject_take_stats): updated by every injecting thread */
static SRWLOCK g_stlk = SRWLOCK_INIT;
static sbl_inj_stats g_st;

static void st_desk_fail(DWORD e)
{
    AcquireSRWLockExclusive(&g_stlk);
    g_st.desk_fails++;
    g_st.desk_err = e;
    ReleaseSRWLockExclusive(&g_stlk);
}

/* Desktop coordinates in physical pixels on this thread, whatever the process awareness ended up as
 * (round 7: helpers started in a session that was not yet connected got SetProcessDpiAwarenessContext
 * error 5 and stayed DPI unaware; GetSystemMetrics(SM_*VIRTUALSCREEN) is then in logical pixels while the
 * DXGI output rect is physical, and at 200% every click would land at twice its position). Per thread,
 * once. */
static void thread_dpi_aware(void)
{
    static __declspec(thread) int done;
    static volatile LONG logged;
    if (done) return;
    done = 1;
    DPI_AWARENESS a = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
    if (a == DPI_AWARENESS_PER_MONITOR_AWARE) return;
    if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        if (InterlockedExchange(&logged, 1) == 0)
            sbl_log("dpi: thread %lu is DPI awareness %d and SetThreadDpiAwarenessContext failed (%lu): pointer "
                    "positions may be scaled", GetCurrentThreadId(), (int)a, GetLastError());
        return;
    }
    if (InterlockedExchange(&logged, 1) == 0)
        sbl_log("dpi: the process is DPI awareness %d: thread %lu (and every other injecting/capturing thread) "
                "switched to per-monitor v2", (int)a, GetCurrentThreadId());
}

/* Puts the calling thread on the desktop that currently receives input (Default, or Winlogon for UAC /
 * the lock screen). SendInput only reaches the input desktop, and DXGI duplicates the desktop of the
 * thread that calls DuplicateOutput. Checked at most every 250 ms per thread unless force. Returns 1 when
 * the thread moved, 0 when it already was there, -1 when the input desktop cannot be opened (a normal
 * user process on the secure desktop) or the thread cannot move (it owns a window: the message thread).
 * Failures are logged whenever the error or the desktop changes, not just once. */
int sbl_follow_input_desktop(int force)
{
    static __declspec(thread) uint64_t checked;
    static __declspec(thread) HDESK owned;
    static __declspec(thread) DWORD failed_err;
    static __declspec(thread) WCHAR failed_on[64];
    thread_dpi_aware();
    uint64_t now = plat_now_us();
    if (!force && checked && now - checked < 250000u) return 0;
    checked = now;
    /* GENERIC_ALL first (SYSTEM gets it on Winlogon too); the rights SendInput and duplication need if not */
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!d) d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP |
                                            DESKTOP_JOURNALPLAYBACK | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE);
    if (!d) {
        DWORD e = GetLastError();
        if (e != failed_err || failed_on[0]) {
            sbl_log("desktop: thread %lu: the input desktop cannot be opened (%lu)%s", GetCurrentThreadId(), e,
                    e == ERROR_ACCESS_DENIED ? ": secure desktop as a normal user?" : "");
            failed_on[0] = 0;
        }
        failed_err = e;
        st_desk_fail(e);
        return -1;
    }
    WCHAR a[64] = L"", b[64] = L"";
    DWORD n = 0;
    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    GetUserObjectInformationW(d, UOI_NAME, b, sizeof b - sizeof(WCHAR), &n);
    if (cur && GetUserObjectInformationW(cur, UOI_NAME, a, sizeof a - sizeof(WCHAR), &n) && !wcscmp(a, b)) {
        CloseDesktop(d);
        failed_err = 0;
        return 0;
    }
    if (!SetThreadDesktop(d)) {
        DWORD e = GetLastError();
        if (e != failed_err || wcscmp(failed_on, b)) {
            sbl_log("desktop: thread %lu: SetThreadDesktop(\"%ls\") failed (%lu)%s: input stays on \"%ls\"",
                    GetCurrentThreadId(), b, e, e == ERROR_BUSY ? " (the thread owns a window)" : "", a);
            wcsncpy(failed_on, b, 63);
        }
        failed_err = e;
        st_desk_fail(e);
        CloseDesktop(d);
        return -1;
    }
    if (owned) CloseDesktop(owned);
    owned = d;
    failed_err = 0;
    failed_on[0] = 0;
    AcquireSRWLockExclusive(&g_stlk);
    g_st.desk_moves++;
    ReleaseSRWLockExclusive(&g_stlk);
    sbl_log("desktop: thread %lu now on the \"%ls\" desktop", GetCurrentThreadId(), b);
    return 1;
}

/* the name of the calling thread's desktop (UTF-8) */
static void thread_desk_name(char *out, size_t n)
{
    WCHAR a[64] = L"";
    DWORD k = 0;
    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    out[0] = 0;
    if (cur && GetUserObjectInformationW(cur, UOI_NAME, a, sizeof a - sizeof(WCHAR), &k))
        WideCharToMultiByte(CP_UTF8, 0, a, -1, out, (int)n, NULL, NULL);
}

void sbl_inject_geom(sbl_geom *g)
{
    thread_dpi_aware();
    g->virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g->virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g->virt_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g->virt_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void sbl_inject_take_stats(sbl_inj_stats *out)
{
    AcquireSRWLockExclusive(&g_stlk);
    *out = g_st;
    g_st.calls = g_st.events = g_st.injected = g_st.short_calls = g_st.first_err = g_st.last_err = 0;
    g_st.desk_moves = g_st.desk_fails = g_st.desk_err = 0;
    ReleaseSRWLockExclusive(&g_stlk);
}

static void to_input(const sbl_ev *e, INPUT *in)
{
    memset(in, 0, sizeof *in);
    int up = e->flags & SBL_EVF_UP, ext = e->flags & SBL_EVF_EXT;
    switch (e->kind) {
    case SBL_EV_KEY_SCAN:
        in->type = INPUT_KEYBOARD;
        in->ki.wScan = e->code;
        in->ki.dwFlags = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        break;
    case SBL_EV_KEY_VK:
        in->type = INPUT_KEYBOARD;
        in->ki.wVk = e->code;
        in->ki.wScan = (WORD)MapVirtualKeyW(e->code, MAPVK_VK_TO_VSC);
        in->ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        break;
    case SBL_EV_UNICODE:
        in->type = INPUT_KEYBOARD;
        in->ki.wScan = e->code;
        in->ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
        break;
    case SBL_EV_MOVE_ABS:
        in->type = INPUT_MOUSE;
        in->mi.dx = e->x;
        in->mi.dy = e->y;
        in->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        break;
    case SBL_EV_BUTTON:
        in->type = INPUT_MOUSE;
        switch (e->code) {
        case SBL_BTN_L: in->mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN; break;
        case SBL_BTN_R: in->mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN; break;
        case SBL_BTN_M: in->mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN; break;
        default:
            in->mi.dwFlags = up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
            in->mi.mouseData = e->code == SBL_BTN_X1 ? XBUTTON1 : XBUTTON2;
        }
        break;
    case SBL_EV_WHEEL:
        in->type = INPUT_MOUSE;
        in->mi.dwFlags = MOUSEEVENTF_WHEEL;
        in->mi.mouseData = (DWORD)e->data;   /* signed, + = away from the user */
        break;
    case SBL_EV_HWHEEL:
        in->type = INPUT_MOUSE;
        in->mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in->mi.mouseData = (DWORD)e->data;   /* + = right */
        break;
    }
}

int sbl_inject(const sbl_ev *ev, int n)
{
    static volatile LONG64 last_log_us;
    static __declspec(thread) uint64_t cur_at;
    INPUT in[64];
    int total = 0, mouse = 0;
    uint32_t calls = 0, asked = 0, shorts = 0;
    DWORD first_err = 0, last_err = 0;
    sbl_follow_input_desktop(0);
    while (n > 0) {
        int k = n < 64 ? n : 64;
        for (int i = 0; i < k; i++) {
            to_input(&ev[i], &in[i]);
            if (in[i].type == INPUT_MOUSE) mouse = 1;
        }
        SetLastError(0);
        UINT sent = SendInput((UINT)k, in, sizeof(INPUT));
        DWORD err = (int)sent == k ? 0 : GetLastError();   /* before any other call can overwrite it */
        calls++;
        asked += (uint32_t)k;
        if ((int)sent != k) {
            shorts++;
            if (!first_err) first_err = err ? err : 0xFFFFFFFFu;
            last_err = first_err == 0xFFFFFFFFu ? 0 : err;
            /* the input desktop may have just switched (UAC, lock): follow it and send the rest again */
            if (sbl_follow_input_desktop(1) == 1) {
                SetLastError(0);
                UINT more = SendInput((UINT)(k - (int)sent), in + sent, sizeof(INPUT));
                if ((int)(sent + more) != k) last_err = GetLastError();
                sent += more;
                calls++;
            }
        }
        total += (int)sent;
        ev += k;
        n -= k;
    }
    if (first_err == 0xFFFFFFFFu) first_err = 0;
    uint64_t now = plat_now_us();
    CURSORINFO ci;
    int have_cur = 0, mp = 0;
    if (mouse && now - cur_at > 1000000u) {   /* where did the moves land (also when the cursor is hidden) */
        cur_at = now;
        memset(&ci, 0, sizeof ci);
        ci.cbSize = sizeof ci;
        have_cur = GetCursorInfo(&ci) != 0;
        mp = GetSystemMetrics(SM_MOUSEPRESENT);
    }
    char desk[32];
    thread_desk_name(desk, sizeof desk);
    AcquireSRWLockExclusive(&g_stlk);
    g_st.calls += calls;
    g_st.events += asked;
    g_st.injected += (uint32_t)total;
    g_st.short_calls += shorts;
    if (shorts) {
        if (!g_st.first_err) g_st.first_err = first_err;
        g_st.last_err = last_err;
    }
    if (desk[0]) memcpy(g_st.desk, desk, sizeof g_st.desk);
    if (have_cur) {
        g_st.cur_valid = 1;
        g_st.cur_x = ci.ptScreenPos.x;
        g_st.cur_y = ci.ptScreenPos.y;
        g_st.cur_flags = ci.flags;
        g_st.mouse_present = mp;
    }
    ReleaseSRWLockExclusive(&g_stlk);
    /* at most one line per 10 s; the stats line counts every one */
    if (shorts && now - (uint64_t)last_log_us > 10000000u) {
        last_log_us = (LONG64)now;
        sbl_log("input: SendInput injected %d of %u events on the \"%s\" desktop (error %lu, after retry %lu): "
                "blocked by UIPI, the secure desktop or a desktop switch?", total, asked, desk, first_err, last_err);
    }
    return total;
}
