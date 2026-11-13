/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of libsb sd.c's fast reads (sd_set_fast: CMD6 high speed + IDMAC) on a fake DesignWare MSHC and a fake
 * SDXC card: the old path stays the default; CMD6 mode 1 only after the mode 0 query offered high speed, and only
 * with 0x80fffff1; CLKSEL / CLKDIV; the IDMAC descriptor chain of a 256-sector CMD18; the PIO fallback after an
 * IDMAC error; never a write command. The bus clock hook (sd_set_bus_hook / sd_rebus, kern/sdclk.c's): called once with
 * the card clock stopped before the 400 kHz init, its bus clock used for every CLKDIV, the old divider put back and
 * one more try when the card fails on the new clock, and the same between commands on a card that is up. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "sb.h"

/* ---- libsb stubs ---- */
static uint64_t now;                         /* ticks, 1 MHz; every read moves it so bounded waits end */
uint64_t sb_ticks(void) { return ++now; }
uint64_t sb_freq(void) { return 1000000U; }
void sb_mdelay(unsigned ms) { now += (uint64_t)ms * 1000U; }
int sb_sd_rails(uint8_t *a, uint8_t *b) { *a = 0xe8; *b = 0xfc; return 1; }
static int fence_on;                        /* t_write_clock only: a write through the fence */
int sb_fence_active(void) { return fence_on; }
static int map_kind = 2;
int sb_map_kind(uint64_t pa) { (void)pa; return map_kind; }
static unsigned n_clean, n_inval;
static uintptr_t last_clean, last_inval;
void sb_dcache_clean_inval(const volatile void *a, uint64_t len) { (void)len; ++n_clean; last_clean = (uintptr_t)a; }
void sb_dcache_inval(const volatile void *a, uint64_t len) { (void)len; ++n_inval; last_inval = (uintptr_t)a; }
static char lastlog[256];
static unsigned nlogs;
void sb_log(const char *s) { snprintf(lastlog, sizeof lastlog, "%s", s); ++nlogs; if (getenv("SD_TEST_V")) printf("log: %s\n", s); }
void ub_init(struct ub *u) { u->n = 0; u->b[0] = 0; }
void ub_s(struct ub *u, const char *s) { while (*s && u->n < sizeof u->b - 1) u->b[u->n++] = *s++; u->b[u->n] = 0; }
void ub_c(struct ub *u, char c) { char t[2] = { c, 0 }; ub_s(u, t); }
void ub_dec(struct ub *u, uint64_t v) { char t[24]; snprintf(t, sizeof t, "%llu", (unsigned long long)v); ub_s(u, t); }

/* ---- the fake controller and card ---- */
static uint32_t regs[64];                    /* 0x00..0xfc */
#define R(off) regs[(off) / 4]
static uint64_t fifo[1 << 16];
static unsigned fh, ft;
static int app_next, hs_offer = 1, card_hs, query_said_yes, dma_fail_next, scr_spec = 2;
static unsigned n_writes, write_clkdiv;
static int write_hs = -1;
static unsigned bad_cmds, cmd6_set, cmd6_query, n_cmd18_dma, n_cmd18_pio, n_cmd9, n_acmd51;
static uint32_t clkdiv_now, clksel_last;
static uint32_t cmu_div_reg;                 /* DIV_CLKCMU_FSYS1_MMC_CARD as the fake CMU has it */
static int clk_on_at_update, fail_cmd8_on_div0;
static uint32_t clkdivs[64];                 /* CLKDIV at every clock update with the card clock on */
static unsigned nclkdivs;
static int log_cmds[256], ncmds;
struct desc { uint32_t d[8]; };
static struct desc cap[64];                  /* the last IDMAC chain as the controller found it */
static unsigned ncap;
static uint64_t cap_dbaddr;

static uint64_t word(uint64_t sector, unsigned w) { return (sector << 32) ^ (w * 0x9e3779b97f4a7c15ULL) ^ 0x5d; }
static void push(uint64_t v) { fifo[ft++ & 0xffff] = v; }

static void status64(int set, uint32_t arg)
{
    uint8_t b[64];
    memset(b, 0, sizeof b);
    unsigned fn = arg & 0xfU;                   /* group 1's function: 1 high speed, 0 default speed (before writes) */
    b[13] = hs_offer ? 0x03 : 0x01;
    b[16] = fn == 0 ? 0x00 : hs_offer ? 0x01 : 0x0f;
    if (set && fn == 0)
        card_hs = 0;
    else if (set && hs_offer)
        card_hs = 1;
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t v = 0;
        for (unsigned k = 0; k < 8; ++k)
            v |= (uint64_t)b[i * 8 + k] << (8 * k);
        push(v);
    }
}

