/* SPDX-License-Identifier: GPL-2.0-only
 * MAX77705 (charger, fuel gauge, USB-C) on HSI2C 0x14360000 (USI_CMGP03), read only: port of
 * shim/starlte/em.c bi_* (build/em-research-battery.md). PMIC 0x66, fuel gauge 0x36 (RepSOC 0x06
 * = %, VCELL 0x09, Current 0x0A), charger 0x69, USB-C 0x25.
 *
 * Never writes a MAX77705 register and never reads the clear-on-read interrupt registers
 * (PMIC 0x22 / 0x24, charger 0xB0). Unlike Engineer Lite it does not write CMU_CMGP's gates or
 * Q-channel (0x14200000): S-Boot drives this bus at every boot (charger, fuel-gauge model), so
 * the clocks run; a closed gate is reported instead. SYSREG_CMGP's USI mode and the GPM14/15 pin
 * mux are written only if they differ from I2C / function 2.
 */
#include "sb.h"

#define BI 0x14360000UL
#define BI_PMIC 0x66U
#define BI_FG 0x36U
#define BI_CHG 0x69U
#define BI_USBC 0x25U

struct sb_i2c sb_max_bus = { BI, 0, 0, 0, 0 };
static uint8_t bi_id, bi_rev;
const char *sb_max_why;

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr_if(uintptr_t a, uint32_t v)
{
    if (rd(a) == v)
        return;
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static int saved;

int sb_max_init(void)
{
    struct sb_i2c *b = &sb_max_bus;
    if (b->state > 0)
        return 1;
    static const uint32_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };
    for (unsigned i = 0; i < 4; ++i)
        if ((rd(0x14200000UL + gates[i]) & 0x00300000U) == 0x00100000U) {
            sb_max_why = "CMU_CMGP gate closed (Engineer OS does not write CMU blocks)";
            b->state = -1;
            return 0;
        }
    wr_if(0x14212030UL, 4U);                            /* USI_CMGP03 mode: I2C */
    for (uintptr_t g = 0x14220180UL; g <= 0x142201a0UL; g += 0x20) {
        wr_if(g, (rd(g) & ~0xfU) | 0x2U);
        wr_if(g + 8, rd(g + 8) & ~0xfU);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    if (!saved) {
        b->fs1 = rd(BI + 0x60);
        b->fs2 = rd(BI + 0x64);
        b->fs3 = rd(BI + 0x68);
        if (!((b->fs3 >> 16) & 0xffU)) {                /* nothing from S-Boot: 200 MHz, 400 kHz */
            b->fs3 = (b->fs3 & ~0x00ff0000U) | (0x21U << 16);
            b->fs2 = (b->fs2 & ~0xffU) | 0xe0U;
            b->fs1 = (b->fs1 & ~0x00ff0000U) | (0xf0U << 16);
        }
        saved = 1;
    }
    sb_i2c_bus(b);
    b->state = sb_i2c_read(b, BI_PMIC, 0x00, &bi_id, 1) ? 1 : -1;
    if (b->state > 0)
        (void)sb_i2c_read(b, BI_PMIC, 0x01, &bi_rev, 1);
    sb_max_why = b->state > 0 ? 0 : "no answer on HSI2C 0x14360000";
    return b->state > 0;
}

int sb_max_id(uint8_t *id, uint8_t *rev)
{
    *id = bi_id;
    *rev = bi_rev;
    return sb_max_bus.state > 0;
}

int sb_fg_word(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    if (!sb_max_init() || !sb_i2c_read(&sb_max_bus, BI_FG, reg, b, 2))
        return 0;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

int sb_chg_read(uint8_t reg, uint8_t *v)
{
    if (reg == 0xb0)                                    /* CHG_INT: clear on read */
        return 0;
    return sb_max_init() && sb_i2c_read(&sb_max_bus, BI_CHG, reg, v, 1);
}

int sb_usbc_read(uint8_t reg, uint8_t *v)
{
    return sb_max_init() && sb_i2c_read(&sb_max_bus, BI_USBC, reg, v, 1);
}

/* One sample for the status bar and the dashboard. */
int sb_bat_read(struct sb_bat *bt)
{
    uint16_t v;
    uint8_t c;
    bt->ok = 0;
    if (sb_max_bus.state < 0)
        sb_max_bus.state = 0;                           /* retry */
    if (!sb_max_init())
        return 0;
    if (!sb_fg_word(0x06, &v))
        return 0;
    bt->ok = 1;
    bt->soc_pct = v >> 8;
    bt->soc_x10 = (int)(v >> 8) * 10 + (int)((v & 0xffU) * 10U / 256U);
    bt->mv = sb_fg_word(0x09, &v) ? (int)((uint32_t)v * 5U / 64U) : 0;
    bt->avg_mv = sb_fg_word(0x19, &v) ? (int)((uint32_t)v * 5U / 64U) : 0;
    bt->ma = sb_fg_word(0x0a, &v) ? (int32_t)(int16_t)v * 5 / 16 : 0;
    bt->avg_ma = sb_fg_word(0x0b, &v) ? (int32_t)(int16_t)v * 5 / 16 : 0;
    bt->input = 0;
    if (sb_chg_read(0xb2, &c))
        bt->input = (c & 0x20U) ? 2 : (c & 0x40U) ? 1 : 0;
    bt->chg_dtls = sb_chg_read(0xb4, &c) ? (int)(c & 0xfU) : -1;
    return 1;
}
