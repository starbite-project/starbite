/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/haptic.c on kern/vib.c (built with -DSB_HOST: the motor is a state, "on" or "off"), against a
 * simulated millisecond clock and vib.c's one timer (KT_UI 9). Checks: each standard pattern's on / off timeline,
 * the strength -> duty mapping (never above the DT's 70 %), the intensity setting, the ring loop and its stop, the
 * 60 s loop end, the 5 s continuous-drive limit, one-shots not cutting a ring, a foreign vib_off stopping a pattern,
 * vib.c's own vib_buzz unaffected, and the probe at levels 0, 1, 2 (also with a timer that never fires, and an abort).
 * Build and run: starbite/sbos/kern/host/haptic-test.sh (the Mac's cc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"
#include "haptic.h"
#include "drvprobe.h"

static int fails, checks, verbose;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fakes: time, the log, the timer, the probe's register reads ---- */
static uint64_t now_ms = 100000;
uint64_t sb_ms(void) { return now_ms; }
uint64_t sb_us(void) { return now_ms * 1000; }
static char logbuf[1 << 18];
static unsigned logn;
void sb_log(const char *s)
{
    size_t n = strlen(s);
    if (logn + n + 2 < sizeof logbuf) {
        memcpy(logbuf + logn, s, n);
        logn += (unsigned)n;
        logbuf[logn++] = '\n';
        logbuf[logn] = 0;
    }
    if (verbose)
        printf("    | %s\n", s);
}
void sb_logf(const char *a, const char *b)
{
    char t[600];
    snprintf(t, sizeof t, "%s%s", a, b ? b : "(null)");
    sb_log(t);
}
void sb_logx(const char *a, uint64_t v)
{
    char t[600];
    snprintf(t, sizeof t, "%s0x%llx", a, (unsigned long long)v);
    sb_log(t);
}
static void log_clear(void) { logn = 0; logbuf[0] = 0; }

static int t_armed = 0, t_fire_ok = 1;
static uint64_t t_due;
int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat)
{
    (void)repeat;
    if (owner == KT_UI && id == 9) {
        t_armed = 1;
        t_due = now + ms;
    }
    return 0;
}
void kt_cancel(unsigned owner, unsigned id)
{
    if (owner == KT_UI && id == 9)
        t_armed = 0;
}

static int probe_fail_at = -1, probe_reads, max_up = 1;
int sb_probe32(uint64_t addr, uint32_t *v)
{
    (void)addr;
    *v = 0x1234;
    return probe_reads++ != probe_fail_at;
}
struct sb_i2c sb_max_bus = { 0x14360000UL, 0, 0, 0, 1 };
static int i2c_reads;
int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    (void)b;
    (void)len;
    ++i2c_reads;
    if (addr != 0x66)
        return 0;
    *data = reg == 0x10 ? 0x82 : 0x00;
    return max_up;
}

/* ---- the motor's timeline ---- */
struct edge { uint64_t t; int on; };
static struct edge ed[100000];
static unsigned ned;
static int last_on;
static uint64_t t0;
static void sample(void)
{
    int o = vib_on();
    if (o != last_on && ned < sizeof ed / sizeof ed[0]) {
        ed[ned].t = now_ms - t0;
        ed[ned].on = o;
        ++ned;
    }
    last_on = o;
}
static void tl_reset(void)
{
    ned = 0;
    last_on = vib_on();
    t0 = now_ms;
}
/* ms of simulated time, 1 ms steps; the timer fires when due */
static void run(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        ++now_ms;
        if (t_armed && t_fire_ok && now_ms >= t_due) {
            t_armed = 0;
            vib_timer();
        }
        sample();
    }
}
static int on_ms_total(void)
{
    int tot = 0;
    for (unsigned i = 0; i + 1 < ned; ++i)
        if (ed[i].on)
            tot += (int)(ed[i + 1].t - ed[i].t);
    return tot;
}
/* the longest stretch of drive whose internal gaps are all shorter than 250 ms */
static int longest_cont(void)
{
    int best = 0;
    int cur = 0;
    for (unsigned i = 0; i + 1 < ned; ++i) {
        int d = (int)(ed[i + 1].t - ed[i].t);
        if (ed[i].on)
            cur += d;
        else if (d >= 250)
            cur = 0;
        if (cur > best)
            best = cur;
    }
    return best;
}
static void dump(const char *what)
{
    if (!verbose)
        return;
    printf("  %s:", what);
    for (unsigned i = 0; i < ned && i < 40; ++i)
        printf(" %s@%llu", ed[i].on ? "on" : "off", (unsigned long long)ed[i].t);
    printf("\n");
}