static void data_read(unsigned idx, uint32_t arg, int autostop)
{
    uint32_t n = R(0x20) / 512U;
    if ((R(0x00) & (1U << 25)) && (R(0x80) & (1U << 7))) {
        ++n_cmd18_dma;
        if (idx != 18) ++bad_cmds;          /* the IDMAC only for CMD18 */
        cap_dbaddr = (uint64_t)R(0x88) | (uint64_t)R(0x8c) << 32;
        struct desc *d = (struct desc *)(uintptr_t)cap_dbaddr;
        uint64_t sector = arg, w = 0;
        ncap = 0;
        for (;;) {
            if (ncap >= 64) break;
            cap[ncap++] = *d;
            if (!(d->d[0] & (1U << 31))) { R(0x90) |= 1U << 4; break; }   /* DU */
            uint64_t *buf = (uint64_t *)(uintptr_t)((uint64_t)d->d[4] | (uint64_t)d->d[5] << 32);
            for (unsigned i = 0; i < (d->d[2] & 0x1fffU) / 8U; ++i, ++w)
                buf[i] = dma_fail_next ? 0xdeadULL : word(sector + w / 64U, (unsigned)(w % 64U));
            if (dma_fail_next)
                break;                          /* a bus error: the descriptor stays owned */
            d->d[0] &= ~(1U << 31);
            if (d->d[0] & (1U << 2)) break;     /* LD */
            d = (struct desc *)(uintptr_t)((uint64_t)d->d[6] | (uint64_t)d->d[7] << 32);
        }
        if (dma_fail_next) {
            --dma_fail_next;
            R(0x90) |= 1U << 2;                 /* FBE */
            return;
        }
        R(0x90) |= (1U << 1) | (1U << 8);       /* RI NI */
        R(0x44) |= (1U << 3) | (autostop ? 1U << 14 : 0);
        return;
    }
    ++n_cmd18_pio;
    for (uint32_t s = 0; s < n; ++s)
        for (unsigned w = 0; w < 64; ++w)
            push(word(arg + s, w));
    R(0x44) |= (1U << 3) | (autostop ? 1U << 14 : 0);
}

static void command(uint32_t v)
{
    if (v & (1U << 21)) {                       /* clock update */
        clkdiv_now = R(0x08);
        clk_on_at_update = R(0x10) & 1U;
        if (clk_on_at_update && nclkdivs < 64)
            clkdivs[nclkdivs++] = clkdiv_now;
        return;
    }
    unsigned idx = v & 0x3fU;
    uint32_t arg = R(0x28);
    int app = app_next;
    app_next = 0;
    if (ncmds < 256)
        log_cmds[ncmds++] = (app ? 100 : 0) + (int)idx;
    if ((v & (1U << 10)) || (!app && (idx == 24 || idx == 25))) {   /* a write: the clock and card mode it ran with */
        ++n_writes;
        write_clkdiv = R(0x08);
        write_hs = card_hs;
    }
    if (v & (1U << 10)) ++bad_cmds;             /* any data write */
    if (!app && (idx == 20 || idx == 24 || idx == 25 || idx == 27 || idx == 28 || idx == 29 || idx == 32 ||
                 idx == 33 || idx == 38 || idx == 42 || idx == 56))
        ++bad_cmds;
    if (app && (idx == 23 || idx == 42))
        ++bad_cmds;
    R(0x44) |= 1U << 2;                         /* CD */
    switch (app ? 100 + idx : idx) {
    case 0: card_hs = 0; break;
    case 8:
        if (fail_cmd8_on_div0 && !(cmu_div_reg & 0x1ffU)) {
            R(0x44) |= 1U << 8;                 /* RTO: the card "fails" on the raised clock */
            break;
        }
        R(0x30) = arg & 0xfffU;
        break;
    case 55: R(0x30) = 0x120; app_next = 1; break;
    case 141: R(0x30) = 0xc0ff8000U; break;
    case 2: R(0x30) = 0x00012345; R(0x34) = 0x30abcdef; R(0x38) = 0x43363447; R(0x3c) = 0x1b534d53; break;
    case 3: R(0x30) = 0xaaaa0500U; break;
    case 9: ++n_cmd9; R(0x30) = 0x0a400001; R(0x34) = 0x76ff0000; R(0x38) = 0x5b590001; R(0x3c) = 0x400e0032; break;
    case 13: R(0x30) = 4U << 9; break;
    case 151: {
        ++n_acmd51;
        uint8_t b[8] = { (uint8_t)scr_spec, 0x35, 0x80, 0, 0, 0, 0, 0 };
        uint64_t w = 0;
        for (unsigned k = 0; k < 8; ++k) w |= (uint64_t)b[k] << (8 * k);
        push(w);
        R(0x44) |= 1U << 3;
        break;
    }
    case 6:
        if (arg & 0x80000000U) {
            ++cmd6_set;
            if (arg != 0x80fffff1U || !query_said_yes) ++bad_cmds;   /* only group 1, only after a yes */
        } else {
            ++cmd6_query;
            if (arg != 0x00fffff1U) ++bad_cmds;
            query_said_yes = hs_offer;
        }
        status64((arg & 0x80000000U) != 0, arg);
        R(0x44) |= 1U << 3;
        break;
    case 17: case 18:
        data_read(idx, arg, (v & (1U << 12)) != 0);
        break;
    default: break;
    }
}

