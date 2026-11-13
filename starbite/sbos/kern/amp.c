/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the MAX98512 amplifier pair for in-call audio (the earpiece; the loudspeaker capped).
 *
 * Ported from Samsung's GPL-2.0 kernel for this phone (ref/starlte-kernel): sound/soc/codecs/max98512.c and .h
 * (max98512_remap_reg, max98512_i2c_probe's revision check, max98512_probe's register setup, max98512_dai_set_fmt,
 * max98512_dai_hw_params, max98512_set_clock, __max98512_spk_enable, max98512_spk_enable), the starlte DT
 * (exynos9810-starlte_common.dtsi hsi2c_18 / max98512@39, exynos9810.dtsi hsi2c_18 / usi_7, exynos9810-pinctrl.dtsi
 * hsi2c18-bus) and One UI's own vendor/etc/mixer_paths.xml and mixer_gains.xml (incall_nb-handset: "One Stop Mode"
 * Receiver Left, "Rcv Digital Gain" 32, "SPK Analog Left Gain" 3dB; incall_nb-speaker: "Mono Right"). Facts measured
 * on this phone by an earlier port (APSL-2.0, facts only, no code: the 0x39 die is the top one, and the setup below
 * ran both dies on the phone) agree.
 *
 * What is written: SYSREG_PERIC1's USI07 mode word (0x10811024 = 4, I2C) and gpp4-4/5 (function 2, no pull), each only
 * when it differs; then the two dies' own registers over I2C. CMU_PERIC1's USI07 gates and clock are only read (a
 * closed gate refuses). Every amplifier register written is in allow[] below, every gain value is checked against the
 * caps (amp.h), and every write except the self-clearing soft reset is read back.
 *
 * Deviations from Android, on purpose: the IV sense (MEAS_EN) and the bus's transmit slots stay off (they feed Maxim's
 * DSM, which does not run here), SPK_GAIN is muted at setup and AMP_VOL_CTRL starts at its lowest (Android writes
 * +15 dB / 0 dB there and the real level later), ENV_TRACK_BOOST_VOUT_READ (0x85, a read register on the B) is not
 * written, and the battery is taken to be above 5 C (BROWNOUT_LVL2/3_THRESH 0x30 / 0x10, Android's warm values). */
#include <stdarg.h>
#include "amp.h"

/* registers in the first revision's numbering (max98512.h MAX98512_R*); a B's address: remap() */
#define R_INT_RAW1 0x01U
#define R_INT_RAW2 0x02U
#define R_INT_RAW3 0x03U
#define R_INT_EN1 0x0AU
#define R_INT_EN2 0x0BU
#define R_IRQ_CTRL 0x10U
#define R_CLK_MON 0x11U
#define R_PCM_RX_EN_A 0x18U
#define R_PCM_TX_EN_A 0x1AU
#define R_PCM_TX_HIZ_A 0x1CU
#define R_PCM_TX_HIZ_B 0x1DU
#define R_PCM_MODE_CFG 0x20U
#define R_PCM_MASTER_MODE 0x21U
#define R_PCM_CLK_SETUP 0x22U
#define R_PCM_SR_SETUP1 0x23U
#define R_PCM_SR_SETUP2 0x24U
#define R_MONOMIX_A 0x25U
#define R_MONOMIX_B 0x26U
#define R_PDM_RX_CTRL 0x34U
#define R_AMP_VOL_CTRL 0x35U
#define R_AMP_DSP_CFG 0x36U
#define R_AMP_EN 0x38U
#define R_SPK_SRC_SEL 0x39U
#define R_SPK_GAIN 0x3AU
#define R_SSM_CFG 0x3BU
#define R_MEAS_EN 0x3CU
#define R_MEAS_DSP_CFG 0x3DU
#define R_BOOST_CTRL0 0x3EU
#define R_BOOST_CTRL3 0x3FU
#define R_BOOST_CTRL1 0x40U
#define R_SQUELCH 0x4EU
#define R_BROWNOUT_EN 0x50U
#define R_BROWNOUT_LVL_HOLD 0x53U
#define R_BROWNOUT_LVL2_THRESH 0x59U
#define R_BROWNOUT_LVL3_THRESH 0x5AU
#define R_BOOST_BYPASS_2 0x87U
#define R_GLOBAL_SHDN 0x400U
#define R_SOFT_RESET 0x401U
#define R_REV_ID 0x402U
#define R_REV_ID_B 0x600U           /* MAX98512B_R0600_REV_ID: where a B keeps it */
#define REV_ID_0 0x40U              /* MAX98512_REV_ID_0: a revision below this is not one */

#define TRIES 3                     /* MAX_TRY_COUNT: each transaction, as Android's wrapper */
#define RESET_POLL_MS 50U           /* the most a soft reset is given before the die answers again */
#define RAMP_DOWN_US 16000U         /* max98512_spk_enable(0): usleep_range(15000, 16000) */

/* ABOX side (One UI: "ABOX UAIF1 rate/width/channel" 48000 / 16 / 2), and the codec-side settings it implies */
#define SR_48000 0x8U               /* PCM_SR_SETUP1 [3:0], PCM_SR_SETUP2 [7:4] */
#define SR_IV 0x5U                  /* PCM_SR_SETUP2 [3:0]: 48 kHz - 3 (interleave_mode 1) */
#define CHANSZ_16 0x40U             /* PCM_MODE_CFG [7:6] = 1 */
#define BSEL_32 0x2U                /* PCM_CLK_SETUP [3:0]: 2 channels x 16 bits */

const uint8_t amp_rcv_vol[AMP_LEVELS] = { 0x08, 0x10, 0x18, 0x20, 0x20, 0x20 };
const uint8_t amp_spk_vol[AMP_LEVELS] = { 0x00, 0x10, 0x1C, 0x28, 0x2E, 0x34 };

static const struct audio_hal *H;
static const char *why = "not probed";

struct die {
    unsigned addr;
    const char *name;
    int kind;                       /* 0 not found, 'A', 'B' */
    uint8_t rev;
    uint8_t cur_limit;              /* BROWNOUT_LVL1-3_CUR_LIMIT: max98512_i2c_probe's defaults (the DT sets none) */
    uint8_t rx_bit;                 /* PCM_RX_EN_A in dai_set_fmt's stereo mode: channel 0 / 1 */
    int on;                         /* 0 shut down, 'R' receiver, 'S' speaker */
};
static struct die dies[2] = {
    { AMP_ADDR_RCV, "0x39 (earpiece)", 0, 0, 0x16, 0x01, 0 },
    { AMP_ADDR_SPK, "0x3A (speaker)", 0, 0, 0x3E, 0x02, 0 },
};
static int bus_up_done;
int amp_use_bb = 1;                 /* amp.c's transfers by hand on gpp4-4/5 (run 7e); 0: USI07's HSI2C */
static int any_b;                   /* a B among them: max98512_probe's extra block, written to both */
static uint8_t boost_mode;          /* BOOST_BYPASS_2: 0 (automatic) when the 0x39 die is a B, else 2 (off) */

/* ---- the log --------------------------------------------------------------------------------------------------- */

static void put(char *b, unsigned *n, unsigned cap, char c)
{
    if (*n + 1 < cap)
        b[(*n)++] = c;
}

/* %s %c %u %d %x with an optional zero-padded width (%02x, %04x); nothing else */
static void say(const char *fmt, ...)
{
    char b[200];
    unsigned n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = "amp: "; *p; ++p)
        put(b, &n, sizeof b, *p);
    for (const char *f = fmt; *f; ++f) {
        if (*f != '%') {
            put(b, &n, sizeof b, *f);
            continue;
        }
        ++f;
        unsigned width = 0;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; ++s)
                put(b, &n, sizeof b, *s);
        } else if (*f == 'c') {
            put(b, &n, sizeof b, (char)va_arg(ap, int));
        } else if (*f == 'u' || *f == 'd' || *f == 'x') {
            unsigned long v;
            if (*f == 'd') {
                int s = va_arg(ap, int);
                if (s < 0) {
                    put(b, &n, sizeof b, '-');
                    v = (unsigned long)(-(long)s);
                } else {
                    v = (unsigned long)s;
                }
            } else {
                v = va_arg(ap, unsigned);
            }
            unsigned base = *f == 'x' ? 16U : 10U;
            char t[24];
            unsigned k = 0;
            do {
                t[k++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v && k < sizeof t);
            while (k < width && k < sizeof t)
                t[k++] = '0';
            while (k)
                put(b, &n, sizeof b, t[--k]);
        } else if (*f == '%') {
            put(b, &n, sizeof b, '%');
        } else {
            break;
        }
    }
    va_end(ap);
    b[n] = 0;
    if (H && H->log)
        H->log(b);
}

