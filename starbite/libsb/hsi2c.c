/* SPDX-License-Identifier: GPL-2.0-only
 * Exynos USI HSI2C in auto mode, polled (shim/starlte/em.c bi_bus / bi_msg / bi_read and
 * menu.c tp_bus / tp_message, merged). Controller registers only: CTL 0x00, FIFO_CTL 0x04,
 * TRAILING_CTL 0x08, INT_EN 0x20, INT_STAT 0x24, FIFO_STAT 0x30, TX 0x34, RX 0x38, CONF 0x40,
 * AUTO_CONF 0x44, TIMEOUT 0x48, TRANS_STATUS 0x50, TIMING_FS1..3 0x60..0x68, ADDR 0x70, 0xC4.
 * The timing comes from what S-Boot (or the loader) left, saved in the sb_i2c before the first
 * controller reset.
 */
#include "sb.h"

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

void sb_i2c_bus(struct sb_i2c *b)
{
    uintptr_t B = b->base;
    wr(B + 0x20, 0);
    wr(B + 0xc4, 0);
    wr(B + 0x00, rd(B + 0x00) | 0x80000000U);     /* soft reset */
    wr(B + 0x00, rd(B + 0x00) & ~0x80000000U);
    wr(B + 0x68, b->fs3);
    wr(B + 0x64, b->fs2);
    wr(B + 0x60, b->fs1);
    wr(B + 0x00, 0x8U);                             /* master */
    wr(B + 0x08, 0x00ffffffU);
    wr(B + 0x48, rd(B + 0x48) & ~0x80000000U);
    wr(B + 0x40, rd(B + 0x40) | 0x80000000U);     /* auto mode */
    wr(B + 0x24, rd(B + 0x24) & 0xfffU);
}

int sb_i2c_msg(struct sb_i2c *b, unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop)
{
    uintptr_t B = b->base;
    int xfer_done = 0;
    unsigned done = 0, settle = 0;
    if (!count || count > 32)
        return 0;
    unsigned trig = count < 8 ? count : 8;
    wr(B + 0x04, 3U | (trig << 4) | (trig << 16));
    wr(B + 0x70, addr << 10);
    wr(B + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    wr(B + 0x20, 0x80U);
    wr(B + 0x44, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    wr(B + 0x44, rd(B + 0x44) | 0x80000000U);
    for (unsigned us = 0; us < 50000; ++us) {
        uint32_t irq = rd(B + 0x24);
        if (irq)
            wr(B + 0x24, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(rd(B + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)rd(B + 0x38);
        } else {
            while (done < count && !(rd(B + 0x30) & (1U << 7)))
                wr(B + 0x34, bytes[done++]);
        }
        if (!receive && !stop && done == count && (rd(B + 0x30) & (1U << 8)))
            ++settle;
        if (done == count && (xfer_done || (receive && stop) || settle > 200)) {
            wr(B + 0x20, 0);
            if (!stop)
                return 1;
            for (unsigned n = 0; n < 10000; ++n) {
                if (!(rd(B + 0x50) & (1U << 17)))
                    return 1;
                sb_udelay(1);
            }
            break;
        }
        sb_udelay(1);
    }
    wr(B + 0x20, 0);
    sb_i2c_bus(b);
    return 0;
}

/* Register read: repeated start (no STOP after the register), then STOP between the halves. */
int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    for (unsigned a = 0; a < 2; ++a) {
        if (sb_i2c_msg(b, addr, 0, &reg, 1, 0) && sb_i2c_msg(b, addr, 1, data, len, 1))
            return 1;
        sb_i2c_bus(b);
        if (sb_i2c_msg(b, addr, 0, &reg, 1, 1) && sb_i2c_msg(b, addr, 1, data, len, 1))
            return 1;
        sb_i2c_bus(b);
        sb_udelay(300);
    }
    return 0;
}
