/* SPDX-License-Identifier: GPL-2.0-only
 * gwin-host: the kernel calls gwin.c makes, for the host harness */
#pragma once
#include <stdint.h>
int hw_m3_on(void);
/* v3: the wall clock for a Linux notification's time (kern.h / sdk/sbapp.h's, the fields gwin.c reads) */
struct sbos_time {
    int64_t utc;
    int32_t y, mo, d, h, mi, s, wday, dst;
    int32_t off_min;
    char abbr[8];
    char zone[24];
    int32_t approx;
};
int clock_now(struct sbos_time *t);
