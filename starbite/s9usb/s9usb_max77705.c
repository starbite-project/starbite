/* SPDX-License-Identifier: GPL-2.0-only
 * s9usb: the MAX77705's Type-C MCU ("USBC", I2C 0x25) over HSI2C 0x14360000 (USI_CMGP03).
 *
 * The bus code and the opcode protocol are starbite/utility/max77705_muic.c (EngineerMode's
 * HSI2C driver; proven in the loader and the Utility OS) with the registers reached through
 * struct s9usb_plat. Opcode protocol: opcode + data into AP_DATAOUT0.. (0x21..), a write to
 * AP_DATAOUT32 (0x41) ends the command, the answer is in AP_DATAIN0.. (0x51..). CONTROL1 =
 * COM_USB (0x09) puts D+/D- on the AP (Samsung's MUIC com_to_usb_ap); opcode 0x54 is the MCU's
 * own VBUS control (Samsung's max77705_usbc_{en,dis}able_auto_vbus: 0x02 on, 0x00 off).
 * Never reads the clear-on-read interrupt registers (0x02..0x05).
 */
#include "s9usb_io.h"

#define A_USBC 0x25U

#define R_HW_REV       0x00U
#define R_FW_REV       0x01U
#define R_USBC_STATUS1 0x06U
#define R_BC_STATUS    0x08U
#define R_CC_STATUS0   0x0aU
#define R_AP_DATAOUT0  0x21U
#define R_AP_DATAOUT32 0x41U
#define R_AP_DATAIN0   0x51U
#define OP_CONTROL1_READ  0x05U
#define OP_CONTROL1_WRITE 0x06U
#define OP_AUTO_VBUS      0x54U
#define COM_USB 0x09U

/* HSI2C registers (offsets in S9_HSI2C) */
#define I2C_CTL        0x00U
#define I2C_FIFO_CTL   0x04U
#define I2C_INT_EN     0x20U
#define I2C_INT_STAT   0x24U
#define I2C_FIFO_STAT  0x30U
#define I2C_TX_DATA    0x34U
#define I2C_RX_DATA    0x38U
#define I2C_CONF       0x40U
#define I2C_AUTO_CONF  0x44U
#define I2C_TIMEOUT    0x48U
#define I2C_TRANS_STAT 0x50U
#define I2C_TIMING1    0x60U
#define I2C_TIMING2    0x64U
#define I2C_TIMING3    0x68U
#define I2C_ADDR       0x70U
#define I2C_USI_CON    0xc4U

static void bus_reset(const struct s9usb_plat *p, const struct s9usb_max *m)
{
    const enum s9usb_blk b = S9_HSI2C;
    s9_wr(p, b, I2C_INT_EN, 0);
    s9_wr(p, b, I2C_USI_CON, 0);
    s9_rmw(p, b, I2C_CTL, 0, 0x80000000U);
    s9_rmw(p, b, I2C_CTL, 0x80000000U, 0);
    s9_wr(p, b, I2C_TIMING3, m->fs[2]);
    s9_wr(p, b, I2C_TIMING2, m->fs[1]);
    s9_wr(p, b, I2C_TIMING1, m->fs[0]);
    s9_wr(p, b, I2C_CTL, 0x8U);
    s9_wr(p, b, 0x08U, 0x00ffffffU);
    s9_rmw(p, b, I2C_TIMEOUT, 0x80000000U, 0);
    s9_rmw(p, b, I2C_CONF, 0, 0x80000000U);
    s9_wr(p, b, I2C_INT_STAT, s9_rd(p, b, I2C_INT_STAT) & 0xfffU);
}

