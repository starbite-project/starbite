/* SPDX-License-Identifier: GPL-2.0-only
 * The model's access point (host/model.c): the network "test-ap" with WPA2-PSK, CCMP, passphrase
 * "correct horse"; its BSSID 02:11:22:33:44:5a. It answers a directed escan with an RSN element, takes the
 * station's security settings, and on SET_SSID associates (SET_SSID, LINK events). With the firmware's
 * supplicant (model M.fwsup: "cap" lists idsup) it checks the PMK and sends PSK_SUP keyed; with the host's
 * it plays the authenticator of the four-way handshake: message 1, checks message 2's MIC and RSN element,
 * message 3 with the GTK wrapped (its own AES-128 encryption and RFC 3394 key wrap, checked against the RFC's
 * example), checks message 4, and then the keys the host installs must be its TK and GTK.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../bcmw_priv.h"
#include "../wpa.h"
#include "model.h"

static const uint8_t ap_bssid[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x5a };
static const char ap_ssid[] = "test-ap";
static const char ap_pass[] = "correct horse";
static const uint8_t gtk[16] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };

static struct {
    uint8_t pmk[32], anonce[32], snonce[32], sta[6], ptk[48], wpaie[64];
    unsigned wpaie_len, wsec, wpa_auth, sup_wpa, step;
    int joined;
} A;

/* ---- AES-128 encryption and the key wrap (the authenticator's side; FIPS 197, RFC 3394) -------- */

static uint8_t sbox[256];

static uint8_t xt(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (; b; b >>= 1) {
        if (b & 1)
            p ^= a;
        a = (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0));
    }
    return p;
}

static void tables(void)
{
    if (sbox[0])
        return;
    for (unsigned x = 0; x < 256; x++) {
        uint8_t inv = 0, s;
        for (unsigned y = 1; y < 256 && x; y++)
            if (xt((uint8_t)x, (uint8_t)y) == 1) {
                inv = (uint8_t)y;
                break;
            }
        s = inv;
        for (unsigned i = 1; i < 5; i++)
            s ^= (uint8_t)(inv << i | inv >> (8 - i));
        sbox[x] = s ^ 0x63;
    }
}

static void expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rc = 1;
    tables();
    memcpy(rk, key, 16);
    for (unsigned i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t f = t[0];
            t[0] = sbox[t[1]] ^ rc;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[f];
            rc = xt(rc, 2);
        }
        for (unsigned j = 0; j < 4; j++)
            rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}

static void encrypt(const uint8_t rk[176], uint8_t s[16])
{
    for (unsigned i = 0; i < 16; i++)
        s[i] ^= rk[i];
    for (unsigned round = 1; round <= 10; round++) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; c++)
            for (unsigned r = 0; r < 4; r++)
                t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
        if (round < 10)
            for (unsigned c = 0; c < 4; c++) {
                uint8_t *a = t + 4 * c, b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                a[0] = xt(b0, 2) ^ xt(b1, 3) ^ b2 ^ b3;
                a[1] = b0 ^ xt(b1, 2) ^ xt(b2, 3) ^ b3;
                a[2] = b0 ^ b1 ^ xt(b2, 2) ^ xt(b3, 3);
                a[3] = xt(b0, 3) ^ b1 ^ b2 ^ xt(b3, 2);
            }
        for (unsigned i = 0; i < 16; i++)
            s[i] = t[i] ^ rk[16 * round + i];
    }
}