uint32_t sd_fake_rd(uintptr_t off)
{
    if (off == 0x48) {                          /* STATUS: FIFO count, never busy */
        unsigned c = ft - fh;
        return (c > 64 ? 64U : c) << 17;
    }
    if (off == 0x6c) return 0x5342270a;
    if (off == 0x70) return R(0x70);
    return R(off);
}

void sd_fake_wr(uintptr_t off, uint32_t v)
{
    switch (off) {
    case 0x44: case 0x90: R(off) &= ~v; return;          /* RINTSTS, IDSTS64: write 1 to clear */
    case 0x00:
        if (v & 2U) fh = ft;                              /* FIFO reset */
        R(off) = v & ~7U;
        return;
    case 0x80: R(off) = v & ~1U; return;                  /* BMOD: SWR self-clears */
    case 0xa8: clksel_last = v; R(off) = v; return;
    case 0x2c:
        R(off) = v & ~(1U << 31);
        if (v & (1U << 31)) command(v);
        return;
    default: R(off) = v; return;
    }
}

uint64_t sd_fake_fifo_rd(void) { return fh == ft ? 0 : fifo[fh++ & 0xffff]; }
void sd_fake_fifo_wr(uint64_t v) { (void)v; ++bad_cmds; }
uint32_t sd_fake_mrd(uintptr_t a)
{
    if (a == 0x1a242070UL || a == 0x11402030UL || a == 0x11402034UL) return 3U << 20;
    if (a == 0x1a241864UL) return cmu_div_reg;
    return 0;                                   /* card detect low (present), pins */
}
void sd_fake_mwr(uintptr_t a, uint32_t v) { (void)a; (void)v; }

#include "../../../libsb/sd.c"

/* ---- the tests ---- */
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); ++fails; } } while (0)

static void reset(uint32_t hcon)
{
    memset(regs, 0, sizeof regs);
    memset(&sd_rep, 0, sizeof sd_rep);
    R(0x70) = hcon;
    R(0xa8) = 0x40000000U;                      /* a bit outside the timing fields: kept */
    fh = ft = 0;
    app_next = card_hs = query_said_yes = dma_fail_next = 0;
    hs_offer = 1;
    scr_spec = 2;
    bad_cmds = cmd6_set = cmd6_query = n_cmd18_dma = n_cmd18_pio = n_cmd9 = n_acmd51 = 0;
    ncmds = 0;
    ncap = 0;
    map_kind = 2;
    n_clean = n_inval = 0;
    nlogs = 0;
    fast_want = 0;
    allowed = 0;
    lastlog[0] = 0;
    cmu_div_reg = 0;                            /* /1 unless a test says otherwise */
    clk_on_at_update = fail_cmd8_on_div0 = 0;
    nclkdivs = 0;
    bus_hook = 0;
    bus_fixed = 0;
}

#define HCON_OK ((1U << 27) | (0U << 16) | (2U << 7))   /* 64-bit addressing, IDMAC, 64-bit FIFO */

static uint64_t big[512 * 64] __attribute__((aligned(64)));

