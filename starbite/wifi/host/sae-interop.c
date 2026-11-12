/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's SAE and AKM 8 handshake against hostap's (host/run-sae-interop.sh: hostap's src/common/sae.c, dragonfly.c,
 * wpa_common.c and their crypto on OpenSSL, from a checkout in build/hostap-src that is never in git; BSD, a
 * reference only). Nothing of hostap is linked into the phone's code.
 *  1. SAE, full commit/confirm exchanges as frames: our station with hostap's AP side and hostap's station with our
 *     AP side, hunting-and-pecking and hash-to-element, each several times with fresh randomness, PMK and PMKID
 *     equal on both sides; a wrong password fails on both sides; an anti-clogging token asked by hostap's side.
 *  2. The four-way handshake for AKM 8: our station (wpa.c) answers a message 1 whose PMKID KDE is SAE's; hostap's
 *     wpa_pmk_to_ptk(WPA_KEY_MGMT_SAE) makes the PTK, hostap's wpa_eapol_key_mic checks our message 2; a message 3
 *     made with hostap's KCK/KEK (its AES key wrap, its MIC) with the RSN element, a GTK and an IGTK is taken by
 *     wpa.c (keys equal), its message 4 checked by hostap's MIC.
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "crypto/crypto.h"
#include "crypto/aes_wrap.h"
#include "common/defs.h"
#include "common/wpa_common.h"
#include "common/sae.h"

#include "../sae.h"
#include "../wpa.h"

/* the core's helpers that sae.c and wpa.c use (core.c is not linked here) */
void bcmw_memcpy(void *d, const void *s, uint32_t n) { memcpy(d, s, n); }
void bcmw_memset(void *d, int c, uint32_t n) { volatile uint8_t *p = d; while (n--) *p++ = (uint8_t)c; }
int bcmw_memcmp(const void *a, const void *b, uint32_t n) { return memcmp(a, b, n); }
uint32_t bcmw_strlen(const char *s) { return (uint32_t)strlen(s); }

static int checks, fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static int our_random(void *ctx, uint8_t *out, uint32_t n)
{
    (void)ctx;
    return os_get_random(out, n);
}

static const u8 sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }, ap[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x5a };
static const char ssid[] = "test-ap";

/* hostap's side ready to commit: group 19, its PWE (from its own PT for H2E) and commit */
static int hostap_prepare(struct sae_data *h, const u8 *own, const u8 *peer, const char *pw, int h2e,
                          struct sae_pt **pt)
{
    int groups[] = { 19, 0 };
    memset(h, 0, sizeof(*h));
    if (sae_set_group(h, 19) < 0)
        return -1;
    if (!h2e)
        return sae_prepare_commit(own, peer, (const u8 *)pw, strlen(pw), h);
    *pt = sae_derive_pt(groups, (const u8 *)ssid, strlen(ssid), (const u8 *)pw, strlen(pw), NULL, 0);
    return *pt ? sae_prepare_commit_pt(h, *pt, own, peer, NULL, NULL) : -1;
}

/* hostap's frame body (from the group on) with our 6-byte header in front */
static unsigned framed(uint8_t *out, unsigned seq, unsigned status, const struct wpabuf *b)
{
    out[0] = 3, out[1] = 0, out[2] = (uint8_t)seq, out[3] = 0, out[4] = (uint8_t)status, out[5] = (uint8_t)(status >> 8);
    memcpy(out + 6, wpabuf_head(b), wpabuf_len(b));
    return 6 + (unsigned)wpabuf_len(b);
}

