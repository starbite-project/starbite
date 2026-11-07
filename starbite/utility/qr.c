/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS QR codes (qr.h), after ISO/IEC 18004: byte mode only, versions 1..10, no ECI.
 *   data:     mode 0100, the length (8 bits; 16 from version 10), the bytes, up to 4 terminator zeros,
 *             zero bits to a byte, then the pad bytes 0xec 0x11 to the version's data capacity.
 *   blocks:   the data split into the version's blocks (the short ones first), Reed-Solomon over GF(256)
 *             (x^8 + x^4 + x^3 + x^2 + 1, generator roots 2^0..2^(n-1)) per block, then the data bytes and
 *             the EC bytes interleaved column by column.
 *   symbol:   finders, separators, timing, alignment, the dark module, format and version areas reserved,
 *             codewords placed in the two-column zigzag from the bottom right, each of the 8 masks scored
 *             and the cheapest kept (ties: the lowest mask number). */
#include "qr.h"

/* EC codewords per block and the number of blocks, [ecc L M Q H][version 1..10] */
static const uint8_t ec_len[4][QR_MAX_VER] = {
    { 7, 10, 15, 20, 26, 18, 20, 24, 30, 18 },
    { 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 },
    { 13, 22, 18, 26, 18, 24, 18, 22, 20, 24 },
    { 17, 28, 22, 16, 22, 28, 26, 26, 24, 28 },
};
static const uint8_t ec_blocks[4][QR_MAX_VER] = {
    { 1, 1, 1, 1, 1, 2, 2, 2, 2, 4 },
    { 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 },
    { 1, 1, 2, 2, 4, 4, 6, 6, 8, 8 },
    { 1, 1, 2, 4, 4, 4, 5, 6, 8, 8 },
};
/* all codewords of a version (data + EC), 1..10 */
static const uint16_t total_cw[QR_MAX_VER] = { 26, 44, 70, 100, 134, 172, 196, 242, 292, 346 };
/* the alignment centres per axis (0 ends the list); version 1 has none */
static const uint8_t align_pos[QR_MAX_VER][4] = {
    { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 },
    { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
};
/* the 2-bit ECC indicator in the format bits: L 01, M 00, Q 11, H 10 */
static const uint8_t ecc_bits[4] = { 1, 0, 3, 2 };

#define MAX_CW 346                  /* total_cw[QR_MAX_VER - 1] */
#define MAX_EC 30

static unsigned data_cw(int ver, int ecc)
{
    return total_cw[ver - 1] - (unsigned)ec_len[ecc][ver - 1] * ec_blocks[ecc][ver - 1];
}

/* bytes that fit in byte mode: the header is 4 mode bits and an 8- or 16-bit count */
static unsigned fits(int ver, int ecc)
{
    return (data_cw(ver, ecc) * 8 - 4 - (ver < 10 ? 8 : 16)) / 8;
}

unsigned qr_capacity(int ecc)
{
    return ecc < QR_ECC_L || ecc > QR_ECC_H ? 0 : fits(QR_MAX_VER, ecc);
}

/* ---- Reed-Solomon over GF(256), polynomial 0x11d ---- */

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    unsigned r = 0, x = a;
    while (b) {                     /* shift-and-add, reducing x whenever it overflows 8 bits */
        if (b & 1)
            r ^= x;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11d;
        b >>= 1;
    }
    return (uint8_t)r;
}

/* the generator (x - 2^0)(x - 2^1)..(x - 2^(n-1)) as g[0..n], the coefficients of x^n..x^0 (g[0] = 1) */
static void rs_generator(int n, uint8_t *g)
{
    uint8_t root = 1;
    g[0] = 1;
    for (int k = 0; k < n; k++) {   /* times (x + root), minus being plus in GF(2^8): degree k -> k + 1 */
        g[k + 1] = 0;
        for (int i = k + 1; i >= 1; i--)
            g[i] ^= gf_mul(g[i - 1], root);
        root = gf_mul(root, 2);
    }
}

/* the n EC bytes of d[0..len-1]: the remainder of d(x) * x^n divided by g(x) */
static void rs_remainder(const uint8_t *d, int len, const uint8_t *g, int n, uint8_t *ec)
{
    for (int i = 0; i < n; i++)
        ec[i] = 0;
    for (int k = 0; k < len; k++) {
        uint8_t f = d[k] ^ ec[0];   /* the leading term, cancelled by f * g(x) */
        for (int i = 0; i < n; i++)
            ec[i] = (i + 1 < n ? ec[i + 1] : 0) ^ gf_mul(g[i + 1], f);
    }
}

/* ---- the data bit stream ---- */

struct bits {
    uint8_t *b;
    unsigned n;                     /* bits written */
};

static void put_bits(struct bits *s, unsigned v, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        if ((v >> i) & 1)
            s->b[s->n >> 3] |= (uint8_t)(0x80 >> (s->n & 7));
        s->n++;
    }
}

