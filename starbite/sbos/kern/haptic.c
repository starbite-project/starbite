/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: haptic patterns on the S9's vibration motor (docs/drivers/HAPTICS.md; API haptic.h).
 *
 * What it drives: the LRA through vib.c (PWM timer 1 at 0x10510000 on gpp3-1, the boost enable gpg1-7, the
 * MAX77705's MAINCTRL1 / MCONFIG at I2C 0x66 on HSI2C 0x14360000), never a register of its own. vib.c gained three
 * hooks for it: vib_set_duty (the strength), vib_wait (a gap on vib.c's KT_UI 9 timer) and vib_set_end_hook (called
 * when a pulse or gap ends, and at vib_off).
 *
 * Strength (Samsung's GPL motor DT exynos9810-starlte_motor.dtsi and include/linux/sec_haptic.h): the motor is driven
 * at period 48828 ns (20.48 kHz, divided by 128 in the MAX77705: 160 Hz at the LRA), duty 34179 ns = 70 % =
 * haptic,normal_ratio; haptic,overdrive_ratio = 95 % is Samsung's short-burst boost, max_timeout 10000 ms. Samsung's
 * sec_haptic maps an intensity onto the duty between half the period (no drive: the PWM input is centred) and the
 * maximum duty [the 50 % centre from memory of sec_haptic.c's set_intensity, not in our sparse tree]. Here: duty =
 * 50 % + 20 % x strength x intensity gain, so 70 % (the rated, vib.c's fixed value) is the most it ever gets, and
 * the overdrive is never used. A non-zero strength gets at least 54 % (below that an LRA hardly moves).
 *
 * Safety limit (why 5 s): vib.c already ends any pulse after 5 s (PULSE_MAX_MS); the DT's own cap per request is
 * 10 s. Patterns are long-lived (a ringtone loops), so this file adds: an "on" segment at most 2 s; the motor never
 * driven for more than 5 s in total without a rest of at least 250 ms (otherwise a forced 1 s rest is inserted);
 * a looped pattern ends by itself after 60 s (the caller plays it again if the phone still rings). The standard
 * ring pattern drives the motor 40 % of the time, the alarm 50 %.
 *
 * Seen on a phone: vib.c's fixed 70 % pulses (1.2-alpha11+). Not seen: any other duty, the patterns. */
#include "sb.h"
#include "kern.h"
#include "haptic.h"
#include "drvprobe.h"

#define SEG_MAX_MS 2000U
#define CONT_MAX_MS 5000U
#define REST_MIN_MS 250U
#define FORCED_REST_MS 1000U
#define LOOP_MAX_MS 60000U
#define DUTY_MIN_PM 540U
#define DUTY_DT_PM 700U

static const struct haptic_seg ring[] = { { 800, 100 }, { 400, 0 }, { 800, 100 }, { 2000, 0 } };
static const struct haptic_seg notify[] = { { 60, 100 }, { 120, 0 }, { 60, 100 } };
static const struct haptic_seg alarm_p[] = { { 500, 100 }, { 300, 0 }, { 500, 100 }, { 300, 0 }, { 500, 100 }, { 1000, 0 } };
static const struct haptic_seg tick[] = { { 20, 60 } };
static const struct haptic_seg success[] = { { 30, 70 }, { 80, 0 }, { 50, 100 } };
static const struct haptic_seg warning[] = { { 50, 100 }, { 150, 0 }, { 50, 70 } };
static const struct haptic_seg error_p[] = { { 40, 100 }, { 70, 0 }, { 40, 100 }, { 70, 0 }, { 40, 100 } };

static const struct {
    const char *name;
    const struct haptic_seg *s;
    uint8_t n, loop;
} PAT[HAPTIC_N] = {
    { "ring", ring, 4, 1 },
    { "notification", notify, 3, 0 },
    { "alarm", alarm_p, 6, 1 },
    { "key tick", tick, 1, 0 },
    { "success", success, 3, 0 },
    { "warning", warning, 3, 0 },
    { "error", error_p, 5, 0 },
};

static const unsigned gain[4] = { 0, 50, 75, 100 };   /* % of the segment's strength at intensity 0..3 */

static struct {
    const struct haptic_seg *s;
    uint8_t n, i, loop, active;
    uint64_t t_start, t_due;        /* the pattern's start; when the running segment's timer should fire */
    unsigned cont;                  /* ms driven since the last rest of REST_MIN_MS or more */
    uint32_t forced;
} H;
static int level = 3;
static const char *why;
static int hooked;

static void end_hook(int ended);

const char *haptic_name(int which)
{
    return which >= 0 && which < HAPTIC_N ? PAT[which].name : "?";
}

unsigned haptic_duty_for(unsigned strength)
{
    if (!strength || level <= 0)
        return 0;
    if (strength > 100U)
        strength = 100U;
    unsigned pm = 500U + 200U * strength * gain[level > 3 ? 3 : level] / 10000U;
    if (pm < DUTY_MIN_PM)
        pm = DUTY_MIN_PM;
    return pm > DUTY_DT_PM ? DUTY_DT_PM : pm;
}

static void done(void)
{
    H.active = 0;
    (void)vib_set_duty(DUTY_DT_PM);        /* vib.c's own users get its fixed drive again */
}

/* start segment H.i (or the next one that is not empty); called at the start and from vib.c's end hook */
static void advance(void)
{
    uint64_t now = sb_ms();
    for (unsigned guard = 0; H.active && guard < 2U * H.n + 4U; ++guard) {
        if (H.i >= H.n) {
            if (!H.loop || now - H.t_start >= LOOP_MAX_MS) {
                if (H.loop)
                    sb_log("haptic: the looped pattern ended after 60 s (play it again to go on)");
                done();
                return;
            }
            H.i = 0;
        }
        const struct haptic_seg *g = &H.s[H.i];
        unsigned ms = g->ms;
        if (!ms) {
            ++H.i;
            continue;
        }
        unsigned pm = haptic_duty_for(g->strength);
        if (!pm) {                          /* a gap */
            if (ms >= REST_MIN_MS)
                H.cont = 0;
            ++H.i;
            H.t_due = now + ms;
            vib_wait(ms);
            return;
        }
        if (ms > SEG_MAX_MS)
            ms = SEG_MAX_MS;
        if (H.cont + ms > CONT_MAX_MS) {    /* the 5 s limit: a rest first, the same segment after it */
            if (!H.forced++)
                sb_log("haptic: 5 s of drive without a rest: a 1 s rest inserted");
            H.cont = 0;
            H.t_due = now + FORCED_REST_MS;
            vib_wait(FORCED_REST_MS);
            return;
        }
        (void)vib_set_duty(pm);
        if (vib_pulse(ms)) {
            why = vib_why();
            sb_logf("haptic: the motor refused: ", why ? why : "?");
            done();
            return;
        }
        H.cont += ms;
        H.t_due = now + ms;
        ++H.i;
        return;
    }
    done();                                 /* nothing but empty segments */
}

static void end_hook(int ended)
{
    if (!H.active)
        return;
    if (!ended) {                           /* vib_off from anyone: the pattern stops with it */
        done();
        return;
    }
    if (sb_ms() + 2U < H.t_due)             /* the end of a pulse we did not start (SOS's Morse vib_pulse): not ours */
        return;
    advance();
}

int haptic_play_segs(const struct haptic_seg *s, unsigned n, int loop)
{
    if (!hooked) {
        hooked = 1;
        vib_set_end_hook(end_hook);
    }
    if (!s || !n || n > 64U)
        return SBOS_EINVAL;
    if (H.active && H.loop && !loop)
        return SBOS_EBUSY;                  /* a ringing pattern is not cut by a tick */
    if (level <= 0)
        return 0;                           /* the user's choice: nothing to feel */
    if (!vib_ready()) {
        why = vib_why();
        return SBOS_EAGAIN;
    }
    why = 0;
    if (H.active) {
        H.active = 0;                       /* (so vib_off's hook does not end the new one) */
        vib_off();
    }
    H.s = s;
    H.n = (uint8_t)n;
    H.i = 0;
    H.loop = loop ? 1 : 0;
    H.active = 1;
    H.t_start = sb_ms();
    H.cont = 0;
    advance();
    return H.active || !why ? 0 : SBOS_EAGAIN;
}

int haptic_play(int which)
{
    if (which < 0 || which >= HAPTIC_N)
        return SBOS_EINVAL;
    return haptic_play_segs(PAT[which].s, PAT[which].n, PAT[which].loop);
}

void haptic_stop(void)
{
    if (!H.active)
        return;
    H.active = 0;
    vib_off();
    done();
}

int haptic_playing(void)
{
    return H.active ? (H.loop ? 2 : 1) : 0;
}

void haptic_set_intensity(int l)
{
    level = l < 0 ? 0 : l > 3 ? 3 : l;
    if (!level)
        haptic_stop();
}

int haptic_intensity(void)
{
    return level;
}

const char *haptic_why(void)
{
    if (level <= 0)
        return "Vibration is off (intensity 0).";
    return why ? why : vib_ready() ? "ready" : vib_why();
}

/* ---- the probe (drvprobe.h) ----------------------------------------------------------------------------------- */
#define PWM_BASE 0x10510000UL
#define GPP3_CON 0x10430060UL
#define GPG1_CON 0x104300A0UL

static struct {
    int ph, saved_level;
    uint64_t t0, t_ph;
} P;

void drv_haptic_abort(void)
{
    haptic_stop();
    vib_off();
    if (P.ph)
        level = P.saved_level;
    memset(&P, 0, sizeof(P));
}

static int fin(int pass, const char *what)
{
    sb_logf(pass ? "drvprobe: haptic: PASS " : "drvprobe: haptic: FAIL ", what);
    if (P.ph)
        drv_haptic_abort();                 /* (level 0 touched nothing: nothing to stop) */
    return 1;
}

static int look(void)
{
    static const uintptr_t r[] = { PWM_BASE + 0x00, PWM_BASE + 0x04, PWM_BASE + 0x08, PWM_BASE + 0x18,
                                   PWM_BASE + 0x1C, GPP3_CON, GPG1_CON, GPG1_CON + 4 };
    static const char *const nm[] = { "TCFG0", "TCFG1", "TCON", "TCNTB1", "TCMPB1", "gpp3 CON", "gpg1 CON", "gpg1 DAT" };
    struct ub u;
    int ok = 1;
    ub_init(&u);
    ub_s(&u, "drvprobe: haptic: ");
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); ++i) {
        uint32_t v = 0;
        ub_s(&u, nm[i]);
        ub_c(&u, ' ');
        if (sb_probe32(r[i], &v)) {
            ub_hexn(&u, v, 8);
        } else {
            ub_s(&u, "unreadable");
            ok = 0;
        }
        ub_s(&u, i + 1 < sizeof(r) / sizeof(r[0]) ? ", " : "");
    }
    sb_log(u.b);
    ub_init(&u);
    ub_s(&u, "drvprobe: haptic: MAX77705 ");
    uint8_t m1 = 0, mc = 0;
    if (sb_max_bus.state <= 0) {
        ub_s(&u, "bus not up yet (not started at level 0): MAINCTRL1 / MCONFIG not read");
    } else if (sb_i2c_read(&sb_max_bus, 0x66, 0x02, &m1, 1) && sb_i2c_read(&sb_max_bus, 0x66, 0x10, &mc, 1)) {
        ub_s(&u, "MAINCTRL1 ");
        ub_hex(&u, m1, 2);
        ub_s(&u, " MCONFIG ");
        ub_hex(&u, mc, 2);
        ub_s(&u, mc & 0x40U ? " (the motor is ENABLED now)" : " (motor enable off)");
    } else {
        ub_s(&u, "MAINCTRL1 / MCONFIG did not read");
        ok = 0;
    }
    ub_s(&u, "; intensity ");
    ub_dec(&u, (uint64_t)level);
    sb_log(u.b);
    return ok;
}

int drv_haptic_probe(int lvl, unsigned *wait_ms)
{
    static const struct haptic_seg one[] = { { 300, 100 } };
    uint64_t now = sb_ms();
    *wait_ms = 20;
    if (lvl == DRV_LVL_READ)
        return look() ? fin(1, "read only: the PWM timer, the pins and the MAX77705's haptic bits read")
                      : fin(0, "a register did not read");
    if (lvl == DRV_LVL_ACTIVE) {
        sb_log("drvprobe: haptic: PASS skipped (attended only: a silent haptic test is impossible)");
        return 1;
    }
    if (!P.ph) {
        P.ph = 1;
        P.saved_level = level;
        P.t0 = P.t_ph = now;
        sb_log("drvprobe: haptic: attended: the notification taps, 3 s of the ring pattern, then light / medium / "
               "strong 300 ms pulses");
    }
    if (now - P.t0 > DRV_HAPTIC_BUDGET_L2 - 500U)
        return fin(0, "did not finish within its budget: the motor stopped");
    /* the motor's own timer (KT_UI 9) must end every segment: a late one stops everything */
    if (haptic_playing() && now > H.t_due + 300U)
        return fin(0, "a segment did not end on time (the KT_UI 9 timer did not fire): the motor stopped");
    switch (P.ph) {
    case 1:
        level = 3;
        if (haptic_play(HAPTIC_NOTIFY))
            return fin(0, haptic_why());
        P.ph = 2;
        P.t_ph = now;
        return 0;
    case 2:
        if ((haptic_playing() || now - P.t_ph < 700U) && now - P.t_ph < 1500U)
            return 0;                       /* the taps, then a short pause */
        if (haptic_play(HAPTIC_RING))
            return fin(0, haptic_why());
        P.ph = 3;
        P.t_ph = now;
        return 0;
    case 3:
        if (now - P.t_ph < 3000U)
            return 0;
        haptic_stop();
        if (vib_on())
            return fin(0, "the motor still ran after the ring pattern's stop");
        P.ph = 4;
        P.t_ph = now;
        return 0;
    case 4:
    case 5:
    case 6:
        if (now - P.t_ph < 700U)
            return 0;
        level = P.ph - 3;
        sb_logx("drvprobe: haptic: intensity (1 light, 2 medium, 3 strong) ", (uint64_t)level);
        if (haptic_play_segs(one, 1, 0))
            return fin(0, haptic_why());
        ++P.ph;
        P.t_ph = now;
        return 0;
    case 7:
        if (haptic_playing() && now - P.t_ph < 1000U)
            return 0;
        if (vib_on())
            return fin(0, "the motor still ran after the last pulse");
        return fin(1, "notification, 3 s of ring, then three strengths played; the motor is off (ask the person: "
                      "two taps, a buzz-buzz pause, then weak / medium / strong)");
    default:
        return fin(0, "lost its place");
    }
}