/* our station, hostap's AP */
static int our_sta(const struct bcmw_sae_ops *o, int h2e, const char *sta_pw, const char *ap_pw, int token, const char **why)
{
    struct bcmw_sae s;
    struct sae_data h;
    struct sae_pt *pt = NULL;
    uint8_t ptb[64], f[600], out[600];
    unsigned fn, on;
    struct wpabuf *b = wpabuf_alloc(600);
    int ok = 0, r;
    const u8 *tok = NULL;
    size_t tok_len = 0;

    *why = "";
    memset(&s, 0, sizeof(s));
    s.random = our_random;
    if (h2e && o->pt((const uint8_t *)ssid, (unsigned)strlen(ssid), (const uint8_t *)sta_pw, (unsigned)strlen(sta_pw),
                     NULL, 0, ptb)) {
        *why = "our PT";
        goto out;
    }
    if (o->prepare(&s, sta, ap, (const uint8_t *)sta_pw, (unsigned)strlen(sta_pw), h2e ? ptb : NULL) ||
        hostap_prepare(&h, ap, sta, ap_pw, h2e, &pt)) {
        *why = "prepare";
        goto out;
    }
    fn = o->commit_frame(&s, f, sizeof(f));
    if (token) {
        /* hostap's AP asks for a token (its own encoding: status 76, group, the token / its container) */
        static const u8 t[32] = "an anti-clogging token, 32 byte";
        struct wpabuf *req = wpabuf_alloc(64);
        wpabuf_put_le16(req, 19);
        if (h2e) {
            wpabuf_put_u8(req, WLAN_EID_EXTENSION);
            wpabuf_put_u8(req, 1 + sizeof(t));
            wpabuf_put_u8(req, WLAN_EID_EXT_ANTI_CLOGGING_TOKEN);
        }
        wpabuf_put_data(req, t, sizeof(t));
        r = o->sta_rx(&s, out, framed(out, 1, WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ, req), f, &fn, why);
        wpabuf_free(req);
        if (r != BCMW_SAE_SEND)
            goto out;
        /* hostap's parse finds the token where it expects it */
        if (sae_parse_commit(&h, f + 6, fn - 6, &tok, &tok_len, NULL, h2e, NULL) != WLAN_STATUS_SUCCESS ||
            tok_len != sizeof(t) || memcmp(tok, t, sizeof(t))) {
            *why = "hostap did not find our token";
            goto out;
        }
    } else if ((r = sae_parse_commit(&h, f + 6, fn - 6, NULL, NULL, NULL, h2e, NULL)) != WLAN_STATUS_SUCCESS) {
        *why = "hostap refused our commit";
        goto out;
    }
    if (WPA_GET_LE16(f + 4) != (h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : 0)) {
        *why = "our commit's status";
        goto out;
    }
    if (sae_process_commit(&h) < 0 || sae_write_commit(&h, b, NULL, NULL, 0) < 0) {
        *why = "hostap's commit";
        goto out;
    }
    r = o->sta_rx(&s, f, framed(f, 1, h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : 0, b), out, &on, why);
    if (r != BCMW_SAE_SEND)
        goto out;
    wpabuf_free(b);
    b = wpabuf_alloc(600);
    if (sae_write_confirm(&h, b) < 0)
        goto out;
    r = o->sta_rx(&s, f, framed(f, 2, 0, b), f, &fn, why);
    if (r != BCMW_SAE_DONE)
        goto out;
    if (sae_check_confirm(&h, out + 6, on - 6, NULL) < 0) {
        *why = "hostap: our confirm does not verify";
        goto out;
    }
    ok = !memcmp(s.pmk, h.pmk, 32) && !memcmp(s.pmkid, h.pmkid, 16) && h.pmk_len == 32;
    if (!ok)
        *why = "the PMKs differ";
out:
    wpabuf_free(b);
    sae_clear_data(&h);
    sae_deinit_pt(pt);
    o->clear(&s);
    return ok;
}

/* hostap's station, our AP side (the frames our model AP sends) */
static int our_ap(const struct bcmw_sae_ops *o, int h2e, const char *sta_pw, const char *ap_pw, const char **why)
{
    struct bcmw_sae s;
    struct sae_data h;
    struct sae_pt *pt = NULL;
    uint8_t ptb[64], f[600], g[600];
    unsigned fn, gn;
    struct wpabuf *b = wpabuf_alloc(600);
    int ok = 0;

    *why = "";
    memset(&s, 0, sizeof(s));
    s.random = our_random;
    if (hostap_prepare(&h, sta, ap, sta_pw, h2e, &pt) || sae_write_commit(&h, b, NULL, NULL, 0) < 0) {
        *why = "hostap's commit";
        goto out;
    }
    if (h2e && o->pt((const uint8_t *)ssid, (unsigned)strlen(ssid), (const uint8_t *)ap_pw, (unsigned)strlen(ap_pw), NULL,
                     0, ptb)) {
        *why = "our PT";
        goto out;
    }
    if (o->prepare(&s, ap, sta, (const uint8_t *)ap_pw, (unsigned)strlen(ap_pw), h2e ? ptb : NULL)) {
        *why = "our prepare";
        goto out;
    }
    fn = framed(f, 1, h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : 0, b);
    if (o->peer_commit(&s, f, fn) != 0) {
        *why = "we refused hostap's commit";
        goto out;
    }
    gn = o->commit_frame(&s, g, sizeof(g));
    if (sae_parse_commit(&h, g + 6, gn - 6, NULL, NULL, NULL, h2e, NULL) != WLAN_STATUS_SUCCESS ||
        sae_process_commit(&h) < 0) {
        *why = "hostap refused our commit";
        goto out;
    }
    gn = o->confirm_frame(&s, g, sizeof(g));
    if (sae_check_confirm(&h, g + 6, gn - 6, NULL) < 0) {
        *why = "hostap: our confirm does not verify";
        goto out;
    }
    wpabuf_free(b);
    b = wpabuf_alloc(600);
    sae_write_confirm(&h, b);
    if (o->peer_confirm(&s, f, framed(f, 2, 0, b)) != 0) {
        *why = "hostap's confirm does not verify";
        goto out;
    }
    ok = !memcmp(s.pmk, h.pmk, 32) && !memcmp(s.pmkid, h.pmkid, 16);
    if (!ok)
        *why = "the PMKs differ";
out:
    wpabuf_free(b);
    sae_clear_data(&h);
    sae_deinit_pt(pt);
    o->clear(&s);
    return ok;
}