/* One auto-mode message (count <= 32), with or without STOP (EngineerMode's bi_msg). */
static int msg(const struct s9usb_plat *p, struct s9usb_max *m, unsigned addr, int receive,
               uint8_t *bytes, unsigned count, int stop)
{
    const enum s9usb_blk b = S9_HSI2C;
    int xfer_done = 0;
    unsigned done = 0, settle = 0;
    unsigned trig = count < 8 ? count : 8;

    s9_wr(p, b, I2C_FIFO_CTL, 3U | (trig << 4) | (trig << 16));
    s9_wr(p, b, I2C_ADDR, addr << 10);
    s9_wr(p, b, I2C_CTL, 0x8U | (receive ? 0x40U : 0x80U));
    s9_wr(p, b, I2C_INT_EN, 0x80U);
    s9_wr(p, b, I2C_AUTO_CONF, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    s9_rmw(p, b, I2C_AUTO_CONF, 0, 0x80000000U);
    for (unsigned us = 0; us < 50000; ++us) {
        uint32_t irq = s9_rd(p, b, I2C_INT_STAT);
        if (irq)
            s9_wr(p, b, I2C_INT_STAT, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(s9_rd(p, b, I2C_FIFO_STAT) & (1U << 24)))
                bytes[done++] = (uint8_t)s9_rd(p, b, I2C_RX_DATA);
        } else {
            while (done < count && !(s9_rd(p, b, I2C_FIFO_STAT) & (1U << 7)))
                s9_wr(p, b, I2C_TX_DATA, bytes[done++]);
        }
        if (!receive && !stop && done == count && (s9_rd(p, b, I2C_FIFO_STAT) & (1U << 8)))
            ++settle;
        if (done == count && (xfer_done || (receive && stop) || settle > 200)) {
            s9_wr(p, b, I2C_INT_EN, 0);
            if (!stop)
                return 1;
            for (unsigned n = 0; n < 10000; ++n) {
                if (!(s9_rd(p, b, I2C_TRANS_STAT) & (1U << 17)))
                    return 1;
                p->udelay(1);
            }
            break;
        }
        p->udelay(1);
    }
    s9_wr(p, b, I2C_INT_EN, 0);
    bus_reset(p, m);
    ++m->errors;
    return 0;
}

static int i2c_read(const struct s9usb_plat *p, struct s9usb_max *m, uint8_t reg, uint8_t *data,
                    unsigned len)
{
    for (unsigned a = 0; a < 2; ++a) {
        if (msg(p, m, A_USBC, 0, &reg, 1, 0) && msg(p, m, A_USBC, 1, data, len, 1))
            return 1;
        bus_reset(p, m);
        if (msg(p, m, A_USBC, 0, &reg, 1, 1) && msg(p, m, A_USBC, 1, data, len, 1))
            return 1;
        bus_reset(p, m);
        p->udelay(300);
    }
    return 0;
}

/* reg, then n data bytes, one message with STOP */
static int i2c_write(const struct s9usb_plat *p, struct s9usb_max *m, uint8_t reg,
                     const uint8_t *data, unsigned n)
{
    uint8_t buf[33];
    if (n > 32)
        return 0;
    buf[0] = reg;
    for (unsigned i = 0; i < n; ++i)
        buf[1 + i] = data[i];
    for (unsigned a = 0; a < 2; ++a) {
        if (msg(p, m, A_USBC, 0, buf, n + 1, 1))
            return 1;
        p->udelay(300);
    }
    return 0;
}

