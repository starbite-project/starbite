/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS kernel: the microSD card's clock in CMU top. libsb sd.c only reads the CMU (its rule: the loader and
 * the shim share it and never write CMU blocks), so the one write lives here, called by sd.c with the card clock
 * stopped (sd_set_bus_hook before the card's 400 kHz init, sd_rebus between commands).
 *
 * The facts (Samsung's GPL starlte kernel, ref/starlte-kernel/drivers/soc/samsung/cal-if/exynos9810/, and our own
 * runs on this phone):
 *   divider  CLK_CON_DIV_CLKCMU_FSYS1_MMC_CARD, CMU top 0x1a240000 + 0x1864: DIVRATIO bits 8:0 (/(n + 1)), BUSY
 *            bit 16, ENABLE_AUTOMATIC_CLKGATING bit 28 (cmucal-sfr.c). Samsung's ra_set_div_mux writes DIVRATIO
 *            and keeps the other bits, then waits up to 100 us for BUSY to clear (ra.c). S-Boot leaves 15 (/16):
 *            XNU 0347 measured it 2026-11-03, drivers run 11 saw its 6 MHz card clock (3.01 MB/s on both paths).
 *   mux      CLK_CON_MUX_MUX_CLKCMU_FSYS1_MMC_CARD 0x106c, SELECT bits 2:0: 0 OSCCLK, 1 PLL_SHARED2, 2 PLL_SHARED4,
 *            3 PLL_SHARED0 / 4, 4 PLL_MMC, 5-7 OSCCLK (cmucal-node.c). Only read here.
 *   user mux FSYS1 0x11400000 + 0x120 (PLL_CON0_MUX_CLKCMU_FSYS1_MMC_CARD_USER) MUX_SEL bit 4: 0 OSCCLK_FSYS1,
 *            1 CMU top's card clock. Only read: with 0 the divider does not reach the card and nothing is changed.
 *   PLLs     PLL_CON0 ENABLE bit 31, STABLE bit 29, MUX_SEL bit 4 (FOUT, ra.h), M 25:16, P 13:8, S 2:0; FIN 26 MHz.
 *            PLL_MMC (0x100, PLL_CON3 0x10c K 15:0 signed) is fractional: (M << 16 + K) * 26 MHz / (P << S) >> 16
 *            (ra_get_pll). Samsung's table entry M 31 P 1 S 0 K (short)50412 = -15124 is 799.99988 MHz (the
 *            table's "825999878" label read K unsigned); sd.c's PLL_MMC_HZ 806 MHz was an estimate. PLL_SHARED2
 *            (0x220) is integer: 400 * 26 MHz / 13 = 800 MHz, Samsung's own "nm" setting for this mux.
 *   rate     The dwmmc2 node (build/linux-bundle/lts.dts): clock-frequency 800 MHz (the CIU rate stock sets at probe),
 *            ciu-div 3 (CLKSEL /4 inside the controller), ciu_clkin 25/50/50/... MHz per timing (stock's bus clock
 *            for SD HS: 50 MHz, CIU 200 MHz). Mainline Linux on this phone (tools/linux/mk-starlte-dts.py) and our
 *            XNU driver (0347/0348) both write DIVRATIO 0 (/1): bus 200 MHz, CLKDIV 2 = 50 MHz, 22.6 and 23.2 MB/s
 *            measured. Here: the smallest DIVRATIO whose CIU rate is <= SDCLK_CIU_MAX (800 MHz + 1 %), so /1 from
 *            PLL_MMC. The bus (CIU / 4) then stays <= 204 MHz, where CLKDIV 255 still gives <= 400 kHz for init.
 *
 * Nothing else is written: no PLL, no mux, no gate, no other block. A divider whose BUSY never clears (1 ms) gets its
 * old value back. */
#include "sb.h"
#include "sd.h"
#include "sdclk.h"

#define CMU_TOP     0x1a240000UL
#define DIV_MMC     0x1864U
#define MUX_MMC     0x106cU
#define PLL_MMC0    0x0100U
#define PLL_MMC3    0x010cU
#define PLL_SH2_0   0x0220U
#define FSYS1_USER  0x11400120UL
#define DIV_MASK    0x1ffU
#define DIV_BUSY    (1U << 16)
#define FIN_HZ      26000000ULL

struct sdclk_report sdclk_rep;

#ifdef SDCLK_HOST_FAKE                      /* host test (host/sdclk-test.c) */
uint32_t sdclk_fake_rd(uintptr_t a);
void sdclk_fake_wr(uintptr_t a, uint32_t v);
static uint32_t rd(uintptr_t a) { return sdclk_fake_rd(a); }
static void wr(uintptr_t a, uint32_t v) { sdclk_fake_wr(a, v); }
#elif defined(SB_HOST)                      /* the kernel's host build: no CMU, every rate unknown */
static uint32_t rd(uintptr_t a) { (void)a; return 0; }
static void wr(uintptr_t a, uint32_t v) { (void)a; (void)v; }
#else
static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}
#endif

