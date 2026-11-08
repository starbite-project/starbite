/* SPDX-License-Identifier: GPL-2.0-only
 * Overclocking tables (docs/ENGINEER-OS.md section 14): the chip's own DVFS tables, parsed from
 * byte copies. Pure (no hardware access, no UI): starbite/tools/unit-test.c checks it on the host
 * against starbite/host/oc-fake.c's synthetic tables.
 *
 * Two sources, both written by Samsung's boot chain, both only read here:
 *  - S-Boot's ECT (Exynos Characteristic Table, "PARA" header, vendor drivers/soc/samsung/
 *    ect_parser.c): the DVFS block lists every domain's frequency levels (kHz, descending); the
 *    GEN block's MINMAX_<domain> tables (6 columns x 7 rows, one row per ASV table version:
 *    index, ?, min MHz, max MHz, boot MHz, suspend MHz) are Samsung's policy limits. On this
 *    phone's ECT (sdb LU, 2026-10-26): A55 levels to 2002 MHz with the cap at 1794, M3 to 2886
 *    with 1924 / 2704 by grade, MIF to 2094 with 1794, G3D to 598 with 598.
 *  - ACPM's fvmap (vendor cal-if/fvmap.c): the frequency / voltage map the APM firmware itself
 *    applies, ASV voltages already chosen for this chip, at the DVFS plugin's base + 0x6700
 *    (exynos9810.dtsi fvmap_offset) in APM SRAM: 13 headers of 24 bytes (struct fvmap_header),
 *    each pointing (o_ratevolt) at { u32 rate kHz, u32 uV } per level.
 * The domain index is the ACPM DVFS id (vendor exynos9810/cmucal-vclk.h acpm_dvfs_id): 0 MIF,
 * 1 INT, 2 CPUCL0 (A55), 3 CPUCL1 (M3), 4 G3D, 5 INTCAM, 6 FSYS0, 7 CAM, 8 DISP, 9 AUD, 10 IVA,
 * 11 SCORE, 12 CP.
 */
#include "oc.h"

