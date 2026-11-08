/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SBM_SB_H
#define SBM_SB_H
#include "../sbm.h"
/* Fills p for a libsb image; line() gets every (already redacted) log line. */
void sbm_sb_plat(struct sbm_plat *p, void (*line)(const char *));
/* SErrors that were already pending before a probe read (not caused by it). */
unsigned sbm_sb_stale_serrors(void);
#endif