uint64_t sdclk_pll_hz(uint32_t con0, uint32_t con3, int frac)
{
    if (!(con0 & (1U << 31)) || !(con0 & (1U << 29)) || !(con0 & (1U << 4)))
        return 0;                           /* off, not locked, or its mux on FIN: not a rate to divide */
    uint64_t m = (con0 >> 16) & 0x3ffU, p = (con0 >> 8) & 0x3fU, s = con0 & 7U;
    if (!m || !p)
        return 0;
    uint64_t f;
    if (frac) {
        int64_t mk = (int64_t)(m << 16) + (int16_t)(con3 & 0xffffU);
        if (mk <= 0)
            return 0;
        f = (FIN_HZ * (uint64_t)mk / (p << s)) >> 16;
    } else {
        f = FIN_HZ * m / (p << s);
    }
    return f >= FIN_HZ && f <= 2000000000ULL ? f : 0;
}

uint32_t sdclk_pick(uint64_t parent_hz)
{
    uint64_t d = (parent_hz + SDCLK_CIU_MAX - 1U) / SDCLK_CIU_MAX;
    d = d ? d - 1U : 0;
    return d > DIV_MASK ? DIV_MASK : (uint32_t)d;
}

/* The mux's parent from the registers as found; 0 and sdclk_rep.why when unknown. */
static uint64_t parent(const char **name)
{
    struct sdclk_report *R = &sdclk_rep;
    R->mux = rd(CMU_TOP + MUX_MMC);
    R->user = rd(FSYS1_USER);
    R->pll0 = R->pll3 = 0;
    *name = "?";
    if (!(R->user & (1U << 4))) {
        R->why = "the card clock is FSYS1's 26 MHz oscillator (user mux), not CMU top's";
        return 0;
    }
    uint64_t hz = 0;
    switch (R->mux & 7U) {
    case 4:
        *name = "PLL_MMC";
        R->pll0 = rd(CMU_TOP + PLL_MMC0);
        R->pll3 = rd(CMU_TOP + PLL_MMC3);
        hz = sdclk_pll_hz(R->pll0, R->pll3, 1);
        break;
    case 1:
        *name = "PLL_SHARED2";
        R->pll0 = rd(CMU_TOP + PLL_SH2_0);
        hz = sdclk_pll_hz(R->pll0, 0, 0);
        break;
    case 0: case 5: case 6: case 7:
        *name = "OSCCLK";
        return FIN_HZ;
    default:
        R->why = "the card's mux is on a PLL sdclk.c does not read (PLL_SHARED4 / PLL_SHARED0)";
        return 0;
    }
    if (!hz)
        R->why = "the card's PLL is off, unlocked or on its 26 MHz input";
    return hz;
}

/* DIVRATIO n, the other bits kept; then BUSY clear (1 ms) and the value read back. 1 ok. */
static int div_set(uint32_t n)
{
    uint32_t v = rd(CMU_TOP + DIV_MMC);
    wr(CMU_TOP + DIV_MMC, (v & ~DIV_MASK) | n);
    uint64_t t = sb_ticks() + sb_freq() / 1000U;
    sdclk_rep.busy_reads = 0;
    for (;;) {
        uint32_t r = rd(CMU_TOP + DIV_MMC);
        ++sdclk_rep.busy_reads;
        if (!(r & DIV_BUSY))
            return (r & DIV_MASK) == n;
        if (sb_ticks() > t)
            return 0;
    }
}

