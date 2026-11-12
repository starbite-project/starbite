/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS settings that last across a restart (docs/STARBITE-OS.md 6.4), 0.3.1.
 *
 * The record "sbos.settings" in the state slot (state.c's two halves, the valid one with the higher sequence
 * wins). A change is saved 3 s after the last one (a series of taps is one write), into the other half.
 * The slot itself is behind hw_slot_read / hw_slot_write: the host preview keeps it in a file; on the phone
 * there is no slot until the UFS writer of doc 6.3 exists, and then everything here works unchanged (until
 * then the settings last until a restart, and Settings says so).
 *
 * Saved: the screen-off time, refresh only on change, the panel off with the screen, the LED in the alarm,
 * the brightness. Never saved: the GPU in High (asked again every start) and the heater's first-run
 * confirmation. A restore is skipped after an unclean start (none yet: the loader's crash detection has it). */
#include "sb.h"
#include "kern.h"
#include "kapp.h"
#include "sbapp.h"
#include "survival.h"
#include "tzdb.h"

#define REC "sbos.settings"
#define SAVE_DELAY_MS 3000U

struct saved {
    uint32_t version;                       /* 1 */
    uint32_t screen_off_s;
    uint8_t panel_save, panel_off, sos_led, bright;
    uint32_t pad[4];
    uint32_t ext[4];                        /* 1.2-alpha18: [0] the B2SOVRC note (0x100 | CHG_CNFG_05 as found);
                                             * Track A: [3] bit 31 set, bit 30 12-hour, bits 0..29 the zone's id */
};

static uint8_t *half[2];
static int slot_ok;                         /* 1 the slot exists and was read */
static int cur_half = -1;                   /* the half that holds the newest valid copy */
static uint64_t seq;
static unsigned saves, save_fail;
static int foreign;
static struct saved loaded;                 /* the record as restored (the self-test image saves its panel parts back) */
static int have_loaded;                         /* the newest copy is of another version: never overwrite it */

int settings_persistent(void)
{
    return slot_ok && !foreign;
}

static void fill(struct saved *s)
{
    memset(s, 0, sizeof(*s));
    s->version = 1;
    s->screen_off_s = K.screen_off_s;
    s->panel_save = (uint8_t)(panel_save_on != 0);
    s->panel_off = (uint8_t)(panel_off_on != 0);
    s->sos_led = (uint8_t)(sos_led_on != 0);
    s->bright = (uint8_t)panel_bright_level;
    {
        uint32_t so = 0;                    /* 1.2-alpha29: Extreme power saving's values are temporary */
        int br = 0, po = 0;
        if (surv_saver_saved(&so, &br, &po)) {
            if (so)
                s->screen_off_s = so;
            s->bright = (uint8_t)br;
            s->panel_off = (uint8_t)(po != 0);
        }
    }
    s->pad[0] = auto_off_min;               /* 0.3.2: was zero padding, so 0 (never) for older records */
    s->pad[1] = 0x100U | (uint32_t)(look_emergency != 0) | (uint32_t)(em_set_auto != 0) << 1;    /* 0.6 */
    s->pad[2] = (uint32_t)(look_is_ready() ? look_style() : look_saved_style) + 1U;
    s->ext[0] = (uint32_t)heat_b2s_note;     /* 1.2-alpha18 */
    s->ext[1] = 0x10000U | (uint32_t)(heat_guard_mv & 0xffff);   /* 1.2-alpha24: the brownout guard's floor */
    s->ext[2] = clock_saved_value();         /* 1.2-alpha26: the last known time (UTC; 0 none) */
    s->ext[3] = 0x80000000U | (uint32_t)(clock_24h == 0) << 30 | tzdb_id(clock_zone());   /* Track A: older: 0 */
    s->pad[3] = 0x10000U | (uint32_t)(heat_surv_stop_c10 & 0xffff) |  /* 1.2-alpha16: Survival's SoC limit (or none) */
                (uint32_t)(heat_unlimited != 0) << 17 |               /* 1.2-alpha24: Unlimited */
                (uint32_t)(heat_guard_on == 0) << 18 |                /* and the brownout guard OFF (0: on) */
                (uint32_t)((heat_surv_bat_c10 / 10) & 0x7f) << 20 |  /* and its battery limit (55 / 58 / 60 / 65) */
                (uint32_t)(heat_no_high_stop != 0) << 27 |            /* 1.2-alpha17: the 4.5 A stop off */
                (uint32_t)(heat_no_current_stop != 0) << 28 |         /* and the 3.5 A limit off */
                (uint32_t)(heat_screen_white != 0) << 29 |            /* 1.2-alpha19: Survival's white screen */
                (uint32_t)(wifi_autojoin == 0) << 30 |                /* 1.2-alpha21: Wi-Fi's automatic join off */
                (uint32_t)(heat_all_out == 0) << 31;                  /* 1.2-alpha23: Survival's All out off (0: on) */
    if (sbos_selftest_enabled && have_loaded) {     /* not applied there: kept as they were saved (0.5.12) */
        s->panel_save = loaded.panel_save;
        s->panel_off = loaded.panel_off;
        s->bright = loaded.bright;
    }
}

void settings_restore(void)
{
    if (!half[0]) {
        half[0] = sb_alloc(STATE_HALF);
        half[1] = sb_alloc(STATE_HALF);
        prefs_init(sb_alloc(PREFS_MEM));    /* Track A: the prefs and blobs work in RAM also without a slot */
    }
    if (!half[0] || !half[1] || !hw_slot_read(half[0], half[1])) {
        sb_log("settings: no state slot yet: defaults, kept until a restart");
        return;
    }
    slot_ok = 1;
    cur_half = state_pick(half[0], half[1]);
    if (cur_half < 0) {
        sb_log("settings: the state slot is empty or not valid: defaults (saved at the first change)");
        return;
    }
    (void)state_valid(half[cur_half], &seq);
    {
        /* Track A: the prefs and blobs (prefs.c), before the shell asks for them (its bake runs after this) */
        int r = prefs_restore(half[cur_half]), nk = 0, nb = 0;
        uint32_t kb = 0, bb = 0;
        prefs_counts(&nk, &kb, &nb, &bb);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "settings: prefs restored: ");
        ub_dec(&u, (uint64_t)nk);
        ub_s(&u, " keys, ");
        ub_dec(&u, (uint64_t)nb);
        ub_s(&u, " blobs (");
        ub_dec(&u, bb);
        ub_s(&u, r ? " bytes; something was not taken: a broken entry or another version)" : " bytes)");
        sb_log(u.b);
    }
    const uint8_t *d = 0;
    {
        /* 0.6.3: the last heating session's record: still "running" means the power went while it heated */
        /* 1.2-alpha6: the last session's heat log, kept until the next session replaces it */
        {
            char id[12] = "heatlog0";
            uint8_t *p = (uint8_t *)&heat_log;
            int ok = 1;
            for (uint32_t o = 0, k = 0; o < sizeof(heat_log); o += STATE_REC_MAX, ++k) {
                id[7] = (char)('0' + k);
                uint32_t len = sizeof(heat_log) - o < STATE_REC_MAX ? sizeof(heat_log) - o : STATE_REC_MAX;
                const uint8_t *q = 0;
                if (state_find(half[cur_half], id, &q) != (int)len || !q) {
                    ok = 0;
                    break;
                }
                memcpy(p + o, q, len);
            }
            if (!ok || heat_log.magic != HLOG_MAGIC)
                memset(&heat_log, 0, sizeof(heat_log));
            else
                sb_logx("settings: the last heat log kept, samples: ", heat_log.n);
        }
        int hn = state_find(half[cur_half], "heat", &d);
        if (hn == (int)sizeof(heat_last) && d) {
            memcpy(&heat_last, d, sizeof(heat_last));
            heat_last.why[sizeof(heat_last.why) - 1] = 0;
            if (heat_last.magic == 0x31435248U && heat_last.running) {
                heat_last_unclean = 1;
                struct ub u;
                ub_init(&u);
                ub_s(&u, "heater: the last session ended without a stop (a forced power-off?) after ");
                ub_dec(&u, (uint64_t)(heat_last.secs > 0 ? heat_last.secs : 0));
                ub_s(&u, " s: level ");
                ub_dec(&u, (uint64_t)(heat_last.level > 0 ? heat_last.level : 0));
                ub_s(&u, ", SoC ");
                ub_dec(&u, (uint64_t)(heat_last.soc_c10 > 0 ? heat_last.soc_c10 / 10 : 0));
                ub_s(&u, " C (peak ");
                ub_dec(&u, (uint64_t)(heat_last.peak_soc_c10 > 0 ? heat_last.peak_soc_c10 / 10 : 0));
                ub_s(&u, "), battery ");
                ub_dec(&u, (uint64_t)(heat_last.bat_c10 > 0 ? heat_last.bat_c10 / 10 : 0));
                ub_s(&u, " C, ");
                ub_dec(&u, (uint64_t)(heat_last.ma < 0 ? -heat_last.ma : heat_last.ma));
                ub_s(&u, " mA, ");
                ub_dec(&u, (uint64_t)(heat_last.pct > 0 ? heat_last.pct : 0));
                ub_s(&u, " %, the hardware trip read ");
                ub_dec(&u, (uint64_t)(heat_last.trip_c > 0 ? heat_last.trip_c : 0));
                ub_s(&u, " C, peak ");
                ub_dec(&u, (uint64_t)(heat_last.peak_mw > 0 ? heat_last.peak_mw / 1000 : 0));
                ub_s(&u, " W, ");
                ub_dec(&u, (uint64_t)(heat_last.mwh > 0 ? heat_last.mwh : 0));
                ub_s(&u, " mWh (1.1-beta4)");
                sb_log(u.b);
            }
            heat_rec = heat_last;           /* kept in the slot until the next session replaces it */
            heat_rec.running = 0;
        }
        d = 0;
    }
    {
        const uint8_t *q = 0;               /* 1.2-alpha27: the survival tools' options (SOS, Last 2 %, the location) */
        if (state_find(half[cur_half], "sbos.surv", &q) == 16 && q)
            surv_unpack(q);
        q = 0;                              /* Track A: Next's alarm list and labels */
        if (state_find(half[cur_half], "sbos.alarm", &q) == ALARM_REC_SIZE && q)
            alarm_unpack(q);
    }
    {
        /* Track A: the passcode's record (salt, hash, wrong tries, the lock-out left; never the digits) */
        const uint8_t *q = 0;
        int pn = state_find(half[cur_half], PASS_REC, &q);
        if (pn > 0 && q) {
            struct sbos_time t;
            if (pass_load(q, pn, sb_ms(), clock_now(&t) ? (int64_t)t.utc : 0))
                sb_log(pass_len() ? "settings: the passcode restored" : "settings: the passcode's tries restored");
            else
                sb_log("settings: the passcode record is not valid: no passcode");
        }
    }
    kmodem_sms_restore(half[cur_half]);     /* Track G: the messages kept in the slot */
    int n = state_find(half[cur_half], REC, &d);
    struct saved s;
    if (n < (int)sizeof(uint32_t) * 2 + 4 || !d) {
        sb_log("settings: no settings record in the slot: defaults");
        return;
    }
    memset(&s, 0, sizeof(s));
    memcpy(&s, d, (unsigned)n < sizeof(s) ? (unsigned)n : sizeof(s));
    if (s.version == 1) {
        loaded = s;
        have_loaded = 1;
    }
    if (s.version != 1) {
        foreign = 1;
        sb_log("settings: a settings record of another version: defaults, and it is not overwritten");
        return;
    }
    if ((s.screen_off_s >= 10 && s.screen_off_s <= 600) || s.screen_off_s == 0) {    /* Track A: 0 = Auto-Lock Never */
        K.screen_off_s = s.screen_off_s;
        power_input();                      /* the screen-off timer with the restored time */
    }
    sos_led_on = s.sos_led != 0;
    if (sbos_selftest_enabled) {
        /* unattended: no panel commands from saved settings (0.5.12); the record stays as it was */
        sb_log("settings: the self-test image leaves the panel's saved settings (brightness, refresh, off) unapplied");
    } else {
        panel_save_set(s.panel_save != 0);
        panel_off_on = s.panel_off != 0;
        if (s.bright >= 1 && s.bright <= 4)
            (void)panel_brightness(s.bright);
    }
    if (s.pad[0] == 5 || s.pad[0] == 15 || s.pad[0] == 60)
        auto_off_min = s.pad[0];
    if ((s.pad[1] & ~3U) == 0x100U) {        /* 0.6: the mode, and whether it set the 5-minute switch-off */
        look_emergency = (int)(s.pad[1] & 1U);
        em_set_auto = (int)(s.pad[1] >> 1 & 1U) && look_emergency && auto_off_min == 5;
    } else if (s.pad[1] == 1 || s.pad[1] == 2) {
        look_emergency = (int)s.pad[1] - 1;  /* an early 0.6 record */
    }
    if (s.pad[2] >= 1 && s.pad[2] <= 4)
        look_saved_style = (int)s.pad[2] - 1;   /* main makes it once, after this */
    if (s.ext[2] > 1577836800U)
        clock_restore_saved(s.ext[2]);      /* 1.2-alpha26: the last known time */
    if (s.ext[3] & 0x80000000U) {           /* Track A: the time zone and 12 / 24-hour (older records: Helsinki, 24) */
        int z = tzdb_by_id(s.ext[3] & 0x3fffffffU);
        if (z >= 0)
            clock_zone_set(z);
        clock_24h = !(s.ext[3] >> 30 & 1U);
        sb_logf("settings: the time zone restored: ", tzdb[clock_zone()].name);
    }
    if ((s.ext[1] & 0xffff0000U) == 0x10000U) {     /* 1.2-alpha24 */
        int g = (int)(s.ext[1] & 0xffffU);
        if (g == 3200 || g == 3350 || g == 3500)
            heat_guard_mv = g;
    }
    if ((s.ext[0] & ~0x1ffU) == 0 && (s.ext[0] & 0x100U))
        heat_b2s_note = (int)s.ext[0];      /* 1.2-alpha18: a Survival run left B2SOVRC raised: the boot puts it back */
    if (s.pad[3] & 0x10000U) {              /* 1.2-alpha16: Survival's SoC limit, as the user left it */
        int v = (int)(s.pad[3] & 0xffffU);
        if ((v >= 1250 && v <= SBOS_HEAT_SOC_HARD_C10) || v == SBOS_HEAT_SOC_NOLIMIT_C10) {
            heat_surv_stop_c10 = v;
            sb_logx("settings: Survival's SoC limit restored, 0.1 C: ", (uint64_t)v);
        }
        heat_no_high_stop = (int)((s.pad[3] >> 27) & 1U);
        heat_no_current_stop = (int)((s.pad[3] >> 28) & 1U);
        heat_screen_white = (int)((s.pad[3] >> 29) & 1U);
        wifi_autojoin = !((s.pad[3] >> 30) & 1U);
        heat_all_out = !((s.pad[3] >> 31) & 1U);    /* 1.2-alpha23: older records: 0, so All out (the default) */
        heat_unlimited = (int)((s.pad[3] >> 17) & 1U);  /* 1.2-alpha24: older records: 0 (off) */
        heat_guard_on = !((s.pad[3] >> 18) & 1U);     /* older records: 0, so on (the default) */
        if (heat_unlimited || !heat_guard_on)
            sb_logx("settings: Survival restored (bit 0 Unlimited, bit 1 the brownout guard OFF): ",
                    (uint64_t)(heat_unlimited | !heat_guard_on << 1));
        if (!heat_all_out)
            sb_log("settings: Survival's All out restored: off (Survival holds as before)");
        if (heat_no_high_stop || heat_no_current_stop)
            sb_logx("settings: Survival's current stops restored (bit 0: 4.5 A off, bit 1: 3.5 A off): ",
                    (uint64_t)(heat_no_high_stop | heat_no_current_stop << 1));
        int b = (int)((s.pad[3] >> 20) & 0x7fU) * 10;
        if (b == 550 || b == 580 || b == 600 || b == 650) {     /* 1.2-alpha23: 65 */
            heat_surv_bat_c10 = b;
            sb_logx("settings: Survival's battery limit restored, 0.1 C: ", (uint64_t)b);
        }
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "settings: restored (slot sequence ");
    ub_dec(&u, seq);
    ub_s(&u, "): screen off ");
    ub_dec(&u, K.screen_off_s);
    ub_s(&u, " s, refresh on change ");
    ub_s(&u, panel_save_on ? "on" : "off");
    ub_s(&u, ", panel off ");
    ub_s(&u, panel_off_on ? "on" : "off");
    ub_s(&u, ", LED in the alarm ");
    ub_s(&u, sos_led_on ? "on" : "off");
    ub_s(&u, ", brightness ");
    ub_dec(&u, s.bright);
    ub_s(&u, ", switch off after ");
    ub_dec(&u, auto_off_min);
    ub_s(&u, " min");
    sb_log(u.b);
}