static int data_ok(uint64_t sector, uint32_t n, const uint64_t *b)
{
    for (uint32_t s = 0; s < n; ++s)
        for (unsigned w = 0; w < 64; ++w)
            if (b[s * 64 + w] != word(sector + s, w))
                return 0;
    return 1;
}

static int find_cmd(int c, int from)
{
    for (int i = from; i < ncmds; ++i)
        if (log_cmds[i] == c)
            return i;
    return -1;
}

static void t_default(void)
{
    reset(HCON_OK);
    sd_allow();
    CHECK(sd_up() == 1, "default: card up");
    CHECK(clkdiv_now == 5 && sd_rep.card_hz == 20150000ULL, "default: CLKDIV 5 = 20.15 MHz from 201.5 MHz");
    CHECK((clksel_last & 0x07070007U) == 0x03020000U && (clksel_last & 0x40000000U), "CLKSEL div 3 drive 2 sample 0, other bits kept");
    CHECK(cmd6_query == 0 && cmd6_set == 0 && n_cmd9 == 0 && n_acmd51 == 0, "default: no CMD6, CMD9 or ACMD51");
    memset(big, 0, sizeof big);
    CHECK(sd_read(1000, 256, (uintptr_t)big) == 0 && data_ok(1000, 256, big), "default: 256 sectors read");
    CHECK(n_cmd18_pio == 1 && n_cmd18_dma == 0 && R(0x80) == 0, "default: PIO, IDMAC off");
    CHECK(sd_write(0, 1, (uintptr_t)big) == -9, "write refused outside the fence");
    CHECK(bad_cmds == 0, "default: no write command");
}

