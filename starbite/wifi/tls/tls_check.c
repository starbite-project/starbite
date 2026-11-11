/* SPDX-License-Identifier: MIT
 * The offline TLS self-check (tls_check.h): what sbhttp's TLS needs, run on the phone's own CPU without a network,
 * against known answers: SHA-1, SHA-256, SHA-384, HMAC-SHA256 (RFC 4231 case 2), AES-128-GCM (the GCM spec's test
 * case 2), ECDH on P-256 both ways, then two real certificate chains (RSA and ECDSA) through BearSSL's X.509
 * engine and the trust anchors in tls/ta.c at the time they were fetched; the same chains for another name, two
 * years later, and with a damaged signature must be refused. Each line: what, ok or FAILED, the time it took. */
#include "tls_check.h"
#include "bearssl.h"
#include "../http.h"

struct ctx { void (*log)(void *, const char *); void *c; uint64_t (*now)(void *); uint64_t t0; };

static void line(struct ctx *k, const char *what, int ok, int code)
{
    char b[160];
    unsigned n = 0, us;
    const char *p;
    uint64_t t = k->now ? k->now(k->c) : 0;
    us = (unsigned)(t - k->t0);
    for (p = "tls: self-check: "; *p; p++) b[n++] = *p;
    for (p = what; *p && n < 110; p++) b[n++] = *p;
    for (p = ok ? ": ok (" : ": FAILED (code "; *p; p++) b[n++] = *p;
    if (!ok) {
        char d[12];
        int i = 0, v = code < 0 ? -code : code;
        if (code < 0) b[n++] = '-';
        do { d[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (i--) b[n++] = d[i];
        b[n++] = ',', b[n++] = ' ';
    }
    {
        char d[12];
        int i = 0;
        do { d[i++] = (char)('0' + us % 10); us /= 10; } while (us);
        while (i--) b[n++] = d[i];
    }
    for (p = " ms)"; *p; p++) b[n++] = *p;
    b[n] = 0;
    if (k->log)
        k->log(k->c, b);
    k->t0 = t;
}

static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned d = 0;
    while (n--)
        d |= (unsigned)(*a++ ^ *b++);
    return !d;
}

static void cpy(uint8_t *d, const uint8_t *s, unsigned n) { while (n--) *d++ = *s++; }

/* one chain through the X.509 engine: BR_ERR_OK or its error */
static int chain(const struct sbtls_chain *c, const char *host, uint64_t t, int damage)
{
    static br_ssl_client_context cc;            /* only to set the X.509 engine up exactly as sbhttp's client */
    static br_x509_minimal_context xc;
    static uint8_t buf[4096];
    const br_x509_class **v = &xc.vtable;
    br_ssl_client_init_full(&cc, &xc, sbtls_tas, sbtls_tas_num);
    br_x509_minimal_set_time(&xc, (uint32_t)(t / 86400 + 719528), (uint32_t)(t % 86400));
    (*v)->start_chain(v, host);
    for (unsigned i = 0; i < c->n; i++) {
        const unsigned char *der = c->certs[i].der;
        unsigned len = c->certs[i].len;
        if (i == 0 && damage && len <= sizeof(buf)) {
            cpy(buf, der, len);
            buf[len - 9] ^= 0x40;                   /* inside the signature at the end */
            der = buf;
        }
        (*v)->start_cert(v, len);
        (*v)->append(v, der, len);
        (*v)->end_cert(v);
    }
    return (int)(*v)->end_chain(v);
}

int sbtls_selfcheck(void (*log)(void *ctx, const char *line), void *lctx, uint64_t (*now_ms)(void *ctx))
{
    struct ctx k = { log, lctx, now_ms, now_ms ? now_ms(lctx) : 0 };
    uint8_t out[64];
    int fail = 0, n = 0;
#define CHK(what, cond, code)                     \
    do {                                          \
        int ok_ = (cond);                         \
        ++n;                                      \
        line(&k, what, ok_, code);                \
        if (!ok_ && !fail)                        \
            fail = n;                             \
    } while (0)

    /* hashes */
    {
        static const uint8_t s1[20] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
        static const uint8_t s256[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                          0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                          0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
        static const uint8_t s384[8] = { 0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b };
        br_sha1_context a;
        br_sha256_context b;
        br_sha384_context c;
        br_sha1_init(&a), br_sha1_update(&a, "abc", 3), br_sha1_out(&a, out);
        CHK("SHA-1 of \"abc\"", same(out, s1, 20), 0);
        br_sha256_init(&b), br_sha256_update(&b, "abc", 3), br_sha256_out(&b, out);
        CHK("SHA-256 of \"abc\"", same(out, s256, 32), 0);
        br_sha384_init(&c), br_sha384_update(&c, "abc", 3), br_sha384_out(&c, out);
        CHK("SHA-384 of \"abc\"", same(out, s384, 8), 0);
    }
    /* HMAC-SHA256, RFC 4231 test case 2 */
    {
        static const uint8_t want[8] = { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e };
        br_hmac_key_context hk;
        br_hmac_context h;
        br_hmac_key_init(&hk, &br_sha256_vtable, "Jefe", 4);
        br_hmac_init(&h, &hk, 0);
        br_hmac_update(&h, "what do ya want for nothing?", 28);
        br_hmac_out(&h, out);
        CHK("HMAC-SHA256 (RFC 4231 case 2)", same(out, want, 8), 0);
    }
    /* AES-128-GCM, the GCM spec's test case 2: key, IV and one block all zero */
    {
        static const uint8_t ct[16] = { 0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                        0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78 };
        static const uint8_t tag[16] = { 0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                         0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf };
        uint8_t key[16] = { 0 }, iv[12] = { 0 }, data[16] = { 0 }, t[16];
        br_aes_ct64_ctr_keys ac;
        br_gcm_context g;
        br_aes_ct64_ctr_init(&ac, key, 16);
        br_gcm_init(&g, &ac.vtable, br_ghash_ctmul64);
        br_gcm_reset(&g, iv, 12);
        br_gcm_flip(&g);
        br_gcm_run(&g, 1, data, 16);
        br_gcm_get_tag(&g, t);
        CHK("AES-128-GCM (test case 2)", same(data, ct, 16) && same(t, tag, 16), 0);
    }
    /* ECDH on P-256: two fixed keys, each side's secret from the other's public point */
    {
        const br_ec_impl *ec = &br_ec_all_m31;
        uint8_t a[32], b[32], pa[65], pb[65], sa[65], sb[65];
        size_t la, lb;
        for (int i = 0; i < 32; i++)
            a[i] = (uint8_t)(i * 7 + 1), b[i] = (uint8_t)(200 - i * 3);
        a[0] = 0x3a, b[0] = 0x5c;
        la = ec->mulgen(pa, a, 32, BR_EC_secp256r1);
        lb = ec->mulgen(pb, b, 32, BR_EC_secp256r1);
        cpy(sa, pb, 65), cpy(sb, pa, 65);
        {
            uint32_t r1 = la == 65 && lb == 65 ? ec->mul(sa, 65, a, 32, BR_EC_secp256r1) : 0;
            uint32_t r2 = la == 65 && lb == 65 ? ec->mul(sb, 65, b, 32, BR_EC_secp256r1) : 0;
            CHK("ECDH on P-256 (both ways agree)", r1 && r2 && same(sa + 1, sb + 1, 32) && !same(sa + 1, pa + 1, 32), 0);
        }
    }
    /* the chains */
    for (int i = 0; i < 2; i++) {
        const struct sbtls_chain *c = &sbtls_selftest_chains[i];
        int r = chain(c, c->host, sbtls_selftest_time, 0);
        CHK(i ? "an ECDSA chain (Let's Encrypt, ISRG Root X2) accepted" : "an RSA chain (example.com) accepted",
            r == BR_ERR_OK, r);
        r = chain(c, "not-this-name.example", sbtls_selftest_time, 0);
        CHK(i ? "the ECDSA chain for another name refused" : "the RSA chain for another name refused",
            r == BR_ERR_X509_BAD_SERVER_NAME, r);
        r = chain(c, c->host, sbtls_selftest_time + 2ull * 366 * 86400, 0);
        CHK(i ? "the ECDSA chain two years later refused" : "the RSA chain two years later refused",
            r == BR_ERR_X509_EXPIRED, r);
        r = chain(c, c->host, sbtls_selftest_time, 1);
        CHK(i ? "the ECDSA chain with a damaged signature refused" : "the RSA chain with a damaged signature refused",
            r != BR_ERR_OK, r);
    }
#undef CHK
    return fail;
}
