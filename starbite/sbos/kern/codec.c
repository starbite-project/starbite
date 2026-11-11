/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the CS47L93 codec's microphone path for in-call audio (codec.h has the wiring and the route).
 *
 * Ported from Samsung's GPL-2.0 kernel for this phone (ref/starlte-kernel): drivers/mfd/madera-core.c (madera_dev_init:
 * reset, supplies, ID, madera_wait_for_boot, the patch, the 32k clock, madera_configure_micbias, CTRL_SCRATCH),
 * drivers/mfd/madera-spi.c and cs47l92-tables.c (the SPI word: 32-bit address with bit 31 = read, 16 bits of padding,
 * 16- or 32-bit value, big-endian; cs47l92_reva_16_patch / _32_patch), sound/soc/codecs/madera.c (madera_set_sysclk,
 * madera_fllhj_apply / _enable / _disable, madera_wait_for_fll, madera_spin_sysclk, madera_set_fmt, madera_hw_params,
 * madera_configure_input_mode, madera_in_ev), cs47l92.c (the DAPM widgets and their order), arizona-micsupp.c (MICVDD),
 * drivers/pinctrl/cirrus/pinctrl-madera.c and pinctrl-cs47l92.c (the "aif1" group: GPIO5-8, bus hold),
 * drivers/spi/spi-s3c64xx.c (exynos_usi_init, s3c64xx_spi_hwinit, flush_fifo, the polled transfer),
 * cal-if/exynos9810/clkout_exynos9810.c and ra.c (CLKOUT0), sound/soc/samsung/star_madera.c (the clocks per bias level),
 * and One UI's mixer_paths.xml / mixer_gains.xml. Facts measured on this phone by an earlier port (APSL-2.0, facts
 * only: the codec answered 0x6371, booted, locked FLL1 from CLKOUT0 and recorded IN3L on AIF1 over UAIF0 with AIF1 on
 * SYSCLK; USI01's IP clock 400 MHz / 16 worked) agree.
 *
 * What is written outside the codec: SYSREG_PERIC0's USI01 mode word (SPI) and the SPI controller's own registers,
 * gpp0-4..7 (the bus pins) and gpg1-0 (the reset), CMU_PERIC0's USI01 divider only when the IP clock comes from CMU_TOP
 * and the divider would let SCLK pass the codec's 25 MHz (then ratio 16: 400 / 16 / 4 = 6.25 MHz, as measured), the
 * PMU's CLKOUT0 word (MCLK1; put back as found when the microphone goes off), and LDO34 only through the HAL's
 * pmic_write, only when it was off, and only switched back off if this file switched it on.
 *
 * Deliberate differences from One UI:
 * - AIF1 runs on SYSCLK (AIF1_RATE 0, SAMPLE_RATE_1 48 kHz). One UI runs it on ASYNCCLK from FLL2 locked to AIF1's own
 *   BCLK, which needs UAIF0 clocking before the codec can be set up. SYSCLK here comes from the same 26 MHz TCXO as
 *   ABOX's PLL_AUD, so the two stay within the PLLs' fractional error (well under a ppm); measured working on this phone.
 * - IN3L / IN4L go straight to AIF1TX1 / AIF1TX2. One UI passes them through LHPF1/2 and ASRC1 (sync to async); the
 *   LHPF coefficients are set by nothing in the vendor XMLs, and ASRC1 is only needed for the ASYNCCLK domain.
 * - MICBIAS2 (the headset microphone) and the jack detection are not configured. */
#include <stdarg.h>
#include "codec.h"

/* ---- the Exynos side ---- */
#define CMU_PERIC0 0x10400000UL
#define CMU_IP_USER 0x0120U         /* PLL_CON0_MUX_CLKCMU_PERIC0_IP_USER: [4] 1 = CMU_TOP's PERIC0_IP, 0 = OSCCLK */
#define CMU_USI01_DIV 0x1808U       /* CLK_CON_DIV_DIV_CLK_PERIC0_USI01_USI: [3:0] ratio - 1, [16] busy */
#define USI01_MODE 0x1041100CUL     /* usi_1: usi@1041100c */
#define USI_MODE_SPI 0x2U
#define GPP0 0x10430000UL           /* PERIC0 GPIO bank gpp0 (CON +0, DAT +4, PUD +8) */
#define GPG1 0x104300A0UL           /* bank gpg1: the reset is pin 0 */
#define PMU_CLKOUT0 0x14060A00UL    /* SEL [13:8] (1 = TCXO), bit 0 = disable */
#define L33CTRL 0x6AU
#define L34CTRL 0x6BU
#define LDO_ON 0xC0U                /* [7:6] = 3 */
#define LDO_VSEL 0x3FU
#define L34_1V2 40U                 /* (1.2 V - 0.7 V) / 12.5 mV */
#define DIV_SAFE 15U                /* USI01 ratio 16 */

/* the SPI controller (spi-s3c64xx.c) */
#define S_CH_CFG 0x00U
#define S_MODE_CFG 0x08U
#define S_SLAVE_SEL 0x0CU
#define S_INT_EN 0x10U
#define S_STATUS 0x14U
#define S_TX 0x18U
#define S_RX 0x1CU
#define S_PACKET_CNT 0x20U
#define S_PENDING_CLR 0x24U
#define S_SWAP_CFG 0x28U
#define S_FB_CLK 0x2CU
#define S_USI_CON 0xC4U
#define CH_SW_RST (1U << 5)
#define CH_HS_EN (1U << 6)
#define CH_SLAVE (1U << 4)
#define CH_CPOL (1U << 3)
#define CH_CPHA (1U << 2)
#define CH_RX_ON (1U << 1)
#define CH_TX_ON (1U << 0)
#define FIFO_MASK 0x7FU             /* samsung,spi-fifosize 64: (64 << 1) - 1 */
#define TX_LVL(s) (((s) >> 6) & FIFO_MASK)
#define RX_LVL(s) (((s) >> 15) & FIFO_MASK)
#define SPI_POLL_US 5000U