const struct oc_dom_info oc_info[OC_NDOM] = {
    { "MIF", "dvfs_mif", "memory (LPDDR4X controller and PHY)" },
    { "INT", "dvfs_int", "internal bus (NoC)" },
    { "A55", "dvfs_cpucl0", "little cluster, 4 x Cortex-A55" },
    { "M3", "dvfs_cpucl1", "big cluster, 4 x Mongoose M3" },
    { "G3D", "dvfs_g3d", "Mali-G72 MP18 GPU" },
    { "INTCAM", "dvfs_intcam", "camera bus" },
    { "FSYS0", "dvfs_fsys0", "UFS host (the boot disk)" },
    { "CAM", "dvfs_cam", "camera ISP" },
    { "DISP", "dvfs_disp_evt1", "display (DECON, the scanout)" },
    { "AUD", "dvfs_aud", "audio DSP" },
    { "IVA", "dvfs_iva", "vision accelerator" },
    { "SCORE", "dvfs_score", "vision DSP (SCore)" },
    { "CP", "dvs_cp", "modem (not running here)" },
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* A bounded reader over the ECT copy: every read checks the size, a failed read sets bad. */
struct rd {
    const uint8_t *b;
    uint32_t size, pos;
    int bad;
};

static uint32_t rd_u32(struct rd *r)
{
    if (r->pos > r->size || r->size - r->pos < 4) {
        r->bad = 1;
        return 0;
    }
    uint32_t v = le32(r->b + r->pos);
    r->pos += 4;
    return v;
}

/* ect_parse_string: u32 length (without the NUL), the bytes, the NUL, padded to 4. */
static const char *rd_str(struct rd *r, uint32_t *len)
{
    uint32_t n = rd_u32(r);
    if (r->bad || n > 64 || r->size - r->pos < n + 1) {
        r->bad = 1;
        return 0;
    }
    const char *s = (const char *)(r->b + r->pos);
    *len = n;
    r->pos += (n + 1 + 3) & ~3U;
    return s;
}

static int name_is(const char *s, uint32_t n, const char *want)
{
    uint32_t i = 0;
    for (; i < n && want[i]; ++i)
        if (s[i] != want[i])
            return 0;
    return i == n && !want[i];
}

/* MINMAX_<ect name> */
static int minmax_is(const char *s, uint32_t n, const char *ect)
{
    static const char pre[] = "MINMAX_";
    if (n < 7)
        return 0;
    for (unsigned i = 0; i < 7; ++i)
        if (s[i] != pre[i])
            return 0;
    return name_is(s + 7, n - 7, ect);
}

uint32_t oct_ect_find(const uint8_t *m, uint32_t size)
{
    for (uint32_t o = 0; size >= 16 && o <= size - 16; o += 4)
        if (m[o] == 'P' && m[o + 1] == 'A' && m[o + 2] == 'R' && m[o + 3] == 'A') {
            uint32_t total = le32(m + o + 8), n = le32(m + o + 12);
            if (n > 0 && n < 32 && total >= 64 && total <= OC_ECT_MAX && total <= size - o)
                return o;
        }
    return OC_NONE;
}

static int dom_of_ect(const char *s, uint32_t n)
{
    for (int i = 0; i < OC_NDOM; ++i)
        if (name_is(s, n, oc_info[i].ect))
            return i;
    return -1;
}

static void parse_dvfs(struct rd *top, uint32_t blk, struct oc_tables *t)
{
    struct rd r = *top;
    r.pos = blk;
    uint32_t pv = rd_u32(&r);
    (void)rd_u32(&r);                       /* version "0.00" */
    uint32_t nd = rd_u32(&r);
    if (r.bad || nd > 32)
        return;
    t->dvfs_parser = pv;
    struct {
        int d;
        uint32_t off;
    } list[32];
    for (uint32_t i = 0; i < nd; ++i) {
        uint32_t len;
        const char *s = rd_str(&r, &len);
        list[i].off = rd_u32(&r);
        if (r.bad)
            return;
        list[i].d = dom_of_ect(s, len);
    }
    for (uint32_t i = 0; i < nd; ++i) {
        int d = list[i].d;
        if (d < 0)
            continue;
        struct rd q = *top;
        q.pos = blk + list[i].off;
        if (q.pos < blk)
            continue;
        (void)rd_u32(&q);                   /* max_frequency (0 on this ECT) */
        (void)rd_u32(&q);                   /* min_frequency */
        if (pv >= 2) {
            (void)rd_u32(&q);               /* boot_level_idx */
            (void)rd_u32(&q);               /* resume_level_idx */
        }
        uint32_t mode = pv >= 3 ? rd_u32(&q) : 1;
        uint32_t nclk = rd_u32(&q), nlv = rd_u32(&q);
        if (q.bad || nclk > 64 || nlv == 0 || nlv > OC_MAXLV)
            continue;
        if (mode == 2) {
            q.pos += 4 * nclk;              /* SFR addresses */
        } else {
            for (uint32_t c = 0; c < nclk && !q.bad; ++c) {
                uint32_t len;
                (void)rd_str(&q, &len);
            }
        }
        struct oc_dom *o = &t->d[d];
        unsigned n = 0;
        for (uint32_t l = 0; l < nlv; ++l) {
            uint32_t khz = rd_u32(&q);
            (void)rd_u32(&q);               /* level_en */
            if (q.bad)
                break;
            if (khz && (!n || khz < o->khz[n - 1]))     /* strictly descending, as the ECT lists them */
                o->khz[n++] = khz;
        }
        if (q.bad || n != nlv) {
            o->nlv = 0;
            continue;
        }
        o->nlv = n;
        t->ect_domains++;
    }
}

static void parse_gen(struct rd *top, uint32_t blk, struct oc_tables *t)
{
    struct rd r = *top;
    r.pos = blk;
    (void)rd_u32(&r);                       /* parser version */
    (void)rd_u32(&r);
    uint32_t nt = rd_u32(&r);
    if (r.bad || nt > 64)
        return;
    for (uint32_t i = 0; i < nt; ++i) {
        uint32_t len;
        const char *s = rd_str(&r, &len);
        uint32_t off = rd_u32(&r);
        if (r.bad)
            return;
        for (int d = 0; d < OC_NDOM; ++d) {
            if (!minmax_is(s, len, oc_info[d].ect))
                continue;
            struct rd q = *top;
            q.pos = blk + off;
            uint32_t ncol = rd_u32(&q), nrow = rd_u32(&q);
            if (q.bad || ncol < 5 || ncol > 16 || nrow == 0 || nrow > 16)
                break;
            struct oc_dom *o = &t->d[d];
            o->mm_rows = 0;
            for (uint32_t row = 0; row < nrow && row < OC_MAXROW; ++row) {
                uint32_t v[16];
                for (uint32_t c = 0; c < ncol; ++c)
                    v[c] = rd_u32(&q);
                if (q.bad)
                    break;
                o->mm_min[row] = v[2] * 1000U;
                o->mm_max[row] = v[3] * 1000U;
                o->mm_boot[row] = v[4] * 1000U;
                o->mm_rows = row + 1;
            }
            break;
        }
    }
}

int oct_parse_ect(const uint8_t *ect, uint32_t size, struct oc_tables *t)
{
    struct rd r = { ect, size, 0, 0 };
    t->ect_domains = 0;
    for (int d = 0; d < OC_NDOM; ++d) {
        t->d[d].nlv = 0;
        t->d[d].mm_rows = 0;
    }
    if (size < 16 || ect[0] != 'P' || ect[1] != 'A' || ect[2] != 'R' || ect[3] != 'A')
        return 0;
    for (int i = 0; i < 4; ++i)
        t->ect_ver[i] = (char)((ect[4 + i] >= 32 && ect[4 + i] < 127) ? ect[4 + i] : '.');
    t->ect_ver[4] = 0;
    r.pos = 8;
    uint32_t total = rd_u32(&r), nb = rd_u32(&r);
    if (total < 16 || nb == 0 || nb > 32)
        return 0;
    if (total < size)
        r.size = total;
    uint32_t dvfs = OC_NONE, gen = OC_NONE;
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t len;
        const char *s = rd_str(&r, &len);
        uint32_t off = rd_u32(&r);
        if (r.bad)
            return 0;
        if (name_is(s, len, "DVFS"))
            dvfs = off;
        else if (name_is(s, len, "GEN"))
            gen = off;
    }
    if (dvfs != OC_NONE)
        parse_dvfs(&r, dvfs, t);
    if (gen != OC_NONE)
        parse_gen(&r, gen, t);
    oct_pick_row(t, t->asv_ver, t->asv_known);
    return t->ect_domains;
}

