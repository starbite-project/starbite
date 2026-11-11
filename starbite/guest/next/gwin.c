/* SPDX-License-Identifier: GPL-2.0-only
 * gwin: Linux apps in Starbite OS Next (docs/STARBITE-LINUX-APPS.md, milestone L2). The kernel's side of the shell's
 * sh_linux_* calls (ui/shell.h): the Terminal app's window is the guest's surface (foot under sbwl, the guest kernel
 * of its own, kern/guest.c's GUEST_WL mode: guest-wl.img / .dtb / -rd.img in the store, 720 x 916 at 2x), copied
 * row by row (damage per row) into the shell's window bitmap; the window's touches and the keyboard's characters go
 * to the guest. Built into the Next images only (sbos/build.sh); INTEGRATION-NEXT.md lists the hooks.
 *   gwin_now()            kern_next.c's next_kdraw, every loop turn: 1 when the window changed (draw a frame)
 *   gwin_selftest_step()  uitest.c's SBOS_SELFTEST_NEXT_LINUX variant, every 100 ms: 0 running, 1 passed, -1 failed
 */
#include "sb.h"
#include "kern.h"
#include "guest.h"
#include "shell.h"
#include "../host/sbghost.h"

static int last_state = -1;
static unsigned frames;                    /* the window's frames (rows copied) since the start */
static uint32_t *win_px;                   /* the shell's bitmap of the last copy (the self test reads it) */
static int win_stride;

static void push_state(void)
{
    int s = guest_state();
    if (s == last_state)
        return;
    last_state = s;
    sh_linux_state(s, s == GUEST_FAILED ? guest_why() : "");
}

int sh_linux_open(void)
{
    int s = guest_state();
    if (s == GUEST_RUNNING || s == GUEST_STARTING) {
        if (guest_mode() != GUEST_WL) {    /* another guest (a self test of the old UI): not this window's */
            sh_linux_state(GUEST_FAILED, "Linux is busy with another guest.");
            return -1;
        }
    } else if (s == GUEST_STOPPING) {
        push_state();                      /* "Linux Is Stopping": the next open starts it again */
        return -1;
    } else {
        int r = guest_start_mode(GUEST_WL);
        if (r < 0) {
            last_state = GUEST_FAILED;
            sh_linux_state(GUEST_FAILED, guest_why());
            sb_logf("gwin: the start was refused: ", guest_why());
            return r;
        }
        frames = 0;
        sb_log("gwin: Linux started for the Terminal (foot on the M3 cores)");
    }
    guest_show(1);                         /* MSG_FOCUS 1; the guest's idle stop cancelled */
    guest_full_next();
    push_state();
    return 0;
}

void sh_linux_hide(void)
{
    guest_show(0);                         /* MSG_FOCUS 0; guest.c stops it after 60 s hidden */
    win_px = 0;
}

int sh_linux_blit(uint32_t *px, int stride_px, int first, int *y0, int *y1)
{
    *y0 = *y1 = -1;
    if (first)
        guest_full_next();
    if (!first && !guest_frame_new())
        return 0;
    int n = guest_blit(px, (unsigned)stride_px, 0, 0, y0, y1);   /* 2x; alpha forced opaque */
    if (n > 0) {
        frames++;
        win_px = px;
        win_stride = stride_px;
    }
    return n;
}

int sh_linux_touch(int phase, int x, int y)
{
    if (!guest_shown())
        return 0;
    uint32_t p = phase == 0 ? SBG_TOUCH_DOWN : phase == 1 ? SBG_TOUCH_MOVE : SBG_TOUCH_UP;
    return sbg_touch(0, p, x, y);
}

int sh_linux_char(char c)
{
    return guest_type(c);                  /* the Linux key, with Shift around it ('\n' Return, '\b' BackSpace) */
}

/* every loop turn (next_kdraw), at most every 16 ms: a state change or a new frame copied into the window */
int gwin_now(void)
{
    static uint64_t t;
    uint64_t now = sb_ms();
    if (now - t < 16)
        return 0;
    t = now;
    int was = last_state;
    push_state();
    int changed = was != last_state;
    if (guest_frame_new() && sh_linux_frame())
        changed = 1;
    return changed;
}

/* ---- the unattended check (starbite-next-linuxtest.img: uitest.c -DSBOS_SELFTEST_NEXT_LINUX) -------------------
 * The Terminal opened through the shell (sh_set "app"), Linux started from the store, its first frame in the window;
 * a command typed through the shell's keyboard path (four characters a step); its kmsg marker in the guest's log
 * and text in the window's bitmap; a tap in the window; the Terminal closed, Linux stopped, the M3 cluster off.
 * Never a storage write. The caller restarts into recovery (pmsg) after the result. */
static int gs_phase, gs_typed;
static uint64_t gs_t, gs_t0;
static unsigned gs_frames;

static void gs_log(const char *what, uint64_t v, int with_v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: next-linux: ");
    ub_s(&u, what);
    if (with_v)
        ub_dec(&u, v);
    sb_log(u.b);
}
static int gs_failed(const char *why)
{
    gs_log("FAILED: ", 0, 0);
    sb_logf("selftest: next-linux: why: ", why);
    if (guest_state() == GUEST_RUNNING || guest_state() == GUEST_STARTING)
        guest_stop();                      /* the cluster off before the restart (guest_power_off waits too) */
    gs_phase = 99;
    return -1;
}
/* rows of the window's bitmap with anything other than its first pixel (the terminal's background) */
static unsigned lit_rows(void)
{
    if (!win_px)
        return 0;
    uint32_t bg = win_px[0];
    unsigned n = 0;
    for (int y = 0; y < 2 * SH_LINUX_H; y += 4) {
        const uint32_t *r = win_px + (uintptr_t)y * (unsigned)win_stride;
        for (int x = 0; x < 2 * SH_LINUX_W; x += 2)
            if (r[x] != bg) {
                n++;
                break;
            }
    }
    return n;
}

