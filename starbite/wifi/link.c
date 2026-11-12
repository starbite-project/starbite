/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: the data path and joining a network.
 *
 * Data: one tx flow ring (a station has one per priority in brcmfmac; everything here goes as best effort,
 * fifo 1), created on the control ring (FLOW_RING_CREATE, answered by FLOW_RING_CREATE_CMPLT), 512 items of
 * 48 bytes; each frame is a TX_POST item with the Ethernet header in the item and the payload by address
 * (brcmfmac msgbuf.c brcmf_msgbuf_txflow), answered by TX_STATUS (request id - 1 = our buffer). Received frames
 * come on the rx completion ring (msgbuf.c) to os->rx, EAPOL frames to the handshake while joining.
 *
 * Joining (brcmfmac cfg80211.c brcmf_cfg80211_connect and add_key, with its own constants): a directed escan
 * finds the network (the strongest BSS of that name whose RSN element offers WPA2-PSK with CCMP), then
 *  - infrastructure, open-system authentication, wsec AES (and TKIP when the group cipher is TKIP), wpa_auth
 *    WPA2-PSK;
 *  - by default the host's supplicant (the way an earlier port joined on this phone): our RSN element as "wpaie" (sup_wpa
 *    is not touched: brcmfmac and bcmdhd set it only for a firmware supplicant), the four-way handshake by wpa.c on the
 *    EAPOL frames, message 2 and 4 sent as data, then the pairwise and group keys by "wsec_key"
 *    (Samsung's firmware is built without BCMSUP_4WAY_HANDSHAKE, an earlier port found); group-key renewals after the
 *    join are answered from bcmw_poll;
 *  - with cfg.fw_supplicant and "cap" listing "idsup": the firmware's own supplicant, sup_wpa 1 and the PMK
 *    (WLC_SET_WSEC_PMK, 32 raw bytes), done at E_PSK_SUP status 6 (keyed); not yet seen on this phone;
 *  - an open network: wsec 0, wpa_auth 0;
 * and WLC_SET_SSID with the name and the BSSID. Events: SET_SSID (status 0 joined), LINK (flag 1 up), DEAUTH,
 * DISASSOC, PSK_SUP. The passphrase, the PMK and the keys are never logged; nor are network names or addresses
 * (only the first half of the AP's address, its maker).
 *
 * WPA3-Personal (SAE, AKM 8; brcmfmac cfg80211.c brcmf_cfg80211_connect, cyw/core.c and fwil_types.h for the
 * external authentication), chosen when the AP offers SAE and this firmware and build can do it (transition
 * networks: SAE, then WPA2-PSK if SAE does not work for another reason than the password):
 *  - "cap" says how (bcmw_caps, logged once): "sae_ext": the host's SAE (sae.c, after bcmw_sae_enable): auth 3
 *    (SAE), wsec AES, wpa_auth WPA3_AUTH_SAE_PSK (0x40000), mfp 2 (required), sup_wpa 0, our RSN element (AKM 8,
 *    MFPC + MFPR; an RSNXE after it when the AP requires hash-to-element) as "wpaie", events 187..190 by
 *    "event_msgs_ext"; SET_SSID; the firmware asks with EXT_AUTH_REQ (187), our commit and confirm go out with the
 *    "mgmt_frame" iovar (struct brcmf_mf_params_le), the AP's come as EXT_AUTH_FRAME_RX (188) and the sends'
 *    acknowledgements as MGMT_FRAME_TXSTATUS (189); then "auth_status" (struct brcmf_auth_req_status_le: success
 *    and the PMKID, or failure); the firmware associates, and the four-way handshake is the host's with AKM 8
 *    (wpa.c: KDF-SHA256, AES-128-CMAC, the IGTK installed with the keys);
 *  - "sae" and "idsup" without "sae_ext" (or with cfg.fw_supplicant): the firmware's own SAE: the same settings
 *    with sup_wpa 1, "wpaie" cleared and the password by "sae_password" (struct brcmf_wsec_sae_pwd_le), done at
 *    PSK_SUP keyed;
 *  - neither: a WPA3-only network is refused with a log line; a transition network joined with WPA2-PSK.
 * PSK-SHA256 (AKM 6) when the AP offers it and not PSK; PMF capable on WPA2 with cfg.wpa2_mfp.
 */
#include "bcmw_priv.h"
#include "wpa.h"

#define FLOW_ITEMS      512u
#define FLOW_ITEM_SIZE  48u
#define FLOW_ID         2u      /* BRCMF_H2D_MSGRING_FLOWRING_IDSTART + flow 0 */
#define FLOW_FIFO       0u      /* brcmf_flowring_prio2fifo[0] = 0 (AC_BE; bcmdhd prio2ac[0] = 0) */

#define E_PSK_SUP       46
#define WSEC_AES        4u
#define WSEC_TKIP       2u
#define WPA2_AUTH_PSK   0x80u
#define WPA2_AUTH_PSK_SHA256 0x8000u
#define WPA3_AUTH_SAE_PSK 0x40000u
#define AUTH_SAE        3u      /* "auth" / WLC_SET_AUTH: brcmf_set_auth_type for NL80211_AUTHTYPE_SAE */
#define ALGO_BIP        12u     /* bcmdhd's CRYPTO_ALGO_BIP (BIP-CMAC-128); brcmfmac gives the IGTK as AES_CCM (4) */
#define EXTAUTH_FAIL    3u      /* brcmf_auth_req_status_le flags */
#define EXTAUTH_SUCCESS 4u
#define SAE_EXT_WAIT_US 4000000u    /* SET_SSID to EXT_AUTH_REQ, at most */
#define C_SET_KEY       45
#define C_DISASSOC      52
#define ALGO_AES_CCM    4u
#define PRIMARY_KEY     2u

/* ---- the flow ring ---------------------------------------------------------------------------- */

static int flow_open(struct bcmw *w, const uint8_t *da, const uint8_t *sa)
{
    struct bcmw_ring *c = &w->ring[RING_H2D_CONTROL];
    uint16_t got;
    uint8_t *it;
    uint64_t t0;

    if (w->flow_state == 2)
        return BCMW_OK;
    if (!w->flow_mem)
        w->flow_mem = bcmw_dma(w, FLOW_ITEMS * FLOW_ITEM_SIZE, 4096, &w->flow_bus);
    if (!w->txb)
        w->txb = bcmw_dma(w, TX_BUFS * TX_BUF_SIZE, 4096, &w->txb_bus);
    if (!w->flow_mem || !w->txb)
        return BCMW_E_NOMEM;
    bcmw_memset(w->flow_mem, 0, FLOW_ITEMS * FLOW_ITEM_SIZE);
    w->flow.ops = w->ring[RING_H2D_CONTROL].ops;
    w->flow.ctx = w;
    w->flow.id = FLOW_ID;
    w->flow.w_where = w->flow_w_where;
    w->flow.r_where = w->flow_r_where;
    ring_config(&w->flow, FLOW_ITEMS, FLOW_ITEM_SIZE, w->flow_mem, w->flow_bus, 1);
    bcmw_sync(w, w->flow_mem, FLOW_ITEMS * FLOW_ITEM_SIZE, 1);

    it = ring_reserve(c, 1, &got);
    if (!it)
        return BCMW_E_NOMEM;
    bcmw_memset(it, 0, c->item_len);
    it[0] = MSGBUF_TYPE_FLOW_RING_CREATE;
    it[1] = 0;
    bcmw_memcpy(it + 8, da, 6);
    bcmw_memcpy(it + 14, sa, 6);
    it[20] = FLOW_FIFO;                     /* tid: brcmf_flowring_tid() = the fifo */
    put16(it + 22, FLOW_ID);
    put16(it + 28, FLOW_ITEMS);
    put16(it + 30, FLOW_ITEM_SIZE);
    put32(it + 32, (uint32_t)w->flow_bus);
    put32(it + 36, (uint32_t)(w->flow_bus >> 32));
    w->flow_state = 1;
    ring_commit(c);
    bcmw_say(w, "link: the tx flow ring asked for (512 x 48 bytes, best effort)");
    t0 = bcmw_now(w);
    while (w->flow_state == 1 && !w->dead) {
        mb_process(w);
        if (bcmw_now(w) - t0 > 2000000) {
            bcmw_say(w, "link: TIMEOUT: no answer to the flow ring's creation");
            w->flow_state = 0;
            return BCMW_E_TIMEOUT;
        }
        bcmw_udelay(w, 100);
    }
    return w->flow_state == 2 ? BCMW_OK : BCMW_E_FW;
}

void link_flow_created(struct bcmw *w, const uint8_t *it)
{
    uint16_t status = le16(it + 8), id = le16(it + 10);
    if (id != FLOW_ID)
        return;
    w->flow_state = status ? -1 : 2;
    bcmw_sayx(w, "link: the flow ring's creation answered, status ", status, status ? "(REFUSED)" : "(open)");
}

void link_tx_status(struct bcmw *w, const uint8_t *it)
{
    uint32_t id = le32(it + 4);
    uint16_t status = le16(it + 8);
    if (id == 0 || id > TX_BUFS || !w->tx_busy[id - 1]) {
        bcmw_sayx(w, "link: a tx status for a buffer that is not ours: ", id, NULL);
        return;
    }
    w->tx_busy[id - 1] = 0;
    w->info.tx_done++;
    if (status)
        w->info.tx_errors++;
}

int bcmw_tx(struct bcmw *w, const uint8_t *frame, uint32_t len)
{
    uint16_t got;
    uint8_t *it, *b;
    unsigned slot;
    int e;

    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    if (w->in_process && w->flow_state != 2)
        return BCMW_E_STATE;                /* from a callback before the flow ring is open: it would poll */
    if (len < 14 || len - 14 > TX_BUF_SIZE)
        return BCMW_E_REFUSED;
    if (w->flow_state != 2 && (e = flow_open(w, frame, frame + 6)) != BCMW_OK)
        return e;
    for (slot = 0; slot < TX_BUFS && w->tx_busy[slot]; slot++)
        ;
    if (slot == TX_BUFS) {
        mb_process(w);                      /* tx status may be waiting (does nothing inside a callback) */
        for (slot = 0; slot < TX_BUFS && w->tx_busy[slot]; slot++)
            ;
        if (slot == TX_BUFS)
            return BCMW_E_NOMEM;
    }
    it = ring_reserve(&w->flow, 1, &got);
    if (!it)
        return BCMW_E_NOMEM;
    b = w->txb + slot * TX_BUF_SIZE;
    bcmw_memcpy(b, frame + 14, len - 14);
    bcmw_sync(w, b, len - 14, 1);
    bcmw_memset(it, 0, FLOW_ITEM_SIZE);
    it[0] = MSGBUF_TYPE_TX_POST;
    it[1] = 0;
    put32(it + 4, slot + 1);                /* brcmfmac: pktid + 1 */
    bcmw_memcpy(it + 8, frame, 14);
    it[22] = 0x01;                          /* 802.3, priority 0 */
    it[23] = 1;
    put32(it + 32, (uint32_t)(w->txb_bus + slot * TX_BUF_SIZE));
    put32(it + 36, (uint32_t)((w->txb_bus + slot * TX_BUF_SIZE) >> 32));
    put16(it + 42, (uint16_t)(len - 14));
    w->tx_busy[slot] = 1;
    w->info.tx_frames++;
    ring_commit(&w->flow);
    return BCMW_OK;
}

/* ---- randomness for the handshake's nonce --------------------------------------------------------- */

static void random_bytes(struct bcmw *w, uint8_t *out, uint32_t n)
{
    if (w->os.random && w->os.random(w->os.ctx, out, n) == 0)
        return;
    /* no source from the OS: timing jitter of the clock around short delays, hashed. Not a strong source;
     * the shims should give one (Windows: BCryptGenRandom; libsb: see docs/WIFI-BCM4361.md) */
    {
        uint8_t pool[8 * 64 + 16], sum[32];
        for (unsigned i = 0; i < 64; i++) {
            uint64_t t = bcmw_now(w);
            bcmw_udelay(w, 1 + (unsigned)(t & 7));
            put32(pool + 8 * i, (uint32_t)t);
            put32(pool + 8 * i + 4, (uint32_t)(bcmw_now(w) ^ (t >> 32)));
        }
        bcmw_memcpy(pool + 512, w->info.mac, 6);
        put32(pool + 518, w->info.polls);
        put32(pool + 522, w->info.events);
        fw_sha256(pool, sizeof(pool), sum);
        for (uint32_t i = 0; i < n; i++)
            out[i] = sum[i % 32];
        bcmw_memset(pool, 0, sizeof(pool));
    }
}

static int sae_random(void *ctx, uint8_t *out, uint32_t n)
{
    random_bytes((struct bcmw *)ctx, out, n);
    return 0;
}

/* ---- what the firmware can do, and what an access point offers ----------------------------------------------- */

static int cap_word(const char *cap, const char *word)
{
    unsigned n = bcmw_strlen(word);
    for (unsigned i = 0; cap[i]; i++)
        if ((i == 0 || cap[i - 1] == ' ') && bcmw_strneq(cap + i, word, n) && (cap[i + n] == ' ' || !cap[i + n]))
            return 1;
    return 0;
}

unsigned bcmw_caps(struct bcmw *w)
{
    char cap[512];
    uint32_t sum = 0x811c9dc5u, v;
    unsigned c = 0;
    if (w->step < BCMW_STEP_UP || w->dead)
        return w->caps;
    bcmw_memset(cap, 0, sizeof(cap));
    if (bcmw_iovar_get(w, "cap", cap, sizeof(cap) - 1, 0) != BCMW_OK)
        cap[0] = 0;
    for (unsigned i = 0; cap[i]; i++)
        sum = (sum ^ (uint8_t)cap[i]) * 0x01000193u;
    if (sum != w->cap_sum) {
        /* the whole string, once (its words are features, nothing identifying), in lines of at most ~150 */
        struct bcmw_lb l;
        unsigned at = 0, n = bcmw_strlen(cap), part = 0;
        w->cap_sum = sum;
        do {
            unsigned take = n - at > 150 ? 150 : n - at;
            while (take < n - at && take > 0 && cap[at + take] != ' ')
                take--;
            if (!take)
                take = n - at > 150 ? 150 : n - at;
            lb_init(&l);
            lb_s(&l, part++ ? "link: cap (more): " : "link: the firmware's cap: ");
            for (unsigned i = 0; i < take; i++) {
                char one[2] = { cap[at + i] >= ' ' && cap[at + i] < 0x7f ? cap[at + i] : '?', 0 };
                lb_s(&l, one);
            }
            bcmw_say_lb(w, &l);
            at += take;
            while (at < n && cap[at] == ' ')
                at++;
        } while (at < n);
    }
    if (cap_word(cap, "sae_ext"))
        c |= BCMW_CAP_SAE_EXT;
    if (cap_word(cap, "sae"))
        c |= BCMW_CAP_SAE;
    if (cap_word(cap, "idsup"))
        c |= BCMW_CAP_IDSUP;
    if (cap_word(cap, "mfp") || bcmw_iovar_get_u32(w, "mfp", &v) == BCMW_OK)   /* brcmfmac: the "mfp" iovar answers */
        c |= BCMW_CAP_MFP;
    if (w->sae_ops)
        c |= BCMW_CAP_HOST_SAE;
    if (((c & BCMW_CAP_HOST_SAE) && (c & BCMW_CAP_SAE_EXT)) || ((c & BCMW_CAP_SAE) && (c & BCMW_CAP_IDSUP)))
        c |= BCMW_CAP_WPA3;
    w->caps = c;
    return c;
}

#define AKM_BIT(n) (1u << (n))

/* the security an RSN element (and the privacy bit) says */
static unsigned security_of(const uint8_t *ie, uint32_t ie_len, uint16_t capability)
{
    const uint8_t *rsn = bcmw_ie_find(ie, ie_len, 48);
    struct bcmw_rsn r;
    int sae, psk;
    if (!rsn)
        return capability & 0x0010 ? BCMW_SEC_OTHER : BCMW_SEC_OPEN;    /* privacy: WEP or WPA of the first kind */
    if (bcmw_rsn_parse(rsn, 2u + rsn[1], &r) != 0 || !(r.pairwise & AKM_BIT(BCMW_CIPHER_CCMP)))
        return BCMW_SEC_OTHER;                                          /* malformed, or TKIP only */
    sae = (r.akms & AKM_BIT(8)) != 0;
    psk = (r.akms & (AKM_BIT(2) | AKM_BIT(6))) != 0;
    return sae && psk ? BCMW_SEC_WPA2_WPA3 : sae ? BCMW_SEC_WPA3 : psk ? BCMW_SEC_WPA2 : BCMW_SEC_OTHER;
}

unsigned bcmw_bss_security(const struct bcmw_bss *b)
{
    return security_of(b->ie, b->ie_len, b->capability);
}

const char *bcmw_security_name(unsigned sec)
{
    static const char *const names[] = { "open", "WPA2", "WPA2/WPA3", "WPA3", "other" };
    return sec <= BCMW_SEC_OTHER ? names[sec] : "other";
}

/* hash-to-element required: the BSS membership selector 123 (0x80 | 123 among the rates) */
static int h2e_required(const uint8_t *ie, uint32_t ie_len)
{
    for (unsigned id = 1; id <= 50; id += 49) {
        const uint8_t *e = bcmw_ie_find(ie, ie_len, id);       /* supported rates, extended supported rates */
        for (unsigned i = 0; e && i < e[1]; i++)
            if (e[2 + i] == (0x80 | 123))
                return 1;
    }
    return 0;
}

/* ---- events and EAPOL while joining ----------------------------------------------------------------- */

static void scan_result(struct bcmw *w, const struct bcmw_event *ev)
{
    const uint8_t *d = ev->data;
    uint32_t blen, ie_off, ie_len;
    const uint8_t *ie, *rsn;
    unsigned group = 0, ok;
    int16_t rssi;
    struct bcmw_rsn r;

    if (ev->status != 8 || ev->datalen < 12 + 128 || le16(d + 10) < 1)
        return;
    d += 12;                                /* the first bss_info */
    blen = le32(d + 4);
    if (blen < 128 || 12 + blen > ev->datalen || d[18] != w->join_ssid_len ||
        bcmw_memcmp(d + 19, w->join_ssid, w->join_ssid_len))
        return;
    ie_off = le16(d + 116);
    ie_len = le32(d + 120);
    if (ie_off > blen || ie_len > blen - ie_off)
        return;
    ie = d + ie_off;
    rsn = bcmw_ie_find(ie, ie_len, 48);
    if (w->join_open)
        ok = !rsn && !(le16(d + 16) & 0x0010);  /* no RSN, privacy off */
    else {
        int psk = rsn && bcmw_rsn_check(rsn, 2u + rsn[1], &group) == 0 && (group == BCMW_CIPHER_CCMP || w->join_fwsup);
        int sae = 0, psk256 = 0;
        if (rsn && bcmw_rsn_parse(rsn, 2u + rsn[1], &r) == 0 && r.group == BCMW_CIPHER_CCMP &&
            (r.pairwise & AKM_BIT(BCMW_CIPHER_CCMP))) {
            /* WPA3: SAE with CCMP and BIP-CMAC-128 (the default) when this firmware and build can do it */
            sae = (r.akms & AKM_BIT(8)) && (w->caps & BCMW_CAP_WPA3) &&
                  (r.group_mgmt == 0 || r.group_mgmt == BCMW_CIPHER_BIP_CMAC);
            psk256 = (r.akms & AKM_BIT(6)) && !w->join_fwsup;
            if (!sae && !psk && !psk256 && (r.akms & AKM_BIT(8)))
                w->join_wpa3_refused++;
            if (sae || psk256)
                group = BCMW_CIPHER_CCMP;
        }
        ok = psk || sae || psk256;
    }
    rssi = (int16_t)le16(d + 78);
    if (!ok) {
        w->join_unsuitable++;
        return;
    }
    {
        /* wifi-core 3ccc92a: 5 GHz preferred when the same network is on both bands (a 5 GHz access point wins
         * unless it is 20 dB weaker): T4-WPA2 joined the user's network on 5 GHz but timed out on its 2.4 GHz side
         * (as W2g's 2.4 GHz hotspot did under Windows); Starbite OS's self tests picked 2.4 GHz and timed out */
        int b5 = (le16(d + 72) & 0xc000) == 0xc000, cur5 = (w->join_chanspec & 0xc000) == 0xc000;
        if ((w->cfg.join_band == 1 && b5) || (w->cfg.join_band == 2 && !b5)) {
            w->join_unsuitable++;       /* Track H: the band asked for (cfg.join_band) only */
            return;
        }
        int better = !w->join_found || (b5 == cur5 ? rssi > w->join_rssi :
                                        b5 ? rssi + 20 > w->join_rssi : rssi > w->join_rssi + 20);
        if (!better)
            return;
    }
    {
        w->join_found = 1;
        w->join_rssi = rssi;
        bcmw_memcpy(w->join_bssid, d + 8, 6);
        w->join_group = group;
        w->join_chanspec = le16(d + 72);
        /* what this AP offers, for join_associate's choice and message 3's check */
        w->join_ap_akms = 0;
        w->join_ap_caps = 0;
        w->join_group_mgmt = 0;
        w->join_ap_rsn_len = w->join_ap_rsnxe_len = 0;
        w->join_h2e = 0;
        if (rsn && bcmw_rsn_parse(rsn, 2u + rsn[1], &r) == 0) {
            const uint8_t *x = bcmw_ie_find(ie, ie_len, 244);   /* RSNXE */
            w->join_ap_akms = r.akms;
            w->join_ap_caps = (uint16_t)r.caps;
            w->join_group_mgmt = r.group_mgmt;
            if (2u + rsn[1] <= sizeof(w->join_ap_rsn)) {
                bcmw_memcpy(w->join_ap_rsn, rsn, 2u + rsn[1]);
                w->join_ap_rsn_len = 2u + rsn[1];
            }
            if (x && 2u + x[1] <= sizeof(w->join_ap_rsnxe)) {
                bcmw_memcpy(w->join_ap_rsnxe, x, 2u + x[1]);
                w->join_ap_rsnxe_len = 2u + x[1];
            }
            w->join_h2e = h2e_required(ie, ie_len);
        }
    }
}

#define SYNC_STEP10 0x1234
#define SYNC_JOIN   0x4a4e

void link_event(struct bcmw *w, const struct bcmw_event *ev)
{
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        /* the escan result's sync id (+8) says whose scan it is: step 10's may still be running when a join
         * starts, and the firmware ends it with status 9 (NEWSCAN) when a new one pre-empts it */
        uint16_t sync = ev->datalen >= 10 ? le16(ev->data + 8) : 0;
        if (sync == SYNC_STEP10 && ev->status != 8)
            w->scan_running = 0;
        if (w->join_phase == 1 && sync == SYNC_JOIN) {
            if (ev->status == 8)
                scan_result(w, ev);
            else
                w->join_scan_done = 1;
        }
        return;
    }
    if (w->join_phase < 2)
        return;
    if (w->join_phase == 2 && ev->type != E_PSK_SUP) {
        /* wifi-core 3ccc92a: every event while joining (what the firmware did before a timeout) */
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "link: event ");
        lb_dec(&l, ev->type);
        lb_s(&l, " status ");
        lb_dec(&l, ev->status);
        lb_s(&l, " reason ");
        lb_dec(&l, ev->reason);
        lb_s(&l, " flags ");
        lb_hex(&l, ev->flags, 4);
        bcmw_say_lb(w, &l);
    }
    switch (ev->type) {
    case BCMW_E_SET_SSID:
        if (ev->status != 0) {
            bcmw_sayx(w, "link: the firmware could not join (SET_SSID status ", ev->status, ")");
            w->join_phase = -1;
        }
        break;
    case BCMW_E_LINK:
        if (ev->flags & 1) {
            w->link_up = 1;
            bcmw_say(w, "link: LINK UP (associated)");
        } else {
            bcmw_sayx(w, "link: link down, reason ", ev->reason, NULL);
            w->link_up = 0;
            if (w->join_phase != 3)
                w->join_phase = -1;
        }
        break;
    case BCMW_E_DEAUTH:
    case BCMW_E_DEAUTH_IND:
    case BCMW_E_DISASSOC_IND:
        bcmw_sayx(w, "link: the access point sent us away, reason ", ev->reason, NULL);
        w->link_up = 0;
        w->join_phase = -1;
        break;
    case BCMW_E_EXT_AUTH_REQ:
        /* brcmf_auth_req_status_le: flags, the peer's address, the name; only for the AP we chose */
        if (w->sae_phase == 1 || w->sae_phase == 2) {
            if (ev->datalen >= 2 + 6 && !bcmw_memcmp(ev->data + 2, w->join_bssid, 6))
                w->sae_req = 1;
            else
                bcmw_say(w, "link: SAE: an EXT_AUTH_REQ for another access point: ignored");
        }
        break;
    case BCMW_E_EXT_AUTH_FRAME_RX:
        /* brcmf_rx_mgmt_data (16 bytes), then the Authentication frame: its 24-byte header, its body (as brcmfmac
         * cyw/core.c brcmf_notify_auth_frame_rx: the sender is the event's address) */
        if (w->sae_phase == 1 || w->sae_phase == 2) {
            if (ev->datalen < 16 + 24 + 6 || ev->datalen - 40 > BCMW_SAE_FRAME_MOST ||
                bcmw_memcmp(ev->addr, w->join_bssid, 6))
                bcmw_sayx(w, "link: SAE: an Authentication frame not from our access point or too long: ",
                          ev->datalen, " bytes, dropped");
            else if (w->sae_rx_n == 4)
                bcmw_say(w, "link: SAE: more than 4 Authentication frames waiting: one dropped");
            else {
                bcmw_memcpy(w->sae_rx[w->sae_rx_n], ev->data + 40, ev->datalen - 40);
                w->sae_rx_len[w->sae_rx_n++] = ev->datalen - 40;
            }
        }
        break;
    case BCMW_E_MGMT_FRAME_TXSTATUS:
        if (w->sae_phase && ev->datalen >= 4 && le32(ev->data) == w->sae_packet) {
            if (ev->status == 0)
                w->sae_acked++;
            else
                w->sae_noack++;
        }
        break;
    case E_PSK_SUP:
        if (w->join_fwsup) {
            if (ev->status == 6) {
                w->keyed = 1;
                bcmw_say(w, "link: the firmware's supplicant finished the handshake (keyed)");
            } else {
                bcmw_sayx(w, "link: the firmware's supplicant reports status ", ev->status,
                          ev->reason == 14 ? "(reason 14: a MIC failure: the passphrase?)" : NULL);
            }
        }
        break;
    }
}