/* a setting changed: save in 3 s (a later change moves the save) */
static uint64_t last_save;

void settings_changed(void)
{
    if (!slot_ok || kt_pending(KT_STATE, 0))
        return;                             /* a save is coming: it takes whatever is current then */
    uint64_t now = sb_ms(), at = now + SAVE_DELAY_MS;
    if (last_save && at < last_save + 10000U)
        at = last_save + 10000U;            /* at most one write in 10 s, whatever an app does */
    kt_set(KT_STATE, 0, now, (uint32_t)(at - now), 0);
}

/* 1.2-alpha18: Survival's heat log: a save within ms (at least ms after the last one) instead of the 10 s spacing */
void settings_changed_soon(uint32_t ms)
{
    if (!slot_ok || kt_pending(KT_STATE, 0))
        return;
    uint64_t now = sb_ms(), at = now + 200U;
    if (last_save && at < last_save + ms)
        at = last_save + ms;
    kt_set(KT_STATE, 0, now, (uint32_t)(at - now), 0);
}

/* Track A, 1.0: a safe start (the loader's one-shot SBFW_REQ_SAFE = 2, Settings > Startup or Starbite Recovery):
 * the settings and the shell's data are read as usual but nothing is written until the next start */
int sbos_safe_start(void)
{
    return sb_fw.valid && sb_fw.req_action == 2U;
}
void settings_timer(unsigned id)
{
    (void)id;
    if (!slot_ok)
        return;
    if (sbos_safe_start()) {
        static int said;
        if (!said) {
            said = 1;
            sb_log("settings: a safe start: nothing is saved until the next start");
        }
        return;
    }
    if (sb_storage_held()) {                /* 1.2-alpha21: the modem runs (no block writes): try again in 5 s */
        static uint64_t said;
        if (!said || sb_ms() - said > 60000U) {
            said = sb_ms();
            sb_log("settings: the save waits: storage writes are held (the modem runs)");
        }
        kt_set(KT_STATE, 0, sb_ms(), 5000, 0);
        return;
    }
    if (foreign) {
        sb_log("settings: not saved: the slot holds a newer version's settings");
        return;
    }
    /* keep every other record of the current half (later: the apps' states), replace ours */
    int to = cur_half < 0 ? 0 : 1 - cur_half;
    struct state_buf b;
    struct saved s;
    fill(&s);
    state_begin(&b, half[to], seq + 1);
    if (state_add(&b, REC, &s, sizeof(s)) < 0) {
        sb_log("settings: not saved: the record did not fit");
        return;
    }
    {
        uint8_t sv[16];
        surv_pack(sv);
        if (state_add(&b, "sbos.surv", sv, 16) < 0)
            sb_log("settings: the survival options did not fit");
    }
    {
        uint8_t pr[PASS_REC_LEN];           /* Track A: the passcode (only when one is set or tries are counted) */
        struct sbos_time t;
        int pn = pass_pack(pr, sb_ms(), clock_now(&t) ? (int64_t)t.utc : 0);
        if (pn > 0 && state_add(&b, PASS_REC, pr, (uint32_t)pn) < 0)
            sb_log("settings: the passcode record did not fit");
        memset(pr, 0, sizeof(pr));
    }
    {
        uint8_t ar[ALARM_REC_SIZE];         /* Track A: Next's alarm list (once it is the kernel's) */
        if (alarm_pack(ar) && state_add(&b, "sbos.alarm", ar, ALARM_REC_SIZE) < 0)
            sb_log("settings: the alarm list did not fit");
        int c = app_by_id("clock"), n = c >= 0 ? alarm_app_state(apps[c].state) : 0;
        if (n)
            apps[c].state_n = (uint32_t)n;  /* the classic Clock app's alarms the same as the list */
    }
    if (heat_rec.magic == 0x31435248U && state_add(&b, "heat", &heat_rec, sizeof(heat_rec)) < 0)
        sb_log("settings: the heat record did not fit");
    if (heat_log.magic == HLOG_MAGIC) {     /* 1.2-alpha6: the heat log in 4 KiB records */
        char id[12] = "heatlog0";
        const uint8_t *p = (const uint8_t *)&heat_log;
        for (uint32_t o = 0, k = 0; o < sizeof(heat_log); o += STATE_REC_MAX, ++k) {
            id[7] = (char)('0' + k);
            uint32_t len = sizeof(heat_log) - o < STATE_REC_MAX ? sizeof(heat_log) - o : STATE_REC_MAX;
            if (state_add(&b, id, p + o, len) < 0) {
                sb_log("settings: the heat log did not fit");
                break;
            }
        }
    }
    /* 0.5.3: each app's own state (SYS_STATE_PUT, up to 4 KiB), as "app:<id>" */
    for (int a = 0; a < napps; ++a) {
        if (!apps[a].state_n)
            continue;
        char id[24] = "app:";
        sb_strcpy(id + 4, sizeof(id) - 4, apps[a].id);
        if (state_add(&b, id, apps[a].state, apps[a].state_n) < 0)
            sb_logf("settings: an app's state did not fit: ", apps[a].id);
    }
    if (prefs_save(&b) < 0)                 /* Track A: the prefs and blobs (all prefs or none, each blob whole) */
        sb_log("settings: some prefs or blobs did not fit (the slot's half is full)");
    kmodem_sms_save(&b);                    /* Track G: the messages ("sms.NN", last: they take what is left) */
    uint32_t n = state_finish(&b);
    int w = hw_slot_write(to, half[to], n);
    if (w < 0) {
        sb_log("settings: dry run: not written");
        return;
    }
    if (!w) {
        ++save_fail;
        sb_log("settings: not saved: the slot write failed (the older copy stays)");
        return;
    }
    cur_half = to;
    ++seq;
    ++saves;
    last_save = sb_ms();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "settings: saved (half ");
    ub_dec(&u, (uint64_t)to);
    ub_s(&u, ", sequence ");
    ub_dec(&u, seq);
    ub_s(&u, ", ");
    ub_dec(&u, n);
    ub_s(&u, " bytes)");
    sb_log(u.b);
}