/* ---- the codec's registers (include/linux/mfd/madera/registers.h) ---- */
#define C_SOFTWARE_RESET 0x0000U
#define C_HARDWARE_REVISION 0x0001U
#define C_CTRL_SCRATCH 0x0087U
#define C_CLOCK_32K_1 0x0100U
#define C_SYSTEM_CLOCK_1 0x0101U
#define C_SAMPLE_RATE_1 0x0102U
#define C_FLL1 0x0170U              /* the FLL's base: CONTROL_n at +n */
#define C_MIC_CHARGE_PUMP_1 0x0200U
#define C_LDO2_CONTROL_1 0x0213U
#define C_MIC_BIAS_CTRL_1 0x0218U
#define C_MIC_BIAS_CTRL_5 0x021CU
#define C_INPUT_ENABLES 0x0300U
#define C_INPUT_ENABLES_STATUS 0x0301U
#define C_INPUT_VOLUME_RAMP 0x0309U
#define C_HPF_CONTROL 0x030CU
#define C_IN3L_CONTROL 0x0320U
#define C_ADC_VOL_3L 0x0321U
#define C_DMIC3L_CONTROL 0x0322U
#define C_IN4L_CONTROL 0x0328U
#define C_ADC_VOL_4L 0x0329U
#define C_DMIC4L_CONTROL 0x032AU
#define C_AIF1_BCLK_CTRL 0x0500U
#define C_AIF1_TX_PIN_CTRL 0x0501U
#define C_AIF1_RX_PIN_CTRL 0x0502U
#define C_AIF1_RATE_CTRL 0x0503U
#define C_AIF1_FORMAT 0x0504U
#define C_AIF1_RX_BCLK_RATE 0x0506U
#define C_AIF1_FRAME_CTRL_1 0x0507U
#define C_AIF1_FRAME_CTRL_2 0x0508U
#define C_AIF1_TX_ENABLES 0x0519U
#define C_AIF1TX1MIX_SRC 0x0700U
#define C_AIF1TX1MIX_VOL 0x0701U
#define C_AIF1TX2MIX_SRC 0x0708U
#define C_AIF1TX2MIX_VOL 0x0709U
#define C_GPIO1_CTRL_1 0x1700U      /* GPIOn: CTRL_1 at + 2 (n - 1) (FN [9:0]), CTRL_2 the next (PU 14, PD 13) */
#define C_IRQ1_STATUS_1 0x1800U
#define C_IRQ1_RAW_STATUS_1 0x1880U
#define C_IRQ1_RAW_STATUS_2 0x1881U
#define BOOT_DONE 0x0080U           /* MADERA_BOOT_DONE_STS1 / _EINT1 */
#define FLL1_LOCK 0x0100U           /* MADERA_FLL1_LOCK_STS1 */
#define IN_VU 0x0200U
#define IN_MUTE 0x0100U
#define SRC_IN3L 0x14U              /* madera_mixer_values */
#define SRC_IN4L 0x16U
#define MIX_0DB 0x80U               /* Input 1 Volume 0x40 << 1 */

/* the clocks (the sound node: cirrus,sysclk <1 4 98304000>, cirrus,fll1-refclk <1 0 26000000>) */
#define MCLK1_HZ 26000000U
#define SYSCLK_HZ 98304000U
#define SYSCLK_VAL 0x0404U          /* SYSCLK_FREQ 4 (98.304 MHz) << 8 | SYSCLK_SRC 4 (FLL1); no FRAC (98304000 % 6144000 == 0) */
#define SR_48K 3U                   /* madera_sr_vals */
#define BCLK_1536K 11U              /* madera_48k_bclk_rates: 1.536 MHz = 48 kHz x 2 x 16 */

static const struct audio_hal *H;
static const char *why = "not probed";
static int spi_ready, probed, inited, mic_on, ldo34_by_us, clkout_saved;
static uint32_t clkout_was;
static int fll_on;

/* ---- the log ---- */

static void put(char *b, unsigned *n, unsigned cap, char c)
{
    if (*n + 1 < cap)
        b[(*n)++] = c;
}

/* %s %c %u %d %x with an optional zero-padded width; nothing else */
static void say(const char *fmt, ...)
{
    char b[220];
    unsigned n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = "codec: "; *p; ++p)
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

static void shut(void);

static int fail(int rc, const char *w)
{
    say("FAILED (%d): %s; the codec goes back in reset", rc, w);
    shut();
    why = w;
    return rc;
}

/* ---- the SPI controller, polled (spi-s3c64xx.c), used when the HAL brings no spi_xfer ---- */

static int spi_flush(uintptr_t B)
{
    H->wr32(B + S_CH_CFG, H->rd32(B + S_CH_CFG) & ~(CH_RX_ON | CH_TX_ON));
    H->wr32(B + S_PACKET_CNT, 0);
    H->wr32(B + S_CH_CFG, (H->rd32(B + S_CH_CFG) | CH_SW_RST) & ~CH_HS_EN);
    unsigned t = 0;
    while (TX_LVL(H->rd32(B + S_STATUS)) && t++ < 1000)
        H->delay_us(1);
    for (unsigned i = 0; i < 2 * (FIFO_MASK + 1) && RX_LVL(H->rd32(B + S_STATUS)); ++i)
        (void)H->rd32(B + S_RX);
    uint32_t st = H->rd32(B + S_STATUS);
    H->wr32(B + S_CH_CFG, H->rd32(B + S_CH_CFG) & ~CH_SW_RST);
    H->wr32(B + S_MODE_CFG, H->rd32(B + S_MODE_CFG) & ~((1U << 2) | (1U << 1)));   /* no DMA */
    return (TX_LVL(st) || RX_LVL(st)) ? AUD_ETIMEDOUT : 0;
}

/* exynos_usi_init, s3c64xx_spi_hwinit, the chip select's feedback delay, s3c64xx_spi_config for 8 bits, mode 0 */
static int spi_init(uintptr_t B)
{
    H->wr32(B + S_USI_CON, 0);                  /* USI_RESET cleared */
    H->wr32(B + S_SLAVE_SEL, 1);                /* SLAVE_SIG_INACT: nCS high */
    H->wr32(B + S_INT_EN, 0);
    H->wr32(B + S_MODE_CFG, 0);
    H->wr32(B + S_PACKET_CNT, 0);
    H->wr32(B + S_PENDING_CLR, 0x1EU);
    H->wr32(B + S_PENDING_CLR, 0);
    H->wr32(B + S_SWAP_CFG, 0);
    H->wr32(B + S_MODE_CFG, (H->rd32(B + S_MODE_CFG) & ~1U & ~(0x3FFU << 19)) | (0x3FFU << 19));
    int rc = spi_flush(B);
    H->wr32(B + S_FB_CLK, 1);                   /* samsung,spi-feedback-delay 1 */
    H->wr32(B + S_CH_CFG, H->rd32(B + S_CH_CFG) & ~(CH_SLAVE | CH_CPOL | CH_CPHA));
    H->wr32(B + S_MODE_CFG, H->rd32(B + S_MODE_CFG) & ~((3U << 29) | (3U << 17)));    /* 8-bit channel and bus */
    return rc;
}

