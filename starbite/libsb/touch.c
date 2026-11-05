/* SPDX-License-Identifier: GPL-2.0-only
 * Touch: Samsung S6SY761 at 0x48 on HSI2C 0x104B0000 (USI03, pins GPP1-4/5 function 2), INT on
 * GPA1-0 active low (port of shim/starlte/menu.c tp_*, itself after bsd/dev/starlte_touch.c).
 *
 * Bring-up: the touch rails must already be on (the loader's menu turned them on; libsb only reads
 * them, with the kernel's checks), SYSREG_PERIC0 USI mode and the pin mux written only if they
 * differ, the controller reset with S-Boot's timing adjusted as the loader does, then BOOT_STATUS
 * 0x55 (0x20 = application), PANEL_INFO 0x23, SENSE_ON 0x10, CLEAR_EVENT_STACK 0x62. Events from
 * READ_ONE_EVENT 0x60 (8 bytes: EID [1:0] 0 = coordinate, TID [5:2] - 1, state [7:6] 1 press /
 * 2 move / 3 release, x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf).
 *
 * Unlike the loader, libsb never writes CMU_PERIC0 (0x10400000): its gates are read and shown
 * (sb_tp.why) if the controller does not answer. The loader opened them before Engineer OS ran.
 */
#include "sb.h"

#define TP_CMU 0x10400000UL
#define TP_SYSREG 0x10411000UL
#define TP_GPIO 0x10430000UL
#define TP_I2C 0x104b0000UL
#define TP_ALIVE 0x14050000UL
#define TP_ADDR 0x48U

struct sb_touch_info sb_tp;
static struct sb_i2c tp_bus_s;

static uint32_t mrd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void mwr_if(uintptr_t a, uint32_t v)
{
    if (mrd(a) == v)
        return;
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static int tp_message(int receive, uint8_t *bytes, unsigned count)
{
    sb_i2c_bus(&tp_bus_s);
    return sb_i2c_msg(&tp_bus_s, TP_ADDR, receive, bytes, count, 1);
}

static int tp_read(uint8_t reg, uint8_t *data, unsigned len, unsigned tries)
{
    for (unsigned a = 0; a < tries; ++a) {
        if (a)
            sb_udelay(200);
        if (tp_message(0, &reg, 1) && tp_message(1, data, len))
            return 1;
    }
    return 0;
}

static int tp_cmd(uint8_t reg)
{
    for (unsigned a = 0; a < 10; ++a) {
        if (a)
            sb_udelay(200);
        if (tp_message(0, &reg, 1))
            return 1;
    }
    return 0;
}

/* A gate in manual mode with its clock off: bits [21:20] = 01. */
static int gate_closed(uintptr_t a)
{
    return (mrd(a) & 0x00300000U) == 0x00100000U;
}

int sb_touch_start(void)
{
    uint8_t b[12];
    if (sb_tp.ok)
        return 1;
    sb_tp.max_x = sb_tp.max_y = 4096;
    int r = sb_touch_rails(&sb_tp.l35, &sb_tp.l43);
    if (r <= 0) {
        sb_tp.why = r ? "PMIC (ACPM) did not answer" : "touch rails off (Power > Touch rails)";
        return 0;
    }
    static const uint32_t gates[4] = { 0x2018, 0x206c, 0x20dc, 0x20e0 };
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(TP_CMU + gates[i])) {
            sb_tp.why = "CMU_PERIC0 gate closed (Engineer OS does not write CMU blocks)";
            return 0;
        }
    mwr_if(TP_SYSREG + 0x1c, 0x4U);                     /* USI03: I2C */
    mwr_if(TP_GPIO + 0x20, (mrd(TP_GPIO + 0x20) & ~0x00ff0000U) | 0x00220000U);
    mwr_if(TP_GPIO + 0x28, mrd(TP_GPIO + 0x28) & ~0x00ff0000U);
    mwr_if(TP_GPIO + 0x2c, mrd(TP_GPIO + 0x2c) & ~0x00ff0000U);
    __asm__ volatile("dsb sy" : : : "memory");
    tp_bus_s.base = TP_I2C;
    tp_bus_s.fs3 = (mrd(TP_I2C + 0x68) & ~0x00ff0000U) | (0x21U << 16);
    tp_bus_s.fs2 = (mrd(TP_I2C + 0x64) & ~0x000000ffU) | 0xe0U;
    tp_bus_s.fs1 = (mrd(TP_I2C + 0x60) & ~0x00ff0000U) | (0xf0U << 16);
    sb_i2c_bus(&tp_bus_s);
    if (!tp_read(0x55, b, 1, 10)) {
        sb_mdelay(150);                                 /* rails just switched on: firmware boot */
        if (!tp_read(0x55, b, 1, 10)) {
            sb_tp.why = "no answer on HSI2C 0x104B0000";
            return 0;
        }
    }
    sb_tp.status = b[0];
    if (b[0] != 0x20) {
        sb_tp.why = "controller not in application mode";
        return 0;
    }
    if (tp_read(0x23, b, 11, 10)) {
        unsigned mx = (unsigned)b[0] << 8 | b[1], my = (unsigned)b[2] << 8 | b[3];
        if (mx >= 1000 && my >= 1000) {
            sb_tp.max_x = mx;
            sb_tp.max_y = my;
        }
        sb_tp.tx = b[8];
        sb_tp.rx = b[9];
    }
    (void)tp_read(0x52, sb_tp.id, 3, 3);
    if (!tp_cmd(0x10)) {
        sb_tp.why = "SENSE_ON refused";
        return 0;
    }
    sb_mdelay(20);
    (void)tp_cmd(0x62);
    sb_tp.ok = 1;
    sb_tp.why = 0;
    sb_log("touch: S6SY761 up");
    return 1;
}