/* The MINMAX row for the ASV table version (when known), else the lowest cap over the rows (the
 * conservative choice: a step above it gets the stronger warning). */
void oct_pick_row(struct oc_tables *t, unsigned asv_ver, int known)
{
    t->asv_ver = asv_ver;
    t->asv_known = known;
    for (int d = 0; d < OC_NDOM; ++d) {
        struct oc_dom *o = &t->d[d];
        o->cap_khz = o->cap_lo = o->cap_hi = o->boot_khz = o->min_khz = 0;
        for (unsigned r = 0; r < o->mm_rows; ++r) {
            if (!o->cap_lo || o->mm_max[r] < o->cap_lo)
                o->cap_lo = o->mm_max[r];
            if (o->mm_max[r] > o->cap_hi)
                o->cap_hi = o->mm_max[r];
        }
        if (!o->mm_rows)
            continue;
        unsigned row = known && asv_ver < o->mm_rows ? asv_ver : 0;
        o->cap_khz = known && asv_ver < o->mm_rows ? o->mm_max[row] : o->cap_lo;
        o->boot_khz = o->mm_boot[row];
        o->min_khz = o->mm_min[row];
        o->cap_exact = known && asv_ver < o->mm_rows;
    }
}

/* fv: a copy of OC_FVMAP_SIZE bytes at a candidate fvmap base. Accepted when the A55, M3 and G3D
 * headers each point at a rate / voltage table whose rates are the ECT's levels (so the ECT must
 * be parsed first). 1 accepted (voltages filled), 0 not an fvmap. */