static void t_fast(int mode)
{
    reset(HCON_OK);
    sd_allow();
    CHECK(sd_set_fast(mode) == 0, "fast before sd_up: remembered only");
    CHECK(ncmds == 0, "fast before sd_up: the card not touched");
    CHECK(sd_up() == 1, "fast: card up");
    CHECK(n_cmd9 == 1 && sd_rep.csd_ok && ((sd_rep.csd[2] >> 20) & 0x400U), "fast: CSD read (class 10)");
    CHECK(n_acmd51 == 1 && sd_rep.scr_ok && (sd_rep.scr[0] & 0xfU) == 2U, "fast: SCR read");
    int q = find_cmd(6, 0);
    CHECK(q >= 0 && cmd6_query == 1 && cmd6_set == 1, "fast: one CMD6 query, one switch");
    CHECK(card_hs && sd_rep.hs_card && sd_rep.fast == mode, "fast: the card in high speed, fast on");
    CHECK(clkdiv_now == (mode == SD_FAST_HS50 ? 2U : 3U), mode == SD_FAST_HS50 ? "HS50: CLKDIV 2" : "ON: CLKDIV 3");
    CHECK(sd_rep.card_hz <= (mode == SD_FAST_HS50 ? 50375000ULL : 50000000ULL), "card clock in range");
    CHECK((clksel_last & 0x07070007U) == 0x03020000U, "SD HS: CLKSEL stays sdr-timing (div 3, drive 2, sample 0)");
    CHECK(n_cmd18_dma == 1 && ncap == 1 && cap[0].d[2] == 4096U, "fast: the IDMAC check read (8 sectors, one descriptor)");
    CHECK(!strncmp(lastlog, "sd: fast reads on", 17), "fast: one 'on' line");

    memset(big, 0, sizeof big);
    n_clean = n_inval = 0;
    unsigned pio0 = n_cmd18_pio;
    CHECK(sd_read(5000, 256, (uintptr_t)big) == 0 && data_ok(5000, 256, big), "fast: 256 sectors by IDMAC");
    CHECK(n_cmd18_dma == 2 && n_cmd18_pio == pio0, "fast: by IDMAC, not PIO");
    CHECK(ncap == 32, "256 sectors: 32 descriptors");
    int chain = 1;
    for (unsigned i = 0; i < ncap; ++i) {
        uint32_t d0 = cap[i].d[0];
        uint64_t buf = (uint64_t)cap[i].d[4] | (uint64_t)cap[i].d[5] << 32;
        uint64_t nx = (uint64_t)cap[i].d[6] | (uint64_t)cap[i].d[7] << 32;
        int last = i == ncap - 1;
        chain &= (d0 & (1U << 31)) != 0;                       /* OWN */
        chain &= ((d0 & (1U << 3)) != 0) == (i == 0);          /* FD on the first only */
        chain &= ((d0 & (1U << 2)) != 0) == last;              /* LD on the last only */
        chain &= ((d0 & (1U << 4)) != 0) == !last;             /* CH but on the last */
        chain &= ((d0 & (1U << 1)) != 0) == !last;             /* DIC but on the last */
        chain &= cap[i].d[2] == 4096U && cap[i].d[1] == 0 && cap[i].d[3] == 0;
        chain &= buf == (uint64_t)(uintptr_t)big + i * 4096U;
        chain &= nx == (uint64_t)(uintptr_t)&descs[last ? 0 : i + 1];
    }
    CHECK(chain, "the chain: OWN, FD first, LD last, CH/DIC but last, 4 KiB each, buffers in order, next pointers");
    CHECK(cap_dbaddr == (uint64_t)(uintptr_t)descs, "DBADDRL/U = the first descriptor");
    CHECK(n_clean == 2 && n_inval == 2 && last_inval == (uintptr_t)big, "cache: descriptors + buffer cleaned before, invalidated after");
    CHECK(!(R(0x00) & ((1U << 25) | (1U << 5))) && R(0x80) == 0 && R(0x4c) == FIFOTH_PIO, "after: PIO configuration again");

    /* a single sector stays CMD17 PIO; an unaligned buffer stays PIO */
    unsigned dma0 = n_cmd18_dma;
    CHECK(sd_read(7, 1, (uintptr_t)big) == 0 && data_ok(7, 1, big), "fast: one sector");
    CHECK(sd_read(9, 4, (uintptr_t)big + 8) == 0 && data_ok(9, 4, (const uint64_t *)((uintptr_t)big + 8)), "fast: unaligned");
    CHECK(n_cmd18_dma == dma0, "single sector and unaligned buffers: PIO");
    map_kind = 1;
    CHECK(sd_read(9, 4, (uintptr_t)big) == 0 && n_cmd18_dma == dma0, "device memory: PIO");
    map_kind = 3;
    n_clean = n_inval = 0;
    CHECK(sd_read(9, 4, (uintptr_t)big) == 0 && n_cmd18_dma == dma0 + 1 && n_clean == 1 && n_inval == 1,
          "non-cacheable buffer: IDMAC, maintenance on the descriptors only");
    map_kind = 2;

    /* an IDMAC error: that chunk again on PIO, then recovered */
    uint32_t rec0 = sd_rep.recoveries;
    pio0 = n_cmd18_pio;
    dma_fail_next = 1;
    memset(big, 0, sizeof big);
    CHECK(sd_read(9000, 256, (uintptr_t)big) == 0 && data_ok(9000, 256, big), "IDMAC error: the data still right (PIO)");
    CHECK(sd_rep.dma_errors == 1 && sd_rep.dma_last_error == -12 && sd_rep.recoveries == rec0 + 1, "IDMAC error counted, recovered");
    CHECK(n_cmd18_pio == pio0 + 1 && sd_rep.fast == mode, "the chunk read by PIO; fast stays on after one error");
    CHECK(find_cmd(12, q) >= 0, "the recovery's CMD12 (stop) sent");
    /* three: fast reads off */
    dma_fail_next = 2;
    CHECK(sd_read(9000, 512, (uintptr_t)big) == 0, "two more IDMAC errors: data read");
    CHECK(sd_rep.fast == 0 && clkdiv_now == 5 && !strncmp(lastlog, "sd: fast reads off", 18), "three errors: 25 MHz PIO, logged");
    /* on again (the card already in high speed), then off */
    CHECK(sd_set_fast(mode) == 1 && sd_rep.fast == mode, "on again");
    CHECK(sd_set_fast(SD_FAST_OFF) == 0 && sd_rep.fast == 0 && clkdiv_now == 5, "off: CLKDIV 5 again");
    dma0 = n_cmd18_dma;
    CHECK(sd_read(100, 256, (uintptr_t)big) == 0 && n_cmd18_dma == dma0, "off: PIO");
    CHECK(bad_cmds == 0, "fast: never a write command, CMD6 mode 1 only 0x80fffff1 after a yes");
}

