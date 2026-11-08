/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS small services: SOS (docs 9), the clock (the Utility OS's tz.c), the battery and its model
 * (docs 4.5), and power (docs 4.1, 4.6). */
#include "sb.h"
#include "kern.h"
#include "kinput.h"
#include "tz.h"

/* ---- SOS ------------------------------------------------------------------------------------- */
/* state 0 off, 2 the countdown (3 s, a hold cancels), 1 flashing: white / red at 2 Hz */
static int sos_st;
int sos_led_on;
static uint64_t sos_t0, sos_phase_t;
static int sos_phase;

int sos_active(void)
{
    return sos_st != 0;
}

int sos_set(int on)
{
    if (on && !sos_st) {
        sos_st = 2;
        sos_t0 = sb_ms() + 3000;
        sos_phase = 0;
        kt_set(KT_SOS, 1, sb_ms(), 250, 1);
        sb_log("sos: countdown");
    } else if (!on && sos_st) {
        sos_st = 0;
        kt_cancel(KT_SOS, 1);
        if (sos_led_on)
            torch_set(0);
        sb_log("sos: off");
    }
    kui_sos_changed();
    app_event_send(app_by_id("sos"), EV_APP_SERVICE, 2, sos_st);
    return 0;
}

void sos_timer(unsigned id)
{
    if (id == 9) {
        torch_timer(id);
        return;
    }
    uint64_t now = sb_ms();
    if (sos_st == 2 && now >= sos_t0) {
        sos_st = 1;
        sos_phase_t = now;
        sb_log("sos: flashing");
        app_event_send(app_by_id("sos"), EV_APP_SERVICE, 2, sos_st);
    }
    if (sos_st == 1) {
        sos_phase = (int)(((now - sos_phase_t) / 250U) & 1U);
        if (sos_led_on)
            torch_set(!sos_phase);              /* the LED with the white phase */
    }
    ui_mark_dirty();
}

int sos_frame(uint32_t *argb)
{
    if (!sos_st)
        return 0;
    *argb = sos_st == 2 ? 0xff1c0606U : sos_phase ? 0xffff1a1aU : 0xffffffffU;
    return 1;
}

void sos_status(struct sbos_sos *s)
{
    memset(s, 0, sizeof(*s));
    s->state = sos_st;
    uint64_t now = sb_ms();
    if (sos_st == 2)
        s->secs = -(int32_t)((sos_t0 - now + 999U) / 1000U);
    else if (sos_st == 1)
        s->secs = (int32_t)((now - sos_phase_t) / 1000U);
    s->torch = sos_led_on && torch_ready();
    s->siren = 0;
}

void sos_init(void)
{
    sos_st = 0;
}

/* ---- clock: the RTC (UTC) once a minute, the counter in between --------------------------------- */
static int64_t rtc_t;
static uint64_t rtc_ms, rtc_try;
static int rtc_state;

int clock_now(struct sbos_time *t)
{
    uint64_t now = sb_ms();
    if (!rtc_state || now - rtc_try >= 60000U) {
        uint64_t e;
        rtc_try = now;
        if (sb_rtc_epoch(&e) && e > 1577836800ULL) {
            rtc_t = (int64_t)e;
            rtc_ms = now;
            rtc_state = 1;
        } else if (rtc_state != 1) {
            rtc_state = -1;
        }
    }
    memset(t, 0, sizeof(*t));
    if (rtc_state != 1)
        return 0;
    int64_t utc = rtc_t + (int64_t)((now - rtc_ms) / 1000U);
    struct tz_time l;
    tz_local(&tz_zones[0], utc, &l);
    t->utc = utc;
    t->y = l.y;
    t->mo = l.mo;
    t->d = l.d;
    t->h = l.h;
    t->mi = l.mi;
    t->s = l.s;
    t->wday = l.wday;
    t->dst = l.dst;
    t->off_min = tz_zones[0].off_min + (l.dst ? 60 : 0);
    sb_strcpy(t->abbr, sizeof(t->abbr), l.abbr);
    sb_strcpy(t->zone, sizeof(t->zone), tz_zones[0].city);
    return 1;
}

/* ---- battery ------------------------------------------------------------------------------- */
static struct bat_model model;
static struct sbos_battery bat;
static int full_mah = 3000;

static void bat_read(void)
{
    struct sb_bat b;
    memset(&bat, 0, sizeof(bat));
    bat.temp_c10 = -1000;
    if (!sb_bat_read(&b) || !b.ok)
        return;
    bat.ok = 1;
    bat.pct = b.soc_pct;
    bat.pct_x10 = b.soc_x10;
    bat.mv = b.mv;
    bat.ma = b.ma;
    bat.avg_ma = b.avg_ma;
    bat.charger = b.input;
    uint16_t v;
    /* RepCap (0x05) and FullCapRep (0x10): ModelGauge 0.5 mAh per LSB with the 10 mOhm sense resistor */
    if (sb_fg_word_checked(0x05, &v, 0, 0))
        bat.cap_mah = v / 2;
    if (sb_fg_word_checked(0x10, &v, 0, 0) && v / 2 > 1000 && v / 2 < 4500)
        full_mah = v / 2;
    bat.full_mah = full_mah;
    if (!bat.cap_mah)
        bat.cap_mah = full_mah * bat.pct / 100;
    int r = sb_adc_filtered(4, 0, 0, 0);
    if (r >= 0)
        bat.temp_c10 = sb_therm_convert(1, r);
}

void battery_get(struct sbos_battery *b)
{
    *b = bat;
}

/* 0.5.6: the charge every 10 minutes for the last 8 hours (48 values; a gap where it was not read) */
#define HIST_N 48
#define HIST_MS 600000U
static int16_t hist[HIST_N];
static unsigned hist_head, hist_n;
static uint64_t hist_t;

static void hist_add(void)
{
    uint64_t now = sb_ms();
    if (hist_n && now - hist_t < HIST_MS)
        return;
    /* missed slots (the phone asleep longer than a slot): gaps */
    while (hist_n && now - hist_t >= 2U * HIST_MS && hist_n < 1000) {
        hist[hist_head] = -32768;
        hist_head = (hist_head + 1) % HIST_N;
        if (hist_n < HIST_N)
            ++hist_n;
        hist_t += HIST_MS;
    }
    hist[hist_head] = bat.ok ? (int16_t)bat.pct_x10 : -32768;
    hist_head = (hist_head + 1) % HIST_N;
    if (hist_n < HIST_N)
        ++hist_n;
    hist_t = now;
}

int battery_history(int16_t *out, int cap)
{
    int n = (int)hist_n < cap ? (int)hist_n : cap;
    for (int i = 0; i < n; ++i)                 /* the newest n, oldest first */
        out[i] = hist[(hist_head + HIST_N - (unsigned)n + (unsigned)i) % HIST_N];
    return n;
}

static int32_t cold_adjust(int32_t min)
{
    if (bat.temp_c10 > -1000 && bat.temp_c10 < 0)
        return min * 6 / 10;
    if (bat.temp_c10 > -1000 && bat.temp_c10 < 100)
        return min * 8 / 10;
    return min;
}

void battery_estimate(struct sbos_estimate *e)
{
    memset(e, 0, sizeof(*e));
    if (!bat.ok)
        return;
    /* to the 5 % cut-off, not to empty */
    int usable = bat.cap_mah - full_mah * 5 / 100;
    if (usable < 0)
        usable = 0;
    int learned_now = 0, learned_off = 0;
    int st = heater_running_ui() ? PS_HEAT : PS_ACTIVE;
    e->now_ma = bat_model_ma(&model, st, &learned_now);
    e->screen_off_ma = bat_model_ma(&model, PS_SCREEN_OFF, &learned_off);
    e->now_min = cold_adjust(bat_minutes(usable, e->now_ma));
    e->screen_off_min = cold_adjust(bat_minutes(usable, e->screen_off_ma));
    e->off_days = 180;                              /* typical self-discharge: shown as typical only */
    e->learned = learned_now && learned_off;
}

void battery_timer(unsigned id)
{
    (void)id;
    bat_read();
    hist_add();
    if (bat.ok)
        bat_model_feed(&model, K.power_state, bat.avg_ma, sb_ms() - K.state_since);
    ui_kminute();
    kt_set(KT_BATTERY, 1, sb_ms(), K.screen_on ? 10000 : 60000, 0);
    /* the reserve (docs 4.6): below 3 % off, unless SOS runs (down to 2 %) */
    if (bat.ok && bat.pct < 2 && torch_on())
        torch_set(0);
    if (bat.ok && !bat.charger && ((bat.pct < 3 && !sos_active()) || bat.pct < 2)) {
        sb_log("sbos: battery reserve reached: power off");
        power_off_now();
    }
}

void battery_init(void)
{
    bat_model_init(&model);
    bat_read();
    hist_add();
}

/* ---- power ------------------------------------------------------------------------------- */
static void set_state(int ps)
{
    if (K.power_state != ps) {
        K.power_state = ps;
        K.state_since = sb_ms();
    }
}

/* ---- low power with the screen off (docs/STARBITE-OS.md 4.2, 0.3.2) -------------------------------------
 * The A55 cluster drops to its lowest listed level (455 MHz) while the screen is off and nothing heats, and goes
 * back to S-Boot's 1053 MHz when the screen comes on (the heater sets its own levels and restores 1053 itself).
 * Through the listed ACPM path with a PLL read-back; never MIF or INT. */
#define A55_LOW 455000U
#define A55_NORMAL 1053000U
static int a55_lowered;
uint32_t auto_off_min;                      /* settings: switch off after this long with the screen off (0 never) */

static void log_mhz(const char *a, uint32_t khz, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_dec(&u, khz / 1000U);
    ub_s(&u, b);
    sb_log(u.b);
}

static int a55_near(uint32_t want)
{
    uint32_t got = hw_a55_khz();
    return got && got <= want + want / 50U && got + want / 50U >= want;
}

static void a55_low(int low)
{
    if (low) {
        if (heater_running_ui() || a55_lowered)
            return;
        uint32_t was = hw_a55_khz();
        if (!hw_dvfs(DVFS_A55, A55_LOW)) {
            sb_log("power: the A55 did not take 455 MHz (ACPM)");
            return;
        }
        for (int i = 0; i < 5 && !a55_near(A55_LOW); ++i)
            sb_mdelay(1);
        a55_lowered = 1;
        log_mhz(a55_near(A55_LOW) ? "power: A55 at 455 MHz with the screen off (was " :
                                    "power: A55 asked for 455 MHz, the PLL does not show it (was ", was, " MHz)");
        return;
    }
    if (!a55_lowered)
        return;
    a55_lowered = 0;
    if (heater_running_ui())
        return;                             /* the heater owns the clocks now */
    if (!hw_dvfs(DVFS_A55, A55_NORMAL))
        sb_log("power: the A55 did not take 1053 MHz back (ACPM)");
    for (int i = 0; i < 5 && !a55_near(A55_NORMAL); ++i)
        sb_mdelay(1);
    log_mhz("power: A55 back at ", hw_a55_khz(), " MHz with the screen on");
}

void power_heater_took_clocks(void)
{
    a55_lowered = 0;
}

static void screen(int on)
{
    if (K.screen_on == on)
        return;
    K.screen_on = on;
    hw_screen(on);
    kui_screen_changed(on);
    if (!on) {
        /* v0.1: black, nothing drawn until the Power key */
        uint32_t *bb = 0;
        (void)bb;
        set_state(heater_running_ui() ? PS_HEAT : PS_SCREEN_OFF);
        kt_cancel(KT_POWER, 1);
        sb_log("sbos: screen off");
        a55_low(1);
        if (auto_off_min)
            kt_set(KT_POWER, 2, sb_ms(), auto_off_min * 60000U, 0);
    } else {
        set_state(heater_running_ui() ? PS_HEAT : PS_ACTIVE);
        kt_set(KT_POWER, 1, sb_ms(), K.screen_off_s * 1000U, 0);
        kt_cancel(KT_POWER, 2);
        sb_log("sbos: screen on");
        a55_low(0);
    }
    kt_set(KT_INPUT, 1, sb_ms(), on ? 16 : 50, 1);    /* off: 50 ms, so a quick Power press is seen */
    kt_set(KT_BATTERY, 1, sb_ms(), on ? 2000 : 60000, 0);
    for (int a = 0; a < napps; ++a)
        app_event_send(a, EV_APP_POWER, 0, on);
}

void power_input(void)
{
    if (K.screen_on)
        kt_set(KT_POWER, 1, sb_ms(), K.screen_off_s * 1000U, 0);
}

void power_key_short(void)
{
    if (sos_active() || kui_screen() == 6 /* SCR_LIGHT */) {
        if (!K.screen_on)
            screen(1);
        return;                                     /* SOS and the light keep the screen */
    }
    screen(!K.screen_on);
    if (K.screen_on)
        kui_lock();
}

/* the self test: the screen off and on again as the Power key would */
void power_screen_set(int on)
{
    screen(on);
}

void power_key_long(void)
{
    if (!K.screen_on)
        screen(1);
    kui_power_menu();
}

/* Switch off after auto_off_min minutes with the screen off (docs 4.4): only when nothing needs the phone on
 * (no charger, no heat, alarm or light, the battery read and above 3 %), after every app got EV_APP_SAVE and the
 * settings were written (FUA + cache flush, where the "sbos-state" entry exists). rehearse: everything but the
 * power-off (the self-test image). 1 it would switch off / did, 0 something kept it on (logged). */
int power_auto_off(int rehearse)
{
    const char *why = 0;
    if (bat.ok && bat.charger)
        why = "a charger is connected";
    else if (heater_running_ui())
        why = "heating";
    else if (sos_active())
        why = "the alarm runs";
    else if (torch_on())
        why = "the LED is on";
    else if (!bat.ok || bat.pct <= 3)
        why = "the battery is unknown or at the reserve";
    if (why) {
        sb_logf(rehearse ? "power: REHEARSAL: it would stay on: " : "power: not switching off: ", why);
        if (!rehearse)
            return 0;                       /* the rehearsal runs the save anyway (the phone is on USB) */
    }
    for (int a = 0; a < napps; ++a)
        app_event_send(a, EV_APP_SAVE, 0, 0);
    apps_service();                         /* their EV_APP_SAVE handlers run now */
    settings_save_now();
    sb_log(rehearse ? "power: REHEARSAL: saved; would switch off now (the self-test image never does)" :
                      "power: saved; switching off after the screen was off");
    if (!rehearse)
        power_off_now();
    return 1;
}

void power_timer(unsigned id)
{
    if (id == 2) {
        if (!K.screen_on && auto_off_min && !power_auto_off(sbos_selftest_enabled))
            kt_set(KT_POWER, 2, sb_ms(), 10U * 60000U, 0);  /* kept on: ask again in 10 minutes */
        return;
    }
    /* the screen-off timer: not while something must stay visible */
    if (!K.screen_on)
        return;
    int down = 0;
    kin_touch_held(&down);
    if (kui_awake_on() || sos_active() || down) {
        kt_set(KT_POWER, 1, sb_ms(), 5000, 0);
        return;
    }
    screen(0);
}

void power_restart(int to_menu)
{
    torch_set(0);
    wifi_power(0);                          /* the radio held in reset and unpowered before the reset */
    heater_stop_all("Restarting");
    sb_mdelay(200);
    if (to_menu)
        sb_reboot(SB_INFORM_NORMAL);
    sb_boot_next("starbite-os");
}

void power_off_now(void)
{
    torch_set(0);
    wifi_power(0);
    if (heater_running_ui())
        heater_stop_all("Switching off");
    sb_mdelay(200);
    sb_power_off();
    /* still here: a cable keeps the PMIC on. Stay on, screen off. */
    sb_log("sbos: power off returned (cable): staying on");
    screen(0);
}

void power_init(void)
{
    K.screen_on = 1;
    K.screen_off_s = 30;
    K.sleep_off_s = 300;
    set_state(PS_ACTIVE);
    kt_set(KT_POWER, 1, sb_ms(), K.screen_off_s * 1000U, 0);
    kt_set(KT_INPUT, 1, sb_ms(), 16, 1);
    kt_set(KT_BATTERY, 1, sb_ms(), 10000, 0);
}
