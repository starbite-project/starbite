/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS time zones (tzdb.h): the table and the DST rules. Pure integer code: no libc, no kernel calls.
 * The offsets and rules are IANA's as of 2026f (Mexico: no DST since 2022; Brazil since 2019; Fiji and Iran since
 * 2022; British Columbia: UTC-7 all year since March 2026). Zones with rules of their own (Chile, Paraguay, Israel,
 * Egypt, Morocco, Lord Howe) are left out rather than shown wrong. Within each region the list goes west to east.
 * Checked against zoneinfo by kern/host/tzdb-test.c (every zone; every transition of 2026 and 2027). */
#include "tzdb.h"

const char *const tzdb_group_name[TZG_N] = { "Americas", "Europe", "Africa", "Asia", "Pacific" };

#define A TZG_AMERICAS
#define E TZG_EUROPE
#define F TZG_AFRICA
#define S TZG_ASIA
#define P TZG_PACIFIC
const struct tzdb_zone tzdb[] = {
    /* the Americas */
    { "America/Anchorage", "Anchorage", "AKST", "AKDT", -540, TZR_US, A },
    { "America/Los_Angeles", "Los Angeles", "PST", "PDT", -480, TZR_US, A },
    { "America/Vancouver", "Vancouver", "MST", "MST", -420, TZR_NONE, A },   /* B.C.: UTC-7 all year since 2026-03-08 */
    { "America/Phoenix", "Phoenix", "MST", "MST", -420, TZR_NONE, A },
    { "America/Denver", "Denver", "MST", "MDT", -420, TZR_US, A },
    { "America/Chicago", "Chicago", "CST", "CDT", -360, TZR_US, A },
    { "America/Mexico_City", "Mexico City", "CST", "CST", -360, TZR_NONE, A },
    { "America/New_York", "New York", "EST", "EDT", -300, TZR_US, A },
    { "America/Toronto", "Toronto", "EST", "EDT", -300, TZR_US, A },
    { "America/Bogota", "Bogot\xc3\xa1", "-05", "-05", -300, TZR_NONE, A },
    { "America/Lima", "Lima", "-05", "-05", -300, TZR_NONE, A },
    { "America/Caracas", "Caracas", "-04", "-04", -240, TZR_NONE, A },
    { "America/Halifax", "Halifax", "AST", "ADT", -240, TZR_US, A },
    { "America/St_Johns", "St. John's", "NST", "NDT", -210, TZR_US, A },
    { "America/Sao_Paulo", "S\xc3\xa3o Paulo", "-03", "-03", -180, TZR_NONE, A },
    { "America/Argentina/Buenos_Aires", "Buenos Aires", "-03", "-03", -180, TZR_NONE, A },
    /* Europe */
    { "Etc/UTC", "UTC", "UTC", "UTC", 0, TZR_NONE, E },
    { "Europe/Lisbon", "Lisbon", "WET", "WEST", 0, TZR_EU, E },
    { "Europe/London", "London", "GMT", "BST", 0, TZR_EU, E },
    { "Europe/Dublin", "Dublin", "GMT", "IST", 0, TZR_EU, E },
    { "Europe/Madrid", "Madrid", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Paris", "Paris", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Amsterdam", "Amsterdam", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Berlin", "Berlin", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Rome", "Rome", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Stockholm", "Stockholm", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Warsaw", "Warsaw", "CET", "CEST", 60, TZR_EU, E },
    { "Europe/Athens", "Athens", "EET", "EEST", 120, TZR_EU, E },
    { "Europe/Helsinki", "Helsinki", "EET", "EEST", 120, TZR_EU, E },
    { "Europe/Kyiv", "Kyiv", "EET", "EEST", 120, TZR_EU, E },
    { "Europe/Istanbul", "Istanbul", "+03", "+03", 180, TZR_NONE, E },
    { "Europe/Moscow", "Moscow", "MSK", "MSK", 180, TZR_NONE, E },
    /* Africa */
    { "Africa/Dakar", "Dakar", "GMT", "GMT", 0, TZR_NONE, F },
    { "Africa/Accra", "Accra", "GMT", "GMT", 0, TZR_NONE, F },
    { "Africa/Lagos", "Lagos", "WAT", "WAT", 60, TZR_NONE, F },
    { "Africa/Kinshasa", "Kinshasa", "WAT", "WAT", 60, TZR_NONE, F },
    { "Africa/Algiers", "Algiers", "CET", "CET", 60, TZR_NONE, F },
    { "Africa/Johannesburg", "Johannesburg", "SAST", "SAST", 120, TZR_NONE, F },
    { "Africa/Maputo", "Maputo", "CAT", "CAT", 120, TZR_NONE, F },
    { "Africa/Nairobi", "Nairobi", "EAT", "EAT", 180, TZR_NONE, F },
    { "Africa/Addis_Ababa", "Addis Ababa", "EAT", "EAT", 180, TZR_NONE, F },
    /* Asia */
    { "Asia/Riyadh", "Riyadh", "+03", "+03", 180, TZR_NONE, S },
    { "Asia/Tehran", "Tehran", "+0330", "+0330", 210, TZR_NONE, S },
    { "Asia/Dubai", "Dubai", "+04", "+04", 240, TZR_NONE, S },
    { "Asia/Karachi", "Karachi", "PKT", "PKT", 300, TZR_NONE, S },
    { "Asia/Kolkata", "Kolkata", "IST", "IST", 330, TZR_NONE, S },
    { "Asia/Kathmandu", "Kathmandu", "+0545", "+0545", 345, TZR_NONE, S },
    { "Asia/Dhaka", "Dhaka", "+06", "+06", 360, TZR_NONE, S },
    { "Asia/Bangkok", "Bangkok", "+07", "+07", 420, TZR_NONE, S },
    { "Asia/Jakarta", "Jakarta", "WIB", "WIB", 420, TZR_NONE, S },
    { "Asia/Singapore", "Singapore", "+08", "+08", 480, TZR_NONE, S },
    { "Asia/Shanghai", "Shanghai", "CST", "CST", 480, TZR_NONE, S },
    { "Asia/Hong_Kong", "Hong Kong", "HKT", "HKT", 480, TZR_NONE, S },
    { "Asia/Taipei", "Taipei", "CST", "CST", 480, TZR_NONE, S },
    { "Asia/Manila", "Manila", "PST", "PST", 480, TZR_NONE, S },
    { "Asia/Seoul", "Seoul", "KST", "KST", 540, TZR_NONE, S },
    { "Asia/Tokyo", "Tokyo", "JST", "JST", 540, TZR_NONE, S },
    /* the Pacific and Australia */
    { "Pacific/Honolulu", "Honolulu", "HST", "HST", -600, TZR_NONE, P },
    { "Australia/Perth", "Perth", "AWST", "AWST", 480, TZR_NONE, P },
    { "Australia/Darwin", "Darwin", "ACST", "ACST", 570, TZR_NONE, P },
    { "Australia/Adelaide", "Adelaide", "ACST", "ACDT", 570, TZR_AU, P },
    { "Australia/Brisbane", "Brisbane", "AEST", "AEST", 600, TZR_NONE, P },
    { "Australia/Sydney", "Sydney", "AEST", "AEDT", 600, TZR_AU, P },
    { "Australia/Melbourne", "Melbourne", "AEST", "AEDT", 600, TZR_AU, P },
    { "Australia/Hobart", "Hobart", "AEST", "AEDT", 600, TZR_AU, P },
    { "Pacific/Guam", "Guam", "ChST", "ChST", 600, TZR_NONE, P },
    { "Pacific/Auckland", "Auckland", "NZST", "NZDT", 720, TZR_NZ, P },
    { "Pacific/Fiji", "Fiji", "+12", "+12", 720, TZR_NONE, P },
    { "Pacific/Tongatapu", "Nuku'alofa", "+13", "+13", 780, TZR_NONE, P },
};
#undef A
#undef E
#undef F
#undef S
#undef P
const int tzdb_n = (int)(sizeof(tzdb) / sizeof(tzdb[0]));

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

