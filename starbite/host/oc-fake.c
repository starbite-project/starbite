/* SPDX-License-Identifier: GPL-2.0-only
 * A synthetic ECT and ACPM fvmap for the host preview and the unit tests (never in the image).
 * The DVFS levels and MINMAX rows are this phone's (backup/20261026-vg4-bootluns/sdb.img, ECT
 * "004x", parser 3); the voltages are invented (plausible ramps), the real ones come from the
 * phone's ACPM at run time.
 */
#include <stdint.h>
#include <string.h>

struct fake_dom {
    const char *name;
    unsigned n;
    unsigned mhz[22];
    unsigned mm[7][6];                      /* MINMAX rows */
    unsigned uv_lo, uv_hi;                  /* invented fvmap voltage ramp */
};

#define R7(a, b, c, d) { { 0, 0, a, b, c, d }, { 1, 1, a, b, c, d }, { 2, 2, a, b, c, d }, { 3, 2, a, b, c, d }, \
                         { 4, 2, a, b, c, d }, { 5, 5, a, b, c, d }, { 6, 5, a, b, c, d } }

static const struct fake_dom fd[] = {
    { "dvfs_cpucl0", 14, { 2002, 1950, 1794, 1690, 1456, 1248, 1053, 949, 832, 715, 598, 455, 299, 208 },
      R7(455, 1794, 1053, 1053), 550000, 1100000 },
    { "dvfs_cpucl1", 21, { 2886, 2860, 2704, 2652, 2496, 2314, 2106, 2002, 1924, 1794, 1690, 1586, 1469, 1261, 1170,
                           1066, 962, 858, 741, 650, 598 },
      { { 0, 0, 598, 1924, 1066, 741 }, { 1, 1, 598, 1924, 1066, 741 }, { 2, 2, 598, 1924, 1066, 741 },
        { 3, 3, 598, 1924, 1066, 741 }, { 4, 4, 598, 2704, 1066, 741 }, { 5, 5, 598, 2704, 1066, 741 },
        { 6, 6, 598, 2704, 1066, 741 } },
      550000, 1200000 },
    { "dvfs_g3d", 8, { 598, 572, 546, 455, 385, 338, 299, 260 }, R7(260, 598, 260, 260), 600000, 850000 },
    { "dvfs_mif", 11, { 2094, 2093, 2002, 1794, 1539, 1352, 1014, 845, 676, 546, 421 }, R7(421, 1794, 1794, 421),
      600000, 850000 },
    { "dvfs_int", 6, { 534, 400, 336, 267, 178, 107 }, R7(107, 534, 534, 107), 550000, 800000 },
    { "dvfs_intcam", 7, { 690, 680, 670, 660, 650, 640, 630 }, R7(630, 690, 630, 630), 600000, 700000 },
    { "dvfs_fsys0", 6, { 336, 267, 200, 134, 100, 73 }, R7(73, 336, 336, 73), 550000, 750000 },
    { "dvfs_cam", 11, { 690, 680, 670, 660, 650, 640, 630, 620, 610, 600, 590 }, R7(590, 690, 590, 590), 600000, 700000 },
    { "dvfs_iva", 5, { 534, 467, 336, 168, 34 }, R7(34, 534, 34, 34), 550000, 800000 },
    { "dvfs_score", 5, { 534, 467, 336, 168, 34 }, R7(34, 534, 34, 34), 550000, 800000 },
    { "dvfs_disp", 5, { 640, 534, 400, 200, 134 }, R7(134, 534, 400, 134), 550000, 750000 },
    { "dvfs_disp_evt1", 5, { 640, 534, 400, 200, 134 }, R7(134, 534, 400, 134), 550000, 750000 },
    { "dvfs_aud", 5, { 1180, 800, 590, 394, 295 }, R7(295, 1180, 295, 295), 550000, 800000 },
    { "dvs_cp", 4, { 1300, 950, 800, 533 }, R7(533, 1300, 950, 950), 600000, 800000 },
};
#define NFD (sizeof(fd) / sizeof(fd[0]))

static uint8_t *B;
static uint32_t P, CAP;

static void w32(uint32_t v)
{
    if (P + 4 <= CAP)
        memcpy(B + P, &v, 4);
    P += 4;
}

static void at32(uint32_t off, uint32_t v)
{
    if (off + 4 <= CAP)
        memcpy(B + off, &v, 4);
}