static int spi_mmio_xfer(uintptr_t B, const uint8_t *tx, uint8_t *rx, unsigned n)
{
    if (!n || n > FIFO_MASK / 2 + 1)
        return AUD_EINVAL;
    int rc = spi_flush(B);
    if (rc)
        return rc;
    H->wr32(B + S_SLAVE_SEL, 0);                /* nCS asserted (manual chip select) */
    H->wr32(B + S_CH_CFG, H->rd32(B + S_CH_CFG) | CH_RX_ON | CH_TX_ON);
    for (unsigned i = 0; i < n; ++i)
        H->wr32(B + S_TX, tx[i]);
    uint32_t st = 0;
    unsigned t = 0;
    while (RX_LVL(st = H->rd32(B + S_STATUS)) < n && t++ < SPI_POLL_US)
        H->delay_us(1);
    if (RX_LVL(st) >= n) {
        for (unsigned i = 0; i < n; ++i) {
            uint32_t v = H->rd32(B + S_RX);
            if (rx)
                rx[i] = (uint8_t)v;
        }
    } else {
        rc = AUD_ETIMEDOUT;
    }
    H->wr32(B + S_SLAVE_SEL, 1);                /* released */
    H->wr32(B + S_CH_CFG, H->rd32(B + S_CH_CFG) & ~(CH_RX_ON | CH_TX_ON));
    if (rc)
        say("spi: %u bytes not received in %u us (status %08x)", n, SPI_POLL_US, st);
    return rc;
}

static int xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
    int rc = H->spi_xfer ? H->spi_xfer(CODEC_SPI, tx, rx, n) : spi_mmio_xfer(CODEC_SPI, tx, rx, n);
    if (rc && !H->spi_xfer)             /* once more, as a fresh transfer */
        rc = spi_mmio_xfer(CODEC_SPI, tx, rx, n);
    return rc ? AUD_EIO : 0;
}

/* ---- the codec's registers ---- */

static int c_rd(uint32_t reg, uint16_t *v)
{
    uint8_t tx[8] = { (uint8_t)(0x80U | (reg >> 24)), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0, 0, 0 };
    uint8_t rx[8] = { 0 };
    int rc = xfer(tx, rx, 8);
    if (rc) {
        say("register %04x could not be read", reg);
        return rc;
    }
    *v = (uint16_t)(rx[6] << 8 | rx[7]);
    return 0;
}

/* the registers this file writes (the patch's are checked against the patch tables) */
static int allowed(uint32_t reg);

static int c_wr_raw(uint32_t reg, uint16_t v)
{
    if (!allowed(reg)) {
        say("refused: register %04x is not one this driver writes", reg);
        return AUD_EPERM;
    }
    uint8_t tx[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0,
                      (uint8_t)(v >> 8), (uint8_t)v };
    int rc = xfer(tx, 0, 8);
    if (rc)
        say("register %04x <- %04x: the bus failed", reg, v);
    return rc;
}