static int fail(int rc, const char *w)
{
    why = w;
    say("FAILED (%d): %s", rc, w);
    return rc;
}

/* ---- the bus ------------------------------------------------------------------------------------------------- */

/* max98512_remap_reg for a B; the first revision's numbering otherwise */
static uint16_t remap(const struct die *d, uint16_t reg)
{
    if (d->kind != 'B')
        return reg;
    if (reg >= 0x01 && reg <= 0x1F)
        return reg;
    if (reg >= 0x20 && reg <= 0x28)
        return (uint16_t)(reg + 3);
    if (reg >= 0x2B && reg <= 0x4C)
        return (uint16_t)(reg + 1);
    if (reg >= 0x4F && reg <= 0x53)
        return (uint16_t)(reg + 2);
    if (reg >= 0x58 && reg <= 0x5F)
        return (uint16_t)(reg - 2);
    if (reg >= 0x70 && reg <= 0x7F)
        return (uint16_t)(reg - 18);
    if (reg >= 0x80 && reg <= 0x8D)
        return (uint16_t)(reg - 13);
    if (reg == R_GLOBAL_SHDN)
        return 0x500;
    if (reg == R_SOFT_RESET)
        return 0x501;
    if (reg == R_REV_ID)
        return R_REV_ID_B;
    return reg;
}

/* one transaction, at the chip's own address (16-bit register address, high byte first, 8-bit value) */
static int raw_rd(unsigned addr, uint16_t at, uint8_t *v)
{
    uint8_t w[2] = { (uint8_t)(at >> 8), (uint8_t)at };
    return H->i2c_xfer(AMP_BUS, addr, w, 2, v, 1) == 0 ? 0 : AUD_EIO;
}

static int rd(const struct die *d, uint16_t reg, uint8_t *v)
{
    uint16_t at = remap(d, reg);
    for (int t = 0; t < TRIES; ++t)
        if (raw_rd(d->addr, at, v) == 0)
            return 0;
    say("%s: register %04x (%04x in the chip) could not be read", d->name, reg, at);
    return AUD_EIO;
}

/* the registers this driver ever writes (first revision's numbering) */
static int allowed(uint16_t reg)
{
    static const uint16_t allow[] = {
        R_INT_EN1, R_INT_EN2, R_IRQ_CTRL, R_CLK_MON, R_PCM_RX_EN_A, R_PCM_TX_EN_A, R_PCM_TX_HIZ_A, R_PCM_TX_HIZ_B,
        R_PCM_MODE_CFG, R_PCM_MASTER_MODE, R_PCM_CLK_SETUP, R_PCM_SR_SETUP1, R_PCM_SR_SETUP2, R_MONOMIX_A, R_MONOMIX_B,
        R_PDM_RX_CTRL, R_AMP_VOL_CTRL, R_AMP_DSP_CFG, R_AMP_EN, R_SPK_SRC_SEL, R_SPK_GAIN, R_SSM_CFG, R_MEAS_EN,
        R_MEAS_DSP_CFG, R_BOOST_CTRL0, R_BOOST_CTRL3, R_BOOST_CTRL1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, R_SQUELCH, R_BROWNOUT_EN, R_BROWNOUT_LVL_HOLD, 0x58, 0x59, 0x5A, 0x5C, 0x5D, 0x5E, 0x70, 0x71, 0x72, 0x73,
        0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x80, 0x81, 0x82, 0x83, 0x84, 0x86, R_BOOST_BYPASS_2,
        0x88, R_GLOBAL_SHDN, R_SOFT_RESET,
    };
    for (unsigned i = 0; i < sizeof allow / sizeof allow[0]; ++i)
        if (allow[i] == reg)
            return 1;
    return 0;
}

