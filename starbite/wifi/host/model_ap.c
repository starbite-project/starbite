/* SPDX-License-Identifier: GPL-2.0-only
 * The model's access point (host/model.c): the network "test-ap" with WPA2-PSK, CCMP, passphrase
 * "correct horse"; its BSSID 02:11:22:33:44:5a. It answers a directed escan with an RSN element, takes the
 * station's security settings, and on SET_SSID associates (SET_SSID, LINK events). With the firmware's
 * supplicant (model M.fwsup: "cap" lists idsup) it checks the PMK and sends PSK_SUP keyed; with the host's
 * it plays the authenticator of the four-way handshake: message 1, checks message 2's MIC and RSN element,
 * message 3 with the GTK wrapped (its own AES-128 encryption and RFC 3394 key wrap, checked against the RFC's
 * example), checks message 4, and then the keys the host installs must be its TK and GTK.
 *
 * WPA3 (M.ap_sec): the RSN element offers SAE (only, or with PSK: a transition network; hash-to-element only with an
 * RSNXE and the membership selector), "cap" lists sae_ext / sae / mfp as asked. External SAE as brcmfmac's cyw
 * firmware does it: on SET_SSID with wpa_auth 0x40000, auth 3, mfp 2 and the events 187.. on (event_msgs_ext) it sends
 * EXT_AUTH_REQ; the station's frames come by "mgmt_frame" (answered by MGMT_FRAME_TXSTATUS), the AP's go back as
 * EXT_AUTH_FRAME_RX (16 bytes of brcmf_rx_mgmt_data, the 802.11 header, the body); the AP's own SAE is sae.c's
 * (model_sae, set by the test: the preview of Starbite OS links no SAE); "auth_status" with the PMKID it made lets
 * it associate, and the four-way handshake is AKM 8's (KDF-SHA256, AES-CMAC, key descriptor version 0, the PMKID
 * KDE in message 1, the beacon's RSN element, the GTK and an IGTK in message 3). In-firmware SAE: "sae_password"
 * checked, then PSK_SUP keyed.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../bcmw_priv.h"
#include "../wpa.h"
#include "../sae.h"
#include "model.h"

const struct bcmw_sae_ops *model_sae;
static const uint8_t igtk[16] = { 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f };
static const uint8_t igtk_ipn[6] = { 0x07, 0x06, 0x05, 0x04, 0x03, 0x02 };

static const uint8_t ap_bssid[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x5a };
static const char ap_ssid[] = "test-ap";
static const char ap_pass[] = "correct horse";
static const uint8_t gtk[16] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };

static struct {
    uint8_t pmk[32], anonce[32], snonce[32], sta[6], ptk[48], wpaie[64], gtk2[16];
    unsigned wpaie_len, wsec, wpa_auth, sup_wpa, step, pm;
    int joined;
    /* WPA3 */
    unsigned auth, mfp, akm;            /* SET_AUTH, "mfp", the AKM of the association (2 or 8) */
    int ext_events, sae_wait, sae_accepted, token_sent, fw_sae, sae_failed;
    uint8_t sae_pw[130];
    struct bcmw_sae sae;
    uint8_t beacon_rsn[32], token[32];
    unsigned beacon_rsn_len;
    struct { uint32_t type, status; uint8_t data[600]; unsigned len; } ev[4];   /* events after the ioctl's answer */
    unsigned nev;
    int message1_due;
} A;

static uint64_t ap_rng = 0x2545f4914f6cdd1dull;
static int ap_random(void *ctx, uint8_t *out, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++) {
        ap_rng ^= ap_rng << 13, ap_rng ^= ap_rng >> 7, ap_rng ^= ap_rng << 17;
        out[i] = (uint8_t)(ap_rng >> 32);
    }
    return 0;
}

static void ap_event(uint32_t type, uint32_t status, const uint8_t *data, unsigned len)
{
    if (A.nev == 4 || len > sizeof(A.ev[0].data)) {
        M.ap_errors |= 0x10000;
        return;
    }
    A.ev[A.nev].type = type, A.ev[A.nev].status = status, A.ev[A.nev].len = len;
    if (len)
        memcpy(A.ev[A.nev].data, data, len);
    A.nev++;
}