static int run_probe(int level, unsigned max_ms, int *calls)
{
    unsigned w = 0;
    *calls = 0;
    for (unsigned t = 0; t < max_ms;) {
        ++*calls;
        if (drv_haptic_probe(level, &w))
            return 1;
        if (w > 100)
            w = 100;
        run(w ? w : 1);
        t += w ? w : 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1;
    (void)argv;
    printf("haptic host test\n");

    /* the strength -> duty mapping */
    CHECK(haptic_intensity() == 3, "default intensity 3 (today's drive)");
    CHECK(haptic_duty_for(100) == 700, "level 3, full strength = the DT's 70 %% (%u)", haptic_duty_for(100));
    CHECK(haptic_duty_for(0) == 0, "strength 0 = no drive");
    haptic_set_intensity(2);
    CHECK(haptic_duty_for(100) == 650, "level 2, full = 650 (%u)", haptic_duty_for(100));
    haptic_set_intensity(1);
    CHECK(haptic_duty_for(100) == 600, "level 1, full = 600 (%u)", haptic_duty_for(100));
    CHECK(haptic_duty_for(10) == 540, "a weak strength is floored at 54 %% (%u)", haptic_duty_for(10));
    int over = 0;
    for (int l = -2; l <= 6; ++l) {
        haptic_set_intensity(l);
        for (unsigned s = 0; s <= 300; ++s) {
            unsigned d = haptic_duty_for(s);
            if (d && (d < 540 || d > 700))
                over = 1;
        }
    }
    CHECK(!over, "no strength at any intensity leaves 540..700 per mille");
    CHECK(vib_set_duty(950) == 700 && vib_set_duty(100) == 500 && vib_set_duty(700) == 700, "vib_set_duty clamps to 500..700");
    haptic_set_intensity(3);

    /* the notification: on 60, off 120, on 60 */
    tl_reset();
    CHECK(haptic_play(HAPTIC_NOTIFY) == 0 && haptic_playing() == 1, "notification starts");
    sample();
    run(400);
    dump("notification");
    CHECK(ned == 4 && ed[0].on && ed[0].t == 0 && !ed[1].on && ed[1].t == 60 && ed[2].on && ed[2].t == 180 && !ed[3].on &&
              ed[3].t == 240, "notification timeline (%u edges)", ned);
    CHECK(!haptic_playing() && !vib_on(), "notification ended, motor off");

    /* every standard pattern ends with the motor off; the one-shots end by themselves */
    for (int p = 0; p < HAPTIC_N; ++p) {
        tl_reset();
        CHECK(haptic_play(p) == 0, "%s plays", haptic_name(p));
        run(7000);
        if (p == HAPTIC_RING || p == HAPTIC_ALARM) {
            CHECK(haptic_playing() == 2, "%s still loops after 7 s", haptic_name(p));
            haptic_stop();
        }
        CHECK(!haptic_playing() && !vib_on(), "%s: off at the end", haptic_name(p));
        CHECK(longest_cont() <= 5000, "%s: never more than 5 s of drive without a rest", haptic_name(p));
    }

    /* the ring: 800 on / 400 off / 800 on / 2000 off, looped; 40 % on */
    tl_reset();
    haptic_play(HAPTIC_RING);
    sample();
    run(8000);
    dump("ring");
    CHECK(ned >= 8 && ed[0].on && !ed[1].on && ed[1].t == 800 && ed[2].on && ed[2].t == 1200 && !ed[3].on && ed[3].t == 2000 &&
              ed[4].on && ed[4].t == 4000, "ring timeline and its loop");
    CHECK(haptic_play(HAPTIC_TICK) == -16 && haptic_playing() == 2, "a tick does not cut the ring");
    haptic_stop();
    CHECK(!vib_on() && !haptic_playing() && !t_armed, "ring stopped: motor off, no timer left");

    /* a foreign vib_off (a call answered) stops the pattern */
    haptic_play(HAPTIC_RING);
    run(100);
    vib_off();
    run(5000);
    CHECK(!haptic_playing() && !vib_on(), "vib_off from elsewhere ends the ring");

    /* the loop ends by itself after 60 s */
    tl_reset();
    haptic_play(HAPTIC_ALARM);
    run(59000);
    CHECK(haptic_playing() == 2, "alarm loops at 59 s");
    run(5000);
    CHECK(!haptic_playing() && !vib_on(), "alarm loop ended by itself after 60 s");
    CHECK(strstr(logbuf, "ended after 60 s") != 0, "the 60 s end is logged");

    /* the 5 s limit: 4 x 3 s "on" with no gap, looped */
    static const struct haptic_seg hard[] = { { 3000, 100 }, { 3000, 100 }, { 100, 0 }, { 3000, 100 }, { 3000, 100 } };
    tl_reset();
    log_clear();
    CHECK(haptic_play_segs(hard, 5, 1) == 0, "an abusive pattern is accepted");
    sample();
    run(30000);
    haptic_stop();
    sample();
    dump("hard");
    int maxseg = 0;
    for (unsigned i = 0; i + 1 < ned; ++i)
        if (ed[i].on && (int)(ed[i + 1].t - ed[i].t) > maxseg)
            maxseg = (int)(ed[i + 1].t - ed[i].t);
    CHECK(longest_cont() <= 5000, "never more than 5 s of drive without a 250 ms rest (%d)", longest_cont());
    CHECK(strstr(logbuf, "a 1 s rest inserted") != 0, "the forced rest is logged");
    CHECK(on_ms_total() <= 30000 * 5 / 6 + 2000, "the forced rests keep the drive below about 5/6 (%d ms)", on_ms_total());
    (void)maxseg;

    /* intensity 0: nothing driven */
    haptic_set_intensity(0);
    tl_reset();
    CHECK(haptic_play(HAPTIC_RING) == 0 && !haptic_playing(), "intensity 0: nothing plays");
    run(2000);
    CHECK(ned == 0 && !vib_on(), "intensity 0: the motor never ran");
    haptic_set_intensity(3);

    /* vib.c's own pattern player still works and is not disturbed by the hook */
    tl_reset();
    CHECK(vib_buzz(2, 100, 100) == 0, "vib_buzz still works");
    sample();
    run(500);
    CHECK(ned == 4 && !vib_on() && !haptic_playing(), "vib_buzz's two pulses (%u edges)", ned);

    /* bad arguments */
    CHECK(haptic_play(-1) == -22 && haptic_play(HAPTIC_N) == -22 && haptic_play_segs(0, 1, 0) == -22, "bad arguments refused");

    /* ---- the probe ---- */
    int calls = 0;
    log_clear();
    i2c_reads = 0;
    CHECK(run_probe(DRV_LVL_READ, 100, &calls) && calls == 1, "level 0: one call");
    CHECK(strstr(logbuf, "drvprobe: haptic: PASS read only") && strstr(logbuf, "MCONFIG 0x82") && !vib_on(), "level 0 passes, reads MCONFIG");
    CHECK(i2c_reads == 2, "level 0: two MAX77705 reads (%d)", i2c_reads);
    log_clear();
    probe_reads = 0;
    probe_fail_at = 2;
    run_probe(DRV_LVL_READ, 100, &calls);
    CHECK(strstr(logbuf, "drvprobe: haptic: FAIL a register did not read") != 0, "level 0: a faulting read fails");
    probe_fail_at = -1;
    log_clear();
    max_up = 0;
    sb_max_bus.state = 0;
    run_probe(DRV_LVL_READ, 100, &calls);
    CHECK(strstr(logbuf, "bus not up yet") && strstr(logbuf, "PASS"), "level 0: MAX77705 bus down: said so, not started");
    sb_max_bus.state = 1;
    max_up = 1;
    log_clear();
    CHECK(run_probe(DRV_LVL_ACTIVE, 100, &calls) && strstr(logbuf, "drvprobe: haptic: PASS skipped (attended only"), "level 1: skipped");
    CHECK(ned == 4 && !vib_on(), "level 1: the motor never ran");

    log_clear();
    tl_reset();
    haptic_set_intensity(2);
    CHECK(run_probe(DRV_LVL_ATTENDED, DRV_HAPTIC_BUDGET_L2, &calls), "level 2 finishes within its budget");
    sample();
    dump("level 2");
    CHECK(strstr(logbuf, "drvprobe: haptic: PASS notification") != 0, "level 2 passes");
    CHECK(!vib_on() && !haptic_playing() && haptic_intensity() == 2, "level 2: motor off, the user's intensity back");
    CHECK(ned >= 14, "level 2: notification + ring + three pulses moved the motor (%u edges)", ned);
    CHECK(longest_cont() <= 5000, "level 2: within the 5 s limit");

    log_clear();
    t_fire_ok = 0;                          /* the KT_UI 9 timer never fires */
    CHECK(run_probe(DRV_LVL_ATTENDED, DRV_HAPTIC_BUDGET_L2, &calls), "level 2 with a dead timer ends");
    CHECK(strstr(logbuf, "drvprobe: haptic: FAIL a segment did not end on time") != 0, "dead timer: FAIL in plain words");
    CHECK(!vib_on() && !haptic_playing(), "dead timer: the motor was stopped");
    t_fire_ok = 1;

    log_clear();
    unsigned w = 0;
    drv_haptic_probe(DRV_LVL_ATTENDED, &w);
    run(50);
    drv_haptic_probe(DRV_LVL_ATTENDED, &w);
    run(50);
    drv_haptic_abort();
    CHECK(!vib_on() && !haptic_playing() && haptic_intensity() == 2, "abort mid-way: motor off, intensity back");

    printf("haptic host test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