/* the caps (amp.h): 1 the value may be written */
static int within_caps(const struct die *d, uint16_t reg, uint8_t v)
{
    int rcv = d->addr == AMP_ADDR_RCV;
    switch (reg) {
    case R_SPK_GAIN:                /* [2:0] the PCM gain, [6:4] the PDM gain (never used: 0) */
        return (v & 0xF8U) == 0 && (v & 0x07U) <= (rcv ? AMP_RCV_GAIN : AMP_SPK_GAIN_MAX);
    case R_AMP_VOL_CTRL:            /* [6:0] the volume, [7] its location (0) */
        return (v & 0x80U) == 0 && v <= (rcv ? AMP_RCV_VOL_MAX : AMP_SPK_VOL_MAX);
    case R_MEAS_EN:                 /* the IV sense: never on here */
        return (v & 0x03U) == 0;
    case R_PCM_TX_EN_A:             /* nothing sent back on the I2S bus */
        return v == 0;
    case R_BOOST_CTRL0:             /* Android's 0x1C at most, PVDD's bit clear */
        return v <= 0x1CU;
    default:
        return 1;
    }
}

/* written, then read back through `check' (0: not read back: the self-clearing soft reset) */
static int wr(const struct die *d, uint16_t reg, uint8_t v, uint8_t check)
{
    if (!allowed(reg) || !within_caps(d, reg, v)) {
        say("%s: refused: register %04x <- %02x is not allowed or above its cap", d->name, reg, v);
        return AUD_EPERM;
    }
    uint16_t at = remap(d, reg);
    uint8_t w[3] = { (uint8_t)(at >> 8), (uint8_t)at, v };
    int ok = 0;
    for (int t = 0; t < TRIES && !ok; ++t)
        ok = H->i2c_xfer(AMP_BUS, d->addr, w, 3, 0, 0) == 0;
    if (!ok) {
        say("%s: register %04x (%04x in the chip) <- %02x: the bus failed", d->name, reg, at, v);
        return AUD_EIO;
    }
    if (!check)
        return 0;
    uint8_t back = 0;
    int rc = rd(d, reg, &back);
    if (rc)
        return rc;
    if ((back ^ v) & check) {
        say("%s: register %04x <- %02x reads back %02x", d->name, reg, v, back);
        return AUD_EVERIFY;
    }
    return 0;
}

/* regmap_update_bits: read, change the mask's bits, write, read back the mask's bits */
static int upd(const struct die *d, uint16_t reg, uint8_t mask, uint8_t v)
{
    uint8_t now = 0;
    int rc = rd(d, reg, &now);
    if (rc)
        return rc;
    return wr(d, reg, (uint8_t)((now & ~mask) | (v & mask)), mask);
}

/* ---- the clocks and pins of the bus (read the gates; USI07 to I2C; gpp4-4/5 to SCL/SDA) -------------------------- */

#define CMU_PERIC1 0x10800000UL
#define USI07_MODE 0x10811024UL     /* usi_7: usi@10811024 */
#define GPP4 0x10830000UL           /* PERIC1 GPIO bank gpp4: CON +0, DAT +4, PUD +8 */
#define USI_MODE_I2C 0x4U

static int gate_closed(uint32_t v)
{
    return (v & 0x00300000U) == 0x00100000U;    /* MANUAL 1 with CG_VAL 0 */
}

static uint32_t gate_found[4];
static int gates_held;
static const uint16_t usi07_gates[4] = { 0x203C, 0x2094, 0x20E4, 0x20E8 };
static void gates_back(void)
{
    if (!gates_held)
        return;
    for (unsigned i = 0; i < 4; ++i)
        H->wr32(CMU_PERIC1 + usi07_gates[i], gate_found[i]);
    gates_held = 0;
    bus_up_done = 0;                /* the next use holds them again */
    say("bus: USI07 gates back as found");
}
static int bus_up(void)
{
    if (bus_up_done)
        return 0;
    static const uint16_t gates[4] = { 0x203C, 0x2094, 0x20E4, 0x20E8 };   /* GATE_CLK_..USI07_USI, RSTnSYNC, IPCLK, PCLK */
    uint32_t g[4];
    for (unsigned i = 0; i < 4; ++i)
        g[i] = H->rd32(CMU_PERIC1 + gates[i]);
    uint32_t mux = H->rd32(CMU_PERIC1 + 0x0120), div = H->rd32(CMU_PERIC1 + 0x181C);
    uint32_t mode = H->rd32(USI07_MODE), con = H->rd32(GPP4), pud = H->rd32(GPP4 + 8);
    uint32_t dat = H->rd32(GPP4 + 4);
    say("bus: CMU_PERIC1 USI07 gates %08x %08x %08x %08x, IP_USER mux %08x, div %08x; usi07 mode %x; gpp4 con %08x pud %08x",
        g[0], g[1], g[2], g[3], mux, div, mode, con, pud);
    /* read-only: the line levels before the pins become SCL / SDA (both 1 = pulled up and idle; 0 = held low, or the
     * pull-ups' rail is off) */
    say("bus: lines before set-up: SCL (gpp4-4) %u, SDA (gpp4-5) %u (gpp4 dat %02x)", (dat >> 4) & 1U, (dat >> 5) & 1U,
        dat & 0xFFU);
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(g[i]))
            return fail(AUD_EPERM, "a CMU_PERIC1 gate of USI07 is closed (not opened from here)");
    /* Drivers run 7c: with the four gates in automatic mode (00200000: bit 21 on, bit 20 manual off) the HSI2C sat in
     * its START state (TRANS_STATUS 00080001, no interrupt) while SCL / SDA idled high: its clock does not run. Held
     * on manually (bit 20 + 21, as cam.c does for its I2C buses) while amp.c uses the bus; amp_off() puts them back. */
    for (unsigned i = 0; i < 4; ++i) {
        gate_found[i] = g[i];
        if ((g[i] & 0x00300000U) != 0x00300000U) {
            H->wr32(CMU_PERIC1 + gates[i], g[i] | 0x00300000U);
            gates_held = 1;
        }
    }
    if (gates_held)
        say("bus: USI07 gates held on: %08x %08x %08x %08x", H->rd32(CMU_PERIC1 + gates[0]),
            H->rd32(CMU_PERIC1 + gates[1]), H->rd32(CMU_PERIC1 + gates[2]), H->rd32(CMU_PERIC1 + gates[3]));
    if ((mode & 0x7U) != USI_MODE_I2C) {
        H->wr32(USI07_MODE, (mode & ~0x7U) | USI_MODE_I2C);
        say("bus: USI07 mode %x -> %x (I2C)", mode, H->rd32(USI07_MODE));
    }
    if (((con >> 16) & 0xFFU) != 0x22U) {
        H->wr32(GPP4, (con & ~0x00FF0000U) | 0x00220000U);
        say("bus: gpp4-4/5 to function 2 (SCL, SDA): con %08x", H->rd32(GPP4));
    }
    if ((pud >> 16) & 0xFFU) {
        H->wr32(GPP4 + 8, pud & ~0x00FF0000U);
        say("bus: gpp4-4/5 pulls off (hsi2c18-bus pin-pud 0): pud %08x", H->rd32(GPP4 + 8));
    }
    if ((H->rd32(USI07_MODE) & 0x7U) != USI_MODE_I2C || ((H->rd32(GPP4) >> 16) & 0xFFU) != 0x22U)
        return fail(AUD_EVERIFY, "USI07's mode or the gpp4 pins did not take");
    bus_up_done = 1;
    return 0;
}

