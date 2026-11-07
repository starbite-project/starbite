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
 *  - the firmware's own supplicant when the firmware says so ("cap" lists "idsup"): sup_wpa 1 and the PMK
 *    (WLC_SET_WSEC_PMK, 32 raw bytes); done at E_PSK_SUP status 6 (keyed);
 *  - otherwise the host's: sup_wpa 0, our RSN element as "wpaie", the four-way handshake by wpa.c on the
 *    EAPOL frames, message 2 and 4 sent as data, then the pairwise and group keys by "wsec_key"
 *    (Samsung's firmware is built without BCMSUP_4WAY_HANDSHAKE, an earlier port found; an earlier port joined this way);
 *  - an open network: wsec 0, wpa_auth 0;
 * and WLC_SET_SSID with the name and the BSSID. Events: SET_SSID (status 0 joined), LINK (flag 1 up), DEAUTH,
 * DISASSOC, PSK_SUP. The passphrase, the PMK and the keys are never logged; nor are network names or addresses
 * (only the first half of the AP's address, its maker).
 */
#include "bcmw_priv.h"
#include "wpa.h"

#define FLOW_ITEMS      512u
#define FLOW_ITEM_SIZE  48u
#define FLOW_ID         2u      /* BRCMF_H2D_MSGRING_FLOWRING_IDSTART + flow 0 */
#define FLOW_FIFO       1u      /* brcmf_flowring_prio2fifo[0]: best effort */

#define E_PSK_SUP       46
#define WSEC_AES        4u
#define WSEC_TKIP       2u
#define WPA2_AUTH_PSK   0x80u
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

/* ---- events and EAPOL while joining ----------------------------------------------------------------- */

static void scan_result(struct bcmw *w, const struct bcmw_event *ev)
{
    const uint8_t *d = ev->data;
    uint32_t blen, ie_off, ie_len;
    const uint8_t *ie, *rsn;
    unsigned group = 0, ok;
    int16_t rssi;

    if (ev->status != 8 || ev->datalen < 12 + 128 || le16(d + 10) < 1)
        return;
    d += 12;                                /* the first bss_info */
    blen = le32(d + 4);
    if (blen < 128 || 12 + blen > ev->datalen || d[18] != w->join_ssid_len ||
        bcmw_memcmp(d + 19, w->join_ssid, w->join_ssid_len))
        return;
    ie_off = le16(d + 116);
    ie_len = le32(d + 120);
    if (ie_off + ie_len > blen)
        return;
    ie = d + ie_off;
    rsn = bcmw_ie_find(ie, ie_len, 48);
    if (w->join_open)
        ok = !rsn && !(le16(d + 16) & 0x0010);  /* no RSN, privacy off */
    else
        ok = rsn && bcmw_rsn_check(rsn, 2u + rsn[1], &group) == 0 && (group == BCMW_CIPHER_CCMP || w->join_fwsup);
    rssi = (int16_t)le16(d + 78);
    if (!ok) {
        w->join_unsuitable++;
        return;
    }
    if (!w->join_found || rssi > w->join_rssi) {
        w->join_found = 1;
        w->join_rssi = rssi;
        bcmw_memcpy(w->join_bssid, d + 8, 6);
        w->join_group = group;
        w->join_chanspec = le16(d + 72);
    }
}

void link_event(struct bcmw *w, const struct bcmw_event *ev)
{
    if (ev->type == BCMW_E_ESCAN_RESULT && w->join_phase == 1) {
        if (ev->status == 8)
            scan_result(w, ev);
        else
            w->join_scan_done = 1;
        return;
    }
    if (w->join_phase < 2)
        return;
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

static int set_key(struct bcmw *w, unsigned index, const uint8_t *key, unsigned len, const uint8_t *ea,
                   const uint8_t *rsc)
{
    uint8_t k[164];
    int fwerr = 0, e;
    bcmw_memset(k, 0, sizeof(k));
    put32(k + 0, index);
    put32(k + 4, len);
    bcmw_memcpy(k + 8, key, len);
    put32(k + 112, ALGO_AES_CCM);
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

    if (w->join_phase != 2 || w->join_fwsup || w->join_open || len < 14 + 4)
        return;
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

static void send_eapol(struct bcmw *w)
{
    int r = w->eapol_out_kind;
    if (!w->eapol_out_len)
        return;
    if (bcmw_tx(w, w->eapol_out, w->eapol_out_len) != BCMW_OK) {
        bcmw_say(w, "link: the handshake's answer could not be sent");
        w->join_phase = -1;
    } else if (r == BCMW_WPA_KEYS || r == BCMW_WPA_GROUP) {
        w->keys_due = r;
    }
    bcmw_memset(w->eapol_out, 0, sizeof(w->eapol_out));
    w->eapol_out_len = 0;
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
    if (a == BCMW_OK && b == BCMW_OK) {
        w->keyed = 1;
        bcmw_say(w, "link: both keys given to the firmware (keyed)");
    } else {
        bcmw_say(w, "link: the firmware refused a key");
        w->join_phase = -1;
    }
}

/* ---- the join ------------------------------------------------------------------------------------------ */

static int has_idsup(struct bcmw *w)
{
    char cap[512];
    bcmw_memset(cap, 0, sizeof(cap));
    if (bcmw_iovar_get(w, "cap", cap, sizeof(cap) - 1, 0) != BCMW_OK)
        return 0;
    for (unsigned i = 0; cap[i]; i++)
        if ((i == 0 || cap[i - 1] == ' ') && bcmw_strneq(cap + i, "idsup", 5) && (cap[i + 5] == ' ' || !cap[i + 5]))
            return 1;
    return 0;
}

static int scan_for(struct bcmw *w)
{
    uint8_t p[80];
    uint64_t t0;
    int e;

    uint32_t n = bcmw_escan_params(w, p, w->join_ssid, w->join_ssid_len, 0x4a4e);
    w->join_phase = 1;
    w->join_found = 0;
    w->join_scan_done = 0;
    w->join_unsuitable = 0;
    if ((e = bcmw_iovar_set(w, "escan", p, n, 0)) != BCMW_OK)
        return e;
    t0 = bcmw_now(w);
    while (!w->join_scan_done && bcmw_now(w) - t0 < 8000000) {
        mb_process(w);
        bcmw_udelay(w, 1000);
    }
    return BCMW_OK;
}

int bcmw_join(struct bcmw *w, const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned timeout_ms)
{
    unsigned pass_len = bcmw_strlen(pass);
    uint8_t j[48];
    int e, fwerr = 0;
    uint64_t t0;
    struct bcmw_lb l;

    if (w->step < BCMW_STEP_UP || w->dead)
        return BCMW_E_STATE;
    if (ssid_len == 0 || ssid_len > 32 || (pass_len && (pass_len < 8 || pass_len > 63)))
        return BCMW_E_REFUSED;
    bcmw_memset(&w->wpa, 0, sizeof(w->wpa));
    bcmw_memcpy(w->join_ssid, ssid, ssid_len);
    w->join_ssid_len = (uint8_t)ssid_len;
    w->join_open = pass_len == 0;
    w->link_up = w->keyed = 0;
    w->keys_due = 0;
    w->join_fwsup = !w->join_open && !w->cfg.host_supplicant && has_idsup(w);
    lb_init(&l);
    lb_s(&l, "link: joining a network (name not logged): ");
    lb_s(&l, w->join_open ? "open" : w->join_fwsup ? "WPA2-PSK, the firmware's supplicant (cap: idsup)"
                                                   : "WPA2-PSK, the host's supplicant (no idsup in cap)");
    bcmw_say_lb(w, &l);

    if ((e = scan_for(w)) != BCMW_OK)
        goto out;
    if (!w->join_found) {
        bcmw_sayx(w, "link: no suitable access point of that name; unsuitable ones heard: ", w->join_unsuitable, NULL);
        e = BCMW_E_NODEV;
        goto out;
    }
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

    if (!w->join_open) {
        uint64_t p0 = bcmw_now(w);
        bcmw_wpa_pmk((const unsigned char *)pass, pass_len, ssid, ssid_len, w->wpa.pmk);
        lb_init(&l);
        lb_s(&l, "link: the PMK made from the passphrase (PBKDF2, 4096 rounds) in ");
        lb_dec(&l, (bcmw_now(w) - p0) / 1000);
        lb_s(&l, " ms");
        bcmw_say_lb(w, &l);
    }
    {
        uint32_t v = 1;
        bcmw_ioctl(w, 0, BCMW_C_SET_INFRA, &v, 4, 1, &fwerr);
        v = 0;
        bcmw_ioctl(w, 0, BCMW_C_SET_AUTH, &v, 4, 1, &fwerr);
    }
    e = bcmw_iovar_set_u32(w, "wsec", w->join_open ? 0 : WSEC_AES | (w->join_group == BCMW_CIPHER_TKIP ? WSEC_TKIP : 0));
    if (e == BCMW_OK)
        e = bcmw_iovar_set_u32(w, "wpa_auth", w->join_open ? 0 : WPA2_AUTH_PSK);
    if (e == BCMW_OK && !w->join_open) {
        e = bcmw_iovar_set_u32(w, "sup_wpa", w->join_fwsup ? 1 : 0);
        if (e == BCMW_OK && w->join_fwsup) {
            uint8_t pmk[132];
            bcmw_memset(pmk, 0, sizeof(pmk));
            put16(pmk, 32);
            bcmw_memcpy(pmk + 4, w->wpa.pmk, 32);
            e = bcmw_ioctl(w, 0, BCMW_C_SET_WSEC_PMK, pmk, sizeof(pmk), 1, &fwerr);
            bcmw_memset(pmk, 0, sizeof(pmk));
        } else if (e == BCMW_OK) {
            bcmw_rsn_ours(w->rsn_ours, w->join_group);
            e = bcmw_iovar_set(w, "wpaie", w->rsn_ours, BCMW_RSN_OURS, &fwerr);
            bcmw_memcpy(w->wpa.own, w->info.mac, 6);
            bcmw_memcpy(w->wpa.peer, w->join_bssid, 6);
            random_bytes(w, w->wpa.own_nonce, 32);
            w->wpa.rsn = w->rsn_ours;
            w->wpa.rsn_length = BCMW_RSN_OURS;
        }
    }
    if (e != BCMW_OK) {
        bcmw_sayx(w, "link: the firmware refused the security settings; its error ", (uint32_t)fwerr, NULL);
        goto out;
    }
    /* WLC_SET_SSID: brcmf_join_params = the name (4 + 32), then the BSSID and no channel list */
    bcmw_memset(j, 0, sizeof(j));
    put32(j, ssid_len);
    bcmw_memcpy(j + 4, ssid, ssid_len);
    bcmw_memcpy(j + 36, w->join_bssid, 6);
    w->join_phase = 2;
    t0 = bcmw_now(w);
    if ((e = bcmw_ioctl(w, 0, BCMW_C_SET_SSID, j, sizeof(j), 1, &fwerr)) != BCMW_OK) {
        bcmw_sayx(w, "link: SET_SSID refused; the firmware's error ", (uint32_t)fwerr, NULL);
        goto out;
    }
    while (w->join_phase == 2 && !w->dead) {
        mb_process(w);
        send_eapol(w);
        if (w->keys_due)
            install_keys(w);
        if (w->link_up && (w->join_open || w->keyed)) {
            w->join_phase = 3;
            break;
        }
        if (bcmw_now(w) - t0 > (uint64_t)(timeout_ms ? timeout_ms : 15000) * 1000) {
            bcmw_say(w, w->link_up ? "link: TIMEOUT: associated, but the handshake did not finish"
                                   : "link: TIMEOUT: not associated");
            e = BCMW_E_TIMEOUT;
            goto out;
        }
        bcmw_udelay(w, 500);
    }
    if (w->join_phase != 3) {
        e = BCMW_E_FW;
        goto out;
    }
    lb_init(&l);
    lb_s(&l, "link: THE NETWORK IS JOINED, ");
    lb_dec(&l, (bcmw_now(w) - t0) / 1000);
    lb_s(&l, " ms after SET_SSID");
    bcmw_say_lb(w, &l);
    bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
    return BCMW_OK;
out:
    if (w->join_phase != 3)
        w->join_phase = 0;
    bcmw_memset(w->wpa.pmk, 0, sizeof(w->wpa.pmk));
    return e;
}

int bcmw_link_up(const struct bcmw *w)
{
    return w->join_phase == 3 && w->link_up;
}

int bcmw_leave(struct bcmw *w)
{
    int fwerr;
    uint8_t ea[6];
    if (w->step < BCMW_STEP_UP)
        return BCMW_E_STATE;
    bcmw_memcpy(ea, w->join_bssid, 6);
    w->join_phase = 0;
    w->link_up = w->keyed = 0;
    bcmw_memset(&w->wpa, 0, sizeof(w->wpa));
    return bcmw_ioctl(w, 0, C_DISASSOC, ea, 6, 1, &fwerr);
}
