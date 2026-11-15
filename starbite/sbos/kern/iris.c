/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the iris camera's probe P1 (iris.h; docs/drivers/IRIS.md 1, 2, 5, 6). Attended only.
 *
 * THE WIRING (this phone's device tree build/linux-bundle/lts.dts; Samsung's GPL-2.0 kernel for it: fimc-is-device-module-5f1.c,
 * fimc-is-hw-csi-v5_1.c, fimc-is-hw-settle-10nm-lpp.h, cmucal-sfr.c (exynos9810), s2mps18-private.h; read as facts)
 *   sensor    S5K5F1 at 0x10 (fimc-is-secure@10) on hsi2c@108D0000 (USI07_I2C of PERIC1), mode word usi@10811024;
 *             pins gpp4-6 SCL / gpp4-7 SDA (PERIC1 0x10830000), function 3 for the HSI2C; CMU_PERIC1 gates 0x2050
 *             (GATE_CLK_PERIC1_USI_I2C, shared with USI08_I2C), 0x2090, 0x20DC, 0x20E0, Q-channel 0x3034. 16-bit
 *             register addresses. A bit-banged fallback on the same two pins (open drain by hand: input = released,
 *             output 0 = low; never driven high), as kern/amp.c does on gpp4-4/5 where USI07's main HSI2C never ran
 *   supplies  S2MPS18 (ACPM, the PMIC block): LDO41 VDDIO 1.8 V (0x72, 0.7 V + 25 mV n: 0x2C), LDO40 VDDA 2.8 V (0x71,
 *             1.8 V + 25 mV n: 0x28), LDO39 VDDD 1.05 V (0x70, 0.7 V + 12.5 mV n: 0x1C); the DT's limits: LDO39
 *             1.000-1.050 V, LDO40 2.8 V, LDO41 1.8 V. The ranges are s2mps18-private.h's (LDO_MIN1 0.7 V, LDO_MIN3
 *             1.8 V, STEP1 12.5 mV, STEP2 25 mV); which LDO uses which is not in our sparse sources: the same codes are
 *             what libsb's LDO35 (1.8 V = 0x2C) and LDO43 (3.0 V in 0x28..0x3C) checks imply, and what an earlier port's iris
 *             driver records working on this phone. Each code is computed and checked against the DT's min / max
 *   pins      iris_en gpg0-7 (PERIC0 0x10430080), reset gpg2-3 (PERIC0 0x104300C0; gpg2-4 the torch, gpg2-5 the front
 *             camera: only bit 3 / nibble 3 is touched), MCLK gpc1-6 (PERIC1 0x10830080) function 2 while it runs
 *   MCLK      CIS_CLK2 of CMU_TOP (0x1A240000): mux 0x1020 [0] (0 = OSCCLK, 26 MHz), divider 0x181C [4:0], gate 0x2020
 *             (bit 20 manual, 21 on), Q-channel 0x300C (cmucal-sfr.c); as cam.c does CIS_CLK1 for the front camera
 *   receiver  CSIS3 0x16260000, its D-PHY 0x16190000 (dphy_m0s4s2_csis3: PMU isolation 0x730 shared with CSIS1, reset
 *             bit 3 of SYSREG_ISPPRE 0x500), VC0's WDMA 0x162E0000 (DT reg: 0x162E0000 / +0x100 / +0x200 / +0x300 are
 *             VC0..VC3, the common block +0x400); the ISPPRE domain, its clocks and the D-PHY / CSIS / WDMA set-up are
 *             cam.c's (cam_port_*): 2 lanes, VC0 RAW8 (0x2A) 2400 x 2400, packet storage, OTF format 3 and input path
 *             PRL (csi_hw_s_config_dma_cmn's RAW8 case); 2041 Mb/s: settle 10 (the table's 2040 / 2050 rows),
 *             SKEWCAL_EN 1, SKEW_DLYSEL 2 (2-3 Gb/s), HS_MODE_SEL 0 (csi_hw_s_phy_config)
 *   the mode  setfile_vision_5f1_fps_30_26MHz (52 writes), then frame length 0x0340 = 0x13D7 (sensor_5f1_s_duration);
 *             init: 0xFCFC = 0x4000, 0x6010 = 1, 5 ms, 0xFCFC = 0x4000, 0x6214 = 0x7971, 0x6218 = 0x7150; OTP:
 *             stream on, 4 ms, 0x0A02 = 0x3E00, 0x0A00 = 0x0100, 350 us, page info 0x0A12 (1 / 0: page 0x3E00, 3:
 *             0x3F00), the page, 0x0A00 = 0x0100, 350 us, 0x0A04..0x0A09, 0x0002 the version, 0x0A00 = 0, stream off;
 *             stream off: 0x0100 = 0, 40 ms (sensor_5f1_init, sensor_5f1_stream_off)
 *   power     on: iris_en high; VDDIO; VDDA; VDDD 1.05 V, 1 ms; the I2C pins, 10 us; reset high; MCLK, 2 ms. Off: MCLK
 *             (pin output low, 200 us), reset low, the I2C pins, VDDD, VDDA, VDDIO, iris_en low
 *             (sensor_5f1_power_setpin); then the three pins and CIS_CLK2 as found
 *   the LED   the S2MPB02's IR channel is NEVER written: there is no write path to it here. Its seven registers are
 *             read before and after P1 and must read the same.
 *
 * Every register write goes through wr(), which refuses (and logs) any address outside the blocks above; the S2MPS18
 * through ldo_put(), which refuses any register but 0x70-0x72 and any value but the computed code (off / on) or the one
 * found. Nothing touches EFS / CPEFS / PARAM or the card. */
#include "iris.h"
#include <stdarg.h>
#ifdef SBOS_KERNEL
#include "sb.h"
#endif

/* ---- addresses ---------------------------------------------------------------------------------------------------- */
#define GPIO_PERIC0  0x10430000ULL
#define GPIO_PERIC1  0x10830000ULL
#define GPG0         (GPIO_PERIC0 + 0x80)   /* iris_en: pin 7 */
#define GPG2         (GPIO_PERIC0 + 0xC0)   /* reset: pin 3 */
#define GPC1         (GPIO_PERIC1 + 0x80)   /* MCLK2: pin 6 */
#define GPP4         (GPIO_PERIC1 + 0x00)   /* the sensor's bus: SCL pin 6, SDA pin 7 */
#define EN_PIN       7U
#define RESET_PIN    3U
#define MCLK_PIN     6U
#define SCL          6U
#define SDA          7U
#define BUS_MASK     0xFF000000U            /* gpp4 CON / PUD: pins 6 and 7 */
#define BUS_FN       0x33000000U            /* function 3: hsi2c (USI07_I2C) */
#define USI_MODE     0x10811024ULL          /* usi@10811024: USI07_I2C's mode word */
#define USI_I2C      4U
#define HS_BASE      0x108D0000ULL
#define CMU_PERIC1   0x10800000ULL
#define QCH_BUS      (CMU_PERIC1 + 0x3034)
#define CMU_TOP      0x1A240000ULL
#define CIS2_MUX     (CMU_TOP + 0x1020)
#define CIS2_DIV     (CMU_TOP + 0x181C)
#define CIS2_GATE    (CMU_TOP + 0x2020)
#define CIS2_QCH     (CMU_TOP + 0x300C)
#define GATE_ON      0x00300000U            /* bit 20 manual, bit 21 on */
#define CSIS3        0x16260000ULL
#define WDMA3        0x162E0000ULL
#define DPHY3        0x16190000ULL
#define DMA_FS       (1U << 4)
#define DMA_FE       (1U << 8)
#define DMA_ERR      (1U << 12)
#define BB_HALF_US   5U

static const uint64_t bus_gates[4] = { CMU_PERIC1 + 0x2050, CMU_PERIC1 + 0x2090, CMU_PERIC1 + 0x20DC,
                                       CMU_PERIC1 + 0x20E0 };