static void wstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    w32(n);
    for (uint32_t i = 0; i <= n; ++i)
        if (P + i < CAP)
            B[P + i] = (uint8_t)s[i];
    P += (n + 1 + 3) & ~3U;
}

uint32_t oc_fake_ect(uint8_t *buf, uint32_t cap)
{
    B = buf;
    CAP = cap;
    P = 0;
    memset(buf, 0, cap);
    memcpy(buf, "PARA004x", 8);
    P = 8;
    uint32_t total_at = P;
    w32(0);
    w32(2);                                 /* DVFS, GEN */
    wstr("DVFS");
    uint32_t dvfs_off_at = P;
    w32(0);
    wstr("GEN");
    uint32_t gen_off_at = P;
    w32(0);
    /* DVFS block */
    uint32_t dvfs = P;
    at32(dvfs_off_at, dvfs);
    w32(3);
    memcpy(B + P, "0.00", 4);
    P += 4;
    w32(NFD);
    uint32_t offs_at[NFD];
    for (unsigned i = 0; i < NFD; ++i) {
        wstr(fd[i].name);
        offs_at[i] = P;
        w32(0);
    }
    for (unsigned i = 0; i < NFD; ++i) {
        at32(offs_at[i], P - dvfs);
        w32(0);
        w32(0xffffffffU);
        w32(0xffffffffU);
        w32(0xffffffffU);
        w32(2);                             /* SFR addresses */
        w32(1);
        w32(fd[i].n);
        w32(0x1d001820U);
        for (unsigned l = 0; l < fd[i].n; ++l) {
            w32(fd[i].mhz[l] * 1000U);
            w32(0);
        }
        for (unsigned l = 0; l < fd[i].n; ++l)
            w32(l);                         /* one dvfs value per level and clock */
    }
    /* GEN block: the MINMAX tables */
    uint32_t gen = P;
    at32(gen_off_at, gen);
    w32(1);
    w32(0);
    w32(NFD);
    uint32_t goffs_at[NFD];
    for (unsigned i = 0; i < NFD; ++i) {
        char nm[48] = "MINMAX_";
        strcat(nm, fd[i].name);
        wstr(nm);
        goffs_at[i] = P;
        w32(0);
    }
    for (unsigned i = 0; i < NFD; ++i) {
        at32(goffs_at[i], P - gen);
        w32(6);
        w32(7);
        for (unsigned r = 0; r < 7; ++r)
            for (unsigned c = 0; c < 6; ++c)
                w32(fd[i].mm[r][c]);
    }
    at32(total_at, P);
    return P <= cap ? P : 0;
}

/* The fvmap: 13 headers in ACPM id order, then the rate / voltage tables. */
uint32_t oc_fake_fvmap(uint8_t *buf, uint32_t cap)
{
    static const char *const order[13] = { "dvfs_mif", "dvfs_int", "dvfs_cpucl0", "dvfs_cpucl1", "dvfs_g3d",
                                           "dvfs_intcam", "dvfs_fsys0", "dvfs_cam", "dvfs_disp_evt1", "dvfs_aud",
                                           "dvfs_iva", "dvfs_score", "dvs_cp" };
    if (cap < 0x2000)
        return 0;
    memset(buf, 0, 0x2000);
    uint32_t p = 13 * 24;
    for (unsigned d = 0; d < 13; ++d) {
        const struct fake_dom *f = 0;
        for (unsigned i = 0; i < NFD; ++i)
            if (!strcmp(fd[i].name, order[d]))
                f = &fd[i];
        if (!f)
            continue;
        uint8_t *h = buf + d * 24;
        h[0] = (uint8_t)d;
        h[1] = (uint8_t)f->n;
        h[8] = (uint8_t)(f->n / 2);
        h[20] = (uint8_t)p;
        h[21] = (uint8_t)(p >> 8);
        for (unsigned l = 0; l < f->n; ++l) {
            uint32_t khz = f->mhz[l] * 1000U;
            uint32_t uv = f->uv_lo + (uint32_t)((uint64_t)(f->uv_hi - f->uv_lo) * (f->n - 1 - l) / (f->n > 1 ? f->n - 1 : 1));
            uv = uv / 6250U * 6250U;
            memcpy(buf + p, &khz, 4);
            memcpy(buf + p + 4, &uv, 4);
            p += 8;
        }
    }
    return 0x2000;
}
