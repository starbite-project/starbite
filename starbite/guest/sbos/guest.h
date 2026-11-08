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

enum { GUEST_OFF = 0, GUEST_STARTING, GUEST_RUNNING, GUEST_STOPPING, GUEST_FAILED };

void guest_init(void);                     /* once, after hw_irq_init (routes the host doorbell) */
int guest_available(void);                 /* 1: the three store entries exist */
int guest_start(void);                     /* 0 started, < 0 refused (heater busy, no files, a load error) */
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
void guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0);   /* 720x1440 at 2x */
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
    const uint32_t *(*panel)(void);         /* the composed frame (UI_W x UI_H), for the tap's pixel */
};
int guest_selftest_step(const struct guest_st_ops *o);
