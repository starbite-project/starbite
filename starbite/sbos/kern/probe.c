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

/* ---- H2 (1.2-alpha12, starbite-os-gputest.img, -DSBOS_SELFTEST_GPU): the GPU above ACPM's 598 MHz ----------------
 * The GPU heat job (gpuheat.c: vdd_g3d held at 950 mV, 18 cores, pixels checked after each chain) at 598 MHz, then
 * PLL_G3D (CMU_G3D 0x17400140, FOUT = 26 MHz x M / (P << S)) set directly for 650, 702, 754, 806 and 858 MHz, 30 s
 * each: the GPU's clock to the 26 MHz oscillator (PLL_CON0 bit 4), the PLL off, M written, the PLL on, its lock
 * (bit 29) awaited (at most 5 ms), the GPU back on the PLL. A step passes with the job still running, no fault, the
 * lock held and the GPU zone below 110 C. The first failure or 110 C ends the ladder: the PLL back to its 598 MHz
 * word, ACPM asked for 598 again (its own state), the GPU heat stopped. vdd_g3d stays 950 mV throughout (ACPM's
 * table: 598 MHz at 731 mV, so 950 is ~220 mV above it); custom voltages above 950 come in a later step. */
#define G3D_CON0 0x17400140UL
static const unsigned g_steps[] = { 650, 702, 754, 806, 858 };
static int gph, gstep;
static uint32_t g_con0_598;
static uint64_t g_t;
static int64_t g_acc;
static int g_n, g_best;
static uint64_t g_jobs0;

/* 1.2-alpha14 (H2c): the GPU's real clock from Mali's cycle counter (GPU_COMMAND 0x30 = 5 starts it; CYCLE_COUNT
 * 0x90 / 0x94) against the arch timer over 20 ms, MHz */
static unsigned g_mhz_measured(void)
{
    uint32_t lo0 = sb_gpu_rd(0x90), hi0 = sb_gpu_rd(0x94);
    uint64_t t0 = sb_us();
    sb_udelay(20000);
    uint32_t lo1 = sb_gpu_rd(0x90), hi1 = sb_gpu_rd(0x94);
    uint64_t dt = sb_us() - t0;
    uint64_t c0 = (uint64_t)hi0 << 32 | lo0, c1 = (uint64_t)hi1 << 32 | lo1;
    return dt && c1 > c0 ? (unsigned)((c1 - c0) / dt) : 0;
}

static uint32_t g_rd(uintptr_t a)
{
#ifndef SB_HOST
    return *(volatile uint32_t *)a;
#else
    (void)a;
    return 0xa0000000U | 23U << 16 | 1U << 8 | 1U | 0x10U;   /* host: M 23, P 1, S 1: 299 MHz */
#endif
}

static void g_wr(uintptr_t a, uint32_t v)
{
#ifndef SB_HOST
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)a;
    (void)v;
#endif
}

/* 1.2-alpha13 (H2b): H2 asked M 200 with P 4, S 1 (VCO 1300 MHz) and the PLL never locked: the 1018X's VCO range is
 * 600 .. 1200 MHz (vendor cal-if pll_spec.c gpll1018X_spec: P 1..63, M 64..1023, S 0..6, Fref 2..8 MHz, lock time 150
 * Fref cycles a P), and 598 MHz already runs it at 1196 (S 1). Above 600 MHz: S 0, the VCO is the clock itself. The
 * vendor's own sequence (cal-if ra.c ra_enable_pll / ra_pll_set_pmsk): the output mux to the oscillator (MUX_SEL 0,
 * then MUX_BUSY bit 7 clear), the PLL off, the lock time P x 150 into PLL_LOCKTIME (0x17400000), then one write of P,
 * M, S with MUX_SEL and ENABLE, and STABLE (bit 29) awaited. 1 locked. */
#define G3D_LOCK 0x17400000UL
static int g_pms_ok(uint32_t p, uint32_t m, uint32_t s)
{
    uint64_t fref_khz = p ? 26000U / p : 0, vco_mhz = p ? 26ULL * m / p : 0;
    return p >= 1 && p <= 63 && m >= 64 && m <= 1023 && s <= 6 && fref_khz >= 2000 && fref_khz <= 8000 &&
           vco_mhz >= 600 && vco_mhz <= 1200;
}