static int c_wr32(uint32_t reg, uint32_t v)
{
    if (!allowed(reg)) {
        say("refused: register %04x is not one this driver writes", reg);
        return AUD_EPERM;
    }
    uint8_t tx[10] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0,
                       (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    int rc = xfer(tx, 0, 10);
    if (rc)
        say("register %04x <- %08x (32-bit): the bus failed", reg, v);
    return rc;
}

/* regmap_update_bits, then the `check' bits read back as written (0: not read back) */
static int c_upd(uint32_t reg, uint16_t mask, uint16_t v, uint16_t check)
{
    uint16_t now = 0, back = 0;
    int rc = c_rd(reg, &now);
    if (rc)
        return rc;
    uint16_t want = (uint16_t)((now & ~mask) | (v & mask));
    if ((rc = c_wr_raw(reg, want)))
        return rc;
    if (!check)
        return 0;
    if ((rc = c_rd(reg, &back)))
        return rc;
    if ((back ^ want) & check) {
        say("register %04x <- %04x reads back %04x", reg, want, back);
        return AUD_EVERIFY;
    }
    return 0;
}

#define UPD(r, m, v) do { if ((rc = c_upd((r), (m), (v), (m)))) return rc; } while (0)
#define UPD_NV(r, m, v) do { if ((rc = c_upd((r), (m), (v), 0))) return rc; } while (0)

/* ---- the chip's patch (cs47l92-tables.c cs47l92_patch, applied by madera_dev_init right after the boot) ---- */

static const uint16_t patch16[][2] = {
    { 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 }, { 0x4EA, 0x0100 }, { 0x22B, 0x0000 },
    { 0x4A0, 0x0080 }, { 0x4A1, 0x0000 }, { 0x4A2, 0x0000 }, { 0x180B, 0x033F }, { 0x190B, 0x033F }, { 0x442, 0x0304 },
    { 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 }, { 0x120, 0x0305 }, { 0x4FA, 0x5064 }, { 0x1300, 0x050E },
    { 0x1302, 0x0101 }, { 0x1380, 0x02E0 }, { 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 },
    { 0x1390, 0x0304 }, { 0x1391, 0xF8FF }, { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 },
    { 0x13A1, 0xF942 }, { 0x13A2, 0x04CE }, { 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 }, { 0x13B1, 0xF8FF },
    { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 }, { 0x412, 0x0005 }, { 0x41A, 0x0005 }, { 0x422, 0x0005 },
};
static const uint32_t patch32[][2] = {
    { 0x3030, 0x04A00C01 }, { 0x3032, 0x0225F501 }, { 0x3044, 0x04A00C00 }, { 0x3046, 0x0225FF01 },
    { 0x3080, 0x04A00C01 }, { 0x3082, 0x0226F501 }, { 0x3094, 0x04A00C00 }, { 0x3096, 0x0226FF01 },
    { 0x30D1, 0x04A10C01 }, { 0x30D2, 0x0227F501 }, { 0x30E4, 0x04A10C00 }, { 0x30E6, 0x0227FF01 },
    { 0x3120, 0x04A10C01 }, { 0x3122, 0x0228F501 }, { 0x3134, 0x04A10C00 }, { 0x3136, 0x0228FF01 },
    { 0x3170, 0x04A20C01 }, { 0x3172, 0x022B0101 }, { 0x3174, 0x0229F501 }, { 0x3184, 0x04A20C00 },
    { 0x3186, 0x022B0100 }, { 0x3188, 0x0229FF01 }, { 0x31C0, 0x04A20C01 }, { 0x31C2, 0x022B0001 },
    { 0x31C4, 0x022AF501 }, { 0x31D4, 0x04A20C00 }, { 0x31D6, 0x022B0000 }, { 0x31D8, 0x022AFF01 },
};

static int allowed(uint32_t reg)
{
    static const uint16_t mine[] = {
        C_CTRL_SCRATCH, C_CLOCK_32K_1, C_SYSTEM_CLOCK_1, C_SAMPLE_RATE_1, C_FLL1 + 1, C_FLL1 + 2, C_FLL1 + 3,
        C_FLL1 + 4, C_FLL1 + 5, C_FLL1 + 6, C_FLL1 + 8, C_FLL1 + 0xA, C_FLL1 + 0xB, C_FLL1 + 0xD, C_MIC_CHARGE_PUMP_1,
        C_LDO2_CONTROL_1, C_MIC_BIAS_CTRL_1, C_MIC_BIAS_CTRL_5, C_INPUT_ENABLES, C_INPUT_VOLUME_RAMP, C_HPF_CONTROL,
        C_IN3L_CONTROL, C_ADC_VOL_3L, C_DMIC3L_CONTROL, C_IN4L_CONTROL, C_ADC_VOL_4L, C_DMIC4L_CONTROL,
        C_AIF1_BCLK_CTRL, C_AIF1_TX_PIN_CTRL, C_AIF1_RX_PIN_CTRL, C_AIF1_RATE_CTRL, C_AIF1_FORMAT,
        C_AIF1_RX_BCLK_RATE, C_AIF1_FRAME_CTRL_1, C_AIF1_FRAME_CTRL_2, C_AIF1_TX_ENABLES, C_AIF1TX1MIX_SRC,
        C_AIF1TX1MIX_VOL, C_AIF1TX2MIX_SRC, C_AIF1TX2MIX_VOL, C_GPIO1_CTRL_1 + 8, C_GPIO1_CTRL_1 + 9,
        C_GPIO1_CTRL_1 + 10, C_GPIO1_CTRL_1 + 11, C_GPIO1_CTRL_1 + 12, C_GPIO1_CTRL_1 + 13, C_GPIO1_CTRL_1 + 14,
        C_GPIO1_CTRL_1 + 15, C_IRQ1_STATUS_1,
    };
    for (unsigned i = 0; i < sizeof mine / sizeof mine[0]; ++i)
        if (mine[i] == reg)
            return 1;
    for (unsigned i = 0; i < sizeof patch16 / sizeof patch16[0]; ++i)
        if (patch16[i][0] == reg)
            return 1;
    for (unsigned i = 0; i < sizeof patch32 / sizeof patch32[0]; ++i)
        if (patch32[i][0] == reg)
            return 1;
    return 0;
}

/* ---- the Exynos side: clocks read, USI01 to SPI, the pins, the reset, the supplies ---- */

static int gate_closed(uint32_t v)
{
    return (v & 0x00300000U) == 0x00100000U;    /* MANUAL 1 with CG_VAL 0 */
}

static int bus_up(void)
{
    static const uint16_t gates[4] = { 0x2010, 0x205C, 0x20BC, 0x20C0 };   /* GATE_CLK_..USI01_USI, RSTnSYNC, IPCLK, PCLK */
    uint32_t g[4];
    for (unsigned i = 0; i < 4; ++i)
        g[i] = H->rd32(CMU_PERIC0 + gates[i]);
    uint32_t mux = H->rd32(CMU_PERIC0 + CMU_IP_USER), div = H->rd32(CMU_PERIC0 + CMU_USI01_DIV);
    uint32_t mode = H->rd32(USI01_MODE), con = H->rd32(GPP0), pud = H->rd32(GPP0 + 8);
    say("bus: CMU_PERIC0 USI01 gates %08x %08x %08x %08x, IP_USER mux %08x, div %08x; usi01 mode %x; gpp0 con %08x pud %08x",
        g[0], g[1], g[2], g[3], mux, div, mode, con, pud);
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(g[i]))
            return AUD_EPERM;
    if ((mux >> 4) & 1U) {
        /* CMU_TOP's PERIC0_IP (400 MHz on this phone): the quarter-multiplier before the SPI makes SCLK IP / 4; the
         * codec takes 25 MHz at most (spi-max-frequency) */
        if ((div & 0xFU) < DIV_SAFE) {
            H->wr32(CMU_PERIC0 + CMU_USI01_DIV, (div & ~0xFU) | DIV_SAFE);
            unsigned t = 0;
            while (((H->rd32(CMU_PERIC0 + CMU_USI01_DIV) >> 16) & 1U) && t++ < 1000)
                H->delay_us(1);
            uint32_t now = H->rd32(CMU_PERIC0 + CMU_USI01_DIV);
            say("bus: USI01's divider %u -> %u (ratio 16: 400 MHz / 16 / 4 = 6.25 MHz SCLK)", div & 0xFU, now & 0xFU);
            if ((now & 0xFU) != DIV_SAFE || ((now >> 16) & 1U))
                return AUD_EVERIFY;
        } else {
            say("bus: USI01's IP clock from CMU_TOP, divider ratio %u: SCLK at most 6.25 MHz", (div & 0xFU) + 1);
        }
    } else {
        say("bus: USI01's IP clock is OSCCLK (26 MHz): SCLK at most 6.5 MHz");
    }
    if ((mode & 0x7U) != USI_MODE_SPI) {
        H->wr32(USI01_MODE, (mode & ~0x7U) | USI_MODE_SPI);
        say("bus: USI01 mode %x -> %x (SPI)", mode, H->rd32(USI01_MODE));
    }
    if ((H->rd32(USI01_MODE) & 0x7U) != USI_MODE_SPI)
        return AUD_EVERIFY;
    if (!H->spi_xfer && !spi_ready) {
        int rc = spi_init(CODEC_SPI);
        say("bus: SPI controller set up (USI reset off, nCS high, 8 bits, mode 0, feedback delay 1)%s",
            rc ? ": its FIFOs did not empty" : "");
        if (rc)
            return rc;
    }
    spi_ready = 1;
    /* the pins after the controller (nCS already inactive): CLK/MISO/MOSI function 2 no pull, nCS function 2 pull-up */
    if ((con >> 16) != 0x2222U) {
        H->wr32(GPP0, (con & 0xFFFFU) | 0x22220000U);
        say("bus: gpp0-4..7 to function 2 (CLK, MISO, MOSI, nCS): con %08x", H->rd32(GPP0));
    }
    if ((pud >> 16) != 0x3000U) {
        H->wr32(GPP0 + 8, (pud & 0xFFFFU) | 0x30000000U);
        say("bus: gpp0-4..6 no pull, gpp0-7 pull-up: pud %08x", H->rd32(GPP0 + 8));
    }
    if ((H->rd32(GPP0) >> 16) != 0x2222U)
        return AUD_EVERIFY;
    return 0;
}

