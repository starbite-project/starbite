/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/sdclk.c (the microSD card's CMU divider) on a fake CMU: the PLL rates from PLL_CON0/3 (PLL_MMC's
 * signed K: 799.99988 MHz, not the table label's 826 MHz), the divider picked (/1 from 800 MHz), the busy wait (bounded,
 * the old value back when it never clears), the restore, the refusals (user mux on the oscillator, an unknown parent,
 * a PLL off), the hook / sd_rebus hand-off by card state; and that nothing but the divider is ever written. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "sb.h"
#include "sd.h"
#include "sdclk.h"

/* ---- libsb stubs ---- */
static uint64_t now;
uint64_t sb_ticks(void) { return ++now; }
uint64_t sb_freq(void) { return 1000000U; }
static char lastlog[256];
static unsigned nlogs;
void sb_log(const char *s) { snprintf(lastlog, sizeof lastlog, "%s", s); ++nlogs; if (getenv("SDCLK_TEST_V")) printf("log: %s\n", s); }
void ub_init(struct ub *u) { u->n = 0; u->b[0] = 0; }
void ub_s(struct ub *u, const char *s) { while (*s && u->n < sizeof u->b - 1) u->b[u->n++] = *s++; u->b[u->n] = 0; }
void ub_c(struct ub *u, char c) { char t[2] = { c, 0 }; ub_s(u, t); }
void ub_dec(struct ub *u, uint64_t v) { char t[24]; snprintf(t, sizeof t, "%llu", (unsigned long long)v); ub_s(u, t); }
void ub_hex(struct ub *u, uint64_t v, unsigned d) { char t[24]; snprintf(t, sizeof t, "0x%0*llx", (int)d, (unsigned long long)v); ub_s(u, t); }

/* ---- sd.c's side ---- */
struct sd_report sd_rep;
static sd_bus_fn hook;
static unsigned n_rebus;
static int rebus_ret;
void sd_set_bus_hook(sd_bus_fn fn) { hook = fn; }
int sd_rebus(sd_bus_fn fn, int restore)
{
    if (sd_rep.state != 1 || !fn)
        return -1;
    ++n_rebus;
    (void)fn(restore);
    return rebus_ret;
}

/* ---- the fake CMU ---- */
#define A_DIV  0x1a241864UL
#define A_MUX  0x1a24106cUL
#define A_MMC0 0x1a240100UL
#define A_MMC3 0x1a24010cUL
#define A_SH2  0x1a240220UL
#define A_USER 0x11400120UL
static uint32_t div_reg, mux, user, mmc0, mmc3, sh2;
static unsigned busy_left, busy_n, bad_writes, n_div_writes;
static int stuck;                               /* 1 the next write never clears BUSY, 2 none ever does */

uint32_t sdclk_fake_rd(uintptr_t a)
{
    switch (a) {
    case A_DIV:
        if (stuck == 2 || (stuck == 3 && busy_left)) return div_reg | (1U << 16);
        if (busy_left) { --busy_left; return div_reg | (1U << 16); }
        return div_reg;
    case A_MUX: return mux;
    case A_USER: return user;
    case A_MMC0: return mmc0;
    case A_MMC3: return mmc3;
    case A_SH2: return sh2;
    default: return 0xdeadbeefU;
    }
}

void sdclk_fake_wr(uintptr_t a, uint32_t v)
{
    if (a != A_DIV) {
        ++bad_writes;
        return;
    }
    ++n_div_writes;
    div_reg = v & ~(1U << 16);
    busy_left = busy_n;
    if (stuck == 1) {                           /* this write never finishes; the next one does */
        stuck = 3;
        busy_left = 1;
    } else if (stuck == 3) {
        stuck = 0;
    }
}

#define CON0(m, p, s) ((1U << 31) | (1U << 29) | (1U << 4) | ((uint32_t)(m) << 16) | ((uint32_t)(p) << 8) | (uint32_t)(s))
#define MMC_HZ 799999877ULL                     /* 26 MHz * (31 * 65536 - 15124) / 65536 */

