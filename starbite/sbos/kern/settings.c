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
#include "sbapp.h"

#define REC "sbos.settings"
#define SAVE_DELAY_MS 3000U

struct saved {
    uint32_t version;                       /* 1 */
    uint32_t screen_off_s;
    uint8_t panel_save, panel_off, sos_led, bright;
    uint32_t pad[4];
};

static uint8_t *half[2];
static int slot_ok;                         /* 1 the slot exists and was read */
static int cur_half = -1;                   /* the half that holds the newest valid copy */
static uint64_t seq;
static unsigned saves, save_fail;

int settings_persistent(void)
{
    return slot_ok;
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
}

void settings_restore(void)
{
    if (!half[0]) {
        half[0] = sb_alloc(STATE_HALF);
        half[1] = sb_alloc(STATE_HALF);
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
    const uint8_t *d = 0;
    int n = state_find(half[cur_half], REC, &d);
    struct saved s;
    if (n < (int)sizeof(uint32_t) * 2 + 4 || !d) {
        sb_log("settings: no settings record in the slot: defaults");
        return;
    }
    memset(&s, 0, sizeof(s));
    memcpy(&s, d, (unsigned)n < sizeof(s) ? (unsigned)n : sizeof(s));
    if (s.version != 1) {
        sb_log("settings: a settings record of another version: defaults");
        return;
    }
    if (s.screen_off_s >= 10 && s.screen_off_s <= 600) {
        K.screen_off_s = s.screen_off_s;
        power_input();                      /* the screen-off timer with the restored time */
    }
    panel_save_set(s.panel_save != 0);
    panel_off_on = s.panel_off != 0;
    sos_led_on = s.sos_led != 0;
    if (s.bright >= 1 && s.bright <= 4)
        (void)panel_brightness(s.bright);
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
    sb_log(u.b);
}

/* a setting changed: save in 3 s (a later change moves the save) */
void settings_changed(void)
{
    if (!slot_ok)
        return;
    kt_set(KT_STATE, 0, sb_ms(), SAVE_DELAY_MS, 0);
}

void settings_timer(unsigned id)
{
    (void)id;
    if (!slot_ok)
        return;
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
    uint32_t n = state_finish(&b);
    if (!hw_slot_write(to, half[to], n)) {
        ++save_fail;
        sb_log("settings: not saved: the slot write failed (the older copy stays)");
        return;
    }
    cur_half = to;
    ++seq;
    ++saves;
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