static int set_key_algo(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, unsigned algo,
                        const uint8_t *ea, const uint8_t *rsc);

static int set_key(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, const uint8_t *ea,
                   const uint8_t *rsc)
{
    return set_key_algo(w, index, key, len, ALGO_AES_CCM, ea, rsc);
}

static int set_key_algo(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, unsigned algo,
                        const uint8_t *ea, const uint8_t *rsc)
{
    uint8_t k[164];
    int fwerr = 0, e;
    bcmw_memset(k, 0, sizeof(k));
    put32(k + 0, index);
    put32(k + 4, len);
    bcmw_memcpy(k + 8, key, len);
    if (len > 32)
        return BCMW_E_REFUSED;
    put32(k + 112, algo);
    /* brcmf_cfg80211_add_key: a key given with the peer's address (the pairwise one) is an "ext key",
     * flags 0 and ea = the peer; one without (the group key) is the primary key */
    put32(k + 116, ea ? 0 : PRIMARY_KEY);
    if (rsc) {                              /* brcmfmac: rxiv.hi = seq[5..2], rxiv.lo = seq[1..0] */
        put32(k + 140, (uint32_t)rsc[5] << 24 | (uint32_t)rsc[4] << 16 | (uint32_t)rsc[3] << 8 | rsc[2]);
        put16(k + 144, (uint16_t)(rsc[1] << 8 | rsc[0]));
        put32(k + 132, 1);
    }
    if (ea)
        bcmw_memcpy(k + 156, ea, 6);
    e = bcmw_iovar_set(w, "wsec_key", k, sizeof(k), &fwerr);
    bcmw_memset(k, 0, sizeof(k));
    return e;
}

