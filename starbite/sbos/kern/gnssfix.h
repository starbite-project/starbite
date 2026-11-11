/* SPDX-License-Identifier: GPL-2.0-only
 * 1.2-alpha28: the last GNSS fix, written to the microSD card by Android (LineageOS / One UI, which run Broadcom's
 * closed GNSS stack: docs/GNSS.md) and read here for the sun times and as the last known place.
 *
 * The file: /STARBITE/GNSS/LASTFIX.TXT on the card's exFAT volume, one line of key=value pairs, e.g.
 *   LASTFIX v1 utc=1794500000 lat=60.169856 lon=24.938379 alt=12.3 acc=8.5 sats=9 src=gps
 * utc (seconds since 1970), lat and lon (decimal degrees, N / E positive) are required; alt (m), acc (m, the 68 %
 * radius), sats and src are optional. Read only: nothing here writes the card. */
#ifndef SBOS_GNSSFIX_H
#define SBOS_GNSSFIX_H
#include <stdint.h>

#define GNSS_FIX_PATH "/STARBITE/GNSS/LASTFIX.TXT"
struct gnss_fix {
    int64_t utc;
    int32_t lat_e6, lon_e6;     /* micro-degrees */
    int32_t alt_dm, acc_dm;     /* decimetres; -1 unknown */
    int32_t sats;               /* -1 unknown */
    char src[12];
};
/* parse one file's text: 1 a valid fix */
int gnss_fix_parse(const char *txt, int n, struct gnss_fix *f);
/* the card's last fix (read again when reread or after 5 minutes): 1 a fix, 0 none, < 0 no card */
int gnss_lastfix(struct gnss_fix *f, int reread);
#endif