/* ---- the four-way handshake, AKM 8 ---------------------------------------------------------------------------- */

static unsigned key_msg(u8 *m, unsigned info, u8 replay, const u8 *anonce, const u8 *data, unsigned dlen)
{
    unsigned len = 99 + dlen;
    memset(m, 0, len);
    m[0] = 2, m[1] = 3, WPA_PUT_BE16(m + 2, len - 4);
    m[4] = 2;
    WPA_PUT_BE16(m + 5, info);
    m[8] = 16;
    m[16] = replay;
    if (anonce)
        memcpy(m + 17, anonce, 32);
    WPA_PUT_BE16(m + 97, dlen);
    memcpy(m + 99, data, dlen);
    return len;
}

static void handshake(void)
{
    struct bcmw_wpa wpa;
    struct wpa_ptk ptk;
    u8 pmk[32], pmkid[16], anonce[32], m[700], reply[BCMW_WPA_REPLY_MOST], mic[32], plain[200], wrapped[208];
    u8 rsn_ap[22], rsn_sta[BCMW_RSN_OURS];
    unsigned rl, n = 0, len;
    const char *why = "";
    int r;

    os_get_random(pmk, 32);
    os_get_random(pmkid, 16);
    os_get_random(anonce, 32);
    bcmw_rsn_build(rsn_sta, 4, 8, 0xc0);
    bcmw_rsn_build(rsn_ap, 4, 8, 0xc0);
    memset(&wpa, 0, sizeof(wpa));
    memcpy(wpa.pmk, pmk, 32);
    memcpy(wpa.own, sta, 6);
    memcpy(wpa.peer, ap, 6);
    os_get_random(wpa.own_nonce, 32);
    wpa.rsn = rsn_sta, wpa.rsn_length = sizeof(rsn_sta);
    wpa.akm = 8;
    wpa.ap_rsn = rsn_ap, wpa.ap_rsn_length = sizeof(rsn_ap);
    memcpy(wpa.pmkid, pmkid, 16);
    wpa.pmkid_known = 1;
    wpa.want_igtk = 1;

    /* message 1 with the PMKID KDE */
    plain[0] = 0xdd, plain[1] = 20, plain[2] = 0, plain[3] = 0x0f, plain[4] = 0xac, plain[5] = 4;
    memcpy(plain + 6, pmkid, 16);
    len = key_msg(m, 0x0088, 1, anonce, plain, 22);
    r = bcmw_wpa_message(&wpa, m, len, reply, &rl, &why);
    CHECK(r == BCMW_WPA_ANSWER && (WPA_GET_BE16(reply + 5) & 7) == 0, "4-way: message 1 answered, version 0 (%s)", why);
    CHECK(wpa_pmk_to_ptk(pmk, 32, "Pairwise key expansion", ap, sta, anonce, wpa.own_nonce, &ptk, WPA_KEY_MGMT_SAE,
                         WPA_CIPHER_CCMP, NULL, 0, 0, 0) == 0, "4-way: hostap's PTK");
    CHECK(ptk.kck_len == 16 && ptk.kek_len == 16 && ptk.tk_len == 16 && !memcmp(ptk.kck, wpa.kck, 16) &&
          !memcmp(ptk.kek, wpa.kek, 16) && !memcmp(ptk.tk, wpa.pairwise, 16), "4-way: our PTK is hostap's (KDF-SHA256)");
    memcpy(m, reply, rl);
    memset(m + 81, 0, 16);
    CHECK(wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_SAE, RSN_HASH_SHA256, 0, m, rl, mic, 0) == 0 &&
          !memcmp(mic, reply + 81, 16), "4-way: hostap verifies our message 2's MIC (AES-128-CMAC)");
    CHECK(WPA_GET_BE16(reply + 97) == sizeof(rsn_sta) && !memcmp(reply + 99, rsn_sta, sizeof(rsn_sta)),
          "4-way: message 2 carries our RSN element");

    /* message 3: the AP's RSN element, the GTK (id 1) and IGTK (id 4) KDEs, wrapped and signed with hostap's code */
    memcpy(plain, rsn_ap, sizeof(rsn_ap));
    n = sizeof(rsn_ap);
    plain[n++] = 0xdd, plain[n++] = 22, plain[n++] = 0, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 1;
    plain[n++] = 1, plain[n++] = 0;
    for (int i = 0; i < 16; i++)
        plain[n++] = (u8)(0x60 + i);
    plain[n++] = 0xdd, plain[n++] = 28, plain[n++] = 0, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 9;
    plain[n++] = 4, plain[n++] = 0;
    for (int i = 0; i < 6; i++)
        plain[n++] = (u8)(i + 1);               /* IPN */
    for (int i = 0; i < 16; i++)
        plain[n++] = (u8)(0x90 + i);
    plain[n++] = 0xdd;
    while (n % 8)
        plain[n++] = 0;
    CHECK(aes_wrap(ptk.kek, 16, n / 8, plain, wrapped) == 0, "4-way: hostap's key wrap");
    len = key_msg(m, 0x13c8, 2, anonce, wrapped, n + 8);
    wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_SAE, RSN_HASH_SHA256, 0, m, len, m + 81, 0);
    r = bcmw_wpa_message(&wpa, m, len, reply, &rl, &why);
    CHECK(r == BCMW_WPA_KEYS, "4-way: message 3 from hostap's keys taken (%d: %s)", r, why);
    CHECK(wpa.group_index == 1 && wpa.group[0] == 0x60 && wpa.have_igtk && wpa.igtk_index == 4 && wpa.igtk[0] == 0x90 &&
          wpa.igtk_ipn[0] == 1 && wpa.igtk_ipn[5] == 6, "4-way: the GTK and the IGTK (id, IPN, key)");
    memcpy(m, reply, rl);
    memset(m + 81, 0, 16);
    wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_SAE, RSN_HASH_SHA256, 0, m, rl, mic, 0);
    CHECK(!memcmp(mic, reply + 81, 16) && (WPA_GET_BE16(reply + 5) & 0x0308) == 0x0308, "4-way: hostap verifies message 4");

    /* a message 3 whose RSN element is not the beacon's (a downgrade) is refused */
    {
        struct bcmw_wpa w2 = wpa;
        u8 other[22];
        bcmw_rsn_build(other, 4, 2, 0);
        w2.replay_known = 0;
        w2.ap_rsn = other;
        len = key_msg(m, 0x13c8, 3, anonce, wrapped, n + 8);
        wpa_eapol_key_mic(ptk.kck, ptk.kck_len, WPA_KEY_MGMT_SAE, RSN_HASH_SHA256, 0, m, len, m + 81, 0);
        r = bcmw_wpa_message(&w2, m, len, reply, &rl, &why);
        CHECK(r == BCMW_WPA_BAD && strstr(why, "downgrade"), "4-way: message 3 with another RSN element refused (%s)", why);
    }
}