void link_eapol(struct bcmw *w, const uint8_t *frame, uint32_t len)
{
    uint8_t reply[14 + BCMW_WPA_REPLY_MOST];
    unsigned rlen = 0;
    const char *why = "";
    int r;

    if ((w->join_phase != 2 && w->join_phase != 3) || w->join_fwsup || w->join_open || len < 14 + 4)
        return;
    if (bcmw_memcmp(frame + 6, w->join_bssid, 6)) {
        bcmw_say(w, "link: a handshake frame from another address than the access point's: ignored");
        return;                             /* only from the access point we joined (as an earlier port) */
    }
    r = bcmw_wpa_message(&w->wpa, frame + 14, len - 14, reply + 14, &rlen, &why);
    if (r == BCMW_WPA_BAD) {
        struct bcmw_lb l;
        lb_init(&l);
        lb_s(&l, "link: a handshake message refused: ");
        lb_s(&l, why);
        bcmw_say_lb(w, &l);
        return;
    }
    if (r == BCMW_WPA_NOTHING || !rlen)
        return;
    bcmw_memcpy(reply, frame + 6, 6);       /* to the access point */
    bcmw_memcpy(reply + 6, w->info.mac, 6);
    reply[12] = 0x88, reply[13] = 0x8e;
    bcmw_say(w, r == BCMW_WPA_ANSWER ? "link: handshake message 1 from the access point: answered (message 2)"
                : r == BCMW_WPA_KEYS ? "link: handshake message 3, signed with the key we have too: answered (message 4)"
                                     : "link: a new group key: answered");
    /* sent by the join loop, outside the completions being processed (bcmw_tx may poll the rings) */
    bcmw_memcpy(w->eapol_out, reply, 14 + rlen);
    w->eapol_out_len = 14 + rlen;
    w->eapol_out_kind = r;
    bcmw_memset(reply, 0, sizeof(reply));
}