int tzdb_home(void)
{
    for (int i = 0; i < tzdb_n; ++i)
        if (same(tzdb[i].name, "Europe/Helsinki"))
            return i;
    return 0;
}

static int wday_of(int64_t days)
{
    return (int)((days % 7 + 11) % 7);              /* 1970-01-01 was a Thursday */
}

/* the day (1..31) of the n-th Sunday of y-mo (n = -1: the last one) */
static int sunday(int y, int mo, int n)
{
    if (n < 0) {
        int last = mo == 2 ? 28 : (mo == 4 || mo == 6 || mo == 9 || mo == 11) ? 30 : 31;
        return last - wday_of(tz_days(y, mo, last));
    }
    return 1 + (7 - wday_of(tz_days(y, mo, 1))) % 7 + 7 * (n - 1);
}

/* local wall-clock hh:00 on a day, as UTC, for a given offset in minutes */
static int64_t at(int y, int mo, int d, int hh, int off_min)
{
    return tz_days(y, mo, d) * 86400 + hh * 3600 - (int64_t)off_min * 60;
}

int tzdb_in_dst(const struct tzdb_zone *z, int64_t t)
{
    if (z->rule == TZR_NONE)
        return 0;
    struct tz_time u;
    tz_civil(t, &u);
    int y = u.y, o = z->off_min;
    int64_t start, end;
    switch (z->rule) {
    case TZR_EU:
        start = at(y, 3, sunday(y, 3, -1), 1, 0);
        end = at(y, 10, sunday(y, 10, -1), 1, 0);
        return t >= start && t < end;
    case TZR_US:
        start = at(y, 3, sunday(y, 3, 2), 2, o);
        end = at(y, 11, sunday(y, 11, 1), 2, o + 60);
        return t >= start && t < end;
    case TZR_AU:                                    /* the south: summer spans the new year */
        end = at(y, 4, sunday(y, 4, 1), 3, o + 60);
        start = at(y, 10, sunday(y, 10, 1), 2, o);
        return t < end || t >= start;
    case TZR_NZ:
        end = at(y, 4, sunday(y, 4, 1), 3, o + 60);
        start = at(y, 9, sunday(y, 9, -1), 2, o);
        return t < end || t >= start;
    }
    return 0;
}

int tzdb_offset(int i, int64_t utc)
{
    if (i < 0 || i >= tzdb_n)
        return 0;
    return tzdb[i].off_min + (tzdb_in_dst(&tzdb[i], utc) ? 60 : 0);
}

void tzdb_local(int i, int64_t utc, struct tz_time *o)
{
    if (i < 0 || i >= tzdb_n)
        i = tzdb_home();
    int dst = tzdb_in_dst(&tzdb[i], utc);
    tz_civil(utc + (int64_t)(tzdb[i].off_min + (dst ? 60 : 0)) * 60, o);
    o->dst = dst;
    o->abbr = dst ? tzdb[i].dst : tzdb[i].std;
}

/* FNV-1a of the name, folded to 30 bits */
uint32_t tzdb_id(int i)
{
    if (i < 0 || i >= tzdb_n)
        return 0;
    uint32_t h = 2166136261U;
    for (const char *p = tzdb[i].name; *p; ++p)
        h = (h ^ (uint8_t)*p) * 16777619U;
    return (h ^ h >> 30) & 0x3fffffffU;
}

int tzdb_by_id(uint32_t id)
{
    for (int i = 0; i < tzdb_n; ++i)
        if (tzdb_id(i) == id)
            return i;
    return -1;
}
