/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha7: the vibration motor (an LRA). The S9's haptic path (vendor DT exynos9810-starlte_motor.dtsi
 * and _if-pmic-max77705.dtsi; Utility OS research 4g):
 *   - the SoC's PWM timer 1 (samsung,s3c6400-pwm at 0x10510000) on gpp3-1 (PERIC0 0x10430060, function 2, pull-down),
 *     at the DT's "alert" setting: period 48828 ns, duty 34179 ns (70 %);
 *   - the motor's boost enable gpg1-7 (PERIC0 0x104300A0 / A4) as an output, high;
 *   - the MAX77705 (I2C 0x66): MAINCTRL1 (0x02) bit 7 BIASEN, MCONFIG (0x10) = 0xC2 (bit 7 LRA, bit 6 MEN on, bit 1
 *     the PWM divided by 128: 20.48 kHz in, 160 Hz to the motor); off: MCONFIG bit 6 clear.
 * The PWM's input clock is not known here: the first use measures it (timer 1 free-running, its count over 2 ms
 * against the arch timer) and refuses if it does not tick. Only those two MAX77705 registers are written, through
 * the gate, read-modify-write. Every first use logs what it read. A pulse ends by a timer (KT_UI 9), at most 5 s. */
#include "sb.h"
#include "kern.h"

#define PWM_BASE 0x10510000UL
#define TCFG0 (PWM_BASE + 0x00)
#define TCFG1 (PWM_BASE + 0x04)
#define TCON (PWM_BASE + 0x08)
#define TCNTB1 (PWM_BASE + 0x18)
#define TCMPB1 (PWM_BASE + 0x1C)
#define TCNTO1 (PWM_BASE + 0x20)
#define T1_START (1U << 8)
#define T1_UPDATE (1U << 9)
#define T1_INVERT (1U << 10)
#define T1_RELOAD (1U << 11)
#define GPP3_CON 0x10430060UL
#define GPP3_PUD 0x10430068UL
#define GPG1_CON 0x104300A0UL
#define GPG1_DAT 0x104300A4UL
#define MAX_TOP 0x66U
#define MAINCTRL1 0x02U
#define MCONFIG 0x10U
#define PERIOD_NS 48828U
#define DUTY_NS 34179U
#define PULSE_MAX_MS 5000U

static int state;                           /* 0 not checked, 1 usable, -1 refused (why) */
static const char *why = "not checked";
static int on;
static uint32_t rate_khz;                   /* the PWM timer's count rate */
#ifndef SB_HOST
static uint8_t mcfg_was, main_was;
static uint32_t tcon_was, gpp3_was, gpg1_was;
#endif
static uint64_t pulses;

#ifndef SB_HOST
static uint32_t rd(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int max_rmw(uint8_t reg, uint8_t clear, uint8_t set)
{
    uint8_t v = 0;
    if (!sb_max_init() || !sb_i2c_read(&sb_max_bus, MAX_TOP, reg, &v, 1))
        return 0;
    uint8_t n = (uint8_t)((v & ~clear) | set);
    if (n == v)
        return 1;
    struct sb_auth au;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)MAX_TOP << 8 | reg, 1, SB_TIER1, 0, "vibration: the MAX77705 haptic bits");
    int ok = sb_gate_open(&au) && sb_max_write(MAX_TOP, reg, n, SB_TIER1);
    sb_gate_close();
    return ok;
}
#endif