static void install_keys(struct bcmw *w);

static void send_eapol(struct bcmw *w)
{
    int r = w->eapol_out_kind;
    uint8_t f[sizeof(w->eapol_out)];
    unsigned n = w->eapol_out_len;
    if (!n)
        return;
    /* bcmw_tx may poll the rings (the flow ring's creation): a new answer may land in eapol_out meanwhile */
    bcmw_memcpy(f, w->eapol_out, n);
    bcmw_memset(w->eapol_out, 0, sizeof(w->eapol_out));
    w->eapol_out_len = 0;
    if (bcmw_tx(w, f, n) != BCMW_OK) {
        bcmw_say(w, "link: the handshake's answer could not be sent");
        w->join_phase = -1;
    } else if (r == BCMW_WPA_KEYS || r == BCMW_WPA_GROUP) {
        w->keys_due = r;
    }
    bcmw_memset(f, 0, sizeof(f));
}

/* after the join: group-key renewals (from bcmw_poll) */
void link_service(struct bcmw *w)
{
    if (w->join_phase != 3 || w->in_process)
        return;
    send_eapol(w);
    if (w->keys_due)
        install_keys(w);
}

static void install_keys(struct bcmw *w)
{
    int a = BCMW_OK, b;
    uint64_t t0 = bcmw_now(w);
    /* message 4 out before the pairwise key (brcmfmac: brcmf_netdev_wait_pend8021x) */
    while (w->info.tx_done < w->info.tx_frames && bcmw_now(w) - t0 < 500000) {
        mb_process(w);
        bcmw_udelay(w, 200);
    }
    if (w->keys_due == BCMW_WPA_KEYS)
        a = set_key(w, 0, w->wpa.pairwise, 16, w->join_bssid, 0);
    b = set_key(w, w->wpa.group_index, w->wpa.group, w->wpa.group_length, 0, w->wpa.group_counter);
    w->keys_due = 0;
    if (b == BCMW_OK && w->wpa.have_igtk && w->join_mfp) {
        /* the IGTK (management frame protection's group key) with its IPN as the receive counter: bcmdhd's BIP
         * algorithm first, then brcmfmac's way (AES_CCM). Not fatal: the data path does not need it. */
        int e = set_key_algo(w, w->wpa.igtk_index, w->wpa.igtk, 16, ALGO_BIP, 0, w->wpa.igtk_ipn);
        const char *how = "link: the IGTK given to the firmware (BIP), key id ";
        if (e != BCMW_OK) {
            e = set_key_algo(w, w->wpa.igtk_index, w->wpa.igtk, 16, ALGO_AES_CCM, 0, w->wpa.igtk_ipn);
            how = e == BCMW_OK ? "link: the IGTK given to the firmware (as AES_CCM, as brcmfmac: BIP refused), key id "
                               : "link: the firmware refused the IGTK (BIP and AES_CCM): protected management frames "
                                 "to all are not checked; key id ";
        }
        bcmw_sayx(w, how, w->wpa.igtk_index, NULL);
    }
    if (a == BCMW_OK && b == BCMW_OK) {
        w->keyed = 1;
        bcmw_say(w, "link: both keys given to the firmware (keyed)");
    } else {
        bcmw_say(w, "link: the firmware refused a key");
        if (w->join_phase == 2)
            w->join_phase = -1;
    }
}

/* ---- external SAE: the frames through the firmware ------------------------------------------------------ */

/* events 187..190 are past event_msgs' 128 bits: the whole mask again with them, by "event_msgs_ext"
 * (struct brcmf_eventmsgs_ext: version 1, command 3 SET_MASK, length, maxgetsize, the mask; brcmfmac cyw/core.c) */
static int sae_events_on(struct bcmw *w)
{
    uint8_t m[4 + 25];
    int e, fwerr = 0;
    bcmw_memset(m, 0, sizeof(m));
    if (bcmw_iovar_get(w, "event_msgs", m + 4, 16, 0) != BCMW_OK)
        bcmw_say(w, "link: SAE: event_msgs could not be read: the extended mask carries only the SAE events");
    m[0] = 1, m[1] = 3, m[2] = 25;
    for (unsigned ev = BCMW_E_EXT_AUTH_REQ; ev <= 190; ev++)
        m[4 + ev / 8] |= (uint8_t)(1u << (ev % 8));
    e = bcmw_iovar_set(w, "event_msgs_ext", m, sizeof(m), &fwerr);
    bcmw_sayx(w, "link: SAE: events 187..190 on (event_msgs_ext): ", e == BCMW_OK ? 0 : (uint32_t)fwerr,
              e == BCMW_OK ? "(fine)" : "(REFUSED: the firmware's error)");
    return e;
}

/* an Authentication frame's body out by "mgmt_frame" (struct brcmf_mf_params_le, as brcmf_cyw_mgmt_tx) */
static int sae_send(struct bcmw *w, const uint8_t *body, unsigned len)
{
    uint8_t p[30 + BCMW_SAE_FRAME_MOST];
    int e, fwerr = 0;
    if (!len || len > BCMW_SAE_FRAME_MOST)
        return BCMW_E_REFUSED;
    bcmw_memset(p, 0, 30);
    put32(p + 0, 0);                            /* version (brcmfmac leaves it 0) */
    put32(p + 4, 4000);                         /* dwell time, ms (MGMT_AUTH_FRAME_DWELL_TIME) */
    put16(p + 8, (uint16_t)len);
    put16(p + 10, 0x00b0);                      /* frame control: management, Authentication */
    put16(p + 12, (uint16_t)bcmw_chanspec_channel(w->join_chanspec));
    bcmw_memcpy(p + 14, w->join_bssid, 6);
    bcmw_memcpy(p + 20, w->join_bssid, 6);
    put32(p + 26, ++w->sae_packet);
    bcmw_memcpy(p + 30, body, len);
    e = bcmw_iovar_set(w, "mgmt_frame", p, 30 + len, &fwerr);
    if (e != BCMW_OK)
        bcmw_sayx(w, "link: SAE: mgmt_frame refused; the firmware's error ", (uint32_t)fwerr, NULL);
    if (body != w->sae_last)
        bcmw_memcpy(w->sae_last, body, len);
    w->sae_last_len = len;
    w->sae_t = bcmw_now(w);
    bcmw_memset(p, 0, sizeof(p));
    return e;
}

/* the result to the firmware: "auth_status" (struct brcmf_auth_req_status_le) */
static int sae_status(struct bcmw *w, int ok)
{
    uint8_t a[2 + 6 + 4 + 32 + 16];
    int e, fwerr = 0;
    bcmw_memset(a, 0, sizeof(a));
    put16(a, (uint16_t)(ok ? EXTAUTH_SUCCESS : EXTAUTH_FAIL));
    bcmw_memcpy(a + 2, w->join_bssid, 6);
    put32(a + 8, w->join_ssid_len);
    bcmw_memcpy(a + 12, w->join_ssid, w->join_ssid_len);
    if (ok)
        bcmw_memcpy(a + 44, w->sae.pmkid, 16);
    e = bcmw_iovar_set(w, "auth_status", a, sizeof(a), &fwerr);
    if (e != BCMW_OK)
        bcmw_sayx(w, "link: SAE: auth_status refused; the firmware's error ", (uint32_t)fwerr, NULL);
    return e;
}

