/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: proximity, light, pressure and the gyroscope behind the BCM4775 sensor hub
 * (docs/drivers/SENSORS.md; API in shub_more.h).
 *
 * What it drives: nothing directly. shub.c (the survival session's) starts the hub, loads its firmware from the card
 * and runs the accelerometer, magnetometer and step counter; this file asks the running hub for more sensors through
 * shub.c's hook (shub_enable_type: Samsung's SSP "add sensor" 0xA1 / "remove sensor" 0xA2) and parses their samples
 * (shub_set_hooks). The parts, all behind the hub's own I2C (never the AP's): AMS TMD4906 light + proximity, ST
 * LPS22H barometer, ST LSM6DSL accelerometer + gyroscope (starlte defconfig CONFIG_SENSORS_SSP_TMD4906 / _LPS22H /
 * _LSM6DSL).
 *
 * Sample layouts (Samsung's GPL drivers/sensorhub/brcm/ssp_data.c get_*_sensordata and ssp.h struct sensor_value,
 * with starlte's defconfig: _LIGHT_REPORT_LUX on, _LIGHT_MAX_GAIN_2BYTE off, TMD4906 = not TMG399x; the same lengths
 * are in an earlier port's bcm_shub.h table, read for facts only):
 *   7 proximity   3 bytes  u8 prox_detect (1 = near; ssp_input.c reports !detect as the distance), u16 adc
 *   9 light      18 bytes  u32 lux, s32 cct, u16 r, g, b, w, u8 a_time, u8 a_gain
 *   5 pressure    6 bytes  s32 pressure, s16 temperature in 0.01 degrees C (pressure_lps22h.c prints it / 100). The
 *                          pressure's unit is not stated in the GPL source: the LPS22H's own is 1/4096 hPa, an earlier port's
 *                          notes say hundredths of hPa. Both are accepted here, told apart by the plausible range
 *                          (300..1100 hPa); the probe logs which one the phone sends.
 *   1 gyroscope  12 bytes  s32 x, y, z. Samsung's default uGyroDps is 1000 (no shift, ssp_dev.c / ssp_input.c);
 *                          70 mdps a count is the LSM6DSL's 2000 dps sensitivity (factory/gyro_lsm6dsl.c
 *                          DEF_GYRO_SENS) [GUESS until a turn on the phone confirms it].
 * Periods: 200 ms for proximity, light and pressure (what an earlier port used on this phone), 100 ms for the gyroscope.
 *
 * Power: a sensor runs only while asked for (each getter is a keep-alive; 10 s without one and it is removed again);
 * the in-call proximity and the probe pin theirs. The hub itself is kept running through survival.c's
 * surv_sensors_on (its 60 s idle stop then applies once nothing here is wanted).
 * Seen on a phone: nothing of this file yet (shub.c's accelerometer / magnetometer / steps ran 2026-11-11). */
#include "sb.h"
#include "shub.h"
#include "survival.h"
#include "shub_more.h"
#include "drvprobe.h"

#define KEEP_MS 10000U              /* a sensor nobody asked for in this long is switched off */
#define FRESH_MS 2000U              /* continuous sensors: a sample younger than this */
#define RETRY_FAILED_MS 30000U      /* a failed hub start is tried again after this */
#define NOSAMPLE_MS 3000U           /* enabled but silent this long: asked again (3 times) */

static const struct {
    uint8_t type, len, on_change;
    uint16_t ms;
    const char *name;
} T[4] = {
    { SENS_T_PROX, 3, 1, 200, "proximity" },
    { SENS_T_LIGHT, 18, 1, 200, "light" },
    { SENS_T_PRESSURE, 6, 0, 200, "pressure" },
    { SENS_T_GYRO, 12, 0, 100, "gyroscope" },
};

static struct {
    int inited;
    unsigned pin;                   /* pinned on: the in-call proximity, the probe's */
    unsigned on;                    /* switched on at the hub */
    unsigned starts;                /* shub_starts() that `on` belongs to */
    uint64_t asked[4], t_on[4], t_last[4], t_keep, t_try;
    uint32_t nsamp[4];
    uint8_t tries[4];
    int incall;
    int near;
    uint16_t prox_adc;
    uint32_t lux;
    int32_t cct, press_raw, hpa_x100, temp_x100, g[3];
    int press_scale;                /* 0 not told yet, 4096 or 100 */
    uint32_t bad, odd_press;
    int refused;                    /* survival refused to start the hub (Last 2 %) */
    const char *why;
    struct sens_stat st[SST_N];
} S;

static void more_sample(unsigned type, const uint8_t *d, unsigned n, uint64_t ts);
static void more_poll(void);

void sens_init(void)
{
    if (S.inited)
        return;
    S.inited = 1;
    S.near = -1;
    S.why = "The sensors start when a page asks for them.";
    shub_set_hooks(more_sample, more_poll);
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void stat(int k, int32_t v)
{
    struct sens_stat *s = &S.st[k];
    if (!s->n || v < s->mn)
        s->mn = v;
    if (!s->n || v > s->mx)
        s->mx = v;
    s->sum += v;
    ++s->n;
}

/* ---- samples (shub.c's hook) ---------------------------------------------------------------------------------- */
static void more_sample(unsigned type, const uint8_t *d, unsigned n, uint64_t ts)
{
    int i = -1;
    (void)ts;
    for (int k = 0; k < 4; ++k)
        if (T[k].type == type)
            i = k;
    if (i < 0 || !(S.on & (1U << i)))
        return;                             /* not one of ours, or arriving after its removal */
    if (!d || n != T[i].len) {
        ++S.bad;
        return;
    }
    S.t_last[i] = sb_ms();
    ++S.nsamp[i];
    switch (type) {
    case SENS_T_PROX:
        S.near = d[0] ? 1 : 0;
        S.prox_adc = le16(d + 1);
        stat(SST_PROX_ADC, S.prox_adc);
        stat(SST_PROX_NEAR, S.near);
        break;
    case SENS_T_LIGHT:
        S.lux = le32(d);
        S.cct = (int32_t)le32(d + 4);
        stat(SST_LUX, S.lux > 0x7fffffffU ? 0x7fffffff : (int32_t)S.lux);
        break;
    case SENS_T_PRESSURE: {
        int32_t r = (int32_t)le32(d);
        S.press_raw = r;
        S.temp_x100 = (int16_t)le16(d + 4);
        if (r >= 300 * 4096 && r <= 1100 * 4096) {
            S.press_scale = 4096;
            S.hpa_x100 = (int32_t)((int64_t)r * 100 / 4096);
        } else if (r >= 30000 && r <= 110000) {
            S.press_scale = 100;
            S.hpa_x100 = r;
        } else {
            S.press_scale = 0;
            if (!S.odd_press++)
                sb_logx("sens: a pressure sample outside 300..1100 hPa in both known units, raw ", (uint32_t)r);
            break;
        }
        stat(SST_HPA, S.hpa_x100);
        stat(SST_TEMP, S.temp_x100);
        break;
    }
    case SENS_T_GYRO:
        for (int a = 0; a < 3; ++a) {
            int32_t c = (int32_t)le32(d + 4 * a);
            if (c > 30000000 || c < -30000000)
                c = c > 0 ? 30000000 : -30000000;     /* (x 70 must fit) */
            S.g[a] = c * 70;
            stat(SST_GX + a, S.g[a]);
        }
        break;
    }
}

/* ---- what runs at the hub ------------------------------------------------------------------------------------- */
static int wanted(int i, uint64_t now)
{
    return (S.pin & (1U << i)) || (S.asked[i] && now - S.asked[i] < KEEP_MS);
}

static unsigned wanted_mask(uint64_t now)
{
    unsigned m = 0;
    for (int i = 0; i < 4; ++i)
        if (wanted(i, now))
            m |= 1U << i;
    return m;
}

static void forget(int i)
{
    S.nsamp[i] = 0;
    if (i == 0)
        S.near = -1;
}

/* one request to the hub at a time (shub.c's single SSP slot): enable what is wanted, remove what is not */
static void service(uint64_t now)
{
    if (shub_state() != SHUB_RUNNING) {
        for (int i = 0; i < 4; ++i)
            if (S.on & (1U << i))
                forget(i);
        S.on = 0;
        return;
    }
    if (shub_starts() != S.starts) {        /* a new start of the hub: only its own three sensors run */
        S.starts = shub_starts();
        for (int i = 0; i < 4; ++i)
            forget(i);
        S.on = 0;
    }
    for (int i = 0; i < 4; ++i) {
        unsigned b = 1U << i;
        int w = wanted(i, now);
        if (w && !(S.on & b)) {
            if (!shub_enable_type(T[i].type, T[i].ms, 1)) {
                S.on |= b;
                S.t_on[i] = now;
                S.tries[i] = 1;
                forget(i);
                sb_logf("sens: on: ", T[i].name);
            }
            return;
        }
        if (!w && (S.on & b)) {
            if (!shub_enable_type(T[i].type, T[i].ms, 0)) {
                S.on &= ~b;
                forget(i);
                sb_logf("sens: off: ", T[i].name);
            }
            return;
        }
        if (w && !S.nsamp[i] && now - S.t_on[i] > NOSAMPLE_MS && S.tries[i] < 3) {
            if (!shub_enable_type(T[i].type, T[i].ms, 1)) {
                S.t_on[i] = now;
                ++S.tries[i];
                sb_logf("sens: no sample in 3 s, asked again: ", T[i].name);
            }
            return;
        }
    }
}

static int probe_owns;

/* the hub running for us: through survival.c (the Compass page's way), never over someone else's start */
static void keep_hub(uint64_t now)
{
    if (probe_owns)
        return;
    int ss = surv_sensors_state(), hs = shub_state();
    if (ss == SHUB_OFF && (hs == SHUB_RUNNING || hs == SHUB_STARTING))
        return;                             /* started by someone else (a test image): theirs to keep */
    if (ss == SHUB_RUNNING || ss == SHUB_STARTING) {
        S.refused = 0;
        if (!S.t_keep || now - S.t_keep >= 1000U) {
            S.t_keep = now;
            (void)surv_sensors_on(1);
        }
        return;
    }
    if (S.t_try && now - S.t_try < (hs == SHUB_FAILED ? RETRY_FAILED_MS : 2000U))
        return;
    S.t_try = now;
    S.refused = surv_sensors_on(1) < 0;
    if (S.refused)
        S.why = "The sensors are off in Last 2 % mode.";
}

/* shub.c's hook: the end of every shub_poll (every 50 ms while the hub runs) */
static void more_poll(void)
{
    uint64_t now = sb_ms();
    if (wanted_mask(now))
        keep_hub(now);
    service(now);
}

static void ask(unsigned mask)
{
    uint64_t now = sb_ms();
    sens_init();
    for (int i = 0; i < 4; ++i)
        if (mask & (1U << i))
            S.asked[i] = now ? now : 1;
    if (wanted_mask(now))
        keep_hub(now);
    service(now);
}

/* ---- the API (shub_more.h) ------------------------------------------------------------------------------------ */
int sens_want(unsigned mask)
{
    ask(mask & SENS_ALL);
    return S.refused ? -1 : 0;
}

void sens_release(unsigned mask)
{
    sens_init();
    for (int i = 0; i < 4; ++i)
        if (mask & (1U << i)) {
            S.asked[i] = 0;
            if (!(i == 0 && S.incall))
                S.pin &= ~(1U << i);
        }
    service(sb_ms());
}

int sens_prox_near(void)
{
    ask(SENS_PROX);
    return (S.on & SENS_PROX) && S.nsamp[0] ? S.near : -1;
}

int sens_lux(uint32_t *lux)
{
    ask(SENS_LIGHT);
    if (!(S.on & SENS_LIGHT) || !S.nsamp[1])
        return 0;
    *lux = S.lux;
    return 1;
}

int sens_pressure_hpa(int32_t *hpa_x100, int32_t *temp_c_x100)
{
    ask(SENS_PRESSURE);
    if (!(S.on & SENS_PRESSURE) || !S.nsamp[2] || !S.press_scale || sb_ms() - S.t_last[2] >= FRESH_MS)
        return 0;
    *hpa_x100 = S.hpa_x100;
    if (temp_c_x100)
        *temp_c_x100 = S.temp_x100;
    return 1;
}

int sens_gyro_mdps(int32_t out[3])
{
    ask(SENS_GYRO);
    if (!(S.on & SENS_GYRO) || !S.nsamp[3] || sb_ms() - S.t_last[3] >= FRESH_MS)
        return 0;
    for (int a = 0; a < 3; ++a)
        out[a] = S.g[a];
    return 1;
}

void sens_incall(int on)
{
    sens_init();
    if (on == S.incall)
        return;
    S.incall = on;
    if (on) {
        S.pin |= SENS_PROX;
        sb_log("sens: in a call: the proximity sensor on (200 ms)");
        ask(SENS_PROX);
    } else {
        S.pin &= ~SENS_PROX;
        S.asked[0] = 0;
        sb_log("sens: the call ended: the proximity sensor off unless asked");
        service(sb_ms());
    }
}

int sens_incall_screen_off(void)
{
    if (!S.incall)
        return 0;
    return sens_prox_near() == 1;
}

int sens_auto_brightness(void)
{
    uint32_t lux = 0;
    if (!sens_lux(&lux))
        return -1;
    /* piecewise linear over decades: 0 -> 6, 1 -> 10, 10 -> 40, 100 -> 90, 1000 -> 170, 10000 -> 255 */
    static const int lv[6] = { 6, 10, 40, 90, 170, 255 };
    if (lux == 0)
        return lv[0];
    uint32_t lo = 1;
    int k = 1;
    while (k < 5 && lux >= lo * 10U) {
        lo *= 10U;
        ++k;
    }
    if (k == 5)
        return lv[5];                       /* 10000 lux and more */
    /* lux in [lo, 10 lo): between lv[k] and lv[k + 1] */
    return lv[k] + (int)((uint64_t)(lv[k + 1] - lv[k]) * (lux - lo) / (9U * lo));
}

const char *sens_why(void)
{
    if (shub_state() == SHUB_FAILED)
        return shub_why();
    if (shub_state() == SHUB_STARTING)
        return "Starting the sensor hub (about 4 seconds).";
    if (shub_state() == SHUB_RUNNING)
        return S.on ? "Waiting for the sensor's first sample." : "The sensor hub is running.";
    return S.why ? S.why : "The sensors are off.";
}

const struct sens_stat *sens_stats(void)
{
    return S.st;
}

unsigned sens_running_mask(void)
{
    return S.on;
}

/* ---- the probe (drvprobe.h) ----------------------------------------------------------------------------------- */
int senstat_look(const char **sum);         /* senstat.c */
extern const char *senstat_why;

static struct {
    int ph, lvl;
    uint64_t t0, t_run, t_all, t_rel;
} P;

static int has(const char *s, const char *w)
{
    for (; s && *s; ++s) {
        unsigned k = 0;
        while (w[k] && s[k] == w[k])
            ++k;
        if (!w[k])
            return 1;
    }
    return 0;
}

static void probe_reset(void)
{
    memset(&P, 0, sizeof(P));
}

void drv_sens_abort(void)
{
    S.pin = 0;
    S.incall = 0;
    for (int i = 0; i < 4; ++i)
        S.asked[i] = 0;
    if (probe_owns) {
        probe_owns = 0;
        shub_leave();                       /* standby: the hub's sensors stop with it, every register as found */
        S.on = 0;
    } else {
        service(sb_ms());
    }
    probe_reset();
}

static void log_stat(struct ub *u, const char *name, int k, int div)
{
    const struct sens_stat *s = &S.st[k];
    ub_s(u, name);
    if (!s->n) {
        ub_s(u, " none; ");
        return;
    }
    ub_s(u, " n ");
    ub_dec(u, s->n);
    ub_s(u, " min ");
    ub_sdec(u, s->mn / div);
    ub_s(u, " max ");
    ub_sdec(u, s->mx / div);
    ub_s(u, " mean ");
    ub_sdec(u, (s->sum / (int64_t)s->n) / div);
    ub_s(u, "; ");
}

static int finish(int pass, const char *what)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, pass ? "drvprobe: sens: PASS " : "drvprobe: sens: FAIL ");
    ub_s(&u, what);
    sb_log(u.b);
    probe_reset();
    return 1;
}

