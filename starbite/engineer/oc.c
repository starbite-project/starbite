/* SPDX-License-Identifier: GPL-2.0-only
 * Overclocking (docs/ENGINEER-OS.md section 14). Every DVFS domain's table as the chip itself has
 * it:
 *  - the frequency steps and Samsung's caps from S-Boot's ECT (DRAM 0xA0000000, read in place),
 *  - ACPM's voltage per step from its fvmap in APM SRAM (32-bit reads, faults caught),
 *  - the current clock from the PLL registers where libsb can read it (A55, M3, G3D, MIF).
 * Step 2 (below the tables): MIF, INT, A55, M3 and G3D can be set to one of their own listed steps,
 * confirmed, guarded (watchdog, storage hold) and put back after 15 s unless kept; each has a
 * stability test. The OC_SELFTEST image (ENG_DEFS=-DOC_SELFTEST) logs the tables to pmsg at start
 * and asks ACPM FREQ_GET (a read) for A55 / M3 / G3D / MIF / INT next to the PLL and the boot step;
 * it requests nothing.
 */
#include "eng.h"
#include "oc.h"

#define ECT_BASE 0xa0000000UL

static struct oc_tables OT;
static int ot_loaded, ot_ect_ok;
static uint32_t ot_ect_at = OC_NONE, ot_ect_size, ot_fv_at = OC_NONE;
static const char *ot_fv_how;               /* where the fvmap was found */
#define OC_NPLUG 16
struct oc_plug {
    uint32_t id, base, size;
    uint8_t att, stay;
    char name[20];
};
static struct oc_plug ot_plug[OC_NPLUG];    /* ACPM's plugin table (initdata->plugins) */
static unsigned ot_nplug;
static uint64_t ot_sram_bad;                /* the scan: unreadable 4 KiB blocks */
static uint8_t ot_sram[0x30000];            /* ACPM_SRAM_SIZE: the scan copy */

static uint32_t rd32(uintptr_t a)
{
    uint32_t v = 0;
    (void)sb_probe32(a, &v);
    return v;
}

/* The self-test's breadcrumbs: one pmsg line before each read block, so a freeze shows where. */
static int oc_crumbs;

static void crumb(const char *what)
{
    sb_wdt_feed(1);                         /* between read blocks only (a no-op unless armed) */
    if (oc_crumbs)
        sb_logf("oc selftest: next: ", what);
}

static void oc_load(void)
{
    const uint8_t *win = (const uint8_t *)ECT_BASE;
    ot_loaded = 1;
    ot_ect_ok = 0;
    ot_ect_at = ot_fv_at = OC_NONE;
    ot_fv_how = 0;
    for (int d = 0; d < OC_NDOM; ++d)
        OT.d[d].fv_n = 0;
    OT.fv_found = 0;
    OT.asv_ver = 0;
    OT.asv_known = 0;                       /* not read yet: the lowest cap over the rows is used */
    crumb("ECT in DRAM 0xA0000000 (find and parse)");
    ot_ect_at = oct_ect_find(win, OC_ECT_MAX);
    if (ot_ect_at != OC_NONE) {
        const uint8_t *e = win + ot_ect_at;
        ot_ect_size = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        ot_ect_ok = oct_parse_ect(e, OC_ECT_MAX - ot_ect_at, &OT) > 0;
    }
    if (!ot_ect_ok)
        return;
    /* The fvmap (vendor acpm.c): APM SRAM + (the DVFS plugin's base_addr & ~1) + fvmap_offset 0x6700
     * for a resident plugin, + the plugin's size for a dynamic one. Each candidate must carry the
     * ECT's A55, M3 and G3D steps, so a wrong place is not taken. */
    crumb("ACPM plugin table (initdata +0x7F00)");
    ot_nplug = 0;
    uint32_t init[2];
    if (sb_acpm_sram_read(0x7f00, init, 8) && init[0] && init[0] < sizeof(ot_sram) && init[1] && init[1] <= 32 &&
        init[0] + init[1] * 0x30U <= sizeof(ot_sram)) {
        for (uint32_t k = 0; k < init[1] && ot_nplug < OC_NPLUG; ++k) {
            uint32_t w[12];
            if (!sb_acpm_sram_read(init[0] + k * 0x30U, w, 0x30))
                continue;
            struct oc_plug *pl = &ot_plug[ot_nplug++];
            pl->id = w[0];
            pl->base = w[1] & ~1U;
            pl->size = w[9];
            pl->att = (uint8_t)w[8];
            pl->stay = (uint8_t)w[10];
            pl->name[0] = 0;
            uint32_t nm[8], a = w[11] & ~3U;
            if (w[11] && a + 32 <= sizeof(ot_sram) && sb_acpm_sram_read(a, nm, 32)) {
                const char *c = (const char *)nm + (w[11] & 3U);
                unsigned q = 0;
                while (q < sizeof(pl->name) - 1 && c + q < (const char *)nm + 32 && c[q] >= 32 && c[q] < 127) {
                    pl->name[q] = c[q];
                    ++q;
                }
                pl->name[q] = 0;
            }
        }
    }
    for (unsigned k = 0; k < ot_nplug && ot_fv_at == OC_NONE; ++k) {
        const struct oc_plug *pl = &ot_plug[k];
        int dv = 0;
        for (unsigned q = 0; pl->name[q] && pl->name[q + 1] && pl->name[q + 2] && pl->name[q + 3]; ++q)
            dv |= (pl->name[q] | 0x20) == 'd' && (pl->name[q + 1] | 0x20) == 'v' && (pl->name[q + 2] | 0x20) == 'f' &&
                  (pl->name[q + 3] | 0x20) == 's';
        if (!dv)
            continue;
        uint32_t cand[2] = { pl->base + OC_FVMAP_OFFSET, pl->base + pl->size };
        for (unsigned c = 0; c < 2 && ot_fv_at == OC_NONE; ++c) {
            uint32_t o = cand[c] & ~3U, len = OC_FVMAP_SIZE;
            if (!cand[c] || o >= sizeof(ot_sram))
                continue;
            if (len > sizeof(ot_sram) - o)
                len = (sizeof(ot_sram) - o) & ~3U;
            crumb(c ? "fvmap at the DVFS plugin + its size" : "fvmap at the DVFS plugin + 0x6700");
            if (sb_acpm_sram_read(o, ot_sram, len) && oct_parse_fvmap(ot_sram, len, &OT)) {
                ot_fv_at = o;
                ot_fv_how = c ? "DVFS plugin + size" : "DVFS plugin + 0x6700";
            }
        }
    }
    if (ot_fv_at != OC_NONE)
        return;
    /* last: a scan of the whole SRAM, 4 KiB blocks; a block that faults reads as zeros (it cannot
     * hold the map) and the scan goes on */
    crumb("APM SRAM scan, 4 KiB blocks, unreadable ones skipped");
    ot_sram_bad = 0;
    for (uint32_t b = 0; b < sizeof(ot_sram); b += 0x1000) {
        if (!sb_acpm_sram_read(b, ot_sram + b, 0x1000)) {
            for (uint32_t z = 0; z < 0x1000; ++z)
                ot_sram[b + z] = 0;
            ot_sram_bad |= 1ULL << (b >> 12);
        }
        sb_wdt_feed(1);
    }
    if (oc_crumbs)
        sb_logx("oc selftest: SRAM 4 KiB blocks unreadable (bit per block) ", ot_sram_bad);
    for (uint32_t o = 0; o + OC_NDOM * 24U <= sizeof(ot_sram); o += 4) {
        uint32_t len = sizeof(ot_sram) - o < OC_FVMAP_SIZE ? sizeof(ot_sram) - o : OC_FVMAP_SIZE;
        if (oct_parse_fvmap(ot_sram + o, len, &OT)) {
            ot_fv_at = o;
            ot_fv_how = "SRAM scan";
            return;
        }
    }
}