const struct cam_port iris_port = { "CSIS3", CSIS3, WDMA3, DPHY3, 1U << 3, 10U, 2U, 0x2AU, IRIS_W, IRIS_H };

/* S2MPS18 LDO39-41, in the power-on order */
const struct iris_ldo_spec iris_ldos[3] = {
    { "VDDIO_1.8V_IRIS (LDO41)", 0x72, 700000U, 25000U, 1800000U, 1800000U, 1800000U },
    { "VDDA_2.8V_IRIS (LDO40)", 0x71, 1800000U, 25000U, 2800000U, 2800000U, 2800000U },
    { "VDDD_1.05V_IRIS (LDO39)", 0x70, 700000U, 12500U, 1000000U, 1050000U, 1050000U },
};

/* setfile_vision_5f1_fps_30_26MHz (fimc-is-device-module-5f1.c, S5K5F1S_EVT0_Ver_0.5_20170710): the sensor's own
 * register values, every one a 16-bit write, in Samsung's order. 2400 x 2400 (0x034C / 0x034E = 0x0960), the CCS data
 * format word 0x0112 = 0x0A08 (10-bit pixels sent as 8: RAW8, data type 0x2A, on the wire), line length 0x0A38, frame
 * length 0x13D7, exposure 0x0339 lines */
static const uint16_t setfile[][2] = {
    { 0xF43E, 0x24CE }, { 0xF440, 0x402F }, { 0x354C, 0x0004 }, { 0x3544, 0x110B }, { 0x3540, 0x110B },
    { 0x3082, 0x0100 }, { 0x3168, 0x00A0 }, { 0x31A6, 0x0100 }, { 0xF470, 0x0000 }, { 0xF43A, 0x0010 },
    { 0x3572, 0x0012 }, { 0xF420, 0x0013 }, { 0xF422, 0x0000 }, { 0xF424, 0x000B }, { 0xF426, 0x000E },
    { 0xF488, 0x0008 }, { 0x3534, 0x0708 }, { 0x320E, 0x0000 }, { 0x3304, 0x0094 }, { 0xF4BA, 0x0008 },
    { 0x319E, 0x0001 }, { 0x3078, 0x0340 }, { 0x31C6, 0x0000 }, { 0x6028, 0x2000 }, { 0x602A, 0x0E1E },
    { 0x6F12, 0x8110 }, { 0x0306, 0x008A }, { 0x030E, 0x009D }, { 0x3560, 0x0050 }, { 0x0342, 0x0A38 },
    { 0x0340, 0x13D7 }, { 0x0202, 0x0339 }, { 0xB134, 0x0180 }, { 0x0344, 0x0008 }, { 0x0346, 0x0008 },
    { 0x0348, 0x0967 }, { 0x034A, 0x0967 }, { 0x034C, 0x0960 }, { 0x034E, 0x0960 }, { 0x0112, 0x0A08 },
    { 0xF1A2, 0x0200 }, { 0xF1A8, 0x09B0 }, { 0xF1AA, 0x09B0 }, { 0x30BE, 0x0100 }, { 0x6028, 0x2000 },
    { 0x602A, 0x0F10 }, { 0x6F12, 0x0001 }, { 0x602A, 0x0F32 }, { 0x6F12, 0x0001 }, { 0x602A, 0x0F1E },
    { 0x6F12, 0x0487 }, { 0x6F12, 0x0725 },
};
#define SETFILE_N (sizeof(setfile) / sizeof(setfile[0]))

/* ---- the pure parts ----------------------------------------------------------------------------------------------- */
int iris_ldo_code(const struct iris_ldo_spec *l, uint32_t uv)
{
    if (uv < l->dt_min_uv || uv > l->dt_max_uv || uv < l->base_uv || !l->step_uv)
        return -1;
    uint32_t d = uv - l->base_uv;
    if (d % l->step_uv || d / l->step_uv > 0x3FU)
        return -1;
    return (int)(d / l->step_uv);
}

uint32_t iris_ldo_uv(const struct iris_ldo_spec *l, unsigned code)
{
    return l->base_uv + (code & 0x3FU) * l->step_uv;
}

unsigned iris_i2c_wire(uint8_t addr7, uint16_t reg, const uint8_t *data, unsigned n, uint8_t *out, unsigned cap)
{
    if (cap < 3U + n)
        return 0;
    out[0] = (uint8_t)(addr7 << 1);
    out[1] = (uint8_t)(reg >> 8);
    out[2] = (uint8_t)reg;
    for (unsigned i = 0; i < n; ++i)
        out[3 + i] = data[i];
    return 3U + n;
}

void iris_frame_stats(const volatile uint8_t *p, unsigned w, unsigned h, unsigned stride, unsigned row_step,
                      struct iris_fstats *s)
{
    uint64_t sum = 0;
    s->min = 255, s->max = 0, s->n = 0, s->fill_words = 0, s->words = 0, s->mean_x10 = 0;
    for (unsigned i = 0; i < 16; ++i)
        s->hist[i] = 0;
    if (!row_step)
        row_step = 1;
    for (unsigned y = 0; y < h; y += row_step) {
        const volatile uint8_t *r = p + (uint64_t)y * stride;
        unsigned fill = 0;
        for (unsigned x = 0; x < w; ++x) {
            unsigned v = r[x];
            sum += v;
            s->hist[v >> 4]++;
            if (v < s->min)
                s->min = v;
            if (v > s->max)
                s->max = v;
            fill += v == IRIS_FILL;
            if ((x & 7U) == 7U) {
                s->words++;
                s->fill_words += fill == 8U;
                fill = 0;
            }
        }
        s->n += w;
    }
    if (s->n)
        s->mean_x10 = (uint32_t)(sum * 10U / s->n);
    else
        s->min = 0;
}

/* ---- the HAL and the log ------------------------------------------------------------------------------------------ */
static struct iris_hal H;
static int hal_ok;

static void fmt_u(char *b, unsigned *n, unsigned cap, unsigned long long v, unsigned base, unsigned width, char pad)
{
    char t[24];
    unsigned k = 0;
    do {
        unsigned d = (unsigned)(v % base);
        t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v && k < sizeof(t));
    while (width > k && *n + 1 < cap) {
        b[(*n)++] = pad;
        width--;
    }
    while (k && *n + 1 < cap)
        b[(*n)++] = t[--k];
}

/* %s %c %d %u %x with an optional 0 and width, and l / ll */
static void vfmt(char *b, unsigned cap, const char *f, va_list ap)
{
    unsigned n = 0;
    for (; *f && n + 1 < cap; ++f) {
        if (*f != '%') {
            b[n++] = *f;
            continue;
        }
        ++f;
        char pad = ' ';
        unsigned width = 0, l = 0;
        if (*f == '0')
            pad = '0', ++f;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        while (*f == 'l')
            l++, f++;
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(none)";
            while (*s && n + 1 < cap)
                b[n++] = *s++;
            break;
        }
        case 'c':
            b[n++] = (char)va_arg(ap, int);
            break;
        case 'd': {
            long long v = l >= 2 ? va_arg(ap, long long) : l ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0 && n + 1 < cap)
                b[n++] = '-', v = -v;
            fmt_u(b, &n, cap, (unsigned long long)v, 10, width, pad);
            break;
        }
        case 'u':
        case 'x': {
            unsigned long long v = l >= 2 ? va_arg(ap, unsigned long long) : l ? va_arg(ap, unsigned long)
                                                                                : va_arg(ap, unsigned);
            fmt_u(b, &n, cap, v, *f == 'x' ? 16 : 10, width, pad);
            break;
        }
        case '%':
            b[n++] = '%';
            break;
        default:
            return;
        }
        if (!*f)
            break;
    }
    b[n] = 0;
}

static void say(const char *f, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *f, ...)
{
    char line[400] = "drvprobe: iris: ";
    va_list ap;
    va_start(ap, f);
    vfmt(line + 16, sizeof(line) - 16, f, ap);
    va_end(ap);
    if (hal_ok)
        H.log(H.ctx, line);
}

