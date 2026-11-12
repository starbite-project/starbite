/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.9 / P3: the guest service (kern/guest.c), Linux apps on the M3 cluster.
 * A drop-in for the starbite-os session (starbite/guest/sbos/INTEGRATION.md lists every hook); built on
 * starbite/guest/host/sbghost (the same core the phone payloads P2..P2d ran).
 */
#pragma once
#include <stdint.h>

#define KT_GUEST_ID_POLL 0U        /* the owner's timer ids (owner KT_GUEST) */
#define KT_GUEST_ID_IDLE 1U

/* the store entries (made by starbite/guest/probe/build.sh bundle) */
#define GUEST_STORE_IMAGE "guest-linux.img"
#define GUEST_STORE_DTB   "guest-linux.dtb"
#define GUEST_STORE_RD    "guest-linux-rd.img"
/* 1.2-alpha3: the terminal (the guest kernel of its own, a shell on fbcon with the Terminus 16 x 32 font; its
 * surface sized for 2x above the keyboard: 720 x 1060 = 1440 x 2120 on the panel, 45 x 33 characters) */
#define GUEST_TERM_IMAGE  "guest-term.img"
#define GUEST_TERM_DTB    "guest-term.dtb"
#define GUEST_TERM_RD     "guest-term-rd.img"
#define GUEST_TERM_W 720U
#define GUEST_TERM_H 1060U
/* L2 + L3 (docs/STARBITE-LINUX-APPS.md): Linux apps for Starbite OS Next: the guest kernel of its own with sbwl (our
 * Wayland compositor), the apps' catalog and the App Store (contract v2); the window between Next's status bar and
 * the home bar: 720 x 1344 = 1440 x 2688 at 2x (the guest's viewport 916 rows with the keyboard up) */
#define GUEST_WL_IMAGE    "guest-wl.img"
#define GUEST_WL_DTB      "guest-wl.dtb"
#define GUEST_WL_RD       "guest-wl-rd.img"
#define GUEST_WL_W 720U
#define GUEST_WL_H 1344U
enum { GUEST_APP = 0, GUEST_TERM = 1, GUEST_WL = 2 };

enum { GUEST_OFF = 0, GUEST_STARTING, GUEST_RUNNING, GUEST_STOPPING, GUEST_FAILED };

void guest_init(void);                     /* once, after hw_irq_init (routes the host doorbell) */
int guest_available(void);                 /* 1: the three store entries exist */
int guest_start(void);                     /* 0 started, < 0 refused (heater busy, no files, a load error) */
int guest_start_mode(int mode);            /* GUEST_APP (= guest_start), GUEST_TERM or GUEST_WL */
int guest_available_mode(int mode);
int guest_mode(void);                      /* the last started */
int guest_scale(void);                     /* the view's scale: 2 (both; the blit also does 4) */
int guest_type(char c);                    /* one character as Linux keys (Shift around it): 1 sent */
int guest_term_ok(void);                   /* the terminal's typed self-test command ran */
void guest_full_next(void);                /* the next guest_blit copies every row (the view redrawn whole) */
int guest_stop(void);                      /* MSG_STOP; the cluster goes off by itself */
int guest_state(void);                     /* GUEST_* */
const char *guest_why(void);               /* GUEST_FAILED: the reason */
int guest_owns_m3(void);                   /* 1 from the start until the cluster is off: the heater must not start */

void guest_timer(unsigned id);             /* main.c dispatch_timer, owner KT_GUEST */
void guest_irq(void);                      /* hw.c irq_drain: INTID GUEST_DB_HOST */
unsigned guest_db_host(void);              /* the INTID irq_drain must not treat as a stray */

/* the Linux view (ui.c): shown full screen between Starbite's bars, input routed to the guest */
void guest_show(int on);                   /* on: MSG_FOCUS 1; off: MSG_FOCUS 0 (stopped after 60 s hidden) */
int guest_shown(void);
int guest_frame_new(void);                 /* 1 when the guest published a frame since the last blit */
/* the rows newer than the frame drawn (damage per row) at 2x / 4x: the rows copied; *py0..*py1 the panel rows */
int guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0, int *py1);
/* L3: the same at 2x into a row table (rows[y]: the target's row y, 2 surf_w px; 2 surf_h rows), *py0..*py1 rows */
int guest_blit_rows(uint32_t *const *rows, int *py0, int *py1);
/* L3: 1 while something of Linux runs without a window shown (the App Store open, a package being installed):
 * no idle stop; 0: the 60 s count starts again (while hidden) */
void guest_keep(int on);
/* v3 (Track E): n Linux apps with a switcher card in the shell: while n > 0 Linux is not stopped for being hidden, and
 * only after GUEST_APPS_SCREEN_OFF_MS of the screen off (not 60 s); n = 0: the 60 s rules again */
void guest_apps(int n);
#define GUEST_APPS_SCREEN_OFF_MS (30U * 60U * 1000U)
/* v3 + Track A's zero-copy window: the guest's front buffer now (NULL: not shown / not running), *idx its index for
 * sbg_surf_reading, *stride_px its row pitch in px; the frame counts as drawn (guest_frame_new 0 until the next) */
const volatile uint32_t *guest_front(int *idx, unsigned *stride_px);
int guest_input(int kin_type, int x, int y);  /* KIN_PRESS/MOVE/RELEASE in panel pixels: 1 taken */
int guest_key(unsigned linux_code, int down);

/* services.c */
void guest_screen(int on);                 /* the screen state machine */
void guest_power_off(void);                /* before a switch-off / restart: stop, wait up to 1 s */

/* 1.2-alpha1 (starbite-os): the unattended check, shared by the phone's self-test image (uitest.c) and the QEMU
 * run of this file (starbite/guest/sbsim/kern-shim.c). Call about every 100 ms: 0 still running, 1 passed,
 * -1 failed (logged). Two cycles: start shown, a tap that must land in the app's pixels, hidden, stop, the
 * cluster off; then a second start in the background and stop (the restart after a real cluster-off). */
struct guest_st_ops {
    void (*open)(void);                     /* the Linux view: guest_start() + the view shown */
    void (*home)(void);                     /* back home: the view hidden (the guest keeps running) */
    void (*open_term)(void);                /* the terminal's view: guest_start_mode(GUEST_TERM) + shown */
    const uint32_t *(*panel)(void);         /* the composed frame (UI_W x UI_H), for the tap's pixel */
};
int guest_selftest_step(const struct guest_st_ops *o);
/* (1.2-alpha3: cycle 2 is the terminal: a command typed as keys, its kmsg marker in the log, text on the view) */
