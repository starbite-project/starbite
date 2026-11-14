/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the iris scanner's probes: P0 read only (levels 0-1), P1 attended (level 2, kern/iris.c) (irisprobe.h;
 * docs/drivers/IRIS.md). Facts from this phone's
 * device tree (build/linux-bundle/lts.dts) and Samsung's GPL kernel (s2mps18-private.h, leds-s2mpb02.c,
 * fimc-is-device-module-5f1.c), read as facts only. Levels 0-1 write nothing to any iris part. */
#include "irisprobe.h"

void iris_ldo_decode(uint8_t ctrl, struct iris_ldo *o)
{
    o->on = (ctrl >> 6) & 3U;
    o->code = ctrl & 0x3FU;
}

void iris_pin_decode(uint32_t con, uint32_t dat, uint32_t pud, unsigned pin, struct iris_pin *o)
{
    o->func = (con >> (4U * pin)) & 0xFU;
    o->level = (dat >> pin) & 1U;
    o->pud = (pud >> (4U * pin)) & 0xFU;
}

void iris_irled_decode(const uint8_t r[7], struct iris_irled *o)
{
    o->ctrl2 = r[0], o->cur2 = r[1], o->time2 = r[2];
    o->cur_code = (r[1] >> 4) + 1U;
    o->width = (unsigned)r[3] << 2 | ((unsigned)r[4] >> 6);
    o->delay = (unsigned)r[5] << 2 | ((unsigned)r[6] >> 6);
    o->max_timer_on = r[2] & 1U;
    o->max_time = (r[4] & 0x1FU) + 1U;
    o->discharge = (r[6] >> 1) & 1U;
}

#ifndef IRISPROBE_HOST
#include "sb.h"
#include "iris.h"

int fled_reg_read(uint8_t r, uint8_t *v);       /* fled.c */

#define PERIC0_GPIO 0x10430000UL
#define PERIC1_GPIO 0x10830000UL
#define GPG0 (PERIC0_GPIO + 0x80U)              /* iris_en: gpg0-7 (DT gpio_iris_en <0x4d 7>) */
#define GPG2 (PERIC0_GPIO + 0xC0U)              /* reset: gpg2-3 (gpio_reset <0x7e 3>); gpg2-4 the torch, gpg2-5 the front camera */
#define GPC1 (PERIC1_GPIO + 0x80U)              /* MCLK: gpc1-6 (gpio_mclk <0x7d 6>) */
#define GPP4 (PERIC1_GPIO + 0x00U)              /* the sensor's I2C: gpp4-6 SCL / gpp4-7 SDA (hsi2c@108D0000) */
#define USI07_MODE 0x10811024UL                 /* usi@10811024 ("i2c": USI07) */
#define USI_NEXT_MODE 0x10811028UL              /* usi@10811028 (disabled in the DT): logged for the record */
#define PMU_ISPPRE_STAT 0x1406404CUL            /* pd-isppre status [3:0] = 0xF on (cam.c) */
#define CSIS3 0x16260000UL

static int plevel = -1;

static void pin_line(struct ub *u, const char *name, uintptr_t bank, unsigned pin)
{
    struct iris_pin p;
    iris_pin_decode(*(volatile uint32_t *)bank, *(volatile uint32_t *)(bank + 4U), *(volatile uint32_t *)(bank + 8U),
                    pin, &p);
    ub_s(u, " ");
    ub_s(u, name);
    ub_s(u, " f");
    ub_dec(u, p.func);
    ub_s(u, "/");
    ub_dec(u, p.level);
    ub_s(u, "/p");
    ub_dec(u, p.pud);
}

