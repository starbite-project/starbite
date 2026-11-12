/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's host test, the WPA3 parts that need no model (linked into bcmw-test, called from host/test.c):
 *  - sae.c against IEEE 802.11-2020 J.10 (the vectors hostap's src/common/common_module_tests.c sae_tests() also
 *    carries): hunting-and-pecking's commit from a given rand and mask, the peer's commit, KCK, PMK, PMKID; and
 *    hash-to-element's PT and PWE (with the password identifier of the example);
 *  - our SAE against itself: both methods, a station and an AP side, confirms both ways, a wrong password, a
 *    reflected commit, scalars and elements out of range, the anti-clogging token (both encodings);
 *  - wpa.c's new pieces: HMAC-SHA256 (RFC 4231), AES-128-CMAC (RFC 4493), KDF-SHA256 (against BearSSL's HMAC),
 *    RSN element parsing and the security names.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../bcmw_priv.h"
#include "../sae.h"
#include "../wpa.h"
#include "bearssl.h"
#include "model.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static int test_random(void *ctx, uint8_t *out, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++) {
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        out[i] = (uint8_t)(rng >> 24);
    }
    return 0;
}

static int hex(uint8_t *out, const char *h)
{
    int n = 0;
    while (h[0] && h[1]) {
        unsigned v;
        if (h[0] == ' ') {
            h++;
            continue;
        }
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

#define CHECK(c, what) test_check(!!(c), what)

static void vectors(const struct bcmw_sae_ops *o)
{
    /* J.10 (hostap common_module_tests.c sae_tests) */
    const uint8_t addr1[6] = { 0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87 }, addr2[6] = { 0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c };
    const char *pw = "mekmitasdigoat";
    uint8_t rand[32], mask[32], local_commit[98], peer_commit[98], kck[32], pmk[32], pmkid[16], frame[400], pt[64];
    uint8_t pwe_x[32], pwe_y[32];
    const uint8_t addr1b[6] = { 0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e }, addr2b[6] = { 0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46 };
    struct bcmw_sae s;
    unsigned n;

    hex(rand, "992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94");
    hex(mask, "9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322");
    hex(local_commit, "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65d5ad9e00829707aa36ba8b859738"
                      "fc961d08243505f47c035376d7ac4bc8d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1");
    hex(peer_commit, "1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3873f20556953a96c"
                     "91536fd8ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db06687032399862999b511e0a1552a5fea317c2");
    hex(kck, "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a");
    hex(pmk, "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59");
    hex(pmkid, "8747a600eea3f9f22475df58ca1e5498");
    hex(pwe_x, "c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e");
    hex(pwe_y, "73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0");

    memset(&s, 0, sizeof(s));
    s.random = test_random;
    CHECK(o->prepare(&s, addr1, addr2, (const uint8_t *)pw, (unsigned)strlen(pw), NULL) == 0, "sae: J.10 hunting-and-pecking PWE");
    CHECK(bcmw_sae_commit_fixed(&s, rand, mask) == 0, "sae: J.10 commit from rand and mask");
    n = o->commit_frame(&s, frame, sizeof(frame));
    CHECK(n == 6 + 98 && !memcmp(frame, "\x03\x00\x01\x00\x00\x00", 6) && !memcmp(frame + 6, local_commit, 98),
          "sae: J.10 our commit is the standard's");
    memcpy(frame + 6, peer_commit, 98);
    CHECK(o->peer_commit(&s, frame, 6 + 98) == 0, "sae: J.10 the peer's commit taken");
    CHECK(!memcmp(s.kck, kck, 32), "sae: J.10 KCK");
    CHECK(!memcmp(s.pmk, pmk, 32), "sae: J.10 PMK");
    CHECK(!memcmp(s.pmkid, pmkid, 16), "sae: J.10 PMKID");

    CHECK(o->pt((const uint8_t *)"byteme", 6, (const uint8_t *)pw, (unsigned)strlen(pw), (const uint8_t *)"psk4internet",
                12, pt) == 0, "sae: J.10 hash-to-element PT");
    memset(&s, 0, sizeof(s));
    s.random = test_random;
    CHECK(o->prepare(&s, addr1b, addr2b, NULL, 0, pt) == 0 && s.h2e, "sae: J.10 PWE from PT");
    CHECK(!memcmp(s.pwe, pwe_x, 32) && !memcmp(s.pwe + 32, pwe_y, 32), "sae: J.10 PWE from PT is the standard's");
    o->clear(&s);
}

/* one exchange between a station and an AP, both ours; 1 if both PMKs and PMKIDs agree */
static int exchange(const struct bcmw_sae_ops *o, int h2e, const char *sta_pw, const char *ap_pw, int token,
                    const char **fail)
{
    const uint8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }, ap[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x5a };
    struct bcmw_sae a, b;
    uint8_t pt_a[64], pt_b[64], f[400], g[400], out[400];
    unsigned fn, gn, on;
    const char *why = "";
    int r;

    *fail = "";
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.random = b.random = test_random;
    if (h2e) {
        o->pt((const uint8_t *)"test-ap", 7, (const uint8_t *)sta_pw, (unsigned)strlen(sta_pw), NULL, 0, pt_a);
        o->pt((const uint8_t *)"test-ap", 7, (const uint8_t *)ap_pw, (unsigned)strlen(ap_pw), NULL, 0, pt_b);
    }
    if (o->prepare(&a, sta, ap, (const uint8_t *)sta_pw, (unsigned)strlen(sta_pw), h2e ? pt_a : NULL) ||
        o->prepare(&b, ap, sta, (const uint8_t *)ap_pw, (unsigned)strlen(ap_pw), h2e ? pt_b : NULL)) {
        *fail = "prepare";
        return 0;
    }
    fn = o->commit_frame(&a, f, sizeof(f));
    if (token) {
        /* the AP asks for a token: status 76, group, the token (H2E: in its container element) */
        uint8_t req[64] = { 3, 0, 1, 0, 76, 0, 19, 0 };
        unsigned rn = 8;
        if (h2e)
            req[rn++] = 255, req[rn++] = 1 + 20, req[rn++] = 93;
        for (unsigned i = 0; i < 20; i++)
            req[rn++] = (uint8_t)(0xc0 + i);
        r = o->sta_rx(&a, req, rn, out, &on, &why);
        if (r != BCMW_SAE_SEND || on != fn + 20 + (h2e ? 3u : 0u)) {
            *fail = "the anti-clogging answer";
            return 0;
        }
        if (h2e ? memcmp(out + on - 20, req + 11, 20) || out[on - 23] != 255 || out[on - 21] != 93
                : memcmp(out + 8, req + 8, 20)) {
            *fail = "the token's place";
            return 0;
        }
        memcpy(f, out, on);
        fn = on;
    }
    /* the AP takes the station's commit (a token in front is skipped), answers with its commit and confirm */
    if ((r = o->peer_commit(&b, f, fn)) != 0) {
        *fail = "the AP refused the station's commit";
        return 0;
    }
    gn = o->commit_frame(&b, g, sizeof(g));
    r = o->sta_rx(&a, g, gn, out, &on, &why);
    if (r != BCMW_SAE_SEND || on != 40) {
        *fail = why;
        return 0;
    }
    memcpy(f, out, on);
    fn = on;                                /* the station's confirm */
    gn = o->confirm_frame(&b, g, sizeof(g));
    r = o->sta_rx(&a, g, gn, out, &on, &why);
    if (r != BCMW_SAE_DONE) {
        *fail = why;
        return 0;
    }
    if (o->peer_confirm(&b, f, fn) != 0) {
        *fail = "the AP: the station's confirm";
        return 0;
    }
    return !memcmp(a.pmk, b.pmk, 32) && !memcmp(a.pmkid, b.pmkid, 16) && memcmp(a.pmk, "\0\0\0\0\0\0\0\0", 8);
}

static void ours(const struct bcmw_sae_ops *o)
{
    const char *why;
    const uint8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }, ap[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x5a };
    struct bcmw_sae a, b;
    uint8_t f[400], out[400];
    unsigned fn, on;
    int r;

    for (int h2e = 0; h2e < 2; h2e++) {
        char what[100];
        snprintf(what, sizeof(what), "sae: ours with ours (%s)", h2e ? "hash-to-element" : "hunting-and-pecking");
        CHECK(exchange(o, h2e, "correct horse", "correct horse", 0, &why), what);
        snprintf(what, sizeof(what), "sae: an anti-clogging token (%s)", h2e ? "in its container" : "before the scalar");
        CHECK(exchange(o, h2e, "correct horse", "correct horse", 1, &why), what);
        CHECK(!exchange(o, h2e, "wrong horse", "correct horse", 0, &why) &&
              strstr(why, "confirm does not verify"), "sae: a wrong password fails at the AP's confirm");
    }
    CHECK(exchange(o, 0, "a password of sixty-three characters, for the longest one there!", "a password of sixty-three "
                   "characters, for the longest one there!", 0, &why), "sae: a long password");

    /* a reflection, scalars out of range, an element off the curve, a group refused */
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.random = b.random = test_random;
    o->prepare(&a, sta, ap, (const uint8_t *)"pw-12345", 8, NULL);
    o->prepare(&b, ap, sta, (const uint8_t *)"pw-12345", 8, NULL);
    fn = o->commit_frame(&a, f, sizeof(f));
    r = o->sta_rx(&a, f, fn, out, &on, &why);
    CHECK(r == BCMW_SAE_IGNORE && strstr(why, "reflected"), "sae: our own commit reflected is dropped");
    memcpy(out, f, fn);
    memset(out + 8, 0, 32);
    out[39] = 1;
    CHECK(o->peer_commit(&b, out, fn) == BCMW_SAE_ST_UNSPECIFIED, "sae: a scalar of 1 is refused");
    memset(out + 8, 0xff, 32);
    CHECK(o->peer_commit(&b, out, fn) == BCMW_SAE_ST_UNSPECIFIED, "sae: a scalar beyond r is refused");
    memcpy(out, f, fn);
    out[fn - 1] ^= 1;
    CHECK(o->peer_commit(&b, out, fn) == BCMW_SAE_ST_UNSPECIFIED, "sae: an element off the curve is refused");
    memcpy(out, f, fn);
    out[6] = 20;
    CHECK(o->peer_commit(&b, out, fn) == BCMW_SAE_ST_GROUP_REFUSED, "sae: group 20 is refused (77)");
    {
        uint8_t refuse[8] = { 3, 0, 1, 0, 77, 0, 19, 0 };
        r = o->sta_rx(&a, refuse, 8, out, &on, &why);
        CHECK(r == BCMW_SAE_FAIL && strstr(why, "group 19"), "sae: the AP refusing group 19 ends it");
    }
    o->clear(&a);
    o->clear(&b);
}

