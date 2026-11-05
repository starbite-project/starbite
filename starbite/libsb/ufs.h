/* SPDX-License-Identifier: GPL-2.0-only
 * UFS for libsb (ufs.c, copied from shim/starlte/ufs.c): commands on S-Boot's running host, read
 * only. libsb has no WRITE(10): the loader's ufs_write_block (its settings record) is not here.
 */
#pragma once
#include <stdint.h>

#define UFS 0x11120000UL
/* DMA memory: libsb's non-cacheable window (mmu.c), the address the loader uses too. */
#define UFS_DMA 0xaf000000UL
#define UFS_BUF (UFS_DMA + 0x4000)
#define UFS_BUF_SIZE (1U << 20)

extern int ufs_state;           /* 0 untouched, 1 ready, -1 failed */
extern int ufs_why;             /* 1 host not ready, 2 busy, 3 list refused, 4 no answer */
extern uint32_t ufs_ocs, ufs_resp, ufs_status, ufs_qresp;

int ufs_up(void);
/* SCSI command to LU0; len bytes in, into UFS_BUF. Only read-direction commands are accepted. */
int ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len);
/* QUERY READ DESCRIPTOR; returns the bytes copied or < 0. */
int ufs_desc(uint8_t idn, uint8_t index, uint8_t *out, unsigned cap);
/* READ(10) of 4 KiB blocks into UFS_BUF (count <= 256). */
int ufs_read(uint64_t lba, uint32_t count);
/* READ(10) of 4 KiB blocks straight into RAM at dst (DMA, 128 KiB per command). */
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst);

static inline uint8_t u8r(uintptr_t a)
{
    return *(volatile uint8_t *)a;
}

static inline void u_zero(uintptr_t a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 8)
        *(volatile uint64_t *)(a + i) = 0;
}

/* LU0 GPT, loaded once. */
#define GPT_MAX 48
extern char gpt_name[GPT_MAX][24];
extern uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
extern int gpt_n;
int gpt_load(void);
/* Partition by GPT name: 1 found. */
int gpt_find(const char *name, uint64_t *first, uint64_t *last);
/* GPT header facts for the viewer: 1 if the primary header was read. */
struct gpt_head {
    uint64_t my_lba, alt_lba, first_usable, last_usable, entries_lba;
    uint32_t hdr_crc, hdr_crc_calc, num, esz, ent_crc, ent_crc_calc;
    uint8_t guid[16];
};
int gpt_header(uint64_t lba, struct gpt_head *h);