/* ---- the dies ------------------------------------------------------------------------------------------------ */

/* max98512_i2c_probe: REV_ID (0x0402) >= 0x40 is the first revision; else REV_ID at 0x0600 >= 0x40 is a B */
/* ---- a bit-banged address check (diagnostic, read-only towards the chip) --------------------------------------
 * Drivers run 7c / 7d: the HSI2C never left its START state while SCL / SDA idled high. To tell "our controller does
 * not drive the bus" from "nobody answers", gpp4-4 (SCL) / gpp4-5 (SDA) are briefly plain GPIOs: open-drain by hand
 * (input = released, the board's pull-ups make it 1; output 0 = pulled low), ~50 kHz, then an address-only write
 * (i2cdetect's quick write: START, addr << 1, the ACK bit, STOP; no register pointer, no data). The pins go back to
 * function 2 afterwards. */
#define BB_SCL 4U
#define BB_SDA 5U
static void bb_pin(unsigned pin, int low)
{
    uint32_t con = H->rd32(GPP4), dat = H->rd32(GPP4 + 4);
    if (low) {
        H->wr32(GPP4 + 4, dat & ~(1U << pin));
        H->wr32(GPP4, (con & ~(0xFU << (4 * pin))) | (1U << (4 * pin)));
    } else {
        H->wr32(GPP4, con & ~(0xFU << (4 * pin)));
    }
    H->delay_us(10);
}
static unsigned bb_get(unsigned pin) { return (H->rd32(GPP4 + 4) >> pin) & 1U; }
static int bb_quick(unsigned addr, unsigned *scl_stuck)
{
    unsigned byte = (addr << 1) & 0xFEU, ack;
    *scl_stuck = 0;
    bb_pin(BB_SDA, 0), bb_pin(BB_SCL, 0);
    bb_pin(BB_SDA, 1);                          /* START: SDA falls while SCL is high */
    bb_pin(BB_SCL, 1);
    for (int i = 7; i >= 0; --i) {
        bb_pin(BB_SDA, !((byte >> i) & 1U));
        bb_pin(BB_SCL, 0);
        for (unsigned w = 0; w < 100 && !bb_get(BB_SCL); ++w)
            H->delay_us(10);                    /* clock stretching, up to 1 ms */
        if (!bb_get(BB_SCL))
            *scl_stuck = 1;
        bb_pin(BB_SCL, 1);
    }
    bb_pin(BB_SDA, 0);                          /* released: the slave pulls it low to acknowledge */
    bb_pin(BB_SCL, 0);
    ack = !bb_get(BB_SDA);
    bb_pin(BB_SCL, 1);
    bb_pin(BB_SDA, 1);                          /* STOP: SDA rises while SCL is high */
    bb_pin(BB_SCL, 0);
    bb_pin(BB_SDA, 0);
    return (int)ack;
}
/* ---- the bit-banged bus amp.c uses for its transfers (run 7e) ------------------------------------------------
 * Run 7e: by hand on gpp4-4/5 both dies acknowledge (0x39, 0x3A ACK; an empty address does not), while USI07's HSI2C
 * never leaves its START state (runs 7c / 7d). So amp.c's register traffic (a few dozen bytes per call or stream
 * start) goes over the same two pins as open-drain GPIOs, ~25 kHz; the audio itself is I2S on UAIF1, not this bus.
 * Pins: input = released (the board's pull-ups), output 0 = low; never driven high. */
static int bb_scl_release(void)
{
    bb_pin(BB_SCL, 0);
    for (unsigned w = 0; w < 200 && !bb_get(BB_SCL); ++w)
        H->delay_us(10);                        /* clock stretching: up to 2 ms */
    return bb_get(BB_SCL) ? 0 : -1;
}
static int bb_start(void)                       /* also a repeated start (from SCL low) */
{
    bb_pin(BB_SDA, 0);
    if (bb_scl_release())
        return -1;
    if (!bb_get(BB_SDA))
        return -1;                              /* SDA held low by someone */
    bb_pin(BB_SDA, 1);
    bb_pin(BB_SCL, 1);
    return 0;
}
static void bb_stop(void)
{
    bb_pin(BB_SDA, 1);
    (void)bb_scl_release();
    bb_pin(BB_SDA, 0);
}
static int bb_wbyte(uint8_t b)                  /* 0 when acknowledged */
{
    for (int i = 7; i >= 0; --i) {
        bb_pin(BB_SDA, !((b >> i) & 1U));
        if (bb_scl_release())
            return -1;
        bb_pin(BB_SCL, 1);
    }
    bb_pin(BB_SDA, 0);
    if (bb_scl_release())
        return -1;
    int nack = (int)bb_get(BB_SDA);
    bb_pin(BB_SCL, 1);
    return nack ? -1 : 0;
}
static int bb_rbyte(uint8_t *b, int ack)
{
    unsigned v = 0;
    bb_pin(BB_SDA, 0);
    for (int i = 0; i < 8; ++i) {
        if (bb_scl_release())
            return -1;
        v = (v << 1) | bb_get(BB_SDA);
        bb_pin(BB_SCL, 1);
    }
    bb_pin(BB_SDA, ack);                        /* ACK: pull SDA low; the last byte: NACK (released) */
    if (bb_scl_release())
        return -1;
    bb_pin(BB_SCL, 1);
    bb_pin(BB_SDA, 0);
    *b = (uint8_t)v;
    return 0;
}
/* wn bytes written to addr, then (rn > 0) a repeated start and rn bytes read; a stop at the end. 0 or -1. */
int amp_bb_xfer(unsigned addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
    uint32_t con = H->rd32(GPP4), pud = H->rd32(GPP4 + 8);
    if ((con >> 16) & 0xFFU)                    /* SCL / SDA as plain inputs (released), no internal pulls */
        H->wr32(GPP4, con & ~0x00FF0000U);
    if ((pud >> 8) & 0xFU || (pud >> 16) & 0xFFU)
        H->wr32(GPP4 + 8, pud & ~0x00FF0F00U & ~0x00FF0000U);
    int rc = 0;
    if (wn) {
        rc = bb_start() || bb_wbyte((uint8_t)(addr << 1));
        for (unsigned i = 0; !rc && i < wn; ++i)
            rc = bb_wbyte(w[i]);
    }
    if (!rc && rn) {
        rc = bb_start() || bb_wbyte((uint8_t)((addr << 1) | 1U));
        for (unsigned i = 0; !rc && i < rn; ++i)
            rc = bb_rbyte(&r[i], i + 1 < rn);
    }
    bb_stop();
    return rc ? -1 : 0;
}

