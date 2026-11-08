/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: a fake SoC for stage 3 (sbm_boot.c): the PMU's CP registers, EL3 (CP_CTRL, the SSS clock,
 * the security call checking the loaded image by CRC), the mailbox, AP2CP_CFG, the carve-out (only where the
 * plan puts sections) and a clock. It records every store and every EL3 call for the allow-list checks. */
#ifndef FAKESOC_H
#define FAKESOC_H
#include "../sbm.h"

#define FS_MAXREC 1024
#define FS_REGIONS 4
struct fakesoc {
    uint32_t pmu[0x2000 / 4];
    uint32_t mbox[SBM_MBOX_SIZE / 4];
    uint32_t ap2cp_cfg, gicd_pend;
    uint32_t ctrl[2];                   /* CP_CTRL_S, CP_CTRL_NS (EL3 only) */
    int sss_on;
    /* the image EL3 accepts: CRC-32 of BOOT at + 0 and MAIN at + 0x10000, and their sizes */
    uint32_t boot_crc, main_crc, boot_size, main_size;
    int validated, cp_running;
    uint64_t clock_us, run_at_us;
    struct { uint32_t off, size; uint8_t *mem; } reg[FS_REGIONS];
    unsigned nreg;
    /* faults to inject */
    int refuse_el3_write, refuse_carve, never_status, started_before, warm_pwron;
    /* records */
    struct { uint64_t pa; uint32_t v; } wr[FS_MAXREC];
    unsigned nwr;
    struct { uint64_t a[4]; uint32_t ret; } smc[FS_MAXREC];
    unsigned nsmc;
    unsigned carve_writes, carve_refused;
};
void fakesoc_init(struct fakesoc *s, const struct sbm_plan *plan, uint32_t boot_crc, uint32_t main_crc);
void fakesoc_free(struct fakesoc *s);
void fakesoc_plat(struct fakesoc *s, struct sbm_plat *p, void (*log)(void *, const char *));
#endif