static void t_no_hs(void)
{
    reset(HCON_OK);
    hs_offer = 0;
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    unsigned n0 = nlogs;
    CHECK(sd_up() == 1, "no HS: card up");
    CHECK(cmd6_query == 1 && cmd6_set == 0, "no HS: queried, never switched");
    CHECK(sd_rep.fast == 0 && clkdiv_now == 5 && sd_rep.hs_support == 0, "no HS: 25 MHz PIO");
    CHECK(nlogs == n0 + 1 && strstr(lastlog, "does not offer high speed"), "no HS: one log line");
    CHECK(sd_read(3, 64, (uintptr_t)big) == 0 && n_cmd18_dma == 0, "no HS: reads PIO");
    CHECK(bad_cmds == 0, "no HS: no write, no switch");
}

static void t_old_card(void)
{
    reset(HCON_OK);
    scr_spec = 0;
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == 0 && cmd6_query == 0 && cmd6_set == 0, "SD 1.0 card: no CMD6 at all");
}

static void t_no_idmac(void)
{
    reset((3U << 16) | (2U << 7));              /* no DMA interface, 32-bit addressing */
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == 0 && cmd6_set == 0 && clkdiv_now == 5, "no 64-bit IDMAC: nothing switched");
}

static void t_dma_check_fails(void)
{
    reset(HCON_OK);
    sd_allow();
    dma_fail_next = 1;                          /* the 8-sector IDMAC check */
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == 0 && clkdiv_now == 5 && strstr(lastlog, "IDMAC read failed"),
          "IDMAC check fails: 25 MHz PIO, logged");
    CHECK(sd_read(3, 64, (uintptr_t)big) == 0 && data_ok(3, 64, big), "then reads PIO");
}

static void t_late(void)
{
    reset(HCON_OK);
    sd_allow();
    CHECK(sd_up() == 1 && clkdiv_now == 5, "late: up on the old path");
    int n0 = ncmds;
    CHECK(sd_set_fast(SD_FAST_ON) == 1 && clkdiv_now == 3 && cmd6_set == 1, "late: switched now");
    CHECK(!sd_rep.csd_ok && find_cmd(9, n0) < 0, "late: no CMD9 outside stand-by");
    CHECK(bad_cmds == 0, "late: no write command");
}

/* ---- the bus clock hook (kern/sdclk.c's divider through sd_set_bus_hook / sd_rebus) ---- */
#define BUS_RAISED 199999969ULL                 /* PLL_MMC 799.99988 MHz / 1 / 4 */
#define BUS_SBOOT  12499998ULL                  /* the same / 16 / 4 */
static unsigned hook_raise, hook_restore, hook_bad;
static int hook_changes = 1;

static uint64_t fake_hook(int restore)
{
    if (R(0x10) != 0 || clk_on_at_update)       /* the card clock must be stopped, and the controller told so */
        ++hook_bad;
    if (restore) {
        ++hook_restore;
        cmu_div_reg = (cmu_div_reg & ~0x1ffU) | 15U;
        return BUS_SBOOT;
    }
    ++hook_raise;
    if (!hook_changes)
        return (cmu_div_reg & 0x1ffU) == 0 ? BUS_RAISED : BUS_SBOOT;
    cmu_div_reg &= ~0x1ffU;
    return BUS_RAISED;
}

static void hook_reset(void)
{
    hook_raise = hook_restore = hook_bad = 0;
    hook_changes = 1;
}

/* What drivers run 11 saw: S-Boot's /16, no hook: 6.3 MHz on both paths; the divider's 9 bits read */
static void t_sboot_div(void)
{
    reset(HCON_OK);
    cmu_div_reg = 0x1000000fU;                  /* DIVRATIO 15, automatic clock gating bit 28 */
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1, "S-Boot /16: card up");
    CHECK(sd_rep.bus_hz == 806000000ULL / 16U / 4U && sd_rep.cmu_div == 0x1000000fU, "S-Boot /16: bus 12.59 MHz estimated");
    CHECK(sd_rep.fast == SD_FAST_ON && clkdiv_now == 1 && sd_rep.card_hz < 6300000ULL, "S-Boot /16: fast path at 6.3 MHz (run 11)");
    CHECK(sd_set_fast(SD_FAST_OFF) == 0 && clkdiv_now == 1, "S-Boot /16: old path at 6.3 MHz too");
    reset(HCON_OK);
    cmu_div_reg = 0x1fU;                        /* DIVRATIO 31: bits 8:0, not 3:0 */
    sd_allow();
    CHECK(sd_up() == 1 && sd_rep.bus_hz == 806000000ULL / 32U / 4U, "DIVRATIO is 9 bits wide");
    CHECK(bad_cmds == 0, "S-Boot /16: no write command");
}

