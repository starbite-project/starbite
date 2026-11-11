/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS protocol: SCSI CDBs, the command UPIU, the UTP transfer request descriptor (UTRD), the PRDT, the
 * response UPIU, and the doorbell audit ("gate") that decodes what was actually written into a slot.
 */
#ifndef SBUFS_PROTO_H
#define SBUFS_PROTO_H

#include "sbufs_port.h"
#include "sbufs_regs.h"
#include "sbufs_fence.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SBUFS_DIR_NONE = 0, SBUFS_DIR_READ = 1, SBUFS_DIR_WRITE = 2 };

struct sbufs_cmd {
	uint8_t lun;
	uint8_t dir;
	uint8_t cdb_len;
	uint8_t cdb[16];
	uint32_t xfer_len;              /* expected data transfer length, bytes */
};

struct sbufs_seg {
	uint64_t pa;
	uint32_t len;
};

/* the command descriptor's layout (the host reads it from the UTRD) */
struct sbufs_layout {
	uint32_t rsp_off, rsp_len;      /* bytes into the UCD */
	uint32_t prdt_off;
	uint32_t ucd_size;              /* room for UPIUs + PRDT */
	uint32_t stride;                /* bytes per PRDT entry: 16 (DESCTYPE 0) or 128 (FMP DESCTYPE 3) */
	uint32_t max_entries;           /* PRDT entries per command, <= (ucd_size - prdt_off) / stride */
	bool byte_gran;                 /* Exynos: UTRD offsets/lengths in bytes (else dwords / entries) */
	uint64_t dma_limit;             /* first address the host cannot reach (1 << 32, or 0 = none) */
};

/* the S9's layout: as herolte_ufs.c runs it on the phone (cmd 0..511, response 512..1023, PRDT from 1024) */
#define SBUFS_UCD_SIZE          0x800u
void sbufs_layout_s9(struct sbufs_layout *L, uint32_t max_entries, bool dma64);

/* CDB builders. Data commands pick the 10-byte form when it fits, else the 16-byte form. */
int sbufs_cdb_read(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks);
int sbufs_cdb_write(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks, bool fua);
int sbufs_cdb_sync(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks);
void sbufs_cdb_tur(struct sbufs_cmd *c, uint8_t lun);
void sbufs_cdb_inquiry(struct sbufs_cmd *c, uint8_t lun, uint8_t alloc);
void sbufs_cdb_read_cap10(struct sbufs_cmd *c, uint8_t lun);
void sbufs_cdb_read_cap16(struct sbufs_cmd *c, uint8_t lun);
void sbufs_cdb_ssu_active(struct sbufs_cmd *c);  /* START STOP UNIT, power condition ACTIVE, device W-LUN */

/* Write the UPIU, the PRDT and the UTRD of one slot. Every segment must be dword-aligned, a dword multiple,
 * at most SBUFS_PRD_MAX_BYTES, below dma_limit, and they must add up to xfer_len: else SBUFS_E_DMA (the caller
 * then bounces). */
int sbufs_build(const struct sbufs_layout *L, uint8_t *utrd, uint8_t *ucd, uint64_t ucd_pa,
    const struct sbufs_cmd *c, uint8_t tag, const struct sbufs_seg *segs, unsigned nsegs, bool irq);

/* What a slot says, decoded from its bytes. */
struct sbufs_decoded {
	uint8_t opcode, lun, dir;
	bool data;                      /* addresses the medium (read, write, sync) */
	bool write;                     /* write class (write, sync): needs write permission */
	uint64_t lba, nblks;
	uint32_t xfer_len;
	const char *why;                /* gate refusal reason */
};

/* The doorbell audit: decode the slot as built and check it against the whitelist and the fence (quietly).
 * Returns SBUFS_OK or SBUFS_E_GATE (d->why says why). */
int sbufs_gate(const struct sbufs_layout *L, const uint8_t *utrd, const uint8_t *ucd,
    const struct sbufs_fence *f, struct sbufs_decoded *d);

struct sbufs_result {
	int err;                        /* SBUFS_OK, SBUFS_E_IO, SBUFS_E_TIMEOUT, ... */
	uint8_t ocs, upiu, response, status;
	uint8_t sense_key, asc, ascq;
	uint32_t residual;
};

/* Read the slot's OCS and response UPIU after the doorbell bit cleared. */
void sbufs_parse_response(const struct sbufs_layout *L, const uint8_t *utrd, const uint8_t *ucd, uint8_t tag,
    struct sbufs_result *r);

#ifdef __cplusplus
}
#endif

#endif /* SBUFS_PROTO_H */