static uint64_t now_us(void)
{
    return H.now_us(H.ctx);
}

static void spin(uint32_t us)
{
    H.delay_us(H.ctx, us);
}

/* ---- registers ---------------------------------------------------------------------------------------------------- */
static unsigned faults, refused;

static uint32_t rd(uint64_t pa)
{
    uint32_t v = 0;
    if (!H.rd32(H.ctx, pa, &v)) {
        if (faults++ < 8)
            say("a read of 0x%llx faulted", (unsigned long long)pa);
        return 0;
    }
    return v;
}

static int may_write(uint64_t pa)
{
    static const uint64_t one[] = { USI_MODE, QCH_BUS, CIS2_MUX, CIS2_DIV, CIS2_GATE, CIS2_QCH };
    static const uint64_t bank[] = { GPG0, GPG2, GPC1, GPP4 };
    for (unsigned i = 0; i < sizeof(one) / sizeof(one[0]); ++i)
        if (pa == one[i])
            return 1;
    for (unsigned i = 0; i < 4; ++i)
        if (pa == bus_gates[i])
            return 1;
    for (unsigned i = 0; i < sizeof(bank) / sizeof(bank[0]); ++i)
        if (pa == bank[i] || pa == bank[i] + 4 || pa == bank[i] + 8)
            return 1;
    return 0;
}

static void wr(uint64_t pa, uint32_t v)
{
    if (!may_write(pa)) {
        if (refused++ < 8)
            say("BUG: a write of 0x%x to 0x%llx refused (not an iris register)", v, (unsigned long long)pa);
        return;
    }
    H.wr32(H.ctx, pa, v);
}

static void upd(uint64_t pa, uint32_t mask, uint32_t val)
{
    uint32_t o = rd(pa), n = (o & ~mask) | (val & mask);
    if (n != o)
        wr(pa, n);
}

/* ---- pins --------------------------------------------------------------------------------------------------------- */
struct pin {
    const char *name;
    uint64_t bank;
    unsigned n;
    uint32_t con, dat, pud;
    int saved;
};
static struct pin en_pin = { "iris_en gpg0-7", GPG0, EN_PIN, 0, 0, 0, 0 };
static struct pin rst_pin = { "reset gpg2-3", GPG2, RESET_PIN, 0, 0, 0, 0 };
static struct pin mclk_pin = { "MCLK gpc1-6", GPC1, MCLK_PIN, 0, 0, 0, 0 };
static unsigned pins_ignored;               /* a pin that did not take a write (the secure world's?) */

static void pin_save(struct pin *p)
{
    p->con = (rd(p->bank) >> (4 * p->n)) & 0xFU;
    p->dat = (rd(p->bank + 4) >> p->n) & 1U;
    p->pud = (rd(p->bank + 8) >> (4 * p->n)) & 0xFU;
    p->saved = 1;
}

static void pin_fn(struct pin *p, unsigned fn)
{
    upd(p->bank, 0xFU << (4 * p->n), fn << (4 * p->n));
}

/* an output at a level (the level first, then the direction); read back: a pin that ignores writes is logged */
static void pin_out(struct pin *p, int high)
{
    upd(p->bank + 4, 1U << p->n, high ? 1U << p->n : 0);
    pin_fn(p, 1);
    uint32_t c = (rd(p->bank) >> (4 * p->n)) & 0xFU, d = (rd(p->bank + 4) >> p->n) & 1U;
    if (c != 1U || d != (unsigned)(high ? 1 : 0)) {
        if (pins_ignored++ < 6)
            say("%s did not take output %u: reads function %u level %u (held by the secure world?)", p->name,
                high ? 1U : 0U, c, d);
    }
}

static void pin_restore(struct pin *p)
{
    if (!p->saved)
        return;
    upd(p->bank + 4, 1U << p->n, p->dat << p->n);
    upd(p->bank, 0xFU << (4 * p->n), p->con << (4 * p->n));
    upd(p->bank + 8, 0xFU << (4 * p->n), p->pud << (4 * p->n));
    p->saved = 0;
}

/* ---- CIS_CLK2: 26 MHz from OSCCLK ---------------------------------------------------------------------------------- */
static struct {
    uint32_t mux, div, gate, qch;
    int mine;
} K;

static void clk_on(void)
{
    K.mux = rd(CIS2_MUX), K.div = rd(CIS2_DIV), K.gate = rd(CIS2_GATE), K.qch = rd(CIS2_QCH);
    K.mine = 1;
    if (K.mux & 1U)
        wr(CIS2_MUX, K.mux & ~1U);
    if (K.div & 0x1FU)
        wr(CIS2_DIV, K.div & ~0x1FU);
    upd(CIS2_QCH, 3U, 3U);
    upd(CIS2_GATE, GATE_ON, GATE_ON);
    say("CIS_CLK2 26 MHz: mux 0x%x -> 0x%x, divider 0x%x -> 0x%x, gate 0x%x -> 0x%x", K.mux, rd(CIS2_MUX), K.div,
        rd(CIS2_DIV), K.gate, rd(CIS2_GATE));
}

static void clk_off(void)
{
    if (!K.mine)
        return;
    wr(CIS2_GATE, K.gate);
    wr(CIS2_QCH, K.qch);
    wr(CIS2_DIV, K.div);
    wr(CIS2_MUX, K.mux);
    K.mine = 0;
}

/* ---- the LDOs ------------------------------------------------------------------------------------------------------ */
static struct {
    uint8_t found[3];
    int code[3], mine[3], bad;
} L;

static int ldo_allowed(unsigned i, uint8_t v)
{
    return L.code[i] >= 0 &&
           (v == (uint8_t)L.code[i] || v == (uint8_t)(IRIS_LDO_ON | L.code[i]) || (v == L.found[i] && !(v & 0xC0U)));
}

/* the only way anything reaches the S2MPS18: LDO39-41, an allowed value, read back */
static int ldo_put(unsigned i, uint8_t v)
{
    const struct iris_ldo_spec *l = &iris_ldos[i];
    if (!ldo_allowed(i, v)) {
        say("BUG: S2MPS18 0x%x (%s) = 0x%x refused: not a value this probe writes", l->reg, l->name, v);
        L.bad = 1;
        return 0;
    }
    uint8_t back = 0;
    int ok = H.pmic_wr(H.ctx, l->reg, v) && H.pmic_rd(H.ctx, l->reg, &back) && back == v;
    if (!ok) {
        say("S2MPS18 0x%x (%s): wrote 0x%x, read back 0x%x", l->reg, l->name, v, back);
        L.bad = 1;
    }
    return ok;
}

/* read all three first; one on at another voltage is someone else's: nothing written */
static int ldo_look(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        const struct iris_ldo_spec *l = &iris_ldos[i];
        L.code[i] = iris_ldo_code(l, l->want_uv);
        L.mine[i] = 0;
        if (!H.pmic_rd(H.ctx, l->reg, &L.found[i])) {
            say("%s (S2MPS18 0x%x) cannot be read through ACPM", l->name, l->reg);
            return 0;
        }
        say("%s found 0x%02x (%s, code 0x%02x = %u uV); writes code 0x%02x = %u uV (DT %u..%u uV)", l->name,
            L.found[i], L.found[i] & 0xC0U ? "on" : "off", L.found[i] & 0x3FU, iris_ldo_uv(l, L.found[i]),
            L.code[i] & 0x3F, L.code[i] >= 0 ? iris_ldo_uv(l, (unsigned)L.code[i]) : 0U, l->dt_min_uv, l->dt_max_uv);
        if (L.code[i] < 0) {
            say("BUG: %s: %u uV is not a code inside the DT's range", l->name, l->want_uv);
            return 0;
        }
        if ((L.found[i] & 0xC0U) && (L.found[i] & 0x3FU) != (unsigned)L.code[i]) {
            say("%s is on at another voltage (0x%02x): someone else's, nothing written", l->name, L.found[i]);
            return 0;
        }
    }
    return 1;
}

