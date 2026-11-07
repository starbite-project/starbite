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

/* Puts the calling thread on the desktop that currently receives input (Default, or Winlogon for UAC /
 * the lock screen). SendInput only reaches the input desktop, and DXGI duplicates the desktop of the
 * thread that calls DuplicateOutput. Checked at most every 250 ms per thread unless force. Returns 1 when
 * the thread moved, 0 when it already was there, -1 when the input desktop cannot be opened (a normal
 * user process on the secure desktop). Threads with windows cannot move: call it only from the capture
 * loop and the receive threads. */
int sbl_follow_input_desktop(int force)
{
    static __declspec(thread) uint64_t checked;
    static __declspec(thread) HDESK owned;
    static __declspec(thread) int failed_logged;
    uint64_t now = plat_now_us();
    if (!force && checked && now - checked < 250000u) return 0;
    checked = now;
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!d) {
        if (!failed_logged) sbl_log("desktop: the input desktop cannot be opened (%lu): secure desktop as a normal user?", GetLastError());
        failed_logged = 1;
        return -1;
    }
    failed_logged = 0;
    WCHAR a[64] = L"", b[64] = L"";
    DWORD n = 0;
    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    GetUserObjectInformationW(d, UOI_NAME, b, sizeof b - sizeof(WCHAR), &n);
    if (cur && GetUserObjectInformationW(cur, UOI_NAME, a, sizeof a - sizeof(WCHAR), &n) && !wcscmp(a, b)) {
        CloseDesktop(d);
        return 0;
    }
    if (!SetThreadDesktop(d)) {
        sbl_log("desktop: SetThreadDesktop(\"%ls\") failed (%lu)", b, GetLastError());
        CloseDesktop(d);
        return -1;
    }
    if (owned) CloseDesktop(owned);
    owned = d;
    sbl_log("desktop: thread %lu now on the \"%ls\" desktop", GetCurrentThreadId(), b);
    return 1;
}

void sbl_inject_geom(sbl_geom *g)
{
    g->virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g->virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g->virt_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g->virt_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
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

void sbl_inject(const sbl_ev *ev, int n)
{
    static int warned;
    INPUT in[64];
    sbl_follow_input_desktop(0);
    while (n > 0) {
        int k = n < 64 ? n : 64;
        for (int i = 0; i < k; i++) to_input(&ev[i], &in[i]);
        UINT sent = SendInput((UINT)k, in, sizeof(INPUT));
        /* the input desktop may have just switched (UAC, lock): follow it and send the rest again */
        if ((int)sent != k && sbl_follow_input_desktop(1) == 1)
            sent += SendInput((UINT)(k - (int)sent), in + sent, sizeof(INPUT));
        if ((int)sent != k && !warned) {
            warned = 1;
            sbl_log("input: SendInput injected %u of %d (%lu): blocked by UIPI or the secure desktop?", sent, k,
                    GetLastError());
        } else if ((int)sent == k) warned = 0;
        ev += k;
        n -= k;
    }
}
