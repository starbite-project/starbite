/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha5 probe H1 (starbite-os-heatprobe.img, -DSBOS_SELFTEST_PROBE; docs/STARBITE-OS-1.2.md "alpha5"):
 * the readings the hotter Survival needs, logged to pmsg, then a restart into recovery. Nothing is written that
 * the normal image does not already write:
 *   1. ACPM's fvmap (APM SRAM 0x1D300 on this phone, Engineer OS ocself-4): every A55, M3 and G3D level with ACPM's
 *      voltage (read only);
 *   2. the S2MPS18's 256 registers (type 1; read only), so the A55 / M3 bucks can be identified;
 *   3. the A55 cluster to 1794 and to 1950 MHz (both levels Survival already sets) with the S2MPS18 registers that
 *      changed after each, then back to the clock it had;
 *   4. the screen's share of the battery current: 8 s each of the home screen at the user's level, full white at
 *      that level, full white at the panel's top level (Survival's boost, AOR 00 0C), then the home screen again;
 *   5. the panel's registers HBM would need (panel_dump: ID, CA, B3, B5, F4, B1, B8; plus ACL 55 / B4), read only.
 * Its own 30 s watchdog (fed by the loop) restarts a hung phone. */
#include "sb.h"
#include "kern.h"

/* linked into every image (only the probe variant's uitest.c calls it) */

#define FV_AT 0x1D300U                      /* APM SRAM: the DVFS plugin 0x16C00 + fvmap_offset 0x6700 (ocself-4) */
#define FV_SIZE 0x2000U
#define FV_NDOM 13U
#define PMIC_T 1U                           /* the S2MPS18's register page ACPM calls type 1 (BUCK6 0x2B = vdd_g3d) */

static uint8_t fv[FV_SIZE] __attribute__((aligned(8)));
static uint8_t regs0[256], regs1[256];
static int ph, sub, pm_ok;
static uint64_t t_ph;
static int64_t acc_ma, acc_mv;
static int nsamp;
static int ma_avg[4], mv_avg[4];
static uint32_t a55_was;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* one domain's table: "probe: fvmap A55 (14 levels, init 6): 1950:1093.75 1898:1075 ..." in lines of 6 */
static void fv_log(unsigned d, const char *name)
{
    const uint8_t *h = fv + d * 24U;
    unsigned nlv = h[1], o = (unsigned)h[20] | (unsigned)h[21] << 8;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "probe: fvmap ");
    ub_s(&u, name);
    if (!nlv || nlv > 32 || o < FV_NDOM * 24U || o + nlv * 8U > FV_SIZE) {
        ub_s(&u, ": no table (header ");
        ub_hex(&u, le32(h), 8);
        ub_c(&u, ')');
        sb_log(u.b);
        return;
    }
    ub_s(&u, " (");
    ub_dec(&u, nlv);
    ub_s(&u, " levels, init ");
    ub_dec(&u, h[8]);
    ub_s(&u, "), MHz:mV");
    sb_log(u.b);
    for (unsigned l = 0; l < nlv; l += 6) {
        ub_init(&u);
        ub_s(&u, "probe: fvmap ");
        ub_s(&u, name);
        ub_s(&u, ":");
        for (unsigned k = l; k < nlv && k < l + 6; ++k) {
            uint32_t khz = le32(fv + o + 8 * k), uv = le32(fv + o + 8 * k + 4);
            ub_c(&u, ' ');
            ub_dec(&u, khz / 1000U);
            ub_c(&u, ':');
            ub_dec(&u, uv / 1000U);
            if (uv % 1000U) {
                ub_c(&u, '.');
                ub_dec(&u, uv % 1000U / 10U);
            }
        }
        sb_log(u.b);
    }
}

static int pmic_all(uint8_t *r)
{
    int n = 0;
    for (unsigned i = 0; i < 256; ++i) {
        r[i] = 0;
        n += sb_pmic_read(PMIC_T, i, &r[i]);
    }
    return n;
}

static void pmic_dump(const uint8_t *r)
{
    for (unsigned b = 0; b < 256; b += 32) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "probe: s2mps18 ");
        ub_hex(&u, b, 2);
        ub_c(&u, ':');
        for (unsigned i = b; i < b + 32; ++i) {
            ub_c(&u, ' ');
            ub_hexn(&u, r[i], 2);
        }
        sb_log(u.b);
    }
}

/* the registers that changed since the reference, as reg=old>new (a buck at 300 + 6.25 mV a step: also mV) */
static void pmic_diff(const char *what, const uint8_t *a, const uint8_t *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "probe: s2mps18 changed after ");
    ub_s(&u, what);
    ub_c(&u, ':');
    int n = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (a[i] == b[i])
            continue;
        ub_c(&u, ' ');
        ub_hexn(&u, i, 2);
        ub_c(&u, '=');
        ub_hexn(&u, a[i], 2);
        ub_c(&u, '>');
        ub_hexn(&u, b[i], 2);
        ub_s(&u, " (");
        ub_dec(&u, (300000U + 6250U * b[i]) / 1000U);
        ub_s(&u, " mV?)");
        ++n;
    }
    if (!n)
        ub_s(&u, " none");
    sb_log(u.b);
}