static void oc_ensure(void)
{
    if (!ot_loaded)
        oc_load();
}

/* ---- the current clock (registers only) ------------------------------------------------------------ */

/* kHz now (0: not read), *how: where it came from. */
static uint32_t now_khz(int d, const char **how)
{
    int off = 0;
    *how = 0;
    switch (d) {
    case OC_A55:
        *how = "CMU_CPUCL0 PLL and divider";
        return (uint32_t)(sb_a55_hz() / 1000U);
    case OC_M3: {
        uint64_t hz = sb_m3_hz(&off);
        *how = off ? "cluster powered off" : "CMU_CPUCL1 PLL and divider";
        return off ? 0 : (uint32_t)(hz / 1000U);
    }
    case OC_G3D: {
        uint64_t hz = sb_gpu_hz(&off);
        *how = off ? "GPU powered off" : "PLL_G3D";
        return off ? 0 : (uint32_t)(hz / 1000U);
    }
    case OC_MIF:
        if (rd32(0x1b801000UL) & 1U) {
            *how = "MIF on its switch clock";
            return 0;
        }
        *how = "PLL_MIF";
        return (uint32_t)(sb_pll_hz(0x1b800120UL, 0) / 1000U);
    default:
        return 0;
    }
}

/* The ECT step nearest to khz within 1.5 % (MIF: the PLL or half of it), -1 none. */
static int step_of(int d, uint32_t khz)
{
    const struct oc_dom *o = &OT.d[d];
    for (int pass = 0; pass < (d == OC_MIF ? 2 : 1); ++pass) {
        uint32_t k = pass ? khz / 2U : khz;
        int best = -1;
        uint32_t bd = 0;
        for (unsigned l = 0; l < o->nlv; ++l) {
            uint32_t df = o->khz[l] > k ? o->khz[l] - k : k - o->khz[l];
            if (best < 0 || df < bd) {
                best = (int)l;
                bd = df;
            }
        }
        if (best >= 0 && (uint64_t)bd * 200U <= (uint64_t)o->khz[best] * 3U)
            return best;
    }
    return -1;
}

/* ---- formatting ---------------------------------------------------------------------------------- */

static void ub_mhz(struct ub *u, uint32_t khz)
{
    ub_dec(u, khz / 1000U);
    if (khz % 1000U) {
        ub_c(u, '.');
        ub_dec(u, (khz % 1000U) / 100U);
    }
}

static void ub_mv(struct ub *u, uint32_t uv)
{
    ub_dec(u, uv / 1000U);
    ub_c(u, '.');
    ub_dec2(u, (uv % 1000U) / 10U);
}

static void ub_pad(struct ub *u, unsigned col)
{
    while (sb_strlen(u->b) < col)
        ub_c(u, ' ');
}

static int above_any(const struct oc_dom *o)
{
    return o->cap_khz && o->nlv && o->khz[0] > o->cap_khz;
}

/* ---- the domain page --------------------------------------------------------------------------- */

static int oc_cur;

/* The domain's facts; returns the step the clock is on (-1 unknown). */
static int dom_head(void)
{
    const int d = oc_cur;
    const struct oc_dom *o = &OT.d[d];
    struct ub u;
    const char *how;
    uint32_t now = now_khz(d, &how);
    int ns = now ? step_of(d, now) : -1;

    kv_hdr("DOMAIN");
    kv_add("What", oc_info[d].what);
    ub_init(&u);
    ub_dec(&u, (unsigned)d);
    ub_s(&u, " (");
    ub_s(&u, oc_info[d].ect);
    ub_s(&u, " in the ECT)");
    kv_add("ACPM DVFS id", u.b);
    ub_init(&u);
    if (o->nlv) {
        ub_dec(&u, o->nlv);
        ub_s(&u, ", ");
        ub_mhz(&u, o->khz[0]);
        ub_s(&u, " to ");
        ub_mhz(&u, o->khz[o->nlv - 1]);
        ub_s(&u, " MHz");
    } else {
        ub_s(&u, "none (not in the ECT)");
    }
    kv_add("ECT steps", u.b);
    ub_init(&u);
    if (o->fv_n) {
        ub_dec(&u, o->fv_n);
        ub_s(&u, ", init step ");
        ub_dec(&u, o->fv_init);
        if (o->fv_init < o->fv_n) {
            ub_s(&u, " (");
            ub_mhz(&u, o->fv_khz[o->fv_init]);
            ub_s(&u, " MHz)");
        }
    } else {
        ub_s(&u, OT.fv_found ? "none for this domain" : "fvmap not found");
    }
    kv_add("ACPM steps", u.b);
    ub_init(&u);
    if (o->cap_khz) {
        ub_mhz(&u, o->cap_khz);
        ub_s(&u, " MHz");
        if (!o->cap_exact && o->cap_hi != o->cap_lo) {
            ub_s(&u, " (lowest row; rows go to ");
            ub_mhz(&u, o->cap_hi);
            ub_s(&u, ")");
        }
    } else {
        ub_s(&u, "no MINMAX table");
    }
    kv_addc("Samsung's cap", u.b, above_any(o) ? C_ORANGE : C_L1);
    if (o->mm_rows) {
        ub_init(&u);
        ub_mhz(&u, o->min_khz);
        ub_s(&u, " / ");
        ub_mhz(&u, o->boot_khz);
        ub_s(&u, " MHz");
        kv_add("Policy min / boot", u.b);
    }
    ub_init(&u);
    if (now) {
        int half = ns >= 0 && d == OC_MIF && o->khz[ns] * 3U < now * 2U;
        ub_mhz(&u, half ? o->khz[ns] : now);
        ub_s(&u, " MHz");
        if (ns >= 0) {
            ub_s(&u, ", step ");
            ub_dec(&u, (unsigned)ns);
            if (half) {
                ub_s(&u, " (PLL ");
                ub_mhz(&u, now);
                ub_s(&u, " / 2)");
            }
        } else {
            ub_s(&u, ", not a listed step");
        }
        kv_add("Now", u.b);
        kv_add("Read from", how);
    } else {
        kv_addc("Now", how ? how : "not readable here (no PLL of its own in libsb)", C_L3);
    }
    return ns;
}