static void bb_check(void)
{
    uint32_t con = H->rd32(GPP4), dat = H->rd32(GPP4 + 4);
    unsigned st39 = 0, st3a = 0;
    bb_pin(BB_SCL, 0), bb_pin(BB_SDA, 0);
    unsigned idle_scl = bb_get(BB_SCL), idle_sda = bb_get(BB_SDA);
    int a39 = bb_quick(0x39, &st39), a3a = bb_quick(0x3A, &st3a), a00 = 0;
    unsigned st00 = 0;
    a00 = bb_quick(0x10, &st00);                /* an address nobody on this bus has: must NACK */
    H->wr32(GPP4, con);                         /* the pins back to SCL / SDA (function 2) */
    H->wr32(GPP4 + 4, dat);
    say("bit-bang (GPIO, ~50 kHz, address only): idle SCL %u SDA %u; 0x39 %s, 0x3A %s, 0x10 (nobody) %s%s", idle_scl,
        idle_sda, a39 ? "ACK" : "no ACK", a3a ? "ACK" : "no ACK", a00 ? "ACK (SDA stuck low?)" : "no ACK",
        st39 || st3a || st00 ? "; SCL held low by someone" : "");
}

static int identify(struct die *d)
{
    uint8_t id = 0, idb = 0;
    d->kind = 0;
    int ra = 1, rb = 1;
    for (int t = 0; t < TRIES && ra; ++t)
        ra = raw_rd(d->addr, R_REV_ID, &id);
    if (!ra && id >= REV_ID_0) {
        d->kind = 'A', d->rev = id;
    } else {
        for (int t = 0; t < TRIES && rb; ++t)
            rb = raw_rd(d->addr, R_REV_ID_B, &idb);
        if (!rb && idb >= REV_ID_0)
            d->kind = 'B', d->rev = idb;
    }
    if (d->kind)
        say("%s: MAX98512%s, revision %02x (REV_ID at %04x)", d->name, d->kind == 'B' ? "B" : "", d->rev,
            d->kind == 'B' ? R_REV_ID_B : R_REV_ID);
    else {
        say("%s: no MAX98512 answers (REV_ID 0x0402: %s %02x, 0x0600: %s %02x)", d->name, ra ? "bus error" : "read",
            id, rb ? (ra ? "bus error" : "not read") : "read", idb);
        /* read-only: the HSI2C's own view of that last read (libsb enables only XFER_DONE, so a NACK reads as 00):
         * TRANS_STATUS 0x50 (its NO_DEV / NO_DEV_ACK bits), INT_STAT 0x24, FIFO_STAT 0x30, CTL 0x00, CONF 0x40,
         * and the line levels now */
        say("%s: HSI2C TRANS_STATUS %08x INT_STAT %08x FIFO_STAT %08x CTL %08x CONF %08x; gpp4 dat %02x", d->name,
            H->rd32(AMP_BUS + 0x50), H->rd32(AMP_BUS + 0x24), H->rd32(AMP_BUS + 0x30), H->rd32(AMP_BUS + 0x00),
            H->rd32(AMP_BUS + 0x40), H->rd32(GPP4 + 4) & 0xFFU);
        /* read-only: a plain one-byte read with no register write first (no repeated start), to tell "nobody
         * acknowledges" from "the write-then-read does not work" (modem-core's suspect 2) */
        uint8_t one = 0;
        int rp = H->i2c_xfer(AMP_BUS, d->addr, 0, 0, &one, 1);
        say("%s: plain 1-byte read: %s %02x; TRANS_STATUS %08x", d->name, rp ? "bus error" : "read", one,
            H->rd32(AMP_BUS + 0x50));
    }
    return d->kind ? 0 : AUD_ENODEV;
}

/* max98512_spk_enable(0) for one die: GLOBAL_SHDN, AMP_EN, the IV sense, in that order; on errors it goes on */
static int die_off(struct die *d, int wait)
{
    int rc = 0, r;
    if ((r = upd(d, R_GLOBAL_SHDN, 0x01, 0)))
        rc = r;
    if ((r = upd(d, R_AMP_EN, 0x01, 0)))
        rc = r;
    if ((r = upd(d, R_MEAS_EN, 0x03, 0)))
        rc = r;
    if (wait)
        H->delay_us(RAMP_DOWN_US);
    d->on = 0;
    say("%s: shut down%s", d->name, rc ? " (with errors)" : "");
    return rc;
}