static int ldo_read(unsigned reg, uint8_t *v)
{
    int rc = H->pmic_read(reg, v);
    if (rc)
        say("the PMIC register %02x could not be read", reg);
    return rc ? AUD_EIO : 0;
}

/* LDO33 must be on (always-on); LDO34 (DCVDD) on at 1.2 V, switched on only through the HAL and only if it was off */
static int supplies_on(void)
{
    uint8_t l33 = 0, l34 = 0;
    int rc;
    if ((rc = ldo_read(L33CTRL, &l33)) || (rc = ldo_read(L34CTRL, &l34)))
        return rc;
    say("supplies: LDO33 (AVDD, DBVDD1, CPVDD1: 1.8 V) %02x %s; LDO34 (DCVDD, CPVDD2) %02x %s at %u mV", l33,
        (l33 & LDO_ON) == LDO_ON ? "on" : "OFF", l34, (l34 & LDO_ON) == LDO_ON ? "on" : "off",
        700U + (l34 & LDO_VSEL) * 25U / 2U);
    if ((l33 & LDO_ON) != LDO_ON)
        return AUD_EPERM;           /* always-on in the DT: not ours to switch */
    if ((l34 & LDO_ON) == LDO_ON) {
        if ((l34 & LDO_VSEL) != L34_1V2)
            return AUD_EPERM;       /* on at another voltage: left alone */
        return 0;
    }
    if (!H->pmic_write) {
        say("supplies: LDO34 is off and the HAL has no PMIC write: not switched");
        return AUD_ENOSYS;
    }
    uint8_t want = (uint8_t)((l34 & ~LDO_VSEL) | L34_1V2);
    if (want != l34 && (rc = H->pmic_write(L34CTRL, want)))
        return AUD_EIO;
    if ((rc = H->pmic_write(L34CTRL, (uint8_t)(want | LDO_ON))))
        return AUD_EIO;
    ldo34_by_us = 1;
    if ((rc = ldo_read(L34CTRL, &l34)))
        return rc;
    if ((l34 & (LDO_ON | LDO_VSEL)) != (LDO_ON | L34_1V2))
        return AUD_EVERIFY;
    H->delay_us(5000);              /* regulator-enable-ramp-delay 5000 */
    say("supplies: LDO34 switched on at 1.2 V (%02x), 5 ms ramp", l34);
    return 0;
}

/* ---- madera_dev_init's after-boot setup, once per power-up ---- */

static int chip_init(void)
{
    int rc;
    for (unsigned i = 0; i < sizeof patch16 / sizeof patch16[0]; ++i)
        if ((rc = c_wr_raw(patch16[i][0], patch16[i][1])))
            return rc;
    for (unsigned i = 0; i < sizeof patch32 / sizeof patch32[0]; ++i)
        if ((rc = c_wr32(patch32[i][0], patch32[i][1])))
            return rc;
    say("init: Cirrus's register patch (cs47l92_reva_16_patch %u, _32_patch %u registers)",
        (unsigned)(sizeof patch16 / sizeof patch16[0]), (unsigned)(sizeof patch32 / sizeof patch32[0]));
    UPD(C_CLOCK_32K_1, 0x0043, 0x0041);         /* CLK_32K_ENA, from MCLK2 */
    /* madera_configure_micbias: MICBIAS1A/1B/1C active discharge (their DT children); MICBIAS1 1.8 V, ext-cap */
    UPD(C_MIC_BIAS_CTRL_5, 0x0222, 0x0222);
    UPD(C_MIC_BIAS_CTRL_1, 0x81EA, 0x8060);
    /* madera_configure_input_mode: IN3 and IN4's DMIC clocks from MICBIAS1 (cirrus,dmic-ref <0 1 1 1>) */
    UPD(C_IN3L_CONTROL, 0x1800, 0x0800);
    UPD(C_IN4L_CONTROL, 0x1800, 0x0800);
    if ((rc = c_wr_raw(C_CTRL_SCRATCH, 0x00A5)))  /* CS47L92/93 */
        return rc;
    /* the pinctrl default: AIF1's pins (GPIO5-8) to their alternate function, bus hold */
    for (unsigned pin = 4; pin <= 7; ++pin) {
        UPD(C_GPIO1_CTRL_1 + 2 * pin, 0x03FF, 0x0000);
        UPD(C_GPIO1_CTRL_1 + 2 * pin + 1, 0x6000, 0x6000);
    }
    say("init: 32k clock from MCLK2, MICBIAS1 1.8 V (ext cap), IN3/IN4 DMIC on MICBIAS1, AIF1 pins (GPIO5-8, bus hold)");
    inited = 1;
    return 0;
}

/* ---- FLL1 (madera_fllhj_apply / _enable / _disable / madera_wait_for_fll) ---- */

struct fllcfg {
    unsigned refdiv, fbdiv, n, theta, lambda, gains, lockdet, hp, fast;
};

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int fllhj_calc(unsigned fin, unsigned fout, struct fllcfg *c)
{
    unsigned refdiv;
    for (refdiv = 0; refdiv < 4; refdiv++)
        if (fin / (1U << refdiv) <= 13000000U)
            break;
    unsigned fref = fin / (1U << refdiv);
    int frac = (fout % fref) != 0;
    unsigned fbdiv, gains, lockdet, fast = 0, min_n, max_n;
    if (fref < 192000U) {
        lockdet = 2, gains = 0x23F0, fbdiv = frac ? 256 : 4;
    } else if (fref < 1152000U) {
        lockdet = 8, gains = 0x22F2, fbdiv = 1;
    } else {
        lockdet = 8, gains = 0x21F0, fbdiv = 1;
        if (frac) {
            fast = 0x3;
            fout *= 6;
        }
    }
    unsigned hp = frac ? 0x3 : 0x0;
    min_n = frac ? 4 : 1;
    max_n = frac ? 255 : 1023;
    unsigned ratio = fout / fref;
    while (ratio / fbdiv < min_n) {
        fbdiv /= 2;
        if (fbdiv < 1)
            return AUD_EINVAL;
    }
    while (frac && ratio / fbdiv > max_n) {
        fbdiv *= 2;
        if (fbdiv >= 1024)
            return AUD_EINVAL;
    }
    unsigned g = gcd(fout, fbdiv * fref);
    unsigned num = fout / g, lambda = fbdiv * fref / g;
    c->refdiv = refdiv, c->fbdiv = fbdiv, c->n = num / lambda, c->theta = num % lambda, c->lambda = lambda;
    c->gains = gains, c->lockdet = lockdet, c->hp = hp, c->fast = fast;
    if (c->n < min_n || c->n > max_n || (frac && fbdiv >= 1024) || (!frac && fbdiv >= 256) || lambda > 0xFFFF)
        return AUD_EINVAL;
    return 0;
}