static void t_hook(int mode)
{
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 0x1000000fU;
    sd_set_bus_hook(fake_hook);
    sd_allow();
    sd_set_fast(mode);
    CHECK(hook_raise == 0, "hook: not called before sd_up");
    CHECK(sd_up() == 1, "hook: card up");
    CHECK(hook_raise == 1 && hook_restore == 0 && hook_bad == 0, "hook: called once, with the card clock stopped");
    CHECK(find_cmd(0, 0) >= 0 && ncmds > 0, "hook: then the init");
    CHECK(sd_rep.bus_set == 1 && sd_rep.cmu_div_was == 0x1000000fU && sd_rep.cmu_div == 0x10000000U &&
          sd_rep.bus_hz_was == 806000000ULL / 16U / 4U && sd_rep.bus_hz == BUS_RAISED,
          "hook: report (was /16, now /1, its bus clock)");
    CHECK(nclkdivs >= 3 && clkdivs[0] == 250U, "hook: init at 400 kHz from 200 MHz (CLKDIV 250)");
    CHECK(nclkdivs >= 3 && clkdivs[1] == 4U, "hook: 25 MHz from 200 MHz (CLKDIV 4)");
    CHECK(sd_rep.fast == mode && clkdiv_now == 2U && sd_rep.card_hz <= 50000000ULL && sd_rep.card_hz > 49900000ULL,
          "hook: high speed at 50.0 MHz (CLKDIV 2), not over");
    CHECK(sd_set_fast(SD_FAST_OFF) == 0 && clkdiv_now == 4U && sd_rep.card_hz <= 25000000ULL &&
          sd_rep.card_hz > 24900000ULL, "hook: off = 25.0 MHz");
    memset(big, 0, sizeof big);
    CHECK(sd_read(77, 256, (uintptr_t)big) == 0 && data_ok(77, 256, big), "hook: reads");
    CHECK(bad_cmds == 0, "hook: no write command");
}

static void t_hook_fail(void)
{
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 15U;
    fail_cmd8_on_div0 = 1;                      /* the card fails on the raised clock */
    sd_set_bus_hook(fake_hook);
    sd_allow();
    unsigned n0 = nlogs;
    CHECK(sd_up() == 1, "hook fail: up on the second try");
    CHECK(hook_raise == 1 && hook_restore == 1 && hook_bad == 0, "hook fail: raised, put back, card clock stopped both times");
    CHECK((cmu_div_reg & 0x1ffU) == 15U && sd_rep.bus_hz == BUS_SBOOT && sd_rep.bus_set == -1, "hook fail: /16 again, its bus clock");
    CHECK(sd_rep.bus_why && strstr(sd_rep.bus_why, "CMD8"), "hook fail: the failed step kept");
    CHECK(nlogs == n0 + 1 && strstr(lastlog, "failed on the raised bus clock"), "hook fail: one log line");
    CHECK(clkdiv_now == 1U, "hook fail: 25 MHz from 12.5 MHz = CLKDIV 1");

    /* the divider did not change (already /1): no second try, no put back */
    reset(HCON_OK);
    hook_reset();
    hook_changes = 0;
    fail_cmd8_on_div0 = 1;
    sd_set_bus_hook(fake_hook);
    sd_allow();
    CHECK(sd_up() == 0 && hook_raise == 1 && hook_restore == 0 && sd_rep.bus_set == 2, "unchanged divider: no retry");
    CHECK(bad_cmds == 0, "hook fail: no write command");
}

static void t_rebus(void)
{
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 15U;
    CHECK(sd_rebus(fake_hook, 0) == -1 && hook_raise == 0, "rebus: card not up = refused, hook not called");
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == SD_FAST_ON && clkdiv_now == 1U, "rebus: up at /16 (6.3 MHz)");
    int cmds0 = ncmds;
    CHECK(sd_rebus(fake_hook, 0) == 0 && hook_raise == 1 && hook_bad == 0, "rebus: raised with the card clock stopped");
    CHECK(ncmds == cmds0, "rebus: no card command");
    CHECK(sd_rep.fast == SD_FAST_ON && clkdiv_now == 2U && sd_rep.bus_hz == BUS_RAISED && sd_rep.bus_set == 1,
          "rebus: fast reads go on at 50.0 MHz");
    memset(big, 0, sizeof big);
    unsigned dma0 = n_cmd18_dma;
    CHECK(sd_read(300, 256, (uintptr_t)big) == 0 && data_ok(300, 256, big) && n_cmd18_dma == dma0 + 1, "rebus: IDMAC reads");
    CHECK(sd_rebus(fake_hook, 1) == 0 && hook_restore == 1 && hook_bad == 0, "rebus: put back with the card clock stopped");
    CHECK((cmu_div_reg & 0x1ffU) == 15U && sd_rep.bus_hz == BUS_SBOOT && sd_rep.bus_set == 0 && clkdiv_now == 1U,
          "rebus: /16 again, 6.25 MHz");
    sd_set_fast(SD_FAST_OFF);
    CHECK(sd_rebus(fake_hook, 0) == 0 && sd_rep.fast == 0 && clkdiv_now == 4U, "rebus on the old path: 25.0 MHz");
    CHECK(bad_cmds == 0, "rebus: no write command");
}

