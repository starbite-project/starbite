/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS fence: the driver may touch ONLY the LBA ranges Starbite owns.
 *
 * Three layers, all inside the driver (none trusts the caller):
 *  1. the allow-list, "starbite,allowed-ranges" on /arm-io/ufs (the loader writes it from the store / GPT):
 *     every data command must lie wholly inside ONE allowed range;
 *  2. the deny-list, "starbite,denied-ranges" (optional): Android's partitions by name (EFS, CPEFS, PARAM,
 *     KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB, DTBO, BOOT, RECOVERY, RADIO, the GPT head and tail ...). An
 *     allowed range that meets a denied extent is dropped at start, and every command is checked against it again;
 *  3. built-in floors that need no property: the first SBUFS_HEAD_FLOOR and last SBUFS_TAIL_FLOOR blocks of
 *     every LU (protective MBR, primary and backup GPT) are never inside a usable range, nor is a block past the
 *     LU's READ CAPACITY; well-known LUs and LUs 8.. are never data targets.
 * Writes need the range's WRITABLE flag AND the global write switch (ADT "starbite,writes-allowed" = 1 AND
 * boot-arg sbufsrw=1). M4 runs with neither: read-only.
 * The property layout is in NOTES.md ("Fence property format"); the host tests build it byte by byte.
 */
#ifndef SBUFS_FENCE_H
#define SBUFS_FENCE_H

#include "sbufs_port.h"
#include "sbufs_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBUFS_MAX_RANGES        8u
#define SBUFS_MAX_DENY          48u
#define SBUFS_HEAD_FLOOR        6u      /* LBA 0 (MBR), 1 (GPT header), 2..5 (128 x 128-byte entries, 4 KiB blocks) */
#define SBUFS_TAIL_FLOOR        6u      /* backup entries + backup header, with one block of margin */

/* "starbite,allowed-ranges": header + records */
#define SBUFS_ALLOW_MAGIC       0x52464253u     /* "SBFR" little endian */
#define SBUFS_DENY_MAGIC        0x44464253u     /* "SBFD" */
#define SBUFS_FENCE_VERSION     1u
#define SBUFS_ALLOW_REC_SIZE    64u
#define SBUFS_DENY_REC_SIZE     40u
#define SBUFS_FENCE_HDR_SIZE    16u

/* allowed range flags */
#define SBUFS_RANGE_WRITABLE    0x1u    /* writes allowed here once the global switch is on */
#define SBUFS_RANGE_ROOT        0x2u    /* the loader's root candidate (informational: logged, published) */
#define SBUFS_RANGE_KNOWN_FLAGS (SBUFS_RANGE_WRITABLE | SBUFS_RANGE_ROOT)

struct sbufs_range {
	uint32_t lun;
	uint32_t flags;
	uint64_t first;                 /* absolute LBA, 4 KiB blocks */
	uint64_t count;
	uint8_t uuid[16];
	char name[17];
	bool usable;                    /* survived validation (capacity known, floors, deny-list, overlaps) */
	const char *why;                /* why not usable */
};

struct sbufs_extent {
	uint32_t lun;
	uint64_t first, count;
	char name[17];
};

/* verdicts */
enum {
	SBUFS_FENCE_OK = 0,
	SBUFS_FENCE_EMPTY,              /* zero blocks */
	SBUFS_FENCE_WRAP,               /* lba + n overflows */
	SBUFS_FENCE_BAD_LUN,            /* well-known or >= 8 */
	SBUFS_FENCE_OUTSIDE,            /* not wholly inside one usable allowed range */
	SBUFS_FENCE_DENIED,             /* meets a denied extent */
	SBUFS_FENCE_FLOOR,              /* meets the LU's head or tail floor, or past its capacity */
	SBUFS_FENCE_READONLY,           /* a write where writes are not allowed */
	SBUFS_FENCE_BAD_RANGE,          /* no such (usable) range index */
};

struct sbufs_fence {
	struct sbufs_range allow[SBUFS_MAX_RANGES];
	unsigned nallow;
	struct sbufs_extent deny[SBUFS_MAX_DENY];
	unsigned ndeny;
	uint64_t lu_blocks[SBUFS_MAX_GENERAL_LU];       /* READ CAPACITY, 0 = unknown: nothing on that LU is usable */
	bool writes_enabled;                            /* global switch (ADT property AND boot-arg) */
	bool sealed;                                    /* sbufs_fence_seal() ran: ranges fixed */
	/* the fence log */
	uint64_t checked, refused, refused_write, blocks_ok;
	uint64_t foreign_touched;                       /* commands that reached a doorbell outside the fence: must stay 0 */
};

/* Parse the two ADT properties. Fail closed: any malformed byte leaves nallow = 0 (nothing exposed).
 * deny may be NULL (no deny-list). Returns 0 or SBUFS_E_*; err gets a one-line reason. */
int sbufs_fence_parse(struct sbufs_fence *f, const void *allow, size_t allow_len, const void *deny,
    size_t deny_len, char *err, size_t errlen);

/* The capacity of a general LU, from READ CAPACITY (blocks). */
void sbufs_fence_set_capacity(struct sbufs_fence *f, uint32_t lun, uint64_t blocks);

/* After every capacity is known: validate each allowed range (floors, capacity, deny-list, overlaps) and fix the
 * set. Ranges that fail stay listed with usable = false and a reason. Returns the number of usable ranges. */
unsigned sbufs_fence_seal(struct sbufs_fence *f);

/* The core check, absolute addressing. Counts into the fence log. */
int sbufs_fence_check(struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n, bool write);

/* Same, without touching the counters (for the doorbell audit and tests). */
int sbufs_fence_check_quiet(const struct sbufs_fence *f, uint32_t lun, uint64_t lba, uint64_t n, bool write);

/* Range-relative (an IOBlockStorageDevice's block numbers) to absolute; checks everything. */
int sbufs_fence_map(struct sbufs_fence *f, unsigned range, uint64_t block, uint64_t nblks, bool write,
    uint32_t *lun, uint64_t *lba);

bool sbufs_fence_range_writable(const struct sbufs_fence *f, unsigned range);
const char *sbufs_fence_verdict_name(int verdict);

#ifdef __cplusplus
}
#endif

#endif /* SBUFS_FENCE_H */