/* 1 locked == want within Android's 30 polls (75-125 us, then 20 ms, then 1 ms, then 20 ms) */
static int fll_wait(int want, unsigned *ms_taken)
{
    unsigned us = 0;
    for (int i = 0; i < 30; i++) {
        uint16_t v = 0;
        if (c_rd(C_IRQ1_RAW_STATUS_2, &v) == 0 && (!!(v & FLL1_LOCK)) == want) {
            *ms_taken = us / 1000;
            return 1;
        }
        unsigned d = i <= 5 ? 100U : (i >= 11 && i <= 20) ? 1000U : 20000U;
        H->delay_us(d);
        us += d;
    }
    *ms_taken = us / 1000;
    return 0;
}

static int fll1_on(void)
{
    struct fllcfg c;
    int rc = fllhj_calc(MCLK1_HZ, SYSCLK_HZ / 2, &c);
    if (rc) {
        say("clocks: FLL1: no valid configuration");
        return rc;
    }
    say("clocks: FLL1 from MCLK1 26 MHz to 49.152 MHz: REFDIV %u, N %u THETA %u LAMBDA %u, FBDIV %u, gains %04x, "
        "lock threshold %u, HP %u, fast oscillator %u", c.refdiv, c.n, c.theta, c.lambda, c.fbdiv, c.gains, c.lockdet,
        c.hp, c.fast);
    UPD(C_FLL1 + 1, 0x0004, 0x0004);            /* HOLD before any other register */
    if ((rc = c_wr_raw(C_FLL1 + 2, (uint16_t)c.n)))   /* N, CTRL_UPD clear */
        return rc;
    UPD(C_FLL1 + 3, 0xFFFF, (uint16_t)c.theta);
    UPD(C_FLL1 + 4, 0xFFFF, (uint16_t)c.lambda);
    UPD(C_FLL1 + 5, 0x03FF, (uint16_t)c.fbdiv);
    UPD(C_FLL1 + 6, 0x00C0, (uint16_t)(c.refdiv << 6));
    UPD(C_FLL1 + 8, 0xFFFF, (uint16_t)c.gains);
    UPD(C_FLL1 + 0xA, 0xD000, (uint16_t)(c.hp << 14 | 0x1000));     /* HP, PHASEDET_ENA */
    UPD(C_FLL1 + 0xB, 0x001E, (uint16_t)(c.lockdet << 1));
    UPD(C_FLL1 + 0xD, 0x0103, (uint16_t)c.fast);                      /* SYNC_EFS off, the VCO's fast source */
    UPD(C_FLL1 + 1, 0xF000, 0x0000);            /* REFCLK_SRC: MCLK1 */
    UPD(C_FLL1 + 1, 0x0001, 0x0001);            /* ENA */
    fll_on = 1;
    UPD(C_FLL1 + 0xB, 0x0001, 0x0001);          /* LOCKDET */
    UPD_NV(C_FLL1 + 2, 0x8000, 0x8000);         /* CTRL_UPD */
    UPD(C_FLL1 + 1, 0x0004, 0x0000);            /* HOLD off: it locks */
    unsigned ms = 0;
    if (!fll_wait(1, &ms)) {
        say("clocks: FLL1 did not lock within %u ms", ms);
        return AUD_ETIMEDOUT;
    }
    say("clocks: FLL1 locked within %u ms", ms);
    return 0;
}

static void fll1_off(void)
{
    if (!fll_on)
        return;
    (void)c_upd(C_FLL1 + 0xB, 0x0001, 0x0000, 0);
    (void)c_upd(C_FLL1 + 1, 0x0004, 0x0004, 0);
    (void)c_upd(C_FLL1 + 1, 0x0001, 0x0000, 0);
    unsigned ms = 0;
    int ok = fll_wait(0, &ms);
    (void)c_upd(C_FLL1 + 2, 0x8000, 0x0000, 0);
    fll_on = 0;
    say("clocks: FLL1 off%s", ok ? "" : " (its lock status did not clear)");
}

/* ---- the microphone path ---- */

static int clocks_on(void)
{
    int rc;
    uint32_t v = H->rd32(PMU_CLKOUT0);
    if (!clkout_saved)
        clkout_was = v, clkout_saved = 1;
    uint32_t want = (v & ~((0x3FU << 8) | 1U)) | (1U << 8);
    if (want != v)
        H->wr32(PMU_CLKOUT0, want);
    say("clocks: CLKOUT0 (MCLK1, the 26 MHz TCXO) %08x -> %08x", v, H->rd32(PMU_CLKOUT0));
    if ((H->rd32(PMU_CLKOUT0) & ((0x3FU << 8) | 1U)) != (1U << 8))
        return AUD_EVERIFY;
    H->delay_us(1000);
    UPD(C_SYSTEM_CLOCK_1, 0x870F, SYSCLK_VAL);  /* madera_set_sysclk: FLL1, 98.304 MHz (star_madera: before the FLL) */
    if ((rc = fll1_on()))
        return rc;
    return 0;
}

/* madera_set_fmt (I2S, NB_NF, CBS_CFS) and madera_hw_params (48 kHz, S16_LE, 2 channels, AIF1 on SYSCLK_1) */
static int aif1_setup(void)
{
    int rc;
    UPD(C_AIF1_BCLK_CTRL, 0x00A0, 0x0000);      /* BCLK_INV, BCLK_MSTR: a slave */
    UPD(C_AIF1_TX_PIN_CTRL, 0x0005, 0x0000);    /* LRCLK_INV, LRCLK_MSTR */
    UPD(C_AIF1_RX_PIN_CTRL, 0x0005, 0x0000);
    UPD(C_AIF1_FORMAT, 0x0007, 0x0002);         /* I2S */
    UPD(C_SAMPLE_RATE_1, 0x001F, SR_48K);
    UPD(C_AIF1_RATE_CTRL, 0xF800, 0x0000);      /* SYSCLK rate 1 */
    UPD(C_AIF1_BCLK_CTRL, 0x001F, BCLK_1536K);
    UPD(C_AIF1_RX_BCLK_RATE, 0x1FFF, 32);       /* 1.536 MHz / 48 kHz */
    UPD(C_AIF1_FRAME_CTRL_1, 0x3FFF, 16U << 8 | 16U);
    UPD(C_AIF1_FRAME_CTRL_2, 0x3FFF, 16U << 8 | 16U);
    say("aif1: I2S slave of UAIF0, 48 kHz, 16 bits in 16-bit slots, 32 BCLKs a frame, on SYSCLK");
    return 0;
}