static int g_set_pms(uint32_t p, uint32_t m, uint32_t s)
{
    if (!g_pms_ok(p, m, s))
        return 0;
    uint32_t v = g_rd(G3D_CON0);
    g_wr(G3D_CON0, v & ~0x10U);                         /* the output to the oscillator */
#ifndef SB_HOST
    for (unsigned us = 0; us < 100 && (g_rd(G3D_CON0) & 0x80U); ++us)
        sb_udelay(1);                                   /* MUX_BUSY */
#endif
    v = g_rd(G3D_CON0) & ~(0x80000000U | 0x10U);
    g_wr(G3D_CON0, v);                                  /* the PLL off */
    g_wr(G3D_LOCK, p * 150U);                           /* the lock time, as the vendor writes it */
    v = (v & ~((0x3ffU << 16) | (0x3fU << 8) | 7U)) | m << 16 | p << 8 | s;
    g_wr(G3D_CON0, v | 0x80000000U | 0x10U);            /* P, M, S, MUX_SEL and ENABLE in one write */
    int locked = 0;
#ifndef SB_HOST
    for (unsigned us = 0; us < 2000; us += 5) {
        if (g_rd(G3D_CON0) & 0x20000000U) {
            locked = 1;
            break;
        }
        sb_udelay(5);
    }
#else
    locked = 1;
#endif
    return locked;
}

static void g_back(const char *why)
{
    uint32_t m = (g_con0_598 >> 16) & 0x3ffU, p = (g_con0_598 >> 8) & 0x3fU, s = g_con0_598 & 7U;
    if (m)
        (void)g_set_pms(p, m, s);
    (void)hw_dvfs(4, 598000U);
    sb_logf("probe: gpu: back at 598 MHz: ", why);
}

static int g_temp_c(void)
{
    struct sb_temps t;
    sb_temps_read(&t);
    return t.sub == 1 ? t.gpu : -100;
}