static void fill_dom(void)
{
    const int d = oc_cur;
    const struct oc_dom *o = &OT.d[d];
    struct ub u;
    kv_note("Read only: no block of this domain runs in Engineer OS, so a step could not be tested here.");
    int ns = dom_head();

    kv_hdr(o->nlv ? "STEPS (ECT) AND ACPM VOLTAGE" : "STEPS (ACPM ONLY)");
    kv_mono("  #      MHz       mV  NOTE");
    unsigned n = o->nlv ? o->nlv : o->fv_n;
    for (unsigned l = 0; l < n; ++l) {
        uint32_t khz = o->nlv ? o->khz[l] : o->fv_khz[l], uv = oct_uv(o, khz);
        uint32_t c = C_L1;
        ub_init(&u);
        ub_c(&u, (int)l == ns ? '>' : ' ');
        if (l < 10)
            ub_c(&u, ' ');
        ub_dec(&u, l);
        ub_pad(&u, 5);
        struct ub m;
        ub_init(&m);
        ub_mhz(&m, khz);
        ub_pad(&u, 12 - sb_strlen(m.b));
        ub_s(&u, m.b);
        ub_init(&m);
        if (uv)
            ub_mv(&m, uv);
        else
            ub_s(&m, "-");
        ub_pad(&u, 21 - sb_strlen(m.b));
        ub_s(&u, m.b);
        ub_s(&u, "  ");
        if (!uv && o->nlv) {
            ub_s(&u, "not in ACPM");
            c = C_L3;
        } else if (oct_undervolted(o, khz)) {
            ub_s(&u, "undervolted: blocked");
            c = C_RED;
        } else if (o->cap_khz && khz > o->cap_khz) {
            ub_s(&u, "above cap");
            c = C_ORANGE;
        } else if (khz == o->cap_khz) {
            ub_s(&u, "cap");
        } else if (khz == o->boot_khz) {
            ub_s(&u, "boot");
        }
        kv_monoc(u.b, c);
    }
    kv_note("> marks the step the clock is on. Above-cap steps are in the chip's own table, outside Samsung's "
            "policy.");
}

/* ---- sources ------------------------------------------------------------------------------------ */

static void fill_src(void)
{
    struct ub u;
    kv_hdr("S-BOOT'S ECT");
    ub_init(&u);
    if (ot_ect_at == OC_NONE) {
        kv_addc("ECT", "no PARA header in 0xA0000000..0xA00FFFFF", C_RED);
    } else {
        ub_hex(&u, ECT_BASE + ot_ect_at, 8);
        ub_s(&u, ", ");
        ub_dec(&u, ot_ect_size / 1024U);
        ub_s(&u, " KiB");
        kv_add("Where", u.b);
        ub_init(&u);
        ub_s(&u, "PARA ");
        ub_s(&u, OT.ect_ver);
        ub_s(&u, ", DVFS parser ");
        ub_dec(&u, OT.dvfs_parser);
        kv_add("Version", u.b);
        kv_dec("Domains read", OT.ect_domains, 0);
    }
    kv_hdr("ACPM FVMAP (APM SRAM)");
    ub_init(&u);
    if (ot_fv_at == OC_NONE) {
        ub_s(&u, ot_ect_ok ? "not found (no SRAM block matches the ECT's CPU and GPU steps)" : "not looked for (no ECT)");
        kv_addc("fvmap", u.b, C_ORANGE);
    } else {
        ub_s(&u, "SRAM + ");
        ub_hex(&u, ot_fv_at, 5);
        ub_s(&u, " (");
        ub_s(&u, ot_fv_how ? ot_fv_how : "?");
        ub_s(&u, ")");
        kv_add("Where", u.b);
    }
    kv_hdr("SAMSUNG'S CAPS");
    kv_add("ASV table version", OT.asv_known ? "read" : "not read yet");
    kv_note("The ECT's MINMAX tables have one row per ASV table version. Until this chip's version is read, the "
            "lowest cap over the rows is shown (the conservative choice).");
    kv_hdr("HOW IT IS READ");
    kv_note("ECT: DRAM reads in place (S-Boot's copy, never written). fvmap: 32-bit APM SRAM reads. Now: PLL "
            "registers. No ACPM message, no CMU, PMU or PMIC write.");
}

/* ---- the log --------------------------------------------------------------------------------------- */