static int ldo_on(unsigned i)
{
    if (L.found[i] & 0xC0U) {
        say("%s was on already: left as it is", iris_ldos[i].name);
        return 1;
    }
    L.mine[i] = 1;
    if ((L.found[i] & 0x3FU) != (unsigned)L.code[i] && !ldo_put(i, (uint8_t)L.code[i]))
        return 0;
    if (!ldo_put(i, (uint8_t)(IRIS_LDO_ON | L.code[i])))
        return 0;
    spin(200);                              /* S2MPS18_ENABLE_TIME_LDO 128 us */
    return 1;
}

/* off, at the code found (always tried, even after a failed write) */
static void ldo_off(unsigned i)
{
    if (!L.mine[i])
        return;
    if (ldo_put(i, L.found[i]))
        L.mine[i] = 0;
}

/* ---- the sensor's bus ---------------------------------------------------------------------------------------------- */
#define BUS_NONE 0
#define BUS_HS   1
#define BUS_BB   2

static struct {
    uint32_t con, dat, pud, mode, gate[4], qch;
    int up, kind, pick;                     /* pick: the kind that answered in P1a (P1b goes straight to it) */
} B;

static void bus_to_bb(void)
{
    upd(GPP4, BUS_MASK, 0);                 /* both released (inputs): the board's pull-ups hold them high */
    upd(GPP4 + 4, 3U << SCL, 0);            /* an output is only ever low */
    upd(GPP4 + 8, BUS_MASK, 0);
    B.kind = BUS_BB;
    uint32_t d = rd(GPP4 + 4);
    say("the bus is bit-banged now (gpp4-6/7 open drain by hand, at most ~%u kHz): idle SCL %u SDA %u",
        500U / BB_HALF_US / 2U,
        (d >> SCL) & 1U, (d >> SDA) & 1U);
}

static int bus_up(void)
{
    B.con = rd(GPP4) & BUS_MASK, B.dat = rd(GPP4 + 4) & (3U << SCL), B.pud = rd(GPP4 + 8) & BUS_MASK;
    B.mode = rd(USI_MODE), B.qch = rd(QCH_BUS);
    for (unsigned i = 0; i < 4; ++i)
        B.gate[i] = rd(bus_gates[i]);
    B.up = 1;
    /* amp.c's lesson (drivers run 7c): a gate in automatic mode did not clock USI07's HSI2C: held on by hand here */
    for (unsigned i = 0; i < 4; ++i)
        if ((B.gate[i] & GATE_ON) != GATE_ON)
            wr(bus_gates[i], B.gate[i] | GATE_ON);
    upd(QCH_BUS, 3U, 3U);
    if ((B.mode & 7U) != USI_I2C)
        wr(USI_MODE, (B.mode & ~7U) | USI_I2C);
    if (B.pick == BUS_BB) {
        bus_to_bb();
        return 1;
    }
    upd(GPP4, BUS_MASK, BUS_FN);
    upd(GPP4 + 8, BUS_MASK, 0);
    if ((rd(GPP4) & BUS_MASK) != BUS_FN && pins_ignored++ < 6)
        say("gpp4-6/7 did not take function 3: CON reads 0x%x (held by the secure world?)", rd(GPP4) & BUS_MASK);
    /* the timing: 400 kHz from an input clock taken as 400 MHz (cam.c's words for USI08_I2C: the same divider,
     * DIV_CLK_PERIC1_USI_I2C 0x1830) */
    uint32_t fs1 = rd(HS_BASE + 0x60), fs2 = rd(HS_BASE + 0x64), fs3 = rd(HS_BASE + 0x68);
    fs3 = (fs3 & ~0x00FF0000U) | (66U << 16);
    fs2 = (fs2 & ~0xFFU) | 0xE0U;
    fs1 = (fs1 & ~0x00FF0000U) | (0xF0U << 16);
    H.hs_init(H.ctx, fs1, fs2, fs3);
    B.kind = BUS_HS;
    say("hsi2c@108D0000 up: mode 0x%x (found 0x%x), pins 0x%x (found 0x%x), gates 0x%x 0x%x 0x%x 0x%x, Q 0x%x",
        rd(USI_MODE), B.mode, rd(GPP4) & BUS_MASK, B.con, rd(bus_gates[0]), rd(bus_gates[1]), rd(bus_gates[2]),
        rd(bus_gates[3]), rd(QCH_BUS));
    return 1;
}

/* the pins first (inputs, as found), then the mode, the Q-channel, the gates */
static void bus_down(void)
{
    if (!B.up)
        return;
    upd(GPP4, BUS_MASK, B.con);
    upd(GPP4 + 8, BUS_MASK, B.pud);
    upd(GPP4 + 4, 3U << SCL, B.dat);
    if (rd(USI_MODE) != B.mode)
        wr(USI_MODE, B.mode);
    upd(QCH_BUS, 3U, B.qch);
    for (unsigned i = 4; i-- > 0;)
        if (rd(bus_gates[i]) != B.gate[i])
            wr(bus_gates[i], B.gate[i]);
    B.up = 0;
    B.kind = BUS_NONE;
}

/* ---- the bit-banged bus: SCL gpp4-6, SDA gpp4-7; released = input, low = output 0 ---------------------------------- */
static void bb_line(unsigned pin, int low)
{
    upd(GPP4, 0xFU << (4 * pin), low ? 1U << (4 * pin) : 0);
    spin(BB_HALF_US);
}

static unsigned bb_get(unsigned pin)
{
    return (rd(GPP4 + 4) >> pin) & 1U;
}

static int bb_scl_up(void)                  /* SCL released; the slave may stretch it (2 ms at the most) */
{
    bb_line(SCL, 0);
    for (unsigned w = 0; w < 200 && !bb_get(SCL); ++w)
        spin(10);
    return (int)bb_get(SCL);
}

static int bb_start(void)                   /* also a repeated start (from SCL low) */
{
    bb_line(SDA, 0);
    if (!bb_scl_up() || !bb_get(SDA))
        return 0;                           /* SCL or SDA held low by someone */
    bb_line(SDA, 1);
    bb_line(SCL, 1);
    return 1;
}

static void bb_stop(void)
{
    bb_line(SDA, 1);
    (void)bb_scl_up();
    bb_line(SDA, 0);
}

static int bb_wbyte(uint8_t b)              /* 1 when acknowledged */
{
    for (int i = 7; i >= 0; --i) {
        bb_line(SDA, !((b >> i) & 1U));
        if (!bb_scl_up())
            return 0;
        bb_line(SCL, 1);
    }
    bb_line(SDA, 0);
    if (!bb_scl_up())
        return 0;
    int ack = !bb_get(SDA);
    bb_line(SCL, 1);
    return ack;
}

static int bb_rbyte(uint8_t *b, int ack)
{
    unsigned v = 0;
    bb_line(SDA, 0);
    for (int i = 0; i < 8; ++i) {
        if (!bb_scl_up())
            return 0;
        v = (v << 1) | bb_get(SDA);
        bb_line(SCL, 1);
    }
    bb_line(SDA, ack);                      /* ACK: SDA low; the last byte: NACK (released) */
    if (!bb_scl_up())
        return 0;
    bb_line(SCL, 1);
    bb_line(SDA, 0);
    *b = (uint8_t)v;
    return 1;
}

/* wire[0..wn) after a START (wire[0] the address byte), then (rn) a repeated start and rn bytes read; a STOP */
static int bb_xfer(const uint8_t *wire, unsigned wn, uint8_t *r, unsigned rn)
{
    int ok = bb_start();
    for (unsigned i = 0; ok && i < wn; ++i)
        ok = bb_wbyte(wire[i]);
    if (ok && rn) {
        ok = bb_start() && bb_wbyte((uint8_t)(wire[0] | 1U));
        for (unsigned i = 0; ok && i < rn; ++i)
            ok = bb_rbyte(&r[i], i + 1 < rn);
    }
    bb_stop();
    return ok;
}