int gpuprobe_step(void)
{
    uint64_t now = sb_ms();
    if (gph > 0 && gph < 9)
        gpuheat_tick();
    switch (gph) {
    case 0:
        sb_log("probe: start: H2c, the GPU above 598 MHz (its cycle counter, jobs a second) (PLL_G3D at S 0, the vendor's sequence, vdd_g3d 950 mV, 30 s a step)");
        gpu_heat_session = 1;
        gpuheat_khz = 598000U;
        if (!gpuheat_start()) {
            sb_log("probe: FAILED: H2: the GPU heat did not start");
            gph = 9;
            return 0;
        }
        g_con0_598 = g_rd(G3D_CON0);
        sb_logx("probe: gpu: PLL_LOCKTIME ", g_rd(G3D_LOCK));
        (void)sb_gpu_wr(0x30, 5);           /* CYCLE_COUNT_START */
        g_jobs0 = gpuheat_jobs();
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "probe: gpu: PLL_G3D at 598: ");
            ub_hex(&u, g_con0_598, 8);
            ub_s(&u, " (M ");
            ub_dec(&u, (g_con0_598 >> 16) & 0x3ffU);
            ub_s(&u, " P ");
            ub_dec(&u, (g_con0_598 >> 8) & 0x3fU);
            ub_s(&u, " S ");
            ub_dec(&u, g_con0_598 & 7U);
            ub_s(&u, ")");
            sb_log(u.b);
        }
        gstep = -1;
        g_t = now;
        g_acc = 0;
        g_n = 0;
        g_best = 598;
        gph = 1;
        return 0;
    case 1: {                                           /* running a step (-1: 598 itself) */
        struct sb_bat b;
        if (now - g_t >= 5000U && sb_bat_read(&b) && b.ok) {
            g_acc += b.ma;
            ++g_n;
        }
        int tc = g_temp_c();
        unsigned mhz = gstep < 0 ? 598U : g_steps[gstep];
        int locked = (g_rd(G3D_CON0) & 0x20000000U) != 0;
        if (!gpuheat_on() || !locked || tc >= 110) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "probe: gpu: ");
            ub_dec(&u, mhz);
            ub_s(&u, !gpuheat_on() ? " MHz FAILED: the GPU job stopped (a fault or a wrong pixel) after " :
                     !locked ? " MHz FAILED: the PLL lost its lock after " : " MHz: the GPU zone reached 110 C after ");
            ub_dec(&u, (now - g_t) / 1000U);
            ub_s(&u, " s");
            sb_log(u.b);
            g_back(tc >= 110 ? "the temperature" : "a failed step");
            gph = 8;
            g_t = now;
            return 0;
        }
        if (now - g_t < 30000U)
            return 0;
        {
            uint64_t j = gpuheat_jobs();
            struct ub u;
            ub_init(&u);
            ub_s(&u, "probe: gpu: ");
            ub_dec(&u, mhz);
            ub_s(&u, " MHz: the GPU's own cycle counter says ");
            ub_dec(&u, g_mhz_measured());
            ub_s(&u, " MHz; ");
            ub_dec(&u, (j - g_jobs0) / 30U);
            ub_s(&u, " fragment jobs a second (1 Mpixel each); G3D mux ");
            ub_hex(&u, g_rd(0x17401000UL), 8);
            ub_s(&u, " div ");
            ub_hex(&u, g_rd(0x17401800UL), 8);
            ub_s(&u, " switch-user ");
            ub_hex(&u, g_rd(0x17400120UL), 8);
            sb_log(u.b);
            g_jobs0 = j;
        }
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "probe: gpu: ");
            ub_dec(&u, mhz);
            ub_s(&u, " MHz passed 30 s: PLL ");
            int off = 0;
            ub_dec(&u, sb_gpu_hz(&off) / 1000000U);
            ub_s(&u, " MHz, GPU zone ");
            ub_sdec(&u, tc);
            ub_s(&u, " C, battery ");
            ub_sdec(&u, g_n ? g_acc / g_n : 0);
            ub_s(&u, " mA (+ charging)");
            sb_log(u.b);
        }
        g_best = (int)mhz;
        if (++gstep >= (int)(sizeof(g_steps) / sizeof(g_steps[0]))) {
            g_back("the ladder's end");
            gph = 8;
            g_t = now;
            return 0;
        }
        {
            /* S 0 above 600 MHz (the VCO is the clock: 600 .. 1200), P kept (4: Fref 6.5 MHz) */
            uint32_t p = (g_con0_598 >> 8) & 0x3fU, s = 0;
            uint32_t m = p ? (uint32_t)(((uint64_t)g_steps[gstep] * p + 13U) / 26U) : 0;
            if (!g_pms_ok(p, m, s)) {
                sb_logx("probe: gpu: the next step is outside the 1018X's limits (VCO 600..1200), MHz ", g_steps[gstep]);
                g_back("the PLL's limits");
                gph = 8;
                g_t = now;
                return 0;
            }
            uint32_t lk0 = g_rd(G3D_LOCK);
            int ok = g_set_pms(p, m, s);
            struct ub u;
            ub_init(&u);
            ub_s(&u, "probe: gpu: to ");
            ub_dec(&u, g_steps[gstep]);
            ub_s(&u, " MHz: P ");
            ub_dec(&u, p);
            ub_s(&u, " M ");
            ub_dec(&u, m);
            ub_s(&u, " S 0 (lock time was ");
            ub_dec(&u, lk0);
            ub_s(&u, "), CON0 ");
            ub_hex(&u, g_rd(G3D_CON0), 8);
            ub_s(&u, ok ? ", locked, PLL reads " : ", NO LOCK in 2 ms, PLL reads ");
            int off = 0;
            ub_dec(&u, sb_gpu_hz(&off) / 1000000U);
            ub_s(&u, " MHz");
            sb_log(u.b);
            if (!ok) {
                g_back("no lock");
                gph = 8;
                g_t = now;
                return 0;
            }
        }
        g_t = now;
        g_acc = 0;
        g_n = 0;
        return 0;
    }
    case 8:
        gpuheat_stop();
        sb_logx("probe: passed: H2 done; the highest GPU step that passed 30 s, MHz: ", (uint64_t)g_best);
        gph = 10;
        return 1;
    case 9:
        gpuheat_stop();
        gph = 10;
        return 1;
    default:
        return 1;
    }
}
