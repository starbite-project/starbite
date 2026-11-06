/* SPDX-License-Identifier: GPL-2.0-only
 * Fastboot in the background while a loader menu waits for keys (fastboot_dev.c,
 * docs/USB-FASTBOOT.md section 11). No struct tb here: the menus include it.
 */
#pragma once
#include <stdint.h>

/* ---- fastboot in the background while a menu waits for keys (fastboot_dev.c) ----------------
 * The boot menu (v7), Starbite Recovery's menu and the crash report keep the same USB device
 * (fastboot + console) up and serve it from their key loops, so the Mac can drive the phone
 * there: getvar, oem log, oem boot-next / reboot-to / maint, reboot, `fastboot boot`, and
 * `oem key` (a press for the menu on screen) and `oem screen` (the panel).
 *   fbbg_enable  the loader wants it from now on (xnu_boot.c, after the catalog; recovery.c); the
 *                first fbbg_poll brings USB up (about 0.1 s; a failure is logged and not retried)
 *   fbbg_poll    from the key loops, often (every frame, every 10 ms): services the controller for
 *                at most about 2 ms, restarts the link on a replug, reboots when asked (the loops
 *                are safe points). FBBG_BOOT once an image (or Starbite Maintenance) is placed:
 *                the menu leaves at once and the loader returns fbbg_entry() (it stays FBBG_BOOT
 *                until then, so every loop on the way out sees it)
 *   fbbg_entry   the last responses out (at most 0.5 s), USB stopped, the entry
 *   fbbg_stop    USB stopped; fbbg_poll starts it again later (the explicit fastboot mode, Lab)
 *   fbbg_off     USB stopped for good: before any OS starts (star_prepare_xnu, every return)
 *   fbbg_inject  the injected key bits now (menu.c keys(), recovery.c r_keys() add them) */
#define FBBG_MENU     1U
#define FBBG_RECOVERY 2U
#define FBBG_CRASH    3U
#define FBBG_NONE 0
#define FBBG_BOOT 1
void fbbg_enable(void);
int fbbg_poll(unsigned where);
int fbbg_pending(void);
uintptr_t fbbg_entry(void);
void fbbg_stop(void);
void fbbg_off(void);
unsigned fbbg_inject(void);