static void wrap(const uint8_t kek[16], const uint8_t *plain, unsigned len, uint8_t *out)
{
    unsigned n = len / 8;
    uint8_t rk[176], a[8], b[16];
    expand(kek, rk);
    memset(a, 0xa6, 8);
    memcpy(out + 8, plain, len);
    for (unsigned j = 0; j <= 5; j++)
        for (unsigned i = 1; i <= n; i++) {
            uint64_t t = (uint64_t)n * j + i;
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            encrypt(rk, b);
            memcpy(a, b, 8);
            for (unsigned k = 0; k < 8; k++)
                a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

int model_ap_selftest(void)
{
    /* RFC 3394 4.1: 128 bits of key data with a 128-bit KEK */
    static const uint8_t kek[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t data[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t want[24] = { 0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47, 0xae, 0xf3, 0x4b, 0xd8,
                                      0xfb, 0x5a, 0x7b, 0x82, 0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5 };
    uint8_t out[24];
    wrap(kek, data, 16, out);
    return memcmp(out, want, 24) == 0;
}

/* ---- the handshake's frames ------------------------------------------------------------------------ */

static void mic(const uint8_t *msg, unsigned len, uint8_t out[16])
{
    uint8_t z[16] = { 0 }, d[20];
    const uint8_t *p[3] = { msg, z, msg + 81 + 16 };
    const unsigned l[3] = { 81, 16, len - 81 - 16 };
    bcmw_hmac_sha1(A.ptk, 16, p, l, 3, d);
    memcpy(out, d, 16);
}

static void send_eapol(const uint8_t *body, unsigned len)
{
    uint8_t f[600];
    memcpy(f, A.sta, 6);
    memcpy(f + 6, ap_bssid, 6);
    f[12] = 0x88, f[13] = 0x8e;
    memcpy(f + 14, body, len);
    model_rx(f, 14 + len);
}

static unsigned key_msg(uint8_t *m, unsigned info, uint8_t replay, const uint8_t *data, unsigned dlen)
{
    unsigned len = 99 + dlen;
    memset(m, 0, len);
    m[0] = 2, m[1] = 3, m[2] = (uint8_t)((len - 4) >> 8), m[3] = (uint8_t)(len - 4);
    m[4] = 2;
    m[5] = (uint8_t)(info >> 8), m[6] = (uint8_t)info;
    m[8] = 16;
    m[16] = replay;
    memcpy(m + 17, A.anonce, 32);
    m[97] = (uint8_t)(dlen >> 8), m[98] = (uint8_t)dlen;
    memcpy(m + 99, data, dlen);
    return len;
}

static void message1(void)
{
    uint8_t m[128];
    for (unsigned i = 0; i < 32; i++)
        A.anonce[i] = (uint8_t)(0xa0 + i);
    send_eapol(m, key_msg(m, 0x008a, 1, 0, 0));
    A.step = 1;
}

static void message3(void)
{
    uint8_t plain[64], wrapped[72], m[256];
    unsigned n = 0, len;
    memcpy(plain, A.wpaie, A.wpaie_len);
    n = A.wpaie_len;
    plain[n++] = 0xdd, plain[n++] = 22, plain[n++] = 0x00, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 1;
    plain[n++] = 1, plain[n++] = 0;     /* key index 1 */
    memcpy(plain + n, gtk, 16);
    n += 16;
    plain[n++] = 0xdd;                  /* padding to a multiple of 8 */
    while (n % 8)
        plain[n++] = 0;
    wrap(A.ptk + 16, plain, n, wrapped);
    len = key_msg(m, 0x13ca, 2, wrapped, n + 8);
    mic(m, len, m + 81);
    send_eapol(m, len);
    A.step = 3;
}

/* a frame the station sent (the model's tx path) */
void model_ap_tx(const uint8_t *f, unsigned len)
{
    const uint8_t *m = f + 14;
    unsigned mlen = len - 14, info;
    uint8_t want[16];

    M.tx_seen++;
    if (len < 14 || f[12] != 0x88 || f[13] != 0x8e) {
        if (len >= 14 && f[12] == 0x08 && f[13] == 0x00)
            M.tx_ip++;
        return;
    }
    if (mlen < 99 || memcmp(f, ap_bssid, 6))
        return;
    info = (unsigned)m[5] << 8 | m[6];
    if (A.step == 1 && (info & 0x0108) == 0x0108) {     /* message 2: pairwise, MIC */
        uint8_t data[76];
        const uint8_t *aa = ap_bssid, *sa = A.sta;
        int af = memcmp(aa, sa, 6) < 0;
        memcpy(A.snonce, m + 17, 32);
        memcpy(data, af ? aa : sa, 6);
        memcpy(data + 6, af ? sa : aa, 6);
        af = memcmp(A.anonce, A.snonce, 32) < 0;
        memcpy(data + 12, af ? A.anonce : A.snonce, 32);
        memcpy(data + 44, af ? A.snonce : A.anonce, 32);
        bcmw_prf(A.pmk, 32, "Pairwise key expansion", data, 76, A.ptk, 48);
        mic(m, mlen, want);
        if (memcmp(want, m + 81, 16)) {
            M.ap_errors |= 1;           /* message 2's MIC */
            return;
        }
        if (((unsigned)m[97] << 8 | m[98]) != A.wpaie_len || memcmp(m + 99, A.wpaie, A.wpaie_len))
            M.ap_errors |= 2;           /* message 2's RSN element is not the association's */
        message3();
        return;
    }
    if (A.step == 3 && (info & 0x0308) == 0x0308) {     /* message 4: pairwise, MIC, secure */
        mic(m, mlen, want);
        if (memcmp(want, m + 81, 16)) {
            M.ap_errors |= 4;
            return;
        }
        A.step = 4;
        M.handshake_done = 1;
    }
}

/* ---- ioctls --------------------------------------------------------------------------------------- */

void model_ap_reset(void)
{
    memset(&A, 0, sizeof(A));
    bcmw_wpa_pmk((const unsigned char *)ap_pass, (unsigned)strlen(ap_pass), (const unsigned char *)ap_ssid,
                 (unsigned)strlen(ap_ssid), A.pmk);
}

/* a directed escan's result: one BSS of this name with an RSN element (WPA2-PSK, CCMP) */
unsigned model_ap_scan_result(uint8_t *r, unsigned cap)
{
    static const uint8_t rsn[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    unsigned blen = 128 + 9 + sizeof(rsn), total = 12 + blen;
    if (total > cap)
        return 0;
    memset(r, 0, total);
    put32(r, total);
    put32(r + 4, 109);
    put16(r + 10, 1);
    uint8_t *b = r + 12;
    put32(b, 109);
    put32(b + 4, blen);
    memcpy(b + 8, ap_bssid, 6);
    put16(b + 16, 0x0011);              /* ESS, privacy */
    b[18] = (uint8_t)strlen(ap_ssid);
    memcpy(b + 19, ap_ssid, strlen(ap_ssid));
    put16(b + 72, 0xd024);              /* a 5 GHz chanspec */
    put16(b + 78, (uint16_t)-48);
    put16(b + 116, 128);
    put32(b + 120, 9 + sizeof(rsn));
    b[128] = 0, b[129] = 7;             /* the SSID element */
    memcpy(b + 130, ap_ssid, 7);
    memcpy(b + 137, rsn, sizeof(rsn));
    return total;
}

/* 1 if the ioctl is the access point's to answer */
int model_ap_ioctl(uint32_t cmd, const char *name, const uint8_t *data, uint32_t dlen, uint8_t *out, uint16_t *rlen,
                   uint16_t *status)
{
    if (cmd == BCMW_C_GET_VAR && !strcmp(name, "cap")) {
        const char *c = M.fwsup ? "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu idsup sup_wpa" :
                                  "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu";
        memcpy(out, c, strlen(c) + 1);
        *rlen = (uint16_t)(strlen(c) + 1);
        return 1;
    }
    if (cmd == BCMW_C_SET_VAR) {
        uint32_t v = dlen >= 4 ? (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24 : 0;
        if (!strcmp(name, "wsec"))
            return A.wsec = v, 1;
        if (!strcmp(name, "wpa_auth"))
            return A.wpa_auth = v, 1;
        if (!strcmp(name, "sup_wpa"))
            return A.sup_wpa = v, 1;
        if (!strcmp(name, "wpaie")) {
            A.wpaie_len = dlen < sizeof(A.wpaie) ? dlen : sizeof(A.wpaie);
            memcpy(A.wpaie, data, A.wpaie_len);
            return 1;
        }
        if (!strcmp(name, "wsec_key")) {
            uint32_t idx = (uint32_t)data[0], len = (uint32_t)data[4];
            if (dlen < 164 || le32(data + 112) != 4)
                M.ap_errors |= 8;
            /* brcmf_cfg80211_add_key: the pairwise key (with the peer's address) flags 0, the group key primary (2) */
            else if (idx == 0 && len == 16 && !memcmp(data + 8, A.ptk + 32, 16) && !memcmp(data + 156, ap_bssid, 6) &&
                     le32(data + 116) == 0)
                M.keys_ok |= 1;
            else if (idx == 1 && len == 16 && !memcmp(data + 8, gtk, 16) && le32(data + 116) == 2)
                M.keys_ok |= 2;
            else
                M.ap_errors |= 16;      /* a key that is not ours */
            return 1;
        }
        return 0;
    }
    if (cmd == BCMW_C_SET_INFRA || cmd == BCMW_C_SET_AUTH || cmd == 52)
        return 1;
    if (cmd == BCMW_C_SET_WSEC_PMK) {
        if (dlen < 36 || le16(data) != 32 || memcmp(data + 4, A.pmk, 32))
            M.ap_errors |= 32;          /* not the network's PMK */
        else
            M.pmk_ok = 1;
        return 1;
    }
    if (cmd == BCMW_C_SET_SSID) {
        if (dlen < 42 || le32(data) != strlen(ap_ssid) || memcmp(data + 4, ap_ssid, strlen(ap_ssid)) ||
            memcmp(data + 36, ap_bssid, 6))
            *status = (uint16_t)-1;
        else if (A.wsec != 4 || A.wpa_auth != 0x80 || (A.sup_wpa != (M.fwsup ? 1u : 0u)))
            M.ap_errors |= 64;          /* the security settings */
        else
            A.joined = 1;
        return 1;
    }
    return 0;
}

/* after the ioctl's answer: what the association does */
void model_ap_after(uint32_t cmd)
{
    uint8_t sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    if (cmd != BCMW_C_SET_SSID || !A.joined)
        return;
    A.joined = 0;
    memcpy(A.sta, M.mac_set[0] | M.mac_set[1] ? M.mac_set : sta, 6);
    model_event(BCMW_E_SET_SSID, 0, 0, 0, 0, 0);
    model_event(BCMW_E_LINK, 0, 1, 0, 0, 0);
    if (M.fwsup)
        model_event(46, M.pmk_ok ? 6 : 4, 0, M.pmk_ok ? 0 : 14, 0, 0);    /* keyed, or a MIC failure */
    else
        message1();
}
