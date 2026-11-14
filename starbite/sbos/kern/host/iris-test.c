/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/iris.c (P1, iris.h) against a fake phone (iris-test.sh builds and runs it; -v traces
 * the driver's log).
 *
 * The fake models: the four GPIO banks (other pins' bits must survive), USI07_I2C's mode word, its CMU_PERIC1 gates and
 * Q-channel, CIS_CLK2 in CMU_TOP; the S2MPS18's LDO39-41 (any other register or a code other than 1.05 / 2.8 / 1.8 V is
 * a violation); the S5K5F1 (answers only with the three supplies, iris_en, reset and MCLK on, MCLK 2 ms before the first
 * access) on the HSI2C and on a bit-banged bus: an I2C slave decodes gpp4-6/7's open-drain lines bit by bit and records
 * every message's bytes; the ISPPRE side (cam.c's port hooks: the port's addresses, settle and format are checked) whose
 * WDMA writes a dark synthetic frame 40 ms after the stream starts; CSIS3 reads fault while the domain is off or when
 * "secure"; the IR LED's seven registers (read only: there is no write hook at all).
 *
 * Checked: the pure parts (LDO codes inside the DT, the 16-bit-register wire layout, the frame statistics); a whole P1 on
 * the HSI2C and on the bit-banged bus (the wire bytes of a 16-bit write and read); the stock power order on and off
 * (iris_en, VDDIO, VDDA, VDDD, 1 ms, I2C, 10 us, reset, MCLK, 2 ms / the reverse), twice; a failure at every power-on
 * step ends in the reverse order of what was switched on; CSIS3 secure, no frame, frame end without data, no sensor,
 * pins that ignore writes, an LDO write failing, an LDO someone else's, the LED changing; an abort at every point; every
 * run leaves every register, supply and pin as found, the stream <= 0.5 s, the domain off. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "iris.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the fake's addresses ------------------------------------------------------------------------------------------ */
#define GPG0 0x10430080ULL
#define GPG2 0x104300C0ULL
#define GPC1 0x10830080ULL
#define GPP4 0x10830000ULL
#define USIM 0x10811024ULL
#define CMUP 0x10800000ULL
#define CMUT 0x1A240000ULL
#define HSB 0x108D0000ULL
#define MEM_PA 0xD8000000ULL

#define NREG 64
struct kv {
    uint64_t pa;
    uint32_t v;
};
static const struct kv init_regs[] = {
    { GPG0, 0x02222222U }, { GPG0 + 4, 0x55U }, { GPG0 + 8, 0x11111111U },          /* pin 7: input, pull 1 */
    { GPG2, 0x00110000U }, { GPG2 + 4, 0x10U }, { GPG2 + 8, 0x00000000U },          /* torch (4), front reset (5) */
    { GPC1, 0x01100000U }, { GPC1 + 4, 0x00U }, { GPC1 + 8, 0x00000000U },          /* pin 6 output low */
    { GPP4, 0x00220000U }, { GPP4 + 4, 0x00U }, { GPP4 + 8, 0x33000000U },          /* the amps' 4/5; 6/7 inputs */
    { USIM, 0x0U },
    { CMUP + 0x2050, 0x00200000U }, { CMUP + 0x2090, 0x00100000U }, { CMUP + 0x20DC, 0x00100000U },
    { CMUP + 0x20E0, 0x00100000U }, { CMUP + 0x3034, 0x0U },
    { CMUT + 0x1020, 0x1U }, { CMUT + 0x181C, 0x3U }, { CMUT + 0x2020, 0x00100000U }, { CMUT + 0x300C, 0x0U },
    { HSB + 0x60, 0x0U }, { HSB + 0x64, 0x0U }, { HSB + 0x68, 0x00210000U },
};
#define NINIT (sizeof init_regs / sizeof init_regs[0])

static struct {
    struct kv r[NREG];
    unsigned nr;
    uint64_t now;
    uint8_t *ram;
    uint8_t pmic[3], pmic0[3];             /* 0x70, 0x71, 0x72 */
    uint8_t sreg[65536];
    uint16_t sptr;
    int powered, streaming, isp_on, link_go, dma_off, frame_done, hs_inited;
    uint64_t t_power, t_mclk, t_vddd, t_i2c, t_stream, max_stream, max_power;
    unsigned violations, pmic_writes, early, accesses, isp_ups, fll_ok;
    char ev[128][8];
    uint64_t evt[128];
    unsigned nev;
    uint8_t led[7];
    unsigned led_reads;
    /* the bit-banged bus */
    int scl, sda, sl_sda, st, rw, addr_phase, master_ack;
    unsigned bits, sh, nbyte;
    uint8_t txb, cur[16];
    unsigned ncur;
    uint8_t tr[512][16];
    unsigned trn[512], ntr;
    /* faults */
    int f_hs_dead, f_csis3_secure, f_noframe, f_nodata, f_led_changes, f_nosensor, f_secure_gpio, f_isp_fail, f_noram;
    int f_pmic_fail_reg;
} F;

static uint32_t *reg(uint64_t pa)
{
    for (unsigned i = 0; i < F.nr; ++i)
        if (F.r[i].pa == pa)
            return &F.r[i].v;
    if (F.nr < NREG) {
        F.r[F.nr].pa = pa, F.r[F.nr].v = 0;
        return &F.r[F.nr++].v;
    }
    return 0;
}
static uint32_t rv(uint64_t pa) { uint32_t *p = reg(pa); return p ? *p : 0; }

static void ev(const char *e)
{
    if (F.nev < 128) {
        snprintf(F.ev[F.nev], sizeof F.ev[0], "%s", e);
        F.evt[F.nev++] = F.now;
    }
    if (verbose)
        printf("  [%llu us] %s\n", (unsigned long long)F.now, e);
}

static unsigned nib(uint64_t bank, unsigned pin) { return (rv(bank) >> (4 * pin)) & 0xFU; }
static unsigned bit(uint64_t bank, unsigned pin) { return (rv(bank + 4) >> pin) & 1U; }
static int en_on(void) { return nib(GPG0, 7) == 1 && bit(GPG0, 7); }
static int rst_on(void) { return nib(GPG2, 3) == 1 && bit(GPG2, 3); }
static int mclk_on(void) { return nib(GPC1, 6) == 2 && (rv(CMUT + 0x2020) & 0x00300000U) == 0x00300000U && !(rv(CMUT + 0x1020) & 1U); }
static int i2c_on(void) { return (rv(USIM) & 7U) == 4U; }
static int ldo_ok(unsigned i, uint8_t code) { return (F.pmic[i] & 0xC0U) == 0xC0U && (F.pmic[i] & 0x3FU) == code; }
static int supplies(void) { return ldo_ok(0, 0x1C) && ldo_ok(1, 0x28) && ldo_ok(2, 0x2C); }   /* 0x70 0x71 0x72 */

static int st_en, st_rst, st_mclk, st_i2c;
static void stream_set(int on)
{
    if (on && !F.streaming)
        F.streaming = 1, F.t_stream = F.now;
    else if (!on && F.streaming) {
        F.streaming = 0;
        if (F.now - F.t_stream > F.max_stream)
            F.max_stream = F.now - F.t_stream;
    }
}
static void derive(void)
{
    int e = en_on(), r = rst_on(), m = mclk_on(), i = i2c_on();
    if (e != st_en)
        ev(e ? "EN1" : "EN0"), st_en = e;
    if (r != st_rst)
        ev(r ? "RST1" : "RST0"), st_rst = r;
    if (m != st_mclk) {
        ev(m ? "MCLK1" : "MCLK0"), st_mclk = m;
        if (m)
            F.t_mclk = F.now;
    }
    if (i != st_i2c) {
        ev(i ? "I2C1" : "I2C0"), st_i2c = i;
        if (i)
            F.t_i2c = F.now;
    }
    int p = e && r && m && supplies() && !F.f_nosensor;
    if (p && !F.powered)
        F.powered = 1, F.t_power = F.now;
    else if (!p && F.powered) {
        F.powered = 0;
        if (F.now - F.t_power > F.max_power)
            F.max_power = F.now - F.t_power;
        stream_set(0);
        memset(F.sreg, 0, sizeof F.sreg);
    }
}

/* ---- the sensor ---------------------------------------------------------------------------------------------------- */
static int sensor_ready(void)
{
    if (!F.powered)
        return 0;
    F.accesses++;
    if (F.now - F.t_mclk < 2000U)
        F.early++;
    return 1;
}
static void s_write(uint16_t a, uint8_t v)
{
    F.sreg[a] = v;
    if (a == 0x0341 && v == 0xD7 && F.sreg[0x0340] == 0x13)
        F.fll_ok++;                         /* the setfile's and sensor_5f1_s_duration's frame length 0x13D7 */
    if (a == 0x0100)
        stream_set(v & 1U);
}
static uint8_t s_read(uint16_t a)
{
    switch (a) {
    case 0x0000: return 0x05;
    case 0x0001: return 0xF1;
    case 0x0002: return 0xA1;
    case 0x0A12: return F.sreg[0x0A00] == 1 && F.streaming ? 0x01 : 0xFF;
    default: break;
    }
    if (a >= 0x0A04 && a <= 0x0A09 && F.sreg[0x0A00] == 1 && F.streaming) {
        static const uint8_t o[6] = { 1, 1, 1, 0x18, 0x03, 0x01 };
        return o[a - 0x0A04];
    }
    return F.sreg[a];
}

/* ---- the bit-banged bus: an I2C slave at 0x10 on gpp4-6 (SCL) / gpp4-7 (SDA) ---------------------------------------- */
static int master_rel(unsigned pin) { return !(nib(GPP4, pin) == 1 && !(rv(GPP4 + 4) >> pin & 1U)); }
static void tr_flush(void)
{
    if (F.ncur && F.ntr < 512) {
        memcpy(F.tr[F.ntr], F.cur, F.ncur);
        F.trn[F.ntr++] = F.ncur;
    }
    F.ncur = 0;
}
static void push(uint8_t b) { if (F.ncur < 16) F.cur[F.ncur++] = b; }
static void bb_rise(void)
{
    if (F.st == 1)
        F.sh = (F.sh << 1) | (unsigned)F.sda, F.bits++;
    else if (F.st == 4)
        F.master_ack = !F.sda;
}
static void load_tx(void)
{
    F.txb = s_read(F.sptr++);
    push(F.txb);
    F.st = 3, F.bits = 0, F.sl_sda = (F.txb >> 7) & 1;
}
static void bb_fall(void)
{
    if (F.st == 1 && F.bits == 8) {
        uint8_t b = (uint8_t)F.sh;
        push(b);
        F.bits = 0, F.sh = 0;
        if (F.addr_phase) {
            F.addr_phase = 0;
            if ((b >> 1) == 0x10 && sensor_ready())
                F.rw = b & 1, F.st = 2, F.sl_sda = 0, F.nbyte = 0;
            else
                F.st = 5;
        } else {
            if (F.nbyte == 0)
                F.sptr = (uint16_t)(b << 8);
            else if (F.nbyte == 1)
                F.sptr = (uint16_t)(F.sptr | b);
            else
                s_write(F.sptr++, b);
            F.nbyte++;
            F.st = 2, F.sl_sda = 0;
        }
    } else if (F.st == 2) {
        F.sl_sda = 1;
        if (F.rw)
            load_tx();
        else
            F.st = 1;
    } else if (F.st == 3) {
        F.bits++;
        if (F.bits == 8)
            F.st = 4, F.sl_sda = 1;
        else
            F.sl_sda = (F.txb >> (7 - F.bits)) & 1;
    } else if (F.st == 4) {
        if (F.master_ack)
            load_tx();
        else
            F.st = 5, F.sl_sda = 1;
    }
}
static void bb_eval(void)
{
    int ms = master_rel(6), md = master_rel(7);
    if (ms != F.scl) {
        F.scl = ms;
        if (ms)
            bb_rise();
        else
            bb_fall();
    }
    int d = md && F.sl_sda;
    if (d != F.sda) {
        F.sda = d;
        if (F.scl && md == d) {             /* the master moved SDA with SCL high */
            if (!d) {                       /* START (or a repeated one) */
                tr_flush();
                F.st = 1, F.bits = 0, F.sh = 0, F.addr_phase = 1, F.sl_sda = 1;
            } else {                        /* STOP */
                tr_flush();
                F.st = 0, F.sl_sda = 1;
            }
        }
    }
}

/* ---- the HAL ------------------------------------------------------------------------------------------------------- */
static int allowed_wr(uint64_t pa)
{
    static const uint64_t ok[] = { GPG0, GPG0 + 4, GPG0 + 8, GPG2, GPG2 + 4, GPG2 + 8, GPC1, GPC1 + 4, GPC1 + 8, GPP4,
                                   GPP4 + 4, GPP4 + 8, USIM, CMUP + 0x2050, CMUP + 0x2090, CMUP + 0x20DC, CMUP + 0x20E0,
                                   CMUP + 0x3034, CMUT + 0x1020, CMUT + 0x181C, CMUT + 0x2020, CMUT + 0x300C };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; ++i)
        if (ok[i] == pa)
            return 1;
    return 0;
}
static int h_rd32(void *c, uint64_t pa, uint32_t *v)
{
    (void)c;
    F.now += 1;
    if (pa >= 0x16000000ULL && pa < 0x17000000ULL) {        /* the ISPPRE side */
        if (!F.isp_on || F.f_csis3_secure)
            return 0;
        *v = pa == 0x16260000ULL ? 0x05010000U : 0x1U;
        return 1;
    }
    if (pa == GPP4 + 4) {
        uint32_t d = rv(pa) & ~0xC0U;
        d |= (uint32_t)(master_rel(6) ? 1U : 0U) << 6;
        d |= (uint32_t)(master_rel(7) && F.sl_sda ? 1U : 0U) << 7;
        *v = d;
        return 1;
    }
    *v = rv(pa);
    return 1;
}
static void h_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    F.now += 1;
    if (!allowed_wr(pa)) {
        F.violations++;
        printf("  violation: write 0x%x to 0x%llx\n", v, (unsigned long long)pa);
        return;
    }
    if (F.f_secure_gpio) {                  /* iris_en's and the bus pins' bits ignore writes */
        uint32_t keep = 0;
        if (pa == GPG0)
            keep = 0xF0000000U;
        else if (pa == GPG0 + 4)
            keep = 0x80U;
        else if (pa == GPP4 || pa == GPP4 + 8)
            keep = 0xFF000000U;
        else if (pa == GPP4 + 4)
            keep = 0xC0U;
        v = (v & ~keep) | (rv(pa) & keep);
    }
    *reg(pa) = v;
    derive();
    if (pa == GPP4 || pa == GPP4 + 4)
        bb_eval();
}
static int h_pmic_rd(void *c, uint8_t r, uint8_t *v)
{
    (void)c;
    if (r < 0x70 || r > 0x72) {
        F.violations++;
        return 0;
    }
    *v = F.pmic[r - 0x70];
    return 1;
}
static int h_pmic_wr(void *c, uint8_t r, uint8_t v)
{
    (void)c;
    F.now += 50;
    if (r < 0x70 || r > 0x72) {
        F.violations++;
        printf("  violation: S2MPS18 0x%x\n", r);
        return 0;
    }
    static const uint8_t want[3] = { 0x1C, 0x28, 0x2C };
    if ((v & 0xC0U) && (v & 0x3FU) != want[r - 0x70]) {
        F.violations++;
        printf("  violation: S2MPS18 0x%x on at code 0x%x\n", r, v & 0x3FU);
    }
    if (r == F.f_pmic_fail_reg)
        return 0;
    F.pmic_writes++;
    static const char *const nm[3] = { "VDDD", "VDDA", "VDDIO" };
    int was = (F.pmic[r - 0x70] & 0xC0U) != 0, is = (v & 0xC0U) != 0;
    F.pmic[r - 0x70] = v;
    if (was != is) {
        char e[8];
        snprintf(e, sizeof e, "%s%d", nm[r - 0x70], is);
        ev(e);
        if (is && r == 0x70)
            F.t_vddd = F.now;
    }
    derive();
    return 1;
}
static void h_hs_init(void *c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c, (void)a, (void)b, (void)d;
    F.hs_inited = 1;
}
static int hs_ready(void)
{
    if (F.f_hs_dead || !F.hs_inited || (rv(USIM) & 7U) != 4U || (rv(GPP4) & 0xFF000000U) != 0x33000000U)
        return 0;
    for (unsigned i = 0; i < 4; ++i) {
        static const uint64_t g[4] = { CMUP + 0x2050, CMUP + 0x2090, CMUP + 0x20DC, CMUP + 0x20E0 };
        if ((rv(g[i]) & 0x00300000U) != 0x00300000U)
            return 0;
    }
    return 1;
}
static int h_hs_msg(void *c, unsigned addr, int rx, uint8_t *b, unsigned n, int stop)
{
    (void)c, (void)stop;
    F.now += 20U * n + 30U;
    if (!hs_ready() || addr != 0x10 || !sensor_ready())
        return 0;
    if (rx) {
        for (unsigned i = 0; i < n; ++i)
            b[i] = s_read(F.sptr++);
        return 1;
    }
    if (n < 2)
        return 0;
    F.sptr = (uint16_t)(b[0] << 8 | b[1]);
    for (unsigned i = 2; i < n; ++i)
        s_write(F.sptr++, b[i]);
    return 1;
}
static int h_led_rd(void *c, uint8_t r[7])
{
    (void)c;
    F.led_reads++;
    for (unsigned i = 0; i < 7; ++i)
        r[i] = F.led[i];
    if (F.f_led_changes && F.led_reads > 1)
        r[1] ^= 0x10;
    return 1;
}
static void h_delay(void *c, uint32_t us) { (void)c; F.now += us; }
static uint64_t h_now(void *c) { (void)c; return ++F.now; }
static char lastline[512];
static void h_log(void *c, const char *s)
{
    (void)c;
    snprintf(lastline, sizeof lastline, "%s", s);
    if (verbose)
        printf("  %s\n", s);
}
static void *h_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (F.f_noram || pa != MEM_PA || len > 0x2000000ULL)
        return 0;
    return F.ram;
}
static int h_isp_up(void *c, const struct cam_port *p)
{
    (void)c;
    F.isp_ups++;
    if (p->csis != 0x16260000ULL || p->wdma != 0x162E0000ULL || p->dphy != 0x16190000ULL || p->dphy_reset != 8U ||
        p->settle != 10U || p->skew != 2U || p->dt != 0x2AU || p->w != 2400U || p->h != 2400U) {
        F.violations++;
        printf("  violation: the CSIS3 port's values\n");
    }
    if (F.f_isp_fail)
        return 0;
    F.isp_on = 1;
    ev("ISP1");
    return 1;
}
static int h_link_up(void *c, uint32_t pa)
{
    (void)c;
    if (!F.isp_on || pa != (uint32_t)MEM_PA)
        F.violations++;
    ev("LINK");
    return 1;
}
static void h_link_go(void *c)
{
    (void)c;
    F.link_go = 1, F.dma_off = 0;
}
static uint32_t h_dma_src(void *c)
{
    (void)c;
    if (!F.isp_on || !F.link_go || F.dma_off || !F.streaming || F.frame_done || F.f_noframe)
        return 0;
    if (F.now - F.t_stream < 40000U)
        return 1U << 4;
    F.frame_done = 1;
    if (!F.f_nodata)
        for (unsigned y = 0; y < 2400; ++y)
            for (unsigned x = 0; x < 2400; ++x)
                F.ram[(size_t)y * 2400 + x] = (uint8_t)(8 + (x + y) % 32);
    return 1U << 8;
}
static void h_diag(void *c, uint32_t d[6]) { (void)c; memset(d, 0, 6 * sizeof d[0]); }
static void h_dma_off(void *c) { (void)c; F.dma_off = 1; }
static void h_isp_down(void *c)
{
    (void)c;
    if (F.isp_on)
        ev("ISP0");
    F.isp_on = 0, F.link_go = 0;
}

