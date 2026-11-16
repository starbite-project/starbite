/* SPDX-License-Identifier: MIT
 * The S9's heart-rate sensor, MAX86915: a pulse check (Track D, 2026-11-16; kern/hrm.h has the wiring).
 *
 * The chip set up as Samsung's hrm_max86915.c sets it for HRM mode (drivers/optics/, GPL; registers only, no code
 * copied): reset; LED1 (IR) at 0x46 in the 150 mA range (about 42 mA in short pulses), the LED sequence IR only;
 * MODE_CONFIGURATION_2 0x6D (400 samples/s, 4 averaged in the FIFO: 100 a second); FLEX mode. The FIFO (32 samples
 * of 3 bytes, 19 bits each) is read every 100 ms. A simple level control keeps the IR level between about 150000 and
 * 420000 counts, so a fingertip neither saturates nor drowns.
 *
 * The pulse (hrm_algo_*): the sign turned (more blood = less light back), an 80 ms smoothing, then its slope over
 * 40 ms (a slow baseline drift from breathing or a moving finger barely changes it); a beat where the slope rises
 * through half its recent top after it was falling, at least 300 ms apart (200 a minute), and only above a noise
 * floor. The rate is the median of the last beat intervals, "steady" when the last five agree within 20 %. */
#include "hrm.h"

#ifndef HRM_HOST
#include "sb.h"
#include "kern.h"
#include "survkit.h"
#endif

/* ---- the pulse ---- */
void hrm_algo_reset(struct hrm_algo *a, unsigned rate_hz)
{
    for (unsigned i = 0; i < sizeof(*a); ++i)
        ((uint8_t *)a)[i] = 0;
    a->rate_hz = rate_hz ? rate_hz : 100U;
    a->dc16 = -1;
}

