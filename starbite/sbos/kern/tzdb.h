/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS time zones (Track A, 2026-11-12): the zone table behind Settings > Date & Time and the shell's
 * sh_tz_* (ui/shell.h). Pure integer C on the Utility OS's civil dates (utility/tz.c's tz_days / tz_civil), so the
 * host test (kern/host/tzdb-test.c) and the host shell link it as it is. utility/tz.c and its 7 zones stay as they
 * were for the Utility OS and the classic Clock app.
 *
 * The rules (those in force in 2026; older years are not modelled):
 *   TZR_EU  last Sunday of March 01:00 UTC .. last Sunday of October 01:00 UTC
 *   TZR_US  second Sunday of March 02:00 local standard .. first Sunday of November 02:00 local daylight
 *   TZR_AU  first Sunday of October 02:00 local standard .. first Sunday of April 03:00 local daylight (the south)
 *   TZR_NZ  last Sunday of September 02:00 local standard .. first Sunday of April 03:00 local daylight
 * Daylight time is always the standard offset + 60 minutes. */
#pragma once
#include <stdint.h>
#include "tz.h"

enum { TZR_NONE, TZR_EU, TZR_US, TZR_AU, TZR_NZ };
enum { TZG_AMERICAS, TZG_EUROPE, TZG_AFRICA, TZG_ASIA, TZG_PACIFIC, TZG_N };

struct tzdb_zone {
    const char *name, *city;        /* "Europe/Helsinki", "Helsinki" */
    const char *std, *dst;          /* "EET", "EEST" */
    int16_t off_min;                /* standard offset from UTC, minutes */
    uint8_t rule, group;            /* TZR_*, TZG_* (the list's region tabs) */
};

extern const struct tzdb_zone tzdb[];
extern const int tzdb_n;
extern const char *const tzdb_group_name[TZG_N];

int tzdb_home(void);                                /* the default zone (Helsinki) */
int tzdb_in_dst(const struct tzdb_zone *z, int64_t utc);
int tzdb_offset(int i, int64_t utc);               /* minutes east of UTC at that instant, DST included */
void tzdb_local(int i, int64_t utc, struct tz_time *out);
/* a stable 30-bit id from the IANA name (the saved setting: the table can be reordered), and back (-1 none) */
uint32_t tzdb_id(int i);
int tzdb_by_id(uint32_t id);
