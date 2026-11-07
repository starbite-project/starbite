/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS time zones (tz.c): local time from the RTC's UTC with the EU and US daylight-saving rules,
 * for the status bar and the world clock. Pure C (the host test checks it against zoneinfo). */
#pragma once
#include <stdint.h>

enum { TZ_FIXED, TZ_EU, TZ_US };

struct tz_zone {
    const char *city, *std, *dst;   /* "Helsinki", "EET", "EEST" */
    int16_t off_min;                /* standard offset from UTC, minutes */
    uint8_t rule;                   /* TZ_FIXED, TZ_EU, TZ_US */
};

extern const struct tz_zone tz_zones[];
extern const int tz_nzones;         /* tz_zones[0] is the home zone (the status bar) */

struct tz_time {
    int y, mo, d, h, mi, s, wday;   /* wday 0 = Sunday */
    int dst;
    const char *abbr;
};

/* epoch seconds (UTC) -> civil date and time */
void tz_civil(int64_t t, struct tz_time *out);
/* days since 1970-01-01 of a civil date */
int64_t tz_days(int y, int mo, int d);
/* the local time in zone z at UTC epoch t */
void tz_local(const struct tz_zone *z, int64_t t, struct tz_time *out);
