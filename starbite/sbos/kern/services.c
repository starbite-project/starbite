/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS small services: SOS (docs 9), the clock (the Utility OS's tz.c), the battery and its model
 * (docs 4.5), and power (docs 4.1, 4.6). */
#include "sb.h"
#include "kern.h"
#include "kinput.h"
#include "tz.h"
#include "survival.h"

/* ---- SOS ------------------------------------------------------------------------------------- */
/* state 0 off, 2 the countdown (3 s, a hold cancels), 1 flashing: white / red at 2 Hz */
static int sos_st;
int sos_led_on;
int led_full;                               /* 0.6.3: Settings > Alarm (RAM only until the first run is done) */
static uint64_t sos_t0, sos_phase_t;
static int sos_phase;

int sos_active(void)
{
    return sos_st != 0;
}

/* 0.6: SOS flashes at the panel's top brightness level (its own maximum, within its rating): panel_boost */

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
        kt_cancel(KT_SOS, 2);               /* 1.2-alpha27: the Morse */
        if (sos_led_on || (surv_sos_opts & SURV_SOS_TORCH))
            torch_set(0);
        if (surv_sos_opts & SURV_SOS_VIB)
            vib_off();
        fled_off();
        panel_boost(2, 0);
        sb_log("sos: off");
    }
    kui_sos_changed();
    app_event_send(app_by_id("sos"), EV_APP_SERVICE, 2, sos_st);
    return 0;
}

/* 1.2-alpha27 (survival.c): Morse S-O-S on the torch (its normal current) and the vibration motor, one element a
 * timer; the screen's beacon can follow it */
static int morse_lit;
static void morse_step(void)
{
    if (sos_st != 1)
        return;
    uint64_t now = sb_ms();
    uint32_t next = 0, at = (uint32_t)(now - sos_phase_t);
    int lit = surv_morse_at(at, &next);
    if (surv_sos_opts & SURV_SOS_TORCH)
        torch_set(lit);
    uint32_t cycle = at / surv_morse_cycle_ms();   /* the motor: the first 3 cycles, then one a minute (battery) */
    if (lit && !morse_lit && (surv_sos_opts & SURV_SOS_VIB) && (cycle < 3 || cycle % 9U == 0))
        (void)vib_pulse(next - at);
    morse_lit = lit;
    kt_set(KT_SOS, 2, now, next > at ? next - at : 1, 0);
    ui_mark_dirty();
}

void sos_timer(unsigned id)
{
    if (id == 9) {
        torch_timer(id);
        return;
    }
    if (id == 2) {
        morse_step();
        return;
    }
    uint64_t now = sb_ms();
    if (sos_st == 2 && now >= sos_t0) {
        sos_st = 1;
        sos_phase_t = now;
        panel_boost(2, 1);                  /* the brightest flashes the panel is rated for */
        sb_log("sos: flashing");
        app_event_send(app_by_id("sos"), EV_APP_SERVICE, 2, sos_st);
        morse_lit = 0;
        if (surv_sos_opts & (SURV_SOS_TORCH | SURV_SOS_VIB)) {
            sb_logx("sos: Morse S-O-S (200 ms unit): torch 1, motor 2: ", surv_sos_opts & 3U);
            morse_step();
        }
    }
    if (sos_st == 1) {
        sos_phase = (int)(((now - sos_phase_t) / 250U) & 1U);
        if (surv_sos_opts & SURV_SOS_TORCH) {
            /* the torch follows the Morse (morse_step) */
        } else if (sos_led_on && led_full && fled_flash_ok()) {   /* 1.1-beta2: else the torch strobes, as before */
            if (!sos_phase)
                (void)fled_pulse();         /* 0.6.3: a 1250 mA flash pulse with the white phase, at most 1 a second */
        } else if (sos_led_on) {
            torch_set(!sos_phase);              /* the LED with the white phase */
        }
    }
    ui_mark_dirty();
}