/* an Authentication frame from the AP to the station, as EXT_AUTH_FRAME_RX carries it */
static void ap_auth_frame(const uint8_t *body, unsigned len)
{
    uint8_t d[16 + 24 + 400];
    memset(d, 0, 40);
    d[1] = 1;                           /* brcmf_rx_mgmt_data: version 1, big-endian */
    d[2] = 0xd0, d[3] = 0x24;           /* chanspec */
    d[16] = 0xb0;                       /* frame control: Authentication */
    memcpy(d + 16 + 4, A.sta, 6);
    memcpy(d + 16 + 10, ap_bssid, 6);
    memcpy(d + 16 + 16, ap_bssid, 6);
    memcpy(d + 40, body, len);
    ap_event(BCMW_E_EXT_AUTH_FRAME_RX, 0, d, 40 + len);
}

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
    if (A.akm == 8) {
        bcmw_aes_cmac(A.ptk, p, l, 3, out);
        return;
    }
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
    if (dlen)
        memcpy(m + 99, data, dlen);
    return len;
}

static void message1(void)
{
    uint8_t m[160], kde[22] = { 0xdd, 20, 0x00, 0x0f, 0xac, 4 };
    for (unsigned i = 0; i < 32; i++)
        A.anonce[i] = (uint8_t)(0xa0 + i);
    if (A.akm == 8) {                   /* version 0, the PMKID KDE */
        memcpy(kde + 6, A.sae.pmkid, 16);
        send_eapol(m, key_msg(m, 0x0088, 1, kde, sizeof(kde)));
    } else {
        send_eapol(m, key_msg(m, 0x008a, 1, 0, 0));
    }
    A.step = 1;
}

static void message3(void)
{
    uint8_t plain[128], wrapped[136], m[256];
    unsigned n = 0, len;
    if (A.akm == 8) {                   /* the beacon's RSN element (and RSNXE) */
        memcpy(plain, A.beacon_rsn, A.beacon_rsn_len);
        n = A.beacon_rsn_len;
    } else {
        memcpy(plain, A.wpaie, A.wpaie_len);
        n = A.wpaie_len;
    }
    plain[n++] = 0xdd, plain[n++] = 22, plain[n++] = 0x00, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 1;
    plain[n++] = 1, plain[n++] = 0;     /* key index 1 */
    memcpy(plain + n, gtk, 16);
    n += 16;
    if (A.mfp) {                        /* the IGTK KDE: key id 4, IPN, the key */
        plain[n++] = 0xdd, plain[n++] = 28, plain[n++] = 0x00, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 9;
        plain[n++] = 4, plain[n++] = 0;
        memcpy(plain + n, igtk_ipn, 6);
        memcpy(plain + n + 6, igtk, 16);
        n += 22;
    }
    plain[n++] = 0xdd;                  /* padding to a multiple of 8 */
    while (n % 8)
        plain[n++] = 0;
    wrap(A.ptk + 16, plain, n, wrapped);
    len = key_msg(m, A.akm == 8 ? 0x13c8 : 0x13ca, 2, wrapped, n + 8);
    mic(m, len, m + 81);
    send_eapol(m, len);
    A.step = 3;
}

/* after the join: a group-key renewal (key index 2), and a message from another address first */
void model_ap_rekey(void)
{
    static const uint8_t gtk2[16] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f };
    uint8_t plain[32], wrapped[40], m[200], f[300];
    unsigned n = 0, len;
    plain[n++] = 0xdd, plain[n++] = 22, plain[n++] = 0x00, plain[n++] = 0x0f, plain[n++] = 0xac, plain[n++] = 1;
    plain[n++] = 2, plain[n++] = 0;
    memcpy(plain + n, gtk2, 16);
    n += 16;
    wrap(A.ptk + 16, plain, n, wrapped);
    len = key_msg(m, A.akm == 8 ? 0x1380 : 0x1382, 3, wrapped, n + 8);
    mic(m, len, m + 81);
    /* the same frame from a stranger first: must be ignored */
    memcpy(f, A.sta, 6);
    memcpy(f + 6, "\x02\x99\x99\x99\x99\x99", 6);
    f[12] = 0x88, f[13] = 0x8e;
    memcpy(f + 14, m, len);
    model_rx(f, 14 + len);
    send_eapol(m, len);
    A.step = 5;
    memcpy(A.gtk2, gtk2, 16);
}

