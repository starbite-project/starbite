/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: battery health ("Maximum Capacity 87 %", the cycle count) from the MAX77705 fuel gauge,
 * strictly read only (docs/drivers/BATTERY-HEALTH.md).
 *
 * Wiring and sources: the gauge at 7-bit 0x36 on HSI2C 0x14360000 (libsb max77705.c, which brings the bus up and
 * retries a failed word: sb_fg_word_checked). Register map: Samsung's max77705_fuelgauge.h (FG_FULLCAPNOM, FG_FULLCAPREP,
 * FG_CYCLE) and Maxim's ModelGauge m5 map, as Engineer OS lab.c reads them (0x17 Cycles, 0x18 DesignCap,
 * 0x23 FullCapNom, 0x35 FullCapRep, 0x00 STATUS). Units: 1 mAh a step (5 mOhm sense resistor, DT fuelgauge,fg_resistor
 * 2, DesignCap programmed 0xB97 = 2967 mAh by Samsung's model load); Cycles 1/100 cycle a step (an earlier port's facts, which
 * ran on this phone, agree).
 *
 * Health: FullCapNom / DesignCap. FullCapNom is the gauge's learned full charge at nominal temperature and rate, so it
 * moves with the cell's age, not with today's temperature or load (FullCapRep does, it is shown as a second number).
 * Seen on a phone: Engineer OS lab.c shows these words raw; this file's maths has not run on the phone yet.
 *
 * Never: a gauge write (no sb_fg_write16, no sb_max_write), the battery's PCM, the charger. */
#include "sb.h"
#include "bathealth.h"

#define R_STATUS 0x00U
#define R_CYCLES 0x17U
#define R_DESIGN 0x18U
#define R_FULLNOM 0x23U
#define R_FULLREP 0x35U
#define ST_POR 0x0002U
#define ST_BST 0x0008U
#define DESIGN_LO 1500U                 /* a DesignCap outside this is not this phone's cell */
#define DESIGN_HI 6000U

static const char *why = "not read yet";

static int pct(unsigned num, unsigned den)
{
    return den ? (int)((num * 100U + den / 2U) / den) : -1;
}

void bathealth_compute(struct bathealth *h, int status, unsigned design, unsigned fullnom, unsigned fullrep,
                       unsigned cycles_raw)
{
    h->ok = 1;
    h->design_from_dt = design < DESIGN_LO || design > DESIGN_HI;
    h->design_mah = h->design_from_dt ? BATH_DT_DESIGN_MAH : design;
    h->fullnom_mah = fullnom;
    h->fullrep_mah = fullrep;
    h->cycles_x100 = cycles_raw;
    h->cycles = cycles_raw / 100U;
    h->gauge_restarted = status >= 0 && (status & ST_POR);
    h->no_battery = status >= 0 && (status & ST_BST);
    /* FullCapNom beyond 0..1.5 x design is not a learned value: FullCapRep instead, else not known */
    unsigned full = fullnom;
    if (!full || full > h->design_mah * 3U / 2U)
        full = fullrep && fullrep <= h->design_mah * 3U / 2U ? fullrep : 0;
    h->max_pct = full ? pct(full, h->design_mah) : -1;
    h->rep_pct = fullrep && fullrep <= h->design_mah * 3U / 2U ? pct(fullrep, h->design_mah) : -1;
    h->shown_pct = h->max_pct < 0 ? -1 : h->max_pct > 100 ? 100 : h->max_pct;
    if (h->no_battery)
        h->words = "No battery found.";
    else if (h->max_pct < 0)
        h->words = "The battery's capacity is not known yet.";
    else if (h->gauge_restarted)
        h->words = "The charge meter restarted and is still learning this battery; the number settles over a few "
                   "full charges.";
    else if (h->max_pct >= 80)
        h->words = "Your battery is in good health.";
    else if (h->max_pct >= 60)
        h->words = "Your battery holds noticeably less than when new. It still works; a new battery would last "
                   "longer on a charge.";
    else
        h->words = "Your battery holds much less than when new. A new battery is recommended.";
}

static int word(uint8_t reg, uint16_t *v)
{
    return sb_fg_word_checked(reg, v, 0, 0);
}

int bathealth_read(struct bathealth *h)
{
    uint16_t st = 0, cy = 0, de = 0, fn = 0, fr = 0;
    h->ok = 0;
    h->words = "The battery's health could not be read.";
    if (!sb_max_init()) {
        why = sb_max_why ? sb_max_why : "the charger chip does not answer";
        return 0;
    }
    int ok_st = word(R_STATUS, &st);
    if (!word(R_DESIGN, &de) || !word(R_FULLNOM, &fn) || !word(R_CYCLES, &cy)) {
        why = "the fuel gauge did not answer (0x36 on HSI2C 0x14360000)";
        return 0;
    }
    if (!word(R_FULLREP, &fr))
        fr = 0;
    bathealth_compute(h, ok_st ? (int)st : -1, de, fn, fr, cy);
    why = h->words;
    return 1;
}

const char *bathealth_why(void)
{
    return why;
}

/* ---- the probe (drvprobe.h) ------------------------------------------------------------------------------------- */

void drv_bathealth_abort(void)
{
    /* nothing to put back: nothing is ever written */
}

int drv_bathealth_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
    if (level == 2) {
        sb_log("drvprobe: bathealth: PASS nothing at level 2 (read only)");
        return 1;
    }
    struct bathealth h;
    if (!bathealth_read(&h)) {
        sb_logf("drvprobe: bathealth: FAIL ", why);
        return 1;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: bathealth: design ");
    ub_dec(&u, h.design_mah);
    ub_s(&u, h.design_from_dt ? " mAh (the DT's; the gauge's was not plausible)" : " mAh");
    ub_s(&u, ", full nominal ");
    ub_dec(&u, h.fullnom_mah);
    ub_s(&u, " mAh, full reported ");
    ub_dec(&u, h.fullrep_mah);
    ub_s(&u, " mAh, cycles ");
    ub_dec(&u, h.cycles);
    ub_c(&u, '.');
    ub_dec2(&u, h.cycles_x100 % 100U);
    if (h.gauge_restarted)
        ub_s(&u, ", gauge restarted (POR)");
    sb_log(u.b);
    ub_init(&u);
    if (h.no_battery || h.max_pct < 0) {
        ub_s(&u, "drvprobe: bathealth: FAIL ");
        ub_s(&u, h.words);
    } else {
        ub_s(&u, "drvprobe: bathealth: PASS maximum capacity ");
        ub_dec(&u, (uint64_t)h.max_pct);
        ub_s(&u, " %, ");
        ub_dec(&u, h.cycles);
        ub_s(&u, " cycles");
    }
    sb_log(u.b);
    return 1;
}