static void fake_reset(void)
{
    uint8_t *ram = F.ram;
    memset(&F, 0, sizeof F);
    F.ram = ram;
    memset(F.ram, 0x5A, 0x600000);
    for (unsigned i = 0; i < NINIT; ++i)
        F.r[F.nr++] = init_regs[i];
    F.pmic[0] = F.pmic0[0] = 0x1C, F.pmic[1] = F.pmic0[1] = 0x28, F.pmic[2] = F.pmic0[2] = 0x2C;
    static const uint8_t led[7] = { 0x00, 0xF0, 0x01, 0x10, 0x5F, 0x00, 0x02 };
    memcpy(F.led, led, 7);
    F.scl = F.sda = F.sl_sda = 1;
    F.f_pmic_fail_reg = -1;
    st_en = st_rst = st_mclk = st_i2c = 0;
    iris_test_fail_step = -1;
    lastline[0] = 0;
    struct iris_hal h = { 0, h_rd32, h_wr32, h_pmic_rd, h_pmic_wr, h_hs_init, h_hs_msg, h_led_rd, h_delay, h_now, h_log,
                          h_ram, h_isp_up, h_link_up, h_link_go, h_dma_src, h_diag, h_dma_off, h_isp_down };
    iris_hal_set(&h);
}

static int run(unsigned max_steps)
{
    iris_p1_begin();
    for (unsigned i = 0; i < max_steps; ++i) {
        unsigned w = 0;
        if (iris_p1_step(&w))
            return 1;
        F.now += (uint64_t)w * 1000U + 100U;
    }
    return 0;
}