static void reset(void)
{
    memset(&sdclk_rep, 0, sizeof sdclk_rep);
    memset(&sd_rep, 0, sizeof sd_rep);
    div_reg = 0x1000000fU;                      /* S-Boot's /16, automatic clock gating on */
    mux = 4;                                    /* PLL_MMC */
    user = 1U << 4;                             /* CMU top's card clock */
    mmc0 = CON0(31, 1, 0);
    mmc3 = 50412;                               /* (short)50412 = -15124 */
    sh2 = CON0(400, 13, 0);
    busy_left = 0;
    busy_n = 3;
    stuck = 0;
    bad_writes = n_div_writes = 0;
    hook = 0;
    n_rebus = 0;
    rebus_ret = 0;
    nlogs = 0;
    lastlog[0] = 0;
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); ++fails; } } while (0)

static void t_math(void)
{
    CHECK(sdclk_pll_hz(CON0(31, 1, 0), 50412, 1) == MMC_HZ, "PLL_MMC: K signed, 799.99988 MHz");
    CHECK(sdclk_pll_hz(CON0(31, 1, 0), 0, 1) == 806000000ULL, "PLL_MMC with K 0: 806 MHz");
    CHECK(sdclk_pll_hz(CON0(400, 13, 0), 0, 0) == 800000000ULL, "PLL_SHARED2: 800 MHz");
    CHECK(sdclk_pll_hz(CON0(400, 13, 1), 0, 0) == 400000000ULL, "S divides by 2^S");
    CHECK(sdclk_pll_hz(CON0(31, 1, 0) & ~(1U << 31), 50412, 1) == 0, "PLL off: unknown");
    CHECK(sdclk_pll_hz(CON0(31, 1, 0) & ~(1U << 29), 50412, 1) == 0, "PLL not locked: unknown");
    CHECK(sdclk_pll_hz(CON0(31, 1, 0) & ~(1U << 4), 50412, 1) == 0, "PLL mux on FIN: unknown");
    CHECK(sdclk_pll_hz(CON0(31, 0, 0), 50412, 1) == 0, "P 0: unknown");
    CHECK(sdclk_pick(MMC_HZ) == 0 && sdclk_pick(806000000ULL) == 0 && sdclk_pick(800000000ULL) == 0, "/1 from ~800 MHz");
    CHECK(sdclk_pick(826000000ULL) == 1, "/2 above 810 MHz");
    CHECK(sdclk_pick(1600000000ULL) == 1 && sdclk_pick(26000000ULL) == 0, "/2 from 1.6 GHz, /1 from 26 MHz");
    CHECK(MMC_HZ / 4U <= 204000000ULL && SDCLK_CIU_MAX / 4U <= 204000000ULL, "the bus stays <= 204 MHz (CLKDIV 255 <= 400 kHz)");
}

static void t_raise_restore(void)
{
    reset();
    CHECK(sdclk_raise() == 2 && hook == sdclk_change && n_div_writes == 0, "card not up: the hook registered, nothing written");
    uint64_t b = hook(0);                       /* what sd_up does, the card clock stopped */
    CHECK(b == MMC_HZ / 4U && sdclk_rep.bus_now == b, "raised: bus 200 MHz");
    CHECK(div_reg == 0x10000000U && n_div_writes == 1, "raised: DIVRATIO 0, bit 28 kept, one write");
    CHECK(sdclk_rep.busy_reads == 4 && sdclk_rep.raised && sdclk_rep.div_was == 15 && sdclk_rep.div_now == 0,
          "raised: busy waited (3 busy reads), report");
    CHECK(sdclk_rep.bus_was == MMC_HZ / 16U / 4U && sdclk_rep.parent_hz == MMC_HZ, "raised: was 12.5 MHz from PLL_MMC");
    CHECK(nlogs == 1 && strstr(lastlog, "raised /16 -> /1") && strstr(lastlog, "PLL_MMC 799999 kHz") &&
          strstr(lastlog, "bus 12499 -> 199999 kHz"), "raised: one log line");
    CHECK(hook(0) == b && n_div_writes == 1, "raised twice: nothing more written");
    sd_rep.state = -1;                          /* the card is not up: restored directly */
    CHECK(sdclk_restore() == 0 && div_reg == 0x1000000fU && !sdclk_rep.raised && n_div_writes == 2, "restored: /16 again");
    CHECK(strstr(lastlog, "put back /1 -> /16") != 0, "restored: logged");
    CHECK(sdclk_restore() == 0 && n_div_writes == 2, "restore when not raised: nothing");
    CHECK(bad_writes == 0, "only the divider written");
}