int gwin_selftest_step(void)
{
    uint64_t now = sb_ms(), dt = now - gs_t;
    int st = guest_state();
    switch (gs_phase) {
    case 0:
        gs_t0 = now;
        if (!guest_available_mode(GUEST_WL))
            return gs_failed("the Linux apps' files are not in the store (guest-wl.img / .dtb / -rd.img)");
        sh_set("app", "terminal", 1);      /* as a tap on the icon: app_open -> sh_linux_open */
        if (guest_state() != GUEST_STARTING && guest_state() != GUEST_RUNNING)
            return gs_failed(guest_why()[0] ? guest_why() : "the Terminal did not start Linux");
        gs_log("the Terminal opened; Linux starting (guest-wl from the store)", 0, 0);
        gs_t = now;
        gs_phase = 1;
        return 0;
    case 1:                                /* Linux up and its first frame in the window */
        if (st == GUEST_FAILED)
            return gs_failed(guest_why());
        if (st == GUEST_RUNNING && frames > 0 && win_px) {
            gs_log("Linux running, the window's first frame after ms ", dt, 1);
            gs_t = now;
            gs_phase = 2;
        } else if (dt > 40000) {
            return gs_failed(st == GUEST_RUNNING ? "Linux runs but the window got no frame within 40 s" :
                                                   "Linux did not come up within 40 s");
        }
        return 0;
    case 2:                                /* the shell's prompt first */
        if (st != GUEST_RUNNING)
            return gs_failed("Linux stopped by itself");
        if (dt >= 2000) {
            gs_typed = 0;
            gs_phase = 3;
        }
        return 0;
    case 3: {                              /* the command through the keyboard's path, four characters a step */
        static const char cmd[] = "uname -a; echo STARBITE-TERM-OK > /dev/kmsg\n";
        for (int i = 0; i < 4 && cmd[gs_typed]; ++i, ++gs_typed)
            if (!sh_linux_char(cmd[gs_typed]))
                return gs_failed("a character could not be typed (no key, or the ring full)");
        if (!cmd[gs_typed]) {
            gs_log("typed: uname -a; echo STARBITE-TERM-OK > /dev/kmsg", 0, 0);
            gs_t = now;
            gs_frames = frames;
            gs_phase = 4;
        }
        return 0;
    }
    case 4:                                /* the marker in the guest's log, the window redrawn, text in it */
        if (st != GUEST_RUNNING)
            return gs_failed("Linux stopped by itself");
        /* the window redrew since the typing and has nothing pending (a quiet terminal publishes only on change),
         * or kept redrawing a guest that keeps publishing */
        if (guest_term_ok() && frames != gs_frames && dt >= 500 && (!guest_frame_new() || frames - gs_frames >= 3)) {
            unsigned lit = lit_rows();
            gs_log("the command ran (STARBITE-TERM-OK in the guest's log) after ms ", dt, 1);
            gs_log("the window's frames since the typing: ", frames - gs_frames, 1);
            gs_log("rows of the window with text (1 in 4): ", lit, 1);
            if (lit < 4)
                return gs_failed("the window shows no text");
            uint32_t sent = sbg_stats()->touches_sent;
            sh_linux_touch(0, SH_LINUX_W / 2, SH_LINUX_H / 2);
            sh_linux_touch(2, SH_LINUX_W / 2, SH_LINUX_H / 2);
            gs_log("a tap in the window: touch messages sent ", sbg_stats()->touches_sent - sent, 1);
            if (sbg_stats()->touches_sent - sent < 2)
                return gs_failed("the tap did not reach the guest's ring");
            gs_t = now;
            gs_phase = 5;
        } else if (dt > 15000) {
            gs_log("the marker: ", (uint64_t)guest_term_ok(), 1);
            gs_log("window frames since the typing: ", frames - gs_frames, 1);
            return gs_failed(guest_term_ok() ? "the window did not redraw after the command within 15 s" :
                                               "STARBITE-TERM-OK did not reach the guest's log within 15 s");
        }
        return 0;
    case 5:                                /* the tap's events delivered, then the Terminal closed, Linux stopped */
        if (dt < 1000)
            return 0;
        sh_set("app", "", 0);              /* app_finish_close -> sh_linux_hide */
        if (guest_shown())
            return gs_failed("the window is still shown after the Terminal closed");
        guest_stop();
        gs_t = now;
        gs_phase = 6;
        return 0;
    case 6:
        if (st == GUEST_OFF) {
            gs_log("off; M3 cluster powered: ", (uint64_t)hw_m3_on(), 1);
            if (hw_m3_on() || guest_owns_m3())
                return gs_failed("the M3 cluster still powered after the stop");
            gs_log("passed: the Terminal's window with foot, a typed command, a tap, the cluster off; seconds ",
                   (now - gs_t0) / 1000U, 1);
            gs_phase = 98;
            return 1;
        }
        if (st == GUEST_FAILED)
            return gs_failed(guest_why());
        if (dt > 8000)
            return gs_failed("Linux did not power off within 8 s of MSG_STOP");
        return 0;
    case 98:
        return 1;
    default:
        return -1;
    }
}