int drv_sens_probe(int level, unsigned *wait_ms)
{
    uint64_t now = sb_ms();
    *wait_ms = 20;
    sens_init();
    if (!P.ph) {
        P.ph = 1;
        P.lvl = level;
        P.t0 = now;
    }
    if (level == DRV_LVL_READ) {
        const char *sum = 0;
        int ok = senstat_look(&sum);
        sb_log(shub_state() == SHUB_RUNNING ? "drvprobe: sens: the hub runs (started by someone else)" :
                                              "drvprobe: sens: the hub is not running (level 0 does not start it)");
        if (!ok) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "a status register did not read (");
            ub_s(&u, senstat_why);
            ub_s(&u, ")");
            return finish(0, u.b);
        }
        return finish(1, sum ? sum : "read only: all read");
    }
    if (level >= DRV_LVL_ATTENDED)
        return finish(1, "nothing to see at level 2 (the heart-rate LEDs are never lit by this driver)");

    /* level 1: the wireless charger and the heart-rate ID (one look each), then the hub */
    if (now - P.t0 > DRV_SENS_BUDGET_L1 - 1000U) {
        drv_sens_abort();
        return finish(0, "did not finish within its budget: everything put back");
    }
    switch (P.ph) {
    case 1: {
        struct sens_wpc w;
        uint8_t id = 0;
        int r = sens_wpc_read(&w);
        sb_log(r > 0 ? "drvprobe: sens: P9320 answered (see the line above)" :
               r == 0 ? "drvprobe: sens: P9320 did not answer (no pad: normal)" :
                        "drvprobe: sens: P9320 bus not usable (see above)");
        if (sens_hrm_part_id(&id))
            sb_logx("drvprobe: sens: MAX86915 part ID (0x2B expected) ", id);
        else
            sb_logf("drvprobe: sens: MAX86915 part ID not read: ", senstat_why);
        P.ph = 2;
        return 0;
    }
    case 2:                                 /* the hub: someone else's if it runs, else our own start */
        memset(S.st, 0, sizeof(S.st));
        if (shub_state() == SHUB_RUNNING) {
            sb_log("drvprobe: sens: the hub already runs: using it");
            P.ph = 4;
            return 0;
        }
        if (shub_state() == SHUB_STARTING) {
            sb_log("drvprobe: sens: the hub is starting (someone else's): waiting");
            P.ph = 3;
            return 0;
        }
        sb_log("drvprobe: sens: starting the hub (its firmware from the card, as the Compass page does)");
        probe_owns = 1;
        shub_begin(0);
        P.ph = 3;
        return 0;
    case 3:
        if (probe_owns) {
            if (!shub_step()) {
                *wait_ms = shub_wait_ms();
                return 0;
            }
        } else if (shub_state() == SHUB_STARTING) {
            *wait_ms = 100;
            return 0;
        }
        if (shub_state() != SHUB_RUNNING) {
            const char *why = shub_why();
            int card = has(why, "not on the card");
            if (card)
                sb_log("drvprobe: sens: the hub's firmware is not on the card (/STARBITE/FW/BCM4775.BIN): ending");
            if (probe_owns) {
                probe_owns = 0;
                shub_leave();
            }
            return finish(0, card ? "the sensor hub's firmware is not on the card: nothing measured" : why);
        }
        P.ph = 4;
        return 0;
    case 4:                                 /* the four on, pinned */
        S.pin = SENS_ALL;
        P.t_run = now;
        P.t_all = 0;
        P.ph = 5;
        return 0;
    case 5:
        shub_poll();                        /* (our hook runs at its end: the enables, one at a time) */
        if (!P.t_all && S.on == SENS_ALL)
            P.t_all = now;
        if ((P.t_all && now - P.t_all >= 6000U) || now - P.t_run >= 10000U) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "drvprobe: sens: ");
            log_stat(&u, "prox adc", SST_PROX_ADC, 1);
            log_stat(&u, "near", SST_PROX_NEAR, 1);
            log_stat(&u, "lux", SST_LUX, 1);
            sb_log(u.b);
            ub_init(&u);
            ub_s(&u, "drvprobe: sens: ");
            log_stat(&u, "hPa", SST_HPA, 100);
            log_stat(&u, "temp C", SST_TEMP, 100);
            ub_s(&u, "pressure unit ");
            ub_s(&u, S.press_scale == 4096 ? "1/4096 hPa" : S.press_scale == 100 ? "1/100 hPa" : "not told");
            sb_log(u.b);
            ub_init(&u);
            ub_s(&u, "drvprobe: sens: gyro mdps ");
            log_stat(&u, "x", SST_GX, 1);
            log_stat(&u, "y", SST_GY, 1);
            log_stat(&u, "z", SST_GZ, 1);
            ub_s(&u, "bad lengths ");
            ub_dec(&u, S.bad);
            sb_log(u.b);
            S.pin = 0;
            P.t_rel = now;
            P.ph = 6;
        }
        return 0;
    case 6: {                               /* the four off again, then the hub as found */
        shub_poll();
        if (S.on && now - P.t_rel < 3000U)
            return 0;
        int left = (int)S.on;
        if (probe_owns) {
            probe_owns = 0;
            shub_leave();
            S.on = 0;
        }
        struct ub u;
        ub_init(&u);
        int miss = 0;
        static const int key[4] = { SST_PROX_ADC, SST_LUX, SST_HPA, SST_GX };
        for (int i = 0; i < 4; ++i)
            if (!S.st[key[i]].n) {
                ub_s(&u, miss++ ? ", " : "no samples from: ");
                ub_s(&u, T[i].name);
            }
        if (miss) {
            ub_s(&u, left ? "; (a removal was not echoed)" : "");
            return finish(0, u.b);
        }
        ub_s(&u, "proximity ");
        ub_s(&u, S.st[SST_PROX_NEAR].mx ? "saw near" : "far");
        ub_s(&u, ", light mean ");
        ub_sdec(&u, S.st[SST_LUX].sum / (int64_t)S.st[SST_LUX].n);
        ub_s(&u, " lux, ");
        ub_sdec(&u, S.st[SST_HPA].sum / (int64_t)S.st[SST_HPA].n / 100);
        ub_s(&u, " hPa, gyroscope ");
        ub_dec(&u, S.st[SST_GX].n);
        ub_s(&u, " samples; all four off again");
        if (left)
            ub_s(&u, " (a removal was not echoed; the hub's standby stopped it)");
        return finish(1, u.b);
    }
    default:
        return finish(0, "lost its place");
    }
}