static void wpa_pieces(void)
{
    uint8_t key[64], data[64], mac[32], want[32], out[48], t[64];
    const uint8_t *p[1];
    unsigned l[1];

    /* RFC 4231 test case 2: key "Jefe", "what do ya want for nothing?" */
    p[0] = (const uint8_t *)"what do ya want for nothing?", l[0] = 28;
    bcmw_hmac_sha256((const uint8_t *)"Jefe", 4, p, l, 1, mac);
    hex(want, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(!memcmp(mac, want, 32), "HMAC-SHA256 (RFC 4231 case 2)");
    /* RFC 4493: AES-128-CMAC, the empty message, 16 and 40 bytes */
    hex(key, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(data, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411");
    l[0] = 0, p[0] = data;
    bcmw_aes_cmac(key, p, l, 1, mac);
    hex(want, "bb1d6929e95937287fa37d129b756746");
    CHECK(!memcmp(mac, want, 16), "AES-CMAC (RFC 4493, empty)");
    l[0] = 16;
    bcmw_aes_cmac(key, p, l, 1, mac);
    hex(want, "070a16b46b4d4144f79bdd9dd04a287c");
    CHECK(!memcmp(mac, want, 16), "AES-CMAC (RFC 4493, 16 bytes)");
    {
        const uint8_t *pp[3] = { data, data + 7, data + 30 };
        unsigned ll[3] = { 7, 23, 10 };
        bcmw_aes_cmac(key, pp, ll, 3, mac);
        hex(want, "dfa66747de9ae63030ca32611497c827");
        CHECK(!memcmp(mac, want, 16), "AES-CMAC (RFC 4493, 40 bytes in three pieces)");
    }
    /* KDF-SHA256 against BearSSL's HMAC of the same input */
    for (unsigned i = 0; i < 32; i++)
        key[i] = (uint8_t)(i * 3), data[i] = (uint8_t)(0x40 + i);
    bcmw_kdf_sha256(key, 32, "Pairwise key expansion", data, 32, out, 48);
    for (unsigned blk = 1; blk <= 2; blk++) {
        br_hmac_key_context kc;
        br_hmac_context hc;
        uint8_t hdr[2] = { (uint8_t)blk, 0 }, bits[2] = { 384 & 0xff, 384 >> 8 };
        br_hmac_key_init(&kc, &br_sha256_vtable, key, 32);
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, hdr, 2);
        br_hmac_update(&hc, "Pairwise key expansion", 22);
        br_hmac_update(&hc, data, 32);
        br_hmac_update(&hc, bits, 2);
        br_hmac_out(&hc, t + 32 * (blk - 1));
    }
    CHECK(!memcmp(out, t, 48), "KDF-SHA256 (384 bits) against BearSSL's HMAC");
    {
        /* RSN elements: a transition network's, a WPA3-only one with BIP-GMAC as its group management cipher */
        static const uint8_t tr[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f,
                                      0xac, 8, 0x80, 0 };
        static const uint8_t w3[] = { 48, 26, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8, 0xc0, 0,
                                      0, 0, 0, 0x0f, 0xac, 11 };
        struct bcmw_rsn r;
        unsigned g;
        CHECK(bcmw_rsn_parse(tr, sizeof(tr), &r) == 0 && r.akms == ((1u << 2) | (1u << 8)) && r.caps == 0x80 &&
              r.group == 4 && r.pairwise == 1u << 4 && r.group_mgmt == 0, "RSN: a transition network's element");
        CHECK(bcmw_rsn_check(tr, sizeof(tr), &g) == 0, "RSN: a transition network still passes the WPA2 check");
        CHECK(bcmw_rsn_parse(w3, sizeof(w3), &r) == 0 && r.akms == 1u << 8 && r.caps == 0xc0 && r.group_mgmt == 11,
              "RSN: a WPA3 element with a group management cipher");
        CHECK(bcmw_rsn_parse(w3, 10, &r) != 0, "RSN: a truncated element");
        bcmw_rsn_build(data, 4, 8, 0xc0);
        CHECK(bcmw_rsn_parse(data, 22, &r) == 0 && r.akms == 1u << 8 && r.caps == 0xc0, "RSN: ours for SAE");
        {
            struct bcmw_bss b;
            uint8_t ie[64];
            memset(&b, 0, sizeof(b));
            b.ie = ie;
            memcpy(ie, tr, sizeof(tr));
            b.ie_len = sizeof(tr);
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_WPA2_WPA3 && !strcmp(bcmw_security_name(2), "WPA2/WPA3"),
                  "security: a transition network");
            memcpy(ie, w3, sizeof(w3));
            b.ie_len = sizeof(w3);
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_WPA3, "security: WPA3 only");
            ie[19] = 1;                         /* 802.1X */
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_OTHER, "security: enterprise is other");
            ie[19] = 2, ie[13] = 2;             /* PSK with TKIP only */
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_OTHER, "security: TKIP only is other");
            ie[13] = 4;
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_WPA2, "security: WPA2");
            b.ie_len = 0;
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_OPEN, "security: open");
            b.capability = 0x0011;
            CHECK(bcmw_bss_security(&b) == BCMW_SEC_OTHER, "security: privacy without RSN (WEP, WPA1) is other");
        }
    }
}

void test_sae(void)
{
    const struct bcmw_sae_ops *o = bcmw_sae_get_ops();
    vectors(o);
    ours(o);
    wpa_pieces();
}