/* ---- the sensor ---------------------------------------------------------------------------------------------------- */
static int sensor_bad;

static int s_rd(uint16_t reg, uint8_t *b, unsigned n)
{
    uint8_t w[4];
    unsigned wn = iris_i2c_wire(IRIS_ADDR, reg, 0, 0, w, sizeof w);
    if (B.kind == BUS_HS)
        return H.hs_msg(H.ctx, IRIS_ADDR, 0, w + 1, wn - 1, 0) && H.hs_msg(H.ctx, IRIS_ADDR, 1, b, n, 1);
    if (B.kind == BUS_BB)
        return bb_xfer(w, wn, b, n);
    return 0;
}

static int s_put(uint16_t reg, uint16_t v, unsigned n)
{
    uint8_t d[2] = { (uint8_t)(n == 2 ? v >> 8 : v), (uint8_t)v }, w[6];
    unsigned wn = iris_i2c_wire(IRIS_ADDR, reg, d, n, w, sizeof w);
    if (sensor_bad)
        return 0;
    int ok = B.kind == BUS_HS ? H.hs_msg(H.ctx, IRIS_ADDR, 0, w + 1, wn - 1, 1) :
             B.kind == BUS_BB ? bb_xfer(w, wn, 0, 0) : 0;
    if (!ok) {
        say("the sensor did not take 0x%04x = 0x%x", reg, v);
        sensor_bad = 1;
    }
    return ok;
}

static int s_w16(uint16_t reg, uint16_t v) { return s_put(reg, v, 2); }
static int s_w8(uint16_t reg, uint8_t v) { return s_put(reg, v, 1); }

/* ---- power, in sensor_5f1_power_setpin's order --------------------------------------------------------------------- */
enum { PS_PINS, PS_EN, PS_VDDIO, PS_VDDA, PS_VDDD, PS_I2C, PS_RESET, PS_MCLK, PS_N };
static const char *const ps_name[PS_N] = {
    "the pins iris_en / reset / MCLK (outputs low; as found when off)", "iris_en", "VDDIO 1.8 V", "VDDA 2.8 V",
    "VDDD 1.05 V (+1 ms)", "the I2C pins (+10 us)", "reset", "MCLK 26 MHz (+2 ms; off: 200 us)",
};
static unsigned ps_done;
static uint64_t t_power;
#ifdef IRIS_HOST
int iris_test_fail_step = -1;
#endif

static int ps_on(unsigned k)
{
#ifdef IRIS_HOST
    if ((int)k == iris_test_fail_step)
        return 0;
#endif
    switch (k) {
    case PS_PINS:
        pin_save(&en_pin), pin_save(&rst_pin), pin_save(&mclk_pin);
        pin_out(&rst_pin, 0), pin_out(&mclk_pin, 0), pin_out(&en_pin, 0);
        return 1;
    case PS_EN:
        pin_out(&en_pin, 1);
        return 1;
    case PS_VDDIO:
    case PS_VDDA:
        return ldo_on(k - PS_VDDIO);
    case PS_VDDD:
        if (!ldo_on(2))
            return 0;
        spin(1000);
        return 1;
    case PS_I2C:
        if (!bus_up())
            return 0;
        spin(10);
        return 1;
    case PS_RESET:
        pin_out(&rst_pin, 1);
        return 1;
    case PS_MCLK:
        clk_on();
        pin_fn(&mclk_pin, 2);
        spin(2000);
        return 1;
    default:
        return 0;
    }
}

static void ps_off(unsigned k)
{
    switch (k) {
    case PS_PINS:
        pin_restore(&mclk_pin), pin_restore(&rst_pin), pin_restore(&en_pin);
        break;
    case PS_EN:
        pin_out(&en_pin, 0);
        break;
    case PS_VDDIO:
    case PS_VDDA:
    case PS_VDDD:
        ldo_off(k - PS_VDDIO);
        break;
    case PS_I2C:
        bus_down();
        break;
    case PS_RESET:
        pin_out(&rst_pin, 0);
        break;
    case PS_MCLK:
        pin_out(&mclk_pin, 0);
        spin(200);
        clk_off();
        break;
    default:
        break;
    }
}

static struct iris_p1_result R;

static void power_down(void)
{
    if (!ps_done)
        return;
    while (ps_done) {
        --ps_done;
        ps_off(ps_done);
        say("power off: %s", ps_name[ps_done]);
    }
    uint32_t d = (uint32_t)(now_us() - t_power);
    if (d > R.powered_us)
        R.powered_us = d;
    int left = L.mine[0] || L.mine[1] || L.mine[2];
    say("the sensor is off (powered %u ms); %s", d / 1000U, left ? "A SUPPLY DID NOT SWITCH OFF" :
                                                                   "supplies, pins, CIS_CLK2 and the bus as found");
}

/* 1: every step on; 0: the step that failed and everything before it switched off again, in reverse order */
static int power_up(const char **why)
{
    sensor_bad = 0;
    if (!ldo_look()) {
        *why = "an iris supply could not be read or is someone else's";
        return 0;
    }
    t_power = now_us();
    for (unsigned k = 0; k < PS_N; ++k) {
        ps_done = k + 1;                    /* a step that fails half-way is undone too */
        if (!ps_on(k)) {
            say("power on: %s FAILED: everything off again", ps_name[k]);
            power_down();
            *why = "a power-on step failed";
            return 0;
        }
        say("power on: %s", ps_name[k]);
    }
    return 1;
}

/* ---- P1 ------------------------------------------------------------------------------------------------------------ */
enum {
    Q_IDLE, Q_START, Q_A_POWER, Q_A_INIT, Q_A_ID, Q_B_ISP, Q_B_POWER, Q_B_INIT, Q_B_ID, Q_B_OTP, Q_B_FILL, Q_B_LINK,
    Q_B_MODE, Q_B_GO, Q_B_STREAM, Q_B_WAIT, Q_B_STOP, Q_B_DOWN, Q_B_STATS, Q_END
};

int iris_use_prepare;

static struct {
    int q, isp, streaming, pass, prepared;
    uint64_t t0, t_q, t_stream;
    unsigned wait_us, k;
    uint8_t led0[7], led1[7];
    int led0_ok;
    volatile uint8_t *mem;
    uint32_t src_all;
    unsigned errs;
} P;

static void go(int q, unsigned us)
{
    P.q = q;
    P.t_q = now_us();
    P.wait_us = us;
}

static const char *bus_name(int k)
{
    return k == BUS_HS ? "HSI2C 0x108D0000" : k == BUS_BB ? "bit-banged gpp4-6/7" : "no bus";
}

static void stream_off(void)
{
    if (!P.streaming)
        return;
    int was = sensor_bad;
    sensor_bad = 0;
    (void)s_w8(0x0100, 0);
    sensor_bad = sensor_bad || was;
    P.streaming = 0;
    uint32_t d = (uint32_t)(now_us() - P.t_stream);
    if (d > R.stream_us)
        R.stream_us = d;
}

static void diag_line(const char *when)
{
    uint32_t d[6];
    H.diag(H.ctx, d);
    say("%s: CSIS3 INT_SRC0 0x%x INT_SRC1 0x%x FRM_CNT %u PHY_STATUS 0x%x; WDMA ERR 0x%x ACT 0x%x; DMA sources seen 0x%x",
        when, d[0], d[1], d[2], d[3], d[4], d[5], P.src_all);
}

/* everything off in reverse order: the stream, the WDMA, the link and the domain, then the sensor */
/* the secure camera session back (UNPREPARE), once, whatever happened: on every way out of P1 */
static void unprepare(void)
{
    if (!P.prepared || !H.smc)
        return;
    P.prepared = 0;
    int64_t r = H.smc(H.ctx, IRIS_SMC_UNPREPARE, 0, 0, 0);
    say("SMC MC_SECURE_CAMERA_UNPREPARE (0x83000044): %lld%s", (long long)r, r ? " (an error)" : " (ok)");
}

