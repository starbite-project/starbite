/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS hashing: hash.c (SHA-256 + CRC32 of a UFS partition or the microSD card, read only). */
#pragma once
#include <stdint.h>

struct sha256 { uint32_t h[8]; uint64_t len; uint8_t b[64]; unsigned n; };
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const uint8_t *p, uint64_t n);
void sha256_final(struct sha256 *s, uint8_t out[32]);
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint64_t n);

enum { HASH_IDLE, HASH_RUNNING, HASH_DONE, HASH_FAILED, HASH_CANCELLED };
enum { HASH_UFS, HASH_SD };
struct hash_job {
    int state, medium, err;
    char name[24];
    uint64_t lba, bytes, done, t0, t1;
    uint32_t crc;
    struct sha256 sha;
    uint8_t digest[32];
};
extern struct hash_job hash_job;

int hash_start_part(const char *name, uint64_t bytes);   /* bytes 0: the whole partition; 0 = started */
int hash_start_card(uint64_t bytes);
int hash_start(int medium, const char *name, uint64_t lba0, uint64_t bytes);
void hash_step(void);                                     /* 1 MiB; from the service loop */
void hash_cancel(void);
void hash_hex(char out[65]);