/* everything as found, nothing on */
static void check_restored(const char *name)
{
    for (unsigned i = 0; i < NINIT; ++i)
        CHECK(rv(init_regs[i].pa) == init_regs[i].v, "%s: 0x%llx reads 0x%x, found 0x%x", name,
              (unsigned long long)init_regs[i].pa, rv(init_regs[i].pa), init_regs[i].v);
    CHECK(!memcmp(F.pmic, F.pmic0, 3), "%s: LDOs %02x %02x %02x, found %02x %02x %02x", name, F.pmic[0], F.pmic[1],
          F.pmic[2], F.pmic0[0], F.pmic0[1], F.pmic0[2]);
    CHECK(!F.isp_on, "%s: ISPPRE left on", name);
    CHECK(!F.powered && !F.streaming, "%s: the sensor left on", name);
    CHECK(F.violations == 0, "%s: %u violations", name, F.violations);
    CHECK(F.max_stream <= 520000U, "%s: streamed %llu us", name, (unsigned long long)F.max_stream);
    CHECK(F.max_power <= 3000000U, "%s: powered %llu us", name, (unsigned long long)F.max_power);
    CHECK(F.early == 0, "%s: %u sensor accesses less than 2 ms after MCLK", name, F.early);
}

static const char *const on_seq[] = { "EN1", "VDDIO1", "VDDA1", "VDDD1", "I2C1", "RST1", "MCLK1" };
static const char *const off_seq[] = { "MCLK0", "RST0", "I2C0", "VDDD0", "VDDA0", "VDDIO0", "EN0" };

