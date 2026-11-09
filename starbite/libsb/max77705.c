/* SPDX-License-Identifier: GPL-2.0-only
 * MAX77705 (charger, fuel gauge, USB-C) on HSI2C 0x14360000 (USI_CMGP03), read freely; written only
 * through sb_max_write with a confirmed authorisation (the charger watchdog feed, tier 1; any other
 * register, tier 3: engineer/policy.c max_tier). Port of
 * shim/starlte/em.c bi_* (build/em-research-battery.md). PMIC 0x66, fuel gauge 0x36 (RepSOC 0x06
 * = %, VCELL 0x09, Current 0x0A), charger 0x69, USB-C 0x25.
 *
 * Writes nothing on its own and never reads the clear-on-read interrupt registers
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

struct sb_fg_stat sb_fg_stat = { 0, 0, 0, 0, -1 };

int sb_fg_word_checked(uint8_t reg, uint16_t *v, unsigned lo, unsigned hi)
{
    uint16_t w = 0;
    int bad = 0;
    ++sb_fg_stat.reads;
    for (unsigned a = 0; a < 3; ++a) {
        if (a)
            ++sb_fg_stat.retries;
        if (!sb_fg_word(reg, &w)) {
            sb_udelay(200);
            continue;
        }
        if (lo < hi && (w < lo || w > hi)) {
            bad = 1;
            sb_fg_stat.last_bad = w;
            sb_udelay(200);
            continue;
        }
        *v = w;
        return 1;
    }
    if (bad)
        ++sb_fg_stat.rejected;
    else
        ++sb_fg_stat.failed;
    return 0;
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

/* A register write (2-byte message reg, value, STOP), with the gate: the caller's policy
 * (engineer/policy.c max_tier) gives the tier, at least 1; an open authorisation must cover
 * SB_SP_MAX addr7 << 8 | reg. Never CHG_INT (0xB0), read back unless the register is write-only. */
int sb_max_write(unsigned addr7, uint8_t reg, uint8_t val, int tier)
{
    const char *why;
    uint8_t m[2] = { reg, val }, back = 0;
    if (addr7 > 0x7f || sb_gate_check(SB_SP_MAX, 0, (uint64_t)addr7 << 8 | reg, 1, tier < SB_TIER1 ? SB_TIER1 : tier,
                                      &why)) {
        sb_logx("max77705 write refused, reg ", (uint64_t)addr7 << 8 | reg);
        return 0;
    }
    if (!sb_max_init())
        return 0;
    int ok = sb_i2c_msg(&sb_max_bus, addr7, 0, m, 2, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "max77705 write ");
    ub_hex(&u, (uint64_t)addr7 << 8 | reg, 4);
    ub_s(&u, " = ");
    ub_hex(&u, val, 2);
    if (ok && sb_i2c_read(&sb_max_bus, addr7, reg, &back, 1)) {
        ub_s(&u, ", reads back ");
        ub_hex(&u, back, 2);
    } else if (!ok) {
        ub_s(&u, ": no ACK");
    }
    sb_log(u.b);
    return ok;
}

/* 1.2-alpha18: a fuel-gauge word (16 bits, little-endian), with the gate (SB_SP_MAX 0x36 << 8 | reg), read back.
 * Starbite OS uses it only to clear the min / max trackers (MaxMinVolt 0x1B, MaxMinCurr 0x1C) at a Survival start. */
int sb_fg_write16(uint8_t reg, uint16_t val, int tier)
{
    const char *why;
    uint8_t m[3] = { reg, (uint8_t)val, (uint8_t)(val >> 8) };
    if (sb_gate_check(SB_SP_MAX, 0, (uint64_t)BI_FG << 8 | reg, 1, tier < SB_TIER1 ? SB_TIER1 : tier, &why)) {
        sb_logx("fuel gauge write refused, reg ", reg);
        return 0;
    }
    if (!sb_max_init())
        return 0;
    int ok = sb_i2c_msg(&sb_max_bus, BI_FG, 0, m, 3, 1);
    uint16_t back = 0;
    return ok && sb_fg_word(reg, &back) && back == val;
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
    if (!sb_fg_word_checked(0x06, &v, 0, 0x6500))                 /* RepSOC <= 101 % */
        return 0;
    bt->soc_pct = v >> 8;
    bt->soc_x10 = (int)(v >> 8) * 10 + (int)((v & 0xffU) * 10U / 256U);
    /* VCELL 2.5..4.7 V (78.125 uV LSB): a failed or impossible read is retried, then the whole
     * sample is "not read" instead of 0 mV (which the battery tests took for an empty cell) */
    if (!sb_fg_word_checked(0x09, &v, 32000, 60160))
        return 0;
    bt->ok = 1;
    bt->mv = (int)((uint32_t)v * 5U / 64U);
    bt->avg_mv = sb_fg_word_checked(0x19, &v, 32000, 60160) ? (int)((uint32_t)v * 5U / 64U) : bt->mv;
    bt->ma = sb_fg_word_checked(0x0a, &v, 0, 0) ? (int32_t)(int16_t)v * 5 / 16 : 0;
    bt->avg_ma = sb_fg_word_checked(0x0b, &v, 0, 0) ? (int32_t)(int16_t)v * 5 / 16 : bt->ma;
    bt->input = 0;
    if (sb_chg_read(0xb2, &c))
        bt->input = (c & 0x20U) ? 2 : (c & 0x40U) ? 1 : 0;
    bt->chg_dtls = sb_chg_read(0xb4, &c) ? (int)(c & 0xfU) : -1;
    return 1;
}
