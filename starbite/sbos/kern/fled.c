/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.6.3: the S2MPB02's flash LED over I2C (build/em-research-flash-led.md; Samsung's leds-s2mpb02.c,
 * LineageOS android_kernel_samsung_exynos9810, GPL-2.0, read for the register meanings only).
 *
 * The chip sits on HSI2C 0x10920000 (USI10, PERIC1) at 0x59. What this file may touch:
 * - read only: CMU_PERIC1's USI10 gates (a closed gate refuses: libsb never writes CMU blocks);
 * - SYSREG_PERIC1 0x1081103C (USI10 mode: I2C = 4) and gpp6 pins 0/1 (CON 0x10830040 bits 7:0 = 0x22, PUD bits 7:0
 *   = 0), each written only if it differs;
 * - the chip: FLED_CTRL1 0x12 bits 7:6 and 3 (00: the pins drive the LED; the low-voltage function off, as Samsung's
 *   setup), FLED_CUR1 0x14 (flash bits 7:4, torch bits 3:0), FLED_TIME1 0x15 bits 7:4 (the flash timer). Never
 *   FLED_CTRL2 0x13 or 0x16..0x1B (the iris IR LED), never 0x63, never the LDO / buck registers.
 * The values: never above what Samsung drives on this phone: torch 240 mA (0xC), flash 1250 mA (0xC) with a hardware
 * timer of at most 500 ms (7); the flash pulse here uses the 125 ms timer (1) and fires at most once a second.
 * The pins: torch1-gpio gpg2-4 (torch.c), flash1-gpio gpg3-5 (PERIC1 0x108300C0 bits 23:20, DAT 0x108300C4 bit 5);
 * never both high. */
#include "sb.h"
#include "kern.h"

#define BUS 0x10920000UL
#define ADDR 0x59U
#define R_CTRL1 0x12U
#define R_CUR1 0x14U
#define R_TIME1 0x15U
#define R_BST2 0x1dU
#define R_63 0x63U
#define FLASH_CON 0x108300C0UL
#define FLASH_DAT 0x108300C4UL
#define FLASH_PIN 5U
#define TORCH_MAX 0xcU                      /* 240 mA: Samsung's highest continuous value */
#define FLASH_MAX 0xcU                      /* 1250 mA: Samsung's camera flash */
#define FLASH_T125 1U                       /* (1 + 1) x 62.5 ms = 125 ms */

