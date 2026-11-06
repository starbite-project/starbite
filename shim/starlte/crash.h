/* SPDX-License-Identifier: GPL-2.0-only
 * Crash and hang detection below every OS (crash.c, docs/STARBITE-HEALTH.md sections 4-6).
 *
 * Before each OS starts, the loader writes a launch record into the store's crash log (which
 * entry, when, watchdog). At the next start it compares that with why the SoC reset (PMU
 * RST_STAT, INFORM3, S-Boot's own reset code from its command line) and what the OS left in RAM
 * (ramoops at 0xFED10000, Samsung's sec_debug page at 0x80000000, XNU's panic dump in the pmsg
 * ring). A panic, a watchdog reset, a forced restart or an unexplained reset becomes a report in
 * the crash log (a ring of 15) and a crash screen with three levels of detail; then the menu, or
 * what the settings say. The screen times out; nothing waits for ever.
 */
#pragma once
#include <stdint.h>

/* Right after the Bixby window, before anything writes the ramoops area: copies 0xFED10000 (32 KiB)
 * and 0x80000000 (4 KiB) to RAM and reads the PMU and watchdog registers and S-Boot's bootargs.
 * Reads and one RAM copy only. fdt: S-Boot's. */
void crash_snapshot(uintptr_t fdt);

/* After POST and the catalog: evaluate the last launch (once per boot), save a report, show the
 * crash screen. Returns what to do next. */
#define CRASH_GO_MENU     0
#define CRASH_GO_RECOVERY 1
#define CRASH_GO_RETRY    2      /* *retry = the catalog index to start again */
int crash_after(int *retry);
/* 1 once this boot found a crash: the menus start no auto-start countdown. */
int crash_hold_autoboot(void);

/* The launch record, the last thing before an OS starts (xnu_boot.c launch_go). method: catalog
 * M_*; lflags: CLF_* (sbfmt.h). A failed write is logged and ignored. */
void crash_launch(const char *key, const char *name, unsigned method, unsigned wdt_s, unsigned wdt_armed,
                  unsigned log_level, unsigned lflags);

/* The settings record changed (Starbite Recovery's settings page): read it again before use. */
void crash_reload_settings(void);

/* Starbite Recovery's "Crash reports": the saved reports, newest first; one opens on the crash
 * screen. Blocks until Back. */
void crash_browse(void);

/* For `fastboot oem log crash` (text, every report at level 3) and `crashraw` (the 64 KiB entry,
 * for `sbstore2.py crashlog`). Bytes written, 0 if there is no crash log. */
unsigned crash_text(char *out, unsigned cap);
unsigned crash_raw(char *out, unsigned cap);