static void log_tables(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "oc: ECT ");
    if (ot_ect_at == OC_NONE) {
        ub_s(&u, "not found");
    } else {
        ub_s(&u, OT.ect_ver);
        ub_s(&u, " at ");
        ub_hex(&u, ECT_BASE + ot_ect_at, 8);
        ub_s(&u, " size ");
        ub_hex(&u, ot_ect_size, 5);
        ub_s(&u, " parser ");
        ub_dec(&u, OT.dvfs_parser);
        ub_s(&u, " domains ");
        ub_dec(&u, OT.ect_domains);
    }
    ub_s(&u, "; fvmap ");
    if (ot_fv_at == OC_NONE) {
        ub_s(&u, "not found");
    } else {
        ub_hex(&u, ot_fv_at, 5);
        ub_s(&u, " (");
        ub_s(&u, ot_fv_how ? ot_fv_how : "?");
        ub_s(&u, ")");
    }
    sb_log(u.b);
    for (unsigned k = 0; k < ot_nplug; ++k) {
        const struct oc_plug *pl = &ot_plug[k];
        ub_init(&u);
        ub_s(&u, "oc: acpm plugin ");
        ub_dec(&u, pl->id);
        ub_s(&u, " base ");
        ub_hex(&u, pl->base, 5);
        ub_s(&u, " size ");
        ub_hex(&u, pl->size, 5);
        ub_s(&u, pl->att ? " attached" : " detached");
        ub_s(&u, pl->stay ? " stay " : " ");
        ub_s(&u, pl->name[0] ? pl->name : "(no name)");
        sb_log(u.b);
    }
    for (int d = 0; d < OC_NDOM; ++d) {
        const struct oc_dom *o = &OT.d[d];
        const char *how;
        uint32_t now = now_khz(d, &how);
        ub_init(&u);
        ub_s(&u, "oc ");
        ub_s(&u, oc_info[d].name);
        ub_s(&u, ": ect ");
        ub_dec(&u, o->nlv);
        ub_s(&u, " fv ");
        ub_dec(&u, o->fv_n);
        ub_s(&u, " init ");
        ub_dec(&u, o->fv_init);
        ub_s(&u, " cap ");
        ub_dec(&u, o->cap_khz);
        ub_s(&u, " boot ");
        ub_dec(&u, o->boot_khz);
        ub_s(&u, " now ");
        ub_dec(&u, now);
        ub_s(&u, " rows");
        for (unsigned r = 0; r < o->mm_rows; ++r) {
            ub_c(&u, r ? ',' : ' ');
            ub_dec(&u, o->mm_max[r] / 1000U);
        }
        sb_log(u.b);
        /* steps, kHz:uV, six a line (the ECT's list; ACPM-only steps marked a) */
        unsigned n = o->nlv ? o->nlv : o->fv_n;
        for (unsigned l = 0; l < n; l += 6) {
            ub_init(&u);
            ub_s(&u, "oc ");
            ub_s(&u, oc_info[d].name);
            ub_s(&u, o->nlv ? " " : " a ");
            for (unsigned k = l; k < n && k < l + 6; ++k) {
                uint32_t khz = o->nlv ? o->khz[k] : o->fv_khz[k];
                ub_c(&u, ' ');
                ub_dec(&u, khz);
                ub_c(&u, ':');
                ub_dec(&u, oct_uv(o, khz));
            }
            sb_log(u.b);
        }
        /* fvmap steps the ECT does not list */
        for (unsigned l = 0; o->nlv && l < o->fv_n; ++l)
            if (oct_level(o, o->fv_khz[l]) < 0) {
                ub_init(&u);
                ub_s(&u, "oc ");
                ub_s(&u, oc_info[d].name);
                ub_s(&u, " fv-only ");
                ub_dec(&u, o->fv_khz[l]);
                ub_c(&u, ':');
                ub_dec(&u, o->fv_uv[l]);
                sb_log(u.b);
            }
    }
}

/* ---- step 2: changing a step --------------------------------------------------------------------
 * Master plan 1.2 rule 3 as amended 2026-11-08 (MIF / INT allowed here only, listed steps only).
 * Every request: a step of the ECT that ACPM's fvmap also lists (sb_acpm_dvfs_listed checks it
 * again), confirmed (tier 3; tier 4 for MIF, INT and above-cap steps), sent only after the
 * watchdog is armed and storage writes are held; then 15 s to keep it (Power), else back to the
 * baseline ACPM reported before Engineer OS's first request. Both guards stay on until every
 * domain is back; a reset (reboot, boot-next, power off) puts every domain back first. */

#define OC_WDT_S 40U
#define OC_KEEP_S 15U
#define OC_CTRL(d) ((d) <= OC_G3D)          /* MIF, INT, A55, M3, G3D: controls and a stability test */

static uint32_t oc_base[OC_NDOM], oc_set[OC_NDOM];
static uint8_t oc_base_ok[OC_NDOM], oc_off[OC_NDOM];
static char oc_res[OC_NDOM][96];            /* the last stability test */
static int oc_res_ok[OC_NDOM];              /* 1 pass, 0 fail, -1 not run */

int oc_active(void)
{
    for (int d = 0; d < OC_NDOM; ++d)
        if (oc_off[d])
            return 1;
    return 0;
}

void oc_tick(void)
{
    if (oc_active())
        sb_wdt_feed(0);
}

/* The steps that may be sent: ECT levels ACPM also lists. */
static unsigned oc_steps(const struct oc_dom *o, uint32_t *out)
{
    unsigned n = 0;
    for (unsigned l = 0; l < o->nlv && n < OC_MAXLV; ++l)
        if (oct_allowed(o, o->khz[l]))
            out[n++] = o->khz[l];
    return n;
}

static void oc_reset_cb(void);

/* Set once MIF / INT or an above-cap step was sent, a stability test failed, a revert was not
 * confirmed or ACPM answered late: then storage stays held until the next restart (Engineer OS's own
 * RAM state, the fence's GPT bounds and the store's directory included, may have taken bit errors). */
static const char *oc_taint;

static void oc_tainted(const char *why)
{
    if (!oc_taint) {
        oc_taint = why;
        sb_logf("oc: storage held until restart: ", why);
    }
}

static void oc_guards(void)
{
    if (sb_acpm_dvfs_untrusted())
        oc_tainted("ACPM answered late or not at all");
    if (oc_active()) {
        sb_storage_hold("overclocking: a clock is off its default step");
        (void)sb_wdt_arm(OC_WDT_S);
        sb_before_reset = oc_reset_cb;
    } else {
        sb_storage_hold(oc_taint ? "restart before any storage write (overclocking)" : 0);
        sb_wdt_disarm();
        sb_before_reset = 0;
    }
}

/* The PLL agrees with step khz: 1 yes, 0 no, -1 no PLL readable (INT, a powered-off M3 or GPU). A
 * request takes a moment: up to 10 ms for the PLL to show it. */