static void sae_fail(struct bcmw *w, const char *why, int wrong_password)
{
    struct bcmw_lb l;
    lb_init(&l);
    lb_s(&l, "link: SAE FAILED: ");
    lb_s(&l, why);
    bcmw_say_lb(w, &l);
    if (w->sae_phase == 2)
        sae_status(w, 0);
    w->sae_phase = -1;
    w->sae_wrong = wrong_password;
    if (w->sae_ops)
        w->sae_ops->clear(&w->sae);
    w->join_phase = -1;
}

/* from bcmw_join_poll, outside the completions: the commit when the firmware asks, the frames that came, the
 * retransmissions (1 s, 3 times), the result */
static void sae_service(struct bcmw *w)
{
    uint8_t out[BCMW_SAE_FRAME_MOST];
    unsigned n;
    if (w->sae_phase != 1 && w->sae_phase != 2)
        return;
    if (w->sae_req) {
        w->sae_req = 0;
        if (w->sae_phase == 1) {
            n = w->sae_ops->commit_frame(&w->sae, out, sizeof(out));
            w->sae_phase = 2;
            w->sae_tries = 0;
            bcmw_say(w, w->join_h2e ? "link: SAE: the firmware asks (EXT_AUTH_REQ): our commit (hash-to-element)"
                                    : "link: SAE: the firmware asks (EXT_AUTH_REQ): our commit (hunting-and-pecking)");
            if (sae_send(w, out, n) != BCMW_OK) {
                sae_fail(w, "our commit could not be sent", 0);
                return;
            }
        } else {
            bcmw_say(w, "link: SAE: the firmware asks again: our last frame again");
            sae_send(w, w->sae_last, w->sae_last_len);
        }
    }
    for (unsigned i = 0; i < w->sae_rx_n && w->sae_phase == 2; i++) {
        const char *why = "";
        int r = w->sae_ops->sta_rx(&w->sae, w->sae_rx[i], w->sae_rx_len[i], out, &n, &why);
        if (r == BCMW_SAE_SEND) {
            if (*why) {
                struct bcmw_lb l;
                lb_init(&l);
                lb_s(&l, "link: SAE: ");
                lb_s(&l, why);
                bcmw_say_lb(w, &l);
            } else {
                bcmw_say(w, "link: SAE: the access point's commit taken: our confirm");
            }
            w->sae_tries = 0;
            if (sae_send(w, out, n) != BCMW_OK)
                sae_fail(w, "our frame could not be sent", 0);
        } else if (r == BCMW_SAE_DONE) {
            uint64_t t = bcmw_now(w);
            bcmw_memcpy(w->wpa.pmk, w->sae.pmk, 32);
            bcmw_memcpy(w->wpa.pmkid, w->sae.pmkid, 16);
            w->wpa.pmkid_known = 1;
            w->sae_phase = sae_status(w, 1) == BCMW_OK ? 3 : -1;
            w->sae_ops->clear(&w->sae);
            bcmw_sayx(w, w->sae_phase == 3 ? "link: SAE: the access point's confirm verified: authenticated (PMK made); "
                                             "auth_status given, the association next ("
                                           : "link: SAE: authenticated, but the firmware refused auth_status (",
                      (bcmw_now(w) - t) / 1000, " ms)");
            if (w->sae_phase != 3)
                w->join_phase = -1;
        } else if (r == BCMW_SAE_FAIL) {
            sae_fail(w, why, w->sae.bad_confirm);
        } else if (*why) {
            struct bcmw_lb l;
            lb_init(&l);
            lb_s(&l, "link: SAE: a frame ignored: ");
            lb_s(&l, why);
            bcmw_say_lb(w, &l);
        }
    }
    w->sae_rx_n = 0;
    bcmw_memset(out, 0, sizeof(out));
    if (w->sae_phase == 2 && bcmw_now(w) - w->sae_t > 1000000) {
        if (w->sae_tries++ < 3) {
            struct bcmw_lb l;
            lb_init(&l);
            lb_s(&l, "link: SAE: no answer for 1 s: our last frame again (");
            lb_dec(&l, w->sae_tries);
            lb_s(&l, " of 3; the firmware's tx status so far: ");
            lb_dec(&l, w->sae_acked);
            lb_s(&l, " acknowledged, ");
            lb_dec(&l, w->sae_noack);
            lb_s(&l, " not)");
            bcmw_say_lb(w, &l);
            sae_send(w, w->sae_last, w->sae_last_len);
        } else {
            sae_fail(w, "no answer from the access point to our SAE frames", 0);
        }
    }
    if (w->sae_phase == 1 && bcmw_now(w) - w->join_t0 > SAE_EXT_WAIT_US)
        sae_fail(w, "the firmware did not ask for the authentication (no EXT_AUTH_REQ in 4 s: events off, or no "
                    "external SAE after all?)", 0);
}

/* ---- the join ------------------------------------------------------------------------------------------ */

static int scan_begin(struct bcmw *w)
{
    uint8_t p[80];
    uint64_t t0;
    int e;

    uint32_t n = bcmw_escan_params(w, p, w->join_ssid, w->join_ssid_len, SYNC_JOIN);
    t0 = bcmw_now(w);
    while (w->scan_running && bcmw_now(w) - t0 < 10000000) {    /* step 10's scan first */
        mb_process(w);
        bcmw_udelay(w, 1000);
    }
    w->join_phase = 1;
    w->join_found = 0;
    w->join_scan_done = 0;
    w->join_settle_t = 0;
    w->join_unsuitable = 0;
    w->join_wpa3_refused = 0;
    for (unsigned k = 0; (e = bcmw_iovar_set(w, "escan", p, n, 0)) != BCMW_OK && k < 5; k++) {
        bcmw_say(w, "link: the scan was refused (busy?): again in 500 ms");
        for (unsigned d = 0; d < 500; d++) {
            mb_process(w);
            bcmw_udelay(w, 1000);
        }
    }
    if (e != BCMW_OK)
        return e;
    w->join_t0 = bcmw_now(w);           /* the directed scan's start: bcmw_join_poll waits up to 8 s for its end */
    return BCMW_OK;
}

/* Power save off for the join and the handshake, as bcmdhd does (it enables PM again later). Set explicitly even
 * when it reads back 0: in the Windows runs the AP's handshake message 1 was dropped inside the firmware ("EL: 30
 * 73b8" at each retry) until W2g set it, and T4-WPA2 (bcmw_join, which did not) showed the same signature
 * (wifi-core e14fca2; Starbite OS 0.6.1 found the same from its tour). */
static void join_pm_off(struct bcmw *w)
{
    uint32_t pm = 0xffffffffu, off = 0;
    int fwerr = 0;
    int ge = bcmw_ioctl(w, 0, BCMW_C_GET_PM, &pm, 4, 0, &fwerr);
    int se = bcmw_ioctl(w, 0, BCMW_C_SET_PM, &off, 4, 1, &fwerr);
    bcmw_sayx(w, "link: power save before the join ", ge == BCMW_OK ? pm : 0xffffffffu,
              se == BCMW_OK ? "; now 0 (off: constantly awake)" : "; setting 0 REFUSED");
}

