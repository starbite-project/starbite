/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's passcode (Track A): the salted, iterated SHA-256, the back-off and the slot record. See passcode.h.
 * Pure C without libc: the kernel (freestanding), the host shell and the host test build this file as it is. */
#include "passcode.h"

/* ---- SHA-256 (FIPS 180-4), small and plain ------------------------------------------------------------------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void pass_sha256(const void *data, uint32_t n, uint8_t out[32])
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint32_t h[8];
    uint8_t blk[64];
    const uint8_t *p = data;
    for (int i = 0; i < 8; i++) h[i] = H0[i];
    uint32_t left = n;
    for (; left >= 64; left -= 64, p += 64) sha_block(h, p);
    uint32_t i = 0;
    for (; i < left; i++) blk[i] = p[i];
    blk[i++] = 0x80;
    if (i > 56) {
        for (; i < 64; i++) blk[i] = 0;
        sha_block(h, blk);
        i = 0;
    }
    for (; i < 56; i++) blk[i] = 0;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) blk[56 + k] = (uint8_t)(bits >> (56 - 8 * k));
    sha_block(h, blk);
    for (int k = 0; k < 8; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24); out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8); out[4 * k + 3] = (uint8_t)h[k];
    }
    for (int k = 0; k < 64; k++) ((volatile uint8_t *)blk)[k] = 0;
}

/* ---- the state ------------------------------------------------------------------------------------------------ */
static struct {
    int len;                        /* 0, 4, 6 */
    uint32_t rounds;
    uint8_t salt[PASS_SALT], hash[32];
    int fails;
    uint64_t end_ms;                /* the wait ends at this now_ms (0: no wait) */
} P;

static void wipe(void *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((volatile uint8_t *)p)[i] = 0; }
static void copy(void *d, const void *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t *)d)[i] = ((const uint8_t *)s)[i]; }

void pass_reset(void) { wipe(&P, sizeof P); }
int pass_len(void) { return P.len; }
int pass_fails(void) { return P.fails; }

uint32_t pass_backoff_s(int fails)
{
    if (fails < 6) return 0;
    if (fails == 6) return 60;
    if (fails == 7) return 5 * 60;
    if (fails == 8) return 15 * 60;
    return 60 * 60;
}

uint32_t pass_wait(uint64_t now_ms)
{
    if (!P.end_ms || now_ms >= P.end_ms) return 0;
    return (uint32_t)((P.end_ms - now_ms + 999) / 1000);
}

/* digits: 4 or 6 of '0'..'9' and nothing else; their count, or 0 */
static int digits_ok(const char *d)
{
    int n = 0;
    if (!d) return 0;
    while (d[n]) {
        if (d[n] < '0' || d[n] > '9' || n >= 6) return 0;
        n++;
    }
    return n == 4 || n == 6 ? n : 0;
}

/* H(1) = SHA-256("sbos.pass/1" | salt | len | digits); H(i + 1) = SHA-256(H(i) | salt), rounds in all */
static void derive(const char *d, int n, const uint8_t salt[PASS_SALT], uint32_t rounds, uint8_t out[32])
{
    static const char tag[11] = { 's', 'b', 'o', 's', '.', 'p', 'a', 's', 's', '/', '1' };
    uint8_t b[11 + PASS_SALT + 1 + 6];
    uint32_t k = 0;
    copy(b, tag, 11); k = 11;
    copy(b + k, salt, PASS_SALT); k += PASS_SALT;
    b[k++] = (uint8_t)n;
    copy(b + k, d, (uint32_t)n); k += (uint32_t)n;
    pass_sha256(b, k, out);
    wipe(b, sizeof b);
    uint8_t c[32 + PASS_SALT];
    copy(c + 32, salt, PASS_SALT);
    for (uint32_t r = 1; r < rounds; r++) {
        copy(c, out, 32);
        pass_sha256(c, sizeof c, out);
    }
    wipe(c, sizeof c);
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t x = 0;
    for (uint32_t i = 0; i < n; i++) x |= a[i] ^ b[i];
    return x == 0;
}

