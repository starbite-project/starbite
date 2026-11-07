/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS UO4 (part): SHA-256 and CRC32 of a UFS partition (GPT of LU0) or the microSD card, or of
 * their first N bytes (to compare with an image written into a larger partition). Read only. One job at a
 * time, advanced 1 MiB per hash_step() from the service loop, so the UI, USB and the shell keep running.
 * SHA-256: FIPS 180-4. CRC32: the zlib polynomial (0xEDB88320, reflected), as `crc32` and Python's zlib. */
#include "sb.h"
#include "hash.h"
#include "ufs.h"
#include "sd.h"
#include "exfat.h"

#define STEP_BLOCKS 256U                               /* 1 MiB per step (4 KiB blocks) */

struct hash_job hash_job;
static uint8_t *buf;

/* ---- the job ------------------------------------------------------------------------------------ */
int hash_start(int medium, const char *name, uint64_t lba0, uint64_t bytes)
{
    if (hash_job.state == HASH_RUNNING)
        return -1;
    if (!buf)
        buf = sb_alloc(STEP_BLOCKS * 4096U);
    if (!buf)
        return -2;
    sb_strcpy(hash_job.name, sizeof(hash_job.name), name);
    hash_job.medium = medium;
    hash_job.lba = lba0;
    hash_job.bytes = bytes;
    hash_job.done = 0;
    hash_job.crc = 0;
    hash_job.t0 = sb_ms();
    hash_job.err = 0;
    sha256_init(&hash_job.sha);
    hash_job.state = HASH_RUNNING;
    return 0;
}

int hash_start_part(const char *name, uint64_t bytes)
{
    uint64_t first, last;
    if (!ufs_up() || gpt_load() <= 0)
        return -3;
    if (!gpt_find(name, &first, &last))
        return -4;
    uint64_t size = (last - first + 1) * 4096ULL;
    if (!bytes || bytes > size)
        bytes = size;
    return hash_start(HASH_UFS, name, first, bytes);
}

int hash_start_card(uint64_t bytes)
{
    uint64_t size = sd_sectors_hint() * 512ULL;
    if (!sd_up() || !size)
        return -5;
    if (!bytes || bytes > size)
        bytes = size;
    return hash_start(HASH_SD, "microSD", 0, bytes);
}

int hash_start_file(struct exfat *fs, const struct exfat_ent *e)
{
    int r = hash_start(HASH_FILE, e->name, 0, e->size);
    if (r)
        return r;
    hash_job.fs = fs;
    hash_job.first = e->first;
    hash_job.nofat = e->nofat;
    if (!e->size) {                                      /* empty file: done at once */
        sha256_final(&hash_job.sha, hash_job.digest);
        hash_job.t1 = sb_ms();
        hash_job.state = HASH_DONE;
    }
    return 0;
}

void hash_step(void)
{
    struct hash_job *j = &hash_job;
    if (j->state != HASH_RUNNING)
        return;
    uint64_t left = j->bytes - j->done;
    uint64_t want = left < STEP_BLOCKS * 4096ULL ? left : STEP_BLOCKS * 4096ULL;
    uint64_t blocks = (want + 4095) / 4096;
    int r = j->medium == HASH_UFS ? ufs_read_to(j->lba, blocks, (uintptr_t)buf)
          : j->medium == HASH_SD  ? sd_read4k(j->lba, blocks, (uintptr_t)buf)
                                  : exfat_read_chain(j->fs, j->first, j->nofat, j->done, buf, want);
    if (r) {
        j->err = r;
        j->state = HASH_FAILED;
        return;
    }
    sha256_update(&j->sha, buf, want);
    j->crc = crc32_update(j->crc, buf, want);
    j->lba += blocks;
    j->done += want;
    if (j->done >= j->bytes) {
        sha256_final(&j->sha, j->digest);
        j->t1 = sb_ms();
        j->state = HASH_DONE;
    }
}

void hash_cancel(void)
{
    if (hash_job.state == HASH_RUNNING)
        hash_job.state = HASH_CANCELLED;
}

void hash_hex(char out[65])
{
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = "0123456789abcdef"[hash_job.digest[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[hash_job.digest[i] & 15];
    }
    out[64] = 0;
}