int bcmw_join_start(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned timeout_ms)
{
    unsigned pass_len = bcmw_strlen(pass);
    int e;
    struct bcmw_lb l;

    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    if (ssid_len == 0 || ssid_len > 32 || (pass_len && (pass_len < 8 || pass_len > 64)))
        return BCMW_E_REFUSED;
    bcmw_memset(&w->wpa, 0, sizeof(w->wpa));
    bcmw_memcpy(w->join_ssid, ssid, ssid_len);
    w->join_ssid_len = (uint8_t)ssid_len;
    w->join_open = pass_len == 0;
    w->link_up = w->keyed = 0;
    w->keys_due = 0;
    w->join_akm = 0;
    w->join_fwsae = w->join_mfp = w->join_no_sae = w->sae_wrong = 0;
    w->sae_phase = 0;
    w->sae_req = 0;
    w->sae_rx_n = 0;
    w->sae_acked = w->sae_noack = 0;
    w->sae.random = sae_random;
    w->sae.random_ctx = w;
    bcmw_caps(w);                       /* "cap": what the firmware can do (logged once) */
    w->join_fwsup = !w->join_open && w->cfg.fw_supplicant && (w->caps & BCMW_CAP_IDSUP);
    bcmw_memset(w->join_pass, 0, sizeof(w->join_pass));
    w->join_pass_len = 0;
    if (!w->join_open && (w->caps & BCMW_CAP_WPA3)) {
        /* SAE needs the password with the AP's address: kept until the AP is chosen (join_associate wipes it) */
        bcmw_memcpy(w->join_pass, pass, pass_len);
        w->join_pass_len = pass_len;
    }
    w->join_timeout_us = (uint64_t)(timeout_ms ? timeout_ms : 15000) * 1000;
    lb_init(&l);
    lb_s(&l, "link: joining a network (name not logged): ");
    lb_s(&l, w->join_open ? "open" : (w->caps & BCMW_CAP_WPA3) ? "WPA2-PSK or WPA3-SAE (as the access point offers)"
             : w->join_fwsup ? "WPA2-PSK, the firmware's supplicant (cap: idsup)" : "WPA2-PSK, the host's supplicant");
    bcmw_say_lb(w, &l);

    /* the PMK first (it needs only the name and the passphrase; the passphrase is not kept after this call) */
    if (!w->join_open && pass_len == 64) {
        /* 64 hex digits are the PSK itself (IEEE 802.11 M.4) */
        for (unsigned i = 0; i < 64; i++) {
            char c = pass[i];
            unsigned v = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                         c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
            if (v == 16) {
                bcmw_say(w, "link: a 64-character passphrase that is not hex");
                e = BCMW_E_REFUSED;
                goto out;
            }
            w->wpa.pmk[i / 2] = (uint8_t)(w->wpa.pmk[i / 2] << 4 | v);
        }
    } else if (!w->join_open) {
        uint64_t p0 = bcmw_now(w);
        bcmw_wpa_pmk((const unsigned char *)pass, pass_len, ssid, ssid_len, w->wpa.pmk);
        lb_init(&l);
        lb_s(&l, "link: the PMK made from the passphrase (PBKDF2, 4096 rounds) in ");
        lb_dec(&l, (bcmw_now(w) - p0) / 1000);
        lb_s(&l, " ms");
        bcmw_say_lb(w, &l);
    }
    bcmw_memcpy(w->join_psk, w->wpa.pmk, 32);
    /* the directed scan: started here, its results waited for in bcmw_join_poll (Starbite OS 0.6.3: the wait blocked
     * the caller's loop for ~2.3 s) */
    if ((e = scan_begin(w)) != BCMW_OK)
        goto out;
    return BCMW_OK;
out:
    w->join_phase = 0;
    bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
    bcmw_memset(w->join_psk, 0, sizeof(w->join_psk));
    bcmw_memset(w->join_pass, 0, sizeof(w->join_pass));
    return e;
}

/* WPA3 or WPA2 for the chosen access point: the AKM, how SAE is done, management frame protection */
static void join_choose(struct bcmw *w)
{
    int ext = (w->caps & BCMW_CAP_HOST_SAE) && (w->caps & BCMW_CAP_SAE_EXT);
    int fw = (w->caps & BCMW_CAP_SAE) && (w->caps & BCMW_CAP_IDSUP);
    unsigned caps = w->join_ap_caps;
    int psk = w->join_ap_akms & AKM_BIT(2);
    w->join_akm = w->join_fwsae = w->join_mfp = 0;
    if (w->join_open)
        return;
    if ((w->join_ap_akms & AKM_BIT(8)) && (ext || fw) && !w->join_no_sae && w->join_group == BCMW_CIPHER_CCMP &&
        (w->join_group_mgmt == 0 || w->join_group_mgmt == BCMW_CIPHER_BIP_CMAC)) {
        w->join_akm = 8;
        w->join_fwsae = fw && (!ext || w->cfg.fw_supplicant);
        w->join_mfp = 2;                                /* WPA3: PMF required */
    } else if (psk || !(w->join_ap_akms & AKM_BIT(6)) || w->join_fwsup) {
        w->join_akm = 2;                                /* WPA2-PSK, as before (mfp only with cfg.wpa2_mfp) */
        if (w->cfg.wpa2_mfp && (caps & BCMW_RSN_CAP_MFPC) && (w->caps & BCMW_CAP_MFP) && !w->join_fwsup)
            w->join_mfp = 1;
    } else {
        w->join_akm = 6;                                /* PSK-SHA256 only */
        w->join_mfp = caps & BCMW_RSN_CAP_MFPR ? 2 : caps & BCMW_RSN_CAP_MFPC ? 1 : 0;
    }
    if (w->join_fwsae)
        w->join_fwsup = 1;                              /* the firmware's handshake: EAPOL frames stay there */
    else if (w->join_akm == 8 || w->join_akm == 6)
        w->join_fwsup = 0;
}

/* after the directed scan: the access point, the security settings, power save, SET_SSID */
static int join_associate(struct bcmw *w)
{
    uint8_t j[48];
    int e, fwerr = 0;
    struct bcmw_lb l;
    if (!w->join_found) {
        if (w->join_wpa3_refused) {
            struct bcmw_lb m;
            lb_init(&m);
            lb_s(&m, "link: a WPA3-only network: this firmware cannot do SAE (cap: ");
            lb_s(&m, w->caps & BCMW_CAP_SAE_EXT ? "sae_ext" : "no sae_ext");
            lb_s(&m, w->caps & BCMW_CAP_SAE ? ", sae" : ", no sae");
            lb_s(&m, w->caps & BCMW_CAP_IDSUP ? ", idsup" : ", no idsup");
            lb_s(&m, w->sae_ops ? "; the host's SAE built in)" : "; no host SAE in this build)");
            bcmw_say_lb(w, &m);
        }
        bcmw_sayx(w, "link: no suitable access point of that name; unsuitable ones heard: ", w->join_unsuitable, NULL);
        bcmw_memset(w->join_pass, 0, sizeof(w->join_pass));
        return BCMW_E_NODEV;
    }
    join_choose(w);
    lb_init(&l);
    lb_s(&l, "link: the access point ");
    for (int i = 0; i < 3; i++) {
        lb_hex(&l, w->join_bssid[i], 2);
        lb_s(&l, ":");
    }
    lb_s(&l, "..:..:.. (its maker's half), RSSI ");
    lb_sdec(&l, w->join_rssi);
    lb_s(&l, " dBm, chanspec ");
    lb_hex(&l, w->join_chanspec, 4);
    lb_s(&l, w->join_open ? ", open" : w->join_group == BCMW_CIPHER_CCMP ? ", group cipher CCMP" : ", group cipher TKIP");
    bcmw_say_lb(w, &l);
    if (w->join_akm && (w->join_akm != 2 || w->join_mfp || (w->join_ap_akms & AKM_BIT(8)))) {
        lb_init(&l);
        lb_s(&l, "link: security: ");
        lb_s(&l, w->join_akm == 8 ? (w->join_fwsae ? "WPA3-SAE, the firmware's SAE and handshake (sae, idsup)"
                                     : w->join_h2e ? "WPA3-SAE, the host's SAE (sae_ext; hash-to-element)"
                                                   : "WPA3-SAE, the host's SAE (sae_ext; hunting-and-pecking)")
                 : w->join_akm == 6 ? "WPA2-PSK-SHA256" : "WPA2-PSK");
        lb_s(&l, w->join_ap_akms & AKM_BIT(8) && w->join_akm != 8 ? " (a transition network: WPA2 instead of SAE)" : "");
        lb_s(&l, w->join_mfp == 2 ? ", PMF required" : w->join_mfp == 1 ? ", PMF capable" : "");
        bcmw_say_lb(w, &l);
    }
    {
        uint32_t v = 1;
        bcmw_ioctl(w, 0, BCMW_C_SET_INFRA, &v, 4, 1, &fwerr);
        v = w->join_akm == 8 ? AUTH_SAE : 0;
        bcmw_ioctl(w, 0, BCMW_C_SET_AUTH, &v, 4, 1, &fwerr);
    }
    e = bcmw_iovar_set_u32(w, "wsec", w->join_open ? 0 : WSEC_AES | (w->join_group == BCMW_CIPHER_TKIP ? WSEC_TKIP : 0));
    if (e == BCMW_OK)
        e = bcmw_iovar_set_u32(w, "wpa_auth", w->join_open ? 0 : w->join_akm == 8 ? WPA3_AUTH_SAE_PSK :
                                              w->join_akm == 6 ? WPA2_AUTH_PSK_SHA256 : WPA2_AUTH_PSK);
    if (e == BCMW_OK && w->join_mfp) {
        /* after wsec and wpa_auth, as brcmfmac (wsec would overwrite it) */
        e = bcmw_iovar_set_u32(w, "mfp", (uint32_t)w->join_mfp);
        if (e != BCMW_OK)
            bcmw_sayx(w, "link: the firmware refused mfp ", (uint32_t)w->join_mfp, NULL);
    }
    if (e == BCMW_OK && !w->join_open) {
        if (w->join_fwsup)
            e = bcmw_iovar_set_u32(w, "sup_wpa", 1);
        if (e == BCMW_OK && w->join_fwsae) {
            /* brcmf_set_sae_password: no RSN element of ours, the password as struct brcmf_wsec_sae_pwd_le */
            uint8_t pw[2 + 128];
            e = bcmw_iovar_set(w, "wpaie", 0, 0, &fwerr);
            bcmw_memset(pw, 0, sizeof(pw));
            put16(pw, (uint16_t)w->join_pass_len);
            bcmw_memcpy(pw + 2, w->join_pass, w->join_pass_len);
            if (e == BCMW_OK)
                e = bcmw_iovar_set(w, "sae_password", pw, sizeof(pw), &fwerr);
            bcmw_memset(pw, 0, sizeof(pw));
        } else if (e == BCMW_OK && w->join_fwsup) {
            uint8_t pmk[132];
            bcmw_memset(pmk, 0, sizeof(pmk));
            put16(pmk, 32);
            bcmw_memcpy(pmk + 4, w->wpa.pmk, 32);
            e = bcmw_ioctl(w, 0, BCMW_C_SET_WSEC_PMK, pmk, sizeof(pmk), 1, &fwerr);
            bcmw_memset(pmk, 0, sizeof(pmk));
        } else if (e == BCMW_OK) {
            /* the host's handshake: the firmware's own supplicant off, as bcmw_connect does (Starbite OS 0.6 tour:
             * associated, then no message 1 and the AP dropped the link; W2d saw the same on Windows). A firmware
             * without one refuses: fine. */
            int sup = bcmw_iovar_set_u32(w, "sup_wpa", 0);
            unsigned caps = w->join_mfp == 2 ? BCMW_RSN_CAP_MFPC | BCMW_RSN_CAP_MFPR : w->join_mfp ? BCMW_RSN_CAP_MFPC : 0;
            bcmw_sayx(w, "link: sup_wpa 0 (the host's handshake): ", sup == BCMW_OK ? 0 : 0xffffffffu,
                      sup == BCMW_OK ? " (set)" : " (refused: no firmware supplicant)");
            /* our RSN element (for WPA2 without PMF: the same bytes as before), and with hash-to-element an RSNXE
             * after it saying so (the AP checks it against the commit's method) */
            w->join_ie_len = bcmw_rsn_build(w->join_ie, w->join_group, w->join_akm ? w->join_akm : 2, caps);
            if (w->join_akm == 8 && w->join_h2e) {
                w->join_ie[w->join_ie_len++] = 244, w->join_ie[w->join_ie_len++] = 1;
                w->join_ie[w->join_ie_len++] = 0x20;    /* SAE hash-to-element (bit 5) */
            }
            bcmw_memcpy(w->rsn_ours, w->join_ie, BCMW_RSN_OURS);
            e = bcmw_iovar_set(w, "wpaie", w->join_ie, w->join_ie_len, &fwerr);
            bcmw_memcpy(w->wpa.own, w->info.mac, 6);
            bcmw_memcpy(w->wpa.peer, w->join_bssid, 6);
            random_bytes(w, w->wpa.own_nonce, 32);
            w->wpa.rsn = w->join_ie;
            w->wpa.rsn_length = w->join_ie_len;
            w->wpa.akm = w->join_akm;
            w->wpa.want_igtk = w->join_mfp && (w->join_ap_caps & BCMW_RSN_CAP_MFPC);
            if (w->join_akm != 2) {             /* (WPA2 as before: message 3's element not compared) */
                w->wpa.ap_rsn = w->join_ap_rsn;
                w->wpa.ap_rsn_length = w->join_ap_rsn_len;
                w->wpa.ap_rsnxe = w->join_ap_rsnxe_len ? w->join_ap_rsnxe : 0;
                w->wpa.ap_rsnxe_length = w->join_ap_rsnxe_len;
            }
        }
    }
    if (e == BCMW_OK && w->join_akm == 8 && !w->join_fwsae) {
        /* the host's SAE: the events on, the password element and our commit now (the firmware asks for it
         * after SET_SSID); the PMK comes from SAE */
        uint64_t t = bcmw_now(w);
        uint8_t pt[64];
        int r;
        if (!w->os.random)
            bcmw_say(w, "link: SAE: WARNING: no random source from the OS: the timing-jitter fallback makes SAE's secrets");
        sae_events_on(w);
        bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
        r = w->join_h2e ? w->sae_ops->pt(w->join_ssid, w->join_ssid_len, (const uint8_t *)w->join_pass, w->join_pass_len,
                                         0, 0, pt) : 0;
        if (r == 0)
            r = w->sae_ops->prepare(&w->sae, w->info.mac, w->join_bssid, (const uint8_t *)w->join_pass,
                                    w->join_pass_len, w->join_h2e ? pt : 0);
        bcmw_memset(pt, 0, sizeof(pt));
        bcmw_sayx(w, r == 0 ? "link: SAE: the password element and our commit made in " : "link: SAE: the password "
                  "element could not be made (no randomness?), after ", (bcmw_now(w) - t) / 1000, " ms");
        if (r != 0)
            e = BCMW_E_FW;
        w->sae_phase = 1;
    }
    bcmw_memset(w->join_pass, 0, sizeof(w->join_pass));    /* not needed from here on */
    w->join_pass_len = 0;
    if (e != BCMW_OK) {
        bcmw_sayx(w, "link: the firmware refused the security settings; its error ", (uint32_t)fwerr, NULL);
        return e;
    }
    join_pm_off(w);
    /* WLC_SET_SSID: brcmf_join_params = the name (4 + 32), then the BSSID and no channel list */
    bcmw_memset(j, 0, sizeof(j));
    put32(j, w->join_ssid_len);
    bcmw_memcpy(j + 4, w->join_ssid, w->join_ssid_len);
    bcmw_memcpy(j + 36, w->join_bssid, 6);
    w->join_phase = 2;
    w->join_t0 = bcmw_now(w);
    if ((e = bcmw_ioctl(w, 0, BCMW_C_SET_SSID, j, sizeof(j), 1, &fwerr)) != BCMW_OK) {
        bcmw_sayx(w, "link: SET_SSID refused; the firmware's error ", (uint32_t)fwerr, NULL);
        return e;
    }
    return BCMW_OK;                     /* the PMK stays for the handshake (bcmw_join_poll wipes it) */
}

