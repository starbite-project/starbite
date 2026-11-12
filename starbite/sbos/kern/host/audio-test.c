/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/amp.c and kern/codec.c against a fake bus: every MMIO, I2C, SPI, GPIO, PMIC access and delay of
 * the drivers is traced; fake MAX98512 dies (both register maps) and a fake CS47L93 behind a model of the Exynos SPI
 * controller answer them. Checks: the writes and their order for each call, the speaker / earpiece caps on every
 * write, nothing written outside the allowed addresses, and that a bus error at any point leaves the part off.
 * Build and run: starbite/sbos/kern/host/audio-test.sh (the Mac's cc). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../amp.h"
#include "../codec.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static int verbose;
static unsigned long long sim_us;

/* ---- the trace -------------------------------------------------------------------------------------------------- */

enum { E_R32, E_W32, E_IW, E_IR, E_SW, E_SW32, E_SR, E_GPIO, E_PR, E_PW, E_DELAY };
struct ev {
    int k;
    unsigned a, b, c;               /* W32: addr, value; IW: die, chip reg, value; SW: reg, value; GPIO: bank, pin, lvl */
};
#define MAXEV 200000
static struct ev evs[MAXEV];
static int nev;
static void ev(int k, unsigned a, unsigned b, unsigned c)
{
    if (nev < MAXEV)
        evs[nev++] = (struct ev){ k, a, b, c };
}
static void trace_reset(void)
{
    nev = 0;
}

static char logbuf[400000];
static size_t loglen;
static void hlog(const char *s)
{
    size_t n = strlen(s);
    if (loglen + n + 2 < sizeof logbuf) {
        memcpy(logbuf + loglen, s, n);
        loglen += n;
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
    if (verbose)
        printf("    | %s\n", s);
}

/* ---- MMIO: a sparse map, with the SPI controller modelled ---------------------------------------------------- */

#define NMM 512
static struct { uintptr_t a; uint32_t v; } mm[NMM];
static int nmm;
static uint32_t *mmref(uintptr_t a)
{
    for (int i = 0; i < nmm; ++i)
        if (mm[i].a == a)
            return &mm[i].v;
    if (nmm == NMM) {
        printf("mmio map full\n");
        exit(2);
    }
    mm[nmm].a = a, mm[nmm].v = 0;
    return &mm[nmm++].v;
}

#define SPIB 0x10470000UL
#define USI01_MODE 0x1041100CUL
#define USI07_MODE 0x10811024UL
#define GPP0 0x10430000UL
#define GPG1 0x104300A0UL
#define GPP4 0x10830000UL
#define CLKOUT0 0x14060A00UL
#define CMU0 0x10400000UL
#define CMU1 0x10800000UL

/* the fake codec */
static uint16_t c16[0x2000];
static uint32_t c32[0x400];         /* 0x3000 + 2i */
static int codec_reset_high, ldo34_on_state, fake_id = 0x6371, boot_never, spi_dead_after = -1, spi_xfers;
static unsigned long long release_us;
static int fll_upd_seen, fll_never;
static uint8_t trb[16];
static int tridx, cs_low;
static uint8_t rxq[256];
static int rxn, rxr;

static int codec_alive(void)
{
    return codec_reset_high && ldo34_on_state && (*mmref(USI01_MODE) & 7) == 2 &&
           (*mmref(GPP0) >> 16) == 0x2222 && *mmref(SPIB + 0xC4) == 0;
}

static void codec_regs_reset(void)
{
    memset(c16, 0, sizeof c16);
    memset(c32, 0, sizeof c32);
    c16[0x321] = 0x0180, c16[0x329] = 0x0180;   /* muted, 0 dB */
    c16[0x701] = 0x0080, c16[0x709] = 0x0080;
    c16[0x500] = 0x000C;
    for (int p = 0; p < 16; ++p)
        c16[0x1700 + 2 * p] = 0x2001, c16[0x1701 + 2 * p] = 0xA000;
    fll_upd_seen = 0;
}

static int fll_locked(void)
{
    uint32_t ck = *mmref(CLKOUT0);
    return !fll_never && (c16[0x171] & 1) && !(c16[0x171] & 4) && fll_upd_seen && ((ck >> 8) & 0x3F) == 1 && !(ck & 1);
}

static uint16_t c16_read(unsigned r)
{
    if (r == 0)
        return (uint16_t)fake_id;
    if (r == 1)
        return 0x0004;
    if (r == 0x1880)
        return (!boot_never && sim_us - release_us >= 3000) ? 0x0080 : 0;
    if (r == 0x1881)
        return fll_locked() ? 0x0100 : 0;
    if (r == 0x301)
        return c16[0x300];
    return r < 0x2000 ? c16[r] : 0;
}

static void c16_write(unsigned r, uint16_t v)
{
    if (r >= 0x2000)
        return;
    if (r == 0x321 || r == 0x329)
        v &= (uint16_t)~0x0200;             /* IN_VU reads back 0 */
    if (r == 0x172 && (v & 0x8000))
        fll_upd_seen = 1;
    c16[r] = v;
}

static void codec_byte(uint8_t x)
{
    uint8_t out = 0xFF;                     /* nobody driving MISO */
    if (codec_alive()) {
        out = 0;
        if (tridx < 16)
            trb[tridx] = x;
        if (tridx >= 6 && (trb[0] & 0x80)) {
            unsigned reg = ((unsigned)(trb[0] & 0x7F) << 24) | (unsigned)trb[1] << 16 | (unsigned)trb[2] << 8 | trb[3];
            int k = tridx - 6;
            if (reg >= 0x3000) {
                uint32_t d = c32[((reg - 0x3000) / 2) & 0x3FF];
                out = k < 4 ? (uint8_t)(d >> (8 * (3 - k))) : 0;
            } else {
                uint16_t d = c16_read(reg);
                out = k < 2 ? (uint8_t)(d >> (8 * (1 - k))) : 0;
            }
        }
    }
    ++tridx;
    rxq[rxn++ & 255] = out;
}

static void codec_cs(int low)
{
    if (low && !cs_low) {
        tridx = 0;
        ++spi_xfers;
    }
    if (!low && cs_low && codec_alive() && tridx >= 8 && !(trb[0] & 0x80)) {
        unsigned reg = (unsigned)trb[0] << 24 | (unsigned)trb[1] << 16 | (unsigned)trb[2] << 8 | trb[3];
        if (tridx == 10 && reg >= 0x3000) {
            uint32_t v = (uint32_t)trb[6] << 24 | (uint32_t)trb[7] << 16 | (uint32_t)trb[8] << 8 | trb[9];
            c32[((reg - 0x3000) / 2) & 0x3FF] = v;
            ev(E_SW32, reg, v, 0);
        } else if (tridx == 8) {
            uint16_t v = (uint16_t)(trb[6] << 8 | trb[7]);
            c16_write(reg, v);
            ev(E_SW, reg, v, 0);
        }
    }
    if (!low && cs_low && tridx == 8 && (trb[0] & 0x80))
        ev(E_SR, ((unsigned)(trb[0] & 0x7F) << 24) | (unsigned)trb[1] << 16 | (unsigned)trb[2] << 8 | trb[3], 0, 0);
    cs_low = low;
}

static uint32_t f_rd32(uintptr_t a)
{
    ev(E_R32, (unsigned)a, 0, 0);
    if (a == SPIB + 0x14)
        return (uint32_t)((rxn - rxr) & 0x7F) << 15;
    if (a == SPIB + 0x1C) {
        if (rxr < rxn)
            return rxq[rxr++ & 255];
        return 0;
    }
    if (a == CMU0 + 0x1808)
        return *mmref(a) & ~(1U << 16);     /* never busy */
    return *mmref(a);
}

static void f_wr32(uintptr_t a, uint32_t v)
{
    ev(E_W32, (unsigned)a, v, 0);
    if (a == SPIB + 0x18) {
        uint32_t ch = *mmref(SPIB);
        int dead = spi_dead_after >= 0 && spi_xfers > spi_dead_after;
        if ((ch & 3) == 3 && !(ch & (1U << 5)) && !dead && cs_low)
            codec_byte((uint8_t)v);
        return;
    }
    if (a == SPIB + 0x0C)
        codec_cs(!(v & 1));
    if (a == SPIB && (v & (1U << 5)))
        rxn = rxr = 0;                      /* SW_RST empties the FIFOs */
    *mmref(a) = v;
}

/* ---- the fake amplifiers -------------------------------------------------------------------------------------- */

struct fdie {
    unsigned addr;
    int kind;                       /* 'A', 'B', 0 absent */
    uint8_t r[0x1000];
    int stuck_reg;                  /* a register that reads 0 whatever is written (-1 none) */
};
static struct fdie fd[2];
static int i2c_fail_from = -1, i2c_fail_count, i2c_xfers, inj_nonpoll;   /* the bus fails for i2c_fail_count transactions from #from */

static unsigned sh(const struct fdie *d)
{
    return d->kind == 'B' ? 0x500 : 0x400;
}

static void fdie_reset(struct fdie *d)
{
    memset(d->r, 0, sizeof d->r);
    if (d->kind == 'A') {           /* max98512_reg defaults that matter */
        d->r[0x14] = 0x75, d->r[0x15] = 0x8C, d->r[0x16] = 0x08, d->r[0x17] = 0x55, d->r[0x20] = 0x22, d->r[0x22] = 0x22;
        d->r[0x402] = 0x41;
    } else if (d->kind == 'B') {
        d->r[0x23] = 0x22, d->r[0x25] = 0x22;
        d->r[0x600] = 0x43;
    }
}

static struct fdie *die_at(unsigned addr)
{
    for (int i = 0; i < 2; ++i)
        if (fd[i].addr == addr && fd[i].kind)
            return &fd[i];
    return 0;
}

/* register numbers (chip addresses) of the gain registers, for the caps monitor */
static int cap_violations;
static void cap_monitor(const struct fdie *d, unsigned reg, uint8_t v)
{
    unsigned gain = d->kind == 'B' ? 0x3B : 0x3A, vol = d->kind == 'B' ? 0x36 : 0x35;
    unsigned vmax = d->addr == 0x39 ? 0x20 : 0x34;
    if (reg == gain && ((v & 7) > 1 || (v & 0x70))) {
        ++cap_violations;
        printf("  CAP: die %02x SPK_GAIN <- %02x\n", d->addr, v);
    }
    if (reg == vol && (v > vmax || (v & 0x80))) {
        ++cap_violations;
        printf("  CAP: die %02x AMP_VOL_CTRL <- %02x\n", d->addr, v);
    }
}

static int f_i2c(uintptr_t bus, unsigned addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
    ++i2c_xfers;
    if (bus != AMP_BUS)
        return -1;
    if (i2c_fail_from >= 0 && i2c_xfers > i2c_fail_from && i2c_xfers <= i2c_fail_from + i2c_fail_count) {
        unsigned rg = wn >= 2 ? ((unsigned)w[0] << 8 | w[1]) : 0;
        if (!(rn == 1 && (rg == 0x402 || rg == 0x600)))
            ++inj_nonpoll;          /* a failure the driver must report (not the soft reset's answer poll) */
        return -1;
    }
    struct fdie *d = die_at(addr);
    if (!d || wn < 2)
        return -1;
    unsigned reg = (unsigned)w[0] << 8 | w[1];
    if (reg >= 0x1000)
        return -1;
    if (wn == 3 && !rn) {
        ev(E_IW, addr, reg, w[2]);
        cap_monitor(d, reg, w[2]);
        if (reg == (sh(d) | 1u) && (w[2] & 1))
            fdie_reset(d);
        else if ((int)reg != d->stuck_reg)
            d->r[reg] = w[2];
        return 0;
    }
    if (wn == 2 && rn == 1) {
        r[0] = (int)reg == d->stuck_reg ? 0 : d->r[reg];
        ev(E_IR, addr, reg, r[0]);
        return 0;
    }
    return -1;
}

/* ---- GPIO, PMIC, delay ------------------------------------------------------------------------------------------ */

static uint8_t pmic[256];
static int pmic_write_ok = 1;
static int f_gpio_set(uintptr_t bank, unsigned pin, int level)
{
    ev(E_GPIO, (unsigned)bank, pin, (unsigned)level);
    if (bank == GPG1 && pin == 0) {
        if (level && !codec_reset_high) {
            release_us = sim_us;
            codec_regs_reset();
        }
        codec_reset_high = level;
    }
    return 0;
}
static int f_gpio_get(uintptr_t bank, unsigned pin)
{
    return bank == GPG1 && pin == 0 ? codec_reset_high : 0;
}
static int f_pmic_read(unsigned reg, uint8_t *v)
{
    ev(E_PR, reg, 0, 0);
    *v = pmic[reg & 255];
    return 0;
}
static int f_pmic_write(unsigned reg, uint8_t v)
{
    ev(E_PW, reg, v, 0);
    if (!pmic_write_ok)
        return -5;
    pmic[reg & 255] = v;
    if (reg == 0x6B)
        ldo34_on_state = (v & 0xC0) == 0xC0;
    return 0;
}
static void f_delay(unsigned us)
{
    ev(E_DELAY, us, 0, 0);
    sim_us += us;
}

static struct audio_hal hal = { f_rd32, f_wr32, f_i2c, 0, f_gpio_set, f_gpio_get, f_pmic_read, f_pmic_write, f_delay,
                                hlog };

/* ---- the world -------------------------------------------------------------------------------------------------- */

static void world(int kind39, int kind3a, int mux_top, int ldo34_on)
{
    memset(mm, 0, sizeof mm);
    nmm = 0;
    trace_reset();
    loglen = 0;
    logbuf[0] = 0;
    sim_us = 0;
    cap_violations = 0;
    i2c_fail_from = -1, i2c_fail_count = 0, i2c_xfers = 0;
    spi_dead_after = -1, spi_xfers = 0;
    boot_never = 0, fake_id = 0x6371, fll_never = 0;
    fd[0] = (struct fdie){ 0x39, kind39, { 0 }, -1 };
    fd[1] = (struct fdie){ 0x3A, kind3a, { 0 }, -1 };
    fdie_reset(&fd[0]);
    fdie_reset(&fd[1]);
    /* what S-Boot leaves (Starbite's probes on this phone: USI01 mode 0, gpp0 con 0, gpg1-0 output low) */
    *mmref(CMU0 + 0x2010) = *mmref(CMU0 + 0x205C) = *mmref(CMU0 + 0x20BC) = *mmref(CMU0 + 0x20C0) = 0x00200000;
    *mmref(CMU1 + 0x203C) = *mmref(CMU1 + 0x2094) = *mmref(CMU1 + 0x20E4) = *mmref(CMU1 + 0x20E8) = 0x00200000;
    *mmref(CMU0 + 0x0120) = mux_top ? 0x10 : 0x00;
    *mmref(CMU0 + 0x1808) = 0x0;
    *mmref(SPIB + 0xC4) = 1;                /* USI_RESET set out of reset */
    *mmref(GPG1) = 0x1, *mmref(GPG1 + 4) = 0;
    *mmref(CLKOUT0) = 0x00003F01;           /* disabled, another source */
    *mmref(GPP4) = 0x11000000;
    codec_reset_high = 0;
    memset(pmic, 0, sizeof pmic);
    pmic[0x6A] = 0xE8;                      /* on, 1.8 V */
    pmic[0x6B] = ldo34_on ? 0xE8 : 0x28;
    ldo34_on_state = ldo34_on;
    pmic_write_ok = 1;
    hal.pmic_write = f_pmic_write;
    rxn = rxr = 0, cs_low = 0, tridx = 0;
    codec_regs_reset();
    amp_hal_set(&hal);
    codec_hal_set(&hal);
}

/* ---- trace helpers --------------------------------------------------------------------------------------------- */

/* the writes to one die, in order: (chip reg, value) */
static int die_writes(unsigned addr, unsigned (*out)[2], int max)
{
    int n = 0;
    for (int i = 0; i < nev && n < max; ++i)
        if (evs[i].k == E_IW && evs[i].a == addr)
            out[n][0] = evs[i].b, out[n][1] = evs[i].c, ++n;
    return n;
}

/* index of the first event matching, from `from'; -1 none */
static int find(int from, int k, unsigned a, long b)
{
    for (int i = from; i < nev; ++i)
        if (evs[i].k == k && evs[i].a == a && (b < 0 || evs[i].b == (unsigned)b))
            return i;
    return -1;
}

/* each (k, a, b) of the list found in this order in the trace */
struct step {
    int k;
    unsigned a;
    long b;
};
static int in_order(const struct step *s, int n, const char *what)
{
    int at = 0;
    for (int i = 0; i < n; ++i) {
        int j = find(at, s[i].k, s[i].a, s[i].b);
        if (j < 0) {
            printf("  FAIL %s: step %d (kind %d, %x, %lx) not found in order\n", what, i, s[i].k, s[i].a, s[i].b);
            return 0;
        }
        at = j + 1;
    }
    return 1;
}

static int die_off_state(const struct fdie *d)
{
    unsigned amp_en = d->kind == 'B' ? 0x39 : 0x38;
    return !(d->r[sh(d)] & 1) && !(d->r[amp_en] & 1);
}

/* every MMIO write inside the allowed set */
static int mmio_writes_allowed(void)
{
    for (int i = 0; i < nev; ++i) {
        if (evs[i].k != E_W32)
            continue;
        unsigned a = evs[i].a;
        int ok = a == USI07_MODE || a == GPP4 || a == GPP4 + 8 || a == USI01_MODE || a == GPP0 || a == GPP0 + 8 ||
                 a == CMU0 + 0x1808 || a == CLKOUT0 || (a >= SPIB && a < SPIB + 0x100);
        if (!ok) {
            printf("  MMIO write outside the allowed set: %08x <- %08x\n", a, evs[i].b);
            return 0;
        }
    }
    return 1;
}

/* ---- the amplifier tests --------------------------------------------------------------------------------------- */

static void t_amp_probe(void)
{
    printf("amp_probe (both MAX98512, first revision)\n");
    world('A', 'A', 1, 1);
    int rc = amp_probe();
    CHECK(rc == 0, "rc %d", rc);
    uint8_t rev = 0;
    CHECK(amp_die_kind(0x39, &rev) == 'A' && rev == 0x41, "0x39 kind %c rev %x", amp_die_kind(0x39, 0), rev);
    CHECK(amp_die_kind(0x3A, 0) == 'A', "0x3A");
    int w = 0;
    for (int i = 0; i < nev; ++i)
        w += evs[i].k == E_IW;
    CHECK(w == 0, "the probe wrote %d amplifier registers", w);
    CHECK(find(0, E_IR, 0x39, 0x402) >= 0 && find(0, E_IR, 0x3A, 0x402) >= 0, "REV_ID reads");
    CHECK(find(0, E_W32, USI07_MODE, 4) >= 0, "USI07 mode 4");
    CHECK(*mmref(GPP4) == 0x11220000, "gpp4 con %08x", *mmref(GPP4));
    CHECK(mmio_writes_allowed(), "mmio");
    CHECK(strstr(logbuf, "amp: probe: both dies found") != 0, "log line");

    printf("amp_probe (0x39 a MAX98512B, 0x3A the first revision)\n");
    world('B', 'A', 1, 1);
    rc = amp_probe();
    CHECK(rc == 0 && amp_die_kind(0x39, &rev) == 'B' && rev == 0x43, "rc %d kind %c", rc, amp_die_kind(0x39, 0));
    CHECK(find(0, E_IR, 0x39, 0x600) >= 0, "B's REV_ID at 0x600 read");

    printf("amp_probe (no die at 0x3A)\n");
    world('A', 0, 1, 1);
    rc = amp_probe();
    CHECK(rc == AUD_ENODEV, "rc %d", rc);
    rc = amp_receiver(1, 0);
    CHECK(rc < 0, "receiver refused without a probe: rc %d", rc);
    w = 0;
    for (int i = 0; i < nev; ++i)
        w += evs[i].k == E_IW && evs[i].b != 0x400 && evs[i].b != 0x38 && evs[i].b != 0x3C;
    CHECK(w == 0, "nothing but shut-down bits written (%d)", w);
}

/* Android's __max98512_spk_enable for OSM_RCV_L on 0x39, first revision, level 3 = One UI's handset gain */
static const unsigned rcv_tail_a[][2] = {
    { 0x3A, 0x01 }, { 0x35, 0x20 }, { 0x3C, 0x00 }, { 0x59, 0x30 }, { 0x5A, 0x10 }, { 0x87, 0x02 },
    { 0x18, 0x03 }, { 0x25, 0x80 }, { 0x26, 0x01 }, { 0x38, 0x01 }, { 0x38, 0x41 }, { 0x400, 0x01 },
};

static void t_amp_receiver(void)
{
    unsigned w[4000][2];
    printf("amp_receiver(1, 3): earpiece, One UI's handset gain (first revision)\n");
    world('A', 'A', 1, 1);
    CHECK(amp_probe() == 0, "probe");
    trace_reset();
    int rc = amp_receiver(1, 3);
    CHECK(rc == 0, "rc %d (%s)", rc, amp_why());
    int n = die_writes(0x39, w, 4000);
    CHECK(n > 60, "writes to 0x39: %d", n);
    CHECK(n >= 2 && w[0][0] == 0x400 && w[0][1] == 0 && w[1][0] == 0x401 && w[1][1] == 1,
          "0x39 starts with GLOBAL_SHDN 0 then SOFT_RESET 1 (got %x=%x, %x=%x)", w[0][0], w[0][1], w[1][0], w[1][1]);
    int tail = (int)(sizeof rcv_tail_a / sizeof rcv_tail_a[0]);
    int ok = n >= tail;
    for (int i = 0; ok && i < tail; ++i)
        ok = w[n - tail + i][0] == rcv_tail_a[i][0] && w[n - tail + i][1] == rcv_tail_a[i][1];
    CHECK(ok, "the enable writes are __max98512_spk_enable's for Receiver Left, in order");
    if (!ok)
        for (int i = n - tail; i < n; ++i)
            printf("    got %04x <- %02x\n", w[i][0], w[i][1]);
    /* the other die shut down before 0x39 is touched */
    int first39 = find(0, E_IW, 0x39, -1), off3a = find(0, E_IW, 0x3A, 0x400);
    CHECK(off3a >= 0 && off3a < first39, "0x3A shut down first");
    CHECK(die_off_state(&fd[1]), "0x3A off");
    /* the setup's key values, in order (max98512_probe, dai_set_fmt, hw_params) */
    static const struct step setup[] = {
        { E_IW, 0x39, 0x3C }, { E_IW, 0x39, 0x1C }, { E_IW, 0x39, 0x1D }, { E_IW, 0x39, 0x25 }, { E_IW, 0x39, 0x26 },
        { E_IW, 0x39, 0x35 }, { E_IW, 0x39, 0x3A }, { E_IW, 0x39, 0x36 }, { E_IW, 0x39, 0x3D }, { E_IW, 0x39, 0x3E },
        { E_IW, 0x39, 0x40 }, { E_IW, 0x39, 0x3F }, { E_IW, 0x39, 0x50 }, { E_IW, 0x39, 0x70 }, { E_IW, 0x39, 0x86 },
        { E_IW, 0x39, 0x87 }, { E_IW, 0x39, 0x88 }, { E_IW, 0x39, 0x1A }, { E_IW, 0x39, 0x11 }, { E_IW, 0x39, 0x21 },
        { E_IW, 0x39, 0x20 }, { E_IW, 0x39, 0x18 }, { E_IW, 0x39, 0x39 }, { E_IW, 0x39, 0x34 }, { E_IW, 0x39, 0x23 },
        { E_IW, 0x39, 0x24 }, { E_IW, 0x39, 0x22 },
    };
    CHECK(in_order(setup, (int)(sizeof setup / sizeof setup[0]), "setup order"), "setup order");
    struct fdie *d = &fd[0];
    CHECK(d->r[0x20] == 0x42, "PCM_MODE_CFG %02x (16 bits, I2S, the default's bit 1 kept)", d->r[0x20]);
    CHECK(d->r[0x21] == 0x00 && (d->r[0x22] & 0xF) == 2, "slave, 32 BCLKs a frame");
    CHECK(d->r[0x23] == 0x08 && d->r[0x24] == 0x85, "48 kHz, IV ADC 24 kHz: %02x %02x", d->r[0x23], d->r[0x24]);
    CHECK(d->r[0x3A] == 0x01 && d->r[0x35] == 0x20, "gain %02x vol %02x", d->r[0x3A], d->r[0x35]);
    CHECK(d->r[0x38] == 0x41 && d->r[0x400] == 0x01 && d->r[0x3C] == 0, "on, DEM 0x40, IV sense off");
    CHECK(d->r[0x1A] == 0 && d->r[0x1C] == 0xFF && d->r[0x1D] == 0xFF, "no transmit slots");
    CHECK(d->r[0x70] == 0x16 && d->r[0x87] == 0x02, "current limit 0x16, boost bypass off (first revision)");
    CHECK(cap_violations == 0, "caps");
    CHECK(mmio_writes_allowed(), "mmio");
    CHECK(strstr(logbuf, "amp: receiver: on") != 0, "log");

    printf("amp_receiver levels 0..5 (never above One UI's handset gain), out-of-range levels refused\n");
    for (int lvl = 0; lvl < AMP_LEVELS; ++lvl) {
        rc = amp_receiver(1, lvl);
        CHECK(rc == 0 && fd[0].r[0x35] == amp_rcv_vol[lvl] && fd[0].r[0x3A] == 1, "level %d: vol %02x", lvl,
              fd[0].r[0x35]);
        CHECK(amp_rcv_vol[lvl] <= AMP_RCV_VOL_MAX, "table");
    }
    rc = amp_receiver(1, 6);
    CHECK(rc == AUD_EINVAL && die_off_state(&fd[0]) && die_off_state(&fd[1]), "level 6: rc %d, both off", rc);
    rc = amp_receiver(1, -1);
    CHECK(rc == AUD_EINVAL, "level -1");
    CHECK(cap_violations == 0, "caps");

    printf("amp_receiver(0): GLOBAL_SHDN, AMP_EN, MEAS_EN, then 15 ms\n");
    CHECK(amp_receiver(1, 2) == 0, "on");
    trace_reset();
    rc = amp_receiver(0, 0);
    n = die_writes(0x39, w, 4000);
    CHECK(rc == 0 && n == 3 && w[0][0] == 0x400 && w[0][1] == 0 && w[1][0] == 0x38 && (w[1][1] & 1) == 0 &&
          w[2][0] == 0x3C && w[2][1] == 0, "off order (n %d)", n);
    int dl = find(0, E_DELAY, 16000, -1);
    CHECK(dl > find(0, E_IW, 0x39, 0x3C), "the ramp-down wait after");
    CHECK(die_off_state(&fd[0]), "off");

    printf("amp_receiver (both MAX98512B: the B register map)\n");
    world('B', 'B', 1, 1);
    rc = amp_receiver(1, 3);
    CHECK(rc == 0, "rc %d (%s)", rc, amp_why());
    d = &fd[0];
    CHECK(d->r[0x3B] == 0x01 && d->r[0x36] == 0x20 && d->r[0x39] == 0x41 && d->r[0x500] == 0x01, "B: gain %02x vol %02x "
          "en %02x shdn %02x", d->r[0x3B], d->r[0x36], d->r[0x39], d->r[0x500]);
    CHECK(d->r[0x28] == 0x80 && d->r[0x29] == 0x01 && d->r[0x18] == 0x03, "B: monomix at 0x28/0x29");
    CHECK(d->r[0x7A] == 0x00 && d->r[0x3C] == 0x05 && d->r[0x5E] == 0x16, "B: boost automatic bypass, SSM_CFG, "
          "current limit at 0x5E: %02x %02x %02x", d->r[0x7A], d->r[0x3C], d->r[0x5E]);
    CHECK(find(0, E_IW, 0x39, 0x501) >= 0 && find(0, E_IW, 0x39, 0x401) < 0, "B: SOFT_RESET at 0x501");
    CHECK(d->r[0x26] == 0x08 && d->r[0x27] == 0x85 && d->r[0x23] == 0x42, "B: SR setup at 0x26/0x27, mode at 0x23");
    CHECK(cap_violations == 0, "caps");
}

static void t_amp_speaker(void)
{
    printf("amp_speaker levels 0..5: 0x39 off first, 0x3A Mono Right, capped at +3 dB / -3 dB\n");
    world('A', 'A', 1, 1);
    CHECK(amp_receiver(1, 3) == 0, "receiver on");
    for (int lvl = 0; lvl < AMP_LEVELS; ++lvl) {
        trace_reset();
        int rc = amp_speaker(1, lvl);
        struct fdie *d = &fd[1];
        CHECK(rc == 0, "level %d rc %d (%s)", lvl, rc, amp_why());
        CHECK(d->r[0x3A] == AMP_SPK_GAIN_MAX && d->r[0x35] == amp_spk_vol[lvl], "level %d: gain %02x vol %02x", lvl,
              d->r[0x3A], d->r[0x35]);
        CHECK(d->r[0x35] <= AMP_SPK_VOL_MAX && d->r[0x3A] <= AMP_SPK_GAIN_MAX, "cap");
        CHECK(d->r[0x25] == 0x41 && d->r[0x38] == 0x81 && d->r[0x400] == 1 && d->r[0x18] == 0x03,
              "mono right, DEM 0x80, on");
        CHECK(die_off_state(&fd[0]), "the earpiece off");
        if (lvl == 0)
            CHECK(find(0, E_IW, 0x39, 0x400) < find(0, E_IW, 0x3A, -1), "0x39 shut down before 0x3A is touched");
    }
    CHECK(amp_speaker(1, 9) == AUD_EINVAL && die_off_state(&fd[1]), "level 9 refused, off");
    CHECK(cap_violations == 0, "no write ever above the caps (%d)", cap_violations);
    /* the caps themselves: 15 dB under One UI's +15 dB / 0 dB speaker */
    CHECK(AMP_SPK_GAIN_MAX * 3 + ((int)AMP_SPK_VOL_MAX - 0x40) / 4 <= 15 - 15, "the speaker's total gain");
    CHECK(amp_speaker(0, 0) == 0 && die_off_state(&fd[1]), "speaker off");
    amp_off();
    CHECK(die_off_state(&fd[0]) && die_off_state(&fd[1]), "amp_off: both off");
}

/* a bus error at every transaction of a turn-on: the call fails and both dies end shut down */
static void t_amp_errors(void)
{
    printf("amp: a bus error (3 failed transactions = retries exhausted) at each point of receiver / speaker on\n");
    int total = 0;
    for (int mode = 0; mode < 2; ++mode) {
        world('A', 'B', 1, 1);
        CHECK(amp_probe() == 0, "probe");
        int base = i2c_xfers;
        (void)(mode ? amp_speaker(1, 5) : amp_receiver(1, 3));
        int len = i2c_xfers - base;
        int bad = 0;
        for (int k = 0; k < len; ++k) {
            world('A', 'B', 1, 1);
            if (amp_probe())
                ++bad;
            i2c_fail_from = i2c_xfers + k, i2c_fail_count = 3, inj_nonpoll = 0;
            int rc = mode ? amp_speaker(1, 5) : amp_receiver(1, 3);
            int off = die_off_state(&fd[0]) && die_off_state(&fd[1]);
            if (rc >= 0 && inj_nonpoll) {       /* only the reset poll may absorb failures (it waits for an answer) */
                ++bad;
                printf("  error at transaction %d of %d: rc %d (on: %d %d)\n", k, len, rc, !die_off_state(&fd[0]),
                       !die_off_state(&fd[1]));
            }
            if (rc < 0 && !off) {
                ++bad;
                printf("  error at transaction %d: rc %d but a die is still on\n", k, rc);
            }
            total++;
        }
        CHECK(bad == 0, "%s: %d of %d injection points misbehaved", mode ? "speaker" : "receiver", bad, len);
        CHECK(cap_violations == 0, "caps");
    }
    printf("  (%d injection points)\n", total);

    printf("amp: the bus dead from the start of receiver on\n");
    world('A', 'A', 1, 1);
    CHECK(amp_probe() == 0, "probe");
    i2c_fail_from = i2c_xfers, i2c_fail_count = 1 << 30;
    int rc = amp_receiver(1, 3);
    CHECK(rc < 0 && die_off_state(&fd[0]) && die_off_state(&fd[1]), "rc %d, nothing on", rc);

    printf("amp: a register that does not read back (MEAS_DSP_CFG stuck)\n");
    world('A', 'A', 1, 1);
    fd[0].stuck_reg = 0x3D;
    rc = amp_receiver(1, 3);
    CHECK(rc == AUD_EVERIFY && die_off_state(&fd[0]) && die_off_state(&fd[1]), "rc %d", rc);
    CHECK(strstr(logbuf, "reads back") != 0 && strstr(logbuf, "FAILED") != 0, "logged");

    printf("amp: a closed USI07 gate refuses before any bus traffic\n");
    world('A', 'A', 1, 1);
    *mmref(CMU1 + 0x20E4) = 0x00100000;
    rc = amp_probe();
    int iw = 0;
    for (int i = 0; i < nev; ++i)
        iw += evs[i].k == E_IW || evs[i].k == E_IR || evs[i].k == E_W32;
    CHECK(rc == AUD_EPERM && iw == 0, "rc %d, %d accesses", rc, iw);

    printf("amp: no HAL\n");
    amp_hal_set(0);
    CHECK(amp_probe() == AUD_ENOSYS && amp_receiver(1, 0) == AUD_ENOSYS, "ENOSYS");
    amp_off();
}

static void t_amp_selftest(void)
{
    printf("amp_selftest(100)\n");
    world('A', 'A', 1, 1);
    int rc = amp_selftest(100);
    CHECK(rc == 0, "rc %d", rc);
    CHECK(die_off_state(&fd[0]) && die_off_state(&fd[1]), "off after");
    CHECK(strstr(logbuf, "amp: selftest: done") != 0, "log");
    int maxvol = 0;
    for (int i = 0; i < nev; ++i)
        if (evs[i].k == E_IW && evs[i].a == 0x39 && evs[i].b == 0x35 && (int)evs[i].c > maxvol)
            maxvol = (int)evs[i].c;
    CHECK(maxvol == amp_rcv_vol[0], "the lowest level only: %02x", maxvol);
    CHECK(cap_violations == 0, "caps");
}

/* ---- the codec tests ------------------------------------------------------------------------------------------ */

static void t_codec_probe(void)
{
    printf("codec_probe (LDO34 already on, USI01's IP clock from CMU_TOP)\n");
    world('A', 'A', 1, 1);
    int rc = codec_probe();
    CHECK(rc == 0, "rc %d (%s)", rc, codec_why());
    static const struct step order[] = {
        { E_W32, (unsigned)(CMU0 + 0x1808), 15 }, { E_W32, (unsigned)USI01_MODE, 2 }, { E_W32, (unsigned)(SPIB + 0xC4), 0 },
        { E_W32, (unsigned)(SPIB + 0x2C), 1 }, { E_W32, (unsigned)GPP0, 0x22220000 },
        { E_W32, (unsigned)(GPP0 + 8), 0x30000000 }, { E_GPIO, (unsigned)GPG1, 0 }, { E_DELAY, 20000, -1 },
        { E_PR, 0x6A, -1 }, { E_PR, 0x6B, -1 }, { E_GPIO, (unsigned)GPG1, 0 }, { E_DELAY, 2000, -1 },
        { E_SR, 0x0000, -1 }, { E_SR, 0x1880, -1 }, { E_SW, 0x1800, 0x0080 }, { E_SR, 0x0001, -1 },
    };
    CHECK(in_order(order, (int)(sizeof order / sizeof order[0]), "probe order"), "probe order");
    int gl = -1, gh = -1;
    for (int i = 0; i < nev; ++i)
        if (evs[i].k == E_GPIO && evs[i].a == GPG1) {
            if (evs[i].c == 0 && gl < 0)
                gl = i;
            if (evs[i].c == 1)
                gh = i;
        }
    CHECK(gl >= 0 && gh > gl && find(gl, E_PR, 0x6B, -1) < gh, "reset low, supplies, then reset high");
    int sw = 0;
    for (int i = 0; i < nev; ++i)
        sw += (evs[i].k == E_SW && evs[i].a != 0x1800) || evs[i].k == E_SW32 || evs[i].k == E_PW;
    CHECK(sw == 0, "only BOOT_DONE's acknowledgement written to the codec, no PMIC write (%d)", sw);
    CHECK(codec_reset_high == 1, "out of reset");
    CHECK(mmio_writes_allowed(), "mmio");
    CHECK(strstr(logbuf, "codec: id: 6371 (CS47L92/93)") != 0, "log id");

    printf("codec_probe (USI01 on OSCCLK: its divider not touched)\n");
    world('A', 'A', 0, 1);
    CHECK(codec_probe() == 0 && find(0, E_W32, (unsigned)(CMU0 + 0x1808), -1) < 0, "no CMU write");

    printf("codec_probe (LDO34 off, a PMIC write hook): switched on, then back off by codec_off\n");
    world('A', 'A', 1, 0);
    rc = codec_probe();
    CHECK(rc == 0 && (pmic[0x6B] & 0xC0) == 0xC0 && (pmic[0x6B] & 0x3F) == 40, "rc %d, L34 %02x", rc, pmic[0x6B]);
    codec_off();
    CHECK((pmic[0x6B] & 0xC0) == 0 && codec_reset_high == 0, "LDO34 off again, reset low");
    int pw = 0;
    for (int i = 0; i < nev; ++i)
        pw += evs[i].k == E_PW && evs[i].a != 0x6B;
    CHECK(pw == 0, "no other PMIC register written");

    printf("codec_probe (LDO34 off, no PMIC write hook): refused, reset low\n");
    world('A', 'A', 1, 0);
    hal.pmic_write = 0;
    rc = codec_probe();
    CHECK(rc == AUD_ENOSYS && codec_reset_high == 0, "rc %d", rc);
    hal.pmic_write = f_pmic_write;

    printf("codec_probe (a wrong ID; no boot; a closed gate)\n");
    world('A', 'A', 1, 1);
    fake_id = 0x6364;
    rc = codec_probe();
    CHECK(rc == AUD_ENODEV && codec_reset_high == 0, "wrong ID: rc %d", rc);
    world('A', 'A', 1, 1);
    boot_never = 1;
    rc = codec_probe();
    CHECK(rc == AUD_ETIMEDOUT && codec_reset_high == 0 && find(0, E_SW, 0x1800, 0x80) >= 0,
          "no BOOT_DONE: rc %d, acknowledged anyway, reset low", rc);
    world('A', 'A', 1, 1);
    *mmref(CMU0 + 0x20BC) = 0x00100000;
    rc = codec_probe();
    CHECK(rc == AUD_EPERM && codec_reset_high == 0, "closed gate: rc %d", rc);
    world('A', 'A', 1, 1);
    spi_dead_after = 0;
    rc = codec_probe();
    CHECK(rc < 0 && codec_reset_high == 0, "SPI dead: rc %d", rc);
}

static void t_codec_mic(void)
{
    printf("codec_mic(1): patch, clocks (CLKOUT0, SYSCLK, FLL1), AIF1, MICVDD, MICBIAS1/1A/1B, AIF1TX, inputs\n");
    world('A', 'A', 1, 1);
    CHECK(codec_probe() == 0, "probe");
    trace_reset();
    int rc = codec_mic(1);
    CHECK(rc == 0, "rc %d (%s)", rc, codec_why());
    static const struct step order[] = {
        { E_SW, 0x3A2, 0x2C29 }, { E_SW, 0x120, 0x0345 }, { E_SW, 0x120, 0x0305 }, { E_SW, 0x422, 0x0005 },
        { E_SW32, 0x3030, 0x04A00C01 }, { E_SW32, 0x31D8, 0x022AFF01 },
        { E_SW, 0x100, 0x0041 }, { E_SW, 0x21C, 0x0222 }, { E_SW, 0x218, 0x8060 }, { E_SW, 0x320, 0x0800 },
        { E_SW, 0x328, 0x0800 }, { E_SW, 0x087, 0x00A5 }, { E_SW, 0x1708, 0x2000 }, { E_SW, 0x1709, 0xE000 },
        { E_SW, 0x170E, 0x2000 }, { E_SW, 0x170F, 0xE000 },
        { E_W32, (unsigned)CLKOUT0, 0x00000100 }, { E_SW, 0x101, 0x0404 },
        { E_SW, 0x171, 0x0004 }, { E_SW, 0x172, 22 }, { E_SW, 0x173, 1114 }, { E_SW, 0x174, 1625 },
        { E_SW, 0x175, 1 }, { E_SW, 0x176, 0x0040 }, { E_SW, 0x178, 0x21F0 }, { E_SW, 0x17A, 0xD000 },
        { E_SW, 0x17B, 0x0010 }, { E_SW, 0x17D, 0x0003 }, { E_SW, 0x171, 0x0005 }, { E_SW, 0x17B, 0x0011 },
        { E_SW, 0x172, 0x8000 | 22 }, { E_SW, 0x171, 0x0001 }, { E_SR, 0x1881, -1 },
        { E_SW, 0x322, 0x0500 }, { E_SW, 0x32A, 0x0500 }, { E_SW, 0x320, 0x8800 }, { E_SW, 0x328, 0x8800 },
        { E_SW, 0x30C, 0x0002 }, { E_SW, 0x309, 0x0005 }, { E_SW, 0x321, 0x0194 }, { E_SW, 0x329, 0x018F },
        { E_SW, 0x700, 0x0014 }, { E_SW, 0x701, 0x0080 }, { E_SW, 0x708, 0x0016 }, { E_SW, 0x709, 0x0080 },
        { E_SW, 0x500, 0x000C & ~0xA0 }, { E_SW, 0x504, 0x0002 }, { E_SW, 0x102, 0x0003 }, { E_SW, 0x503, 0x0000 },
        { E_SW, 0x500, 0x000B }, { E_SW, 0x506, 32 }, { E_SW, 0x507, 0x1010 }, { E_SW, 0x508, 0x1010 },
        { E_SW, 0x213, 0x0480 }, { E_SW, 0x200, 0x0001 }, { E_SW, 0x101, 0x0444 }, { E_SW, 0x218, 0x8061 },
        { E_SW, 0x21C, 0x0233 }, { E_SW, 0x519, 0x0003 }, { E_SW, 0x300, 0x00A0 }, { E_SW, 0x321, 0x0094 },
        { E_SW, 0x329, 0x008F }, { E_SW, 0x321, 0x0294 }, { E_SW, 0x329, 0x028F },
    };
    CHECK(in_order(order, (int)(sizeof order / sizeof order[0]), "mic on order"), "mic on order");
    CHECK(c16[0x700] == 0x14 && c16[0x708] == 0x16 && c16[0x300] == 0xA0 && c16[0x519] == 3, "route and enables");
    CHECK(c16[0x321] == 0x94 && c16[0x329] == 0x8F, "unmuted at One UI's in-call volumes: %04x %04x", c16[0x321],
          c16[0x329]);
    CHECK(c16[0x101] == 0x0444 && c16[0x102] == 3 && c16[0x503] == 0 && c16[0x504] == 2 && (c16[0x500] & 0xBF) == 0x0B,
          "SYSCLK on from FLL1 at 98.304 MHz, 48 kHz, AIF1 on SYSCLK, I2S slave, BCLK 1.536 MHz");
    CHECK(fll_locked(), "FLL1 locked");
    CHECK(mmio_writes_allowed(), "mmio");
    int pw = 0;
    for (int i = 0; i < nev; ++i)
        pw += evs[i].k == E_PW;
    CHECK(pw == 0, "no PMIC write");

    printf("codec_mic(0): DAPM's down order, FLL1 off, CLKOUT0 back as found\n");
    trace_reset();
    rc = codec_mic(0);
    static const struct step off[] = {
        { E_SW, 0x321, 0x0394 }, { E_SW, 0x300, 0x0000 }, { E_SW, 0x519, 0x0000 }, { E_SW, 0x21C, 0x0222 },
        { E_SW, 0x218, 0x8060 }, { E_SW, 0x101, 0x0404 }, { E_SW, 0x200, 0x0000 }, { E_SW, 0x171, 0x0005 },
        { E_SW, 0x171, 0x0004 }, { E_W32, (unsigned)CLKOUT0, 0x00003F01 },
    };
    CHECK(rc == 0 && in_order(off, (int)(sizeof off / sizeof off[0]), "mic off order"), "mic off order");
    CHECK(*mmref(CLKOUT0) == 0x00003F01 && !fll_locked() && c16[0x300] == 0, "off");
    CHECK(codec_reset_high == 1, "still powered (codec_off puts it in reset)");
    codec_off();
    CHECK(codec_reset_high == 0, "codec_off: reset low");

    printf("codec_mic(1) without a probe first: probes, then the path\n");
    world('A', 'A', 1, 1);
    rc = codec_mic(1);
    CHECK(rc == 0 && c16[0x300] == 0xA0, "rc %d", rc);
    codec_off();
    CHECK(codec_reset_high == 0 && *mmref(CLKOUT0) == 0x00003F01, "off: reset low, CLKOUT0 restored");
}

static void t_codec_errors(void)
{
    printf("codec: the SPI bus dying at each transfer of mic on: the call fails, the codec ends in reset\n");
    world('A', 'A', 1, 1);
    CHECK(codec_probe() == 0, "probe");
    int base = spi_xfers;
    CHECK(codec_mic(1) == 0, "mic");
    int len = spi_xfers - base, bad = 0, steps = 0;
    for (int k = 0; k < len; k += 3) {
        world('A', 'A', 1, 1);
        if (codec_probe())
            ++bad;
        spi_dead_after = spi_xfers + k;
        int rc = codec_mic(1);
        if (rc >= 0 || codec_reset_high || *mmref(CLKOUT0) != 0x00003F01) {
            ++bad;
            printf("  dead after %d: rc %d, reset %d, clkout %08x\n", k, rc, codec_reset_high, *mmref(CLKOUT0));
        }
        ++steps;
    }
    CHECK(bad == 0, "%d of %d points misbehaved", bad, steps);
    printf("  (%d injection points over %d transfers)\n", steps, len);

    printf("codec: FLL1 never locks\n");
    world('A', 'A', 1, 1);
    CHECK(codec_probe() == 0, "probe");
    fll_never = 1;
    int rc = codec_mic(1);
    CHECK(rc == AUD_ETIMEDOUT && codec_reset_high == 0 && *mmref(CLKOUT0) == 0x00003F01 && c16[0x300] == 0,
          "rc %d, reset low, CLKOUT0 restored", rc);
    CHECK(strstr(logbuf, "did not lock") != 0, "logged");

    printf("codec: no HAL\n");
    codec_hal_set(0);
    CHECK(codec_probe() == AUD_ENOSYS && codec_mic(1) == AUD_ENOSYS, "ENOSYS");
    codec_off();
}

static void t_codec_selftest(void)
{
    printf("codec_selftest\n");
    world('A', 'A', 1, 1);
    int rc = codec_selftest();
    CHECK(rc == 0 && codec_reset_high == 0, "rc %d, in reset after", rc);
    int sw = 0;
    for (int i = 0; i < nev; ++i)
        sw += (evs[i].k == E_SW && evs[i].a != 0x1800) || evs[i].k == E_SW32;
    CHECK(sw == 0, "only BOOT_DONE's acknowledgement written (%d)", sw);
    CHECK(strstr(logbuf, "codec: selftest: done") != 0, "log");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    t_amp_probe();
    t_amp_receiver();
    t_amp_speaker();
    t_amp_errors();
    t_amp_selftest();
    t_codec_probe();
    t_codec_mic();
    t_codec_errors();
    t_codec_selftest();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
