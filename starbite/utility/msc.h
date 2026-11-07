/* SPDX-License-Identifier: GPL-2.0-only
 * USB Mass Storage (Bulk-Only Transport + SCSI) for the Utility OS: msc.c. */
#pragma once
#include <stdint.h>
#include "usb_dwc3.h"

#define MSC_BLOCK 512U
#define MSC_CHUNK 0x20000U                     /* 128 KiB per USB transfer (DMA window) */

struct msc_lun {
    char vendor[9], product[17], serial[9];
    uint64_t blocks;                           /* 512-byte blocks */
    int ro, present;
    /* 0 on success; dst/src are in the non-cacheable DMA window, n blocks */
    int (*read)(struct msc_lun *l, uint64_t lba, uint32_t n, uint8_t *dst);
    int (*write)(struct msc_lun *l, uint64_t lba, uint32_t n, const uint8_t *src);
    uintptr_t ctx;
    uint64_t rd_bytes, wr_bytes;
    uint8_t sense_key, asc, ascq;
};

struct msc_stats { uint64_t cmds, errors, bad_cbw, resets; unsigned last_op; };
extern struct msc_stats msc_stats;

void msc_setup(struct msc_lun *luns, unsigned n);
void msc_event(const struct usbd_ev *e);       /* every driver event */
void msc_service(void);                        /* each loop turn: arm the next CBW */
int msc_busy(void);
