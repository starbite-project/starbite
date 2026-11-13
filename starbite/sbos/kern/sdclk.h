/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS kernel: the microSD card's CMU divider (sdclk.c). libsb sd.c never writes CMU blocks; this does, for
 * the card's own divider only, with the card clock stopped (sd_set_bus_hook / sd_rebus). */
#ifndef SBOS_SDCLK_H
#define SBOS_SDCLK_H
#include <stdint.h>

#define SDCLK_CIU_MAX 810000000ULL          /* the CIU input at most: stock clock-frequency 800 MHz + 1 % */

struct sdclk_report {
    uint32_t div_reg, mux, user, pll0, pll3; /* the registers as found (DIV, MUX, FSYS1 user mux, PLL_CON0/3) */
    uint64_t parent_hz;                     /* the mux's parent read off its PLL, 0 unknown */
    uint32_t div_was, div_now;              /* DIVRATIO (bits 8:0): /(n + 1) */
    uint64_t bus_was, bus_now;              /* the controller's bus clock (CIU / 4), Hz */
    uint32_t busy_reads;                    /* reads until the divider's busy bit cleared */
    const char *pname;                      /* the mux's parent: PLL_MMC, PLL_SHARED2, OSCCLK, "?" */
    int raised;                             /* 1 the divider is ours (sdclk_restore puts div_was back) */
    const char *why;                        /* the last refusal, 0 none */
};
extern struct sdclk_report sdclk_rep;

/* sd_up's hook (sd.h sd_bus_fn): 0 raise, 1 put the old divider back. Returns the bus clock in Hz, 0 unknown. */
uint64_t sdclk_change(int restore);
/* Before the card's first sd_up: registers the hook (raised then, with the card clock stopped). On a card that is
 * up: raised now between commands (sd_rebus). 1 raised, 0 not (sdclk_rep.why), -1 the card failed. */
int sdclk_raise(void);
/* The divider as found again (on a card that is up through sd_rebus). 0 done or nothing to do, -1 failed. */
int sdclk_restore(void);
/* The parent rate of a PLL_CON0 (and PLL_CON3 for a fractional PLL) from FIN 26 MHz, 0 unknown. */
uint64_t sdclk_pll_hz(uint32_t con0, uint32_t con3, int frac);
/* The smallest DIVRATIO whose CIU rate is <= SDCLK_CIU_MAX. */
uint32_t sdclk_pick(uint64_t parent_hz);

#endif
