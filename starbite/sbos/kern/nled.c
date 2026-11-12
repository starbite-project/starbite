/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the notification LED, the RGB LED block of the MAX77705 (docs/drivers/NLED.md).
 *
 * Wiring and sources:
 * - the chip: MAX77705 on HSI2C 0x14360000 (USI_CMGP03; libsb max77705.c brings the bus up), its top / PMIC address
 *   7-bit 0x66. Samsung's GPL-2.0 driver drivers/leds/leds-max77705-rgb.c (starlte kernel; read from the public tree
 *   a public exynos9810 kernel tree, our sparse ref checkout lacks drivers/leds) takes
 *   the parent's client (max77705_rgb->i2c = max77705_dev->i2c, I2C_ADDR_PMIC 0xCC >> 1 in drivers/mfd/max77705.c).
 *   max77705-private.h's "Slave addr = 0x94" is a stale comment: drivers/mfd/max77705.c makes no client at 0x4A.
 * - registers (max77705-private.h:248-257): LEDEN 0x30 (two bits per LED n at 2n: 0 off, 1 on, 2 blink), LED0BRT ..
 *   LED3BRT 0x31..0x34 (current, 0.1 mA a step), LEDRMP 0x36 (ramp up [7:4], down [3:0]), LEDBLNK 0x38 (on [7:4],
 *   off [3:0]: Samsung's LEDBLNK_ON / LEDBLNK_OFF).
 * - channels: enum max77705_led_color { WHITE, RED, GREEN, BLUE } = LED0..LED3.
 * - currents: the phone's DT node rgb (device/dtb/starlte.dts:12530): normal_powermode_current 10, br_ratio_r 700,
 *   br_ratio_g 60, br_ratio_b 120 (the "uu" set, the lowest of the colour variants' red). Samsung's set_state writes
 *   brightness x ratio / 100, so its full brightness is red 70 (7.0 mA), green 6, blue 12, white 10. Those are the
 *   caps (nled_cap): no write above them, whatever the caller asks.
 *
 * What is written: only the seven LED registers at 0x66 (nled_allowed), each through libsb's gate (sb_max_write, a
 * tier-1 authorisation covering exactly that one register, as kern/vib.c does for the haptic bits at the same
 * address), each read back. Everything else at 0x66 (MAINCTRL1, the haptic, INTSRC, BOOST, SW_RESET 0x50, ...), the
 * charger, the gauge and USB-C are refused here before the gate is asked. A failed write turns the LED off.
 *
 * Seen on a phone: nothing yet. The address, bit meanings and channel order come from Samsung's driver; the probe's
 * level 2 is the first check (the person says which colour lit at each step). */
#include "sb.h"
#include "nled.h"

#define NREG 7
static const uint8_t regs[NREG] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x36, 0x38 };
/* LED0 white (DT normal_powermode_current), LED1 red, LED2 green, LED3 blue: current x br_ratio / 100 */
const uint8_t nled_cap[4] = { 10, 70, 6, 12 };
#define ST_ON 1U
#define ST_BLINK 2U

static int state;                           /* 0 not read, 1 ready, -1 the block does not answer */
static const char *why = "not read yet";
static uint8_t found[NREG], cur[NREG];

static int idx(unsigned reg)
{
    for (int i = 0; i < NREG; ++i)
        if (regs[i] == reg)
            return i;
    return -1;
}

int nled_allowed(unsigned addr7, unsigned reg)
{
    return addr7 == NLED_ADDR && idx(reg) >= 0;
}

/* Samsung's LEDBLNK_ON / LEDBLNK_OFF */
uint8_t nled_blnk_code(unsigned on, unsigned off)
{
    unsigned a = on < 100 ? 0 : on < 500 ? on / 100 - 1 : on < 3250 ? (on - 500) / 250 + 4 : 15;
    unsigned b = off < 1      ? 0
                 : off < 500   ? 1
                 : off < 5000  ? off / 500
                 : off < 8000  ? (off - 5000) / 1000 + 10
                 : off < 12000 ? (off - 8000) / 2000 + 13
                               : 15;
    return (uint8_t)(a << 4 | b);
}

static void logv(const char *what, unsigned reg, unsigned v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "nled: ");
    ub_s(&u, what);
    ub_s(&u, ", reg ");
    ub_hex(&u, reg, 2);
    ub_s(&u, " value ");
    ub_hex(&u, v, 2);
    sb_log(u.b);
}

static int rd(uint8_t reg, uint8_t *v)
{
    return sb_max_init() && sb_i2c_read(&sb_max_bus, NLED_ADDR, reg, v, 1);
}

