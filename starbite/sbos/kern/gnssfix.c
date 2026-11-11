/* SPDX-License-Identifier: GPL-2.0-only
 * 1.2-alpha28: the last GNSS fix from the card (gnssfix.h). */
#include "sb.h"
#include "kern.h"
#include "gnssfix.h"

/* a decimal number (optional sign, optional fraction) times 10^scale, rounded; 0 if not a number */
static int num(const char *s, int len, int scale, int64_t *out)
{
    int i = 0, neg = 0, any = 0, frac = 0;
    int64_t v = 0;
    if (i < len && (s[i] == '-' || s[i] == '+'))
        neg = s[i++] == '-';
    for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i, any = 1)
        if (v < 100000000000000LL)
            v = v * 10 + (s[i] - '0');
    int d = 0;
    if (i < len && s[i] == '.') {
        for (++i; i < len && s[i] >= '0' && s[i] <= '9'; ++i, any = 1) {
            if (d < scale) {
                frac = frac * 10 + (s[i] - '0');
                ++d;
            } else if (d == scale) {
                if (s[i] >= '5')
                    ++frac;             /* round at the last kept digit */
                ++d;
            }
        }
    }
    if (!any || i != len)
        return 0;
    int kept = d > scale ? scale : d;
    for (int k = kept; k < scale; ++k)
        frac *= 10;
    for (int k = 0; k < scale; ++k)
        v *= 10;
    v += frac;
    *out = neg ? -v : v;
    return 1;
}

int gnss_fix_parse(const char *t, int n, struct gnss_fix *f)
{
    memset(f, 0, sizeof(*f));
    f->alt_dm = f->acc_dm = f->sats = -1;
    int have = 0;
    for (int i = 0; i < n;) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n'))
            ++i;
        int k0 = i;
        while (i < n && t[i] != '=' && t[i] != ' ' && t[i] != '\n' && t[i] != '\r' && t[i] != '\t')
            ++i;
        if (i >= n || t[i] != '=')
            continue;                       /* a bare word (LASTFIX, v1) */
        int klen = i - k0, v0 = ++i;
        while (i < n && t[i] != ' ' && t[i] != '\n' && t[i] != '\r' && t[i] != '\t')
            ++i;
        const char *k = t + k0, *v = t + v0;
        int vlen = i - v0;
        int64_t x = 0;
#define KEY(s) (klen == (int)sizeof(s) - 1 && !memcmp(k, s, sizeof(s) - 1))
        if (KEY("utc") && num(v, vlen, 0, &x) && x > 1600000000LL && x < 4102444800LL) {
            f->utc = x;
            have |= 1;
        } else if (KEY("lat") && num(v, vlen, 6, &x) && x >= -90000000LL && x <= 90000000LL) {
            f->lat_e6 = (int32_t)x;
            have |= 2;
        } else if (KEY("lon") && num(v, vlen, 6, &x) && x >= -180000000LL && x <= 180000000LL) {
            f->lon_e6 = (int32_t)x;
            have |= 4;
        } else if (KEY("alt") && num(v, vlen, 1, &x) && x > -5000 && x < 100000) {
            f->alt_dm = (int32_t)x;
        } else if (KEY("acc") && num(v, vlen, 1, &x) && x >= 0 && x < 1000000) {
            f->acc_dm = (int32_t)x;
        } else if (KEY("sats") && num(v, vlen, 0, &x) && x >= 0 && x < 200) {
            f->sats = (int32_t)x;
        } else if (KEY("src")) {
            int m = vlen < (int)sizeof(f->src) - 1 ? vlen : (int)sizeof(f->src) - 1;
            memcpy(f->src, v, (size_t)m);
            f->src[m] = 0;
        }
#undef KEY
    }
    return have == 7;
}

static struct gnss_fix cached;
static int cached_st;                       /* 0 not read, 1 a fix, 2 none, < 0 the card's error */
static uint64_t cached_t;

int gnss_lastfix(struct gnss_fix *f, int reread)
{
    if (reread || !cached_st || sb_ms() - cached_t > 300000U) {
        static char buf[512];
        int n = kfile_read(GNSS_FIX_PATH, buf, sizeof(buf) - 1);
        cached_t = sb_ms();
        if (n < 0) {
            cached_st = n == SBOS_EINVAL ? 2 : -1;      /* no such file / no card */
        } else {
            buf[n] = 0;
            cached_st = gnss_fix_parse(buf, n, &cached) ? 1 : 2;
            if (cached_st == 1)
                sb_logx("gnss: the card's last fix, UTC ", (uint64_t)cached.utc);
            else
                sb_log("gnss: " GNSS_FIX_PATH " is not a valid fix");
        }
    }
    if (cached_st == 1)
        *f = cached;
    return cached_st == 1 ? 1 : cached_st == 2 ? 0 : -1;
}
