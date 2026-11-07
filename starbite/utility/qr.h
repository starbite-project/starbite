/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS QR codes (qr.c): byte-mode QR symbols of versions 1..10 (up to 271 bytes at ECC L) for showing
 * a link, a Wi-Fi login or a hash on the screen. Freestanding: no malloc, no floats, no libc. */
#pragma once
#include <stdint.h>

#define QR_MAX_VER 10
#define QR_MAX_SIZE (17 + 4 * QR_MAX_VER)   /* 57 */

struct qr {
    int size;                               /* modules per side, 17 + 4 * ver */
    uint8_t m[QR_MAX_SIZE][QR_MAX_SIZE];    /* m[y][x] = 1 for dark; no quiet zone */
    int ver, ecc, mask;
};

enum { QR_ECC_L, QR_ECC_M, QR_ECC_Q, QR_ECC_H };

/* Byte-mode encode of len bytes at the lowest version (1..QR_MAX_VER) that fits at ecc, with the mask of the
 * lowest penalty (ISO/IEC 18004 rules N1..N4). 1 ok, 0 too long (or ecc out of range). */
int qr_encode(const uint8_t *data, unsigned len, int ecc, struct qr *out);

/* the most bytes qr_encode takes at ecc (version QR_MAX_VER) */
unsigned qr_capacity(int ecc);
