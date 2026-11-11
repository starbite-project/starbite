/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader (sd.c), for the card's OS store (store.c): reads, and writes only
 * inside the fence store.c opens (the home store's settings block and crash log, nothing else). */
#pragma once
#include <stdint.h>

/* Nothing here touches the card hardware until sd_allow(): store.c allows it when the settings
 * record says so (SET_SD_STORE), EngineerMode's "microSD card" page allows it by hand. */
void sd_allow(void);
int sd_allowed(void);
/* Card up (clocks, pins, rails, controller, SD init): 1 ready, 0 failed (sd_rep says where). */
int sd_up(void);
/* The card's rails were cycled (hw_sd_rails, before an OS start): the card is back in idle, so the
 * next read brings it up again (STC1 2026-11-11: every read after Linux's power cycle failed). */
void sd_forget(void);
/* count 512-byte sectors from sector to dst: 0 on success. */
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst);
/* The card's store partition (MBR entry of type 0xDA) in 4 KiB blocks: 1 found. */
int sd_store_area(uint64_t *first, uint64_t *blocks);
/* count 4 KiB blocks from 4 KiB block lba of the card to dst: 0 on success. */
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst);

/* The write fence: slot 0..SD_FENCES-1 holds [lba, lba + count) in 4 KiB blocks (count 0 closes
 * it). Only store.c sets it, to the home card store's settings block and crash log entry. */
#define SD_FENCES 2U
void sd_fence(unsigned slot, uint64_t lba, uint64_t count);
/* count 4 KiB blocks from src to 4 KiB block lba (CMD24/25, then the card's busy and CMD13 until
 * tran): 0 on success, -9 outside the fence (nothing touched), other < 0 a failed step. The
 * caller reads the blocks back. */
int sd_write4k(uint64_t lba, uint64_t count, uintptr_t src);

/* What the last sd_up() found, step by step (EngineerMode shows it). */
struct sd_report {
    int state;                  /* 0 not tried, 1 ready, -1 failed */
    const char *why;            /* the failed step */
    int present;                /* card detect GPA1-5 low */
    int rails;                  /* hw_sd_rails: 1 on, 0 refused, -1 no ACPM */
    uint8_t l31, l2;
    uint32_t gate[3], cmu_div, verid, hcon, clksel, clksel_was;
    uint64_t bus_hz, card_hz;
    uint32_t r7, ocr, rca, cid[4];
    int high_capacity;          /* SDHC/SDXC: block addressing */
    uint32_t last_rintsts, last_status;
    int last_error;             /* the last failed read_chunk / write_chunk (-1..-11), 0 none */
    uint32_t writes, write_errors, fenced;  /* sd_write4k calls: done, failed, refused (-9) */
    int card_state;             /* CMD13 state after the last recovery (4 tran), -1 no answer */
    uint32_t recoveries;        /* sd_recover() runs */
};
extern struct sd_report sd_rep;
