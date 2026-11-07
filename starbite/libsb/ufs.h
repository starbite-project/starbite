/* SPDX-License-Identifier: GPL-2.0-only
 * UFS for libsb (ufs.c, copied from shim/starlte/ufs.c): commands on S-Boot's running host. Reads
 * go through ufs_scsi's read-only opcode list; the only write is ufs_write_blocks (WRITE(10) of
 * single blocks, the loader's ufs_write_block), which refuses unless fence.c's sb_write runs it.
 */
#pragma once
#include <stdint.h>

#define UFS 0x11120000UL
/* DMA memory: libsb's non-cacheable window (mmu.c), the address the loader uses too. */
#define UFS_DMA 0xaf000000UL
#define UFS_BUF (UFS_DMA + 0x4000)
#define UFS_BUF_SIZE (1U << 20)
/* Engineer OS v2 I/O buffers in the same non-cacheable window: 0xaf200000 one UFS write staging
 * block (ufs.c), 0xaf400000..0xaf800000 the lab I/O buffer (partition copies, read-back verify):
 * the first 3 MiB for the caller, the last 1 MiB for the store writer (store.c). */
#define SB_IO_BUF (UFS_DMA + 0x400000)
#define SB_IO_SIZE (3U << 20)
#define SB_STORE_IO (UFS_DMA + 0x700000)

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
/* The same on logical unit lu (0 = the main LU with the GPT; the S9's boot LUs are 1..4). */
int ufs_read_lu(unsigned lu, uint64_t lba, uint32_t count);
int ufs_read_lu_to(unsigned lu, uint64_t lba, uint64_t count, uintptr_t dst);
/* WRITE(10) of count 4 KiB blocks from src (any RAM), one block per command. -6 unless the write
 * fence is active (only sb_write calls it); 0 on success. */
int ufs_write_blocks(unsigned lu, uint64_t lba, uint64_t count, uintptr_t src);
/* Set by the fence around its volatile-entry writes: WRITE(10) with FUA (forced unit access). */
extern int ufs_fua;
/* SYNCHRONIZE CACHE(10) of the LU: -6 unless the fence is active. 0 done. */
int ufs_sync_cache(unsigned lu);

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
