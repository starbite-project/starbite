/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: SAE for group 19 (NIST P-256) on the host: IEEE 802.11-2020 12.4 (Dragonfly), sae.h says what is here.
 *
 * Arithmetic: BearSSL (tls/bearssl, MIT): br_ec_all_m31 for the curve (its P-256 code: constant time; it checks
 * that points are on the curve), br_i31 modular integers modulo p for the field (the square root, the Legendre
 * symbol and SSWU's inversion by modular exponentiation: constant time in the value), SHA-256 and HMAC. Scalars
 * modulo r are added and compared byte-wise without branches on their values.
 *
 * Written from the standard; the layout of each step follows hostap's src/common/sae.c and dragonfly.c (BSD,
 * Jouni Malinen) so the two can be compared side by side, and host/sae-test.c checks this file against the
 * standard's example (J.10, the vectors hostap's common_module_tests.c carries) and against hostap itself.
 */
#include "sae.h"
#include "bcmw_priv.h"
#include "bearssl.h"

/* BearSSL's modular integers and constant-time copy (declared in its src/inner.h, which is not a public header) */
uint32_t br_i31_add(uint32_t *a, const uint32_t *b, uint32_t ctl);
uint32_t br_i31_sub(uint32_t *a, const uint32_t *b, uint32_t ctl);
uint32_t br_i31_iszero(const uint32_t *x);
void br_i31_decode(uint32_t *x, const void *src, size_t len);
uint32_t br_i31_decode_mod(uint32_t *x, const void *src, size_t len, const uint32_t *m);
void br_i31_decode_reduce(uint32_t *x, const void *src, size_t len, const uint32_t *m);
void br_i31_encode(void *dst, size_t len, const uint32_t *x);
uint32_t br_i31_ninv31(uint32_t x);
void br_i31_montymul(uint32_t *d, const uint32_t *x, const uint32_t *y, const uint32_t *m, uint32_t m0i);
void br_i31_to_monty(uint32_t *x, const uint32_t *m);
void br_i31_modpow(uint32_t *x, const unsigned char *e, size_t elen, const uint32_t *m, uint32_t m0i, uint32_t *t1,
                   uint32_t *t2);
void br_ccopy(uint32_t ctl, void *dst, const void *src, size_t len);

#define CURVE BR_EC_secp256r1
#define EC (&br_ec_all_m31)

/* ---- constants ------------------------------------------------------------------------------------------- */