static int level0(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: iris: LDO39 VDDD / LDO40 VDDA / LDO41 VDDIO:");
    static const uint32_t regs[3] = { IRIS_L39CTRL, IRIS_L40CTRL, IRIS_L41CTRL };
    unsigned any_on = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t v = 0;
        struct iris_ldo l;
        if (!sb_pmic_read(1, regs[i], &v)) {
            ub_s(&u, " ?");
            continue;
        }
        iris_ldo_decode(v, &l);
        any_on |= l.on;
        ub_s(&u, l.on ? " on(" : " off(");
        ub_hex(&u, v, 2);
        ub_s(&u, ")");
    }
    sb_log(u.b);

    ub_init(&u);
    ub_s(&u, "drvprobe: iris: pins (function/level/pull):");
    pin_line(&u, "iris_en gpg0-7", GPG0, 7);
    pin_line(&u, "reset gpg2-3", GPG2, 3);
    pin_line(&u, "MCLK gpc1-6", GPC1, 6);
    pin_line(&u, "SCL gpp4-6", GPP4, 6);
    pin_line(&u, "SDA gpp4-7", GPP4, 7);
    sb_log(u.b);

    uint32_t isp = *(volatile uint32_t *)PMU_ISPPRE_STAT, m7 = *(volatile uint32_t *)USI07_MODE,
             m8 = *(volatile uint32_t *)USI_NEXT_MODE;
    ub_init(&u);
    ub_s(&u, "drvprobe: iris: USI07 mode ");
    ub_hex(&u, m7, 8);
    ub_s(&u, ", usi@10811028 ");
    ub_hex(&u, m8, 8);
    ub_s(&u, ", ISPPRE ");
    ub_s(&u, (isp & 0xFU) == 0xFU ? "on" : "off");
    if ((isp & 0xFU) == 0xFU) {                 /* only inside a powered domain; fault-safe */
        uint32_t c0 = 0, c4 = 0;
        int f0 = sb_probe32(CSIS3, &c0), f4 = sb_probe32(CSIS3 + 4U, &c4);
        ub_s(&u, ": CSIS3 +0 ");
        if (f0)
            ub_hex(&u, c0, 8);
        else
            ub_s(&u, "FAULT");
        ub_s(&u, " +4 ");
        if (f4)
            ub_hex(&u, c4, 8);
        else
            ub_s(&u, "FAULT");
        ub_s(&u, f0 && f4 ? " (readable)" : " (the secure world holds CSIS3?)");
    } else {
        ub_s(&u, " (CSIS3 not read: its domain is off; P1 powers it, attended)");
    }
    sb_log(u.b);
    sb_log(any_on ? "drvprobe: iris: PASS read only; an iris supply is on from before (logged above)"
                  : "drvprobe: iris: PASS read only; the iris supplies are off, as expected");
    return 1;
}

static int level1(void)
{
    static const uint8_t at[7] = { 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };
    uint8_t r[7];
    for (unsigned i = 0; i < 7; ++i)
        if (fled_reg_read(at[i], &r[i])) {
            sb_log("drvprobe: iris: FAIL the S2MPB02 (0x59 on USI10) did not answer: IR LED registers not read");
            return 1;
        }
    struct iris_irled d;
    iris_irled_decode(r, &d);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: iris: PASS IR LED (S2MPB02, read only): CTRL2 ");
    ub_hex(&u, d.ctrl2, 2);
    ub_s(&u, " current code ");
    ub_dec(&u, d.cur_code);
    ub_s(&u, "/16, width ");
    ub_dec(&u, d.width);
    ub_s(&u, ", delay ");
    ub_dec(&u, d.delay);
    ub_s(&u, d.max_timer_on ? ", max timer ON " : ", max timer off ");
    ub_dec(&u, d.max_time);
    ub_s(&u, d.discharge ? ", discharge on" : ", discharge off");
    sb_log(u.b);
    return 1;
}

static int l2_running;

int drv_iris_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
    plevel = level;
    if (level == 0)
        return level0();
    if (level == 1)
        return level1();
    /* level 2, attended only: P1a (the sensor up, its ID, off) and P1b (ISPPRE, CSIS3 fault-safe, one dark frame
     * without the LED), kern/iris.c; its last line is the PASS / FAIL with the key fact first */
    if (!l2_running) {
        l2_running = 1;
        iris_p1_begin();
    }
    int fin = iris_p1_step(wait_ms);
    if (fin)
        l2_running = 0;
    return fin;
}

void drv_iris_abort(void)
{
    if (l2_running)
        iris_p1_abort();                        /* P1: everything off in reverse order */
    l2_running = 0;
    plevel = -1;                                /* levels 0-1 write nothing to the iris parts */
}
#endif