static int med(const uint16_t *v, unsigned n)
{
    uint16_t s[HRM_IV];
    for (unsigned i = 0; i < n; ++i)
        s[i] = v[i];
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && s[j - 1] > s[j]; --j) {
            uint16_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    return n ? (n & 1 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : 0;
}

static unsigned last_n(const struct hrm_algo *a, unsigned k, uint16_t *out)
{
    unsigned n = a->n_iv < k ? a->n_iv : k;
    for (unsigned i = 0; i < n; ++i)
        out[i] = a->iv[(a->ivh + HRM_IV - 1 - i) % HRM_IV];
    return n;
}

int hrm_algo_push(struct hrm_algo *a, int32_t ir)
{
    if (a->dc16 < 0)
        a->dc16 = ir;                       /* the reference: values are kept small and relative to it */
    a->lp[a->lpi++ % HRM_LP] = (int32_t)(a->dc16 - ir);   /* more blood: less light back: up */
    int32_t sm = 0;
    for (unsigned i = 0; i < HRM_LP; ++i)
        sm += a->lp[i];
    sm /= (int32_t)HRM_LP;
    a->sh[a->shi++ % HRM_DS] = sm;
    /* the slope over 40 ms: the systolic upstroke is the steepest part of a beat; a slow baseline drift barely adds */
    int32_t f = a->t >= HRM_DS ? sm - a->sh[a->shi % HRM_DS] : 0;
    a->env = f > a->env ? f : a->env - a->env / (int32_t)(a->rate_hz * 2U);   /* the recent top, fading in ~2 s */
    int beat = 0;
    ++a->t;
    if (f < 0)
        a->armed = 1;                       /* falling: the next steep rise is a beat */
    uint32_t refr = a->rate_hz * 3U / 10U;  /* 300 ms: 200 a minute at most */
    if (a->armed && a->t > a->rate_hz && a->env >= HRM_FLOOR && f > a->env / 2 &&
        (!a->last_peak || a->t - a->last_peak >= refr)) {
        a->armed = 0;
        uint32_t tp = a->t;
        int keep = 1;
        if (a->last_peak) {
            uint32_t ms = (tp - a->last_peak) * 1000U / a->rate_hz;
            uint16_t r[HRM_IV];
            unsigned n = last_n(a, 7, r);
            int md = med(r, n);
            if (n >= 3 && (int)ms * 10 < md * 6)
                keep = 0;                   /* far too soon after the last: noise, not a beat */
            else if (ms <= 2000U) {
                a->iv[a->ivh] = (uint16_t)ms;
                a->ivh = (a->ivh + 1) % HRM_IV;
                if (a->n_iv < HRM_IV)
                    ++a->n_iv;
            }
        }
        if (keep) {
            a->last_peak = tp;
            ++a->beats;
            beat = 1;
            uint16_t r[HRM_IV];
            unsigned n = last_n(a, 7, r);
            if (n >= 3) {
                int md = med(r, n);
                a->bpm = md ? (60000 + md / 2) / md : 0;
                uint16_t q[5];
                unsigned k = last_n(a, 5, q), ok = 1;
                for (unsigned i = 0; i < k; ++i)
                    if ((int)q[i] * 10 < md * 8 || (int)q[i] * 10 > md * 12)
                        ok = 0;
                a->quality = k == 5 && ok ? 2 : 1;
            }
        }
    }
    if (a->last_peak && a->t - a->last_peak > a->rate_hz * 3U) {
        a->quality = 0;                     /* 3 s with no beat: lost */
        a->n_iv = 0;
        a->last_peak = 0;
        a->bpm = 0;
    }
    a->f2 = a->f1;
    a->f1 = f;
    return beat;
}

#ifndef HRM_HOST
/* ---- the driver ---- */
#define BUS 0x108E0000UL
#define ADDR 0x57U
#define USI_MODE 0x10811034UL               /* usi@10811034: usi_v2_mode "i2c" */
#define GPP5 0x10830020UL                   /* gpp5-0 SCL, gpp5-1 SDA: function 2 */
#define GPG1 0x104300A0UL                   /* gpg1-6: the LED boost enable */
#define L38 0x6FU
#define L38_ON_1V8 0xECU
#define PART_ID 0x2BU
#define LED_INIT 0x46U
#define LED_MIN 0x08
#define LED_MAX 0x90
#define RUN_MAX_MS 120000U
#define KEEP_MS 3000U

static const uintptr_t gates[2] = { 0x108020F4UL, 0x108020F8UL };   /* CMU_PERIC1 USI08 IPCLK, PCLK */
static const unsigned ctl_regs[] = { 0x00, 0x04, 0x08, 0x20, 0x40, 0x44, 0x48, 0x60, 0x64, 0x68, 0x70, 0xc4 };
#define NCTL (sizeof(ctl_regs) / sizeof(ctl_regs[0]))

static struct {
    int state, have_ldo8, have_l38, have_pins, have_ctl, have_gpg, chip_on;
    uint8_t ldo8_was, l38_was, led;
    uint32_t usi_was, con_was, pud_was, ctl[NCTL], g_con, g_dat, g_pud;
    struct sb_i2c bus;
    uint64_t t_want, t_start, t_finger, t_agc, t_rate;
    unsigned n_rate, n_total;
    int32_t dc;
    int finger;
    struct hrm_algo al;
} H;
static const char *why = "not started";

static int rd(uintptr_t a, uint32_t *v) { return sb_probe32(a, v); }
static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int reg_w(uint8_t r, uint8_t v)
{
    uint8_t m[2] = { r, v };
    return sb_i2c_msg(&H.bus, ADDR, 0, m, 2, 1);
}

static int reg_r(uint8_t r, uint8_t *v, unsigned n) { return sb_i2c_read(&H.bus, ADDR, r, v, n); }

static int l38_write(uint8_t v)
{
    struct sb_auth a;
    sb_auth_init(&a, SB_SP_PMIC, 0, 1U << 8 | L38, 1, SB_TIER1, 0, "heart rate: LDO38 (its I2C pull-ups), 1.8 V");
    sb_auth_step(&a, SB_STEP_WARN);
    sb_auth_step(&a, SB_STEP_HELD);
    int ok = sb_gate_open(&a) && sb_pmic_write(1, L38, v, SB_TIER1);
    sb_gate_close();
    return ok;
}

/* everything back as found, in the reverse order */
static void put_back(void)
{
    if (H.chip_on) {
        (void)reg_w(0x0C, 0x00);            /* LED1 off */
        (void)reg_w(0x09, 0x80);            /* shut down */
        H.chip_on = 0;
    }
    if (H.have_ctl) {
        wr(BUS + 0x00, H.ctl[0] | 0x80000000U);
        wr(BUS + 0x00, H.ctl[0] & ~0x80000000U);
        for (unsigned i = 1; i < NCTL; ++i)
            if (ctl_regs[i] != 0x24U)
                wr(BUS + ctl_regs[i], H.ctl[i]);
        wr(BUS + 0x00, H.ctl[0]);
        H.have_ctl = 0;
    }
    if (H.have_pins) {
        wr(GPP5 + 8, H.pud_was);
        wr(GPP5, H.con_was);
        wr(USI_MODE, H.usi_was);
        H.have_pins = 0;
    }
    if (H.have_gpg) {
        wr(GPG1 + 4, (H.g_dat & 0x40U) | (*(volatile uint32_t *)(GPG1 + 4) & ~0x40U));
        wr(GPG1, H.g_con);
        wr(GPG1 + 8, H.g_pud);
        H.have_gpg = 0;
    }
    if (H.have_ldo8) {
        if (!fled_ldo8_write(H.ldo8_was, H.ldo8_was))
            sb_log("hrm: LDO8 NOT back as found (it stays on until a restart)");
        H.have_ldo8 = 0;
    }
    if (H.have_l38) {
        if (!l38_write(H.l38_was))
            sb_log("hrm: LDO38 NOT back as found (it stays on until a restart)");
        H.have_l38 = 0;
    }
}

static int fail(const char *w)
{
    why = w;
    sb_logf("hrm: not started: ", w);
    put_back();
    H.state = HRM_FAILED;
    return -1;
}

static int start(void)
{
    uint32_t g = 0;
    uint8_t v = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (!rd(gates[i], &g) || (g & 0x00300000U) == 0x00100000U)
            return fail("the sensor's I2C clock gate is closed (not opened by this driver)");
    /* the rails: LDO38 (pull-ups) first, then LDO8 (VDD), then the LED boost pin; 2 ms */
    if (!sb_pmic_read(1, L38, &v) && !sb_pmic_read(1, L38, &v))
        return fail("LDO38 does not read");
    H.l38_was = v;
    if ((v & 0xC0U) != 0xC0U) {
        if ((v & 0x3FU) != 0x2CU && (v & 0x3FU) != 0)
            return fail("LDO38 is set to an unexpected voltage: not touched");
        if (!l38_write(L38_ON_1V8))
            return fail("LDO38 could not be switched on");
        H.have_l38 = 1;
    } else if ((v & 0x3FU) != 0x2CU) {
        return fail("LDO38 is on at a voltage other than 1.8 V (someone else's): not used");
    }
    if (!fled_ldo8_read(&v))
        return fail("the S2MPB02 (LDO8) does not answer");
    H.ldo8_was = v;
    if (v != 0xB0U) {
        if ((v & 0x80U) && (v & 0x7FU) != 0x30U)
            return fail("LDO8 is on at a voltage other than 1.8 V (someone else's): not used");
        if (((v & 0x7FU) != 0x30U && !fled_ldo8_write(0x30U, H.ldo8_was)) || !fled_ldo8_write(0xB0U, H.ldo8_was)) {
            H.have_ldo8 = 1;
            return fail("LDO8 could not be switched on");
        }
        H.have_ldo8 = 1;
    }
    if (!rd(GPG1, &H.g_con) || !rd(GPG1 + 4, &H.g_dat) || !rd(GPG1 + 8, &H.g_pud))
        return fail("the boost pin does not read");
    H.have_gpg = 1;
    wr(GPG1 + 8, H.g_pud & ~(0xfU << 24));
    wr(GPG1 + 4, H.g_dat | 0x40U);
    wr(GPG1, (H.g_con & ~(0xfU << 24)) | (1U << 24));
    sb_mdelay(2);
    /* the bus: USI08 in I2C mode, gpp5-0/1 function 2 without pulls (LDO38 is the pull-up), the controller */
    int ok = rd(USI_MODE, &H.usi_was) && rd(GPP5, &H.con_was) && rd(GPP5 + 8, &H.pud_was);
    for (unsigned i = 0; ok && i < NCTL; ++i)
        ok = rd(BUS + ctl_regs[i], &H.ctl[i]);
    if (!ok)
        return fail("the sensor's I2C controller or its pins do not read");
    H.have_pins = H.have_ctl = 1;
    wr(USI_MODE, 4U);
    wr(GPP5 + 8, H.pud_was & ~0xffU);
    wr(GPP5, (H.con_was & ~0xffU) | 0x22U);
    H.bus.base = BUS;
    H.bus.fs1 = H.ctl[7], H.bus.fs2 = H.ctl[8], H.bus.fs3 = H.ctl[9];
    H.bus.state = 0;
    if (!((H.bus.fs3 >> 16) & 0xffU)) {     /* nothing from S-Boot: Android's 200 MHz / 400 kHz timing */
        H.bus.fs1 = 0x01F0FF00U;
        H.bus.fs2 = 0x030003E0U;
        H.bus.fs3 = 0x00210000U;
    }
    sb_i2c_bus(&H.bus);
    /* the chip */
    uint8_t id = 0, rev = 0;
    if (!reg_r(0xFF, &id, 1)) {
        sb_logx("hrm: no answer at 0x57; I2C int_stat ", sb_i2c_fail_irq);
        return fail("the sensor does not answer at 0x57");
    }
    (void)reg_r(0xFE, &rev, 1);
    if (id != PART_ID) {
        sb_logx("hrm: part ID ", id);
        return fail("not the MAX86915 (part ID differs)");
    }
    H.chip_on = 1;
    if (!reg_w(0x09, 0x40))                 /* reset */
        return fail("the reset write failed");
    for (int i = 0; i < 20; ++i) {
        sb_mdelay(1);
        if (reg_r(0x09, &v, 1) && !(v & 0x40U))
            break;
    }
    H.led = LED_INIT;
    static const uint8_t cfg[][2] = {
        { 0x0C, LED_INIT }, { 0x0D, 0 }, { 0x0E, 0 }, { 0x0F, 0 },  /* IR on, red / green / blue off */
        { 0x11, 0xAA },                                              /* all LEDs in the 150 mA range */
        { 0x26, 0 }, { 0x27, 0 },                                    /* no crosstalk offset */
        { 0x02, 0 }, { 0x03, 0 },                                    /* no interrupts: polled */
        { 0x13, 0x01 }, { 0x14, 0x00 },                              /* the sequence: IR only */
        { 0x0A, 0x6D },                                              /* 400 / s, ADC range 3, pulse width 1 */
        { 0x08, 0x40 },                                              /* 4 averaged: 100 a second */
        { 0x04, 0 }, { 0x05, 0 }, { 0x06, 0 },                       /* the FIFO empty */
        { 0x09, 0x03 },                                              /* FLEX mode: running */
    };
    for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); ++i)
        if (!reg_w(cfg[i][0], cfg[i][1]))
            return fail("a set-up write failed");
    hrm_algo_reset(&H.al, 100);
    H.t_start = H.t_rate = H.t_agc = sb_ms();
    H.n_rate = H.n_total = 0;
    H.finger = 0;
    H.t_finger = 0;
    H.dc = 0;
    H.state = HRM_RUN;
    why = 0;
    kt_set(KT_SURV, KT_SURV_KIT_HRM, sb_ms(), 100, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "hrm: MAX86915 rev ");
    ub_hex(&u, rev, 2);
    ub_s(&u, " running (LDO38 was ");
    ub_hex(&u, H.l38_was, 2);
    ub_s(&u, ", LDO8 was ");
    ub_hex(&u, H.ldo8_was, 2);
    ub_s(&u, "): IR at 100 samples a second");
    sb_log(u.b);
    return 0;
}