static int mic_path_on(void)
{
    int rc;
    /* mixer_paths.xml's defaults for these inputs, then dev-dual-mic and gain-incall_nb-handset-mic */
    UPD(C_DMIC3L_CONTROL, 0x0700, 5U << 8);     /* IN3 OSR 3.072 MHz */
    UPD(C_DMIC4L_CONTROL, 0x0700, 5U << 8);
    UPD(C_IN3L_CONTROL, 0x8000, 0x8000);        /* IN3L HPF Switch */
    UPD(C_IN4L_CONTROL, 0x8000, 0x8000);
    UPD(C_HPF_CONTROL, 0x0007, 2);              /* IN HPF Cutoff Frequency 10 Hz */
    UPD(C_INPUT_VOLUME_RAMP, 0x0007, 5);        /* Input Ramp Up 8 ms/6 dB */
    UPD(C_ADC_VOL_3L, 0x01FF, IN_MUTE | CODEC_MIC_VOL_MAIN);   /* muted until the input is up (madera_in_ev) */
    UPD(C_ADC_VOL_4L, 0x01FF, IN_MUTE | CODEC_MIC_VOL_SUB);
    UPD(C_AIF1TX1MIX_SRC, 0x00FF, SRC_IN3L);
    UPD(C_AIF1TX1MIX_VOL, 0x00FE, MIX_0DB);
    UPD(C_AIF1TX2MIX_SRC, 0x00FF, SRC_IN4L);
    UPD(C_AIF1TX2MIX_VOL, 0x00FE, MIX_0DB);
    say("route: AIF1TX1 <- IN3L (DMIC1, main, %u), AIF1TX2 <- IN4L (DMIC4, second, %u), HPF 10 Hz, OSR 3.072 MHz",
        CODEC_MIC_VOL_MAIN, CODEC_MIC_VOL_SUB);
    if ((rc = aif1_setup()))
        return rc;
    /* DAPM, in its order: MICVDD (regulator), SYSCLK / MICBIAS1 / 1A / 1B (supplies), AIF1TX1/2, the input PGAs */
    UPD(C_LDO2_CONTROL_1, 0x07E0, 0x24U << 5);  /* micvdd 3.0 V (LDO2_VSEL 0x24) */
    UPD(C_MIC_CHARGE_PUMP_1, 0x0003, 0x0001);   /* CPMIC_ENA, not bypassed */
    H->delay_us(3000);                          /* its enable_time */
    UPD(C_SYSTEM_CLOCK_1, 0x0040, 0x0040);      /* SYSCLK_ENA, then madera_spin_sysclk */
    for (int i = 0; i < 4; ++i) {
        uint16_t v;
        (void)c_rd(C_SOFTWARE_RESET, &v);
    }
    H->delay_us(300);
    UPD(C_MIC_BIAS_CTRL_1, 0x0001, 0x0001);     /* MICBIAS1 */
    UPD(C_MIC_BIAS_CTRL_5, 0x0011, 0x0011);     /* MICBIAS1A (DMIC1), MICBIAS1B (DMIC4) */
    H->delay_us(5000);
    say("power: MICVDD 3.0 V, SYSCLK on, MICBIAS1 1.8 V with 1A and 1B");
    UPD(C_AIF1_TX_ENABLES, 0x0003, 0x0003);
    UPD(C_INPUT_ENABLES, 0x00A0, 0x00A0);       /* IN3L_ENA, IN4L_ENA */
    UPD(C_ADC_VOL_3L, IN_MUTE, 0);              /* POST_PMU: unmuted, then the volume update */
    UPD(C_ADC_VOL_4L, IN_MUTE, 0);
    H->delay_us(2000);
    UPD_NV(C_ADC_VOL_3L, IN_VU, IN_VU);         /* IN_VU reads back 0 */
    UPD_NV(C_ADC_VOL_4L, IN_VU, IN_VU);
    uint16_t st = 0;
    (void)c_rd(C_INPUT_ENABLES_STATUS, &st);
    say("power: AIF1TX1/2 on, IN3L and IN4L on (INPUT_ENABLES_STATUS %04x)", st);
    return 0;
}

/* DAPM's down order, then the bias level off; best effort, every step tried */
static void mic_path_off(void)
{
    (void)c_upd(C_ADC_VOL_3L, IN_MUTE | IN_VU, IN_MUTE | IN_VU, 0);   /* PRE_PMD */
    (void)c_upd(C_ADC_VOL_4L, IN_MUTE | IN_VU, IN_MUTE | IN_VU, 0);
    (void)c_upd(C_INPUT_ENABLES, 0x00A0, 0x0000, 0);
    (void)c_upd(C_AIF1_TX_ENABLES, 0x0003, 0x0000, 0);
    (void)c_upd(C_AIF1TX1MIX_SRC, 0x00FF, 0x0000, 0);
    (void)c_upd(C_AIF1TX2MIX_SRC, 0x00FF, 0x0000, 0);
    (void)c_upd(C_MIC_BIAS_CTRL_5, 0x0011, 0x0000, 0);
    (void)c_upd(C_MIC_BIAS_CTRL_1, 0x0001, 0x0000, 0);
    (void)c_upd(C_SYSTEM_CLOCK_1, 0x0040, 0x0000, 0);
    (void)c_upd(C_MIC_CHARGE_PUMP_1, 0x0001, 0x0000, 0);
    fll1_off();
    say("mic off: inputs muted and off, AIF1TX off, MICBIAS1 and MICVDD off, SYSCLK and FLL1 off");
}

static void clkout_restore(void)
{
    if (!clkout_saved)
        return;
    H->wr32(PMU_CLKOUT0, clkout_was);
    clkout_saved = 0;
    say("clocks: CLKOUT0 put back as found (%08x)", H->rd32(PMU_CLKOUT0));
}

