/* SPDX-License-Identifier: GPL-2.0-only
 * Overclocking (docs/ENGINEER-OS.md section 14): the tables (octab.c, pure) and the page (oc.c).
 * Step 1 (this build) only reads. Step 2 will request only steps that exist in BOTH the chip's ECT
 * and ACPM's own frequency / voltage map, so ACPM applies its own ASV voltage; no PLL or PMIC
 * value is ever written here.
 */
#pragma once
#include <stdint.h>

#define OC_NDOM 13
#define OC_MAXLV 24
#define OC_MAXROW 8
#define OC_NONE 0xffffffffU
#define OC_ECT_MAX 0x100000U                /* S-Boot's ECT window at 0xA0000000 */
#define OC_FVMAP_SIZE 0x2000U               /* vendor fvmap.c FVMAP_SIZE */
#define OC_FVMAP_OFFSET 0x6700U             /* exynos9810.dtsi acpm fvmap_offset */

enum { OC_MIF, OC_INT, OC_A55, OC_M3, OC_G3D, OC_INTCAM, OC_FSYS0, OC_CAM, OC_DISP, OC_AUD, OC_IVA,
       OC_SCORE, OC_CP };

struct oc_dom_info {
    const char *name;                       /* "MIF" */
    const char *ect;                        /* the ECT's domain name */
    const char *what;
};
extern const struct oc_dom_info oc_info[OC_NDOM];

struct oc_dom {
    unsigned nlv;                           /* ECT levels (0: not in the ECT) */
    uint32_t khz[OC_MAXLV];                 /* descending */
    unsigned mm_rows;                       /* MINMAX rows (0: none) */
    uint32_t mm_min[OC_MAXROW], mm_max[OC_MAXROW], mm_boot[OC_MAXROW];  /* kHz */
    uint32_t cap_khz, cap_lo, cap_hi, boot_khz, min_khz;               /* the chosen row */
    int cap_exact;                          /* the row of this chip's ASV table version */
    unsigned fv_n, fv_init;                 /* ACPM fvmap levels (0: not found), its init level */
    uint32_t fv_khz[OC_MAXLV], fv_uv[OC_MAXLV];
};

struct oc_tables {
    struct oc_dom d[OC_NDOM];
    unsigned ect_domains, dvfs_parser;
    char ect_ver[5];
    unsigned asv_ver;
    int asv_known, fv_found;
};

uint32_t oct_ect_find(const uint8_t *mem, uint32_t size);
int oct_parse_ect(const uint8_t *ect, uint32_t size, struct oc_tables *t);
void oct_pick_row(struct oc_tables *t, unsigned asv_ver, int known);
int oct_parse_fvmap(const uint8_t *fv, uint32_t size, struct oc_tables *t);
uint32_t oct_uv(const struct oc_dom *o, uint32_t khz);
int oct_level(const struct oc_dom *o, uint32_t khz);
int oct_undervolted(const struct oc_dom *o, uint32_t khz);
int oct_allowed(const struct oc_dom *o, uint32_t khz);
int oct_above_cap(const struct oc_dom *o, uint32_t khz);

/* oc.c */
void oc_page(void);                         /* Clocks tab > Overclocking */
void oc_tick(void);                         /* main.c's ui_tick: feeds the watchdog while active */
void oc_selftest_boot(void) __attribute__((noreturn));  /* OC_SELFTEST: tables to pmsg, then recovery */
int oc_active(void);                        /* 1 while any domain is off its default step */

/* stability runs (lab.c, gpu.c): 1 pass, 0 fail, -1 not run; detail: one line */
int lab_oc_cpu(unsigned mask, unsigned secs, char *detail, unsigned cap);
int lab_oc_mem(unsigned size_idx, unsigned passes, char *detail, unsigned cap, unsigned *bw_read);
int lab_oc_gpu_prepare(void);
int lab_oc_gpu_up(void);
int lab_oc_gpu(unsigned secs, char *detail, unsigned cap);
int lab_oc_m3_probe(int may_ask, uint64_t v[6], uint64_t *esr, const char **why);