static int pll_agrees(int d, uint32_t khz)
{
    const char *how;
    int want = oct_level(&OT.d[d], khz), r = -1;
    for (unsigned i = 0; i < 10; ++i) {
        uint32_t now = now_khz(d, &how);
        if (!now)
            return -1;
        r = step_of(d, now) == want;
        if (r)
            return 1;
        sb_udelay(1000);
    }
    return r;
}

static void log_dom(const char *what, int d, uint32_t khz)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "oc ");
    ub_s(&u, what);
    ub_c(&u, ' ');
    ub_s(&u, oc_info[d].name);
    ub_s(&u, " ");
    ub_dec(&u, khz);
    ub_s(&u, " kHz");
    audit_line(u.b);
}

/* The step a domain is on. This ACPM firmware answers FREQ_GET with 0 (9810 Linux reads the CMU
 * instead), so it comes from the PLL: kHz, 0 when unknown. Without a PLL to read (INT; M3 or G3D
 * powered off) it is the ECT's boot step and *assumed is set. */
static uint32_t oc_cur_step(int d, int *assumed)
{
    const struct oc_dom *o = &OT.d[d];
    const char *how;
    uint32_t now = now_khz(d, &how);
    *assumed = 0;
    if (now) {
        int l = step_of(d, now);
        return l >= 0 ? o->khz[l] : 0;
    }
    if (d == OC_MIF || d == OC_A55)         /* always readable: unreadable means unknown */
        return 0;
    *assumed = 1;
    return o->boot_khz;
}

/* Back to the baseline; 1 when the PLL shows it (or, with no PLL to read, when ACPM took the
 * request: storage then stays held until restart). */
static int oc_revert(int d)
{
    if (!oc_off[d])
        return 1;
    int sent = sb_acpm_dvfs_revert((uint32_t)d);
    int p = sent ? pll_agrees(d, oc_base[d]) : 0;
    int ok = sent && (p > 0 || (p < 0 && d != OC_MIF && d != OC_A55));
    log_dom(ok ? (p > 0 ? "reverted (PLL agrees)" : "reverted (no PLL to read)") : "REVERT NOT CONFIRMED", d, oc_base[d]);
    if (ok) {
        oc_off[d] = 0;
        oc_set[d] = oc_base[d];
        if (p < 0)
            oc_tainted("a revert that cannot be read back (no PLL)");
    } else {
        oc_tainted("a revert was not confirmed");
    }
    return ok;
}

static int oc_revert_all(void)
{
    int ok = 1;
    for (int d = 0; d < OC_NDOM; ++d)
        ok &= oc_revert(d);
    oc_guards();
    return ok;
}

static void oc_reset_cb(void)
{
    for (int d = 0; d < OC_NDOM; ++d)
        if (oc_off[d])
            (void)sb_acpm_dvfs_revert((uint32_t)d);
    sb_wdt_disarm();
    sb_log("oc: every domain sent back to its baseline before the reset");
}

/* the keep-or-revert countdown */
static int cd_d, cd_pll;
static uint32_t cd_khz, cd_left;

static void fill_cd(void)
{
    struct ub u, v;
    const char *how;
    ub_init(&u);
    ub_dec(&u, cd_left);
    ub_s(&u, " s");
    ub_init(&v);
    ub_s(&v, "Keep ");
    ub_s(&v, oc_info[cd_d].name);
    ub_s(&v, " at ");
    ub_mhz(&v, cd_khz);
    ub_s(&v, " MHz?");
    kv_bigc(v.b, u.b, C_ORANGE);
    kv_note("Press Power to keep it. Volume, Back or nothing for the countdown: it goes back to the step it "
            "was on.");
    kv_hdr("READ BACK");
    uint32_t now = now_khz(cd_d, &how);
    if (now) {
        ub_init(&u);
        ub_mhz(&u, now);
        ub_s(&u, " MHz (");
        ub_s(&u, how);
        ub_s(&u, ")");
        kv_addc("Clock", u.b, cd_pll > 0 ? C_GREEN : C_RED);
    } else {
        kv_addc("Clock", "no PLL to read here: not verifiable (storage held until restart)", C_ORANGE);
    }
    ub_init(&u);
    ub_mhz(&u, oc_base[cd_d]);
    ub_s(&u, " MHz");
    kv_add("Goes back to", u.b);
    kv_add("Guards", "watchdog armed, storage writes held");
}

static int oc_countdown(int d, uint32_t khz, int pll)
{
    uint64_t end = sb_ms() + OC_KEEP_S * 1000U;
    uint32_t shown = 0;
    cd_d = d;
    cd_khz = khz;
    cd_pll = pll;
    ui_input_reset();
    for (;;) {
        uint64_t t = sb_ms();
        if (t >= end)
            return 0;
        cd_left = (uint32_t)((end - t + 999U) / 1000U);
        if (cd_left != shown) {
            shown = cd_left;
            show_now("Keep this step?", fill_cd);
        }
        struct ui_ev e;
        ui_poll(&e, 50);
        if (e.type == EV_SELECT)
            return 1;
        if (e.type == EV_UP || e.type == EV_DOWN || e.type == EV_BACK || e.type == EV_HOME || e.type == EV_SWIPE_R)
            return 0;
    }
}

/* The stability test of a domain at its present step: 1 pass, 0 fail (then reverted), -1 not run. */
static int oc_test(int d)
{
    char det[96];
    unsigned bw = 0;
    int r = -1;
    det[0] = 0;
    switch (d) {
    case OC_A55: r = lab_oc_cpu(0x0fU, 60, det, sizeof(det)); break;
    case OC_M3: r = lab_oc_cpu(0xf0U, 60, det, sizeof(det)); break;
    case OC_G3D:
        if (lab_oc_gpu_up() || lab_oc_gpu_prepare())
            r = lab_oc_gpu(60, det, sizeof(det));
        break;
    case OC_MIF:
    case OC_INT: r = lab_oc_mem(1, 3, det, sizeof(det), &bw); break;
    default: return -1;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, r > 0 ? "PASS at " : r == 0 ? "FAIL at " : "not run at ");
    ub_mhz(&u, oc_set[d]);
    ub_s(&u, " MHz: ");
    ub_s(&u, det);
    sb_strcpy(oc_res[d], sizeof(oc_res[d]), u.b);
    oc_res_ok[d] = r;
    struct ub l;
    ub_init(&l);
    ub_s(&l, "oc test ");
    ub_s(&l, oc_info[d].name);
    ub_c(&l, ' ');
    ub_s(&l, u.b);
    audit_line(l.b);
    if (r == 0)
        oc_tainted("a stability test failed");
    if (r == 0 && oc_off[d]) {
        int back = oc_revert(d);
        oc_guards();
        (void)dialog_danger("Stability test failed", oc_res[d],
                            back ? "Back on its default step. Restart before saving anything: storage stays held." :
                                   "The revert was NOT confirmed: restart the phone (hold Power + Volume Down).",
                            "OK");
    } else if (r > 0) {
        ui_toast("Stability test passed", C_GREEN, 1500);
    }
    return r;
}