/* back in reset; LDO34 off only if this file switched it on; CLKOUT0 as found */
static void shut(void)
{
    if (!H)
        return;
    if (mic_on || fll_on) {
        if (probed)
            mic_path_off();
        mic_on = 0;
    }
    clkout_restore();
    if (H->gpio_set && H->gpio_set(GPG1, 0, 0) == 0)
        say("reset: gpg1-0 low (the codec held in reset)");
    else
        say("reset: gpg1-0 could NOT be driven low");
    if (ldo34_by_us && H->pmic_write) {
        uint8_t v = 0;
        if (ldo_read(L34CTRL, &v) == 0 && H->pmic_write(L34CTRL, (uint8_t)(v & ~LDO_ON)) == 0)
            say("supplies: LDO34 switched back off (this file had switched it on)");
        H->delay_us(20000);         /* madera_dcvdd_notify: 20 ms after DCVDD goes off */
        ldo34_by_us = 0;
    }
    probed = inited = 0;
}

/* ---- the API ---- */

void codec_hal_set(const struct audio_hal *hal)
{
    H = hal;
    spi_ready = probed = inited = mic_on = ldo34_by_us = clkout_saved = fll_on = 0;
    why = "not probed";
}

const char *codec_why(void)
{
    return why;
}

static int hal_ok(void)
{
    return H && H->rd32 && H->wr32 && H->gpio_set && H->pmic_read && H->delay_us;
}

int codec_probe(void)
{
    if (!hal_ok()) {
        why = "no HAL";
        return AUD_ENOSYS;
    }
    say("probe: SPI %x (USI01 of PERIC0), reset gpg1-0, LDO33 / LDO34", (unsigned)CODEC_SPI);
    if (mic_on)
        mic_path_off(), mic_on = 0;
    int rc = bus_up();
    if (rc)
        return fail(rc, rc == AUD_EPERM ? "a CMU_PERIC0 gate of USI01 is closed (not opened from here)" :
                                          "USI01, its clock or the gpp0 pins did not take");
    if ((rc = H->gpio_set(GPG1, 0, 0)))
        return fail(AUD_EIO, "the reset pin gpg1-0 could not be driven low");
    say("reset: held (gpg1-0 low), 20 ms before the supplies (madera_dev_init)");
    H->delay_us(20000);
    if ((rc = supplies_on()))
        return fail(rc, rc == AUD_ENOSYS ? "LDO34 (DCVDD) is off and there is no PMIC write hook" :
                        rc == AUD_EPERM ? "LDO33 is off, or LDO34 is on at another voltage than 1.2 V" :
                                          "the codec's supplies could not be read or set");
    if ((rc = H->gpio_set(GPG1, 0, 1)))
        return fail(AUD_EIO, "the reset pin gpg1-0 could not be driven high");
    H->delay_us(2000);              /* madera_disable_hard_reset: usleep_range(1000, 2000) */
    say("reset: released (gpg1-0 high, level %d), 2 ms", H->gpio_get ? H->gpio_get(GPG1, 0) : -1);
    uint16_t id = 0, raw = 0, rev = 0;
    if ((rc = c_rd(C_SOFTWARE_RESET, &id)))
        return fail(rc, "the ID register could not be read over SPI");
    say("id: %04x (%s)", id, id == CODEC_ID ? "CS47L92/93" : "NOT a CS47L92/93");
    if (id != CODEC_ID)
        return fail(AUD_ENODEV, id == 0x0000 || id == 0xFFFF ? "no answer on SPI (ID 0000 / ffff)" :
                                                                "the ID is not 0x6371");
    unsigned waited = 0;
    for (;;) {                      /* madera_wait_for_boot: every 5 ms, 25 ms at most */
        if (c_rd(C_IRQ1_RAW_STATUS_1, &raw) == 0 && (raw & BOOT_DONE))
            break;
        if (waited >= 25000)
            break;
        H->delay_us(5000);
        waited += 5000;
    }
    (void)c_wr_raw(C_IRQ1_STATUS_1, BOOT_DONE);     /* BOOT_DONE starts unmasked: acknowledged, always */
    if (!(raw & BOOT_DONE)) {
        say("boot: BOOT_DONE not seen in 25 ms (IRQ1_RAW_STATUS_1 %04x)", raw);
        return fail(AUD_ETIMEDOUT, "the codec did not report BOOT_DONE");
    }
    if ((rc = c_rd(C_HARDWARE_REVISION, &rev)))
        return fail(rc, "the revision register could not be read");
    say("boot: done within %u ms (IRQ1_RAW_STATUS_1 %04x, acknowledged); silicon revision %u", waited / 1000, raw,
        rev & 0xFFU);
    probed = 1;
    inited = 0;
    why = "probed";
    return 0;
}

int codec_read(uint32_t reg, uint16_t *v)
{
    if (!hal_ok() || !probed)
        return AUD_ENOSYS;
    return c_rd(reg, v);
}

int codec_mic(int on)
{
    if (!hal_ok()) {
        why = "no HAL";
        return AUD_ENOSYS;
    }
    int rc;
    if (!on) {
        if (mic_on || fll_on) {
            mic_path_off();
            clkout_restore();
        } else {
            say("mic off: it was not on");
        }
        mic_on = 0;
        why = probed ? "mic off" : why;
        return 0;
    }
    if (mic_on)
        return 0;
    say("mic on: DMIC1/IN3L + DMIC4/IN4L to AIF1, an I2S slave of ABOX UAIF0, 48 kHz 2x16");
    if (!probed && (rc = codec_probe()))
        return rc;
    if (!inited && (rc = chip_init()))
        return fail(rc, "the after-boot setup (patch, MICBIAS, pins) failed");
    if ((rc = clocks_on()))
        return fail(rc, "the clocks (CLKOUT0, SYSCLK, FLL1) did not come up");
    mic_on = 1;
    if ((rc = mic_path_on()))
        return fail(rc, "the microphone path did not come up");
    why = "mic on";
    say("mic on: done");
    return 0;
}

void codec_off(void)
{
    if (!H)
        return;
    say("off");
    shut();
    why = "off";
}

int codec_selftest(void)
{
    say("selftest: probe (ID, BOOT_DONE), a few registers, then back in reset");
    int rc = codec_probe();
    if (rc)
        return rc;
    static const uint16_t regs[] = { C_SOFTWARE_RESET, C_HARDWARE_REVISION, C_SYSTEM_CLOCK_1, C_IRQ1_RAW_STATUS_1,
                                     C_IRQ1_RAW_STATUS_2, C_INPUT_ENABLES_STATUS };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i) {
        uint16_t v = 0;
        rc = c_rd(regs[i], &v);
        say("selftest: register %04x = %04x%s", regs[i], v, rc ? " (read failed)" : "");
    }
    codec_off();
    say("selftest: done");
    return 0;
}
