/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.7 (sound), step 1: a read-only probe of the S9's audio codec (Cirrus Logic CS47L93, "Madera").
 *
 * From the vendor DT (device/dtb/starlte.dts ~7392-7440) and Samsung's exynos9810.dtsi (~2526, ~1244):
 * - the codec on spi_2 = SPI 0x10470000 (USI01 in PERIC0; USI mode register SYSREG_PERIC0 0x1041100C, "spi"),
 *   chip select 0, at most 25 MHz; pins gpp0-4/5/6 (CLK, MISO, MOSI: function 2) and gpp0-7 (CS: function 2);
 * - reset: gpg1-0 (PERIC0 pinctrl 0x10430000, bank +0xA0); supplies: AVDD / DBVDD1 / CPVDD1 = LDO32 (1.8 V,
 *   always on), CPVDD2 / DCVDD = LDO34 (1.2 V, NOT always on: S2MPS18 L34CTRL 0x6B, enable bits 7:6);
 * - the loudspeaker amplifier: MAX98512 at 0x39 on HSI2C 0x108C0000 (later).
 *
 * Tier A (always, reads only): CMU_PERIC0's USI01 gates, the USI mode, the SPI controller's registers, the pins, the
 * reset pin, LDO32 / LDO34. Tier B (one SPI read of the codec's ID, register 0x0000, and its revision 0x0001): only
 * when S-Boot left the codec powered (LDO34 on) and out of reset (gpg1-0 an output, high) and the USI is in SPI mode
 * with the pins in their SPI functions and the gates open. It then writes only the SPI controller's own registers
 * (a reset of the block, mode 0, 8-bit, the chip select by register); never the codec, the PMIC, the pins or the
 * CMU. Madera over SPI (regmap: 32-bit address, bit 31 = read, 16 bits of padding, 16-bit data). */
#include "sb.h"
#include "kern.h"

#define SPI 0x10470000UL
#define CH_CFG 0x00U
#define MODE_CFG 0x08U
#define CS_REG 0x0cU
#define STATUS 0x14U
#define TX_DATA 0x18U
#define RX_DATA 0x1cU
#define PENDING_CLR 0x24U
#define PERIC0_PIN 0x10430000UL
#define CMU_PERIC0 0x10400000UL
#define USI01_MODE 0x1041100CUL

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static unsigned rx_level(void)
{
    return (rd(SPI + STATUS) >> 15) & 0x1ffU;   /* RX_FIFO_LVL [23:15] (64-entry FIFO) */
}

/* one full-duplex transfer of n bytes, polled, 2 ms at most per byte; 1 done */
static int xfer(const uint8_t *tx, uint8_t *rxb, unsigned n)
{
    wr(SPI + CH_CFG, 1U << 5);                  /* SW_RST */
    wr(SPI + CH_CFG, 0);
    wr(SPI + MODE_CFG, 0);                      /* 8-bit channel and bus, no DMA */
    wr(SPI + PENDING_CLR, 0x1fU);
    wr(SPI + CS_REG, 0);                        /* manual chip select, asserted (NSSOUT 0) */
    wr(SPI + CH_CFG, 3U);                       /* TX and RX on, mode 0 */
    int ok = 1;
    for (unsigned i = 0; i < n && ok; ++i) {
        wr(SPI + TX_DATA, tx[i]);
        unsigned t = 0;
        while (!rx_level() && t < 2000) {
            sb_udelay(1);
            ++t;
        }
        if (!rx_level())
            ok = 0;
        else
            rxb[i] = (uint8_t)rd(SPI + RX_DATA);
    }
    wr(SPI + CS_REG, 1);                        /* released */
    wr(SPI + CH_CFG, 0);
    return ok;
}

/* the codec's 16-bit register r (Madera: 4 address bytes with the read bit, 2 padding bytes, 2 data bytes) */
static int codec_read(uint32_t r, uint16_t *v)
{
    uint8_t tx[8] = { (uint8_t)(0x80U | (r >> 24)), (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r, 0, 0, 0, 0 };
    uint8_t rxb[8] = { 0 };
    if (!xfer(tx, rxb, 8))
        return 0;
    *v = (uint16_t)(rxb[6] << 8 | rxb[7]);
    return 1;
}

/* 1 the probe ran (whatever it found), 0 a read failed */
int audio_probe(void)
{
    struct ub u;
    static const uint32_t gates[4] = { 0x2010, 0x205c, 0x20bc, 0x20c0 };
    uint32_t g[4];
    int closed = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!sb_probe32(CMU_PERIC0 + gates[i], &g[i]))
            return 0;
        closed |= (g[i] & 0x00300000U) == 0x00100000U;
    }
    uint32_t mode = 0, con0 = 0, cong1 = 0, datg1 = 0, ch = 0, cs = 0, st = 0;
    int ok = sb_probe32(USI01_MODE, &mode) && sb_probe32(PERIC0_PIN + 0x00, &con0) &&
             sb_probe32(PERIC0_PIN + 0xa0, &cong1) && sb_probe32(PERIC0_PIN + 0xa4, &datg1) &&
             sb_probe32(SPI + CH_CFG, &ch) && sb_probe32(SPI + CS_REG, &cs) && sb_probe32(SPI + STATUS, &st);
    uint8_t l32 = 0, l34 = 0;
    int pm = sb_pmic_read(1, 0x69, &l32) && sb_pmic_read(1, 0x6b, &l34);
    ub_init(&u);
    ub_s(&u, "audio: gates ");
    for (unsigned i = 0; i < 4; ++i) {
        ub_hex(&u, g[i] >> 20 & 3U, 1);
        ub_c(&u, i < 3 ? '/' : ' ');
    }
    ub_s(&u, "usi01 mode ");
    ub_hex(&u, mode, 2);
    ub_s(&u, ", gpp0 con ");
    ub_hex(&u, con0, 8);
    ub_s(&u, ", reset gpg1-0 con ");
    ub_hex(&u, cong1 & 0xfU, 1);
    ub_s(&u, " dat ");
    ub_hex(&u, datg1 & 1U, 1);
    ub_s(&u, ", spi ch ");
    ub_hex(&u, ch, 8);
    ub_s(&u, " cs ");
    ub_hex(&u, cs, 2);
    ub_s(&u, " st ");
    ub_hex(&u, st, 8);
    ub_s(&u, "; LDO32 ");
    ub_hex(&u, l32, 2);
    ub_s(&u, " LDO34 ");
    ub_hex(&u, l34, 2);
    sb_log(u.b);
    if (!ok || !pm)
        return 0;
    const char *skip = 0;
    if (closed)
        skip = "a USI01 clock gate is closed";
    else if ((mode & 0x7U) != 2U)
        skip = "USI01 is not in SPI mode (S-Boot did not set it up)";
    else if (((con0 >> 16) & 0xffffU) != 0x2222U)
        skip = "the SPI pins are not in their SPI functions";
    else if (!(l34 & 0xc0U))
        skip = "the codec's 1.2 V core supply (LDO34) is off";
    else if ((cong1 & 0xfU) != 1U || !(datg1 & 1U))
        skip = "the codec is held in reset (gpg1-0)";
    if (skip) {
        sb_logf("audio: no ID read: ", skip);
        sb_log("audio: powering and configuring the codec is a later step, with the user present");
        return 1;
    }
    uint16_t id = 0, rev = 0;
    if (!codec_read(0x0000, &id) || !codec_read(0x0001, &rev)) {
        sb_log("audio: the SPI transfer did not complete");
        return 1;
    }
    ub_init(&u);
    ub_s(&u, "audio: codec ID ");
    ub_hex(&u, id, 4);
    ub_s(&u, id == 0x6371U ? " (CS47L93 / Madera family?)" : id == 0x0000U || id == 0xffffU ? " (no answer)" : "");
    ub_s(&u, ", revision ");
    ub_hex(&u, rev, 4);
    sb_log(u.b);
    return 1;
}