/* ---- the open network's DHCP server (M.open_ap: Starbite OS's host preview) ------------------------- */
static void dhcp_server(const uint8_t *f, unsigned len)
{
    const uint8_t *ip = f + 14, *udp, *b;
    unsigned ihl, type = 0;
    if (len >= 14 + 20 + 8 && len <= 14 + 1500 && ip[9] == 1 && ip[20] == 8) {     /* an echo request: answered */
        uint8_t r[14 + 1500];
        memcpy(r, f + 6, 6);
        memcpy(r + 6, f, 6);
        memcpy(r + 12, f + 12, len - 12);
        memcpy(r + 14 + 12, f + 14 + 16, 4);    /* source and destination swapped */
        memcpy(r + 14 + 16, f + 14 + 12, 4);
        r[14 + 20] = 0;                         /* echo reply: the checksum moves by 0x0800 */
        unsigned c = (unsigned)r[14 + 22] << 8 | r[14 + 23];
        c += 0x0800;
        c = (c & 0xffff) + (c >> 16);
        r[14 + 22] = (uint8_t)(c >> 8), r[14 + 23] = (uint8_t)c;
        model_rx(r, len);
        return;
    }
    if (len >= 14 + 20 + 8 + 12 && ip[9] == 17) {
        const uint8_t *u = ip + (ip[0] & 15) * 4u;
        unsigned dport = (unsigned)u[2] << 8 | u[3], ulen = (unsigned)u[4] << 8 | u[5];
        if ((dport == 53 || dport == 123) && ulen >= 8 + 12 && u + ulen <= f + len && ulen <= 600) {
            /* DNS: any A question answered with 192.0.2.123; NTP: a server answer at 2026-11-08 01:30:00 UTC */
            uint8_t r[14 + 20 + 8 + 620];
            unsigned pl = ulen - 8;
            uint8_t *rp = r + 14 + 20 + 8;
            memset(r, 0, sizeof(r));
            memcpy(r, f + 6, 6);
            memcpy(r + 6, ap_bssid, 6);
            r[12] = 0x08;
            if (dport == 53) {
                memcpy(rp, u + 8, pl);
                rp[2] = 0x81, rp[3] = 0x80;     /* a response, recursion available, no error */
                rp[7] = 1;                      /* one answer */
                uint8_t *a = rp + pl;
                const uint8_t ans[16] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 123 };
                memcpy(a, ans, sizeof(ans));
                pl += sizeof(ans);
            } else {
                if (pl < 48)
                    return;
                pl = 48;
                rp[0] = 0x24;                   /* version 4, server */
                rp[1] = 2;
                memcpy(rp + 24, u + 8 + 40, 8);     /* the originate time: the client's transmit time (its nonce) */
                uint32_t t = 1794101400U + 2208988800U;
                rp[40] = (uint8_t)(t >> 24), rp[41] = (uint8_t)(t >> 16), rp[42] = (uint8_t)(t >> 8), rp[43] = (uint8_t)t;
            }
            uint8_t *rip = r + 14, *rudp = rip + 20;
            rip[0] = 0x45, rip[8] = 64, rip[9] = 17;
            memcpy(rip + 12, ip + 16, 4);       /* from where it was sent */
            memcpy(rip + 16, ip + 12, 4);
            rudp[0] = u[2], rudp[1] = u[3], rudp[2] = u[0], rudp[3] = u[1];
            unsigned ul = 8 + pl, tot = 20 + ul;
            rudp[4] = (uint8_t)(ul >> 8), rudp[5] = (uint8_t)ul;
            rip[2] = (uint8_t)(tot >> 8), rip[3] = (uint8_t)tot;
            uint32_t s = 0;
            for (unsigned i = 0; i < 20; i += 2)
                s += (uint32_t)rip[i] << 8 | rip[i + 1];
            while (s >> 16)
                s = (s & 0xffff) + (s >> 16);
            s = ~s & 0xffff;
            rip[10] = (uint8_t)(s >> 8), rip[11] = (uint8_t)s;
            model_rx(r, 14 + tot);
            return;
        }
    }
    if (len < 14 + 20 + 8 + 240 || ip[9] != 17)
        return;
    ihl = (ip[0] & 15) * 4u;
    udp = ip + ihl;
    if (udp + 8 + 240 > f + len || udp[2] != 0 || udp[3] != 67)
        return;
    b = udp + 8;
    if (b[0] != 1 || b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99)
        return;
    for (const uint8_t *o = b + 240; o + 2 <= f + len && o[0] != 255;) {
        if (o[0] == 0) {
            o++;
            continue;
        }
        if (o[0] == 53 && o[1] == 1)
            type = o[2];
        o += 2 + o[1];
    }
    if (type != 1 && type != 3)
        return;
    uint8_t r[14 + 20 + 8 + 300];
    memset(r, 0, sizeof(r));
    memcpy(r, f + 6, 6);                    /* to the station */
    memcpy(r + 6, ap_bssid, 6);
    r[12] = 0x08;
    uint8_t *rip = r + 14, *rudp = rip + 20, *rb = rudp + 8;
    rip[0] = 0x45, rip[8] = 64, rip[9] = 17;
    rip[12] = 192, rip[13] = 168, rip[14] = 4, rip[15] = 1;
    memset(rip + 16, 0xff, 4);
    rudp[1] = 67, rudp[3] = 68;
    rb[0] = 2, rb[1] = 1, rb[2] = 6;
    memcpy(rb + 4, b + 4, 4);               /* xid */
    rb[16] = 192, rb[17] = 168, rb[18] = 4, rb[19] = 23;    /* yiaddr */
    memcpy(rb + 28, b + 28, 16);            /* chaddr */
    rb[236] = 99, rb[237] = 130, rb[238] = 83, rb[239] = 99;
    uint8_t *o = rb + 240;
    *o++ = 53, *o++ = 1, *o++ = (uint8_t)(type == 1 ? 2 : 5);
    *o++ = 54, *o++ = 4, *o++ = 192, *o++ = 168, *o++ = 4, *o++ = 1;
    *o++ = 51, *o++ = 4, *o++ = 0, *o++ = 0, *o++ = 0x0e, *o++ = 0x10;     /* 3600 s */
    *o++ = 1, *o++ = 4, *o++ = 255, *o++ = 255, *o++ = 255, *o++ = 0;
    *o++ = 3, *o++ = 4, *o++ = 192, *o++ = 168, *o++ = 4, *o++ = 1;
    *o++ = 6, *o++ = 4, *o++ = 192, *o++ = 168, *o++ = 4, *o++ = 1;
    *o++ = 255;
    unsigned blen = (unsigned)(o - rb), ulen = 8 + blen, tot = 20 + ulen;
    rudp[4] = (uint8_t)(ulen >> 8), rudp[5] = (uint8_t)ulen;
    rip[2] = (uint8_t)(tot >> 8), rip[3] = (uint8_t)tot;
    uint32_t s = 0;
    for (unsigned i = 0; i < 20; i += 2)
        s += (uint32_t)rip[i] << 8 | rip[i + 1];
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    s = ~s & 0xffff;
    rip[10] = (uint8_t)(s >> 8), rip[11] = (uint8_t)s;
    if (type == 1)
        M.dhcp_offers++;
    else
        M.dhcp_acks++;
    model_rx(r, 14 + tot);
}