/* GLOBAL_SHDN 0, then max98512_probe's software reset; the die must answer with its revision again */
static int die_reset(struct die *d)
{
    int rc = upd(d, R_GLOBAL_SHDN, 0x01, 0x00);
    if (rc)
        return rc;
    if ((rc = wr(d, R_SOFT_RESET, 0x01, 0)))
        return rc;
    uint16_t at = remap(d, R_REV_ID);
    for (unsigned ms = 1; ms <= RESET_POLL_MS; ++ms) {
        H->delay_us(1000);
        uint8_t id = 0;
        if (raw_rd(d->addr, at, &id) == 0 && id == d->rev) {
            say("%s: soft reset, answering again after %u ms", d->name, ms);
            return 0;
        }
    }
    say("%s: no answer %u ms after the soft reset", d->name, RESET_POLL_MS);
    return AUD_ETIMEDOUT;
}

/* max98512_probe's setup (IV sense off, muted: see the header), its adc_config's reinit, then the DAI: I2S, NB_NF,
 * CBS_CFS (exynos9810/star_madera's UAIF1 link) and hw_params for S16_LE, 48 kHz, 2 channels */
static int die_setup(struct die *d)
{
    struct w {
        uint16_t reg;
        uint8_t v;
    };
    static const struct w head[] = {
        { R_MEAS_EN, 0x00 },        /* Android: 0x03 for DSM */
        { R_PCM_TX_HIZ_A, 0xFF }, { R_PCM_TX_HIZ_B, 0xFF },
    };
    static const struct w body[] = {
        { R_AMP_DSP_CFG, 0x01 },    /* the DC blocker */
        { R_MEAS_DSP_CFG, 0xF7 }, { R_BOOST_CTRL0, 0x1C }, { R_BOOST_CTRL1, 0x3E }, { R_BOOST_CTRL3, 0x01 },
        { 0x41, 0x07 }, { 0x42, 0x00 }, { 0x43, 0x0B }, { 0x44, 0x02 }, { 0x45, 0x02 }, { 0x46, 0x02 },
        { 0x47, 0x0C }, { 0x48, 0x0C }, { 0x49, 0x0C }, { R_SQUELCH, 0x10 }, { R_BROWNOUT_EN, 0x07 },
        { R_BROWNOUT_LVL_HOLD, 0xFF }, { 0x58, 0xA0 }, { 0x59, 0x30 }, { 0x5A, 0x10 }, { 0x5C, 0x0F }, { 0x5D, 0x76 },
    };
    static const struct w env[] = {
        { 0x7C, 0x00 }, { 0x7D, 0x00 }, { 0x80, 0x0A }, { 0x84, 0x01 }, { 0x86, 0x05 },
    };
    static const struct w b_extra[] = {    /* with a B among the dies (max98512_probe), to both */
        { R_INT_EN1, 0x00 }, { R_INT_EN2, 0x00 }, { R_IRQ_CTRL, 0x00 }, { R_PCM_MASTER_MODE, 0x00 },
        { R_PCM_CLK_SETUP, 0x22 }, { R_SSM_CFG, 0x05 }, { 0x5E, 0x00 }, { 0x81, 0x00 }, { 0x82, 0x00 }, { 0x83, 0x00 },
    };
    int rc = 0;
#define W(r, v) do { if ((rc = wr(d, (r), (v), 0xFF))) return rc; } while (0)
#define U(r, m, v) do { if ((rc = upd(d, (r), (m), (v)))) return rc; } while (0)
    for (unsigned i = 0; i < sizeof head / sizeof head[0]; ++i)
        W(head[i].reg, head[i].v);
    /* mono_stereo 1: 0x39 plays channel 0, 0x3A channel 1 (until __max98512_spk_enable sets the mode's mix) */
    W(R_MONOMIX_A, d->addr == AMP_ADDR_RCV ? 0x00 : 0x41);
    W(R_MONOMIX_B, d->addr == AMP_ADDR_RCV ? 0x00 : 0x01);
    W(R_AMP_VOL_CTRL, 0x00);        /* Android 0x40 (0 dB): here -16 dB until the level is set */
    W(R_SPK_GAIN, 0x00);            /* Android 0x05 (+15 dB): here muted until the level is set */
    for (unsigned i = 0; i < sizeof body / sizeof body[0]; ++i)
        W(body[i].reg, body[i].v);
    static const uint8_t ctrl1[3] = { 0x06, 0x09, 0x0C };
    for (unsigned l = 0; l < 3; ++l) {  /* BROWNOUT_LVL1-3: the current limit, AMP1_CTRL1-3 */
        W((uint16_t)(0x70 + 4 * l), d->cur_limit);
        W((uint16_t)(0x71 + 4 * l), ctrl1[l]);
        W((uint16_t)(0x72 + 4 * l), 0x00);
        W((uint16_t)(0x73 + 4 * l), 0x00);
    }
    for (unsigned i = 0; i < sizeof env / sizeof env[0]; ++i)
        W(env[i].reg, env[i].v);
    W(R_BOOST_BYPASS_2, boost_mode);
    W(0x88, 0x01);                  /* BOOST_BYPASS_3 */
    W(R_PCM_TX_HIZ_A, 0xFF);        /* max98512_reinit_reg: nothing sent back on the bus */
    W(R_PCM_TX_EN_A, 0x00);
    W(R_CLK_MON, 0x00);             /* the clock monitor off: shutting the clocks never trips it */
    if (any_b)
        for (unsigned i = 0; i < sizeof b_extra / sizeof b_extra[0]; ++i)
            W(b_extra[i].reg, b_extra[i].v);
    /* max98512_dai_set_fmt: slave, BCLK not inverted, this die's channel, I2S, the PCM input, no PDM */
    U(R_PCM_MASTER_MODE, 0x03, 0x00);
    U(R_PCM_MODE_CFG, 0x04, 0x00);
    U(R_PCM_RX_EN_A, d->rx_bit, d->rx_bit);
    U(R_PCM_MODE_CFG, 0x38, 0x00);
    U(R_SPK_SRC_SEL, 0x03, 0x00);
    U(R_PDM_RX_CTRL, 0x01, 0x00);
    /* max98512_dai_hw_params, max98512_set_clock: 16 bits, 48 kHz, the IV ADC at 24 kHz, 32 BCLKs a frame */
    U(R_PCM_MODE_CFG, 0xC0, CHANSZ_16);
    U(R_PCM_SR_SETUP1, 0x0F, SR_48000);
    U(R_PCM_SR_SETUP2, 0xF0, SR_48000 << 4);
    U(R_PCM_SR_SETUP2, 0x0F, SR_IV);
    U(R_PCM_CLK_SETUP, 0x0F, BSEL_32);
#undef W
#undef U
    return 0;
}