/* Track A's question 2: a card without high speed while the kernel raised the bus clock: S-Boot's divider back,
 * the card at the 6.25 MHz it always had */
static void t_hook_no_hs(void)
{
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 0x1000000fU;
    hs_offer = 0;
    sd_set_bus_hook(fake_hook);
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == 0, "no hs + hook: card up, no fast reads");
    CHECK(hook_raise == 1 && hook_restore == 1 && hook_bad == 0, "no hs + hook: the divider raised, then back (clock stopped)");
    CHECK(sd_rep.bus_hz == BUS_SBOOT && sd_rep.card_hz <= 6250000ULL && sd_rep.card_hz > 6200000ULL,
          "no hs + hook: the card at 6.25 MHz, as before the fast path");
    CHECK(strstr(lastlog, "S-Boot's CMU divider back") != 0, "no hs + hook: logged");
    hs_offer = 1;
}

/* Track A's question 1: a write (the state slot's) with fast reads on and the bus clock raised: the write command goes
 * out at 6.25 MHz (CLKDIV 16 from 200 MHz) with the card in default speed; high speed and 50 MHz again after */
static void t_write_clock(void)
{
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 0x1000000fU;
    sd_set_bus_hook(fake_hook);
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1 && sd_rep.fast == SD_FAST_ON && clkdiv_now == 2U, "write: fast reads on at 50 MHz");
    fence_on = 1;
    n_writes = 0, write_clkdiv = 0, write_hs = -1;
    (void)sd_write(4096, 8, (uintptr_t)big);         /* the fake does not complete a write: the command is what counts */
    fence_on = 0;
    CHECK(n_writes >= 1, "write: no write command reached the card");
    CHECK(write_clkdiv == 16U, "write: CLKDIV %u at the write (16 = 6.25 MHz from 200 MHz)");
    CHECK(write_hs == 0, "write: the card was not in default speed at the write");
    CHECK(card_hs == 1 && clkdiv_now == 2U && sd_rep.fast == SD_FAST_ON, "write: high speed and 50 MHz again after");
    /* fast reads turned off while the bus stays raised: still 6.25 MHz for a write */
    reset(HCON_OK);
    hook_reset();
    cmu_div_reg = 0x1000000fU;
    hook_changes = 1;
    sd_set_bus_hook(fake_hook);
    sd_allow();
    sd_set_fast(SD_FAST_ON);
    CHECK(sd_up() == 1, "write 2: up");
    CHECK(sd_set_fast(SD_FAST_OFF) == 0 && sd_rep.card_hz > 24900000ULL, "write 2: fast off, the bus still raised (25 MHz)");
    fence_on = 1;
    n_writes = 0, write_clkdiv = 0;
    (void)sd_write(4096, 8, (uintptr_t)big);
    fence_on = 0;
    CHECK(n_writes >= 1 && write_clkdiv == 16U, "write 2: CLKDIV %u at the write (16 = 6.25 MHz)");
    CHECK(clkdiv_now == 4U, "write 2: the 25 MHz setting back after the write");
}

int main(void)
{
    t_default();
    t_fast(SD_FAST_ON);
    t_fast(SD_FAST_HS50);
    t_no_hs();
    t_old_card();
    t_no_idmac();
    t_dma_check_fails();
    t_late();
    t_sboot_div();
    t_hook(SD_FAST_ON);
    t_hook(SD_FAST_HS50);
    t_hook_fail();
    t_rebus();
    t_hook_no_hs();
    t_write_clock();
    printf("sd-test: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
