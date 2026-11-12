/* SPDX-License-Identifier: GPL-2.0-only
 * gwin-host: the few libsb calls gwin.c makes, for the host harness (gwin-host.c implements them) */
#pragma once
#include <stdint.h>
uint64_t sb_ms(void);
void sb_log(const char *s);
void sb_logf(const char *a, const char *b);
struct ub {
    char b[256];
    unsigned n;
};
void ub_init(struct ub *u);
void ub_s(struct ub *u, const char *s);
void ub_dec(struct ub *u, uint64_t v);