static void oc_apply(int d, uint32_t khz)
{
    struct oc_dom *o = &OT.d[d];
    uint32_t steps[OC_MAXLV], cur = 0;
    unsigned n = oc_steps(o, steps);
    struct ub u;
    if (!OC_CTRL(d) || !OT.fv_found || !oct_allowed(o, khz)) {
        log_dom("refused (not a listed step)", d, khz);
        (void)dialog_danger("Not a listed step", "Only steps in both the ECT and ACPM's own table are sent.",
                            "Nothing was sent.", "OK");
        return;
    }
    /* G3D: the GPU powered first, so its PLL can be read */
    if (d == OC_G3D && !lab_oc_gpu_up() && !lab_oc_gpu_prepare())
        return;
    /* the baseline: the step the PLL shows before Engineer OS's first request (INT, or a powered-off
     * M3: the ECT boot step), listed, at or below the cap; libsb's revert sends exactly it */
    if (!oc_base_ok[d]) {
        int assumed = 0;
        const char *bad = 0;
        cur = oc_cur_step(d, &assumed);
        if (!cur)
            bad = "the PLL does not show a listed step";
        else if (!oct_allowed(o, cur))
            bad = "the step is not in ACPM's table";
        else if (oct_above_cap(o, cur))
            bad = "the step is above Samsung's cap";
        else if (sb_acpm_dvfs_untrusted())
            bad = "ACPM answered late before (restart first)";
        else if (!sb_acpm_dvfs_set_baseline((uint32_t)d, cur))
            bad = "a request changed it since start, e.g. the GPU page (restart first)";
        if (bad) {
            ub_init(&u);
            ub_s(&u, bad);
            if (cur) {
                ub_s(&u, ": ");
                ub_dec(&u, cur);
                ub_s(&u, " kHz");
            }
            log_dom("refused (baseline not proven)", d, cur);
            (void)dialog_danger("No safe way back", u.b, "Without a proven default step to return to, nothing is "
                                "changed.", "OK");
            return;
        }
        oc_base[d] = oc_set[d] = cur;
        oc_base_ok[d] = 1;
        log_dom(assumed ? "baseline (boot step, no PLL)" : "baseline (PLL)", d, cur);
    }
    if (khz == oc_set[d])
        return;
    if (khz == oc_base[d]) {
        if (!oc_revert(d))
            (void)dialog_danger("Revert not confirmed", "The PLL does not show the default step.",
                                "Restart the phone (hold Power + Volume Down).", "OK");
        oc_guards();
        return;
    }
    int hi = oct_above_cap(o, khz), bus = d == OC_MIF || d == OC_INT;
    char tgt[8], nowv[24], newv[40];
    sb_strcpy(tgt, sizeof(tgt), oc_info[d].name);
    ub_init(&u);
    ub_mhz(&u, oc_set[d]);
    ub_s(&u, " MHz");
    sb_strcpy(nowv, sizeof(nowv), u.b);
    ub_init(&u);
    ub_mhz(&u, khz);
    ub_s(&u, " MHz, ");
    ub_mv(&u, oct_uv(o, khz));
    ub_s(&u, " mV (ACPM)");
    sb_strcpy(newv, sizeof(newv), u.b);
    struct action a = { "Overclocking (ACPM DVFS)", tgt,
                        { "ACPM DVFS request: a step of the chip's ECT that ACPM's own table lists; ACPM sets the "
                          "clock and its ASV voltage (no PLL or PMIC value from here)",
                          bus ? "Memory / bus clock: an unlisted INT value once froze the phone (green screen). A hang "
                                "now resets the phone (watchdog)" :
                                hi ? "Above Samsung's cap: the chip may be unstable or hot; a hang resets the phone "
                                     "(watchdog)" :
                                     "A hang resets the phone (watchdog)",
                          "Storage writes are held and the watchdog armed until every clock is back", 0 },
                        "back by itself after 15 s unless you keep it; any restart restores it", nowv, newv, "",
                        0, SB_SP_DVFS, 0, 0, (uint64_t)d, 1 };
    a.tier = bus || hi ? SB_TIER4 : SB_TIER3;
    struct sb_auth au;
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return;
    oc_off[d] = 1;                          /* guards on before anything is sent */
    oc_guards();
    log_dom("request", d, khz);
    if (bus || hi)
        oc_tainted(bus ? "the memory / bus clock was changed" : "an above-cap step was used");
    int sent = sb_acpm_dvfs_listed((uint32_t)d, khz, a.tier, steps, n);
    sb_gate_close();
    int pll = sent ? pll_agrees(d, khz) : 0;
    oc_set[d] = khz;
    ub_init(&u);
    ub_s(&u, !sent ? "ACPM did NOT answer" : pll > 0 ? "ACPM answered; the PLL shows the step" :
                     pll < 0 ? "ACPM answered; no PLL to read it back" : "ACPM answered; the PLL does NOT show the step");
    if (pll < 0 && sent)
        oc_tainted("a step that cannot be read back (no PLL)");
    if (!sent || !pll) {
        ub_s(&u, ": back to the default");
        int back = oc_revert(d);
        oc_guards();
        lab_major_result(a.title, tgt, u.b);
        (void)dialog_danger("Step not taken", u.b,
                            back ? "The domain is back on its default step." :
                                   "The revert was NOT confirmed: restart the phone (hold Power + Volume Down).",
                            "OK");
        return;
    }
    int keep = oc_countdown(d, khz, pll);
    ub_s(&u, keep ? ", kept" : ", not kept: reverted");
    lab_major_result(a.title, tgt, u.b);
    if (!keep) {                            /* back to the default, also from an earlier kept step */
        if (!oc_revert(d))
            (void)dialog_danger("Revert not confirmed", "The PLL does not show the default step.",
                                "Restart the phone (hold Power + Volume Down).", "OK");
        oc_guards();
        return;
    }
    oc_res[d][0] = 0;
    oc_res_ok[d] = -1;
    if (dialog("Stability test", "Run this domain's stability test at the new step now?",
               d == OC_MIF || d == OC_INT ? "Memory patterns, 64 MiB, 3 passes." : "60 s under load.", "RUN"))
        (void)oc_test(d);
}