int sos_frame(uint32_t *argb)
{
    if (!sos_st)
        return 0;
    unsigned beacon = (surv_sos_opts >> SURV_BEACON_SHIFT) & 3U;    /* 1.2-alpha27 */
    if (sos_st == 2)
        *argb = 0xff1c0606U;
    else if (beacon == 1)
        *argb = sos_phase ? 0xff000000U : 0xffffffffU;
    else if (beacon == 2)
        *argb = sos_phase ? 0xff000000U : 0xffff1a1aU;
    else if (beacon == 3)
        *argb = morse_lit ? 0xffffffffU : 0xff000000U;
    else
        *argb = sos_phase ? 0xffff1a1aU : 0xffffffffU;
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
static int rtc_state;                       /* 0 not read, 1 the PMIC's, 2 the network's, 3 the last known (approx.), -1 none */
/* 1.2-alpha26 (the user: "starbite os didn't know the time"): alpha24's boot found the PMIC clock lost (the evening's
 * brownouts can reset it; nothing of ours wrote it back). Now: the network's time is written into the PMIC clock
 * (it keeps time across a power loss), the last known time is kept in the state slot (hourly, at a network set, at a
 * restart or power-off) and used, marked approximate, when the PMIC clock is lost or behind it; and a boot without a
 * clock asks Wi-Fi once for the time (wifi.c) */
static uint32_t clock_saved;                /* the last known UTC from the state slot (0 none) */
static uint64_t clock_save_t;

void clock_restore_saved(uint32_t utc)
{
    clock_saved = utc;
    if (rtc_state == -1)
        rtc_state = 0;                      /* read again with the last known time at hand */
}

uint32_t clock_saved_value(void)
{
    struct sbos_time t;
    return clock_now(&t) && !t.approx && t.utc > 0 ? (uint32_t)t.utc : clock_saved;
}

static void ymdhm(struct ub *u, int64_t e)
{
    int64_t days = e / 86400, s = e % 86400;
    int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, mo = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mo <= 2);
    ub_dec(u, (uint64_t)y);
    ub_c(u, '-');
    ub_dec2(u, (unsigned)mo);
    ub_c(u, '-');
    ub_dec2(u, (unsigned)d);
    ub_c(u, ' ');
    ub_dec2(u, (unsigned)(s / 3600));
    ub_c(u, ':');
    ub_dec2(u, (unsigned)(s / 60 % 60));
}

/* at boot (after the settings): what the PMIC clock says, and what is used */
void clock_boot_log(void)
{
    uint64_t e = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "clock: the PMIC clock ");
    if (!sb_rtc_epoch(&e)) {
        ub_s(&u, "not read");
    } else {
        ub_s(&u, "reads ");
        ymdhm(&u, (int64_t)e);
        ub_s(&u, " UTC");
        if (e <= 1577836800ULL)
            ub_s(&u, ": LOST (before 2020: reset by a power loss?)");
        else if (clock_saved && e + 60 < clock_saved)
            ub_s(&u, ": BEHIND the last known time (reset?)");
    }
    sb_log(u.b);
    if (clock_saved) {
        ub_init(&u);
        ub_s(&u, "clock: the last known time (the state slot) ");
        ymdhm(&u, clock_saved);
        ub_s(&u, " UTC");
        sb_log(u.b);
    }
    struct sbos_time t;
    sb_log(!clock_now(&t) ? "clock: no time yet (the network gives it)" :
           t.approx ? "clock: the last known time, shown as approximate until the network or the PMIC clock gives it" :
                      "clock: the PMIC clock is used");
}

/* 0.5.8: the time from the network (NTP over Wi-Fi): kept as an offset to the PMIC's clock, never written to it;
 * gone at a restart. With no readable PMIC clock the network's time is used as it is (rtc_state 2). Returns the
 * correction in seconds (0: within 2 s, nothing changed). */
static int64_t clock_adj;

/* 1.2-alpha26: the network's time into the PMIC clock (it keeps time across a power loss); the network's time used
 * either way; the last known time saved */
static void clock_to_pmic(int64_t utc)
{
    clock_saved = (uint32_t)utc;
    clock_save_t = sb_ms();
    settings_changed();
    if (sb_rtc_write((uint64_t)utc)) {
        rtc_t = utc;
        rtc_ms = sb_ms();
        rtc_state = 1;
        clock_adj = 0;
        sb_log("clock: the PMIC clock set from the network (it keeps the time across a power loss)");
    } else {
        sb_log("clock: the PMIC clock could NOT be set (refused or no read-back): the network's time until a restart");
    }
}

int64_t clock_set_net(int64_t utc, int *was_set)
{
    struct sbos_time t;
    *was_set = 0;
    if (!clock_now(&t) || t.approx) {
        rtc_t = utc;
        rtc_ms = sb_ms();
        rtc_state = 2;
        clock_adj = 0;
        *was_set = 1;                       /* there was no clock (or only the last known): set, not corrected */
        clock_to_pmic(utc);
        ui_mark_dirty();
        return 0;
    }
    int64_t d = utc - t.utc;
    if (d > -2 && d < 2)
        return 0;
    clock_adj += d;
    clock_to_pmic(utc);
    ui_mark_dirty();
    return d;
}