static int check(void)
{
    if (state)
        return state > 0;
#ifdef SB_HOST
    state = 1;
    why = 0;
    rate_khz = 100000;
    sb_log("vib: (host) the motor's path is a log line here");
    return 1;
#else
    uint32_t c0 = 0, c1 = 0, tc = 0, gp = 0, gg = 0, gd = 0;
    int ok = sb_probe32(TCFG0, &c0) && sb_probe32(TCFG1, &c1) && sb_probe32(TCON, &tc) && sb_probe32(GPP3_CON, &gp) &&
             sb_probe32(GPG1_CON, &gg) && sb_probe32(GPG1_DAT, &gd);
    uint8_t m1 = 0, mc = 0;
    int mok = sb_max_init() && sb_i2c_read(&sb_max_bus, MAX_TOP, MAINCTRL1, &m1, 1) &&
              sb_i2c_read(&sb_max_bus, MAX_TOP, MCONFIG, &mc, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "vib: pwm TCFG0 ");
    ub_hex(&u, c0, 8);
    ub_s(&u, " TCFG1 ");
    ub_hex(&u, c1, 8);
    ub_s(&u, " TCON ");
    ub_hex(&u, tc, 8);
    ub_s(&u, "; gpp3 con ");
    ub_hex(&u, gp, 8);
    ub_s(&u, ", gpg1 con ");
    ub_hex(&u, gg, 8);
    ub_s(&u, " dat ");
    ub_hex(&u, gd, 2);
    ub_s(&u, mok ? "; max77705 MAINCTRL1 " : "; max77705 NOT READ");
    if (mok) {
        ub_hex(&u, m1, 2);
        ub_s(&u, " MCONFIG ");
        ub_hex(&u, mc, 2);
    }
    sb_log(u.b);
    if (!ok || c0 == 0xffffffffU || tc == 0xffffffffU) {
        state = -1;
        why = "The vibration's PWM timer does not read";
    } else if (!mok) {
        state = -1;
        why = "The MAX77705 does not answer";
    } else if (tc & (T1_START << 0) && ((gp >> 4) & 0xfU) == 2U) {
        state = -1;
        why = "PWM timer 1 already drives the motor's pin (someone else's)";
    } else {
        /* the timer's rate: timer 1 free-running from its largest count, mux 1/1, its count over 2 ms; the pin stays
         * as it is (not the PWM's), so nothing reaches the motor */
        tcon_was = tc;
        wr(TCFG1, c1 & ~0xf0U);
        wr(TCNTB1, 0xffffffffU);
        wr(TCMPB1, 0);
        wr(TCON, (tc & ~(T1_START | T1_UPDATE | T1_INVERT | T1_RELOAD)) | T1_UPDATE);
        wr(TCON, (tc & ~(T1_START | T1_UPDATE | T1_INVERT | T1_RELOAD)) | T1_START | T1_RELOAD);
        uint32_t a = rd(TCNTO1);
        uint64_t t0 = sb_us();
        while (sb_us() - t0 < 2000U) {
        }
        uint32_t b = rd(TCNTO1);
        uint64_t dt = sb_us() - t0;
        wr(TCON, tc & ~(T1_START | T1_UPDATE | T1_INVERT | T1_RELOAD));
        rate_khz = dt ? (uint32_t)((uint64_t)(a - b) * 1000U / dt) : 0;
        sb_logx("vib: the PWM timer counts, kHz: ", rate_khz);
        if (rate_khz < 1000U || rate_khz > 400000U) {
            state = -1;
            why = "The vibration's PWM timer does not tick (its clock is off)";
        } else {
            state = 1;
            why = 0;
            mcfg_was = mc;
            main_was = m1;
        }
    }
    if (state < 0)
        sb_logf("vib: refused: ", why);
    return state > 0;
#endif
}

int vib_ready(void)
{
    return check();
}

const char *vib_why(void)
{
    return why;
}

static unsigned pat_left, pat_on_ms, pat_off_ms;   /* the pattern player: pulses left, their on and off times */
static int pat_gap;                                 /* 1 while in an off gap */

static void motor_off(void);

void vib_off(void)
{
    pat_left = 0;
    pat_gap = 0;
    kt_cancel(KT_UI, 9);
    motor_off();
}