int s9usb_max_init(const struct s9usb_plat *p, struct s9usb_max *m)
{
    /* CMU_CMGP gates (MANUAL + CG_VAL) and QCH for USI_CMGP03, USI mode I2C, GPM14/15 as I2C,
     * pulls off (EngineerMode's em.c, the loader's max77705_muic.c) */
    static const uint16_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };
    for (unsigned i = 0; i < 4; ++i)
        s9_rmw(p, S9_CMU_CMGP, gates[i], 0, 0x00300000U);
    s9_rmw(p, S9_CMU_CMGP, 0x3050U, 0, 3U);
    s9_wr(p, S9_SYSREG_CMGP, 0x2030U, 4U);
    for (uint32_t g = 0x180U; g <= 0x1a0U; g += 0x20U) {
        s9_rmw(p, S9_GPIO_CMGP, g, 0xfU, 0x2U);
        s9_rmw(p, S9_GPIO_CMGP, g + 8U, 0xfU, 0);
    }
    S9_DSB();
    if (!m->bus) {
        /* S-Boot's timing, or 400 kHz at the 200 MHz source when it left none */
        m->fs[0] = s9_rd(p, S9_HSI2C, I2C_TIMING1);
        m->fs[1] = s9_rd(p, S9_HSI2C, I2C_TIMING2);
        m->fs[2] = s9_rd(p, S9_HSI2C, I2C_TIMING3);
        if (!((m->fs[2] >> 16) & 0xffU)) {
            m->fs[2] = (m->fs[2] & ~0x00ff0000U) | (0x21U << 16);
            m->fs[1] = (m->fs[1] & ~0xffU) | 0xe0U;
            m->fs[0] = (m->fs[0] & ~0x00ff0000U) | (0xf0U << 16);
        }
    }
    bus_reset(p, m);
    m->bus = i2c_read(p, m, R_HW_REV, &m->hw, 1) ? 1 : -1;
    if (m->bus > 0)
        (void)i2c_read(p, m, R_FW_REV, &m->fw, 1);
    m->ctrl1 = -1;
    return m->bus > 0;
}

int s9usb_max_status(const struct s9usb_plat *p, struct s9usb_max *m)
{
    if (m->bus <= 0)
        return 0;
    return i2c_read(p, m, R_USBC_STATUS1, &m->usbc1, 1) && i2c_read(p, m, R_BC_STATUS, &m->bc, 1) &&
           i2c_read(p, m, R_CC_STATUS0, &m->cc0, 1);
}

/* max77705_i2c_opcode_write(): opcode + data, then the end marker */
static int opcode_write(const struct s9usb_plat *p, struct s9usb_max *m, uint8_t op,
                        const uint8_t *data, unsigned n)
{
    uint8_t buf[4];
    uint8_t z = 0;
    buf[0] = op;
    for (unsigned i = 0; i < n && i < 3; ++i)
        buf[1 + i] = data[i];
    if (!i2c_write(p, m, R_AP_DATAOUT0, buf, 1 + (n < 3 ? n : 3)))
        return 0;
    return i2c_write(p, m, R_AP_DATAOUT32, &z, 1);
}

/* the answer to `op` (AP_DATAIN0 echoes the opcode), polled for 100 ms */
static int opcode_answer(const struct s9usb_plat *p, struct s9usb_max *m, uint8_t op)
{
    for (unsigned i = 0; i < 10; ++i) {
        p->udelay(10000);
        if (i2c_read(p, m, R_AP_DATAIN0, m->answer, 4) && m->answer[0] == op)
            return 1;
    }
    return 0;
}

int s9usb_max_route_usb(const struct s9usb_plat *p, struct s9usb_max *m)
{
    uint8_t v = COM_USB;
    if (m->bus <= 0)
        return 0;
    for (unsigned a = 0; a < 4; ++a) {
        if (opcode_write(p, m, OP_CONTROL1_WRITE, &v, 1)) {
            p->udelay(50000);
            m->ctrl1 = (opcode_write(p, m, OP_CONTROL1_READ, 0, 0) &&
                        opcode_answer(p, m, OP_CONTROL1_READ)) ? m->answer[1] : -1;
            if (m->ctrl1 == COM_USB)
                return 1;
        }
        p->udelay(200000);
    }
    return 0;
}

int s9usb_max_auto_vbus(const struct s9usb_plat *p, struct s9usb_max *m, int on)
{
    uint8_t v = on ? 0x02U : 0x00U;
    if (m->bus <= 0)
        return 0;
    return opcode_write(p, m, OP_AUTO_VBUS, &v, 1) && opcode_answer(p, m, OP_AUTO_VBUS);
}