/* SAE did not work on a transition network for another reason than the password: WPA2-PSK with the same AP */
static int sae_fallback(struct bcmw *w)
{
    if (w->join_akm != 8 || w->join_fwsae || w->sae_wrong || w->sae_phase == 3 || w->join_no_sae ||
        !(w->join_ap_akms & AKM_BIT(2)))
        return 0;
    bcmw_say(w, "link: SAE did not work on this transition network: WPA2-PSK with the same access point instead");
    bcmw_leave(w);                      /* (wipes the handshake's state; the PSK's PMK was kept aside) */
    w->join_no_sae = 1;
    w->sae_phase = 0;
    w->sae_rx_n = 0;
    w->join_fwsup = w->cfg.fw_supplicant && (w->caps & BCMW_CAP_IDSUP);
    bcmw_memcpy(w->wpa.pmk, w->join_psk, 32);
    if (join_associate(w) != BCMW_OK)
        return 0;
    return 1;
}

int bcmw_join_poll(struct bcmw *w)
{
    struct bcmw_lb l;
    int e;
    if (w->join_phase == 3)
        return BCMW_OK;
    if (w->join_phase == 1 && !w->dead) {
        mb_process(w);
        if (!w->join_scan_done && bcmw_now(w) - w->join_t0 < 8000000)
            return BCMW_E_PENDING;      /* the directed scan's results */
        /* wifi-core 24a0adf: 300 ms of settling after the directed scan before SET_SSID (Windows joined seconds
         * after its last scan) */
        if (!w->join_settle_t)
            w->join_settle_t = bcmw_now(w);
        if (bcmw_now(w) - w->join_settle_t < 300000)
            return BCMW_E_PENDING;
        if ((e = join_associate(w)) != BCMW_OK)
            goto out;
        return BCMW_E_PENDING;
    }
    if (w->join_phase != 2 || w->dead) {
        e = w->dead ? BCMW_E_NODEV : BCMW_E_STATE;
        goto out;
    }
    mb_process(w);
    sae_service(w);
    send_eapol(w);
    if (w->keys_due)
        install_keys(w);
    if (w->join_phase == 2 && w->link_up && (w->join_open || w->keyed)) {
        w->join_phase = 3;
        lb_init(&l);
        lb_s(&l, "link: THE NETWORK IS JOINED, ");
        lb_dec(&l, (bcmw_now(w) - w->join_t0) / 1000);
        lb_s(&l, w->join_akm == 8 ? " ms after SET_SSID (WPA3-SAE)" : " ms after SET_SSID");
        bcmw_say_lb(w, &l);
        bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
        bcmw_memset(w->join_psk, 0, sizeof(w->join_psk));
        return BCMW_OK;
    }
    if (w->join_phase != 2) {
        if (sae_fallback(w))
            return BCMW_E_PENDING;
        e = BCMW_E_FW;
        goto out;
    }
    if (bcmw_now(w) - w->join_t0 > w->join_timeout_us) {
        bcmw_say(w, w->sae_phase == 1 || w->sae_phase == 2 ? "link: TIMEOUT: SAE (the authentication) did not finish"
                    : w->link_up ? "link: TIMEOUT: associated, but the handshake did not finish"
                                 : "link: TIMEOUT: not associated");
        if (w->sae_phase == 2)
            sae_status(w, 0);
        e = BCMW_E_TIMEOUT;
        goto out;
    }
    return BCMW_E_PENDING;
out:
    if (w->join_phase != 3) {
        if (w->join_phase == 2 || w->join_phase == -1)
            bcmw_leave(w);
        w->join_phase = 0;
    }
    bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
    bcmw_memset(w->join_psk, 0, sizeof(w->join_psk));
    bcmw_memset(w->join_pass, 0, sizeof(w->join_pass));
    if (w->sae_ops)
        w->sae_ops->clear(&w->sae);
    w->sae_phase = 0;
    return e;
}

int bcmw_join(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned timeout_ms)
{
    int e = bcmw_join_start(w, ssid, ssid_len, pass, timeout_ms);
    if (e != BCMW_OK)
        return e;
    while ((e = bcmw_join_poll(w)) == BCMW_E_PENDING)
        bcmw_udelay(w, 500);
    return e;
}