static struct sb_i2c bus = { BUS, 0, 0, 0, 0 };
static int st;                              /* 0 untried, 1 ready, -1 refused */
static const char *why = "not tried";
static uint64_t last_pulse;
static int pulse_on;

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void wr_if(uintptr_t a, uint32_t v)
{
    if (rd(a) == v)
        return;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int reg_rd(uint8_t r, uint8_t *v)
{
    return sb_i2c_read(&bus, ADDR, r, v, 1);
}

/* a masked register write, read back */
static int reg_upd(uint8_t r, uint8_t mask, uint8_t val)
{
    uint8_t old = 0, chk = 0;
    if (!reg_rd(r, &old))
        return 0;
    uint8_t nv = (uint8_t)((old & ~mask) | (val & mask));
    if (nv != old) {
        uint8_t m[2] = { r, nv };
        if (!sb_i2c_msg(&bus, ADDR, 0, m, 2, 1))
            return 0;
    }
    return reg_rd(r, &chk) && chk == nv;
}

static void flash_pin(int high)
{
    uint32_t c = rd(FLASH_CON);
    if (((c >> (4 * FLASH_PIN)) & 0xfU) != 1U)
        wr_if(FLASH_CON, (c & ~(0xfU << (4 * FLASH_PIN))) | (1U << (4 * FLASH_PIN)));     /* output */
    uint32_t d = rd(FLASH_DAT);
    wr_if(FLASH_DAT, high ? (d | (1U << FLASH_PIN)) : (d & ~(1U << FLASH_PIN)));
}

int fled_init(void)
{
    if (st)
        return st > 0;
    static const uint32_t gates[4] = { 0x2048, 0x20ac, 0x2114, 0x2118 };
    for (unsigned i = 0; i < 4; ++i)
        if ((rd(0x10800000UL + gates[i]) & 0x00300000U) == 0x00100000U) {
            st = -1;
            why = "CMU_PERIC1's USI10 gate is closed (not opened from here)";
            sb_logf("fled: refused: ", why);
            return 0;
        }
    uint32_t mode = rd(0x1081103CUL), con = rd(0x10830040UL), pud = rd(0x10830048UL);
    wr_if(0x1081103CUL, 4U);
    wr_if(0x10830040UL, (con & ~0xffU) | 0x22U);
    wr_if(0x10830048UL, pud & ~0xffU);
    bus.fs1 = rd(BUS + 0x60), bus.fs2 = rd(BUS + 0x64), bus.fs3 = rd(BUS + 0x68);
    if (!((bus.fs3 >> 16) & 0xffU)) {       /* nothing from S-Boot: Android's 200 MHz / 400 kHz timing */
        bus.fs1 = 0x01F0FF00U;
        bus.fs2 = 0x030003E0U;
        bus.fs3 = 0x00210000U;
    }
    sb_i2c_bus(&bus);
    uint8_t id = 0, c1 = 0, cur = 0, tim = 0, r63 = 0;
    int ok = reg_rd(R_BST2, &id) && reg_rd(R_CTRL1, &c1) && reg_rd(R_CUR1, &cur) && reg_rd(R_TIME1, &tim) &&
             reg_rd(R_63, &r63);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "fled: usi mode ");
    ub_hex(&u, mode, 2);
    ub_s(&u, ", gpp6 con ");
    ub_hex(&u, con & 0xffU, 2);
    ub_s(&u, "; S2MPB02 0x1D ");
    ub_hex(&u, id, 2);
    ub_s(&u, " CTRL1 ");
    ub_hex(&u, c1, 2);
    ub_s(&u, " CUR1 ");
    ub_hex(&u, cur, 2);
    ub_s(&u, " TIME1 ");
    ub_hex(&u, tim, 2);
    ub_s(&u, " 0x63 ");
    ub_hex(&u, r63, 2);
    sb_log(u.b);
    if (!ok) {
        st = -1, why = "the LED chip does not answer on I2C";
    } else if (id != 0x90U) {
        st = -1, why = "the LED chip is not the expected S2MPB02 (0x1D)";
    } else if (r63 != 0x7fU) {
        st = -1, why = "the LED chip's register 0x63 is not its default (a continuous flash mode?)";
    } else if (!reg_upd(R_CTRL1, 0xc8U, 0x00U)) {
        st = -1, why = "the LED chip did not take its pin mode";
    } else {
        st = 1, why = 0;
    }
    if (st < 0)
        sb_logf("fled: refused: ", why);
    return st > 0;
}

const char *fled_why(void)
{
    return why;
}

/* the torch's current: 240 mA, Samsung's highest continuous value (torch.c switches the pin) */
int fled_torch_max(void)
{
    if (!fled_init())
        return 0;
    int ok = reg_upd(R_CUR1, 0x0fU, TORCH_MAX);
    sb_log(ok ? "fled: torch current 240 mA (Samsung's highest continuous)" : "fled: the torch current did not read back");
    return ok;
}

/* one flash pulse at 1250 mA, ended by the chip's 125 ms timer; at most one a second; never with the torch on */
int fled_pulse(void)
{
    uint64_t now = sb_ms();
    if (!fled_init() || torch_on() || now - last_pulse < 1000U)
        return 0;
    static int set;
    if (!set) {
        if (!reg_upd(R_CUR1, 0xf0U, (uint8_t)(FLASH_MAX << 4)) || !reg_upd(R_TIME1, 0xf0U, (uint8_t)(FLASH_T125 << 4))) {
            sb_log("fled: the flash current or timer did not read back: no pulses");
            st = -1, why = "the flash settings did not read back";
            return 0;
        }
        set = 1;
        sb_log("fled: flash pulses: 1250 mA, the chip's 125 ms timer, at most one a second");
    }
    last_pulse = now;
    flash_pin(1);
    pulse_on = 1;
    kt_set(KT_UI, 7, now, 130, 0);          /* the pin low again after the timer (fled_timer) */
    return 1;
}

void fled_timer(void)
{
    if (pulse_on) {
        flash_pin(0);
        pulse_on = 0;
    }
}

void fled_off(void)
{
    if (st > 0 || pulse_on)
        flash_pin(0);
    pulse_on = 0;
}