/* data codewords of the message into cw[0..data_cw-1] (the whole buffer zeroed first) */
static void make_data(const uint8_t *data, unsigned len, int ver, int ecc, uint8_t *cw)
{
    unsigned cap = data_cw(ver, ecc);
    for (unsigned i = 0; i < MAX_CW; i++)
        cw[i] = 0;
    struct bits s = { cw, 0 };
    put_bits(&s, 4, 4);
    put_bits(&s, len, ver < 10 ? 8 : 16);
    for (unsigned i = 0; i < len; i++)
        put_bits(&s, data[i], 8);
    unsigned room = cap * 8 - s.n;
    put_bits(&s, 0, room < 4 ? (int)room : 4);
    s.n = (s.n + 7) & ~7u;
    for (int pad = 0xec; s.n < cap * 8; pad ^= 0xec ^ 0x11)
        put_bits(&s, (unsigned)pad, 8);
}

/* split into blocks, add each block's EC, interleave: out[0..total_cw-1] */
static void make_codewords(const uint8_t *dcw, int ver, int ecc, uint8_t *out)
{
    int nb = ec_blocks[ecc][ver - 1], ne = ec_len[ecc][ver - 1], total = total_cw[ver - 1];
    int nlong = total % nb, nshort = nb - nlong;
    int slen = total / nb - ne;     /* data bytes of a short block; the long ones have one more */
    uint8_t g[MAX_EC + 1], ec[8][MAX_EC];
    rs_generator(ne, g);
    for (int b = 0, at = 0; b < nb; b++) {
        int dl = slen + (b >= nshort);
        rs_remainder(dcw + at, dl, g, ne, ec[b]);
        at += dl;
    }
    int o = 0;
    for (int i = 0; i <= slen; i++)
        for (int b = 0; b < nb; b++) {
            if (i == slen && b < nshort)
                continue;           /* the short blocks have no byte in the last column */
            out[o++] = dcw[b * slen + (b > nshort ? b - nshort : 0) + i];
        }
    for (int i = 0; i < ne; i++)
        for (int b = 0; b < nb; b++)
            out[o++] = ec[b][i];
}

/* ---- the symbol ---- */

/* fn[y][x] marks the function modules, which the data and the masks skip */
typedef uint8_t qr_map[QR_MAX_SIZE][QR_MAX_SIZE];

static void set_fn(struct qr *q, qr_map fn, int x, int y, int dark)
{
    q->m[y][x] = (uint8_t)(dark != 0);
    fn[y][x] = 1;
}

/* a finder centred at (cx, cy) with its light separator ring, clipped at the edges */
static void finder(struct qr *q, qr_map fn, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= q->size || y >= q->size)
                continue;
            int r = dx < 0 ? -dx : dx, ry = dy < 0 ? -dy : dy;
            if (ry > r)
                r = ry;             /* Chebyshev distance: rings 0-1 dark, 2 light, 3 dark, 4 light */
            set_fn(q, fn, x, y, r != 2 && r != 4);
        }
}

/* the 15 format bits: ECC indicator and mask, BCH(15,5) with 0x537, XOR 0x5412 */
static unsigned format_bits(int ecc, int mask)
{
    unsigned d = (unsigned)(ecc_bits[ecc] << 3 | mask), r = d;
    for (int i = 0; i < 10; i++)
        r = (r << 1) ^ ((r >> 9) * 0x537);
    return (d << 10 | (r & 0x3ff)) ^ 0x5412;
}

/* both copies of the format bits (bit 0 = least significant) */
static void draw_format(struct qr *q, qr_map fn, int ecc, int mask)
{
    unsigned f = format_bits(ecc, mask);
    int n = q->size;
    for (int i = 0; i <= 5; i++)
        set_fn(q, fn, 8, i, (f >> i) & 1);
    set_fn(q, fn, 8, 7, (f >> 6) & 1);
    set_fn(q, fn, 8, 8, (f >> 7) & 1);
    set_fn(q, fn, 7, 8, (f >> 8) & 1);
    for (int i = 9; i < 15; i++)
        set_fn(q, fn, 14 - i, 8, (f >> i) & 1);
    for (int i = 0; i < 8; i++)
        set_fn(q, fn, n - 1 - i, 8, (f >> i) & 1);
    for (int i = 8; i < 15; i++)
        set_fn(q, fn, 8, n - 15 + i, (f >> i) & 1);
    set_fn(q, fn, 8, n - 8, 1);     /* the dark module */
}