static void teardown(void)
{
    if (P.streaming) {
        stream_off();
        spin(10000);
    }
    if (P.isp) {
        H.dma_off(H.ctx);
        H.isp_down(H.ctx);
        P.isp = 0;
    }
    power_down();
    unprepare();
}

static void led_check(void)
{
    R.led_same = -1;
    if (!P.led0_ok)
        return;
    if (!H.led_rd(H.ctx, P.led1)) {
        say("the IR LED's registers could not be read afterwards");
        return;
    }
    R.led_same = 1;
    for (unsigned i = 0; i < 7; ++i)
        R.led_same = R.led_same && P.led0[i] == P.led1[i];
    say("the IR LED (S2MPB02 0x13, 0x16-0x1B): before %02x %02x %02x %02x %02x %02x %02x, after %02x %02x %02x %02x "
        "%02x %02x %02x: %s", P.led0[0], P.led0[1], P.led0[2], P.led0[3], P.led0[4], P.led0[5], P.led0[6], P.led1[0],
        P.led1[1], P.led1[2], P.led1[3], P.led1[4], P.led1[5], P.led1[6], R.led_same ? "the same (never written)" :
                                                                                     "CHANGED");
}

/* the summary: the key fact first */
static int finish(const char *why)
{
    unprepare();                                /* also on the way out without a teardown */
    led_check();
    char key[160];
    unsigned n = 0;
    const char *k = R.csis3 == 0 ? "CSIS3 FAULT: secure" : R.csis3 == 1 ? "CSIS3 readable" : "CSIS3 not reached";
    while (*k && n < sizeof key - 1)
        key[n++] = *k++;
    key[n] = 0;
    int pass = R.csis3 == 1 && R.frame && !why && R.led_same != 0 && !refused && R.stream_us <= IRIS_STREAM_MAX_US + 20000U;
    if (R.led_same == 0)
        why = "the IR LED's registers changed";
    else if (refused)
        why = "a write outside the iris's registers was refused (a bug)";
    if (pass) {
        say("PASS %s, frame %ux%u mean %u.%u (min %u max %u, %u.%u%% still fill), sensor 0x%04x ver 0x%02x on %s, "
            "streamed %u ms, LED %s", key, IRIS_W, IRIS_H, R.st.mean_x10 / 10U, R.st.mean_x10 % 10U, R.st.min, R.st.max,
            R.st.words ? R.st.fill_words * 1000U / R.st.words / 10U : 0U,
            R.st.words ? R.st.fill_words * 1000U / R.st.words % 10U : 0U, R.model, R.ver, bus_name(R.bus),
            R.stream_us / 1000U, R.led_same > 0 ? "unchanged" : "not read");
    } else if (R.csis3 == 0) {
        say("FAIL %s (a read of CSIS3 faulted once ISPPRE was on: no iris camera in the normal world); sensor %s, LED %s",
            key, R.sensor ? "answered" : "did not answer", R.led_same > 0 ? "unchanged" : R.led_same ? "not read" :
                                                                                                    "CHANGED");
    } else {
        const char *w = why ? why : R.frame_end ? "a frame end, but the frame did not reach memory" :
                                    R.csis3 == 1 ? "no frame end within 0.5 s of streaming" : "not finished";
        say("FAIL %s, %s: %s; sensor %s (ver 0x%02x on %s), %u pins ignored writes, LED %s", key,
            R.frame ? "frame received" : "no frame", w,
            R.sensor ? "answered" : "did not answer", R.ver, bus_name(R.bus), pins_ignored,
            R.led_same > 0 ? "unchanged" : R.led_same ? "not read" : "CHANGED");
    }
    P.pass = pass;
    P.q = Q_END;
    return 1;
}

static int fail(const char *why)
{
    say("FAILED: %s: everything off", why);
    teardown();
    return finish(why);
}

/* the OTP the stock way (sensor_5f1_init), streaming already on when stream is set */
static void otp_read(void)
{
    uint8_t page = 0xFF, v = 0;
    if (!s_w16(0x0A02, 0x3E00) || !s_w16(0x0A00, 0x0100))
        return;
    spin(360);
    (void)s_rd(0x0A12, &page, 1);
    uint16_t sel = page == 0x01 || page == 0x00 ? 0x3E00 : page == 0x03 ? 0x3F00 : 0;
    R.page = page;
    if (sel && s_w16(0x0A02, sel) && s_w16(0x0A00, 0x0100)) {
        spin(360);
        for (unsigned i = 0; i < 6; ++i)
            (void)s_rd((uint16_t)(0x0A04 + i), &R.otp[i], 1);
    }
    (void)s_rd(0x0002, &v, 1);
    (void)s_w16(0x0A00, 0x0000);
    say("OTP: page info 0x%02x (page 0x%x); test (focus, macro, pan) %u %u %u (1 1 1: passed), made 20%02x-%02x by 0x%x;"
        " 0x0002 0x%02x", page, sel, R.otp[0], R.otp[1], R.otp[2], R.otp[3], R.otp[4], R.otp[5], v);
}

static int ident(const char *when)
{
    uint8_t id[2] = { 0, 0 }, ver = 0;
    int ok = s_rd(0x0000, id, 2) && s_rd(0x0002, &ver, 1);
    if (!ok)
        return 0;
    R.model = (uint16_t)(id[0] << 8 | id[1]), R.ver = ver, R.sensor = 1;
    say("%s: THE SENSOR ANSWERS at 0x10 on %s: 0x0000 = 0x%04x, version 0x0002 = 0x%02x (%s)", when, bus_name(B.kind),
        R.model, ver, ver == 0xA1 ? "a final module" : "not 0xA1: not a final module");
    return 1;
}

/* the first access decides the bus: HSI2C first, then the bit-banged one; logged */
static int bus_pick(void)
{
    uint8_t v = 0;
    if (B.kind == BUS_HS) {
        for (unsigned t = 0; t < 2; ++t)
            if (s_rd(0x0002, &v, 1))
                return BUS_HS;
        say("hsi2c@108D0000: no answer at 0x10 (TRANS_STATUS 0x%x INT_STAT 0x%x FIFO 0x%x CTL 0x%x; lines 0x%x)",
            rd(HS_BASE + 0x50), rd(HS_BASE + 0x24), rd(HS_BASE + 0x30), rd(HS_BASE + 0x00), (rd(GPP4 + 4) >> SCL) & 3U);
        bus_to_bb();
    }
    for (unsigned t = 0; t < 2; ++t)
        if (s_rd(0x0002, &v, 1))
            return BUS_BB;
    return BUS_NONE;
}

static int init_reset(void)
{
    return s_w16(0xFCFC, 0x4000) && s_w16(0x6010, 0x0001);
}

static int init_clock(void)
{
    return s_w16(0xFCFC, 0x4000) && s_w16(0x6214, 0x7971) && s_w16(0x6218, 0x7150);
}

/* CSIS3 read fault-safe: its version, its control word, its WDMA and the D-PHY */
static int csis3_look(void)
{
    static const struct {
        const char *name;
        uint64_t pa;
    } at[] = { { "CSIS3 VERSION", CSIS3 }, { "CSIS3 CMN_CTRL", CSIS3 + 4 }, { "WDMA3 CTRL", WDMA3 },
               { "WDMA3 common", WDMA3 + 0x400 }, { "D-PHY SC_06", DPHY3 + 0x1018 } };
    int ok = 1;
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; ++i) {
        uint32_t v = 0;
        int r = H.rd32(H.ctx, at[i].pa, &v);
        if (r)
            say("%s (0x%llx) = 0x%x", at[i].name, (unsigned long long)at[i].pa, v);
        else
            say("%s (0x%llx): FAULT", at[i].name, (unsigned long long)at[i].pa);
        if (i < 3)
            ok = ok && r;
    }
    return ok;
}