static void wrong(uint64_t now_ms)
{
    P.fails++;
    uint32_t w = pass_backoff_s(P.fails);
    P.end_ms = w ? now_ms + (uint64_t)w * 1000 : 0;
}

int pass_check(const char *digits, uint64_t now_ms, int64_t utc)
{
    (void)utc;
    if (!P.len) return 1;
    uint32_t w = pass_wait(now_ms);
    if (w) return -(int)w;
    int n = digits_ok(digits);
    uint8_t h[32];
    int ok = 0;
    if (n == P.len) {
        derive(digits, n, P.salt, P.rounds, h);
        ok = same(h, P.hash, 32);
        wipe(h, sizeof h);
    }
    if (!ok) {
        wrong(now_ms);
        return 0;
    }
    P.fails = 0;
    P.end_ms = 0;
    return 1;
}

int pass_set(const char *old, const char *nw, const uint8_t salt[PASS_SALT], uint64_t now_ms, int64_t utc)
{
    int n = nw && nw[0] ? digits_ok(nw) : 0;
    if (nw && nw[0] && !n) return -1;               /* a new code of 4 or 6 digits, or "" to remove */
    if (P.len && pass_check(old, now_ms, utc) != 1) return -1;
    if (!n) {
        pass_reset();
        return 0;
    }
    P.len = n;
    P.rounds = PASS_ROUNDS;
    copy(P.salt, salt, PASS_SALT);
    derive(nw, n, P.salt, P.rounds, P.hash);
    P.fails = 0;
    P.end_ms = 0;
    return 0;
}

/* ---- the record: "SBP1", version 1, len, 2 zero, rounds, fails, the wait left (s), its end (UTC s; 0 unknown),
 * the salt, the hash: 76 bytes, little-endian ----------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int pass_pack(uint8_t rec[PASS_REC_LEN], uint64_t now_ms, int64_t utc)
{
    uint32_t w = pass_wait(now_ms);
    if (!P.len && !P.fails) return 0;
    wipe(rec, PASS_REC_LEN);
    rec[0] = 'S'; rec[1] = 'B'; rec[2] = 'P'; rec[3] = '1';
    rec[4] = 1;
    rec[5] = (uint8_t)P.len;
    put32(rec + 8, P.rounds);
    put32(rec + 12, (uint32_t)P.fails);
    put32(rec + 16, w);
    int64_t end = w && utc > 0 ? utc + (int64_t)w : 0;
    put32(rec + 20, (uint32_t)(uint64_t)end);
    put32(rec + 24, (uint32_t)((uint64_t)end >> 32));
    copy(rec + 28, P.salt, PASS_SALT);
    copy(rec + 44, P.hash, 32);
    return (int)PASS_REC_LEN;
}

int pass_load(const uint8_t *rec, int n, uint64_t now_ms, int64_t utc)
{
    if (!rec || n < (int)PASS_REC_LEN || rec[0] != 'S' || rec[1] != 'B' || rec[2] != 'P' || rec[3] != '1' || rec[4] != 1)
        return 0;
    int len = rec[5];
    uint32_t rounds = get32(rec + 8), fails = get32(rec + 12), w = get32(rec + 16);
    int64_t end = (int64_t)((uint64_t)get32(rec + 20) | (uint64_t)get32(rec + 24) << 32);
    if ((len != 0 && len != 4 && len != 6) || (len && (rounds < 1000 || rounds > 1000000)) || fails > 100000 || w > 2U * 3600U)
        return 0;
    pass_reset();
    P.len = len;
    P.rounds = len ? rounds : 0;
    if (len) {
        copy(P.salt, rec + 28, PASS_SALT);
        copy(P.hash, rec + 44, 32);
    }
    P.fails = (int)fails;
    /* the wait left: from the wall clock when both ends know it (never longer than it was), else all of it again */
    uint32_t left = w;
    if (w && end > 0 && utc > 0) left = end <= utc ? 0 : end - utc < (int64_t)w ? (uint32_t)(end - utc) : w;
    P.end_ms = left ? now_ms + (uint64_t)left * 1000 : 0;
    return 1;
}