/* One LED register: the allow-list, the cap (a restore may put back exactly what was found), the gate, the read-back. */
static int wr(uint8_t reg, uint8_t v, int restoring)
{
    int i = idx(reg);
    if (i < 0) {
        why = "refused: not an LED register";
        logv("refused (not an LED register)", reg, v);
        return 0;
    }
    if (reg >= NLED_BRT0 && reg < NLED_BRT0 + 4 && v > nled_cap[reg - NLED_BRT0] && !(restoring && v == found[i])) {
        why = "refused: above the DT's current for that LED";
        logv("refused (above the DT's current)", reg, v);
        return 0;
    }
    if (cur[i] == v)
        return 1;
    struct sb_auth au;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)NLED_ADDR << 8 | reg, 1, SB_TIER1, 0, "nled: the MAX77705 RGB LED");
    /* the value is this file's (capped above), the register one of the seven LED registers: the same fixed-value
     * tier-1 pattern as kern/vib.c's haptic bits at this address */
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    if (!sb_gate_open(&au)) {
        why = "the gate refused the LED write";
        logv("the gate refused the write", reg, v);
        return 0;
    }
    int ok = sb_max_write(NLED_ADDR, reg, v, SB_TIER1);
    sb_gate_close();
    uint8_t back = 0;
    if (!ok || !rd(reg, &back)) {
        why = "the LED write was not answered";
        logv("write not answered", reg, v);
        return 0;
    }
    cur[i] = back;
    if (back != v) {
        why = "the LED register did not read back what was written";
        logv("read back differs", reg, back);
        return 0;
    }
    return 1;
}

int nled_init(void)
{
    if (state > 0)
        return 1;                           /* "as found" is the first successful read; a failed one is retried */
    uint8_t v[NREG];
    for (int i = 0; i < NREG; ++i)
        if (!rd(regs[i], &v[i])) {
            state = -1;
            why = sb_max_why ? sb_max_why : "the LED block (MAX77705 0x66) does not answer";
            sb_log("nled: the LED block does not answer (MAX77705 0x66 on HSI2C 0x14360000)");
            return 0;
        }
    for (int i = 0; i < NREG; ++i)
        found[i] = cur[i] = v[i];
    state = 1;
    why = "ready";
    struct ub u;
    ub_init(&u);
    ub_s(&u, "nled: as found LEDEN ");
    ub_hex(&u, v[0], 2);
    ub_s(&u, " BRT");
    for (int i = 1; i <= 4; ++i) {
        ub_c(&u, ' ');
        ub_hex(&u, v[i], 2);
    }
    ub_s(&u, " RMP ");
    ub_hex(&u, v[5], 2);
    ub_s(&u, " BLNK ");
    ub_hex(&u, v[6], 2);
    sb_log(u.b);
    return 1;
}

int nled_off(void)
{
    if (!nled_init())
        return 0;
    return wr(NLED_LEDEN, 0, 0);
}

/* codes for LED0..3 (0 = that LED off), steady or blinking with blnk */
static int show(const uint8_t code[4], int blink, uint8_t blnk)
{
    if (!nled_init())
        return 0;
    uint8_t en = 0;
    int ok = 1;
    if (blink)
        ok = wr(NLED_RMP, 0, 0) && wr(NLED_BLNK, blnk, 0);
    for (unsigned n = 0; ok && n < 4; ++n)
        if (code[n]) {
            ok = wr((uint8_t)(NLED_BRT0 + n), code[n], 0);
            en |= (uint8_t)((blink ? ST_BLINK : ST_ON) << (2 * n));
        }
    if (ok)
        ok = wr(NLED_LEDEN, en, 0);
    if (!ok) {
        const char *w = why;
        (void)wr(NLED_LEDEN, 0, 0);
        why = w;
    }
    return ok;
}

static uint8_t scale(uint8_t c, unsigned pct, uint8_t cap)
{
    if (!c || !pct)
        return 0;
    if (pct > 100)
        pct = 100;
    unsigned v = (unsigned)c * pct * cap / 25500U;
    return (uint8_t)(v ? v : 1);              /* Samsung's: a lit colour is at least 1 */
}

static void codes(uint8_t code[4], uint8_t r, uint8_t g, uint8_t b, unsigned pct)
{
    code[0] = 0;
    code[1] = scale(r, pct, nled_cap[1]);
    code[2] = scale(g, pct, nled_cap[2]);
    code[3] = scale(b, pct, nled_cap[3]);
}

int nled_color(uint8_t r, uint8_t g, uint8_t b, unsigned pct)
{
    uint8_t c[4];
    codes(c, r, g, b, pct);
    if (!c[1] && !c[2] && !c[3])
        return nled_off();
    return show(c, 0, 0);
}

int nled_blink(uint8_t r, uint8_t g, uint8_t b, unsigned pct, unsigned on_ms, unsigned off_ms)
{
    uint8_t c[4];
    codes(c, r, g, b, pct);
    if (!c[1] && !c[2] && !c[3])
        return nled_off();
    on_ms = on_ms < 100 ? 100 : on_ms > 3250 ? 3250 : on_ms;
    off_ms = off_ms < 500 ? 500 : off_ms > 12000 ? 12000 : off_ms;
    return show(c, 1, nled_blnk_code(on_ms, off_ms));
}