/* the domain page with controls */
static int ctl_n;
static uint32_t ctl_khz[OC_MAXLV];

static void build_ctl(void)
{
    const int d = oc_cur;
    const struct oc_dom *o = &OT.d[d];
    struct ub u;
    if (oc_active())
        kv_bigc("Overclocking active", "storage held", C_ORANGE);
    kv_note("Tap a step to ask ACPM for it (confirmed first). It goes back by itself after 15 s unless you press "
            "Power. Only the chip's own steps are listed.");
    int ns = dom_head();
    ub_init(&u);
    if (oc_base_ok[d]) {
        ub_mhz(&u, oc_base[d]);
        ub_s(&u, " MHz (ACPM before the first request)");
    } else {
        ub_s(&u, "read from ACPM at the first request");
    }
    kv_add("Default step", u.b);
    if (oc_res[d][0])
        kv_addc("Stability test", oc_res[d], oc_res_ok[d] > 0 ? C_GREEN : oc_res_ok[d] == 0 ? C_RED : C_L2);
    unsigned under = 0;
    for (unsigned l = 0; l < o->nlv; ++l)
        under += oct_uv(o, o->khz[l]) && oct_undervolted(o, o->khz[l]);
    if (under) {
        ub_init(&u);
        ub_dec(&u, under);
        ub_s(&u, under == 1 ? " step is" : " steps are");
        ub_s(&u, " not offered: ACPM gives them less voltage than a slower step (not characterised for this "
                 "chip; MIF 2093 MHz froze the phone this way under iOS).");
        kv_note(u.b);
    }
    kv_hdr("STEPS: TAP ONE TO REQUEST IT");
    ctl_n = (int)oc_steps(o, ctl_khz);
    for (int i = 0; i < ctl_n; ++i) {
        uint32_t khz = ctl_khz[i];
        struct ub t;
        ub_init(&t);
        if (oc_off[d] ? khz == oc_set[d] : ns >= 0 && khz == o->khz[ns])
            ub_s(&t, "> ");
        ub_mhz(&t, khz);
        ub_s(&t, " MHz");
        ub_init(&u);
        ub_mv(&u, oct_uv(o, khz));
        ub_s(&u, " mV");
        if (oct_above_cap(o, khz) && o->cap_khz)
            ub_s(&u, " " U_DOT " above cap");
        else if (khz == o->cap_khz)
            ub_s(&u, " " U_DOT " cap");
        if (oc_base_ok[d] && khz == oc_base[d])
            ub_s(&u, " " U_DOT " default");
        row_add(t.b, u.b, (o->cap_khz && khz > o->cap_khz) || d == OC_MIF || d == OC_INT ? R_WARN : 0);
    }
    kv_hdr("TEST AND UNDO");
    row_add("Stability test at the current step",
            d == OC_MIF || d == OC_INT ? "memory patterns, 64 MiB, 3 passes" :
            d == OC_G3D ? "GPU stress, 60 s" : "CPU stress on this cluster, 60 s", 0);
    row_add("Back to the default step", oc_off[d] ? "ACPM's step before the first request" : "already on it", 0);
}

static int act_ctl(int r)
{
    const int d = oc_cur;
    if (r < ctl_n) {
        oc_apply(d, ctl_khz[r]);
        return 0;
    }
    switch (r - ctl_n) {
    case 0: (void)oc_test(d); break;
    case 1:
        if (!oc_revert(d))
            (void)dialog_danger("Revert not confirmed", "The PLL does not show the default step.",
                                "Restart the phone (hold Power + Volume Down).", "OK");
        oc_guards();
        break;
    default: break;
    }
    return 0;
}

/* ---- the hub ------------------------------------------------------------------------------------- */

static int oc_rowdom[OC_NDOM], oc_nrow, oc_nact;

static void build_oc(void)
{
    struct ub u;
    oc_ensure();
    if (oc_active())
        kv_bigc("OVERCLOCKING ACTIVE", "watchdog armed, storage held", C_ORANGE);
    else if (oc_taint)
        kv_bigc("RESTART BEFORE SAVING", oc_taint, C_RED);
    kv_note("Every clock domain's steps from the chip's own tables, ACPM's voltage per step and Samsung's cap. "
            "MIF, INT, A55, M3 and G3D can be set to one of their own steps (confirmed, back after 15 s unless "
            "kept); the others are read only.");
    if (!ot_ect_ok)
        kv_monoc("No ECT found at 0xA0000000: no tables.", C_RED);
    kv_hdr("DOMAINS");
    oc_nrow = 0;
    for (int d = 0; d < OC_NDOM; ++d) {
        const struct oc_dom *o = &OT.d[d];
        if (!o->nlv && !o->fv_n)
            continue;
        const char *how;
        uint32_t now = now_khz(d, &how);
        unsigned n = o->nlv ? o->nlv : o->fv_n;
        const uint32_t *k = o->nlv ? o->khz : o->fv_khz;
        ub_init(&u);
        ub_dec(&u, n);
        ub_s(&u, " steps ");
        ub_mhz(&u, k[n - 1]);
        ub_s(&u, "-");
        ub_mhz(&u, k[0]);
        if (o->cap_khz) {
            ub_s(&u, " " U_DOT " cap ");
            ub_mhz(&u, o->cap_khz);
        }
        if (now) {
            int ns = step_of(d, now);
            ub_s(&u, " " U_DOT " now ");
            ub_mhz(&u, ns >= 0 ? o->khz[ns] : now);         /* MIF: the step, not PLL_MIF's 2x */
            if (ns < 0)
                ub_s(&u, " (PLL)");
        }
        struct ub t;
        ub_init(&t);
        ub_s(&t, oc_info[d].name);
        if (oc_off[d]) {
            ub_s(&t, "  set to ");
            ub_mhz(&t, oc_set[d]);
        } else {
            ub_s(&t, above_any(o) ? "  (steps above cap)" : "");
        }
        oc_rowdom[oc_nrow++] = d;
        row_add(t.b, u.b, oc_off[d] ? R_WARN : 0);
    }
    oc_nact = 0;
    if (oc_active()) {
        kv_hdr("UNDO");
        row_add("Every domain back to its default step", "then the watchdog off and storage writes allowed", R_WARN);
        oc_nact = 1;
    }
    kv_hdr("SOURCES");
    ub_init(&u);
    ub_s(&u, ot_ect_ok ? "ECT " : "no ECT");
    if (ot_ect_ok)
        ub_s(&u, OT.ect_ver);
    ub_s(&u, ot_fv_at != OC_NONE ? ", ACPM fvmap found" : ", no ACPM fvmap");
    row_add("Where the tables come from", u.b, 0);
    row_add("Write the tables to the log", "pmsg ring in RAM; nothing on storage", 0);
    row_add("Read the tables again", "ECT and APM SRAM", 0);
}