/* finders, timing, alignment, version info (7+), the format area */
static void draw_function(struct qr *q, qr_map fn)
{
    int n = q->size, v = q->ver;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            q->m[y][x] = 0;
            fn[y][x] = 0;
        }
    for (int i = 0; i < n; i++) {
        set_fn(q, fn, 6, i, i % 2 == 0);
        set_fn(q, fn, i, 6, i % 2 == 0);
    }
    finder(q, fn, 3, 3);
    finder(q, fn, n - 4, 3);
    finder(q, fn, 3, n - 4);
    const uint8_t *ap = align_pos[v - 1];
    for (int i = 0; i < 4 && ap[i]; i++)
        for (int j = 0; j < 4 && ap[j]; j++) {
            int cx = ap[i], cy = ap[j];
            if ((cx == 6 && cy == 6) || (cx == 6 && cy == n - 7) || (cx == n - 7 && cy == 6))
                continue;           /* those three would sit on the finders */
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int r = dx < 0 ? -dx : dx, ry = dy < 0 ? -dy : dy;
                    set_fn(q, fn, cx + dx, cy + dy, (r > ry ? r : ry) != 1);
                }
        }
    if (v >= 7) {                   /* 6 version bits, BCH(18,6) with 0x1f25, in two 6x3 blocks */
        unsigned r = (unsigned)v;
        for (int i = 0; i < 12; i++)
            r = (r << 1) ^ ((r >> 11) * 0x1f25);
        unsigned bits = (unsigned)v << 12 | (r & 0xfff);
        for (int i = 0; i < 18; i++) {
            int a = n - 11 + i % 3, b = i / 3, bit = (bits >> i) & 1;
            set_fn(q, fn, a, b, bit);
            set_fn(q, fn, b, a, bit);
        }
    }
    draw_format(q, fn, 0, 0);       /* reserve; the real bits go in with the mask */
}

/* the codewords into the free modules: column pairs from the right, up then down, skipping column 6 */
static void place(struct qr *q, qr_map fn, const uint8_t *cw, int ncw)
{
    int n = q->size, i = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        int up = ((right + 1) & 2) == 0;
        for (int k = 0; k < n; k++) {
            int y = up ? n - 1 - k : k;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (fn[y][x])
                    continue;
                if (i < ncw * 8)
                    q->m[y][x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
                i++;                /* the remainder bits (0, 3, 4 or 7) stay light */
            }
        }
    }
}

static int mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* XOR the mask over the data modules (applying it twice undoes it) */
static void apply_mask(struct qr *q, qr_map fn, int mask)
{
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++)
            if (!fn[y][x] && mask_bit(mask, x, y))
                q->m[y][x] ^= 1;
}

/* module i of row (or column, col = 1) k; outside the symbol is the light quiet zone */
static int at(const struct qr *q, int col, int k, int i)
{
    if (i < 0 || i >= q->size)
        return 0;
    return col ? q->m[i][k] : q->m[k][i];
}

/* N1 (runs of 5+: 3 + extra) and N3 (1:1:3:1:1 with 4 light on a side: 40 per side) of one line */
static long line_penalty(const struct qr *q, int col, int k)
{
    static const uint8_t core[7] = { 1, 0, 1, 1, 1, 0, 1 };
    int n = q->size;
    long p = 0;
    int run = 1;
    for (int i = 1; i <= n; i++) {
        if (i < n && at(q, col, k, i) == at(q, col, k, i - 1)) {
            run++;
            continue;
        }
        if (run >= 5)
            p += 3 + (run - 5);
        run = 1;
    }
    for (int i = 0; i + 7 <= n; i++) {
        int ok = 1;
        for (int j = 0; j < 7 && ok; j++)
            ok = at(q, col, k, i + j) == core[j];
        if (!ok)
            continue;
        int before = 1, after = 1;
        for (int j = 1; j <= 4; j++) {
            before &= !at(q, col, k, i - j);
            after &= !at(q, col, k, i + 6 + j);
        }
        p += 40 * (before + after);
    }
    return p;
}

static long penalty(const struct qr *q)
{
    int n = q->size;
    long p = 0, dark = 0;
    for (int k = 0; k < n; k++)
        p += line_penalty(q, 0, k) + line_penalty(q, 1, k);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int c = q->m[y][x];
            dark += c;
            if (x + 1 < n && y + 1 < n && c == q->m[y][x + 1] && c == q->m[y + 1][x] && c == q->m[y + 1][x + 1])
                p += 3;             /* N2: each 2x2 block of one colour */
        }
    long total = (long)n * n, dev = dark * 20 - total * 10;
    if (dev < 0)
        dev = -dev;
    p += 10 * ((dev + total - 1) / total - 1);  /* N4: 10 per full 5% off a half of dark */
    return p;
}

int qr_encode(const uint8_t *data, unsigned len, int ecc, struct qr *out)
{
    if (ecc < QR_ECC_L || ecc > QR_ECC_H)
        return 0;
    int ver = 1;
    while (ver <= QR_MAX_VER && fits(ver, ecc) < len)
        ver++;
    if (ver > QR_MAX_VER)
        return 0;

    uint8_t dcw[MAX_CW], cw[MAX_CW];
    qr_map fn;
    make_data(data, len, ver, ecc, dcw);
    make_codewords(dcw, ver, ecc, cw);

    out->ver = ver;
    out->ecc = ecc;
    out->size = 17 + 4 * ver;
    draw_function(out, fn);
    place(out, fn, cw, total_cw[ver - 1]);

    int best = 0;
    long best_p = -1;
    for (int mask = 0; mask < 8; mask++) {
        apply_mask(out, fn, mask);
        draw_format(out, fn, ecc, mask);
        long p = penalty(out);
        if (best_p < 0 || p < best_p) {
            best_p = p;
            best = mask;
        }
        apply_mask(out, fn, mask);
    }
    apply_mask(out, fn, best);
    draw_format(out, fn, ecc, best);
    out->mask = best;
    return 1;
}