/* __max98512_spk_enable for one die: the receiver (OSM_RCV_L) or the mono speaker (OSM_MONO_R) */
static int die_enable(struct die *d, int mode, uint8_t gain, uint8_t vol)
{
    int rc;
    int rcv = mode == 'R';
#define W(r, v) do { if ((rc = wr(d, (r), (v), 0xFF))) return rc; } while (0)
#define U(r, m, v) do { if ((rc = upd(d, (r), (m), (v)))) return rc; } while (0)
    U(R_SPK_GAIN, 0x07, gain);
    U(R_AMP_VOL_CTRL, 0x7F, vol);
    U(R_MEAS_EN, 0x03, 0x00);       /* receiver: vimon 0; speaker: no DSM here */
    W(R_BROWNOUT_LVL2_THRESH, 0x30);
    W(R_BROWNOUT_LVL3_THRESH, 0x10);
    W(R_BOOST_BYPASS_2, boost_mode);
    W(R_PCM_RX_EN_A, 0x03);         /* both channels in */
    W(R_MONOMIX_A, rcv ? 0x80 : 0x41);  /* receiver: (ch0 + ch1) / 2; mono right: channel 1 */
    W(R_MONOMIX_B, 0x01);
    U(R_AMP_EN, 0x01, 0x01);
    U(R_AMP_EN, 0xC0, rcv ? 0x40 : 0x80);   /* receiver: DEM 0x40; speaker: 0x80 */
    U(R_GLOBAL_SHDN, 0x01, 0x01);
#undef W
#undef U
    d->on = mode;
    return 0;
}

/* ---- the API ------------------------------------------------------------------------------------------------- */

void amp_hal_set(const struct audio_hal *hal)
{
    H = hal;
    bus_up_done = 0;
    dies[0].kind = dies[1].kind = 0;
    dies[0].on = dies[1].on = 0;
    why = "not probed";
}

const char *amp_why(void)
{
    return why;
}

int amp_die_kind(unsigned addr, uint8_t *rev)
{
    for (unsigned i = 0; i < 2; ++i)
        if (dies[i].addr == addr) {
            if (rev)
                *rev = dies[i].rev;
            return dies[i].kind;
        }
    return 0;
}

static int hal_ok(void)
{
    return H && H->rd32 && H->wr32 && H->i2c_xfer && H->delay_us;
}

int amp_probe(void)
{
    if (!hal_ok()) {
        why = "no HAL";
        return AUD_ENOSYS;
    }
    say("probe: HSI2C %x (USI07), dies 0x39 and 0x3A", (unsigned)AMP_BUS);
    int rc = bus_up();
    if (rc)
        return rc;
    int r0 = identify(&dies[0]), r1 = identify(&dies[1]);
    any_b = dies[0].kind == 'B' || dies[1].kind == 'B';
    boost_mode = dies[0].kind == 'B' ? 0x00 : 0x02;
    if (r0 || r1) {
        bb_check();
        gates_back();
        return fail(AUD_ENODEV, r0 ? "the 0x39 die (earpiece) does not answer as a MAX98512" :
                                     "the 0x3A die (speaker) does not answer as a MAX98512");
    }
    why = "probed";
    say("probe: both dies found (boost bypass mode %u)", boost_mode);
    return 0;
}

static int ensure_probed(void)
{
    if (!hal_ok()) {
        why = "no HAL";
        return AUD_ENOSYS;
    }
    if (dies[0].kind && dies[1].kind)
        return 0;
    return amp_probe();
}

static void off_quiet(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (dies[i].kind)
            (void)die_off(&dies[i], 0);
    H->delay_us(RAMP_DOWN_US);
}

void amp_off(void)
{
    if (!hal_ok())
        return;
    say("off: both dies");
    for (unsigned i = 0; i < 2; ++i)
        if (dies[i].kind)
            (void)die_off(&dies[i], 0);
        else
            say("%s: never identified: not touched", dies[i].name);
    H->delay_us(RAMP_DOWN_US);
    gates_back();
    why = "off";
}

/* one mode on, the other die off; a failure leaves both shut down */
static int mode_on(int mode, int level)
{
    struct die *d = mode == 'R' ? &dies[0] : &dies[1];
    struct die *other = mode == 'R' ? &dies[1] : &dies[0];
    uint8_t gain = mode == 'R' ? AMP_RCV_GAIN : AMP_SPK_GAIN_MAX;
    uint8_t vol = mode == 'R' ? amp_rcv_vol[level] : amp_spk_vol[level];
    int rc;
    const char *m = mode == 'R' ? "receiver" : "speaker";
    if (d->on == mode) {            /* already on: the level only */
        say("%s: level %d: SPK_GAIN %u, AMP_VOL_CTRL %02x", m, level, gain, vol);
        if ((rc = upd(d, R_SPK_GAIN, 0x07, gain)) || (rc = upd(d, R_AMP_VOL_CTRL, 0x7F, vol))) {
            off_quiet();
            return fail(rc, "the level could not be set: both dies shut down");
        }
        return 0;
    }
    say("%s on, level %d: %s, I2S slave of UAIF1, 48 kHz 2x16", m, level, d->name);
    if (other->on || other->kind) {
        say("%s: %s first", m, other->on ? "the other die is on: shut down" : "the other die: made sure it is shut down");
        if ((rc = die_off(other, other->on != 0))) {
            off_quiet();
            return fail(rc, "the other die could not be shut down");
        }
    }
    say("%s: shut down and soft reset", m);
    if ((rc = die_reset(d))) {
        off_quiet();
        return fail(rc, "the soft reset failed: both dies shut down");
    }
    say("%s: setup (max98512_probe), I2S slave, 16 bits, 48 kHz, 32 BCLKs a frame", m);
    if ((rc = die_setup(d))) {
        off_quiet();
        return fail(rc, "the setup failed: both dies shut down");
    }
    say("%s: %s, SPK_GAIN %u (+%u dB), AMP_VOL_CTRL %02x, then AMP_EN and GLOBAL_SHDN", m,
        mode == 'R' ? "One Stop Mode Receiver Left" : "One Stop Mode Mono Right", gain, 3 * gain, vol);
    if ((rc = die_enable(d, mode, gain, vol))) {
        off_quiet();
        return fail(rc, "the enable failed: both dies shut down");
    }
    why = mode == 'R' ? "receiver on" : "speaker on";
    say("%s: on", m);
    return 0;
}

