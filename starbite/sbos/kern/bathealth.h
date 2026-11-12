/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: battery health from the MAX77705 fuel gauge, read only (bathealth.c).
 *
 * Wiring: the gauge (ModelGauge m5) at 7-bit 0x36 on HSI2C 0x14360000, read through libsb's sb_fg_word_checked.
 * Words (1 mAh a step with the phone's 5 mOhm sense resistor, DT fuelgauge,fg_resistor 2; libsb's 312.5 uA current
 * step agrees): STATUS 0x00, Cycles 0x17 (1/100 cycle), DesignCap 0x18, FullCapNom 0x23, FullCapRep 0x35.
 * Nothing is ever written to the gauge or anywhere else.
 *
 * The kernel-facing calls: bathealth_read() fills the struct (1 read, 0 not read: bathealth_why()); the UI shows
 * "Maximum Capacity <shown_pct> %", "Cycle count <cycles>" and the words. */
#ifndef SBOS_BATHEALTH_H
#define SBOS_BATHEALTH_H
#include <stdint.h>

#define BATH_DT_DESIGN_MAH 2967U        /* DT fuelgauge,capacity 0xB97 (battery_full_capacity 3000) */

struct bathealth {
    int ok;                             /* 1: the numbers below were read */
    unsigned design_mah;                /* DesignCap (or the DT's when the gauge's is not plausible: design_from_dt) */
    unsigned fullnom_mah;               /* FullCapNom: the full charge at nominal conditions (what ageing lowers) */
    unsigned fullrep_mah;               /* FullCapRep: the full charge the gauge reports now (0 if not read) */
    unsigned cycles_x100;               /* Cycles: 1/100 of a full cycle a step (wraps at 655.35) */
    unsigned cycles;                    /* whole cycles */
    int max_pct;                        /* FullCapNom / DesignCap, rounded (may be above 100 on a new cell) */
    int shown_pct;                      /* max_pct capped to 0..100: "Maximum Capacity 87 %" */
    int rep_pct;                        /* FullCapRep / DesignCap, rounded, or -1 */
    int design_from_dt;                 /* 1: the gauge's DesignCap was not plausible, the DT's 2967 mAh used */
    int gauge_restarted;                /* STATUS POR: the gauge lost power and restarted from its defaults */
    int no_battery;                     /* STATUS Bst */
    const char *words;                  /* plain words for the UI */
};

int bathealth_read(struct bathealth *h);
const char *bathealth_why(void);
/* The pure maths (host-tested): the words / percentages from the raw words. status < 0: STATUS not read. */
void bathealth_compute(struct bathealth *h, int status, unsigned design, unsigned fullnom, unsigned fullrep,
                       unsigned cycles_raw);

/* The probe (drvprobe.h): levels 0 and 1 read and log; level 2 nothing. Budget BATH_PROBE_BUDGET_MS. */
#define BATH_PROBE_BUDGET_MS 2000U
int drv_bathealth_probe(int level, unsigned *wait_ms);
void drv_bathealth_abort(void);

#endif
