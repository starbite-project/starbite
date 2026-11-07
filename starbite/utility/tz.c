/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS time zones (tz.h). The rules in force since 2007 (US; 1987-2006 is handled too) and 1996 (EU):
 *   EU: summer time from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC.
 *   US: daylight time from the second Sunday of March 02:00 local standard time to the first Sunday of
 *       November 02:00 local daylight time.
 * Civil dates: Howard Hinnant's days_from_civil / civil_from_days. */
#include "tz.h"

const struct tz_zone tz_zones[] = {
    { "Helsinki", "EET", "EEST", 120, TZ_EU },
    { "UTC", "UTC", "UTC", 0, TZ_FIXED },
    { "London", "GMT", "BST", 0, TZ_EU },
    { "Berlin", "CET", "CEST", 60, TZ_EU },
    { "New York", "EST", "EDT", -300, TZ_US },
    { "Los Angeles", "PST", "PDT", -480, TZ_US },
    { "Tokyo", "JST", "JST", 540, TZ_FIXED },
};
const int tz_nzones = (int)(sizeof(tz_zones) / sizeof(tz_zones[0]));

int64_t tz_days(int y, int mo, int d)
{
    int64_t yy = y - (mo <= 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void tz_civil(int64_t t, struct tz_time *o)
{
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t sec = t - days * 86400;
    o->h = (int)(sec / 3600);
    o->mi = (int)(sec / 60 % 60);
    o->s = (int)(sec % 60);
    o->wday = (int)((days % 7 + 11) % 7);             /* 1970-01-01 was a Thursday */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    o->d = (int)(doy - (153 * mp + 2) / 5 + 1);
    o->mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    o->y = (int)(yoe + era * 400 + (o->mo <= 2));
    o->dst = 0;
    o->abbr = "UTC";
}

/* the day (1..31) of the n-th Sunday of y-mo (n = -1: the last one) */
static int sunday(int y, int mo, int n)
{
    if (n < 0) {
        int last = mo == 2 ? 28 : (mo == 4 || mo == 6 || mo == 9 || mo == 11) ? 30 : 31;
        int64_t dd = tz_days(y, mo, last);
        int wd = (int)((dd % 7 + 11) % 7);
        return last - wd;
    }
    int64_t d1 = tz_days(y, mo, 1);
    int wd = (int)((d1 % 7 + 11) % 7);
    return 1 + (7 - wd) % 7 + 7 * (n - 1);
}

static int in_dst(const struct tz_zone *z, int64_t t)
{
    struct tz_time u;
    tz_civil(t, &u);
    int64_t start, end;
    if (z->rule == TZ_EU) {
        start = tz_days(u.y, 3, sunday(u.y, 3, -1)) * 86400 + 3600;
        end = tz_days(u.y, 10, sunday(u.y, 10, -1)) * 86400 + 3600;
    } else if (z->rule == TZ_US && u.y >= 2007) {
        start = tz_days(u.y, 3, sunday(u.y, 3, 2)) * 86400 + 7200 - z->off_min * 60;
        end = tz_days(u.y, 11, sunday(u.y, 11, 1)) * 86400 + 7200 - (z->off_min + 60) * 60;
    } else if (z->rule == TZ_US) {                     /* 1987-2006: first Sunday of April .. last of October */
        start = tz_days(u.y, 4, sunday(u.y, 4, 1)) * 86400 + 7200 - z->off_min * 60;
        end = tz_days(u.y, 10, sunday(u.y, 10, -1)) * 86400 + 7200 - (z->off_min + 60) * 60;
    } else {
        return 0;
    }
    return t >= start && t < end;
}

void tz_local(const struct tz_zone *z, int64_t t, struct tz_time *o)
{
    int dst = in_dst(z, t);
    tz_civil(t + (int64_t)(z->off_min + (dst ? 60 : 0)) * 60, o);
    o->dst = dst;
    o->abbr = dst ? z->dst : z->std;
}
