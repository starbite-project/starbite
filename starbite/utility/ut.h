/* SPDX-License-Identifier: GPL-2.0-only
 * Shared between the Utility OS's main.c and its tool pages (tools.c). */
#pragma once

/* a page that must stay visible (a running timer, Measure): no screen saver and no idle return */
extern int ut_awake;
void ut_service(void);          /* USB, the shell, the watchdog, the idle return: every loop turn */
void ut_status_bar(void);
void ut_measure(void);
void ut_everyday(void);
void ut_pad(void);
void ut_files(void);
struct exfat;
struct exfat *ut_card(void);           /* the card's exFAT (tools.c), 0 if not readable */