static int act_oc(int r)
{
    if (r < oc_nrow) {
        oc_cur = oc_rowdom[r];
        struct ub t;
        ub_init(&t);
        ub_s(&t, oc_info[oc_cur].name);
        ub_s(&t, " steps");
        static char title[24];
        sb_strcpy(title, sizeof(title), t.b);
        if (OC_CTRL(oc_cur) && OT.fv_found)
            list_screen(title, build_ctl, act_ctl);
        else
            info_screen(title, fill_dom, 0);
        return 0;
    }
    if (r < oc_nrow + oc_nact) {
        if (!oc_revert_all())
            (void)dialog_danger("Revert not confirmed", "ACPM did not report every default step back.",
                                "Restart the phone (hold Power + Volume Down).", "OK");
        return 0;
    }
    switch (r - oc_nrow - oc_nact) {
    case 0: info_screen("Table sources", fill_src, 0); break;
    case 1:
        log_tables();
        (void)dialog("Tables logged", "Every domain's steps are in the Engineer OS log and the pmsg ring.",
                     "Logs > Engineer OS log shows them.", "OK");
        break;
    case 2: oc_load(); break;
    default: break;
    }
    return 0;
}

void oc_page(void)
{
    list_screen("Overclocking", build_oc, act_oc);
}

#ifdef OC_SELFTEST
/* The read-only self-test image, unattended: the watchdog armed first, a breadcrumb before each read
 * block, tables to pmsg first (so a hang later still leaves them), then
 * ACPM FREQ_GET (reads) for the five domains with controls, each next to its PLL step and boot step:
 * the check that FREQ_GET reports the live step before any change trusts it as the way back. Then
 * the watchdog back as S-Boot left it and a reboot to recovery; it never returns. */
void oc_selftest_boot(void)
{
    /* unattended: the cluster 1 watchdog (60 s) before the first ECT / SRAM / ACPM read, fed only
     * between read blocks; at the end straight to recovery (TWRP pulls pmsg) */
    oc_crumbs = 1;
    sb_log("oc selftest: start (read only: ECT, APM SRAM, PLLs, FREQ_GET); watchdog 60 s, then recovery");
    (void)sb_wdt_arm(60);
    oc_load();
    crumb("PLL reads and the table log");
    log_tables();
    sb_log("oc selftest: tables done; FREQ_GET (diagnostic: this firmware answers 0) and the PLL default per domain next");
    static const int gd[5] = { OC_A55, OC_M3, OC_G3D, OC_MIF, OC_INT };
    for (int i = 0; i < 5; ++i) {
        const int d = gd[i];
        const struct oc_dom *o = &OT.d[d];
        uint32_t khz = 0;
        const char *how;
        struct ub u;
        crumb(oc_info[d].name);             /* "next: MIF" before its FREQ_GET */
        ub_init(&u);
        ub_s(&u, "oc selftest: FREQ_GET ");
        ub_s(&u, oc_info[d].name);
        if (sb_acpm_dvfs_get((uint32_t)d, &khz)) {
            ub_s(&u, " = ");
            ub_dec(&u, khz);
            int l = oct_level(o, khz);
            ub_s(&u, l >= 0 ? " step " : " NOT LISTED");
            if (l >= 0)
                ub_dec(&u, (unsigned)l);
            ub_s(&u, oct_above_cap(o, khz) ? " ABOVE CAP" : "");
        } else {
            ub_s(&u, ": no answer");
        }
        uint32_t now = now_khz(d, &how);
        ub_s(&u, "; pll ");
        ub_dec(&u, now);
        if (now) {
            int s = step_of(d, now);
            ub_s(&u, " step ");
            if (s >= 0)
                ub_dec(&u, (unsigned)s);
            else
                ub_c(&u, '-');
        }
        ub_s(&u, "; boot ");
        ub_dec(&u, o->boot_khz);
        ub_s(&u, " init ");
        ub_dec(&u, o->fv_init < o->fv_n ? o->fv_khz[o->fv_init] : 0);
        sb_log(u.b);
        /* the step 2 go / no-go: the default step it would take, from the PLL (FREQ_GET answers 0 here) */
        int assumed = 0;
        uint32_t cur = oc_cur_step(d, &assumed);
        ub_init(&u);
        ub_s(&u, "oc selftest: default ");
        ub_s(&u, oc_info[d].name);
        ub_s(&u, " = ");
        ub_dec(&u, cur);
        ub_s(&u, assumed ? " (boot step, no PLL)" : " (PLL)");
        ub_s(&u, !cur ? " UNKNOWN" : !oct_allowed(o, cur) ? " NOT IN ACPM TABLE" : oct_above_cap(o, cur) ? " ABOVE CAP" :
                                                                                                    " ok");
        sb_log(u.b);
    }
    sb_logx("oc selftest: ACPM late or missing answers ", sb_acpm_dvfs_untrusted());
    sb_log("oc selftest: done; rebooting to recovery");
    __asm__ volatile("dsb sy" : : : "memory");  /* pmsg is in RAM: every line out before the reset */
    sb_wdt_disarm();                        /* S-Boot's masked state back, so no reset fires in TWRP */
    sb_reboot(SB_INFORM_RECOVERY);
}
#endif
