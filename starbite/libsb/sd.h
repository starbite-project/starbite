/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader (sd.c), for the card's OS store (store.c). Writes only through
 * the write fence (fence.c sb_write): sd_write refuses otherwise. */
#pragma once
#include <stdint.h>

/* Nothing here touches the card hardware until sd_allow(): store.c allows it when the settings
 * record says so (SET_SD_STORE), EngineerMode's "microSD card" page allows it by hand. */
void sd_allow(void);
int sd_allowed(void);
/* Card up (clocks, pins, rails, controller, SD init): 1 ready, 0 failed (sd_rep says where). */
int sd_up(void);
/* count 512-byte sectors from sector to dst: 0 on success. */
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst);
/* The card's store partition (MBR entry of type 0xDA) in 4 KiB blocks: 1 found. */
int sd_store_area(uint64_t *first, uint64_t *blocks);
/* count 4 KiB blocks from 4 KiB block lba of the card to dst: 0 on success. */
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst);
/* Writes: -9 unless the write fence is active (sb_write). 0 on success. */
int sd_write(uint64_t sector, uint32_t count, uintptr_t src);
int sd_write4k(uint64_t lba, uint64_t count, uintptr_t src);
/* The end of the MBR's last partition, in sectors (a lower bound of the card size): 0 unknown. */
uint64_t sd_sectors_hint(void);

/* Fast reads (sd.c's header): off unless an image asks. SD_FAST_ON: CMD6 high speed at the fastest card
 * clock <= 50 MHz the bus gives (33.58 MHz from 201.5 MHz, 50.0 MHz from 200.0 MHz) and CMD18 by IDMAC; SD_FAST_HS50 the same at
 * 50.375 MHz (CLKDIV 2: Linux's and XNU's clock, 0.75 % over the HS limit). Read only; writes keep the old
 * clock and code. Before sd_up it is only remembered (sd_up switches); on a card that is up it switches now.
 * Any failure: 25 MHz PIO again and one log line. 1 when fast reads are on afterwards. */
#define SD_FAST_OFF 0
#define SD_FAST_ON 1
#define SD_FAST_HS50 2
int sd_set_fast(int mode);
int sd_fast_mode(void);                     /* the last sd_set_fast mode */
/* CMD6 mode 0 (a read-only query; ACMD51 first when the SCR is not known): 1 the card offers high speed,
 * 0 not, -1 no card or no answer. */
int sd_hs_query(void);

/* The card's bus clock (Starbite OS's kernel only: libsb never writes CMU blocks; kern/sdclk.c). fn(0) may change the
 * card's CMU divider (DIV_CLKCMU_FSYS1_MMC_CARD) and returns the controller's bus clock after it in Hz (after CLKSEL's
 * /4), 0 = unknown (sd.c then estimates it from the divider); fn(1) puts the old divider back and returns the bus
 * clock of that, 0 unknown. sd_up calls fn(0) once, after the controller reset and before the 400 kHz init, with the
 * card clock stopped; when the divider changed and the card then does not come up, it calls fn(1) and tries once
 * more from the old clock. sd_rebus does the same on a card that
 * is up (between commands: the card clock stopped, fn(restore), the clock back at the running mode's rate from the
 * new bus clock): 0 ok, -1 not up or a clock update failed (then 25 MHz PIO). */
typedef uint64_t (*sd_bus_fn)(int restore);
void sd_set_bus_hook(sd_bus_fn fn);
int sd_rebus(sd_bus_fn fn, int restore);

/* What the last sd_up() found, step by step (EngineerMode shows it). */
struct sd_report {
    int state;                  /* 0 not tried, 1 ready, -1 failed */
    const char *why;            /* the failed step */
    int present;                /* card detect GPA1-5 low */
    int rails;                  /* hw_sd_rails: 1 on, 0 refused, -1 no ACPM */
    uint8_t l31, l2;
    uint32_t gate[3], cmu_div, verid, hcon, clksel, clksel_was;
    uint64_t bus_hz, card_hz;
    uint64_t write_hz;                          /* the card clock the last write ran at (sd_write), 0 none yet */
    int write_hs;                               /* the card's mode at that write: 0 default speed, 1 high speed */
    uint32_t r7, ocr, rca, cid[4];
    int high_capacity;          /* SDHC/SDXC: block addressing */
    uint32_t last_rintsts, last_status;
    int last_error;             /* the last failed read_chunk (-1..-8), 0 none */
    int card_state;             /* CMD13 state after the last recovery (4 tran), -1 no answer */
    uint32_t recoveries;        /* sd_recover() runs */
    /* fast reads (sd_set_fast) */
    uint32_t csd[4];            /* CMD9 at sd_up when fast reads were asked for first (RESP0..3) */
    int csd_ok;
    uint8_t scr[8];             /* ACMD51, bus order: scr[0] = SCR[63:56] */
    int scr_ok;
    int hs_asked, hs_support;   /* CMD6 query: 1 offered, 0 not, -1 no answer */
    int hs_card;                /* the card took high speed (CMD6 mode 1) */
    int fast;                   /* 0 the old path, else the SD_FAST_* mode running */
    const char *fast_why;       /* why fast reads are off after the last attempt, 0 none */
    uint32_t dma_chunks, dma_errors, dma_idsts;
    int dma_last_error;         /* the last failed IDMAC chunk (-1..-13), 0 none */
    /* the bus clock hook (sd_set_bus_hook) */
    int bus_set;                /* 0 none, 1 the hook changed the divider, 2 it gave the rate of the unchanged one,
                                 * -1 the card failed on the changed one (put back), -2 the card clock did not stop
                                 * (not called), -3 the hook did not know the rate */
    uint32_t cmu_div_was;       /* the divider and bus clock sd_up found, before the hook */
    uint64_t bus_hz_was;
    const char *bus_why;        /* bus_set -1: the step that failed on the raised clock */
};
extern struct sd_report sd_rep;
