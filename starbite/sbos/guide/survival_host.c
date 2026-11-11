/* SPDX-License-Identifier: MIT
 * The survival API (kern/survival.h) for the toolkit's host harness (starbite/sbos/ui/host): the same functions with
 * simulated state, so Starbite OS Next's Emergency screens can be built, filmed and diffed on the Mac. Time comes from
 * surv_host_now_ms (the harness sets it each frame). Nothing here touches hardware. Not linked into the kernel. */
#include <string.h>
#include "../kern/survival.h"
#include "sbguide.h"

uint64_t surv_host_now_ms;                  /* the harness's clock, ms */
static uint64_t t_on, t_cool;
static int st = SURV_USB_OFF, refused;
static uint32_t bursts;
static const char *why;

const char *const surv_usb_guide[] = {
    "5 V from the USB-C port, at most 1.5 A (the charger chip's own limit), 10 s a burst.",
    "To start a fire: a USB-C cable or adapter with bare ends, then fine steel wool (0000) or a thin wire on dry tinder.",
    "Touch both ends to the steel wool at once; it glows. Keep the phone and the cable away from the flame.",
    "Never short the phone's port directly and never put metal into it.",
    "Unplug any charger or computer first. The phone stops by itself on any fault, when the battery is hot or low.",
    0,
};

static void advance(void)
{
    if (st == SURV_USB_ON && surv_host_now_ms - t_on >= SURV_USB_BURST_MS) {
        st = SURV_USB_COOL;
        t_cool = surv_host_now_ms + SURV_USB_COOL_MS;
        why = "the 10 s burst is over";
    }
    if (st == SURV_USB_COOL && surv_host_now_ms >= t_cool)
        st = SURV_USB_OFF;
}

int surv_usb_start(void)
{
    advance();
    if (st == SURV_USB_ON)
        return 0;
    if (st == SURV_USB_COOL) {
        why = "cooling down after the last burst";
        refused = 1;
        return -1;
    }
    st = SURV_USB_ON;
    t_on = surv_host_now_ms;
    refused = 0;
    why = 0;
    ++bursts;
    return 0;
}

void surv_usb_stop(const char *w)
{
    advance();
    if (st != SURV_USB_ON)
        return;
    st = SURV_USB_COOL;
    t_cool = surv_host_now_ms + SURV_USB_COOL_MS;
    why = w;
}

void surv_usb_status(struct surv_usb *s)
{
    advance();
    memset(s, 0, sizeof(*s));
    s->state = st;
    s->ms_left = st == SURV_USB_ON ? (int32_t)(SURV_USB_BURST_MS - (surv_host_now_ms - t_on)) :
                 st == SURV_USB_COOL ? (int32_t)(t_cool - surv_host_now_ms) : 0;
    uint64_t on = st == SURV_USB_ON ? surv_host_now_ms - t_on : 0;
    s->est_ma = st == SURV_USB_ON ? (on < 300 ? -1 : 1180 + (int32_t)(on / 100 % 7) * 9) : -1;
    s->bat_ma = st == SURV_USB_ON ? -1700 : -380;
    s->bat_mv = st == SURV_USB_ON ? 3720 : 3860;
    s->vbus_lo_mv = st == SURV_USB_ON ? 3500 : -1;
    s->vbus_hi_mv = st == SURV_USB_ON ? 4500 : -1;
    s->bat_c10 = 312 + (int32_t)(on / 1000) * 3;
    s->limit_c10 = 650;
    s->ilim_ma = 1500;
    s->bursts = bursts;
    s->refused = refused;
    s->why = why;
}

uint32_t surv_sos_opts = SURV_SOS_TORCH | SURV_SOS_VIB;
static const uint8_t morse[] = { 1, 1, 1, 1, 1, 3, 3, 1, 3, 1, 3, 3, 1, 1, 1, 1, 1, 7 };
uint32_t surv_morse_cycle_ms(void) { return 34U * SURV_MORSE_UNIT_MS; }
int surv_morse_at(uint32_t ms, uint32_t *next_ms)
{
    uint32_t cyc = surv_morse_cycle_ms(), base = ms - ms % cyc, at = 0;
    ms %= cyc;
    for (unsigned i = 0; i < sizeof(morse); ++i) {
        uint32_t end = at + morse[i] * SURV_MORSE_UNIT_MS;
        if (ms < end) {
            if (next_ms)
                *next_ms = base + end;
            return !(i & 1U);
        }
        at = end;
    }
    return 0;
}

int32_t surv_lat_x100 = 6017, surv_lon_x100 = 2494;    /* the host: Helsinki */
int surv_sun(int32_t lat, int32_t lon, int y, int mo, int d, int off, int *rise, int *set)
{
    (void)lat, (void)lon, (void)y, (void)mo, (void)d, (void)off;
    *rise = 8 * 60 + 9;                     /* the kernel's answer for Helsinki, 2026-11-11 */
    *set = 15 * 60 + 59;
    return 0;
}
int surv_sun_today(int *rise, int *set) { return surv_sun(surv_lat_x100, surv_lon_x100, 2026, 11, 11, 120, rise, set); }

const char *const surv_card_label[SURV_CARD_NF] = { "Name", "Blood type", "Allergies", "Medicines", "Conditions",
                                                    "Call", "Call 2", "Call 3", "Notes" };
static struct surv_card card;
static int have_card;
int surv_card_get(struct surv_card *c)
{
    if (!have_card) {
        static const char *const demo[SURV_CARD_NF] = { "Alex Example", "0+", "Penicillin", "None", "Asthma",
                                                        "Sam (partner) +358 40 000 0000", "", "", "Inhaler in the left pocket" };
        for (int i = 0; i < SURV_CARD_NF; ++i)
            strncpy(card.f[i], demo[i], SURV_CARD_FL);
        have_card = 1;
    }
    *c = card;
    return 1;
}
int surv_card_set(const struct surv_card *c) { card = *c; have_card = 1; return 1; }

int surv_last2_auto = 1;
static int last2;
int surv_last2(void) { return last2; }
int surv_last2_set(int on, const char *w) { (void)w; last2 = on != 0; return 0; }
int surv_last2_battery(int pct, int mv, int chg) { (void)pct, (void)mv, (void)chg; return -1; }

const char *surv_sensors_why(void)
{
    return "The compass and the level need the sensor hub inside the GNSS chip (BCM4775): its driver is not written yet.";
}

const struct sbg_chapter *surv_guide(int *n)
{
    if (n)
        *n = SBG_NCH;
    return sbg_chapters;
}