int clock_now(struct sbos_time *t)
{
    uint64_t now = sb_ms();
    if (rtc_state != 2 && (!rtc_state || now - rtc_try >= 60000U)) {   /* a network-set clock stays */
        uint64_t e;
        rtc_try = now;
        if (sb_rtc_epoch(&e) && e > 1577836800ULL && (!clock_saved || e + 60 >= clock_saved)) {
            rtc_t = (int64_t)e;
            rtc_ms = now;
            rtc_state = 1;
        } else if (rtc_state != 1 && rtc_state != 2 && rtc_state != 3) {
            if (clock_saved > 1577836800U) {   /* 1.2-alpha26: the last known time, approximate */
                rtc_t = (int64_t)clock_saved;
                rtc_ms = now;
                rtc_state = 3;
            } else {
                rtc_state = -1;
            }
        }
    }
    memset(t, 0, sizeof(*t));
    if (rtc_state != 1 && rtc_state != 2 && rtc_state != 3)
        return 0;
    if (rtc_state != 3 && now - clock_save_t >= 3600000U && now > 60000U) {
        clock_save_t = now;                 /* 1.2-alpha26: the last known time saved hourly */
        clock_saved = (uint32_t)(rtc_t + (int64_t)((now - rtc_ms) / 1000U) + clock_adj);
        settings_changed();
    }
    t->approx = rtc_state == 3;
    int64_t utc = rtc_t + (int64_t)((now - rtc_ms) / 1000U) + clock_adj;
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
    while (hist_n && now - hist_t >= 2U * HIST_MS) {
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
    hist_t = hist_n > 1 ? hist_t + HIST_MS : now;   /* a fixed grid: late reads do not stretch the slots */
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
    {
        /* 1.2-alpha9: a low battery, once at 15 % and once at 5 % (again after a charge above them) */
        static int said15, said5;
        if (bat.ok && !bat.charger) {
            if (bat.pct <= 5 && !said5) {
                said5 = said15 = 1;
                notif_post("battery", "Battery at 5 %", "Plug in soon; the phone switches off at 3 %");
            } else if (bat.pct <= 15 && !said15) {
                said15 = 1;
                notif_post("battery", "Battery at 15 %", "");
            }
        } else if (bat.ok && bat.pct > 20) {
            said15 = said5 = 0;
        }
    }
    /* 1.2-alpha27: Last 2 % mode (survival.c) keeps the torch and runs to 1 % or 3.4 V instead */
    int l2 = bat.ok ? surv_last2_battery(bat.pct, bat.mv, bat.charger) : -1;
    if (bat.ok && bat.pct < 2 && torch_on() && l2 < 0)
        torch_set(0);
    if (bat.ok && !bat.charger && (l2 > 0 || (l2 < 0 && ((bat.pct < 3 && !sos_active()) || bat.pct < 2)))) {
        if (sbos_selftest_enabled) {       /* unattended: never switched off (no one to press Power) */
            sb_log("sbos: battery reserve reached in the self-test image: back to the menu, not off");
            power_restart(1);
        }
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
    if (!a55_lowered || surv_last2())
        return;                             /* 1.2-alpha27: Last 2 % keeps the lowest step with the screen on */
    a55_lowered = 0;
    if (heater_running_ui())
        return;                             /* the heater owns the clocks now */
    if (!hw_dvfs(DVFS_A55, A55_NORMAL))
        sb_log("power: the A55 did not take 1053 MHz back (ACPM)");
    for (int i = 0; i < 5 && !a55_near(A55_NORMAL); ++i)
        sb_mdelay(1);
    log_mhz("power: A55 back at ", hw_a55_khz(), " MHz with the screen on");
}

void power_last2_clock(int on)
{
    if (on)
        a55_low(1);
    else if (K.screen_on)
        a55_low(0);
}

void power_heater_took_clocks(void)
{
    a55_lowered = 0;
}

int pocket_lock;

static void screen(int on)
{
    if (K.screen_on == on)
        return;
    pocket_lock = 0;
    K.screen_on = on;
    int aod = !on && em_aod && look_emergency && !sos_active();
    if (on && panel_in_lpm) {
        (void)panel_lpm(0);                 /* 0.6.5: out of the dim clock first */
        kt_cancel(KT_POWER, 3);
    }
    if (!aod)
        hw_screen(on);                      /* (the dim clock keeps the panel on, in its low-power mode) */
    kui_screen_changed(on);
    guest_screen(on);                       /* 1.2: Linux told; 60 s with the screen off stops it */
    if (aod) {
        sb_mdelay(40);                      /* the clock's frame into the panel's RAM first */
        (void)panel_lpm(1);
        kt_set(KT_POWER, 3, sb_ms(), 60000, 1);     /* the clock, once a minute */
    }
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
        kt_set(KT_HOUSE, 0, sb_ms(), 1000, 0);     /* the watchdog fed every second again (0.5.12) */
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
    if (alarm_ringing() >= 0) {                     /* 1.2-alpha8: Power snoozes a ringing alarm */
        if (!K.screen_on)
            screen(1);
        (void)alarm_stop(1);
        return;
    }
    if (sos_active() || kui_screen() == 6 /* SCR_LIGHT */) {
        if (!K.screen_on)
            screen(1);
        return;                                     /* SOS and the light keep the screen */
    }
    if (K.screen_on && kui_sleep_hook && kui_sleep_hook(1))
        return;                                     /* P2r: Next fades to black first and turns the screen off then */
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
    pocket_lock = 0;                                /* the power menu needs touch */
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
    else if (alarm_any())
        why = "a wake-up alarm is set";             /* 1.2-alpha8: it could not ring switched off */
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
    if (rehearse) {                         /* the rehearsal writes nothing (the self test's slot step does) */
        int was = sb_fence_is_dry_run();
        sb_fence_dry_run(1);
        settings_save_now();
        sb_fence_dry_run(was);
    } else {
        settings_save_now();
    }
    sb_log(rehearse ? "power: REHEARSAL: the save as a dry run; would switch off now (the self-test image never does)" :
                      "power: saved; switching off after the screen was off");
    if (!rehearse)
        power_off_now();
    return 1;
}

void power_timer(unsigned id)
{
    if (id == 3) {
        if (!K.screen_on && panel_in_lpm)
            kui_aod_frame();                /* 0.6.5: the dim clock's minute */
        else
            kt_cancel(KT_POWER, 3);
        return;
    }
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
    if (heater_running_ui()) {
        /* 0.6: heating keeps the screen on (it is part of the heat); after the screen-off time the lock screen with
         * touch and keys locked (a pocket), until a short Power press */
        if (!pocket_lock) {
            pocket_lock = 1;
            kui_lock();
            sb_log("power: heating: the screen stays on; touch locked until Power");
        }
        kt_set(KT_POWER, 1, sb_ms(), 5000, 0);
        return;
    }
    if (kui_sleep_hook && kui_sleep_hook(0))
        return;                                     /* P2r: Next's fade, then the screen off */
    screen(0);
}

void power_restart(int to_menu)
{
    torch_set(0);
    kmodem_stop("Restarting");              /* 1.2-alpha21: the CP parked before any reset */
    clock_saved = clock_saved_value();      /* 1.2-alpha26: the last known time kept (after the modem's storage hold) */
    settings_save_now();
    wifi_power(0);                          /* the radio held in reset and unpowered before the reset */
    guest_power_off();                      /* 1.2: Linux stopped; its last core powers the M3 cluster off (<= 1 s) */
    heater_stop_all("Restarting");
    heater_trip_restore_now();              /* 1.1: the chip's trip back on before any reset we make */
    sb_mdelay(200);
    if (to_menu)
        sb_reboot(SB_INFORM_NORMAL);
    sb_boot_next("starbite-os");
}

/* 1.2-alpha25: the lock screen's OS card: everything off as for a restart, then the loader starts that key once */
void power_boot_os(const char *key)
{
    torch_set(0);
    kmodem_stop("Restarting");
    wifi_power(0);
    guest_power_off();
    heater_stop_all("Restarting");
    heater_trip_restore_now();
    sb_mdelay(200);
    sb_boot_next(key);
}

/* 1.2-alpha1: the Linux self-test image's end: everything off as for a restart, then TWRP (pmsg) */
void power_restart_recovery(void)
{
    torch_set(0);
    kmodem_stop("Restarting");              /* 1.2-alpha21: the CP parked before any reset */
    wifi_power(0);
    guest_power_off();
    heater_stop_all("Restarting");
    heater_trip_restore_now();
    sb_mdelay(200);
    sb_reboot(SB_INFORM_RECOVERY);
    sb_boot_next("starbite-os");            /* (sb_reboot does not return) */
}

void power_off_now(void)
{
    torch_set(0);
    kmodem_stop("Switching off");              /* 1.2-alpha21: the CP parked before any reset */
    clock_saved = clock_saved_value();      /* 1.2-alpha26: the last known time kept */
    settings_save_now();
    wifi_power(0);
    guest_power_off();                      /* 1.2: Linux stopped before the power goes */
    if (heater_running_ui()) {
        heater_stop_all("Switching off");   /* the hardware off first */
        settings_save_now();                /* 1.0-rc2: then the heat record ("stopped"), before the power goes */
    }
    heater_trip_restore_now();              /* 1.1: the chip's trip back on before the power goes */
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