void iris_p1_begin(void)
{
    uint8_t *z = (uint8_t *)&R;
    for (size_t i = 0; i < sizeof R; ++i)
        z[i] = 0;
    z = (uint8_t *)&P;
    for (size_t i = 0; i < sizeof P; ++i)
        z[i] = 0;
    z = (uint8_t *)&L;
    for (size_t i = 0; i < sizeof L; ++i)
        z[i] = 0;
    z = (uint8_t *)&B;
    for (size_t i = 0; i < sizeof B; ++i)
        z[i] = 0;
    R.csis3 = -1, R.led_same = -1;
    ps_done = 0, K.mine = 0, sensor_bad = 0, faults = 0, refused = 0, pins_ignored = 0;
    en_pin.saved = rst_pin.saved = mclk_pin.saved = 0;
#ifdef SBOS_KERNEL
    if (!hal_ok) {
        struct iris_hal h;
        iris_hal_libsb(&h);
        iris_hal_set(&h);
    }
#endif
    if (!hal_ok)
        return;
    P.t0 = now_us();
    go(Q_START, 0);
}

int iris_p1_step(unsigned *wait_ms)
{
    *wait_ms = 0;
    if (!hal_ok)
        return 1;
    uint64_t t = now_us();
    if (P.q == Q_IDLE || P.q == Q_END)
        return 1;
    if (t - P.t_q < P.wait_us) {
        uint64_t left = P.wait_us - (t - P.t_q);
        *wait_ms = (unsigned)((left + 999U) / 1000U);
        return 0;
    }
    if (t - P.t0 > IRIS_P1_LIMIT_US)
        return fail("P1 ran out of its own time (9 s)");
    switch (P.q) {
    case Q_START:
        say("P1 (attended): the IR LED is never written; its registers now:");
        P.led0_ok = H.led_rd(H.ctx, P.led0);
        if (P.led0_ok)
            say("IR LED before: %02x %02x %02x %02x %02x %02x %02x", P.led0[0], P.led0[1], P.led0[2], P.led0[3],
                P.led0[4], P.led0[5], P.led0[6]);
        else
            say("IR LED: not readable (the S2MPB02's bus): P1 goes on; nothing here can write it");
        P.mem = (volatile uint8_t *)H.ram(H.ctx, IRIS_MEM_PA, IRIS_FRAME_BYTES);
        if (!P.mem)
            return fail("the frame's memory (0xD8000000) is not mappable");
        if (iris_use_prepare) {                 /* P1c: the stock kernel's PREPARE before the sensor's power-up */
            if (!H.smc)
                return fail("P1c asked for PREPARE but there is no SMC hook");
            int64_t r = H.smc(H.ctx, IRIS_SMC_PREPARE, 0, 0, 0);
            say("SMC MC_SECURE_CAMERA_PREPARE (0x83000043): %lld%s", (long long)r,
                r ? " (refused: P1 goes on as without it)" : " (ok: the secure camera session is open)");
            P.prepared = r == 0;
        }
        go(Q_A_POWER, 0);
        break;
    case Q_A_POWER: {
        const char *why = 0;
        say("P1a: the sensor up in the stock order, its ID, everything off; no streaming");
        if (!power_up(&why))
            return fail(why);
        go(Q_A_INIT, 0);
        break;
    }
    case Q_A_INIT:
        R.bus = bus_pick();
        if (R.bus == BUS_NONE)
            return fail("the sensor does not answer at 0x10 (HSI2C, then bit-banged)");
        B.pick = R.bus;
        say("the sensor answers on %s", bus_name(R.bus));
        if (!init_reset())
            return fail("the sensor did not take its reset");
        go(Q_A_ID, 5000);
        break;
    case Q_A_ID: {
        if (!init_clock() || !ident("P1a"))
            return fail("the sensor did not take its clock words / answer after its reset");
        /* the OTP without streaming (P1a streams nothing): page info only, logged; P1b reads it the stock way */
        uint8_t page = 0xFF;
        if (s_w16(0x0A02, 0x3E00) && s_w16(0x0A00, 0x0100)) {
            spin(360);
            (void)s_rd(0x0A12, &page, 1);
            (void)s_w16(0x0A00, 0x0000);
        }
        say("P1a: OTP page info without streaming: 0x%02x (%s)", page,
            page <= 1 || page == 3 ? "readable" : "not readable this way: stock streams 4 ms for it, P1b does");
        power_down();
        say("P1a done: sensor 0x%04x ver 0x%02x on %s; everything off", R.model, R.ver, bus_name(R.bus));
        go(Q_B_ISP, 20000);
        break;
    }
    case Q_B_ISP:
        say("P1b: ISPPRE on, CSIS3 read fault-safe, then one dark frame (no LED, at most 0.5 s of streaming)");
        if (!H.isp_up(H.ctx, &iris_port))
            return fail("ISPPRE did not come up for CSIS3 (cam: the domain, its parent or the DTZPC SMC)");
        P.isp = 1;
        R.csis3 = csis3_look();
        if (!R.csis3) {
            say("CSIS3 FAULT: the secure world holds CSIS3 (or its WDMA): stopping, everything off");
            teardown();
            return finish(0);
        }
        say("CSIS3 readable in the normal world");
        go(Q_B_POWER, 0);
        break;
    case Q_B_POWER: {
        const char *why = 0;
        if (!power_up(&why))
            return fail(why);
        if (!init_reset())
            return fail("the sensor did not take its reset (P1b)");
        go(Q_B_ID, 5000);
        break;
    }
    case Q_B_ID:
        if (!init_clock() || !ident("P1b"))
            return fail("the sensor did not answer after its reset (P1b)");
        if (!s_w8(0x0100, 1))
            return fail("the sensor did not start streaming for its OTP");
        P.streaming = 1, P.t_stream = now_us();
        go(Q_B_OTP, 4000);
        break;
    case Q_B_OTP:
        otp_read();
        stream_off();
        P.k = 0;
        go(Q_B_FILL, 40000);
        break;
    case Q_B_FILL: {
        /* 1 MiB a call: the fill, so that what the WDMA wrote shows */
        volatile uint64_t *p = (volatile uint64_t *)(P.mem + (uint64_t)P.k * 0x100000U);
        uint32_t left = IRIS_FRAME_BYTES - P.k * 0x100000U, n = left < 0x100000U ? left : 0x100000U;
        for (uint32_t i = 0; i < n / 8U; ++i)
            p[i] = 0xA5A5A5A5A5A5A5A5ULL;
        if (++P.k * 0x100000U < IRIS_FRAME_BYTES) {
            go(Q_B_FILL, 0);
            break;
        }
        go(Q_B_LINK, 0);
        break;
    }
    case Q_B_LINK:
        if (!H.link_up(H.ctx, (uint32_t)IRIS_MEM_PA))
            return fail("CSIS3 / its D-PHY / its WDMA could not be set up");
        P.k = 0;
        go(Q_B_MODE, 0);
        break;
    case Q_B_MODE: {
        unsigned end = P.k + 13U < SETFILE_N ? P.k + 13U : (unsigned)SETFILE_N;
        for (; P.k < end; ++P.k)
            if (!s_w16(setfile[P.k][0], setfile[P.k][1]))
                return fail("the sensor did not take its setfile");
        if (P.k < SETFILE_N) {
            go(Q_B_MODE, 0);
            break;
        }
        if (!s_w16(0x0340, 0x13D7))
            return fail("the sensor did not take its frame length");
        uint8_t w[2] = { 0, 0 }, h[2] = { 0, 0 };
        (void)s_rd(0x034C, w, 2);
        (void)s_rd(0x034E, h, 2);
        say("the setfile: %u writes; the sensor reads back %u x %u", (unsigned)SETFILE_N, (unsigned)(w[0] << 8 | w[1]),
            (unsigned)(h[0] << 8 | h[1]));
        H.link_go(H.ctx);
        P.src_all = 0;
        (void)H.dma_src(H.ctx);
        go(Q_B_STREAM, 8000);
        break;
    }
    case Q_B_STREAM:
        if (!s_w8(0x0100, 1))
            return fail("the sensor did not start streaming");
        P.streaming = 1, P.t_stream = now_us();
        say("STREAM ON (0x0100 = 1), no LED: waiting for one frame end, 0.5 s at the most");
        go(Q_B_WAIT, 2000);
        break;
    case Q_B_WAIT: {
        uint32_t s = H.dma_src(H.ctx);
        P.src_all |= s;
        if ((s & DMA_ERR) && P.errs++ < 2)
            diag_line("the WDMA reports an error");
        if (s & DMA_FE) {
            R.frame_end = 1;
            H.dma_off(H.ctx);
            say("a frame end after %u ms of streaming", (unsigned)((now_us() - P.t_stream) / 1000U));
            go(Q_B_STOP, 0);
            break;
        }
        if (now_us() - P.t_stream >= IRIS_STREAM_MAX_US) {
            diag_line("no frame end in 0.5 s");
            H.dma_off(H.ctx);
            go(Q_B_STOP, 0);
            break;
        }
        go(Q_B_WAIT, 2000);
        break;
    }
    case Q_B_STOP:
        diag_line("after the capture");
        stream_off();
        say("STREAM OFF (0x0100 = 0) after %u ms; 40 ms for the frame to end", R.stream_us / 1000U);
        go(Q_B_DOWN, 40000);
        break;
    case Q_B_DOWN:
        H.isp_down(H.ctx);
        P.isp = 0;
        power_down();
        go(Q_B_STATS, 0);
        break;
    case Q_B_STATS: {
        iris_frame_stats(P.mem, IRIS_W, IRIS_H, IRIS_W, 4, &R.st);
        const volatile uint8_t *e = P.mem + IRIS_FRAME_BYTES - 8U;
        unsigned tail = 0;
        for (unsigned i = 0; i < 8; ++i)
            tail += e[i] == IRIS_FILL;
        uint32_t pm = R.st.words ? R.st.fill_words * 1000U / R.st.words : 1000U;
        R.frame = R.frame_end && pm < 50U && tail < 8U;
        say("frame (every 4th row): min %u max %u mean %u.%u, %u.%u%% of 8-byte words still 0xA5, last 8 bytes %s; "
            "histogram (16 bins of 16) %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", R.st.min, R.st.max,
            R.st.mean_x10 / 10U, R.st.mean_x10 % 10U, pm / 10U, pm % 10U, tail == 8U ? "fill" : "written",
            R.st.hist[0], R.st.hist[1], R.st.hist[2], R.st.hist[3], R.st.hist[4], R.st.hist[5], R.st.hist[6],
            R.st.hist[7], R.st.hist[8], R.st.hist[9], R.st.hist[10], R.st.hist[11], R.st.hist[12], R.st.hist[13],
            R.st.hist[14], R.st.hist[15]);
        return finish(0);
    }
    default:
        return fail("lost its place");
    }
    *wait_ms = (P.wait_us + 999U) / 1000U;
    return 0;
}

