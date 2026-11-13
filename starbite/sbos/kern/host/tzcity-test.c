/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/tzcity.c over the generated kern/tzcities.c (Track A, 8a): the search and the table's sanity. */
#include <stdio.h>
#include <string.h>
#include "tzcities.h"
#include "tzdb.h"

static int nchk, nfail;
static void chk(int ok, const char *what) { nchk++; if (!ok) { nfail++; printf("FAIL: %s\n", what); } }
static int finds(const char *q, const char *name)
{
    int o[64], n = tzc_find(q, o, 64);
    for (int i = 0; i < n; i++) if (!strcmp(tzcities[o[i]].name, name)) return 1;
    return 0;
}

int main(void)
{
    chk(tzcities_n > 2000 && tzcities_n <= 3000, "about 3000 cities");
    int bad = 0;
    for (int i = 0; i < tzcities_n; i++) {
        const struct tzcity *c = &tzcities[i];
        if (c->zone >= tzdb_n || c->country >= tzc_ncountry || c->lat_e4 < -900000 || c->lat_e4 > 900000 ||
            c->lon_e4 < -1800000 || c->lon_e4 > 1800000 || !c->name[0]) bad++;
    }
    chk(!bad, "every city has a zone, a country and a place in range");
    chk(finds("zur", "Z\303\274rich"), "\"zur\" finds Zurich (accents ignored)");
    chk(finds("ZÜR", "Z\303\274rich"), "upper case and the accent in the query");
    chk(finds("york", "New York City"), "a later word matches");
    chk(finds("sao p", "S\303\243o Paulo"), "two words");
    chk(finds("helsinki", "Helsinki") && finds("tokyo", "Tokyo") && finds("oslo", "Oslo"), "big cities");
    int o[4];
    chk(tzc_find("", o, 4) == 0 && tzc_find("qqqxz", o, 4) == 0, "nothing for an empty or unknown query");
    chk(tzc_find("a", o, 4) == 4, "max is respected");
    printf("tzcity-test: %d checks, %d failed\n", nchk, nfail);
    return nfail != 0;
}