static void a55_step(uint32_t khz, const char *what)
{
    power_heater_took_clocks();
    int ok = hw_dvfs(DVFS_A55, khz);
    sb_mdelay(5);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "probe: A55 to ");
    ub_dec(&u, khz / 1000U);
    ub_s(&u, ok ? " MHz: ACPM answered, PLL " : " MHz: ACPM DID NOT ANSWER, PLL ");
    ub_dec(&u, hw_a55_khz() / 1000U);
    ub_s(&u, " MHz");
    sb_log(u.b);
    (void)pmic_all(regs1);
    pmic_diff(what, regs0, regs1);
}

static const char *const scr_name[4] = { "the home screen (the user's level)", "full white (the user's level)",
                                         "full white at the panel's top (Survival)", "the home screen again" };

static void scr_set(int k)
{
    if (k == 0 || k == 3) {
        kui_light(0);
        panel_boost(4, 0);
        kui_home();
    } else if (k == 1) {
        kui_light(0xffffffffU);
    } else {
        panel_boost(4, 1);
    }
}

/* 0 while it runs, 1 done */
int heatprobe_step(void)
{
    uint64_t now = sb_ms();
    switch (ph) {
    case 0: {
        sb_log("probe: start: H1, read only (fvmap, the S2MPS18, the A55 buck, the screen's current, the panel)");
        int ok = sb_acpm_sram_read(FV_AT, fv, FV_SIZE);
        if (!ok) {
            sb_log("probe: fvmap: APM SRAM 0x1D300 not readable");
        } else {
            fv_log(2, "A55");
            fv_log(3, "M3");
            fv_log(4, "G3D");
            fv_log(0, "MIF");
        }
        ph = 1;
        return 0;
    }
    case 1:
        pm_ok = pmic_all(regs0);
        sb_logx("probe: s2mps18 registers read (of 256): ", (uint64_t)pm_ok);
        pmic_dump(regs0);
        a55_was = hw_a55_khz();
        sb_logx("probe: A55 now, kHz (PLL): ", a55_was);
        ph = 2;
        return 0;
    case 2:
        if (pm_ok > 128) {
            a55_step(1794000U, "A55 1794 MHz");
            a55_step(1950000U, "A55 1950 MHz");
            uint32_t back = a55_was >= 455000U && a55_was <= 1950000U ? a55_was : 1053000U;
            /* the PLL reads the level ACPM set; back to the nearest listed level at or below it */
            static const uint32_t lv[] = { 1950000, 1898000, 1794000, 1690000, 1456000, 1248000, 1053000, 949000,
                                           832000, 715000, 598000, 455000 };
            uint32_t to = 455000U;
            for (unsigned i = 0; i < sizeof(lv) / sizeof(lv[0]); ++i)
                if (lv[i] <= back + back / 50U) {
                    to = lv[i];
                    break;
                }
            a55_step(to, "the A55 back");
        } else {
            sb_log("probe: the S2MPS18 did not answer: no A55 step");
        }
        ph = 3;
        sub = 0;
        scr_set(0);
        t_ph = now;
        acc_ma = acc_mv = 0;
        nsamp = 0;
        return 0;
    case 3: {
        /* 8 s per screen: 2 s to settle, then a gauge sample every turn (the self-test's 100 ms step) */
        if (now - t_ph >= 2000U) {
            struct sb_bat b;
            if (sb_bat_read(&b) && b.ok) {
                acc_ma += b.ma;
                acc_mv += b.mv;
                ++nsamp;
            }
        }
        if (now - t_ph < 8000U)
            return 0;
        ma_avg[sub] = nsamp ? (int)(acc_ma / nsamp) : 0;
        mv_avg[sub] = nsamp ? (int)(acc_mv / nsamp) : 0;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "probe: screen: ");
        ub_s(&u, scr_name[sub]);
        ub_s(&u, ": ");
        ub_sdec(&u, ma_avg[sub]);
        ub_s(&u, " mA at ");
        ub_dec(&u, (uint64_t)mv_avg[sub]);
        ub_s(&u, " mV (");
        ub_dec(&u, (uint64_t)nsamp);
        ub_s(&u, " samples)");
        sb_log(u.b);
        if (++sub < 4) {
            scr_set(sub);
            t_ph = now;
            acc_ma = acc_mv = 0;
            nsamp = 0;
            return 0;
        }
        int base = (ma_avg[0] + ma_avg[3]) / 2;
        ub_init(&u);
        ub_s(&u, "probe: screen: white costs ");
        ub_sdec(&u, base - ma_avg[1]);
        ub_s(&u, " mA more than the home screen; white at the top ");
        ub_sdec(&u, base - ma_avg[2]);
        ub_s(&u, " mA more (about ");
        ub_sdec(&u, (int64_t)(base - ma_avg[2]) * mv_avg[2] / 1000);
        ub_s(&u, " mW)");
        sb_log(u.b);
        ph = 4;
        return 0;
    }
    case 4:
        (void)panel_dump();
        (void)panel_dump_acl();
        sb_log("probe: passed: H1 done (read only); restart into recovery in 3 s");
        ph = 5;
        return 1;
    default:
        return 1;
    }
}