int main(void)
{
    const struct bcmw_sae_ops *o = bcmw_sae_get_ops();
    const char *why;
    unsigned rounds = 8;

    for (int h2e = 0; h2e < 2; h2e++) {
        const char *m = h2e ? "hash-to-element" : "hunting-and-pecking";
        int a = 0, b = 0;
        for (unsigned i = 0; i < rounds; i++) {
            a += our_sta(o, h2e, "correct horse", "correct horse", 0, &why);
            if (a != (int)i + 1)
                printf("  our station / hostap's AP (%s): %s\n", m, why);
            b += our_ap(o, h2e, "correct horse", "correct horse", &why);
            if (b != (int)i + 1)
                printf("  hostap's station / our AP (%s): %s\n", m, why);
        }
        CHECK(a == (int)rounds, "SAE %s: our station with hostap's AP: %d of %u", m, a, rounds);
        CHECK(b == (int)rounds, "SAE %s: hostap's station with our AP: %d of %u", m, b, rounds);
        CHECK(our_sta(o, h2e, "correct horse", "correct horse", 1, &why), "SAE %s: hostap's anti-clogging token (%s)", m, why);
        CHECK(!our_sta(o, h2e, "wrong horse", "correct horse", 0, &why) && strstr(why, "confirm does not verify"),
              "SAE %s: a wrong password: our station refuses hostap's confirm (%s)", m, why);
        CHECK(!our_ap(o, h2e, "wrong horse", "correct horse", &why), "SAE %s: a wrong password: hostap and our AP (%s)", m,
              why);
    }
    handshake();
    printf("sae-interop: %s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