/* a frame the station sent (the model's tx path) */
void model_ap_tx(const uint8_t *f, unsigned len)
{
    const uint8_t *m = f + 14;
    unsigned mlen = len - 14, info;
    uint8_t want[16];

    M.tx_seen++;
    if ((M.open_ap || M.echo_sync) && len >= 14 + 28 && f[12] == 0x08 && f[13] == 0x06 && m[7] == 1 &&
        m[24] == 192 && m[25] == 168 && m[26] == 4 && m[27] == 1) {     /* ARP: who has the router? */
        uint8_t r[14 + 28];
        memcpy(r, f + 6, 6);
        memcpy(r + 6, ap_bssid, 6);
        r[12] = 0x08, r[13] = 0x06;
        memcpy(r + 14, m, 6);
        r[14 + 6] = 0, r[14 + 7] = 2;           /* a reply */
        memcpy(r + 14 + 8, ap_bssid, 6);
        memcpy(r + 14 + 14, m + 24, 4);
        memcpy(r + 14 + 18, m + 8, 10);         /* to the asker's address and IP */
        model_rx(r, sizeof(r));
        return;
    }
    if (len < 14 || f[12] != 0x88 || f[13] != 0x8e) {
        if (len >= 14 && f[12] == 0x08 && f[13] == 0x00) {
            M.tx_ip++;
            if ((M.open_ap || M.echo_sync) && !getenv("SBOS_HOST_NO_DHCP"))   /* 1.2-alpha25: a silent DHCP server */
                dhcp_server(f, len);
        }
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
        if (A.akm == 8)
            bcmw_kdf_sha256(A.pmk, 32, "Pairwise key expansion", data, 76, A.ptk, 48);
        else
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
        return;
    }
    if (A.step == 5 && (info & 0x0308) == 0x0300) {     /* the group message's answer: MIC, secure, not pairwise */
        mic(m, mlen, want);
        if (memcmp(want, m + 81, 16)) {
            M.ap_errors |= 256;
            return;
        }
        M.rekey_answered++;
    }
}

/* ---- ioctls --------------------------------------------------------------------------------------- */