int oct_parse_fvmap(const uint8_t *fv, uint32_t size, struct oc_tables *t)
{
    struct oc_dom tmp[OC_NDOM];
    unsigned matched = 0;
    if (size < OC_NDOM * 24U)
        return 0;
    for (int d = 0; d < OC_NDOM; ++d) {
        const uint8_t *h = fv + d * 24;
        unsigned nlv = h[1], o = le16(h + 20);
        struct oc_dom *x = &tmp[d];
        x->fv_n = 0;
        if (!nlv || nlv > OC_MAXLV || o < OC_NDOM * 24U || o + nlv * 8U > size)
            continue;
        uint32_t last = 0xffffffffU;
        unsigned n = 0;
        for (unsigned l = 0; l < nlv; ++l) {
            uint32_t rate = le32(fv + o + 8 * l), uv = le32(fv + o + 8 * l + 4);
            if (!rate || rate >= last || uv < 300000U || uv > 1500000U)
                break;
            x->fv_khz[n] = rate;
            x->fv_uv[n] = uv;
            ++n;
            last = rate;
        }
        if (n != nlv)
            continue;
        x->fv_n = n;
        x->fv_init = h[8];
        if (d == OC_A55 || d == OC_M3 || d == OC_G3D) {
            const struct oc_dom *e = &t->d[d];
            unsigned hit = 0;
            for (unsigned l = 0; l < n; ++l)
                for (unsigned k = 0; k < e->nlv; ++k)
                    hit += e->khz[k] == x->fv_khz[l];
            matched += e->nlv && hit * 2 >= n && hit >= 2;
        }
    }
    if (matched < 3)
        return 0;
    for (int d = 0; d < OC_NDOM; ++d) {
        struct oc_dom *o = &t->d[d];
        o->fv_n = tmp[d].fv_n;
        o->fv_init = tmp[d].fv_init;
        for (unsigned l = 0; l < o->fv_n; ++l) {
            o->fv_khz[l] = tmp[d].fv_khz[l];
            o->fv_uv[l] = tmp[d].fv_uv[l];
        }
    }
    t->fv_found = 1;
    return 1;
}

/* ACPM's voltage for an ECT level: uV, 0 when ACPM's table has no such step. */
uint32_t oct_uv(const struct oc_dom *o, uint32_t khz)
{
    for (unsigned l = 0; l < o->fv_n; ++l)
        if (o->fv_khz[l] == khz)
            return o->fv_uv[l];
    return 0;
}

int oct_level(const struct oc_dom *o, uint32_t khz)
{
    for (unsigned l = 0; l < o->nlv; ++l)
        if (o->khz[l] == khz)
            return (int)l;
    return -1;
}

/* ACPM gives khz less voltage than some slower step: an uncharacterised level for this chip (the phone,
 * 2026-11-08: MIF 2002-2094 MHz at 712.5 mV under 1794 MHz's 737.5 mV, where iOS froze at 2093 MHz;
 * M3 2886 MHz under 2860 MHz). 1 when so. */
int oct_undervolted(const struct oc_dom *o, uint32_t khz)
{
    uint32_t uv = oct_uv(o, khz);
    for (unsigned l = 0; l < o->fv_n; ++l)
        if (o->fv_khz[l] < khz && o->fv_uv[l] > uv)
            return 1;
    return 0;
}

/* A step the overclocking pages may request: in the ECT's level list AND in ACPM's own fvmap
 * (so ACPM applies its own ASV voltage) AND not given less voltage than a slower step; never
 * anything else. */
int oct_allowed(const struct oc_dom *o, uint32_t khz)
{
    return oct_level(o, khz) >= 0 && oct_uv(o, khz) != 0 && !oct_undervolted(o, khz);
}

int oct_above_cap(const struct oc_dom *o, uint32_t khz)
{
    return !o->cap_khz || khz > o->cap_khz;
}