static void t_busy(void)
{
    reset();
    stuck = 1;                                  /* the new value never settles; the old one does */
    uint64_t b = sdclk_change(0);
    CHECK(b == MMC_HZ / 16U / 4U && !sdclk_rep.raised && div_reg == 0x1000000fU && n_div_writes == 2,
          "busy stuck: the old divider back, its bus clock");
    CHECK(sdclk_rep.why && strstr(sdclk_rep.why, "old one back") && strstr(lastlog, "NOT raised"), "busy stuck: said");
    reset();
    stuck = 2;                                  /* nothing ever settles */
    uint64_t t0 = now;
    CHECK(sdclk_change(0) == 0 && !sdclk_rep.raised && strstr(sdclk_rep.why, "unknown"), "busy forever: rate unknown");
    CHECK(now - t0 < 5000U, "busy forever: bounded (1 ms each)");
    reset();
    busy_n = 0;
    CHECK(sdclk_change(0) == MMC_HZ / 4U && sdclk_rep.busy_reads == 1, "never busy: one read");
    CHECK(bad_writes == 0, "busy: only the divider written");
}

static void t_refuse(void)
{
    reset();
    user = 0;                                   /* SDCLKIN from FSYS1's oscillator */
    CHECK(sdclk_change(0) == 0 && n_div_writes == 0 && strstr(sdclk_rep.why, "oscillator"), "user mux on OSCCLK: left");
    reset();
    mux = 2;                                    /* PLL_SHARED4: not read here */
    CHECK(sdclk_change(0) == 0 && n_div_writes == 0, "unknown parent: left");
    reset();
    mmc0 &= ~(1U << 31);
    CHECK(sdclk_change(0) == 0 && n_div_writes == 0 && strstr(sdclk_rep.why, "PLL"), "PLL off: left");
    reset();
    mux = 1;                                    /* PLL_SHARED2, Samsung's own setting */
    CHECK(sdclk_change(0) == 200000000ULL && sdclk_rep.raised && strstr(lastlog, "PLL_SHARED2"), "PLL_SHARED2: /1, 200 MHz");
    reset();
    div_reg = 0x10000000U;                      /* already /1 (after Linux or XNU) */
    CHECK(sdclk_change(0) == MMC_HZ / 4U && n_div_writes == 0 && !sdclk_rep.raised, "already /1: the rate, nothing written");
    CHECK(sdclk_change(1) == MMC_HZ / 4U && n_div_writes == 0, "already /1: nothing to put back");
    reset();
    mmc3 = 0;
    mmc0 = CON0(32, 1, 0);                      /* 832 MHz: over the 810 MHz CIU limit */
    CHECK(sdclk_change(0) == 832000000ULL / 2U / 4U && div_reg == 0x10000001U, "832 MHz: /2");
    reset();
    div_reg = 0;
    mmc3 = 0;
    mmc0 = CON0(64, 1, 0);                      /* 1.664 GHz at /1: faster than stock, not ours */
    CHECK(sdclk_change(0) == 1664000000ULL / 4U && n_div_writes == 0 && strstr(sdclk_rep.why, "faster"), "faster than stock: left");
    CHECK(bad_writes == 0, "refusals: only the divider written");
}

static void t_states(void)
{
    reset();
    sd_rep.state = 1;                           /* the card is up: between commands, through sd_rebus */
    CHECK(sdclk_raise() == 1 && n_rebus == 1 && sdclk_rep.raised && hook == 0, "card up: raised through sd_rebus");
    CHECK(sdclk_raise() == 1 && n_rebus == 1, "card up, raised: nothing more");
    CHECK(sdclk_restore() == 0 && n_rebus == 2 && !sdclk_rep.raised && div_reg == 0x1000000fU, "card up: put back through sd_rebus");
    reset();
    sd_rep.state = 1;
    rebus_ret = -1;
    CHECK(sdclk_raise() == -1, "card up, sd_rebus failed: -1");
    reset();
    sd_rep.state = -1;
    CHECK(sdclk_raise() == 0 && n_rebus == 0 && n_div_writes == 0 && hook == 0, "card failed: nothing");
    CHECK(bad_writes == 0, "states: only the divider written");
}

int main(void)
{
    t_math();
    t_raise_restore();
    t_busy();
    t_refuse();
    t_states();
    printf("sdclk-test: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