static void motor_off(void)
{
    if (!on)
        return;
    on = 0;
#ifndef SB_HOST
    (void)max_rmw(MCONFIG, 0xffU, (uint8_t)(mcfg_was & ~0x40U));   /* MEN off first: the motor stops */
    wr(TCON, rd(TCON) & ~(T1_START | T1_UPDATE | T1_RELOAD));
    wr(GPP3_CON, gpp3_was);                             /* the pin as it was (the PWM's function off) */
    wr(GPG1_DAT, rd(GPG1_DAT) & ~(1U << 7));            /* the motor's boost off */
    wr(GPG1_CON, gpg1_was);
    if (!(main_was & 0x80U))
        (void)max_rmw(MAINCTRL1, 0x80U, 0);             /* the bias as it was */
#endif
    sb_log("vib: off");
}

/* on for ms (at most 5 s), then off by the UI timer; 0 if it runs */
int vib_pulse(unsigned ms)
{
    if (!ms)
        return 0;
    if (!check())
        return SBOS_EAGAIN;
    if (ms > PULSE_MAX_MS)
        ms = PULSE_MAX_MS;
    if (!on) {
#ifndef SB_HOST
        uint32_t cnt = (uint32_t)((uint64_t)rate_khz * PERIOD_NS / 1000000U);
        uint32_t duty = (uint32_t)((uint64_t)rate_khz * DUTY_NS / 1000000U);
        if (cnt < 4U || duty >= cnt)
            return SBOS_EAGAIN;
        gpp3_was = rd(GPP3_CON);
        gpg1_was = rd(GPG1_CON);
        /* the boost: low, an output, then high */
        wr(GPG1_DAT, rd(GPG1_DAT) & ~(1U << 7));
        wr(GPG1_CON, (gpg1_was & ~(0xfU << 28)) | (1U << 28));
        wr(GPG1_DAT, rd(GPG1_DAT) | (1U << 7));
        /* the PWM: timer 1 at the period, the compare for the duty (Samsung's: the output is high for the last
         * TCNTB - TCMPB counts), started, then the pin's function 2 with its pull-down */
        wr(TCFG1, rd(TCFG1) & ~0xf0U);
        wr(TCNTB1, cnt - 1U);
        wr(TCMPB1, cnt - duty);
        uint32_t t = rd(TCON) & ~(T1_START | T1_UPDATE | T1_INVERT | T1_RELOAD);
        wr(TCON, t | T1_UPDATE);
        wr(TCON, t | T1_START | T1_RELOAD);
        wr(GPP3_PUD, (rd(GPP3_PUD) & ~(0xfU << 4)) | (1U << 4));
        wr(GPP3_CON, (gpp3_was & ~(0xfU << 4)) | (2U << 4));
        /* the MAX77705: the bias, then LRA + enable + divide by 128 */
        if (!max_rmw(MAINCTRL1, 0, 0x80U) || !max_rmw(MCONFIG, 0xffU, 0xc2U)) {
            on = 1;
            vib_off();
            state = -1;
            why = "The MAX77705 did not take the haptic bits";
            sb_logf("vib: refused: ", why);
            return SBOS_EAGAIN;
        }
#endif
        on = 1;
        if (!pulses++)
            sb_logx("vib: on (the first pulse), PWM count rate kHz ", rate_khz);
    }
    kt_set(KT_UI, 9, sb_ms(), ms, 0);
    return 0;
}

/* n pulses of on_ms with off_ms between (an alarm, the timer's end, a call): 0 if it started */
int vib_buzz(unsigned n, unsigned on_ms, unsigned off_ms)
{
    if (!n || !on_ms)
        return 0;
    int r = vib_pulse(on_ms);
    if (r)
        return r;
    pat_left = n - 1;
    pat_on_ms = on_ms;
    pat_off_ms = off_ms ? off_ms : 1;
    pat_gap = 0;
    return 0;
}

/* KT_UI 9: the end of an on time or of a gap */
void vib_timer(void)
{
    if (pat_left && !pat_gap) {
        motor_off();
        pat_gap = 1;
        kt_set(KT_UI, 9, sb_ms(), pat_off_ms, 0);
        return;
    }
    if (pat_left && pat_gap) {
        pat_gap = 0;
        --pat_left;
        if (vib_pulse(pat_on_ms))
            pat_left = 0;
        return;
    }
    motor_off();
}

int vib_on(void)
{
    return on;
}