static void say(const char *what, uint32_t d0, uint32_t d1, uint64_t b0, uint64_t b1)
{
    struct sdclk_report *R = &sdclk_rep;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "sd: CMU divider ");
    ub_s(&u, what);
    ub_s(&u, " /");
    ub_dec(&u, d0 + 1U);
    ub_s(&u, " -> /");
    ub_dec(&u, d1 + 1U);
    ub_s(&u, " (found ");
    ub_hex(&u, R->div_reg, 8);
    ub_s(&u, ", busy ");
    ub_dec(&u, R->busy_reads);
    ub_s(&u, " reads): ");
    ub_s(&u, R->pname);
    ub_c(&u, ' ');
    ub_dec(&u, R->parent_hz / 1000U);
    ub_s(&u, " kHz, bus ");
    ub_dec(&u, b0 / 1000U);
    ub_s(&u, " -> ");
    ub_dec(&u, b1 / 1000U);
    ub_s(&u, " kHz");
    if (R->why) {
        ub_s(&u, "; ");
        ub_s(&u, R->why);
    }
    sb_log(u.b);
}

uint64_t sdclk_change(int restore)
{
    struct sdclk_report *R = &sdclk_rep;
    if (restore) {
        if (!R->raised)
            return R->bus_now;
        R->why = 0;
        if (!div_set(R->div_was)) {
            R->why = "the old divider did not take (busy or read back wrong)";
            say("NOT put back", R->div_now, R->div_was, R->bus_now, R->bus_was);
            return 0;
        }
        R->raised = 0;
        say("put back", R->div_now, R->div_was, R->bus_now, R->bus_was);
        R->div_now = R->div_was;
        R->bus_now = R->bus_was;
        return R->bus_now;
    }
    if (R->raised)
        return R->bus_now;
    R->why = 0;
    R->busy_reads = 0;
    R->div_reg = rd(CMU_TOP + DIV_MMC);
    R->div_was = R->div_now = R->div_reg & DIV_MASK;
    R->parent_hz = parent(&R->pname);
    R->bus_was = R->bus_now = R->parent_hz / (R->div_was + 1U) / 4U;
    if (!R->parent_hz) {
        say("left", R->div_was, R->div_now, R->bus_was, R->bus_now);
        return 0;
    }
    uint32_t want = sdclk_pick(R->parent_hz);
    if (want >= R->div_was) {               /* already there (or faster than stock: not ours to slow) */
        if (want > R->div_was)
            R->why = "already faster than the stock 800 MHz CIU: left as found";
        say("left", R->div_was, R->div_now, R->bus_was, R->bus_now);
        return R->bus_was;
    }
    if (!div_set(want)) {
        int back = div_set(R->div_was);
        R->why = back ? "the new divider did not take (busy or read back wrong): old one back"
                      : "the new divider did not take, nor the old one: divider state unknown";
        R->div_now = rd(CMU_TOP + DIV_MMC) & DIV_MASK;
        say("NOT raised", R->div_was, R->div_now, R->bus_was, R->bus_was);
        return back ? R->bus_was : 0;
    }
    R->raised = 1;
    R->div_now = want;
    R->bus_now = R->parent_hz / (want + 1U) / 4U;
    say("raised", R->div_was, R->div_now, R->bus_was, R->bus_now);
    return R->bus_now;
}

int sdclk_raise(void)
{
    if (sdclk_rep.raised)
        return 1;
    if (sd_rep.state == 0) {                /* the card's first sd_up raises it, the card clock stopped */
        sd_set_bus_hook(sdclk_change);
        return 2;
    }
    if (sd_rep.state < 0) {
        sdclk_rep.why = "the card did not come up";
        return 0;
    }
    if (sd_rebus(sdclk_change, 0))
        return -1;
    return sdclk_rep.raised;
}

int sdclk_restore(void)
{
    if (!sdclk_rep.raised)
        return 0;
    if (sd_rep.state == 1)
        return sd_rebus(sdclk_change, 1) || sdclk_rep.raised ? -1 : 0;
    (void)sdclk_change(1);                  /* the card is not up: no command can be running */
    return sdclk_rep.raised ? -1 : 0;
}