/* the power events only (no ISP / LINK), from index *i: one whole on-sequence then off-sequence */
static int power_cycle_at(unsigned *i, const char *name)
{
    unsigned k = 0, ok = 1;
    uint64_t t_vddd = 0, t_i2c = 0;
    for (; *i < F.nev && k < 14; ++*i) {
        if (!strcmp(F.ev[*i], "ISP1") || !strcmp(F.ev[*i], "ISP0") || !strcmp(F.ev[*i], "LINK"))
            continue;
        const char *want = k < 7 ? on_seq[k] : off_seq[k - 7];
        if (strcmp(F.ev[*i], want)) {
            CHECK(0, "%s: power event %u is %s, want %s", name, k, F.ev[*i], want);
            ok = 0;
            break;
        }
        if (!strcmp(want, "VDDD1"))
            t_vddd = F.evt[*i];
        if (!strcmp(want, "I2C1")) {
            t_i2c = F.evt[*i];
            CHECK(t_i2c - t_vddd >= 1000U, "%s: I2C %llu us after VDDD (1 ms wanted)", name,
                  (unsigned long long)(t_i2c - t_vddd));
        }
        if (!strcmp(want, "RST1"))
            CHECK(F.evt[*i] - t_i2c >= 10U, "%s: reset %llu us after the I2C pins", name,
                  (unsigned long long)(F.evt[*i] - t_i2c));
        k++;
    }
    CHECK(k == 14, "%s: a power cycle has %u of 14 events", name, k);
    return ok && k == 14;
}