void iris_p1_abort(void)
{
    if (!hal_ok || P.q == Q_IDLE || P.q == Q_END) {
        P.q = Q_IDLE;
        return;
    }
    say("abort: everything off in reverse order");
    teardown();
    P.q = Q_IDLE;
}

int iris_p1_passed(void)
{
    return P.pass;
}

const struct iris_p1_result *iris_p1_last(void)
{
    return &R;
}

void iris_hal_set(const struct iris_hal *h)
{
    H = *h;
    hal_ok = h->rd32 && h->wr32 && h->pmic_rd && h->pmic_wr && h->hs_init && h->hs_msg && h->led_rd && h->delay_us &&
             h->now_us && h->log && h->ram && h->isp_up && h->link_up && h->link_go && h->dma_src && h->diag &&
             h->dma_off && h->isp_down;
}

/* ---- libsb glue ----------------------------------------------------------------------------------------------------- */
#ifdef SBOS_KERNEL
int fled_reg_read(uint8_t r, uint8_t *v);   /* fled.c */

static struct sb_i2c kbus = { 0x108D0000UL, 0, 0, 0, 0 };

static int k_rd32(void *c, uint64_t pa, uint32_t *v)
{
    (void)c;
    return sb_probe32(pa, v);
}

static void k_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int k_pmic_rd(void *c, uint8_t reg, uint8_t *v)
{
    (void)c;
    return sb_pmic_read(1, reg, v);
}

/* libsb's gate: one tier-1 authorisation per write, for that one register (the attended image is the user's
 * confirmation; the value is iris.c's own, limited by ldo_put above), as kern/nled.c / kern/vib.c / gpuheat.c do */
static int k_pmic_wr(void *c, uint8_t reg, uint8_t v)
{
    (void)c;
    if (reg < 0x70 || reg > 0x72)
        return 0;
    struct sb_auth au;
    sb_auth_init(&au, SB_SP_PMIC, 0, 1U << 8 | reg, 1, SB_TIER1, 0, "iris: LDO39-41 (VDDD, VDDA, VDDIO)");
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    int ok = sb_gate_open(&au) && sb_pmic_write(1, reg, v, SB_TIER1);
    sb_gate_close();
    return ok;
}

static void k_hs_init(void *c, uint32_t fs1, uint32_t fs2, uint32_t fs3)
{
    (void)c;
    kbus.fs1 = fs1, kbus.fs2 = fs2, kbus.fs3 = fs3;
    sb_i2c_bus(&kbus);
}

static int k_hs_msg(void *c, unsigned addr, int rx, uint8_t *b, unsigned n, int stop)
{
    (void)c;
    return sb_i2c_msg(&kbus, addr, rx, b, n, stop);
}

static int k_led_rd(void *c, uint8_t r[7])
{
    static const uint8_t at[7] = { 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };
    (void)c;
    for (unsigned i = 0; i < 7; ++i)
        if (fled_reg_read(at[i], &r[i]))
            return 0;
    return 1;
}

static void k_delay(void *c, uint32_t us)
{
    (void)c;
    sb_udelay(us);
}

static uint64_t k_now(void *c)
{
    (void)c;
    return sb_us();
}

static void k_log(void *c, const char *s)
{
    (void)c;
    sb_log(s);
}

static void *k_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (!len || sb_map_kind(pa) != 3 || sb_map_kind(pa + len - 1U) != 3)
        return 0;
    return (void *)(uintptr_t)pa;
}

static int k_isp_up(void *c, const struct cam_port *p)
{
    (void)c;
    return cam_port_up(p);
}

static int k_link_up(void *c, uint32_t pa)
{
    (void)c;
    return cam_port_link(pa);
}

static void k_link_go(void *c)
{
    (void)c;
    cam_port_go();
}

static uint32_t k_dma_src(void *c)
{
    (void)c;
    return cam_port_dma_src();
}

static void k_diag(void *c, uint32_t d[6])
{
    (void)c;
    cam_port_diag(d);
}

static void k_dma_off(void *c)
{
    (void)c;
    cam_port_dma_off();
}

static void k_isp_down(void *c)
{
    (void)c;
    cam_port_down();
}

static int64_t k_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c;
    return sb_psci(fn, a1, a2, a3);
}

void iris_hal_libsb(struct iris_hal *h)
{
    h->ctx = 0;
    h->rd32 = k_rd32, h->wr32 = k_wr32, h->pmic_rd = k_pmic_rd, h->pmic_wr = k_pmic_wr;
    h->hs_init = k_hs_init, h->hs_msg = k_hs_msg, h->led_rd = k_led_rd;
    h->delay_us = k_delay, h->now_us = k_now, h->log = k_log, h->ram = k_ram;
    h->isp_up = k_isp_up, h->link_up = k_link_up, h->link_go = k_link_go, h->dma_src = k_dma_src, h->diag = k_diag;
    h->dma_off = k_dma_off, h->isp_down = k_isp_down;
    h->smc = k_smc;
}
#endif