void settings_app_restore(int a)
{
    if (a >= 0 && a < napps && sb_streq(apps[a].id, "clock")) {
        int n = alarm_app_state(apps[a].state);    /* Track A: the kernel's list (restored first) wins */
        if (n) {
            apps[a].state_n = (uint32_t)n;
            return;
        }
    }
    if (!slot_ok || cur_half < 0 || a < 0 || a >= napps)
        return;
    char id[24] = "app:";
    sb_strcpy(id + 4, sizeof(id) - 4, apps[a].id);
    const uint8_t *d = 0;
    int n = state_find(half[cur_half], id, &d);
    if (n <= 0 || !d || n > (int)STATE_REC_MAX)
        return;
    memcpy(apps[a].state, d, (unsigned)n);
    apps[a].state_n = (uint32_t)n;
    sb_logf("settings: an app's state restored: ", apps[a].id);
}

void settings_save_now(void)
{
    if (!slot_ok)
        return;
    kt_cancel(KT_STATE, 0);
    settings_timer(0);
}

/* The self test's slot check (uitest.c): a dry run of a save (the fence logs the blocks it would write), then
 * one real save of the current settings, both halves read again and the newer picked. 1 passed, 0 failed,
 * -1 no slot (nothing written). */
int settings_selftest(void)
{
    if (!slot_ok || foreign)
        return -1;
    uint64_t seq0 = seq;
    int was_dry = sb_fence_is_dry_run();
    sb_fence_dry_run(1);
    settings_timer(0);                      /* "fence: would write ...", then "dry run: not written" */
    sb_fence_dry_run(was_dry);
    if (seq != seq0) {
        sb_log("selftest: slot: the dry run changed the sequence");
        return 0;
    }
    settings_timer(0);
    if (seq != seq0 + 1) {
        sb_log("selftest: slot: the save did not go through");
        return 0;
    }
    if (!hw_slot_read(half[0], half[1])) {
        sb_log("selftest: slot: the read after the save failed");
        return 0;
    }
    int pick = state_pick(half[0], half[1]);
    uint64_t s2 = 0;
    if (pick != cur_half || !state_valid(half[pick], &s2) || s2 != seq) {
        sb_log("selftest: slot: the newest copy is not the one just written");
        return 0;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: slot: dry run logged only, then saved sequence ");
    ub_dec(&u, seq);
    ub_s(&u, " to half ");
    ub_dec(&u, (uint64_t)cur_half);
    ub_s(&u, ", read back and picked: ok");
    sb_log(u.b);
    return 1;
}