int hrm_want(void)
{
    H.t_want = sb_ms();
    if (H.state == HRM_RUN)
        return 0;
    if (H.state == HRM_FAILED && sb_ms() - H.t_start < 5000U)
        return -1;                          /* not again at every keep-alive */
    H.t_start = sb_ms();
    return start();
}

void hrm_stop(const char *w)
{
    if (H.state != HRM_RUN)
        return;
    kt_cancel(KT_SURV, KT_SURV_KIT_HRM);
    put_back();
    H.state = HRM_OFF;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "hrm: off (");
    ub_s(&u, w ? w : "");
    ub_s(&u, "); last ");
    ub_dec(&u, (uint64_t)H.al.bpm);
    ub_s(&u, " a minute, quality ");
    ub_dec(&u, (uint64_t)H.al.quality);
    ub_s(&u, "; everything back as found");
    sb_log(u.b);
}

static void set_led(int v)
{
    v = v < LED_MIN ? LED_MIN : v > LED_MAX ? LED_MAX : v;
    if (v == H.led || !reg_w(0x0C, (uint8_t)v))
        return;
    H.led = (uint8_t)v;
    hrm_algo_reset(&H.al, H.al.rate_hz);    /* a step in the level: the baseline starts again */
}

void hrm_tick(void)
{
    if (H.state != HRM_RUN) {
        kt_cancel(KT_SURV, KT_SURV_KIT_HRM);
        return;
    }
    uint64_t now = sb_ms();
    if (now - H.t_want > KEEP_MS) {
        hrm_stop("its page closed");
        return;
    }
    if (now - H.t_start > RUN_MAX_MS) {
        hrm_stop("2 minutes: open the page again for another check");
        return;
    }
    uint8_t p[3];
    if (!reg_r(0x04, p, 3)) {               /* write pointer, overflow counter, read pointer */
        hrm_stop("the sensor stopped answering");
        H.state = HRM_FAILED;
        why = "the sensor stopped answering";
        return;
    }
    unsigned n = p[1] ? 32U : (unsigned)((p[0] - p[2]) & 0x1FU);
    int32_t sum = 0;
    unsigned got = 0;
    while (n) {
        uint8_t b[48];
        unsigned k = n > 16U ? 16U : n;
        if (!reg_r(0x07, b, k * 3U))
            break;
        for (unsigned i = 0; i < k; ++i) {
            int32_t ir = ((int32_t)b[3 * i] << 16 | (int32_t)b[3 * i + 1] << 8 | b[3 * i + 2]) & 0x7FFFF;
            sum += ir / 16;
            ++got;
            if (H.finger)
                (void)hrm_algo_push(&H.al, ir);
        }
        n -= k;
    }
    if (!got)
        return;
    H.n_rate += got;
    H.n_total += got;
    if (now - H.t_rate >= 2000U) {          /* the real rate, counted */
        unsigned r = (unsigned)(H.n_rate * 1000U / (unsigned)(now - H.t_rate));
        if (r >= 25U && r <= 400U && (r * 10U < H.al.rate_hz * 9U || r * 10U > H.al.rate_hz * 11U)) {
            sb_logx("hrm: samples a second (counted) ", r);
            hrm_algo_reset(&H.al, r);
        }
        H.n_rate = 0;
        H.t_rate = now;
    }
    H.dc = (int32_t)(sum / (int32_t)got) * 16;
    int was = H.finger;
    H.finger = H.dc > 60000 && H.dc < 520000;
    if (H.finger && !was) {
        H.t_finger = now;
        hrm_algo_reset(&H.al, H.al.rate_hz);
    } else if (!H.finger) {
        H.t_finger = 0;
    }
    if (now - H.t_agc >= 500U) {            /* the level control, twice a second */
        H.t_agc = now;
        if (H.dc >= 470000)
            set_led(H.led - 12);
        else if (H.dc > 420000)
            set_led(H.led - 4);
        else if (H.dc > 60000 && H.dc < 150000)
            set_led(H.led + 4);
    }
}

void hrm_status(struct hrm_status *s)
{
    s->state = H.state;
    s->finger = H.state == HRM_RUN && H.finger;
    s->bpm = s->finger ? H.al.bpm : 0;
    s->quality = s->finger ? H.al.quality : 0;
    s->seconds = s->finger && H.t_finger ? (int)((sb_ms() - H.t_finger) / 1000U) : 0;
    s->dc = H.dc;
    s->led_ma10 = H.led * 6;                /* 150 mA range: 0.6 mA a step */
    s->rate_hz = H.al.rate_hz;
    s->samples = H.n_total;
}

const char *hrm_why(void) { return why; }
#endif