int bcmw_link_up(const struct bcmw *w)
{
    return w->join_phase == 3 && w->link_up;
}

int bcmw_leave(struct bcmw *w)
{
    int fwerr;
    uint8_t scb[12];                    /* brcmf_scb_val_le: reason, then the address */
    if (w->step < BCMW_STEP_UP)
        return BCMW_E_STATE;
    bcmw_memset(scb, 0, sizeof(scb));
    put32(scb, 3);                      /* WLAN_REASON_DEAUTH_LEAVING */
    bcmw_memcpy(scb + 4, w->join_bssid, 6);
    w->join_phase = 0;
    w->link_up = w->keyed = 0;
    w->eapol_out_len = 0;
    bcmw_memset(&w->wpa, 0, sizeof(w->wpa));
    w->sae_phase = 0;
    w->sae_rx_n = 0;
    if (w->sae_ops)
        w->sae_ops->clear(&w->sae);
    return bcmw_ioctl(w, 0, C_DISASSOC, scb, sizeof(scb), 1, &fwerr);
}

/* ---- the pieces, for an OS with its own supplicant (bcmw.h) --------------------------------------------- */

/* d11ac chanspec -> the control (primary 20 MHz) channel. The low byte is the CENTER channel of a 40/80/160 MHz
 * BSS (W2: 5 GHz access points were reported on channel 42 instead of 36); bits 10:8 say which 20 MHz part is the
 * control one, counted up from the lowest. 80+80 keeps the low byte (rare; its encoding differs). */
unsigned bcmw_chanspec_channel(uint16_t cs)
{
    unsigned c = cs & 0xff, sb = (cs >> 8) & 7, half;
    switch (cs & 0x3800) {
    case 0x1800: half = 2; break;       /* 40 MHz */
    case 0x2000: half = 6; break;       /* 80 MHz */
    case 0x2800: half = 14; break;      /* 160 MHz */
    default: return c;                  /* 20 MHz (0x1000), 80+80, or unknown */
    }
    if (c < half || 4 * sb > 2 * half)
        return c;
    return c - half + 4 * sb;
}

int bcmw_bss_parse(const struct bcmw_event *ev, struct bcmw_bss *b, uint16_t *sync)
{
    const uint8_t *d = ev->data;
    uint32_t blen, ie_off, ie_len;

    if (ev->type != BCMW_E_ESCAN_RESULT || ev->datalen < 12)
        return 0;
    if (sync)
        *sync = le16(d + 8);
    if (ev->status != 8 || ev->datalen < 12 + 128 || le16(d + 10) < 1)
        return 0;
    d += 12;
    blen = le32(d + 4);
    if (blen < 128 || blen > ev->datalen - 12 || d[18] > 32)
        return 0;
    ie_off = le16(d + 116);
    ie_len = le32(d + 120);
    if (ie_off > blen || ie_len > blen - ie_off)
        return 0;
    bcmw_memset(b, 0, sizeof(*b));
    bcmw_memcpy(b->bssid, d + 8, 6);
    b->beacon_period = le16(d + 14);
    b->capability = le16(d + 16);
    b->ssid_len = d[18];
    bcmw_memcpy(b->ssid, d + 19, b->ssid_len);
    b->chanspec = le16(d + 72);
    b->channel = bcmw_chanspec_channel(b->chanspec);
    b->band5 = (b->chanspec & 0xc000) == 0xc000;   /* d11ac chanspec: band bits 15:14, 5 GHz = 3 */
    b->rssi = (int16_t)le16(d + 78);
    b->ie = d + ie_off;
    b->ie_len = ie_len;
    return 1;
}

int bcmw_scan_ended(const struct bcmw_event *ev, uint16_t sync)
{
    return ev->type == BCMW_E_ESCAN_RESULT && ev->status != 8 && ev->datalen >= 10 && le16(ev->data + 8) == sync;
}

int bcmw_scan_start(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, uint16_t sync)
{
    uint8_t p[80];
    uint32_t n;
    if (w->step < BCMW_STEP_UP || w->dead || ssid_len > 32)
        return BCMW_E_STATE;
    n = bcmw_escan_params(w, p, ssid, ssid_len, sync);
    return bcmw_iovar_set(w, "escan", p, n, 0);
}

int bcmw_connect(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const uint8_t bssid[6], const uint8_t *rsn_ie,
                 unsigned rsn_len, int group_tkip)
{
    uint8_t j[48];
    uint32_t v;
    int e, fwerr = 0;
    if (w->step < BCMW_STEP_UP || w->dead || !ssid_len || ssid_len > 32)
        return BCMW_E_STATE;
    /* the OS runs the handshake: EAPOL frames go to os->rx, never to the core's own (a bcmw_join before leaves
     * join_phase at 3, which would keep them) */
    w->join_phase = 0;
    w->join_fwsup = 0;
    v = 1;
    bcmw_ioctl(w, 0, BCMW_C_SET_INFRA, &v, 4, 1, &fwerr);
    v = 0;
    bcmw_ioctl(w, 0, BCMW_C_SET_AUTH, &v, 4, 1, &fwerr);
    e = bcmw_iovar_set_u32(w, "wsec", rsn_ie ? WSEC_AES | (group_tkip ? WSEC_TKIP : 0) : 0);
    if (e == BCMW_OK)
        e = bcmw_iovar_set_u32(w, "wpa_auth", rsn_ie ? WPA2_AUTH_PSK : 0);
    if (e == BCMW_OK && rsn_ie) {
        /* the OS runs the handshake: the firmware's own supplicant off, as bcmdhd does for a host supplicant (if
         * it ran, it would keep the AP's EAPOL frames: W2d saw none). A firmware without one refuses: fine. */
        int sup = bcmw_iovar_set_u32(w, "sup_wpa", 0);
        bcmw_sayx(w, "link: sup_wpa 0 (the OS's supplicant): ", sup == BCMW_OK ? 0 : 0xffffffffu,
                  sup == BCMW_OK ? " (set)" : " (refused: no firmware supplicant)");
        e = bcmw_iovar_set(w, "wpaie", rsn_ie, rsn_len, &fwerr);
    }
    if (e != BCMW_OK) {
        bcmw_sayx(w, "link: the firmware refused the security settings; its error ", (uint32_t)fwerr, NULL);
        return e;
    }
    join_pm_off(w);
    bcmw_memset(j, 0, sizeof(j));
    put32(j, ssid_len);
    bcmw_memcpy(j + 4, ssid, ssid_len);
    if (bssid)
        bcmw_memcpy(j + 36, bssid, 6);
    else
        bcmw_memset(j + 36, 0xff, 6);
    if (bssid)
        bcmw_memcpy(w->join_bssid, bssid, 6);
    return bcmw_ioctl(w, 0, BCMW_C_SET_SSID, j, sizeof(j), 1, &fwerr);
}

void bcmw_console_more(struct bcmw *w, unsigned n) { w->console_quota += n; }

int bcmw_set_mac(struct bcmw *w, const uint8_t mac[6])
{
    int fwerr = 0, e;
    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    e = bcmw_iovar_set(w, "cur_etheraddr", mac, 6, &fwerr);
    if (e == BCMW_OK)
        bcmw_memcpy(w->info.mac, mac, 6);
    return e;
}

int bcmw_set_key(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, unsigned algo, const uint8_t *ea,
                 const uint8_t *rsc)
{
    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    return set_key_algo(w, index, key, len, algo, ea, rsc);
}

int bcmw_assoc_ies(struct bcmw *w, uint8_t *req, uint32_t *req_len, uint8_t *resp, uint32_t *resp_len)
{
    uint8_t info[512], buf[1024];   /* assoc_info: brcmfmac asks with WL_ASSOC_INFO_MAX (512); the firmware's
                                     * struct is longer than the two lengths read here (W2d: an 8-byte ask failed) */
    uint32_t rq, rs;
    int e, fwerr = 0;
    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    bcmw_memset(info, 0, sizeof(info));
    if ((e = bcmw_iovar_get(w, "assoc_info", info, sizeof(info), &fwerr)) != BCMW_OK) {  /* brcmf_cfg80211_assoc_ielen_le */
        bcmw_sayx(w, "link: assoc_info refused; the firmware's error ", (uint32_t)fwerr, NULL);
        return e;
    }
    rq = le32(info);
    rs = le32(info + 4);
    if (rq > sizeof(buf) || rs > sizeof(buf))
        return BCMW_E_PROTO;
    if (rq && (e = bcmw_iovar_get(w, "assoc_req_ies", buf, sizeof(buf), &fwerr)) != BCMW_OK) {
        bcmw_sayx(w, "link: assoc_req_ies refused; the firmware's error ", (uint32_t)fwerr, NULL);
        return e;
    }
    if (rq > *req_len)
        rq = *req_len;
    bcmw_memcpy(req, buf, rq);
    *req_len = rq;
    if (rs && (e = bcmw_iovar_get(w, "assoc_resp_ies", buf, sizeof(buf), &fwerr)) != BCMW_OK) {
        bcmw_sayx(w, "link: assoc_resp_ies refused; the firmware's error ", (uint32_t)fwerr, NULL);
        return e;
    }
    if (rs > *resp_len)
        rs = *resp_len;
    bcmw_memcpy(resp, buf, rs);
    *resp_len = rs;
    return BCMW_OK;
}
