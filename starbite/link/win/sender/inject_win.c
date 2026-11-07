/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd input injection on Windows: SendInput. Keys by scan code (the Windows layout picks the
 * character), Pause/media keys by virtual key, text by KEYEVENTF_UNICODE, the pointer absolute over the
 * virtual desktop (MOUSEEVENTF_VIRTUALDESK) so it lands on the captured output wherever it is.
 * Limits (Windows, by design): no input into elevated windows (UIPI) or the secure desktop (UAC, lock
 * screen) from a normal user process.
 */
#include "sender.h"

#include <string.h>

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
    while (n > 0) {
        int k = n < 64 ? n : 64;
        for (int i = 0; i < k; i++) to_input(&ev[i], &in[i]);
        UINT sent = SendInput((UINT)k, in, sizeof(INPUT));
        if ((int)sent != k && !warned) {
            warned = 1;
            sbl_log("input: SendInput injected %u of %d (%lu): blocked by UIPI or the secure desktop?", sent, k,
                    GetLastError());
        } else if ((int)sent == k) warned = 0;
        ev += k;
        n -= k;
    }
}