static int map_event(const uint8_t *e, int *id, int *st, int *x, int *y)
{
    if ((e[0] & 0x3U) != 0)
        return 0;
    int tid = (int)((e[0] >> 2) & 0xfU) - 1;
    unsigned state = e[0] >> 6;
    if (tid < 0 || state < 1)
        return 0;
    unsigned rx = (unsigned)e[1] << 4 | e[3] >> 4;
    unsigned ry = (unsigned)e[2] << 4 | (e[3] & 0xfU);
    *id = tid;
    *st = (int)state;
    *x = (int)((uint64_t)rx * SB_FB_W / sb_tp.max_x);
    *y = (int)((uint64_t)ry * SB_FB_H / sb_tp.max_y);
    return 1;
}

int sb_touch_event(int *st, int *x, int *y)
{
    uint8_t e[8];
    if (!sb_tp.ok || (mrd(TP_ALIVE + 0x44) & 1U))
        return 0;
    for (unsigned n = 0; n < 16; ++n) {
        int id;
        if (!tp_read(0x60, e, 8, 3) || e[0] == 0)
            return 0;
        if (map_event(e, &id, st, x, y) && id == 0)
            return 1;
    }
    return 0;
}

int sb_touch_events(int *id, int *st, int *x, int *y, int max)
{
    uint8_t e[8];
    int n = 0;
    if (!sb_tp.ok || (mrd(TP_ALIVE + 0x44) & 1U))
        return 0;
    for (unsigned k = 0; k < 32 && n < max; ++k) {
        if (!tp_read(0x60, e, 8, 3) || e[0] == 0)
            break;
        if (map_event(e, &id[n], &st[n], &x[n], &y[n]))
            ++n;
    }
    return n;
}

int sb_touch_read(uint8_t reg, uint8_t *data, unsigned len)
{
    return sb_tp.ok && tp_read(reg, data, len, 5);
}

void sb_touch_clear(void)
{
    if (sb_tp.ok)
        (void)tp_cmd(0x62);
}