static void check_reverse(const char *name)
{
    /* the on-events seen, then exactly their offs in reverse */
    unsigned non = 0, i = 0;
    while (i < F.nev && non < 7 && !strcmp(F.ev[i], on_seq[non]))
        non++, i++;
    for (unsigned k = 0; k < non; ++k, ++i) {
        const char *want = off_seq[7 - non + k];
        CHECK(i < F.nev && !strcmp(F.ev[i], want), "%s: off event %u is %s, want %s", name, k, i < F.nev ? F.ev[i] : "-",
              want);
    }
    CHECK(i == F.nev, "%s: %u events after the reverse order", name, F.nev - i);
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static int has_tr(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < F.ntr; ++i)
        if (F.trn[i] == n && !memcmp(F.tr[i], b, n))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    F.ram = malloc(0x600000);
    if (!F.ram)
        return 2;

    /* ---- the pure parts ---- */
    CHECK(iris_ldo_code(&iris_ldos[0], 1800000U) == 0x2C, "LDO41 1.8 V code %d", iris_ldo_code(&iris_ldos[0], 1800000U));
    CHECK(iris_ldo_code(&iris_ldos[1], 2800000U) == 0x28, "LDO40 2.8 V code %d", iris_ldo_code(&iris_ldos[1], 2800000U));
    CHECK(iris_ldo_code(&iris_ldos[2], 1050000U) == 0x1C, "LDO39 1.05 V code %d", iris_ldo_code(&iris_ldos[2], 1050000U));
    CHECK(iris_ldo_code(&iris_ldos[2], 1000000U) == 0x18, "LDO39 1.0 V code");
    CHECK(iris_ldo_code(&iris_ldos[2], 1062500U) == -1, "LDO39 above the DT's max must be refused");
    CHECK(iris_ldo_code(&iris_ldos[0], 1825000U) == -1, "LDO41 above 1.8 V must be refused");
    CHECK(iris_ldo_code(&iris_ldos[2], 1056250U) == -1, "off the 12.5 mV grid");
    for (unsigned i = 0; i < 3; ++i) {
        int c = iris_ldo_code(&iris_ldos[i], iris_ldos[i].want_uv);
        CHECK(c >= 0 && iris_ldo_uv(&iris_ldos[i], (unsigned)c) == iris_ldos[i].want_uv &&
              iris_ldos[i].want_uv <= iris_ldos[i].dt_max_uv && iris_ldos[i].want_uv >= iris_ldos[i].dt_min_uv,
              "%s: the wanted voltage is a code inside the DT", iris_ldos[i].name);
    }
    {
        uint8_t o[8], d[2] = { 0x79, 0x71 };
        unsigned n = iris_i2c_wire(0x10, 0x6214, d, 2, o, sizeof o);
        CHECK(n == 5 && o[0] == 0x20 && o[1] == 0x62 && o[2] == 0x14 && o[3] == 0x79 && o[4] == 0x71,
              "the wire bytes of 0x6214 = 0x7971: %u %02x %02x %02x %02x %02x", n, o[0], o[1], o[2], o[3], o[4]);
        n = iris_i2c_wire(0x10, 0x0002, 0, 0, o, sizeof o);
        CHECK(n == 3 && o[0] == 0x20 && o[1] == 0x00 && o[2] == 0x02, "the wire bytes of a read of 0x0002");
        CHECK(iris_i2c_wire(0x10, 0x0100, d, 2, o, 4) == 0, "too small a buffer");
    }
    {
        static uint8_t fr[64 * 16];
        for (unsigned i = 0; i < sizeof fr; ++i)
            fr[i] = (uint8_t)(i < 64 * 8 ? IRIS_FILL : i % 64);
        struct iris_fstats s;
        iris_frame_stats(fr, 64, 16, 64, 1, &s);
        CHECK(s.n == 1024 && s.words == 128 && s.fill_words == 64, "stats: n %u words %u fill %u", s.n, s.words,
              s.fill_words);
        CHECK(s.min == 0 && s.max == IRIS_FILL, "stats: min %u max %u", s.min, s.max);
        /* 512 x 0xA5 (165) + 8 rows of 0..63 (mean 31.5) = (512 x 165 + 512 x 31.5) / 1024 = 98.25 */
        CHECK(s.mean_x10 == 982, "stats: mean x10 %u", s.mean_x10);
        CHECK(s.hist[10] == 512 && s.hist[0] == 128 && s.hist[3] == 128 && s.hist[4] == 0, "stats: histogram");
        iris_frame_stats(fr, 64, 16, 64, 2, &s);
        CHECK(s.n == 512, "stats: every 2nd row");
    }

    /* ---- a whole P1 on the HSI2C ---- */
    fake_reset();
    CHECK(run(100000), "hsi2c: P1 finished");
    {
        const struct iris_p1_result *r = iris_p1_last();
        CHECK(iris_p1_passed(), "hsi2c: P1 passed: %s", lastline);
        CHECK(starts(lastline, "drvprobe: iris: PASS CSIS3 readable, frame 2400x2400 mean 23.5"), "hsi2c: %s", lastline);
        CHECK(r->bus == 1 && r->ver == 0xA1 && r->model == 0x05F1 && r->csis3 == 1 && r->frame == 1, "hsi2c: result");
        CHECK(r->st.min == 8 && r->st.max == 39 && r->st.fill_words == 0, "hsi2c: frame stats %u %u", r->st.min,
              r->st.max);
        CHECK(r->otp[0] == 1 && r->otp[3] == 0x18 && r->page == 1, "hsi2c: the OTP read the stock way");
        CHECK(r->led_same == 1 && F.led_reads == 2, "hsi2c: the LED read before and after");
        CHECK(F.isp_ups == 1, "hsi2c: ISPPRE up once");
        CHECK(F.fll_ok == 2, "hsi2c: the setfile and the frame length written (%u)", F.fll_ok);
        unsigned i = 0;
        power_cycle_at(&i, "hsi2c P1a");
        power_cycle_at(&i, "hsi2c P1b");
        CHECK(i == F.nev, "hsi2c: events after the two cycles");
        int isp1 = -1, isp0 = -1, en2 = -1, mclk0b = -1, link = -1;
        for (unsigned k = 0, ens = 0, mcl = 0; k < F.nev; ++k) {
            if (!strcmp(F.ev[k], "ISP1"))
                isp1 = (int)k;
            if (!strcmp(F.ev[k], "ISP0"))
                isp0 = (int)k;
            if (!strcmp(F.ev[k], "LINK"))
                link = (int)k;
            if (!strcmp(F.ev[k], "EN1") && ++ens == 2)
                en2 = (int)k;
            if (!strcmp(F.ev[k], "MCLK0") && ++mcl == 2)
                mclk0b = (int)k;
        }
        CHECK(isp1 >= 0 && isp1 < en2 && link > en2 && isp0 > link && isp0 < mclk0b,
              "hsi2c: ISPPRE up before P1b's power, the link after it, ISPPRE down before the sensor (%d %d %d %d %d)",
              isp1, en2, link, isp0, mclk0b);
    }
    check_restored("hsi2c");

    /* ---- the bit-banged bus ---- */
    fake_reset();
    F.f_hs_dead = 1;
    CHECK(run(100000), "bit-bang: P1 finished");
    CHECK(iris_p1_passed() && iris_p1_last()->bus == 2, "bit-bang: passed on the bit-banged bus: %s", lastline);
    {
        static const uint8_t w[5] = { 0x20, 0x62, 0x14, 0x79, 0x71 }, a[3] = { 0x20, 0x00, 0x02 }, rd[2] = { 0x21, 0xA1 },
                             id[3] = { 0x21, 0x05, 0xF1 }, so[4] = { 0x20, 0x01, 0x00, 0x01 };
        CHECK(has_tr(w, 5), "bit-bang: 0x6214 = 0x7971 on the wire as 20 62 14 79 71");
        CHECK(has_tr(a, 3) && has_tr(rd, 2), "bit-bang: a read of 0x0002: 20 00 02, repeated start, 21 A1");
        CHECK(has_tr(id, 3), "bit-bang: a 2-byte read of 0x0000: 21 05 F1 (ACK, then NACK)");
        CHECK(has_tr(so, 4), "bit-bang: stream on 0x0100 = 1: 20 01 00 01");
        CHECK(F.fll_ok == 2 && F.ntr > 90, "bit-bang: %u messages, frame length written %u times", F.ntr, F.fll_ok);
    }
    check_restored("bit-bang");

    /* ---- CSIS3 held by the secure world ---- */
    fake_reset();
    F.f_csis3_secure = 1;
    CHECK(run(100000), "secure: finished");
    CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 FAULT: secure"), "secure: %s", lastline);
    CHECK(iris_p1_last()->csis3 == 0 && !iris_p1_passed(), "secure: result");
    {
        unsigned ens = 0;
        for (unsigned k = 0; k < F.nev; ++k)
            ens += !strcmp(F.ev[k], "EN1");
        CHECK(ens == 1, "secure: the sensor powered only in P1a (%u)", ens);
    }
    check_restored("secure");

    /* ---- no frame end / a frame end without data / the LED changing ---- */
    fake_reset();
    F.f_noframe = 1;
    CHECK(run(100000), "no frame: finished");
    CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 readable, no frame: no frame end within 0.5 s"), "no frame: %s",
          lastline);
    CHECK(F.max_stream >= 500000U && F.max_stream <= 520000U, "no frame: streamed %llu us",
          (unsigned long long)F.max_stream);
    check_restored("no frame");

    fake_reset();
    F.f_nodata = 1;
    CHECK(run(100000), "no data: finished");
    CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 readable, no frame: a frame end, but the frame did not reach"),
          "no data: %s", lastline);
    CHECK(iris_p1_last()->st.fill_words == iris_p1_last()->st.words, "no data: every word still the fill");
    check_restored("no data");

    fake_reset();
    F.f_led_changes = 1;
    CHECK(run(100000), "led: finished");
    CHECK(!iris_p1_passed() && iris_p1_last()->led_same == 0 && strstr(lastline, "LED CHANGED"), "led: %s", lastline);
    check_restored("led");

    /* ---- no sensor; the pins held by the secure world ---- */
    fake_reset();
    F.f_nosensor = 1;
    CHECK(run(100000), "no sensor: finished");
    CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 not reached, no frame: the sensor does not answer"),
          "no sensor: %s", lastline);
    CHECK(F.isp_ups == 0, "no sensor: ISPPRE never touched");
    {
        unsigned i = 0;
        power_cycle_at(&i, "no sensor");
        CHECK(i == F.nev, "no sensor: one power cycle");
    }
    check_restored("no sensor");

    fake_reset();
    F.f_secure_gpio = 1;
    CHECK(run(100000), "secure gpio: finished");
    CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 not reached") && strstr(lastline, "pins ignored writes") &&
          !strstr(lastline, " 0 pins ignored"), "secure gpio: %s", lastline);
    check_restored("secure gpio");

    /* ---- a failure at every power-on step: the reverse order of what was on ---- */
    for (int k = 0; k < 8; ++k) {
        char nm[32];
        snprintf(nm, sizeof nm, "fail at step %d", k);
        fake_reset();
        iris_test_fail_step = k;
        CHECK(run(100000), "%s: finished", nm);
        CHECK(starts(lastline, "drvprobe: iris: FAIL CSIS3 not reached"), "%s: %s", nm, lastline);
        check_reverse(nm);
        unsigned on = 0, want = k > 0 ? (unsigned)k - 1U : 0U;
        for (unsigned e = 0; e < F.nev; ++e)
            on += F.ev[e][strlen(F.ev[e]) - 1] == '1';
        /* step k fails before switching anything: steps 1..k-1 had an event each (step 0, the pins low, has none) */
        CHECK(on == want, "%s: %u on events, %u wanted", nm, on, want);
        check_restored(nm);
    }
    /* a supply's write failing half-way (VDDA): VDDIO and iris_en back off, VDDA as found */
    fake_reset();
    F.f_pmic_fail_reg = 0x71;
    CHECK(run(100000), "vdda fails: finished");
    check_reverse("vdda fails");
    CHECK(F.nev == 4 && !strcmp(F.ev[1], "VDDIO1") && !strcmp(F.ev[2], "VDDIO0"), "vdda fails: %u events", F.nev);
    check_restored("vdda fails");
    /* codes found other than the stock ones (off): written before switching on, put back after */
    fake_reset();
    F.pmic[0] = F.pmic0[0] = 0x05, F.pmic[1] = F.pmic0[1] = 0x00, F.pmic[2] = F.pmic0[2] = 0x3F;
    CHECK(run(100000) && iris_p1_passed(), "other codes: passed: %s", lastline);
    check_restored("other codes");
    /* an LDO on at another voltage: someone else's, nothing written */
    fake_reset();
    F.pmic[2] = F.pmic0[2] = 0xC0 | 0x30;
    CHECK(run(100000) && !iris_p1_passed(), "in use: refused");
    CHECK(F.pmic_writes == 0 && F.nev == 0, "in use: %u S2MPS18 writes, %u events", F.pmic_writes, F.nev);
    check_restored("in use");
    /* no memory for the frame */
    fake_reset();
    F.f_noram = 1;
    CHECK(run(100000) && !iris_p1_passed() && F.nev == 0, "no memory: refused before anything");

    /* ---- an abort at every point ---- */
    unsigned aborted = 0;
    for (unsigned n = 1; n < 400; ++n) {
        fake_reset();
        iris_p1_begin();
        int done = 0;
        for (unsigned i = 0; i < n && !done; ++i) {
            unsigned w = 0;
            done = iris_p1_step(&w);
            F.now += (uint64_t)w * 1000U + 100U;
        }
        if (done)
            break;
        iris_p1_abort();
        aborted++;
        char nm[32];
        snprintf(nm, sizeof nm, "abort after %u", n);
        check_restored(nm);
    }
    CHECK(aborted > 20, "aborts tried: %u", aborted);

    printf("iris-test: %d checks, %d failed\n", checks, fails);
    free(F.ram);
    return fails ? 1 : 0;
}