int nled_pattern(enum nled_pattern p)
{
    switch (p) {
    case NLED_MISSED_CALL:
    case NLED_MESSAGE:
        return nled_blink(0, 0, 255, 100, 500, 5000);
    case NLED_CHARGING:
        return nled_color(255, 0, 0, 100);
    case NLED_CHARGED:
        return nled_color(0, 255, 0, 100);
    case NLED_LOW_BATTERY:
        return nled_blink(255, 0, 0, 100, 500, 5000);
    case NLED_CHARGE_ERROR:
        return nled_blink(255, 0, 0, 100, 500, 500);
    case NLED_OFF:
        return nled_off();
    default:
        why = "refused: no such pattern";
        return 0;
    }
}

const char *nled_pattern_name(enum nled_pattern p)
{
    static const char *const n[NLED_NPATTERNS] = { "Off", "Missed call", "New message", "Charging", "Charged",
                                                   "Low battery", "Charging problem" };
    return (unsigned)p < NLED_NPATTERNS ? n[p] : "?";
}

void nled_restore(void)
{
    if (state <= 0)
        return;
    int same = 1;
    for (int i = 0; i < NREG; ++i)
        same &= cur[i] == found[i];
    if (same)
        return;
    if (cur[0])
        (void)wr(NLED_LEDEN, 0, 1);
    for (int i = 1; i < NREG; ++i)
        (void)wr(regs[i], found[i], 1);
    (void)wr(NLED_LEDEN, found[0], 1);
}

const char *nled_why(void)
{
    return why;
}

/* ---- the probe (drvprobe.h) ------------------------------------------------------------------------------------- */

static int pstep = -1, plevel = -1;
static uint64_t pt0;

static int pend(const char *pass, const char *fail)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, pass ? "drvprobe: nled: PASS " : "drvprobe: nled: FAIL ");
    ub_s(&u, pass ? pass : fail);
    sb_log(u.b);
    pstep = plevel = -1;
    return 1;
}

static int found_again(void)
{
    for (int i = 0; i < NREG; ++i) {
        uint8_t v;
        if (!rd(regs[i], &v) || v != found[i])
            return 0;
    }
    return 1;
}

void drv_nled_abort(void)
{
    nled_restore();
    pstep = plevel = -1;
}

int drv_nled_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
    if (level != plevel) {
        pstep = 0;
        plevel = level;
        pt0 = sb_ms();
    }
    if (level == 0) {
        /* read only: the seven LED registers (nled_init reads, never writes) */
        if (!nled_init())
            return pend(0, "the LED block (MAX77705 0x66 on HSI2C 0x14360000) does not answer");
        struct ub u;
        ub_init(&u);
        ub_s(&u, "the LED block answers at 0x66, LEDEN ");
        ub_hex(&u, found[0], 2);
        ub_s(&u, found[0] ? " (an LED is on from before)" : " (all off)");
        return pend(u.b, 0);
    }
    if (level == 1)
        return pend("skipped (attended)", 0);
    if (level != 2)
        return pend("nothing at this level", 0);
    if (pstep > 0 && sb_ms() - pt0 > NLED_PROBE_BUDGET_MS) {
        drv_nled_abort();
        return pend(0, "did not finish within its 12 s (the LED block put back as found)");
    }
    /* a quarter of the DT's current: LED0 2, red 17, green 1, blue 3 */
    uint8_t lo[4];
    for (unsigned n = 0; n < 4; ++n)
        lo[n] = nled_cap[n] / 4 ? (uint8_t)(nled_cap[n] / 4) : 1;
    static const char *const say[6] = { "red", "green", "blue", "white (red + green + blue)", "LED0 (the DT's led_w)",
                                        "blue blinking, 0.5 s on / 0.5 s off, 3 s" };
    uint8_t c[4] = { 0, 0, 0, 0 };
    int ok;
    switch (pstep) {
    case 0:
        if (!nled_init())
            return pend(0, "the LED block (MAX77705 0x66 on HSI2C 0x14360000) does not answer");
        c[1] = lo[1];
        ok = show(c, 0, 0);
        break;
    case 1:
        c[2] = lo[2];
        ok = show(c, 0, 0);
        break;
    case 2:
        c[3] = lo[3];
        ok = show(c, 0, 0);
        break;
    case 3:
        c[1] = lo[1], c[2] = lo[2], c[3] = lo[3];
        ok = show(c, 0, 0);
        break;
    case 4:
        c[0] = lo[0];
        ok = show(c, 0, 0);
        break;
    case 5:
        c[3] = lo[3];
        ok = show(c, 1, nled_blnk_code(500, 500));
        break;
    default:
        nled_restore();
        if (!found_again())
            return pend(0, "the LED registers did not read back as found after the test");
        return pend("red, green, blue, white, LED0 and a blink shown at a quarter of the DT's current; "
                    "the LED block back as found (the person says which colours lit)", 0);
    }
    if (!ok) {
        const char *w = why;
        drv_nled_abort();
        return pend(0, w);
    }
    sb_logf("drvprobe: nled: now showing ", say[pstep]);
    *wait_ms = pstep == 5 ? 3000U : 1000U;
    ++pstep;
    return 0;
}
