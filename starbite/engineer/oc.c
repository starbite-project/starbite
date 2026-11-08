/* SPDX-License-Identifier: GPL-2.0-only
 * Overclocking, step 1 (docs/ENGINEER-OS.md section 14): READ ONLY. Every DVFS domain's table as
 * the chip itself has it, nothing changed:
 *  - the frequency steps and Samsung's caps from S-Boot's ECT (DRAM 0xA0000000, read in place),
 *  - ACPM's voltage per step from its fvmap in APM SRAM (32-bit reads, faults caught),
 *  - the current clock from the PLL registers where libsb can read it (A55, M3, G3D, MIF).
 * No ACPM message is sent from this page. The OC_SELFTEST image (ENG_DEFS=-DOC_SELFTEST) logs the
 * tables to pmsg at start and, after that, asks ACPM FREQ_GET (a read) for A55 / M3 / G3D only.
 * Requests (step 2) are not in this build.
 */
#include "eng.h"
#include "oc.h"

#define ECT_BASE 0xa0000000UL

static struct oc_tables OT;
static int ot_loaded, ot_ect_ok;
static uint32_t ot_ect_at = OC_NONE, ot_ect_size, ot_fv_at = OC_NONE;
static int ot_fv_scanned;                   /* 1: found by scanning APM SRAM, not at the dtsi offset */
static uint8_t ot_sram[0x30000];            /* ACPM_SRAM_SIZE: the scan copy */

static uint32_t rd32(uintptr_t a)
{
    uint32_t v = 0;
    (void)sb_probe32(a, &v);
    return v;
}

static void oc_load(void)
{
    const uint8_t *win = (const uint8_t *)ECT_BASE;
    ot_loaded = 1;
    ot_ect_ok = 0;
    ot_ect_at = ot_fv_at = OC_NONE;
    ot_fv_scanned = 0;
    for (int d = 0; d < OC_NDOM; ++d)
        OT.d[d].fv_n = 0;
    OT.fv_found = 0;
    OT.asv_ver = 0;
    OT.asv_known = 0;                       /* not read yet: the lowest cap over the rows is used */
    ot_ect_at = oct_ect_find(win, OC_ECT_MAX);
    if (ot_ect_at != OC_NONE) {
        const uint8_t *e = win + ot_ect_at;
        ot_ect_size = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        ot_ect_ok = oct_parse_ect(e, OC_ECT_MAX - ot_ect_at, &OT) > 0;
    }
    if (!ot_ect_ok)
        return;
    /* the fvmap: first at the dtsi offset, else anywhere in APM SRAM (each candidate must carry the
     * ECT's A55, M3 and G3D steps, so a wrong place is not taken) */
    if (sb_acpm_sram_read(OC_FVMAP_OFFSET, ot_sram, OC_FVMAP_SIZE) &&
        oct_parse_fvmap(ot_sram, OC_FVMAP_SIZE, &OT)) {
        ot_fv_at = OC_FVMAP_OFFSET;
        return;
    }
    uint32_t n = 0;
    while (n < sizeof(ot_sram) && sb_acpm_sram_read(n, ot_sram + n, 0x1000))
        n += 0x1000;
    for (uint32_t o = 0; o + OC_NDOM * 24U <= n; o += 4) {
        uint32_t len = n - o < OC_FVMAP_SIZE ? n - o : OC_FVMAP_SIZE;
        if (oct_parse_fvmap(ot_sram + o, len, &OT)) {
            ot_fv_at = o;
            ot_fv_scanned = 1;
            return;
        }
    }
}

static void oc_ensure(void)
{
    if (!ot_loaded)
        oc_load();
}

int oc_active(void)
{
    return 0;                               /* step 1 changes nothing */
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

static void fill_dom(void)
{
    const int d = oc_cur;
    const struct oc_dom *o = &OT.d[d];
    struct ub u;
    const char *how;
    uint32_t now = now_khz(d, &how);
    int ns = now ? step_of(d, now) : -1;

    kv_note("Read only. These are the chip's own steps; nothing here changes a clock.");
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
            "policy. This version only reads.");
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
        ub_hex(&u, ot_fv_at, 4);
        ub_s(&u, ot_fv_scanned ? " (found by a scan)" : " (the dtsi offset)");
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
        ub_hex(&u, ot_fv_at, 4);
        ub_s(&u, ot_fv_scanned ? " (scan)" : " (dtsi)");
    }
    sb_log(u.b);
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

/* ---- the hub ------------------------------------------------------------------------------------- */

static int oc_rowdom[OC_NDOM], oc_nrow;

static void build_oc(void)
{
    struct ub u;
    oc_ensure();
    kv_note("Read only: every clock domain's steps from the chip's own tables, ACPM's voltage per step and "
            "Samsung's cap. Nothing on this page changes a clock.");
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
        ub_s(&t, above_any(o) ? "  (steps above cap)" : "");
        oc_rowdom[oc_nrow++] = d;
        row_add(t.b, u.b, 0);
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
        info_screen(title, fill_dom, oc_cur <= OC_G3D ? 1000 : 0);
        return 0;
    }
    switch (r - oc_nrow) {
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
/* The read-only self-test image: tables to pmsg first (so a hang later still leaves them), then
 * ACPM FREQ_GET for A55, M3, G3D (reads; sb_acpm_dvfs_get refuses every other domain). */
void oc_selftest_boot(void)
{
    sb_log("oc selftest: start (read only: ECT, APM SRAM, PLLs)");
    oc_load();
    log_tables();
    sb_log("oc selftest: tables done; FREQ_GET A55, M3, G3D next");
    static const int gd[3] = { OC_A55, OC_M3, OC_G3D };
    for (int i = 0; i < 3; ++i) {
        uint32_t khz = 0;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "oc selftest: FREQ_GET ");
        ub_s(&u, oc_info[gd[i]].name);
        if (sb_acpm_dvfs_get((uint32_t)gd[i], &khz)) {
            ub_s(&u, " = ");
            ub_dec(&u, khz);
            int s = step_of(gd[i], khz);
            ub_s(&u, s >= 0 ? " kHz, step " : " kHz, not a listed step");
            if (s >= 0)
                ub_dec(&u, (unsigned)s);
        } else {
            ub_s(&u, ": no answer");
        }
        sb_log(u.b);
    }
    sb_log("oc selftest: done");
}
#endif