void model_ap_reset(void)
{
    memset(&A, 0, sizeof(A));
    bcmw_wpa_pmk((const unsigned char *)ap_pass, (unsigned)strlen(ap_pass), (const unsigned char *)ap_ssid,
                 (unsigned)strlen(ap_ssid), A.pmk);
}

/* the phone firmware's defaults (Starbite OS 0.6 tour): its own supplicant on, power save on */
void model_ap_real_defaults(void)
{
    M.real_defaults = 1;
    A.sup_wpa = 1;
    A.pm = 2;
}

/* the AP's RSN element (and RSNXE) for its security (M.ap_sec) */
static unsigned beacon_rsn(uint8_t *out)
{
    static const uint8_t psk[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    static const uint8_t sae[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 8, 0xc0, 0 };
    static const uint8_t both[26] = { 48, 24, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 2, 0, 0x00, 0x0f, 0xac, 2,
                                      0x00, 0x0f, 0xac, 8, 0x80, 0 };
    switch (M.ap_sec) {
    case 1:
        memcpy(out, sae, 22);
        return 22;
    case 2:
        memcpy(out, both, 26);
        return 26;
    case 3:
        memcpy(out, sae, 22);
        out[22] = 244, out[23] = 1, out[24] = 0x20;     /* RSNXE: SAE hash-to-element */
        return 25;
    case 4:                             /* WPA2-PSK with management frame protection capable */
        memcpy(out, psk, 22);
        out[20] = 0x80;
        return 22;
    default:
        memcpy(out, psk, 22);
        return 22;
    }
}

/* a directed escan's result: one BSS of this name with an RSN element (WPA2-PSK, CCMP; or WPA3: M.ap_sec) */
unsigned model_ap_scan_result(uint8_t *r, unsigned cap, uint16_t sync)
{
    uint8_t rsn[32], ies[64];
    unsigned rl = beacon_rsn(rsn), il = 0, blen, total;
    ies[il++] = 0, ies[il++] = 7;       /* the SSID element */
    memcpy(ies + il, ap_ssid, 7);
    il += 7;
    if (M.ap_sec == 3)                  /* supported rates: 6 Mb/s basic, the membership selector 123 (H2E only) */
        ies[il++] = 1, ies[il++] = 2, ies[il++] = 0x8c, ies[il++] = 0x80 | 123;
    if (!M.open_ap) {
        memcpy(ies + il, rsn, rl);
        il += rl;
    }
    blen = 128 + il, total = 12 + blen;
    if (total > cap)
        return 0;
    memset(r, 0, total);
    put32(r, total);
    put32(r + 4, 109);
    put16(r + 8, sync);
    put16(r + 10, 1);
    uint8_t *b = r + 12;
    put32(b, 109);
    put32(b + 4, blen);
    memcpy(b + 8, ap_bssid, 6);
    put16(b + 16, M.open_ap ? 0x0001 : 0x0011);     /* ESS (, privacy) */
    b[18] = (uint8_t)strlen(ap_ssid);
    memcpy(b + 19, ap_ssid, strlen(ap_ssid));
    put16(b + 72, 0xd024);              /* a 5 GHz chanspec */
    put16(b + 78, (uint16_t)-48);
    put16(b + 116, 128);
    put32(b + 120, il);
    memcpy(b + 128, ies, il);
    return total;
}

/* the station's Authentication frame (by "mgmt_frame"), the AP's SAE side */
static void ap_sae(const uint8_t *f, unsigned len)
{
    uint8_t out[400], pt[64];
    unsigned n;
    int h2e;
    if (!model_sae || len < 6 || le16(f) != 3)
        return;
    if (le16(f + 2) == 1) {
        M.sae_commits++;
        if (M.sae_mute)
            return;
        h2e = le16(f + 4) == BCMW_SAE_ST_H2E;
        M.sae_h2e_seen += (unsigned)h2e;
        if (M.ap_sec == 3 && !h2e) {
            M.ap_errors |= 0x20000;     /* hash-to-element only: a hunting-and-pecking commit */
            return;
        }
        if (M.sae_token && !A.token_sent) {
            /* status 76: the group, the token (in its container with H2E) */
            n = 0;
            out[n++] = 3, out[n++] = 0, out[n++] = 1, out[n++] = 0, out[n++] = 76, out[n++] = 0, out[n++] = 19, out[n++] = 0;
            if (h2e)
                out[n++] = 255, out[n++] = 33, out[n++] = 93;
            for (unsigned i = 0; i < 32; i++)
                A.token[i] = (uint8_t)(0x30 + i);
            memcpy(out + n, A.token, 32);
            n += 32;
            A.token_sent = 1;
            M.sae_tokens_sent++;
            ap_auth_frame(out, n);
            return;
        }
        if (A.token_sent && (h2e ? len < 35 || memcmp(f + len - 32, A.token, 32) || f[len - 35] != 255
                                 : len != 8 + 32 + 96 || memcmp(f + 8, A.token, 32)))
            M.ap_errors |= 0x40000;     /* the token is not where it belongs */
        A.sae.random = ap_random;
        if (h2e)
            model_sae->pt((const uint8_t *)ap_ssid, 7, (const uint8_t *)ap_pass, (unsigned)strlen(ap_pass), NULL, 0, pt);
        if (model_sae->prepare(&A.sae, ap_bssid, A.sta, (const uint8_t *)ap_pass, (unsigned)strlen(ap_pass),
                               h2e ? pt : NULL) != 0 ||
            model_sae->peer_commit(&A.sae, f, len) != 0) {
            M.ap_errors |= 0x80000;     /* the station's commit */
            return;
        }
        n = model_sae->commit_frame(&A.sae, out, sizeof(out));
        ap_auth_frame(out, n);
        n = model_sae->confirm_frame(&A.sae, out, sizeof(out));
        ap_auth_frame(out, n);
        return;
    }
    if (le16(f + 2) == 2) {
        M.sae_confirms++;
        if (model_sae->peer_confirm(&A.sae, f, len) == 0)
            A.sae_accepted = 1;
        else
            M.sae_confirm_bad++;
    }
}

/* 1 if the ioctl is the access point's to answer */
int model_ap_ioctl(uint32_t cmd, const char *name, const uint8_t *data, uint32_t dlen, uint8_t *out, uint16_t *rlen,
                   uint16_t *status)
{
    if (cmd == BCMW_C_GET_VAR && !strcmp(name, "cap")) {
        char c[200];
        snprintf(c, sizeof(c), "%s%s%s%s", M.fwsup || M.real_defaults ?
                 "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu idsup sup_wpa" :
                 "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu", M.cap_sae ? " sae" : "",
                 M.cap_sae_ext ? " sae_ext" : "", M.cap_mfp ? " mfp" : "");
        memcpy(out, c, strlen(c) + 1);
        *rlen = (uint16_t)(strlen(c) + 1);
        return 1;
    }
    if (cmd == BCMW_C_GET_VAR && !strcmp(name, "mfp")) {
        if (!M.cap_mfp)
            return *status = (uint16_t)-23, 1;
        out[0] = (uint8_t)A.mfp, out[1] = out[2] = out[3] = 0;
        *rlen = 4;
        return 1;
    }
    if (cmd == BCMW_C_SET_VAR) {
        uint32_t v = dlen >= 4 ? (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24 : 0;
        if (!strcmp(name, "wsec"))
            return A.wsec = v, 1;
        if (!strcmp(name, "wpa_auth"))
            return A.wpa_auth = v, 1;
        if (!strcmp(name, "sup_wpa")) {
            if (!M.fwsup && !M.real_defaults)   /* a firmware without its own supplicant does not know it */
                return *status = (uint16_t)-23, 1;
            return A.sup_wpa = v, 1;
        }
        if (!strcmp(name, "wpaie")) {
            A.wpaie_len = dlen < sizeof(A.wpaie) ? dlen : sizeof(A.wpaie);
            memcpy(A.wpaie, data, A.wpaie_len);
            return 1;
        }
        if (!strcmp(name, "mfp")) {
            if (!M.cap_mfp && !M.cap_sae_ext)
                return *status = (uint16_t)-23, 1;
            M.mfp_set++;
            return A.mfp = v, 1;
        }
        if (!strcmp(name, "event_msgs_ext")) {
            /* brcmf_eventmsgs_ext: version 1, SET_MASK (3), the length, the mask */
            M.events_ext++;
            if (dlen >= 4 + 24 && data[0] == 1 && data[1] == 3 && data[2] >= 24 && (data[4 + 187 / 8] & 0x38) == 0x38 &&
                (data[4 + 69 / 8] & (1u << (69 % 8))))
                A.ext_events = 1;       /* 187..189 on, and the escan event kept */
            return M.cap_sae_ext ? 1 : (*status = (uint16_t)-23, 1);
        }
        if (!strcmp(name, "sae_password")) {
            if (!M.cap_sae || dlen < 2 + 128)
                return *status = (uint16_t)-23, 1;
            memcpy(A.sae_pw, data, sizeof(A.sae_pw));
            return 1;
        }
        if (!strcmp(name, "mgmt_frame")) {
            /* brcmf_mf_params_le: version, dwell, len, frame control, channel, da, bssid, packet id, the body */
            unsigned n = dlen >= 30 ? le16(data + 8) : 0;
            if (!M.cap_sae_ext)
                return *status = (uint16_t)-23, 1;
            M.mgmt_frames++;
            if (dlen < 30 || n + 30 > dlen || le16(data + 10) != 0x00b0 || le16(data + 12) != 36 ||
                memcmp(data + 14, ap_bssid, 6) || memcmp(data + 20, ap_bssid, 6)) {
                M.ap_errors |= 0x100000;
                return 1;
            }
            ap_event(BCMW_E_MGMT_FRAME_TXSTATUS, 0, data + 26, 4);
            ap_sae(data + 30, n);
            return 1;
        }
        if (!strcmp(name, "auth_status")) {
            /* brcmf_auth_req_status_le: flags, the peer, the name's length and the name, the PMKID */
            if (!M.cap_sae_ext)
                return *status = (uint16_t)-23, 1;
            if (dlen < 60 || memcmp(data + 2, ap_bssid, 6) || le32(data + 8) != strlen(ap_ssid) ||
                memcmp(data + 12, ap_ssid, strlen(ap_ssid))) {
                M.ap_errors |= 0x200000;
                return 1;
            }
            if (le16(data) == 4) {
                if (!A.sae_accepted || memcmp(data + 44, A.sae.pmkid, 16)) {
                    M.ap_errors |= 0x400000;    /* success said without our confirm verified, or another PMKID */
                    return 1;
                }
                M.sae_ok++;
                memcpy(A.pmk, A.sae.pmk, 32);
                A.akm = 8;
                ap_event(BCMW_E_SET_SSID, 0, 0, 0);
                ap_event(BCMW_E_LINK, 0, 0, 0);     /* (flags 1: model_ap_after) */
                A.message1_due = 1;
            } else {
                M.sae_fail++;
                A.sae_failed = 1;
                ap_event(BCMW_E_SET_SSID, 1, 0, 0);
            }
            return 1;
        }
        if (!strcmp(name, "wsec_key")) {
            uint32_t idx = (uint32_t)data[0], len = (uint32_t)data[4];
            if (dlen >= 164 && (idx == 4 || idx == 5)) {
                /* the IGTK: BIP (bcmdhd's algorithm 12), its IPN as the receive counter (hi: 5..2, lo: 1..0) */
                if (idx == 4 && len == 16 && !memcmp(data + 8, igtk, 16) && le32(data + 112) == 12)
                    M.keys_ok |= 8;
                else
                    M.ap_errors |= 0x800000;
                if (le32(data + 140) == ((uint32_t)igtk_ipn[5] << 24 | (uint32_t)igtk_ipn[4] << 16 |
                                         (uint32_t)igtk_ipn[3] << 8 | igtk_ipn[2]) &&
                    le16(data + 144) == (igtk_ipn[1] << 8 | igtk_ipn[0]))
                    M.igtk_rxiv_ok = 1;
                return 1;
            }
            if (dlen < 164 || le32(data + 112) != 4)
                M.ap_errors |= 8;
            /* brcmf_cfg80211_add_key: the pairwise key (with the peer's address) flags 0, the group key primary (2) */
            else if (idx == 0 && len == 16 && !memcmp(data + 8, A.ptk + 32, 16) && !memcmp(data + 156, ap_bssid, 6) &&
                     le32(data + 116) == 0)
                M.keys_ok |= 1;
            else if (idx == 1 && len == 16 && !memcmp(data + 8, gtk, 16) && le32(data + 116) == 2)
                M.keys_ok |= 2;
            else if (idx == 2 && len == 16 && !memcmp(data + 8, A.gtk2, 16) && le32(data + 116) == 2)
                M.keys_ok |= 4;
            else
                M.ap_errors |= 16;      /* a key that is not ours */
            return 1;
        }
        return 0;
    }
    if (cmd == BCMW_C_SET_INFRA)
        return 1;
    if (cmd == BCMW_C_SET_AUTH)
        return A.auth = dlen >= 4 ? data[0] : 0, 1;
    if (cmd == BCMW_C_GET_PM) {
        out[0] = (uint8_t)A.pm, out[1] = out[2] = out[3] = 0;
        *rlen = 4;
        return 1;
    }
    if (cmd == BCMW_C_SET_PM) {
        A.pm = dlen >= 4 ? data[0] : A.pm;
        return 1;
    }
    if (cmd == 52) {                        /* DISASSOC: brcmf_scb_val_le, 12 bytes */
        if (dlen != 12)
            M.ap_errors |= 128;
        M.disassoc++;
        return 1;
    }
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
        else if (A.wpa_auth == 0x40000) {
            /* WPA3: SAE, auth 3, mfp required; the firmware's own SAE (sup_wpa 1, the password) or the host's */
            if (!M.ap_sec || A.wsec != 4 || A.auth != 3 || A.mfp != 2 || (A.sup_wpa ? !M.cap_sae || A.wpaie_len : !M.cap_sae_ext))
                M.ap_errors |= 64;
            else {
                A.joined = 2;
                A.fw_sae = A.sup_wpa == 1;
            }
        } else if (M.open_ap ? (A.wsec != 0 || A.wpa_auth != 0) :
                             (A.wsec != 4 || A.wpa_auth != 0x80 || M.ap_sec == 1 || M.ap_sec == 3 || A.auth != 0 ||
                              (!M.real_defaults && A.sup_wpa != (M.fwsup ? 1u : 0u))))
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
    if (cmd == BCMW_C_SET_VAR && A.nev) {
        /* the events an iovar made (mgmt_frame's status and the AP's frames, auth_status's association) */
        for (unsigned i = 0; i < A.nev; i++) {
            if (A.ev[i].type == BCMW_E_EXT_AUTH_FRAME_RX)
                model_event_addr(A.ev[i].type, 0, 0, 0, ap_bssid, A.ev[i].data, A.ev[i].len);
            else if (A.ev[i].type != BCMW_E_MGMT_FRAME_TXSTATUS || A.ext_events)
                model_event(A.ev[i].type, A.ev[i].status, A.ev[i].type == BCMW_E_LINK ? 1 : 0, 0, A.ev[i].data,
                            A.ev[i].len);
        }
        A.nev = 0;
        if (A.message1_due) {
            A.message1_due = 0;
            M.akm_joined = 8;
            message1();
        }
        return;
    }
    if (cmd != BCMW_C_SET_SSID || !A.joined)
        return;
    memcpy(A.sta, M.mac_set[0] | M.mac_set[1] ? M.mac_set : sta, 6);
    A.beacon_rsn_len = beacon_rsn(A.beacon_rsn);
    if (A.joined == 2) {
        A.joined = 0;
        if (A.fw_sae) {
            /* the firmware's own SAE and handshake: the password it was given */
            int ok = le16(A.sae_pw) == strlen(ap_pass) && !memcmp(A.sae_pw + 2, ap_pass, strlen(ap_pass));
            if (ok) {
                M.fw_sae_ok = 1;
                M.akm_joined = 8;
                model_event(BCMW_E_SET_SSID, 0, 0, 0, 0, 0);
                model_event(BCMW_E_LINK, 0, 1, 0, 0, 0);
                model_event(46, 6, 0, 0, 0, 0);
            } else {
                model_event(BCMW_E_SET_SSID, 1, 0, 0, 0, 0);
            }
            return;
        }
        /* the host's SAE: the firmware asks for it (brcmf_auth_req_status_le), if the events are on */
        if (A.ext_events && !M.no_ext_auth) {
            uint8_t r[60];
            memset(r, 0, sizeof(r));
            r[0] = 1;                   /* BRCMF_EXTAUTH_START */
            memcpy(r + 2, ap_bssid, 6);
            r[8] = (uint8_t)strlen(ap_ssid);
            memcpy(r + 12, ap_ssid, strlen(ap_ssid));
            model_event(BCMW_E_EXT_AUTH_REQ, 0, 0, 0, r, sizeof(r));
        }
        return;
    }
    A.joined = 0;
    A.akm = 2;
    M.akm_joined = 2;
    model_event(BCMW_E_SET_SSID, 0, 0, 0, 0, 0);
    model_event(BCMW_E_LINK, 0, 1, 0, 0, 0);
    if (M.open_ap)
        return;                         /* open: associated is joined */
    if (M.fwsup)
        model_event(46, M.pmk_ok ? 6 : 4, 0, M.pmk_ok ? 0 : 14, 0, 0);    /* keyed, or a MIC failure */
    else if (M.real_defaults && (A.sup_wpa || A.pm))
        M.m1_withheld++;                /* the firmware's supplicant keeps it, or the AP holds it for a dozing station */
    else
        message1();
}