static const uint8_t P_BYTES[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t B_BYTES[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t R_BYTES[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

/* ---- bytes, big-endian, without branches on their values -------------------------------------------------- */

static uint32_t be_sub(uint8_t *d, const uint8_t *a, const uint8_t *b, unsigned n)     /* d = a - b; the borrow */
{
    uint32_t borrow = 0;
    for (unsigned i = n; i-- > 0;) {
        uint32_t t = (uint32_t)a[i] - b[i] - borrow;
        d[i] = (uint8_t)t;
        borrow = (t >> 8) & 1;
    }
    return borrow;
}

static uint32_t be_add(uint8_t *d, const uint8_t *a, const uint8_t *b, unsigned n)     /* d = a + b; the carry */
{
    uint32_t carry = 0;
    for (unsigned i = n; i-- > 0;) {
        uint32_t t = (uint32_t)a[i] + b[i] + carry;
        d[i] = (uint8_t)t;
        carry = t >> 8;
    }
    return carry;
}

static uint32_t be_lt(const uint8_t *a, const uint8_t *b, unsigned n)                  /* 1 if a < b */
{
    uint8_t t[64];
    uint32_t r = be_sub(t, a, b, n);
    bcmw_memset(t, 0, n);
    return r;
}

static uint32_t be_gt1(const uint8_t *a, unsigned n)                                   /* 1 if a > 1 */
{
    uint32_t hi = 0;
    for (unsigned i = 0; i + 1 < n; i++)
        hi |= a[i];
    hi |= a[n - 1] & 0xfe;
    return (hi | (0u - hi)) >> 31;
}

static uint32_t ct_eq(const uint8_t *a, const uint8_t *b, unsigned n)                  /* 1 if equal */
{
    uint32_t d = 0;
    for (unsigned i = 0; i < n; i++)
        d |= (uint32_t)(a[i] ^ b[i]);
    return ((d | (0u - d)) >> 31) ^ 1;
}

/* (a + b) mod r for a, b < r */
static void scalar_add(uint8_t d[32], const uint8_t a[32], const uint8_t b[32])
{
    uint8_t s[32], t[32];
    uint32_t carry = be_add(s, a, b, 32), borrow = be_sub(t, s, R_BYTES, 32);
    br_ccopy(carry | (borrow ^ 1), s, t, 32);       /* s >= r (counting the carry): s - r */
    bcmw_memcpy(d, s, 32);
    bcmw_memset(s, 0, 32);
    bcmw_memset(t, 0, 32);
}

/* ---- the field modulo p (br_i31: 9 words of 31 bits and the bit-length word) ------------------------------ */

typedef uint32_t fe[10];

static struct {
    int ready;
    fe p, a, b, z, one;
    uint32_t m0i;
    uint8_t e_legendre[32], e_sqrt[32], e_inv[32];      /* (p - 1) / 2, (p + 1) / 4, p - 2 */
} F;

static void fe_set(fe x, const uint8_t b[32]) { br_i31_decode_mod(x, b, 32, F.p); }
static void fe_get(uint8_t b[32], const fe x) { br_i31_encode(b, 32, x); }
static void fe_copy(fe d, const fe s) { bcmw_memcpy(d, s, sizeof(fe)); }

static void field(void)
{
    uint8_t t[32], small[32];
    if (F.ready)
        return;
    br_i31_decode(F.p, P_BYTES, 32);
    F.m0i = br_i31_ninv31(F.p[1]);
    bcmw_memset(small, 0, 32);
    small[31] = 3;
    be_sub(t, P_BYTES, small, 32);
    fe_set(F.a, t);                     /* a = -3 */
    fe_set(F.b, B_BYTES);
    small[31] = 10;
    be_sub(t, P_BYTES, small, 32);
    fe_set(F.z, t);                     /* SSWU's Z for group 19: -10 */
    small[31] = 1;
    fe_set(F.one, small);
    be_sub(t, P_BYTES, small, 32);      /* p - 1, halved */
    for (int i = 31; i >= 0; i--)
        F.e_legendre[i] = (uint8_t)(t[i] >> 1 | (i ? t[i - 1] << 7 : 0));
    be_add(t, P_BYTES, small, 32);      /* p + 1 (no carry: p's last byte is ff, but the sum fits 256 bits) */
    for (int i = 31; i >= 0; i--)
        F.e_sqrt[i] = (uint8_t)(t[i] >> 2 | (i ? t[i - 1] << 6 : 0));
    small[31] = 2;
    be_sub(F.e_inv, P_BYTES, small, 32);
    F.ready = 1;
}

static void fe_mul(fe d, const fe a, const fe b)
{
    fe t, r;
    fe_copy(t, a);
    br_i31_to_monty(t, F.p);
    br_i31_montymul(r, t, b, F.p, F.m0i);  /* (a R) b / R */
    fe_copy(d, r);
}

static void fe_add(fe d, const fe a)        /* d += a */
{
    br_i31_add(d, a, 1);
    br_i31_sub(d, F.p, br_i31_sub(d, F.p, 0) ^ 1);
}

static void fe_sub(fe d, const fe a)        /* d -= a */
{
    br_i31_add(d, F.p, br_i31_sub(d, a, 1));
}

static void fe_pow(fe d, const fe a, const uint8_t e[32])
{
    fe t1, t2;
    fe_copy(d, a);
    br_i31_modpow(d, e, 32, F.p, F.m0i, t1, t2);
}

static uint32_t fe_eq(const fe a, const fe b)
{
    uint32_t d = 0;
    for (unsigned i = 1; i < 10; i++)
        d |= a[i] ^ b[i];
    return ((d | (0u - d)) >> 31) ^ 1;
}

/* x^3 + a x + b */
static void fe_curve(fe d, const fe x)
{
    fe t, u;
    fe_mul(t, x, x);
    fe_mul(t, t, x);
    fe_mul(u, F.a, x);
    fe_add(t, u);
    fe_add(t, F.b);
    fe_copy(d, t);
}

/* 1 if (x, y), both < p, is on the curve */
static uint32_t on_curve(const uint8_t xy[64])
{
    fe x, y, l, r;
    if (!be_lt(xy, P_BYTES, 32) || !be_lt(xy + 32, P_BYTES, 32))
        return 0;
    fe_set(x, xy);
    fe_set(y, xy + 32);
    fe_mul(l, y, y);
    fe_curve(r, x);
    return fe_eq(l, r);
}

/* ---- hashes ---------------------------------------------------------------------------------------------- */

static void hmac(const uint8_t *key, unsigned key_len, const uint8_t *const p[], const unsigned l[], unsigned n,
                 uint8_t out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    for (unsigned i = 0; i < n; i++)
        br_hmac_update(&hc, p[i], l[i]);
    br_hmac_out(&hc, out);
    bcmw_memset(&kc, 0, sizeof(kc));
    bcmw_memset(&hc, 0, sizeof(hc));
}

/* KDF-Hash-Length with SHA-256 (12.7.1.7.2): HMAC(K, i || label || context || length), i and length 16-bit LE */
static void kdf(const uint8_t key[32], const char *label, const uint8_t *ctx, unsigned ctx_len, uint8_t *out,
                unsigned bits)
{
    uint8_t i_le[2], len_le[2], block[32];
    const uint8_t *p[4] = { i_le, (const uint8_t *)label, ctx, len_le };
    unsigned l[4] = { 2, bcmw_strlen(label), ctx_len, 2 };
    len_le[0] = (uint8_t)bits, len_le[1] = (uint8_t)(bits >> 8);
    for (unsigned i = 1, done = 0; done < bits / 8; i++, done += 32) {
        i_le[0] = (uint8_t)i, i_le[1] = (uint8_t)(i >> 8);
        hmac(key, 32, p, l, 4, block);
        bcmw_memcpy(out + done, block, bits / 8 - done < 32 ? bits / 8 - done : 32);
    }
    bcmw_memset(block, 0, sizeof(block));
}

/* HKDF-Expand (RFC 5869) with SHA-256 */
static void hkdf_expand(const uint8_t prk[32], const char *info, uint8_t *out, unsigned len)
{
    uint8_t t[32], c;
    unsigned tl = 0;
    for (unsigned done = 0; done < len; done += 32) {
        const uint8_t *p[3] = { t, (const uint8_t *)info, &c };
        unsigned l[3] = { tl, bcmw_strlen(info), 1 };
        c = (uint8_t)(done / 32 + 1);
        hmac(prk, 32, p, l, 3, t);
        tl = 32;
        bcmw_memcpy(out + done, t, len - done < 32 ? len - done : 32);
    }
    bcmw_memset(t, 0, sizeof(t));
}

static void max_min(uint8_t out[12], const uint8_t a[6], const uint8_t b[6])
{
    int a_first = bcmw_memcmp(a, b, 6) > 0;
    bcmw_memcpy(out, a_first ? a : b, 6);
    bcmw_memcpy(out + 6, a_first ? b : a, 6);
}

/* ---- randomness ------------------------------------------------------------------------------------------- */

/* a random value v with lo <= v < limit (lo 1 or 2) */
static int random_below(struct bcmw_sae *s, uint8_t out[32], const uint8_t limit[32], unsigned lo)
{
    for (unsigned tries = 0; tries < 100; tries++) {
        if (s->random(s->random_ctx, out, 32) != 0)
            return -1;
        if (be_lt(out, limit, 32) && (lo < 2 ? be_gt1(out, 32) | (out[31] & 1) : be_gt1(out, 32)))
            return 0;
    }
    return -1;
}

/* ---- the curve, through BearSSL's encoding (04 || x || y) -------------------------------------------------- */

static int ec_mul(uint8_t xy[64], const uint8_t k[32])
{
    uint8_t g[65];
    uint32_t ok;
    g[0] = 4;
    bcmw_memcpy(g + 1, xy, 64);
    ok = EC->mul(g, 65, k, 32, CURVE);
    bcmw_memcpy(xy, g + 1, 64);
    bcmw_memset(g, 0, sizeof(g));
    return ok ? 0 : -1;
}

/* a = x a + y b */
static int ec_muladd(uint8_t a[64], const uint8_t b[64], const uint8_t x[32], const uint8_t y[32])
{
    uint8_t A[65], B[65];
    uint32_t ok;
    A[0] = B[0] = 4;
    bcmw_memcpy(A + 1, a, 64);
    bcmw_memcpy(B + 1, b, 64);
    ok = EC->muladd(A, B, 65, x, 32, y, 32, CURVE);
    bcmw_memcpy(a, A + 1, 64);
    bcmw_memset(A, 0, sizeof(A));
    return ok ? 0 : -1;
}

static void ec_negate(uint8_t xy[64])
{
    be_sub(xy + 32, P_BYTES, xy + 32, 32);
}

/* ---- hunting and pecking (12.4.4.2.2) ----------------------------------------------------------------------- */

/* 1 if v is a quadratic residue, with v blinded by a random r^2 and a known residue or non-residue (r odd: qr,
 * the symbol 1 says yes; r even: qnr, the symbol -1 says yes), as the standard asks; -1 without randomness */
static int qr_blind(struct bcmw_sae *s, const fe v, const fe qr, const fe qnr)
{
    uint8_t rb[32], lb[32], one[32], pm1[32];
    fe r, num, pick, l;
    uint32_t odd, is1, ism1;
    if (random_below(s, rb, P_BYTES, 1))
        return -1;
    fe_set(r, rb);
    fe_mul(num, v, r);
    fe_mul(num, num, r);
    odd = rb[31] & 1;
    fe_copy(pick, qnr);
    br_ccopy(odd, pick, qr, sizeof(fe));
    fe_mul(num, num, pick);
    fe_pow(l, num, F.e_legendre);
    fe_get(lb, l);
    bcmw_memset(one, 0, 32);
    one[31] = 1;
    be_sub(pm1, P_BYTES, one, 32);
    is1 = ct_eq(lb, one, 32);
    ism1 = ct_eq(lb, pm1, 32);
    bcmw_memset(rb, 0, 32);
    return (int)((odd & is1) | ((odd ^ 1) & ism1));
}

static int random_qr_qnr(struct bcmw_sae *s, fe qr, fe qnr)
{
    uint8_t b[32], lb[32], one[32], pm1[32];
    int got_qr = 0, got_qnr = 0;
    fe v, l;
    bcmw_memset(one, 0, 32);
    one[31] = 1;
    be_sub(pm1, P_BYTES, one, 32);
    for (unsigned tries = 0; tries < 200 && !(got_qr && got_qnr); tries++) {
        if (random_below(s, b, P_BYTES, 1))
            return -1;
        fe_set(v, b);
        fe_pow(l, v, F.e_legendre);
        fe_get(lb, l);
        if (!got_qr && ct_eq(lb, one, 32))
            fe_copy(qr, v), got_qr = 1;
        else if (!got_qnr && ct_eq(lb, pm1, 32))
            fe_copy(qnr, v), got_qnr = 1;
    }
    return got_qr && got_qnr ? 0 : -1;
}

static int pwe_hnp(struct bcmw_sae *s, const uint8_t *pw, unsigned pw_len)
{
    uint8_t macs[12], use[128], stub[128], seed[32], value[32], x_saved[32], counter, yb[32], py[32];
    const uint8_t *p[2] = { use, &counter };
    unsigned l[2] = { pw_len, 1 };
    uint32_t found = 0, odd = 0;
    fe qr, qnr, x, y;
    int ret = -1;

    if (pw_len == 0 || pw_len > sizeof(use) || s->random(s->random_ctx, stub, pw_len) != 0 ||
        random_qr_qnr(s, qr, qnr) != 0)
        return -1;
    max_min(macs, s->own, s->peer);
    bcmw_memset(x_saved, 0, 32);
    /* at least 40 rounds whatever round finds the point, so their number says nothing about the password; after
     * the find, a random stub password is hashed instead (hostap's way) */
    for (counter = 1; counter <= 40 || !found; counter++) {
        uint32_t in_range, res;
        int q;
        fe xc, ysq;
        if (counter > 200)
            goto out;
        bcmw_memcpy(use, pw, pw_len);
        br_ccopy(found, use, stub, pw_len);
        hmac(macs, 12, p, l, 2, seed);
        kdf(seed, "SAE Hunting and Pecking", P_BYTES, 32, value, 256);
        in_range = br_i31_decode_mod(xc, value, 32, F.p);
        fe_curve(ysq, xc);
        q = qr_blind(s, ysq, qr, qnr);
        if (q < 0)
            goto out;
        res = (uint32_t)q & in_range;
        br_ccopy(res & (found ^ 1), x_saved, value, 32);
        odd ^= (res & (found ^ 1)) & ((seed[31] & 1u) ^ odd);
        found |= res;
    }
    /* y = sqrt(x^3 + ax + b): the one whose lowest bit is the seed's */
    fe_set(x, x_saved);
    fe_curve(y, x);
    fe_pow(y, y, F.e_sqrt);
    fe_get(yb, y);
    be_sub(py, P_BYTES, yb, 32);
    br_ccopy(((uint32_t)yb[31] & 1) ^ odd, yb, py, 32);
    bcmw_memcpy(s->pwe, x_saved, 32);
    bcmw_memcpy(s->pwe + 32, yb, 32);
    ret = on_curve(s->pwe) ? 0 : -1;
out:
    bcmw_memset(use, 0, sizeof(use));
    bcmw_memset(stub, 0, sizeof(stub));
    bcmw_memset(seed, 0, sizeof(seed));
    bcmw_memset(value, 0, sizeof(value));
    bcmw_memset(x_saved, 0, sizeof(x_saved));
    bcmw_memset(yb, 0, sizeof(yb));
    bcmw_memset(py, 0, sizeof(py));
    return ret;
}

/* ---- hash to element (12.4.4.2.3) --------------------------------------------------------------------------- */

/* simplified SWU (RFC 9380 6.6.2 as 12.4.4.2.3 writes it), constant time */
static void sswu(uint8_t out[64], const fe u)
{
    fe u2, t1, t2, m, t, x1a, x1b, x1, gx1, x2, gx2, v, x, y, l;
    uint8_t b1[32], b2[32], lb[32];
    uint32_t m_zero, is_qr, same;

    fe_mul(u2, u, u);
    fe_mul(t1, F.z, u2);
    fe_mul(t2, t1, t1);
    fe_copy(m, t1);
    fe_add(m, t2);                          /* m = z^2 u^4 + z u^2 */
    m_zero = br_i31_iszero(m);
    fe_pow(t, m, F.e_inv);                  /* t = 1/m (0 when m is 0) */
    fe_mul(t1, F.z, F.a);
    fe_pow(t1, t1, F.e_inv);
    fe_mul(x1a, F.b, t1);                   /* x1a = b / (z a) */
    fe_copy(t1, F.p);                       /* (the i31 subtraction needs the same length: -b = 0 - b + p) */
    br_i31_sub(t1, F.p, 1);
    fe_sub(t1, F.b);                        /* -b */
    fe_pow(t2, F.a, F.e_inv);
    fe_mul(t1, t1, t2);                     /* -b / a */
    fe_copy(t2, F.one);
    fe_add(t2, t);
    fe_mul(x1b, t1, t2);                    /* x1b = (-b / a) (1 + t) */
    fe_copy(x1, x1b);
    br_ccopy(m_zero, x1, x1a, sizeof(fe));
    fe_curve(gx1, x1);
    fe_mul(t1, F.z, u2);
    fe_mul(x2, t1, x1);                     /* x2 = z u^2 x1 */
    fe_curve(gx2, x2);
    fe_pow(l, gx1, F.e_legendre);
    is_qr = br_i31_iszero(l) | fe_eq(l, F.one);
    fe_copy(v, gx2);
    br_ccopy(is_qr, v, gx1, sizeof(fe));
    fe_copy(x, x2);
    br_ccopy(is_qr, x, x1, sizeof(fe));
    fe_pow(y, v, F.e_sqrt);
    fe_get(b1, u);
    fe_get(b2, y);
    same = ((uint32_t)(b1[31] ^ b2[31]) & 1) ^ 1;
    be_sub(lb, P_BYTES, b2, 32);            /* p - y */
    br_ccopy(same, lb, b2, 32);
    fe_get(out, x);
    bcmw_memcpy(out + 32, lb, 32);
    bcmw_memset(b1, 0, 32);
    bcmw_memset(b2, 0, 32);
    bcmw_memset(lb, 0, 32);
}

static int sae_pt(const uint8_t *ssid, unsigned ssid_len, const uint8_t *pw, unsigned pw_len, const uint8_t *id,
                  unsigned id_len, uint8_t pt[64])
{
    uint8_t seed[32], value[48], p2[64], one[32];
    const uint8_t *p[2] = { pw, id };
    unsigned l[2] = { pw_len, id_len };
    fe u;
    int ret;
    field();
    hmac(ssid, ssid_len, p, l, id ? 2 : 1, seed);          /* pwd-seed = HKDF-Extract(ssid, password [|| id]) */
    hkdf_expand(seed, "SAE Hash to Element u1 P1", value, 48);
    br_i31_decode_reduce(u, value, 48, F.p);
    sswu(pt, u);
    hkdf_expand(seed, "SAE Hash to Element u2 P2", value, 48);
    br_i31_decode_reduce(u, value, 48, F.p);
    sswu(p2, u);
    bcmw_memset(one, 0, 32);
    one[31] = 1;
    ret = ec_muladd(pt, p2, one, one);                      /* PT = P1 + P2 */
    bcmw_memset(seed, 0, sizeof(seed));
    bcmw_memset(value, 0, sizeof(value));
    bcmw_memset(p2, 0, sizeof(p2));
    bcmw_memset(u, 0, sizeof(u));
    return ret;
}

/* PWE = val PT, val = H(0^32, MAX(addresses) || MIN(addresses)) mod (r - 1) + 1 */
static int pwe_from_pt(struct bcmw_sae *s, const uint8_t pt[64])
{
    uint8_t zero[32], macs[12], val[32], t[32], rm1[32], one[32];
    const uint8_t *p[1] = { macs };
    unsigned l[1] = { 12 };
    max_min(macs, s->own, s->peer);
    bcmw_memset(zero, 0, 32);
    hmac(zero, 32, p, l, 1, val);
    bcmw_memset(one, 0, 32);
    one[31] = 1;
    be_sub(rm1, R_BYTES, one, 32);
    br_ccopy(be_sub(t, val, rm1, 32) ^ 1, val, t, 32);     /* val < 2^256 < 2 (r - 1): one subtraction */
    be_add(val, val, one, 32);
    bcmw_memcpy(s->pwe, pt, 64);
    return ec_mul(s->pwe, val);
}

/* ---- commit, confirm ------------------------------------------------------------------------------------------ */

static int commit_from(struct bcmw_sae *s, const uint8_t rand[32], const uint8_t mask[32])
{
    scalar_add(s->scalar, rand, mask);
    if (!be_gt1(s->scalar, 32))
        return -1;
    bcmw_memcpy(s->rand, rand, 32);
    bcmw_memcpy(s->element, s->pwe, 64);
    if (ec_mul(s->element, mask) != 0)          /* COMMIT-ELEMENT = inverse(mask PWE) */
        return -1;
    ec_negate(s->element);
    s->state = BCMW_SAE_COMMITTED;
    s->send_confirm = 0;
    return 0;
}

int bcmw_sae_commit_fixed(struct bcmw_sae *s, const uint8_t rand[32], const uint8_t mask[32])
{
    return commit_from(s, rand, mask);
}

static int sae_commit(struct bcmw_sae *s)
{
    uint8_t rand[32], mask[32];
    int r = -1;
    for (unsigned tries = 0; tries < 20 && r; tries++) {
        if (random_below(s, rand, R_BYTES, 2) || random_below(s, mask, R_BYTES, 2))
            break;
        r = commit_from(s, rand, mask);
    }
    bcmw_memset(rand, 0, 32);
    bcmw_memset(mask, 0, 32);
    return r;
}

static int sae_prepare(struct bcmw_sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *pw,
                       unsigned pw_len, const uint8_t *pt)
{
    int (*random)(void *, uint8_t *, uint32_t) = s->random;
    void *rctx = s->random_ctx;
    bcmw_memset(s, 0, sizeof(*s));
    s->random = random, s->random_ctx = rctx;
    if (!random)
        return -1;
    field();
    bcmw_memcpy(s->own, own, 6);
    bcmw_memcpy(s->peer, peer, 6);
    s->h2e = pt != 0;
    if ((pt ? pwe_from_pt(s, pt) : pwe_hnp(s, pw, pw_len)) != 0)
        return -1;
    return sae_commit(s);
}

static unsigned commit_frame(const struct bcmw_sae *s, uint8_t *out, unsigned cap)
{
    unsigned n = 0;
    if (cap < 8 + 96 + 3 + s->token_len)
        return 0;
    out[n++] = 3, out[n++] = 0, out[n++] = 1, out[n++] = 0;
    out[n++] = s->h2e ? BCMW_SAE_ST_H2E : 0, out[n++] = 0;
    out[n++] = BCMW_SAE_GROUP, out[n++] = 0;
    if (!s->h2e && s->token_len) {
        bcmw_memcpy(out + n, s->token, s->token_len);
        n += s->token_len;
    }
    bcmw_memcpy(out + n, s->scalar, 32);
    bcmw_memcpy(out + n + 32, s->element, 64);
    n += 96;
    if (s->h2e && s->token_len) {           /* the Anti-Clogging Token Container element (extension 93) */
        out[n++] = 255, out[n++] = (uint8_t)(1 + s->token_len), out[n++] = 93;
        bcmw_memcpy(out + n, s->token, s->token_len);
        n += s->token_len;
    }
    return n;
}

static void derive_keys(struct bcmw_sae *s, const uint8_t k[32])
{
    uint8_t zero[32], keyseed[32], ctx[32], keys[64];
    const uint8_t *p[1] = { k };
    unsigned l[1] = { 32 };
    bcmw_memset(zero, 0, 32);
    hmac(zero, 32, p, l, 1, keyseed);       /* keyseed = H(0^32, k) (no rejected groups) */
    scalar_add(ctx, s->scalar, s->peer_scalar);
    kdf(keyseed, "SAE KCK and PMK", ctx, 32, keys, 512);
    bcmw_memcpy(s->kck, keys, 32);
    bcmw_memcpy(s->pmk, keys + 32, 32);
    bcmw_memcpy(s->pmkid, ctx, 16);
    bcmw_memset(keyseed, 0, 32);
    bcmw_memset(keys, 0, 64);
}

static int peer_commit(struct bcmw_sae *s, const uint8_t *f, unsigned len)
{
    uint8_t K[64], one[32];
    const uint8_t *sc, *el;
    unsigned at = 8;
    if (len < 8 + 96 || le16(f) != 3 || le16(f + 2) != 1)
        return BCMW_SAE_ST_UNSPECIFIED;
    if (le16(f + 6) != BCMW_SAE_GROUP)
        return BCMW_SAE_ST_GROUP_REFUSED;
    if (!s->h2e)
        at = len - 96;                      /* an anti-clogging token in front (an AP's check), elements: none */
    sc = f + at;
    el = sc + 32;
    if (!be_gt1(sc, 32) || !be_lt(sc, R_BYTES, 32) || !on_curve(el))
        return BCMW_SAE_ST_UNSPECIFIED;
    if (ct_eq(sc, s->scalar, 32) & ct_eq(el, s->element, 64))
        return -2;                          /* our own commit reflected */
    bcmw_memcpy(s->peer_scalar, sc, 32);
    bcmw_memcpy(s->peer_element, el, 64);
    /* K = rand (peer-scalar PWE + PEER-ELEMENT); k = its x */
    bcmw_memcpy(K, s->pwe, 64);
    bcmw_memset(one, 0, 32);
    one[31] = 1;
    if (ec_muladd(K, s->peer_element, s->peer_scalar, one) != 0 || ec_mul(K, s->rand) != 0) {
        bcmw_memset(K, 0, 64);
        return BCMW_SAE_ST_UNSPECIFIED;
    }
    derive_keys(s, K);
    bcmw_memset(K, 0, 64);
    return 0;
}

static void confirm_of(const struct bcmw_sae *s, const uint8_t sc[2], int ours, uint8_t out[32])
{
    const uint8_t *p[5] = { sc, ours ? s->scalar : s->peer_scalar, ours ? s->element : s->peer_element,
                            ours ? s->peer_scalar : s->scalar, ours ? s->peer_element : s->element };
    unsigned l[5] = { 2, 32, 64, 32, 64 };
    hmac(s->kck, 32, p, l, 5, out);
}

static unsigned confirm_frame(struct bcmw_sae *s, uint8_t *out, unsigned cap)
{
    if (cap < 40)
        return 0;
    if (s->send_confirm < 0xffff)
        s->send_confirm++;
    out[0] = 3, out[1] = 0, out[2] = 2, out[3] = 0, out[4] = 0, out[5] = 0;
    out[6] = (uint8_t)s->send_confirm, out[7] = (uint8_t)(s->send_confirm >> 8);
    confirm_of(s, out + 6, 1, out + 8);
    return 40;
}

static int peer_confirm(struct bcmw_sae *s, const uint8_t *f, unsigned len)
{
    uint8_t v[32];
    uint32_t ok;
    if (len < 40 || le16(f) != 3 || le16(f + 2) != 2 || le16(f + 4) != 0)
        return -1;
    confirm_of(s, f + 6, 0, v);
    ok = ct_eq(v, f + 8, 32);
    bcmw_memset(v, 0, 32);
    return ok ? 0 : -1;
}

/* ---- the station's side ------------------------------------------------------------------------------------- */

static int sta_rx(struct bcmw_sae *s, const uint8_t *f, unsigned len, uint8_t *out, unsigned *out_len, const char **why)
{
    unsigned seq, status;
    int r;
    *out_len = 0;
    *why = "";
    if (len < 6 || le16(f) != 3) {
        *why = "not an SAE frame";
        return BCMW_SAE_IGNORE;
    }
    seq = le16(f + 2);
    status = le16(f + 4);
    if (seq == 1) {
        if (status == BCMW_SAE_ST_TOKEN_REQUIRED) {
            const uint8_t *t = f + 8;
            unsigned tl = len >= 8 ? len - 8 : 0;
            if (s->state != BCMW_SAE_COMMITTED || len < 8 || le16(f + 6) != BCMW_SAE_GROUP) {
                *why = "an anti-clogging request that is not for our commit";
                return BCMW_SAE_IGNORE;
            }
            if (s->h2e) {                   /* the token in its container element */
                const uint8_t *e = 0;
                for (unsigned at = 8; at + 3 <= len && at + 2u + f[at + 1] <= len; at += 2u + f[at + 1])
                    if (f[at] == 255 && f[at + 1] >= 1 && f[at + 2] == 93)
                        e = f + at;
                if (!e) {
                    *why = "an anti-clogging request without its token";
                    return BCMW_SAE_IGNORE;
                }
                t = e + 3, tl = e[1] - 1u;
            }
            if (tl == 0 || tl > BCMW_SAE_TOKEN_MOST || ++s->token_rounds > 3) {
                *why = tl ? "the access point keeps asking for an anti-clogging token" : "an empty anti-clogging token";
                return BCMW_SAE_FAIL;
            }
            bcmw_memcpy(s->token, t, tl);
            s->token_len = tl;
            *out_len = commit_frame(s, out, BCMW_SAE_FRAME_MOST);
            *why = "the access point asked for an anti-clogging token: the commit again, with it";
            return BCMW_SAE_SEND;
        }
        if (status == BCMW_SAE_ST_GROUP_REFUSED) {
            *why = "the access point does not take group 19 (P-256)";
            return BCMW_SAE_FAIL;
        }
        if (status == BCMW_SAE_ST_UNKNOWN_ID) {
            *why = "the access point wants a password identifier";
            return BCMW_SAE_FAIL;
        }
        if (status != 0 && status != BCMW_SAE_ST_H2E) {
            *why = "the access point refused our commit";
            return BCMW_SAE_FAIL;
        }
        if ((status == BCMW_SAE_ST_H2E) != (s->h2e != 0)) {
            *why = "the access point's commit uses the other way to the password element";
            return BCMW_SAE_FAIL;
        }
        if (s->state >= BCMW_SAE_CONFIRMED) {
            /* its commit again (our confirm lost?): our confirm again, for the same commit only */
            if (s->state == BCMW_SAE_CONFIRMED && len >= 8 + 96 && !bcmw_memcmp(f + len - 96, s->peer_scalar, 32) &&
                !bcmw_memcmp(f + len - 64, s->peer_element, 64)) {
                *out_len = confirm_frame(s, out, BCMW_SAE_FRAME_MOST);
                *why = "the access point's commit again: our confirm again";
                return BCMW_SAE_SEND;
            }
            *why = "another commit after ours was answered";
            return BCMW_SAE_IGNORE;
        }
        if (s->state != BCMW_SAE_COMMITTED) {
            *why = "a commit before ours";
            return BCMW_SAE_IGNORE;
        }
        r = peer_commit(s, f, len);
        if (r == -2) {
            *why = "our own commit reflected back: dropped";
            return BCMW_SAE_IGNORE;
        }
        if (r != 0) {
            *why = r == BCMW_SAE_ST_GROUP_REFUSED ? "the access point's commit is not for group 19"
                                                  : "the access point's commit is not valid (scalar or element)";
            return BCMW_SAE_FAIL;
        }
        s->state = BCMW_SAE_CONFIRMED;
        *out_len = confirm_frame(s, out, BCMW_SAE_FRAME_MOST);
        return BCMW_SAE_SEND;
    }
    if (seq == 2) {
        if (status != 0) {
            *why = "the access point refused our confirm";
            return BCMW_SAE_FAIL;
        }
        if (s->state != BCMW_SAE_CONFIRMED) {
            *why = s->state == BCMW_SAE_ACCEPTED ? "a confirm after the exchange ended" : "a confirm before the commit";
            return BCMW_SAE_IGNORE;
        }
        if (peer_confirm(s, f, len) != 0) {
            s->bad_confirm = 1;
            *why = "the access point's confirm does not verify (a wrong password gives this)";
            return BCMW_SAE_FAIL;
        }
        s->state = BCMW_SAE_ACCEPTED;
        bcmw_memset(s->rand, 0, sizeof(s->rand));
        bcmw_memset(s->pwe, 0, sizeof(s->pwe));
        return BCMW_SAE_DONE;
    }
    *why = "an SAE frame of another sequence number";
    return BCMW_SAE_IGNORE;
}

static void sae_clear(struct bcmw_sae *s)
{
    int (*random)(void *, uint8_t *, uint32_t) = s->random;
    void *rctx = s->random_ctx;
    bcmw_memset(s, 0, sizeof(*s));
    s->random = random, s->random_ctx = rctx;
}

static const struct bcmw_sae_ops ops = {
    sae_pt, sae_prepare, commit_frame, peer_commit, confirm_frame, peer_confirm, sta_rx, sae_clear,
};

const struct bcmw_sae_ops *bcmw_sae_get_ops(void) { return &ops; }

void bcmw_sae_enable(struct bcmw *w) { w->sae_ops = &ops; }