int amp_receiver(int on, int level)
{
    int rc = ensure_probed();
    if (rc)
        return rc;
    if (!on) {
        say("receiver off");
        rc = die_off(&dies[0], 1);
        if (rc) {
            off_quiet();
            return fail(rc, "the receiver did not shut down cleanly: both dies shut down");
        }
        why = "receiver off";
        return 0;
    }
    if (level < 0 || level >= AMP_LEVELS) {
        off_quiet();
        return fail(AUD_EINVAL, "receiver level out of 0..5: both dies shut down");
    }
    return mode_on('R', level);
}

int amp_speaker(int on, int level)
{
    int rc = ensure_probed();
    if (rc)
        return rc;
    if (!on) {
        say("speaker off");
        rc = die_off(&dies[1], 1);
        if (rc) {
            off_quiet();
            return fail(rc, "the speaker did not shut down cleanly: both dies shut down");
        }
        why = "speaker off";
        return 0;
    }
    if (level < 0 || level >= AMP_LEVELS) {
        off_quiet();
        return fail(AUD_EINVAL, "speaker level out of 0..5: both dies shut down");
    }
    return mode_on('S', level);
}

static void status(const struct die *d)
{
    static const uint16_t regs[] = { R_INT_RAW1, R_INT_RAW2, R_INT_RAW3, R_GLOBAL_SHDN, R_AMP_EN, R_SPK_GAIN,
                                     R_AMP_VOL_CTRL, R_PCM_RX_EN_A, R_MONOMIX_A, R_MEAS_EN };
    uint8_t v[10] = { 0 };
    int bad = 0;
    for (unsigned i = 0; i < 10; ++i)
        bad |= rd(d, regs[i], &v[i]) != 0;
    say("%s: INT_RAW1-3 %02x %02x %02x, GLOBAL_SHDN %02x, AMP_EN %02x, SPK_GAIN %02x, AMP_VOL %02x, RX_EN %02x, "
        "MONOMIX_A %02x, MEAS_EN %02x%s", d->name, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
        bad ? " (some reads failed)" : "");
}

int amp_selftest(unsigned hold_ms)
{
    say("selftest: probe, status, the earpiece on at level 0 for %u ms, status, off", hold_ms);
    int rc = amp_probe();
    if (rc)
        return rc;
    status(&dies[0]);
    status(&dies[1]);
    rc = amp_receiver(1, 0);
    if (rc)
        return rc;
    for (unsigned i = 0; i < hold_ms; ++i)
        H->delay_us(1000);
    status(&dies[0]);
    rc = amp_receiver(0, 0);
    amp_off();
    status(&dies[0]);
    status(&dies[1]);
    say("selftest: %s", rc ? "the receiver did not shut down cleanly" : "done");
    return rc;
}

/* ---- libsb's HAL (the kernel) -------------------------------------------------------------------------------- */
#ifdef SBOS_KERNEL
#include "sb.h"

static uint32_t k_rd32(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void k_wr32(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static struct sb_i2c k_amp_bus = { AMP_BUS, 0, 0, 0, 0 };

static int k_i2c_xfer(uintptr_t bus, unsigned addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
    if (bus != AMP_BUS || wn > 32 || rn > 32 || (!wn && !rn))
        return AUD_EINVAL;
    if (amp_use_bb)                 /* run 7e: the HSI2C never starts a transfer; the dies answer by hand */
        return amp_bb_xfer(addr, w, wn, r, rn) ? AUD_EIO : 0;
    if (!k_amp_bus.state) {         /* first use: S-Boot's timing, else Android's 200 MHz / 400 kHz one (fled.c) */
        k_amp_bus.fs1 = k_rd32(bus + 0x60), k_amp_bus.fs2 = k_rd32(bus + 0x64), k_amp_bus.fs3 = k_rd32(bus + 0x68);
        if (!((k_amp_bus.fs3 >> 16) & 0xffU)) {
            k_amp_bus.fs1 = 0x01F0FF00U;
            k_amp_bus.fs2 = 0x030003E0U;
            k_amp_bus.fs3 = 0x00210000U;
        }
        sb_i2c_bus(&k_amp_bus);
        k_amp_bus.state = 1;
    }
    uint8_t buf[32];
    for (unsigned i = 0; i < wn; ++i)
        buf[i] = w[i];
    if (wn && !sb_i2c_msg(&k_amp_bus, addr, 0, buf, wn, rn ? 0 : 1))
        return AUD_EIO;
    if (rn && !sb_i2c_msg(&k_amp_bus, addr, 1, r, rn, 1))
        return AUD_EIO;
    return 0;
}

static int k_gpio_set(uintptr_t bank, unsigned pin, int level)
{
    if (pin > 7)
        return AUD_EINVAL;
    uint32_t d = k_rd32(bank + 4);
    k_wr32(bank + 4, level ? (d | (1U << pin)) : (d & ~(1U << pin)));          /* the level first */
    k_wr32(bank + 8, k_rd32(bank + 8) & ~(0xfU << (4 * pin)));                  /* no pull */
    k_wr32(bank, (k_rd32(bank) & ~(0xfU << (4 * pin))) | (1U << (4 * pin)));    /* an output */
    return 0;
}

static int k_gpio_get(uintptr_t bank, unsigned pin)
{
    return pin > 7 ? AUD_EINVAL : (int)((k_rd32(bank + 4) >> pin) & 1U);
}

static int k_pmic_read(unsigned reg, uint8_t *v)
{
    return sb_pmic_read(1, reg, v) ? 0 : AUD_EIO;
}

static void k_delay(unsigned us)
{
    sb_udelay(us);
}

static const struct audio_hal k_hal = {
    k_rd32, k_wr32, k_i2c_xfer, 0, k_gpio_set, k_gpio_get, k_pmic_read, 0, k_delay, sb_log,
};

const struct audio_hal *audio_hal_libsb(void)
{
    return &k_hal;
}
#endif
