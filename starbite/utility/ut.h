/* SPDX-License-Identifier: GPL-2.0-only
 * Shared between the Utility OS's main.c and its tool pages (tools.c). */
#pragma once

void ut_service(void);          /* USB, the shell, the watchdog, the idle return: every loop turn */
void ut_status_bar(void);
void ut_measure(void);
void ut_everyday(void);
void ut_pad(void);
void ut_files(void);
