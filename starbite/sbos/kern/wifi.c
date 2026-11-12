/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Wi-Fi (docs/STARBITE-OS.md 9.4), 0.4: the BCM4361 through the portable core starbite/wifi (bcmw, the
 * wifi-core session's) and its IPv4 layer (sbnet): the radio on and off at the user's word, a scan, joining an
 * OPEN network, DHCP, the gateway pinged. Secured networks are listed with a lock (their passphrase entry comes
 * later).
 *
 * The loop is not blocked for long except by the bring-up: it runs one step per tick, but step 8 reads the
 * firmware from VENDOR (the first time) and waits for it to start (up to 5 s), steps 9 and 10 send commands that
 * may each wait up to 2 s; the UI stalls meanwhile (the heater is never on then). Scans and joins are the
 * non-blocking calls (bcmw_scan_start, bcmw_connect) with events; the first transmitted frame waits for the tx
 * flow ring (up to 2 s, once). Heating and Wi-Fi do not run together in 0.4: the
 * heater's 250 ms safety ticks must never wait behind the radio (each refuses while the other is on).
 *
 * Writes (on the phone): only what bcmw_write_allowed() lists (PCIe channel 0, the chip's windows, single words of
 * the PMU, CMU_FSYS1, SYSREG_FSYS1, three GPIO banks); the radio's files are read from VENDOR, never written.
 * Logged: states, counts and times only: no network names, no addresses (the screen shows them, the pmsg ring
 * does not). Off: bcmw_quiesce (the chip in reset and unpowered).
 *
 * Next (wifi-sbos-next): TCP (sbtcp), HTTPS (sbhttp on BearSSL, Mozilla's CA roots: starbite/wifi/tls) for one fetch
 * at a time (wifi_fetch), its TLS randomness from an entropy pool (see ent_*), and a scan-only self test with the
 * offline TLS self-check. The fetch's URL and the body are never logged; the log has steps, sizes and times. */
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "bcmw.h"
#include "bcmw_priv.h"                      /* w->dead: the chip stopped answering */
#include "net.h"
#include "tcp.h"
#include "netio.h"
#include "http.h"
#include "tls/tls_check.h"
#include "store.h"
#ifndef SB_HOST
#include "sd.h"
#endif
#ifdef SB_HOST
#include <stdlib.h>
#include "host/model.h"
#else
#include "sb/bcmw_sb.h"
#endif

/* Track G's guest NAT (kern/guestnet.c on claude/sbos-calls): its two hooks are built when its kern.h is merged in
 * (KNET_UPLINK_WIFI / SBOS_HAVE_GUESTNET defined); without it this file builds as before */
#if defined(KNET_UPLINK_WIFI) || defined(SBOS_HAVE_GUESTNET)
#define WIFI_GUESTNET 1
#endif

#define TICK_ON_MS 20U                      /* the core's poll while the screen is on */
#define TICK_OFF_MS 200U                    /* ... and off */
#define SCAN_MS 8000U
#define JOIN_MS 15000U
#define DHCP_MS 20000U

static struct bcmw *w;
static struct bcmw_os os;
static struct bcmw_cfg cfg;
static struct sbnet net;
static int st = SBOS_WIFI_OFF, step;
static uint64_t t_state, t_on;
static char why[64];
static struct sbos_wifi_net nets[SBOS_WIFI_NETS];
static int nnets, scan_heard;
static uint16_t sync;
static int joined = -1;
static int pinged;
/* 0.5.8: the time over the network (NTP, pool.ntp.org) once joined: 0 idle, 1 DNS, 2 asked, 3 done (checked),
 * -1 no answer */
#define NTP_PORT 50123
static int ntp_st, clock_checked;
static int32_t clock_off;
static uint64_t ntp_t;
static uint8_t ntp_nonce[8], ntp_server[4];
/* the saved network (0.4.2): the store entry "wifinet" (Track H: UFS's store first, then the card's: the OS store
 * moved to the card on 2026-11-11; h1 looked on UFS only and missed it), two lines (name, WPA2 passphrase or empty), made on the
 * Mac from the user's input for the Wi-Fi tests; read only, never logged, never shown (the passphrase) */
static uint8_t sv_ssid[33];
static unsigned sv_len;
static char sv_pass[65];
static int sv_ok, join_pending, join_active;   /* 0.6.1: join_active: the handshake runs, one round per tick */
static uint64_t join_t0;
/* 0.5: networks joined with a typed passphrase (never logged). Track H: up to 8, most recently joined first, kept
 * across a restart in the state slot (record "wifi.known", saved with the settings: wifi_state_put / _get); each
 * with its own automatic join and whether it is hidden (joined by its name). The passphrases are stored as typed:
 * the slot is the phone's own storage, not encrypted (as a hotspot list on a phone without a secure element). */
#define REMEMBER 8
static struct { uint8_t ssid[33]; unsigned len; char pass[65]; uint8_t hidden, noauto, sec; } rem[REMEMBER];
static unsigned nrem;
static int user_on;                         /* Track H: the user's Wi-Fi switch, saved (on at the next start) */
static int surv_off, surv_was_on;           /* Track H: Survival mode holds Wi-Fi off (wifi_survival) */
static int pm_set;                          /* Track H: power save (PM 2) set after the join */
static int join_hidden;                     /* Track H: the pending join is by name (the network may be hidden) */
/* Track H: back by itself after a drop (2 s, 5 s, 15 s, 30 s, then every 60 s), and a background scan for a
 * remembered network while on and not joined (every 60 s with the screen on, 5 min off) */
static uint64_t reconnect_at, bg_scan_at, rssi_at;
static unsigned revive_n;                   /* Track H: the chip stopped answering: powered up again (twice a boot) */
static unsigned reconnect_n;
static void reconnect_later(void);
static int time_only;                       /* (1.2-alpha26, below) the boot's time check */
static char jp_pass[65];                    /* the passphrase of the pending join */

static void logn(const char *a, uint64_t n, const char *b);
static const char *known_pass(const uint8_t *ssid, unsigned len)
{
    for (unsigned i = 0; i < nrem; ++i)
        if (rem[i].len == len && !memcmp(rem[i].ssid, ssid, len))
            return rem[i].pass;         /* "" for an open network that was joined */
    if (sv_ok && sv_len == len && !memcmp(sv_ssid, ssid, len))
        return sv_pass;
    return 0;
}

/* 1.2-alpha21: join a known network the scan heard, once per power-on (Wi-Fi > Known networks; on by default) */
int wifi_autojoin = 1;
int wifi_pm = 2;                            /* Track H: power save once joined (PM 2); 0 never */
static int auto_pending, auto_done;
static int wifi_in_selftest(void)
{
#ifdef SB_HOST
    if (getenv("SBOS_HOST_SELFTEST"))
        return 1;
#endif
    return sbos_selftest_enabled;           /* the self test's Wi-Fi step joins by itself */
}

static void remember(const uint8_t *ssid, unsigned len, const char *pass, int hidden, int sec)
{
    unsigned i = 0;
    while (i < nrem && !(rem[i].len == len && !memcmp(rem[i].ssid, ssid, len)))
        ++i;
    uint8_t noauto = i < nrem ? rem[i].noauto : 0;
    if (i == nrem) {
        if (nrem < REMEMBER)
            ++nrem;
        i = nrem - 1;                       /* full: the least recently joined goes */
    }
    for (; i > 0; --i)                      /* to the front: most recently joined first */
        rem[i] = rem[i - 1];
    memset(&rem[0], 0, sizeof(rem[0]));
    memcpy(rem[0].ssid, ssid, len);
    rem[0].len = len;
    sb_strcpy(rem[0].pass, sizeof(rem[0].pass), pass ? pass : "");
    rem[0].hidden = (uint8_t)(hidden != 0);
    rem[0].noauto = noauto;
    rem[0].sec = (uint8_t)(sec < 0 ? KWIFI_SEC_OTHER : sec);
    settings_changed();                     /* saved within seconds (the state slot) */
}

static int rem_find(const uint8_t *ssid, unsigned len)
{
    for (unsigned i = 0; i < nrem; ++i)
        if (rem[i].len == len && !memcmp(rem[i].ssid, ssid, len))
            return (int)i;
    return -1;
}

/* ---- Track H: the record "wifi.known" in the state slot -------------------------------------------------------
 * 'W' 'K' 'N' '2', flags (bit 0 the user's switch on), count, then per network: name length, name (32), passphrase
 * length, passphrase (64), bit 0 hidden, bit 1 no automatic join, security (KWIFI_SEC_*). 8 x 100 + 8 bytes. */
#define KREC_ENT 100U
int wifi_state_put(struct state_buf *b)
{
    static uint8_t r[8 + REMEMBER * KREC_ENT];
    memset(r, 0, sizeof(r));
    memcpy(r, "WKN2", 4);
    r[4] = (uint8_t)(user_on != 0);
    r[5] = (uint8_t)nrem;
    for (unsigned i = 0; i < nrem; ++i) {
        uint8_t *e = r + 8 + i * KREC_ENT;
        unsigned pl = sb_strlen(rem[i].pass);
        e[0] = (uint8_t)rem[i].len;
        memcpy(e + 1, rem[i].ssid, rem[i].len);
        e[33] = (uint8_t)pl;
        memcpy(e + 34, rem[i].pass, pl);
        e[98] = (uint8_t)(rem[i].hidden | rem[i].noauto << 1);
        e[99] = rem[i].sec;
    }
    int k = state_add(b, "wifi.known", r, 8 + nrem * KREC_ENT);
    memset(r, 0, sizeof(r));
    return k;
}

void wifi_state_get(const uint8_t *half)
{
    const uint8_t *d = 0;
    int n = state_find(half, "wifi.known", &d);
    if (n < 8 || !d || memcmp(d, "WKN2", 4))
        return;
    unsigned c = d[5];
    if (c > REMEMBER || (unsigned)n < 8 + c * KREC_ENT)
        return;
    nrem = 0;
    for (unsigned i = 0; i < c; ++i) {
        const uint8_t *e = d + 8 + i * KREC_ENT;
        unsigned sl = e[0], pl = e[33];
        if (!sl || sl > 32 || pl > 64 || (pl && pl < 8))
            continue;                       /* not usable: skipped (never logged) */
        memset(&rem[nrem], 0, sizeof(rem[nrem]));
        memcpy(rem[nrem].ssid, e + 1, sl);
        rem[nrem].len = sl;
        memcpy(rem[nrem].pass, e + 34, pl);
        rem[nrem].pass[pl] = 0;
        rem[nrem].hidden = e[98] & 1U;
        rem[nrem].noauto = (e[98] >> 1) & 1U;
        rem[nrem].sec = e[99];
        ++nrem;
    }
    user_on = d[4] & 1;
    logn("wifi: remembered networks restored: ", nrem, user_on ? " (names not logged); Wi-Fi was on" :
                                                                " (names not logged); Wi-Fi was off");
}
static int w_ready;
/* 0.5: the chip's INTx (SPI 253 = INTID 285); polling stays as the fallback and relaxes once interrupts arrive */
#define WIFI_INTID 285U
static int irq_on, irq_kicked, irq_trusted;
static int scan_retry;                      /* a refused scan: tries left */
static uint64_t scan_retry_at;
static unsigned irq_n, irq_window_n, irq_storms, irq_missed;
static uint64_t irq_window_t;
static void irq_stop(void);                         /* bcmw_init succeeded on w (quiesce and leave only then) */
static int dhcp_go;                         /* the link came up inside a callback: DHCP starts from the tick */
static void net_tcp(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len);
static void net_down(void);                 /* Next: sbnet stopped and every TCP connection ended */
static void ent_tick(void);
static void ent_add(const void *p, uint32_t n);
static void fetch_poll(void);
static int fx_state;

/* ---- DMA memory reused across on / off (the review of 0.4: bcmw_init forgets the core's blocks, and the shim's
 * allocator never frees): the core asks for the same sizes in the same order every bring-up, so the n-th request
 * of a power-on gets the n-th block of the first one (zeroed); a request that does not match gets a new block. */
#define DMA_KEEP 48
static struct { uint32_t size, align; void *p; uint64_t bus; } dk[DMA_KEEP];
static unsigned ndk, dk_next, dk_new, dk_reused;
static void *(*dma_first)(void *ctx, uint32_t size, uint32_t align, uint64_t *bus);

static void *dma_reuse(void *ctx, uint32_t size, uint32_t align, uint64_t *bus)
{
    if (dk_next < ndk && dk[dk_next].size == size && dk[dk_next].align == align) {
        memset(dk[dk_next].p, 0, size);
        ++dk_reused;
        *bus = dk[dk_next].bus;
        return dk[dk_next++].p;
    }
    void *p = dma_first(ctx, size, align, bus);
    ++dk_new;
    if (p && dk_next < DMA_KEEP) {
        dk[dk_next].size = size;
        dk[dk_next].align = align;
        dk[dk_next].p = p;
        dk[dk_next].bus = *bus;
        ++dk_next;
        if (ndk < dk_next)
            ndk = dk_next;
    }
    return p;
}

int wifi_on(void)
{
    return st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED;
}

static void notify(void)
{
    ui_mark_dirty();
    int a = app_by_id("wifi");
    if (a >= 0)
        app_event_send(a, EV_APP_SERVICE, 3, st);
}

static void set_state(int s, const char *w_)
{
    st = s;
    t_state = sb_ms();
    sb_strcpy(why, sizeof(why), w_ ? w_ : "");
    notify();
}

static void logn(const char *a, uint64_t n, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_dec(&u, n);
    ub_s(&u, b);
    sb_log(u.b);
}

/* ---- the core's callbacks ------------------------------------------------------------------------------- */

static void os_log(void *ctx, const char *line)
{
    (void)ctx;
    /* the firmware's console ("fw: <text>") stays out of the pmsg ring; the core's own step errors ("fw: 8: ...")
     * go in */
    if (sb_prefix(line, "fw: ") && !(line[4] >= '0' && line[4] <= '9' && line[5] == ':'))
        return;
    sb_logf("wifi: ", line);
}

static void load_saved(void)
{
    static uint8_t buf[4096] __attribute__((aligned(4096)));
    unsigned i = 0, n = 0;
    sv_ok = 0;
#ifdef SB_HOST
    unsigned len = 0;
    const char *h = getenv("SBOS_HOST_WIFINET");    /* "name\npassphrase" */
    if (!h)
        return;
    while (h[len] && len + 1 < sizeof(buf))
        buf[len] = (uint8_t)h[len], ++len;
    buf[len] = 0;
#else
    struct store_ent e;
    int found = store_find("wifinet", &e);
    if (!found) {
        /* h1b on the phone: a card-only boot (sbstore=card) has no UFS store, so libsb never opens the card's (it waits
         * for the UFS settings' SET_SD_STORE, as before the store moved to the card). Starbite OS reads the card anyway
         * (Files): allow it, forget the first miss, look there. */
        static int card_tried;
        if (!card_tried) {
            card_tried = 1;
            if (!sd_allowed()) {
                sd_allow();
                store_reset(STORE_SD);
            }
            found = store_find("sd:wifinet", &e);
            sb_log(found ? "wifi: the saved network entry is on the card's store" :
                           "wifi: no saved network entry (wifinet) on UFS's store or the card's");
        }
    }
    if (!found || !e.size || e.size >= sizeof(buf) || store_load(&e, (uintptr_t)buf, sizeof(buf)))
        return;
    buf[e.size] = 0;
#endif
    while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && n < 32)
        sv_ssid[n++] = buf[i++];
    sv_len = n;
    while (buf[i] == '\r' || buf[i] == '\n')
        ++i;
    n = 0;
    while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && n < 64)
        sv_pass[n++] = (char)buf[i++];
    sv_pass[n] = 0;
    memset(buf, 0, sizeof(buf));
    sv_ok = sv_len > 0 && (n == 0 || (n >= 8 && n <= 64));
    sb_log(sv_ok ? "wifi: a saved network is set (name and passphrase not logged)" :
                   "wifi: the saved network entry is not usable (name, or a passphrase of 8..64 characters)");
}

/* ---- Track H: what a heard network asks for (its RSN / WPA elements), the saved network's bands ------------- */
static int8_t nsec[SBOS_WIFI_NETS];         /* KWIFI_SEC_* of nets[i] (kept in step with nets[]) */
static int sv_heard24, sv_heard5, sv_rssi24, sv_rssi5;

static int bss_sec(const struct bcmw_bss *b)
{
    const uint8_t *p = b->ie, *end = b->ie ? b->ie + b->ie_len : 0;
    int rsn = 0, wpa1 = 0, ent = 0, psk = 0, sae = 0, owe = 0;
    while (p && p + 2 <= end && p + 2 + p[1] <= end) {
        const uint8_t *d = p + 2;
        unsigned n = p[1];
        if (p[0] == 48 && n >= 8) {         /* RSN: version, group, pairwise list, AKM list */
            rsn = 1;
            unsigned o = 6, pc = d[o] | (unsigned)d[o + 1] << 8;
            o += 2 + 4 * pc;
            if (o + 2 <= n) {
                unsigned ac = d[o] | (unsigned)d[o + 1] << 8;
                o += 2;
                for (unsigned k = 0; k < ac && o + 4 <= n; ++k, o += 4) {
                    if (d[o] != 0x00 || d[o + 1] != 0x0f || d[o + 2] != 0xac)
                        continue;
                    unsigned a = d[o + 3];
                    if (a == 1 || a == 3 || a == 5 || a == 11 || a == 12 || a == 13)
                        ent = 1;
                    else if (a == 2 || a == 4 || a == 6)
                        psk = 1;
                    else if (a == 8 || a == 9 || a == 24 || a == 25)
                        sae = 1;
                    else if (a == 18)
                        owe = 1;
                }
            }
        } else if (p[0] == 221 && n >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 1) {
            wpa1 = 1;
        }
        p += 2 + n;
    }
    if (rsn)
        return sae && psk ? KWIFI_SEC_WPA23 : sae ? KWIFI_SEC_WPA3 : psk ? KWIFI_SEC_WPA2 : ent ? KWIFI_SEC_ENT :
               owe ? KWIFI_SEC_OWE : KWIFI_SEC_OTHER;
    if (wpa1)
        return KWIFI_SEC_WPA1;
    return (b->capability & 0x0010) ? KWIFI_SEC_WEP : KWIFI_SEC_OPEN;
}

static void add_bss(const struct bcmw_bss *b)
{
    if (!b->ssid_len)
        return;                             /* hidden networks are not listed */
    if (sv_ok && b->ssid_len == sv_len && !memcmp(b->ssid, sv_ssid, sv_len)) {
        if (b->band5 && (!sv_heard5 || b->rssi > sv_rssi5))
            sv_heard5 = 1, sv_rssi5 = b->rssi;
        if (!b->band5 && (!sv_heard24 || b->rssi > sv_rssi24))
            sv_heard24 = 1, sv_rssi24 = b->rssi;
    }
    int secure = (b->capability & 0x0010) != 0;
    int sec = bss_sec(b);
    for (int i = 0; i < nnets; ++i)
        if (nets[i].ssid_len == b->ssid_len && !memcmp(nets[i].ssid, b->ssid, b->ssid_len)) {
            if (b->rssi > nets[i].rssi) {   /* the strongest access point of a name */
                nets[i].rssi = b->rssi;
                memcpy(nets[i].bssid, b->bssid, 6);
                nets[i].channel = (int32_t)b->channel;
                nets[i].band5 = (int32_t)b->band5;
                nsec[i] = (int8_t)sec;
            }
            return;
        }
    int at = nnets;
    if (nnets == SBOS_WIFI_NETS) {          /* full: replace the weakest if this one is stronger */
        at = 0;
        for (int i = 1; i < nnets; ++i)
            if (nets[i].rssi < nets[at].rssi)
                at = i;
        if (nets[at].rssi >= b->rssi)
            return;
    } else {
        ++nnets;
    }
    struct sbos_wifi_net *n = &nets[at];
    memset(n, 0, sizeof(*n));
    memcpy(n->ssid, b->ssid, b->ssid_len);
    n->ssid_len = b->ssid_len;
    memcpy(n->bssid, b->bssid, 6);
    n->rssi = b->rssi;
    n->secure = secure;
    n->saved = secure && known_pass(b->ssid, b->ssid_len) != 0;
    n->channel = (int32_t)b->channel;
    n->band5 = (int32_t)b->band5;
    nsec[at] = (int8_t)sec;
}

static void sort_nets(void)
{
    for (int i = 1; i < nnets; ++i)         /* strongest first */
        for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; --j) {
            struct sbos_wifi_net t = nets[j];
            nets[j] = nets[j - 1];
            nets[j - 1] = t;
            int8_t q = nsec[j];
            nsec[j] = nsec[j - 1];
            nsec[j - 1] = q;
        }
}

/* Track H (h1 on the phone heard 2-4 names where the core's own wifitest hears 35-42 access points): what each
 * scan really got, counts only: results, distinct access points and channels per band, hidden ones, results of
 * another scan or that did not parse, how it ended and when */
#define SC_AP 96
static struct {
    unsigned res, other, unparsed, hidden, extra, nap, ap5, ch24, ch5;
    uint32_t ch24m;
    uint64_t ch5m, t0;
    uint8_t ap[SC_AP][6];
} sc;

static void sc_reset(void)
{
    memset(&sc, 0, sizeof(sc));
    sc.t0 = sb_ms();
}

static void sc_add(const struct bcmw_event *ev, const struct bcmw_bss *b)
{
    ++sc.res;
    if (ev->datalen >= 12) {
        unsigned cnt = (unsigned)ev->data[10] | (unsigned)ev->data[11] << 8;
        if (cnt > 1)
            sc.extra += cnt - 1;            /* bss_infos after the first in one event (not parsed) */
    }
    if (!b->ssid_len)
        ++sc.hidden;
    if (b->band5) {
        unsigned k = b->channel >= 32 && b->channel < 288 ? (b->channel - 32) / 4 : 63;
        if (!(sc.ch5m >> k & 1))
            sc.ch5m |= 1ULL << k, ++sc.ch5;
    } else if (b->channel >= 1 && b->channel <= 14 && !(sc.ch24m >> b->channel & 1)) {
        sc.ch24m |= 1U << b->channel, ++sc.ch24;
    }
    for (unsigned i = 0; i < sc.nap && i < SC_AP; ++i)
        if (!memcmp(sc.ap[i], b->bssid, 6))
            return;
    if (sc.nap < SC_AP)
        memcpy(sc.ap[sc.nap], b->bssid, 6);   /* RAM only, never logged */
    ++sc.nap;
    sc.ap5 += b->band5 != 0;
}

static void sc_log(const char *how, unsigned status)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "wifi: scan: ");
    ub_dec(&u, sc.res);
    ub_s(&u, " results, ");
    ub_dec(&u, sc.nap);
    ub_s(&u, " access points (");
    ub_dec(&u, sc.nap - sc.ap5);
    ub_s(&u, " on 2.4 GHz over ");
    ub_dec(&u, sc.ch24);
    ub_s(&u, " channels, ");
    ub_dec(&u, sc.ap5);
    ub_s(&u, " on 5 GHz over ");
    ub_dec(&u, sc.ch5);
    ub_s(&u, " channels), hidden ");
    ub_dec(&u, sc.hidden);
    ub_s(&u, ", another scan's ");
    ub_dec(&u, sc.other);
    ub_s(&u, ", unparsed ");
    ub_dec(&u, sc.unparsed);
    ub_s(&u, ", extra in one event ");
    ub_dec(&u, sc.extra);
    ub_s(&u, "; ");
    ub_s(&u, how);
    ub_s(&u, " status ");
    ub_dec(&u, status);
    ub_s(&u, " after ");
    ub_dec(&u, sb_ms() - sc.t0);
    ub_s(&u, " ms");
    sb_log(u.b);
}

static void os_event(void *ctx, const struct bcmw_event *ev)
{
    (void)ctx;
    struct bcmw_bss b;
    uint16_t s = 0;
    ent_tick();
    ent_add(ev, sizeof(*ev));
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        if (bcmw_bss_parse(ev, &b, &s)) {
            if (st == SBOS_WIFI_SCANNING && s != sync)
                ++sc.other;
            if (st != SBOS_WIFI_SCANNING || s != sync)
                return;                     /* a late result or another scan's: the list stays as shown */
            ++scan_heard;
            sc_add(ev, &b);
            add_bss(&b);
        } else if (st == SBOS_WIFI_SCANNING && ev->status == 8) {
            ++sc.unparsed;                  /* a result that did not parse */
        } else if (st == SBOS_WIFI_SCANNING && bcmw_scan_ended(ev, sync)) {
            sc_log("ended by the firmware,", (unsigned)ev->status);
            sort_nets();
            logn("wifi: the scan heard ", (uint64_t)nnets, " networks (names not logged)");
            set_state(SBOS_WIFI_UP, "");
            auto_pending = wifi_autojoin && !auto_done && joined < 0 && !wifi_in_selftest();
        }
        return;
    }
    if (st != SBOS_WIFI_JOINING && st != SBOS_WIFI_DHCP && st != SBOS_WIFI_CONNECTED)
        return;
    if (ev->type == BCMW_E_SET_SSID && ev->status) {
        sb_logx("wifi: the join was refused, status ", ev->status);
        set_state(SBOS_WIFI_UP, "The network did not let the phone join");
        joined = -1;
    } else if (ev->type == BCMW_E_LINK) {
        if (ev->flags & 1) {
            if (st == SBOS_WIFI_JOINING && (join_pending || join_active)) {
                /* 0.6.1: a secured join: associated is not joined; the address only after the handshake (0.6 asked
                 * for it here, before the keys) */
                logn("wifi: associated after ", sb_ms() - t_state, " ms; the handshake next");
            } else if (st == SBOS_WIFI_JOINING) {
                logn("wifi: the link is up after ", sb_ms() - t_state, " ms; asking for an address (DHCP)");
                set_state(SBOS_WIFI_DHCP, "");
                dhcp_go = 1;                /* not from inside bcmw_poll: the first frame opens the tx flow ring */
            }
        } else {
            sb_log("wifi: the link went down");
            net_down();
            set_state(SBOS_WIFI_UP, "The network dropped the phone");
            joined = -1;
            reconnect_later();
        }
    } else if (ev->type == BCMW_E_DEAUTH || ev->type == BCMW_E_DEAUTH_IND || ev->type == BCMW_E_DISASSOC_IND) {
        sb_log("wifi: left by the access point");
        net_down();
        set_state(SBOS_WIFI_UP, "The network dropped the phone");
        joined = -1;
        reconnect_later();
    }
}

static void os_rx(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    (void)ifidx;
    ent_tick();
    ent_add(frame, len < 64 ? len : 64);
#ifdef WIFI_GUESTNET
    /* Track G: the Linux guest's flows (its mapped ports, ICMP errors about them) to its NAT; the rest to sbnet */
    if (len > 14 && frame[12] == 0x08 && frame[13] == 0x00 && st == SBOS_WIFI_CONNECTED &&
        guestnet_rx(KNET_UPLINK_WIFI, frame + 14, len - 14))
        return;
#endif
    sbnet_rx(&net, frame, len);
}

static int net_tx(void *ctx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    return bcmw_tx(w, frame, len) == BCMW_OK ? 0 : -1;
}

static uint64_t net_now(void *ctx)
{
    (void)ctx;
    return sb_ms();
}

static uint32_t net_random(void *ctx)
{
    (void)ctx;
    static uint64_t x = 0x9e3779b97f4a7c15ULL;
    x ^= sb_ticks() + (x << 13);
    x ^= x >> 7;
    x ^= x << 17;
    return (uint32_t)x;
}

static uint32_t ntp_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* sbnet's UDP for our ports: the NTP answer */
static void net_udp(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    (void)from;
    if (port != NTP_PORT || from_port != 123 || ntp_st != 2 || len < 48 || (d[0] & 7) != 4)
        return;
    if (memcmp(from, ntp_server, 4) || memcmp(d + 24, ntp_nonce, 8))
        return;                             /* not from the server asked, or not an answer to our request */
    if ((d[0] >> 6) == 3 || d[1] == 0 || d[1] >= 16)
        return;                             /* the server is not synchronised */
    uint32_t secs = ntp_be32(d + 40);           /* the transmit time, seconds since 1900 */
    if (secs < 2208988800U + 1577836800U)   /* before 2020: not believed */
        return;
    uint64_t rtt = sb_ms() - ntp_t;
    int64_t utc = (int64_t)(secs - 2208988800U) + (int64_t)((rtt / 2 + 500) / 1000U);
    int was_set = 0;
    int64_t d_s = clock_set_net(utc, &was_set);
    if (d_s > 2000000000LL)
        d_s = 2000000000LL;
    if (d_s < -2000000000LL)
        d_s = -2000000000LL;
    clock_off = (int32_t)d_s;
    clock_checked = was_set ? 3 : d_s ? 2 : 1;
    ntp_st = 3;
    if (was_set)
        sb_log("wifi: the clock was set from the network (there was none)");
    else if (d_s)
        logn("wifi: the clock was checked over the network and corrected by ", (uint64_t)(d_s < 0 ? -d_s : d_s),
             d_s < 0 ? " s (it was ahead)" : " s (it was behind)");
    else
        sb_log("wifi: the clock was checked over the network: right");
    notify();
}

static void net_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("wifi: ", line);
}

/* ---- TCP, the entropy pool, HTTPS (Next) ------------------------------------------------------------------ */

static struct sbtcp tcp;
static struct sbnetio nio;

static void net_tcp(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len)
{
    (void)ctx;
    sbtcp_input(&tcp, from, seg, len);
}

static int gn_up;                           /* Track G's NAT has Wi-Fi as an uplink */
static void gn_uplink(int on)
{
#ifdef WIFI_GUESTNET
    if (on != gn_up)
        guestnet_uplink(KNET_UPLINK_WIFI, on ? &net : 0);
#endif
    gn_up = on;
}

static void net_down(void)
{
    gn_uplink(0);                           /* every way down: off, left, dropped, no address, the chip gone */
    sbnet_stop(&net);
    sbtcp_down(&tcp);
}

/* The entropy pool for TLS's randomness (ECDHE keys, nonces). The phone gives this kernel no hardware random source
 * (the Exynos TRNG is the secure world's), so a SHA-256 pool takes everything hard to predict from outside: the
 * cycle counter at every radio interrupt, frame and event (their arrival times depend on the air and the chip),
 * the frames' and events' first bytes, timing jitter of memory walks when a seed is drawn, and the radio
 * firmware's own random source when it answers the "rand" iovar. Each seed is the pool's hash, then fed back in.
 * Not a certified generator: good enough that a passive listener cannot reproduce the TLS keys. */
static br_sha256_context ent;
static int ent_ready, fw_rand_logged;
static uint64_t ent_events;

static void ent_add(const void *p, uint32_t n)
{
    if (!ent_ready) {
        br_sha256_init(&ent);
        ent_ready = 1;
    }
    br_sha256_update(&ent, p, n);
    ent_events++;
}
static void ent_tick(void)
{
    uint64_t t = sb_ticks();
    ent_add(&t, sizeof(t));
}

static void ent_seed_io(uint8_t out[32], int ask_radio);
static void ent_seed(uint8_t out[32])
{
    ent_seed_io(out, 1);
}

/* Track H: the core's randomness (the handshake's nonces, SAE's secrets): the pool, never the radio's iovar here (the
 * core may ask from inside its own command path); n bytes as SHA-256(seed, counter) blocks */
static int os_random(void *ctx, uint8_t *out, uint32_t n)
{
    (void)ctx;
    uint8_t seed[32], blk[32];
    ent_seed_io(seed, 0);
    for (uint32_t ctr = 0, o = 0; o < n; ++ctr, o += 32) {
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, seed, 32);
        br_sha256_update(&c, &ctr, sizeof(ctr));
        br_sha256_out(&c, blk);
        memcpy(out + o, blk, n - o < 32 ? n - o : 32);
    }
    memset(seed, 0, sizeof(seed));
    memset(blk, 0, sizeof(blk));
    return 0;
}

static void ent_seed_io(uint8_t out[32], int ask_radio)
{
    static volatile uint8_t walk[8192];
    uint8_t jit[256];
    memset(jit, 0, sizeof(jit));
    for (unsigned i = 0; i < 4096; i++) {
        uint64_t a = sb_ticks();
        for (unsigned k = 0; k < 48; k++)
            walk[(i * 67u + k * 1031u) & 8191u] += (uint8_t)(a >> (k & 7));
        jit[i & 255] ^= (uint8_t)(sb_ticks() - a) ^ (uint8_t)((sb_ticks() - a) >> 3);
        if ((i & 255) == 255)
            ent_add(jit, sizeof(jit));
    }
    if (ask_radio && w_ready && w && !w->dead) {
        uint8_t fr[32];
        int fe = 0;
        memset(fr, 0, sizeof(fr));
        int r = bcmw_iovar_get(w, "rand", fr, sizeof(fr), &fe);
        int any = 0;
        for (unsigned i = 0; i < sizeof(fr); i++)
            any |= fr[i];
        if (r == BCMW_OK && any)
            ent_add(fr, sizeof(fr));
        if (!fw_rand_logged) {
            fw_rand_logged = 1;
            sb_log(r == BCMW_OK && any ? "wifi: tls: the radio's random source answers: mixed into the pool"
                                       : "wifi: tls: the radio has no random source (\"rand\"): timing only");
        }
        memset(fr, 0, sizeof(fr));
    }
    ent_tick();
    {
        br_sha256_context c = ent;          /* the seed: the pool's hash so far, then fed back */
        br_sha256_out(&c, out);
    }
    ent_add(out, 32);
}

/* one fetch at a time: the request, its body (64 KiB kept), its result */
#define FETCH_BODY (64U * 1024U)
static struct sbhttp *fx;
static uint8_t *fx_body;
/* fx_state (declared above): 0 none, 1 running, 2 finished */
static int fx_result;
static char fx_host[64];
static int fx_cell;                         /* this fetch runs over mobile data (kmodem_netif), not Wi-Fi */

/* the clock was checked over mobile data (kern/modem.c's NTP on the cellular interface): certificates may use it */
void wifi_clock_checked_by(int how)
{
    if (how > clock_checked)
        clock_checked = how;
}

static uint64_t nio_unix(void *ctx)
{
    (void)ctx;
    struct sbos_time t;
    if (!clock_now(&t) || t.utc <= 0)
        return 0;
    if (t.approx && clock_checked <= 0)
        return 0;                           /* the last known time, not checked yet: not good enough for certificates */
    return (uint64_t)t.utc;
}

static uint64_t ms_now(void *ctx)
{
    (void)ctx;
    return sb_ms();
}

/* the offline TLS self-check's log: to pmsg */
static void tls_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("wifi: ", line);
}

int wifi_tls_selfcheck(void)
{
    return sbtls_selfcheck(tls_log, 0, ms_now);
}

/* Track H: the uplink every user of the network shares (the fetch, the Linux guest's gateway): Wi-Fi when joined with
 * an address, else mobile data when the user turned it on and it is up. 1 Wi-Fi, 2 mobile data, 0 none. */
int wifi_uplink(struct sbnet **n, struct sbtcp **t)
{
    if (st == SBOS_WIFI_CONNECTED) {
        if (n)
            *n = &net;
        if (t)
            *t = &tcp;
        return 1;
    }
    return kmodem_netif(n, t) ? 2 : 0;
}

int wifi_fetch(const char *url)
{
    struct sbhttp_net hn;
    uint8_t seed[32];
    struct sbnet *fnet = &net;
    struct sbtcp *ftcp = &tcp;
    if (fx_state == 1)
        return SBOS_EBUSY;
    /* the interface: Wi-Fi when joined, else mobile data when the user turned it on and it is up */
    int up = wifi_uplink(&fnet, &ftcp);
    if (!up)
        return SBOS_EAGAIN;
    fx_cell = up == 2;
    if (!fx) {
        fx = sb_alloc(sizeof(*fx));
        fx_body = sb_alloc(FETCH_BODY);
        if (!fx || !fx_body)
            return SBOS_EAGAIN;
    }
    nio.net = fnet, nio.tcp = ftcp, nio.unix_time = nio_unix, nio.log = net_log, nio.ctx = 0;
    sbnetio_init(&nio, &hn);
    hn.log_host = 0;                        /* where the user goes is theirs: not logged */
    {
        /* the host, for the screen (never logged) */
        const char *p = url;
        unsigned n = 0;
        if (sb_prefix(p, "https://"))
            p += 8;
        else if (sb_prefix(p, "http://"))
            p += 7;
        while (*p && *p != '/' && *p != ':' && n + 1 < sizeof(fx_host))
            fx_host[n++] = *p++;
        fx_host[n] = 0;
    }
    hw_wdt_feed();
    ent_seed(seed);
    hw_wdt_feed();
    fx_state = 1;
    fx_result = sbhttp_start(fx, &hn, url, fx_body, FETCH_BODY, seed, 30000);
    memset(seed, 0, sizeof(seed));
    if (fx_result != SBHTTP_BUSY)
        fx_state = 2;
    if (!fx_cell)
        kt_set(KT_WIFI, 0, sb_ms(), 10, 0);
    notify();
    return 0;
}

static void fetch_poll(void)
{
    if (fx_state != 1)
        return;
    if (fx_cell ? !kmodem_netif(0, 0) : st != SBOS_WIFI_CONNECTED) {
        sbhttp_abort(fx);
        fx_result = SBHTTP_E_NETWORK;
        fx_state = 2;
        notify();
        return;
    }
    hw_wdt_feed();                          /* a TLS handshake's RSA/ECDSA checks take tens of ms */
    fx_result = sbhttp_poll(fx);
    if (fx_result != SBHTTP_BUSY) {
        fx_state = 2;
        notify();
    }
}

/* kern/modem.c's tick while mobile data is up: a fetch over it is driven from there (Wi-Fi may be off) */
void wifi_fetch_poll_cell(void)
{
    if (fx_cell)
        fetch_poll();
}

void wifi_fetch_cancel(void)
{
    if (fx_state == 1) {
        sbhttp_abort(fx);
        fx_result = SBHTTP_E_ABORTED;
        fx_state = 2;
        notify();
    }
}

void wifi_fetch_status(struct sbos_wifi_fetch *f)
{
    memset(f, 0, sizeof(*f));
    f->state = fx_state;
    f->result = fx_result;
    sb_strcpy(f->host, sizeof(f->host), fx_host);
    if (!fx)
        return;
    sb_strcpy(f->why, sizeof(f->why), sbhttp_why(fx_result));
    f->status = fx->status;
    f->tls_err = fx->tls_err;
    f->tls = fx->tls;
    f->tls_version = fx->tls_version;
    f->tls_suite = fx->tls_suite;
    f->bytes = (uint32_t)(fx->body_total > 0xffffffffull ? 0xffffffffu : fx->body_total);
    f->kept = fx->body_len;
    f->dns_ms = fx->dns_ms, f->connect_ms = fx->connect_ms, f->tls_ms = fx->tls_ms, f->total_ms = fx->total_ms;
    f->first_byte_ms = fx->first_byte_ms;
    f->secs = (int32_t)((sb_ms() - fx->t_start) / 1000U);
}

const uint8_t *wifi_fetch_body(uint32_t *len)
{
    *len = fx && fx_state == 2 ? fx->body_len : 0;
    return fx_body;
}

/* ---- on and off ------------------------------------------------------------------------------------------ */

#ifdef SB_HOST
static int host_file(void *ctx, const char *name, const uint8_t **data, uint32_t *len)
{
    /* the model's synthetic firmware (starbite/wifi/host/test.c's synth_files) */
    static uint8_t fw[64 * 1024];
    static const char banner[] = "4361b2-roml/config_pcie_release_ipa Version: 13.50.99 (model) FWID: 01-0";
    static const char nv[] = "boardrev=0x1101\nmacaddr=00:90:4c:12:d0:01\nccode=0\n";
    static uint8_t clm[3000];
    (void)ctx;
    if (!fw[1]) {
        for (unsigned i = 0; i < sizeof(fw); i++)
            fw[i] = (uint8_t)(i * 7);
        fw[0x6c] = 0;
        memcpy(fw + sizeof(fw) - 200, banner, sizeof(banner));
        memcpy(clm, "BLOB", 4);
    }
    /* every time: model_reset (each power-on) clears them, and the model checks the image it boots against them */
    M.fw_rstvec = (uint32_t)fw[0] | (uint32_t)fw[1] << 8 | (uint32_t)fw[2] << 16 | (uint32_t)fw[3] << 24;
    M.fw_image = fw;
    M.fw_image_len = sizeof(fw);
    if (sb_prefix(name, "bcmdhd_sta.bin"))
        return *data = fw, *len = sizeof(fw), 0;
    if (sb_prefix(name, "nvram.txt"))
        return *data = (const uint8_t *)nv, *len = sizeof(nv) - 1, 0;
    if (sb_prefix(name, "bcmdhd_clm"))
        return *data = clm, *len = sizeof(clm), 0;
    return -1;
}
#endif

#ifndef SB_HOST
static void say(const char *s)
{
    sb_log(s);
}
#endif

static void reconnect_later(void)
{
    static const uint32_t backoff[] = { 2000U, 5000U, 15000U, 30000U, 60000U };
    if (!wifi_autojoin || wifi_in_selftest() || time_only)
        return;
    reconnect_at = sb_ms() + backoff[reconnect_n < 4 ? reconnect_n : 4];
    ++reconnect_n;
}

int wifi_power(int on)
{
    if (!on) {
        if (st == SBOS_WIFI_OFF)
            return 0;
        wifi_fetch_cancel();
        irq_stop();
        logn("wifi: interrupts while on: ", irq_n, "");
        if (w_ready) {
            net_down();
            if (joined >= 0 && !w->dead)
                (void)bcmw_leave(w);
            bcmw_quiesce(w);
        }
        kt_cancel(KT_WIFI, 0);
        dhcp_go = 0;
        scan_retry = 0;
        join_pending = join_active = join_hidden = 0;
        auto_pending = 0;
        memset(jp_pass, 0, sizeof(jp_pass));
        joined = -1;
        nnets = 0;
        logn("wifi: off (the radio held in reset and unpowered) after ", (sb_ms() - t_on) / 1000U, " s");
        set_state(SBOS_WIFI_OFF, "");
        return 0;
    }
    if (st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED)
        return 0;
    if (heater_busy()) {
        set_state(SBOS_WIFI_OFF, "Not while heating (0.4)");
        return SBOS_EAGAIN;
    }
    if (!w) {
        w = sb_alloc(bcmw_size());
        if (!w) {
            sb_log("wifi: not enough memory for the Wi-Fi core");
            set_state(SBOS_WIFI_FAILED, "Not enough memory for Wi-Fi");   /* P3n: the shell said "Heat?" */
            return SBOS_EAGAIN;
        }
        memset(w, 0, bcmw_size());
    }
    w_ready = 0;
    scan_retry = 0;
    auto_pending = auto_done = 0;           /* 1.2-alpha21: one automatic join per power-on */
    reconnect_at = bg_scan_at = 0;
    reconnect_n = 0;
    irq_n = irq_missed = 0;
    irq_trusted = irq_kicked = 0;
    dk_next = dk_new = dk_reused = 0;      /* the DMA blocks of the first power-on, again in order */
#ifdef SB_HOST
    model_reset();
    M.open_ap = !getenv("SBOS_HOST_WIFINET") && !getenv("SBOS_HOST_WPA2");   /* else the model's WPA2 "test-ap" */
    M.echo_sync = 1;
    {
        const uint8_t *fd;
        uint32_t fl;
        (void)host_file(0, "bcmdhd_sta.bin", &fd, &fl);    /* the model's M.fw_* again (the core may not ask) */
    }
    memset(&os, 0, sizeof(os));
    os.rd32 = model_rd32, os.wr32 = model_wr32, os.rd16 = model_rd16, os.wr16 = model_wr16;
    os.udelay = model_udelay, os.now_us = model_now, os.dma_alloc = model_dma_alloc;
    os.file = host_file, os.log = os_log, os.event = os_event;
#else
    if (!bcmw_sb_vendor(say)) {
        set_state(SBOS_WIFI_FAILED, "The radio's files could not be read");
        return SBOS_EAGAIN;
    }
    bcmw_sb_os(&os, os_log, os_event);
#endif
    os.rx = os_rx;
    os.random = os_random;                  /* Track H: the pool (nonces, SAE) */
    dma_first = os.dma_alloc;
    os.dma_alloc = dma_reuse;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stop_after = BCMW_STEP_UP;
    cfg.rx_buffers = 256;                   /* 1.2-alpha7: as the Windows driver that joins (wifi-core 24a0adf) */
#ifdef SB_HOST
    cfg.any_files = 1;
    memcpy(cfg.mac, "\x02\x11\x22\x33\x44\x55", 6);   /* the station address the model's access point expects */
#endif
    hw_wdt_feed();
    if (bcmw_init(w, &os, &cfg) != BCMW_OK) {
        set_state(SBOS_WIFI_FAILED, "The Wi-Fi core did not start");
        return SBOS_EAGAIN;
    }
    w_ready = 1;
    bcmw_sae_enable(w);                     /* Track H: WPA3 (SAE) by the host when the firmware asks (cap sae_ext) */
    dhcp_go = 0;
    join_pending = join_active = join_hidden = 0;
    load_saved();
    step = 0;
    nnets = 0;
    joined = -1;
    pinged = 0;
    t_on = sb_ms();
    sb_log("wifi: on (asked): the radio's bring-up, one step per tick");
    set_state(SBOS_WIFI_STARTING, "");
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);
    return 0;
}

int wifi_scan(void)
{
    if (st != SBOS_WIFI_UP && st != SBOS_WIFI_CONNECTED)
        return SBOS_EAGAIN;
    int was = st;
    nnets = st == SBOS_WIFI_CONNECTED ? nnets : 0;
    scan_heard = 0;
    sv_heard24 = sv_heard5 = 0;
    sc_reset();
    if (was == SBOS_WIFI_UP)
        set_state(SBOS_WIFI_SCANNING, "");  /* first: the scan's events can arrive while the command waits */
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);    /* the tick soon: the scan's events */
    hw_wdt_feed();
    if (bcmw_scan_start(w, 0, 0, ++sync) != BCMW_OK) {
        if (was == SBOS_WIFI_UP)
            set_state(SBOS_WIFI_UP, "");
        if (!scan_retry)
            scan_retry = 3;
        if (--scan_retry > 0) {             /* busy (the bring-up's own scan): again in 1.5 s */
            scan_retry_at = sb_ms() + 1500;
            sb_log("wifi: the scan was refused (busy?): trying again");
            return 0;
        }
        sb_log("wifi: the scan did not start");
        if (was == SBOS_WIFI_UP)
            set_state(SBOS_WIFI_UP, "The scan did not start");
        return SBOS_EAGAIN;
    }
    scan_retry = 0;
    return 0;
}

int wifi_join(int i, const char *pass)
{
    if (st != SBOS_WIFI_UP || i < 0 || i >= nnets)
        return SBOS_EAGAIN;
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);    /* the tick soon (a relaxed poll may be pending) */
    scan_retry = 0;
    auto_pending = 0;                       /* a join asked for: the last scan's automatic one is moot (it would fire
                                             * as soon as Wi-Fi is UP again: after a leave, at once) */
    if (nets[i].secure) {
        const char *k = pass && pass[0] ? pass : known_pass(nets[i].ssid, nets[i].ssid_len);
        unsigned n = k ? sb_strlen(k) : 0;
        if (!k)
            return SBOS_EPERM;              /* secured, no passphrase known: the app asks for one */
        if (n < 8 || n > 64)
            return SBOS_EINVAL;
        sb_strcpy(jp_pass, sizeof(jp_pass), k);
    }
    if (nets[i].secure || join_hidden) {
        /* 0.6.1: bcmw_join_start (the directed scan, the PMK, SET_SSID: a few seconds at most) on the next tick, then
         * one handshake round per tick (bcmw_join_poll): the screen keeps working (0.6 blocked up to 15 s) */
        const struct bcmw_info *in = bcmw_info(w);
        struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, net_udp, net_tcp };
        sbnet_init(&net, &no, in->mac);
        sbtcp_init(&tcp, &net);
        joined = i;
        join_pending = 1;
        sb_log("wifi: joining a secured network (name and passphrase not logged)");
        set_state(SBOS_WIFI_JOINING, "");
        return 0;
    }
    const struct bcmw_info *in = bcmw_info(w);
    struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, net_udp, net_tcp };
    sbnet_init(&net, &no, in->mac);
    sbtcp_init(&tcp, &net);
    joined = i;
    sb_log("wifi: joining an open network (name not logged)");
    set_state(SBOS_WIFI_JOINING, "");       /* first: the association's events can arrive inside bcmw_connect */
    if (bcmw_connect(w, nets[i].ssid, nets[i].ssid_len, nets[i].bssid, 0, 0, 0) != BCMW_OK) {
        joined = -1;
        set_state(SBOS_WIFI_UP, "The join did not start");
        return SBOS_EAGAIN;
    }
    return 0;
}

void wifi_status(struct sbos_wifi *s)
{
    memset(s, 0, sizeof(*s));
    s->state = st;
    s->nets = nnets;
    sb_strcpy(s->why, sizeof(s->why), why);
    if (joined >= 0 && joined < nnets) {
        memcpy(s->ssid, nets[joined].ssid, nets[joined].ssid_len);
        s->ssid_len = nets[joined].ssid_len;
        s->rssi = nets[joined].rssi;
    }
    if (st == SBOS_WIFI_CONNECTED) {
        memcpy(s->ip, net.ip, 4);
        memcpy(s->gateway, net.gateway, 4);
        s->ping_ms = net.ping_replies ? (int32_t)net.ping_rtt_ms : -1;
        s->clock_checked = clock_checked;
        s->clock_off_s = clock_off;
    }
    s->secs = (int32_t)((sb_ms() - t_state) / 1000U);
}

/* 1.2-alpha21: the known networks: the saved one (the store's "wifinet", set from the computer) first, then the ones
 * joined with a typed passphrase since the start (RAM only). Names only; passphrases never leave the kernel. */
int wifi_known(struct sbos_wifi_known *out, int max)
{
    int n = 0;
    if (sv_ok && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].ssid, sv_ssid, sv_len);
        out[n].ssid_len = (uint8_t)sv_len;
        out[n].saved = 1;
        ++n;
    }
    for (unsigned i = 0; i < nrem && n < max; ++i) {
        if (sv_ok && rem[i].len == sv_len && !memcmp(rem[i].ssid, sv_ssid, sv_len))
            continue;                       /* the saved one, typed again: listed once */
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].ssid, rem[i].ssid, rem[i].len);
        out[n].ssid_len = (uint8_t)rem[i].len;
        ++n;
    }
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < nnets; ++i)
            if (nets[i].ssid_len == out[k].ssid_len && !memcmp(nets[i].ssid, out[k].ssid, out[k].ssid_len))
                out[k].heard = 1;
    return n;
}

/* forget a network joined since the start (k: its place in wifi_known's list); the saved one: SBOS_EPERM */
int wifi_forget(int k)
{
    struct sbos_wifi_known kn[1 + REMEMBER];
    int n = wifi_known(kn, 1 + REMEMBER);
    if (k < 0 || k >= n)
        return SBOS_EINVAL;
    if (kn[k].saved)
        return SBOS_EPERM;
    for (unsigned i = 0; i < nrem; ++i)
        if (rem[i].len == kn[k].ssid_len && !memcmp(rem[i].ssid, kn[k].ssid, rem[i].len)) {
            memset(rem[i].pass, 0, sizeof(rem[i].pass));
            for (unsigned j = i; j + 1 < nrem; ++j)
                rem[j] = rem[j + 1];
            --nrem;
            memset(&rem[nrem], 0, sizeof(rem[nrem]));
            sb_log("wifi: a remembered network was forgotten (name not logged)");
            settings_changed();
            return 0;
        }
    return SBOS_EINVAL;
}

/* Track H: the remembered networks for Settings > Wi-Fi (the saved "wifinet" first, then most recent first); the
 * index k is the same as wifi_forget's and wifi_known_auto's */
int wifi_known2(struct kwifi_known *out, int max)
{
    struct sbos_wifi_known kn[1 + REMEMBER];
    int n = wifi_known(kn, 1 + REMEMBER);
    if (n > max)
        n = max;
    for (int k = 0; k < n; ++k) {
        memset(&out[k], 0, sizeof(out[k]));
        memcpy(out[k].ssid, kn[k].ssid, kn[k].ssid_len);
        out[k].ssid_len = kn[k].ssid_len;
        out[k].saved = kn[k].saved;
        out[k].heard = kn[k].heard;
        out[k].autojoin = 1;
        out[k].sec = -1;
        int r = rem_find(kn[k].ssid, kn[k].ssid_len);
        if (r >= 0) {
            out[k].hidden = rem[r].hidden;
            out[k].autojoin = !rem[r].noauto;
            out[k].sec = rem[r].sec;
        }
        out[k].joined = joined >= 0 && joined < nnets && nets[joined].ssid_len == kn[k].ssid_len &&
                        !memcmp(nets[joined].ssid, kn[k].ssid, kn[k].ssid_len) && st == SBOS_WIFI_CONNECTED;
    }
    return n;
}

int wifi_known_auto(int k, int on)
{
    struct sbos_wifi_known kn[1 + REMEMBER];
    int n = wifi_known(kn, 1 + REMEMBER);
    if (k < 0 || k >= n)
        return SBOS_EINVAL;
    int r = rem_find(kn[k].ssid, kn[k].ssid_len);
    if (r < 0)
        return SBOS_EPERM;                  /* the saved one (from the computer): always automatic */
    rem[r].noauto = (uint8_t)(on == 0);
    settings_changed();
    return 0;
}

/* Track H: join by name (Settings > Wi-Fi > Other...: a hidden network, or one not in the list yet). pass: "" for
 * open. The core's directed scan asks for the name, so a hidden access point answers. */
int wifi_join_name(const uint8_t *ssid, unsigned len, const char *pass)
{
    if (st != SBOS_WIFI_UP || !len || len > 32)
        return SBOS_EAGAIN;
    unsigned pl = pass ? sb_strlen(pass) : 0;
    if (pl && (pl < 8 || pl > 64))
        return SBOS_EINVAL;
    for (int i = 0; i < nnets; ++i)
        if (nets[i].ssid_len == len && !memcmp(nets[i].ssid, ssid, len))
            return wifi_join(i, pass);
    int at = nnets < SBOS_WIFI_NETS ? nnets++ : SBOS_WIFI_NETS - 1;
    memset(&nets[at], 0, sizeof(nets[at]));
    memcpy(nets[at].ssid, ssid, len);
    nets[at].ssid_len = (uint8_t)len;
    nets[at].rssi = -100;
    nets[at].secure = pl != 0;
    nsec[at] = (int8_t)(pl ? KWIFI_SEC_WPA2 : KWIFI_SEC_OPEN);
    join_hidden = 1;
    sb_log("wifi: joining a network by its name (hidden or not heard; name not logged)");
    int r = wifi_join(at, pl ? pass : "");
    if (r)
        join_hidden = 0;
    return r;
}

/* Track H: the details of the joined network for Settings > Wi-Fi > (i) */
void wifi_details(struct kwifi_details *d)
{
    memset(d, 0, sizeof(*d));
    d->state = st;
    if (joined < 0 || joined >= nnets)
        return;
    memcpy(d->ssid, nets[joined].ssid, nets[joined].ssid_len);
    d->ssid_len = nets[joined].ssid_len;
    d->rssi = nets[joined].rssi;
    d->channel = nets[joined].channel;
    d->band5 = nets[joined].band5;
    d->sec = nsec[joined];
    d->power_save = pm_set;
    if (st == SBOS_WIFI_CONNECTED) {
        memcpy(d->ip, net.ip, 4);
        memcpy(d->mask, net.mask, 4);
        memcpy(d->gateway, net.gateway, 4);
        memcpy(d->dns, net.dns, 4);
        d->lease_s = net.lease_s;
        d->ping_ms = net.ping_replies ? (int32_t)net.ping_rtt_ms : -1;
    }
}

/* Track H: the user's switch (Settings, the Control Centre): remembered for the next start */
int wifi_user_power(int on)
{
    if (user_on != (on != 0)) {
        user_on = on != 0;
        settings_changed();
    }
    if (on)
        surv_was_on = 0;
    return wifi_power(on);
}

int wifi_user_on(void)
{
    return user_on;
}

/* Track H: Survival mode holds Wi-Fi off (wifi-for-survival.md). On: off now, nothing automatic until it ends (the
 * user may still turn it on by hand); off: back on if it was on when Survival started or the user's switch says so */
void wifi_survival(int on)
{
    if (on) {
        if (surv_off)
            return;
        surv_off = 1;
        surv_was_on = wifi_on() && !time_only;  /* on only for the boot's time check: not the user's */
        wifi_time_only_end("Survival mode");
        if (wifi_on()) {
            sb_log("wifi: Survival mode: off (power save)");
            (void)wifi_power(0);
        }
        return;
    }
    if (!surv_off)
        return;
    surv_off = 0;
    if ((surv_was_on || user_on) && !wifi_on()) {
        sb_log("wifi: Survival mode ended: on again");
        (void)wifi_power(1);
    }
    surv_was_on = 0;
}

int wifi_list(struct sbos_wifi_net *out, int max)
{
    int n = nnets < max ? nnets : max;
    for (int i = 0; i < n; ++i)
        out[i] = nets[i];
    return n;
}

/* Track H: the security of the i-th listed network (KWIFI_SEC_*), -1 none */
int wifi_net_sec(int i)
{
    return i >= 0 && i < nnets ? nsec[i] : -1;
}

/* Track H: what the firmware says it can do ("cap": capability words, nothing identifying), logged once per boot
 * and kept as KWIFI_CAP_* bits for the UI (WPA3 needs SAE) */
static unsigned fw_caps;
static int fw_caps_read;

static int cap_has(const char *cap, const char *word)
{
    unsigned n = sb_strlen(word);
    for (unsigned i = 0; cap[i]; ++i)
        if ((i == 0 || cap[i - 1] == ' ') && !memcmp(cap + i, word, n) && (cap[i + n] == ' ' || !cap[i + n] ||
                                                                          cap[i + n] == '\n'))
            return 1;
    return 0;
}

static void caps_read(void)
{
    static char cap[512];
    int fe = 0;
    if (fw_caps_read || !w_ready)
        return;
    fw_caps_read = 1;
    memset(cap, 0, sizeof(cap));
    hw_wdt_feed();
    if (bcmw_iovar_get(w, "cap", cap, sizeof(cap) - 1, &fe) != BCMW_OK) {
        sb_logx("wifi: the firmware's capability list (cap) did not answer: ", (uint64_t)(uint32_t)fe);
        return;
    }
    for (unsigned i = 0; cap[i]; ++i)
        if (cap[i] == '\n' || cap[i] == '\r')
            cap[i] = ' ';
    fw_caps = (cap_has(cap, "sae") ? KWIFI_CAP_SAE : 0) | (cap_has(cap, "sae_ext") ? KWIFI_CAP_SAE_EXT : 0) |
              (cap_has(cap, "mfp") ? KWIFI_CAP_MFP : 0) | (cap_has(cap, "idsup") ? KWIFI_CAP_IDSUP : 0) |
              (cap_has(cap, "wpa3") ? KWIFI_CAP_WPA3 : 0);
    /* the words in lines of at most ~200 characters, cut at a space */
    unsigned n = sb_strlen(cap), at = 0, part = 0;
    while (at < n) {
        unsigned e = at + 200 < n ? at + 200 : n;
        if (e < n)
            while (e > at + 1 && cap[e] != ' ')
                --e;
        struct ub u;
        ub_init(&u);
        ub_s(&u, part ? "wifi: firmware cap (more): " : "wifi: firmware cap: ");
        char c = cap[e];
        cap[e] = 0;
        ub_s(&u, cap + at);
        cap[e] = c;
        sb_log(u.b);
        at = e;
        while (at < n && cap[at] == ' ')
            ++at;
        ++part;
    }
    sb_logx("wifi: firmware capability bits (1 sae, 2 sae_ext, 4 mfp, 8 idsup, 16 wpa3): ", fw_caps);
}

unsigned wifi_caps(void)
{
    return fw_caps;
}

/* Track H, for Bluetooth (the drivers track picks the .hcd): the radio module's maker from its OTP, read at the
 * bring-up's step 7: 1 SEMCO (91 33 / a1 33), 0 another (Murata), -1 not known yet (Wi-Fi not brought up this boot) */
static int module_kind = -1;
int wifi_module(void)
{
    return module_kind;
}

/* Track H: leave the joined network (the radio stays on, the list stays) */
int wifi_leave(void)
{
    if (st != SBOS_WIFI_JOINING && st != SBOS_WIFI_DHCP && st != SBOS_WIFI_CONNECTED)
        return SBOS_EAGAIN;
    wifi_fetch_cancel();
    join_pending = join_active = join_hidden = 0;
    memset(jp_pass, 0, sizeof(jp_pass));
    dhcp_go = 0;
    if (w_ready && !w->dead) {
        hw_wdt_feed();
        (void)bcmw_leave(w);
    }
    net_down();
    joined = -1;
    reconnect_at = 0;
    bg_scan_at = sb_ms() + 600000U;         /* asked to leave: no automatic join for 10 min */
    sb_log("wifi: left the network (asked)");
    set_state(SBOS_WIFI_UP, "");
    return 0;
}

/* the status bar: 0 off, 1 on (not joined), 2 joined with an address */
int wifi_bar(void)
{
    return st == SBOS_WIFI_CONNECTED ? 2 : st == SBOS_WIFI_OFF || st == SBOS_WIFI_FAILED ? 0 : 1;
}

void wifi_irq(void)
{
    uint64_t now = sb_ms();
    ent_tick();
    ++irq_n;
    if (now - irq_window_t >= 1000) {
        irq_window_t = now;
        irq_window_n = 0;
    }
    if (++irq_window_n > 2000) {            /* a storm: the line stays asserted; back to polling */
        hw_spi(WIFI_INTID, 0);
        irq_on = 0;
        ++irq_storms;
        sb_log("wifi: an interrupt storm (over 2000 in a second): interrupts off, polling");
        return;
    }
    if (w && w_ready)
        (void)bcmw_isr(w);                  /* the chip's mailbox masked until the next poll; the latch cleared */
    irq_kicked = 1;
    if (st != SBOS_WIFI_OFF)
        kt_set(KT_WIFI, 0, now, 0, 0);      /* the tick (bcmw_poll) at once */
}

static void irq_start(void)
{
    if (irq_on || irq_storms)
        return;
    ex_intx(w, 1);                          /* the root complex's INTx enable (ELBI), the core's own sequence */
    if (hw_spi(WIFI_INTID, 1)) {
        irq_on = 1;
        sb_log("wifi: interrupts on (SPI 253); polling stays as the fallback");
    }
}

static void irq_stop(void)
{
    if (irq_on)
        hw_spi(WIFI_INTID, 0);
    irq_on = 0;
}

/* 1.2-alpha26: a boot without a clock: Wi-Fi on once for the time (the saved network joins by itself), then off.
 * Not in a self test, not without a saved network or with automatic joins off; the user's own Wi-Fi switch, or Heat,
 * ends it */
static int time_only;
static uint64_t time_only_t;

int wifi_time_only(void)
{
    return time_only && st != SBOS_WIFI_OFF;
}

void wifi_time_only_end(const char *why)
{
    if (!time_only)
        return;
    time_only = 0;
    sb_logf("wifi: the boot's time check ends: ", why);
}

void wifi_boot_time_check(void)
{
    struct sbos_time t;
    if (sbos_selftest_enabled || wifi_in_selftest() || surv_off)
        return;
    if (user_on) {
        /* Track H: Wi-Fi was on at the last start: on again (a remembered network joins by itself) */
        sb_log("wifi: on at start (the user's switch was on)");
        (void)wifi_power(1);
        return;
    }
    if (!wifi_autojoin || (clock_now(&t) && !t.approx))
        return;
    load_saved();
    if (!sv_ok && !nrem)
        return;
    time_only = 1;
    time_only_t = sb_ms();
    sb_log("wifi: no clock at boot: on once for the time (a known network), then off");
    if (wifi_power(1))
        wifi_time_only_end("Wi-Fi did not come on");
}

void wifi_timer(unsigned id)
{
    (void)id;
    if (st == SBOS_WIFI_OFF)
        return;
    uint64_t now = sb_ms();
    if (st == SBOS_WIFI_FAILED) {
        /* Track H: only here when the chip stopped answering and a restart of the radio was asked for (below) */
        if (user_on && !surv_off && !wifi_in_selftest() && revive_n < 2) {
            ++revive_n;
            logn("wifi: the radio is started again after it stopped answering, time ", revive_n, "");
            (void)wifi_power(1);
        }
        return;
    }
    if (time_only && (ntp_st == 3 || ntp_st == -1 || now - time_only_t > 90000U)) {
        wifi_time_only_end(ntp_st == 3 ? "the clock is set; Wi-Fi off again" : "no time came; Wi-Fi off again");
        wifi_power(0);
        return;
    }
#ifdef SB_HOST
    if (M.now < now * 1000U)
        M.now = now * 1000U;                /* the model's clock follows the host's (a polled join's timeout) */
#endif
    if (st == SBOS_WIFI_UP && !time_only && !wifi_in_selftest() && wifi_autojoin && !join_pending && !join_active) {
        /* Track H: after a drop, and now and then while not joined: a scan, then the automatic join of a known one */
        int due = (reconnect_at && now >= reconnect_at) || (!reconnect_at && bg_scan_at && now >= bg_scan_at);
        if (due && (nrem || sv_ok)) {
            int again = reconnect_at != 0;
            if (again)
                logn("wifi: trying to get back on a known network, attempt ", reconnect_n, "");
            reconnect_at = 0;
            bg_scan_at = now + (K.screen_on ? 60000U : 300000U);
            auto_done = 0;
            (void)wifi_scan();
            if (again && reconnect_n < 5)
                reconnect_later();          /* the next try (5, 15, 30, 60 s) unless this one gets back on */
        } else if (!bg_scan_at) {
            bg_scan_at = now + (K.screen_on ? 60000U : 300000U);
        }
    }
    if (auto_pending && st == SBOS_WIFI_UP) {   /* 1.2-alpha21: from the tick, not inside the chip's event */
        auto_pending = 0;
        auto_done = 1;
        for (int i = 0; i < nnets; ++i)     /* strongest first (sort_nets) */
            if (known_pass(nets[i].ssid, nets[i].ssid_len) &&
                !(rem_find(nets[i].ssid, nets[i].ssid_len) >= 0 && rem[rem_find(nets[i].ssid, nets[i].ssid_len)].noauto)) {
                sb_log("wifi: a known network was heard: joining it automatically (name not logged)");
                (void)wifi_join(i, 0);
                break;
            }
        for (unsigned k = 0; st == SBOS_WIFI_UP && k < nrem; ++k)
            if (rem[k].hidden && !rem[k].noauto) {
                /* a remembered hidden network is never in the list (no name heard): asked for by its name */
                sb_log("wifi: joining a remembered hidden network automatically (name not logged)");
                (void)wifi_join_name(rem[k].ssid, rem[k].len, rem[k].pass);
                break;
            }
    }
    if (st == SBOS_WIFI_STARTING) {
        uint64_t t0 = sb_ms();
        hw_wdt_feed();                      /* the longest steps take seconds (0.5.12) */
        int r = bcmw_bringup(w, (unsigned)step + 1U);
        hw_wdt_feed();
        uint64_t took = sb_ms() - t0;
        if (r < 0) {
            logn("wifi: the bring-up failed at step ", (uint64_t)step + 1U, "");
            irq_stop();
            bcmw_quiesce(w);
            set_state(SBOS_WIFI_FAILED, "The radio did not start (logged)");
            return;
        }
        step = r;
        if (took >= 150)
            logn("wifi: a bring-up step took ", took, " ms");
        if (step >= BCMW_STEP_UP) {
            irq_start();
            logn("wifi: up in ", now - t_on, " ms; scanning");
            struct ub u;
            ub_init(&u);
            ub_s(&u, "wifi: DMA blocks: ");
            ub_dec(&u, dk_new);
            ub_s(&u, " new, ");
            ub_dec(&u, dk_reused);
            ub_s(&u, " reused");
            sb_log(u.b);
            set_state(SBOS_WIFI_UP, "");
            caps_read();                    /* Track H: once per boot (WPA3 needs SAE) */
            {
                const uint8_t *v = bcmw_info(w)->module_vid;
                if (v[0] || v[1])
                    module_kind = (v[0] == 0x91 || v[0] == 0xa1) && v[1] == 0x33;
            }
            (void)wifi_scan();
        }
    } else {
        int kicked = irq_kicked;
        irq_kicked = 0;
        int got = bcmw_poll(w);
        if (got > 0 && !kicked && irq_on && irq_n && irq_trusted) {
            /* work no interrupt announced: the line is not reliable; poll fast again (logged once) */
            irq_trusted = 0;
            if (!irq_missed++)
                sb_log("wifi: work arrived without an interrupt: polling at the normal rate again");
        } else if (kicked && got > 0 && !irq_missed) {
            irq_trusted = 1;
        }
        if (w->dead) {
            irq_stop();
            sb_log("wifi: the chip stopped answering: off");
            bcmw_quiesce(w);
            joined = -1;
            kt_cancel(KT_WIFI, 0);
            set_state(SBOS_WIFI_FAILED, "The radio stopped answering (logged)");
            net_down();
            if (user_on && !surv_off && !wifi_in_selftest() && revive_n < 2)
                kt_set(KT_WIFI, 0, sb_ms(), 5000, 0);   /* Track H: once more in 5 s (wifi_timer, FAILED) */
            return;
        }
        if (scan_retry && st == SBOS_WIFI_UP && now >= scan_retry_at)
            (void)wifi_scan();
        int r = BCMW_E_PENDING;
        if (join_pending && st == SBOS_WIFI_JOINING && now - t_state >= 100) {
            join_pending = 0;
            join_active = 1;                /* first: the association's events arrive inside bcmw_join_start */
            join_t0 = sb_ms();
            hw_wdt_feed();
            r = joined >= 0 ? bcmw_join_start(w, nets[joined].ssid, nets[joined].ssid_len, jp_pass, 15000) : BCMW_E_STATE;
            hw_wdt_feed();
            uint64_t took = sb_ms() - join_t0;
            if (took >= 150)
                logn("wifi: the join's start (the directed scan, the PMK) took ", took, " ms");
            if (r == BCMW_OK)
                r = BCMW_E_PENDING;
            now = sb_ms();
        } else if (join_active) {
            r = bcmw_join_poll(w);
        }
        if (r != BCMW_E_PENDING) {
            uint64_t t0 = join_t0;
            join_active = 0;
            if (r == BCMW_OK && joined >= 0)
                remember(nets[joined].ssid, nets[joined].ssid_len, jp_pass, join_hidden, nsec[joined]);
            join_hidden = 0;
            memset(jp_pass, 0, sizeof(jp_pass));
            if (r != BCMW_OK) {
                logn("wifi: the network did not take the phone after ", sb_ms() - t0, " ms");
                joined = -1;
                dhcp_go = 0;                /* a LINK event may have come before the handshake failed */
                set_state(SBOS_WIFI_UP, "Could not join: the passphrase, or too far away");
            } else if (st == SBOS_WIFI_JOINING) {
                logn("wifi: joined a secured network in ", sb_ms() - t0, " ms; asking for an address (DHCP)");
                set_state(SBOS_WIFI_DHCP, "");
                dhcp_go = 1;
            }
            now = sb_ms();
        }
        if (dhcp_go && st == SBOS_WIFI_DHCP) {
            dhcp_go = 0;
            sbnet_start(&net);
        }
        if (st == SBOS_WIFI_SCANNING && now - t_state > SCAN_MS) {
            sc_log("TIMED OUT (no end event),", 0);
            sort_nets();
            logn("wifi: the scan timed out with ", (uint64_t)nnets, " networks");
            set_state(SBOS_WIFI_UP, "");
            auto_pending = wifi_autojoin && !auto_done && joined < 0 && !wifi_in_selftest();
        }
        if (st == SBOS_WIFI_JOINING && !join_pending && !join_active && now - t_state > JOIN_MS) {
            sb_log("wifi: the join timed out");
            (void)bcmw_leave(w);
            joined = -1;
            set_state(SBOS_WIFI_UP, "The network did not answer");
        }
        if (st == SBOS_WIFI_DHCP || st == SBOS_WIFI_CONNECTED) {
            sbnet_tick(&net);
            sbtcp_tick(&tcp);
            if (!fx_cell)
                fetch_poll();
            if (st == SBOS_WIFI_DHCP && net.state == SBNET_BOUND) {
                logn("wifi: an address from DHCP (not logged), lease ", net.lease_s, " s");
                set_state(SBOS_WIFI_CONNECTED, "");
                gn_uplink(1);                       /* Track G's NAT: Wi-Fi is an uplink (preferred over LTE) */
                reconnect_n = 0;
                reconnect_at = 0;
                pm_set = 0;
                if (joined >= 0 && !nets[joined].secure && rem_find(nets[joined].ssid, nets[joined].ssid_len) < 0)
                    remember(nets[joined].ssid, nets[joined].ssid_len, "", 0, nsec[joined]);   /* Track H: open too */
                pinged = 0;
                ntp_st = 0;
                clock_checked = 0;
            } else if (st == SBOS_WIFI_DHCP && now - t_state > DHCP_MS) {
                sb_log("wifi: no DHCP answer");
                (void)bcmw_leave(w);
                net_down();
                joined = -1;
                set_state(SBOS_WIFI_UP, "Joined, but no address came (DHCP)");
            }
            if (st == SBOS_WIFI_CONNECTED && net.state != SBNET_BOUND && net.state != SBNET_RENEWING &&
                net.state != SBNET_REBINDING) {
                sb_log("wifi: the address was lost (DHCP); asking again");
                gn_uplink(0);
                set_state(SBOS_WIFI_DHCP, "");
                sbnet_start(&net);
            }
            if (st == SBOS_WIFI_CONNECTED && !pinged && now - t_state > 500) {
                pinged = 1;
                (void)sbnet_ping(&net, net.gateway);
            }
            if (st == SBOS_WIFI_CONNECTED && pinged == 1 && net.ping_replies) {
                pinged = 2;
                logn("wifi: the gateway answers a ping in ", net.ping_rtt_ms, " ms");
                notify();
            }
            if (st == SBOS_WIFI_CONNECTED && ntp_st == 0 && (pinged == 2 || (pinged == 1 && now - t_state > 3500))) {
                ntp_st = sbnet_dns_query(&net, "pool.ntp.org") == 0 ? 1 : -1;
                ntp_t = now;
            }
            if (st == SBOS_WIFI_CONNECTED && !pm_set && wifi_pm && (ntp_st == 3 || ntp_st == -1) &&
                fx_state != 1 && w_ready && !w->dead) {
                /* Track H: power save once joined and the clock checked: PM 2 (fast power save: the radio sleeps
                 * between beacons, wakes for traffic; Android's default when the screen is off) */
                uint32_t two = 2;
                int fe = 0;
                hw_wdt_feed();
                pm_set = bcmw_ioctl(w, 0, BCMW_C_SET_PM, &two, 4, 1, &fe) == BCMW_OK ? 1 : -1;
                sb_log(pm_set > 0 ? "wifi: power save on (PM 2)" : "wifi: power save refused (PM 2): stays off");
            }
            if (ntp_st == 1 && net.dns_state == SBNET_DNS_DONE) {
                uint8_t q[48];
                memset(q, 0, sizeof(q));
                q[0] = 0x23;                /* NTP version 4, client */
                for (int i = 0; i < 8; i += 4) {    /* a nonce in the transmit time: the answer must echo it */
                    uint32_t r = net_random(0);
                    memcpy(ntp_nonce + i, &r, 4);
                }
                memcpy(q + 40, ntp_nonce, 8);
                memcpy(ntp_server, net.dns_result, 4);
                ntp_t = sb_ms();
                ntp_st = sbnet_udp_send(&net, net.dns_result, 123, NTP_PORT, q, sizeof(q)) == 0 ? 2 : -1;
            } else if ((ntp_st == 1 && net.dns_state == SBNET_DNS_FAILED) ||
                       ((ntp_st == 1 || ntp_st == 2) && now - ntp_t > 8000)) {
                ntp_st = -1;
                clock_checked = -1;
                sb_log("wifi: no answer from the time server (the clock stays as it is)");
                notify();
            }
        }
    }
    if (fx_state == 1 && st != SBOS_WIFI_CONNECTED)
        fetch_poll();                       /* the network went: the fetch ends */
    if (st == SBOS_WIFI_CONNECTED && K.screen_on && fx_state != 1 && joined >= 0 && joined < nnets &&
        now - rssi_at >= 10000U && w_ready && !w->dead) {
        /* Track H: the joined network's signal now (WLC_GET_RSSI, brcmf_scb_val_le), for the status bar's bars */
        uint8_t sv[12];
        int fe = 0;
        rssi_at = now;
        memset(sv, 0, sizeof(sv));
        hw_wdt_feed();
        if (bcmw_ioctl(w, 0, 127, sv, sizeof(sv), 0, &fe) == BCMW_OK) {
            int32_t v = (int32_t)((uint32_t)sv[0] | (uint32_t)sv[1] << 8 | (uint32_t)sv[2] << 16 | (uint32_t)sv[3] << 24);
            if (v < 0 && v > -120) {
                int was = nets[joined].rssi;
                nets[joined].rssi = v;
                if ((was > -60) != (v > -60) || (was > -70) != (v > -70))
                    notify();               /* the bars changed */
            }
        }
    }
    int busy = st == SBOS_WIFI_STARTING || st == SBOS_WIFI_SCANNING || st == SBOS_WIFI_JOINING || st == SBOS_WIFI_DHCP ||
               join_pending || join_active || dhcp_go || fx_state == 1;
    uint32_t every = st == SBOS_WIFI_STARTING ? 10U : K.screen_on ? TICK_ON_MS : TICK_OFF_MS;
    if (irq_on && irq_n && irq_trusted && !busy)
        every = st == SBOS_WIFI_CONNECTED ? 250U : 1000U;   /* interrupts wake it; sbnet's timers need 250 ms */
    if (fx_state == 1)
        every = 5U;                         /* a fetch: TCP's segments and TLS's records without waiting */
    if (st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED && !kt_pending(KT_WIFI, 0))
        kt_set(KT_WIFI, 0, sb_ms(), every, 0);
}

/* ---- Track H's h1 self test (wifi_selftest_scan_only == 2): diagnostics, then the saved network on each band ---
 * After the scan: the firmware's capabilities (cap, logged at bring-up), the settings that matter for joining and
 * power (read only), what the heard networks ask for (counts by security and band: no names). Then, only when the
 * store's saved network ("wifinet") was heard: a join forced to 2.4 GHz (never seen to work: docs/WIFI-BCM4361.md
 * T4-WPA2 run b; once more with Bluetooth coexistence off if it fails), its address, the clock, one HTTPS GET of
 * example.com; then the same on 5 GHz, with a power-save check (interrupt counts idle with PM 0 and PM 2, the router's
 * pings under PM 2). Every step has its own time limit; the whole run at most 240 s. */
#define H1_URL "https://example.com/"
static int h1, h1_band, h1_btc_tried, h1_ok24, h1_ok5, h1_fails;
static uint64_t h1_t, h1_t0;
static uint32_t h1_btc_was = 0xffffffffu, h1_irq0, h1_pings;
static int h1_btc_set;

static void h1_iovar(const char *name)
{
    uint32_t v = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: wifi: h1: ");
    ub_s(&u, name);
    hw_wdt_feed();
    if (bcmw_iovar_get_u32(w, name, &v) == BCMW_OK) {
        ub_s(&u, " = ");
        ub_dec(&u, v);
    } else {
        ub_s(&u, ": not supported");
    }
    sb_log(u.b);
}

static void h1_diag(void)
{
    static const char *const sn[] = { "open", "WEP", "WPA", "WPA2", "WPA2/WPA3", "WPA3", "enterprise", "OWE", "other" };
    unsigned cnt[9], b5 = 0;
    struct ub u;
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i < nnets; ++i) {
        if (nsec[i] >= 0 && nsec[i] < 9)
            ++cnt[(int)nsec[i]];
        b5 += nets[i].band5 != 0;
    }
    ub_init(&u);
    ub_s(&u, "selftest: wifi: h1: heard by security:");
    for (int k = 0; k < 9; ++k)
        if (cnt[k]) {
            ub_c(&u, ' ');
            ub_s(&u, sn[k]);
            ub_c(&u, ' ');
            ub_dec(&u, cnt[k]);
        }
    ub_s(&u, "; strongest side on 5 GHz: ");
    ub_dec(&u, b5);
    ub_s(&u, " of ");
    ub_dec(&u, (uint64_t)nnets);
    sb_log(u.b);
    sb_logx("selftest: wifi: h1: firmware capability bits (1 sae, 2 sae_ext, 4 mfp, 8 idsup, 16 wpa3): ", fw_caps);
    {
        uint32_t pm = 0xffffffffu;
        int fe = 0;
        hw_wdt_feed();
        if (bcmw_ioctl(w, 0, BCMW_C_GET_PM, &pm, 4, 0, &fe) == BCMW_OK)
            logn("selftest: wifi: h1: PM (power save mode) = ", pm, "");
    }
    static const char *const iv[] = { "mfp", "btc_mode", "bcn_li_dtim", "assoc_listen", "pm2_sleep_ret", "roam_off",
                                      "wme", "nmode", "vhtmode", "mimo_bw_cap", "bw_cap", "sup_wpa", "wpa_auth" };
    for (unsigned k = 0; k < sizeof(iv) / sizeof(iv[0]); ++k)
        h1_iovar(iv[k]);
    if (!sv_ok) {
        sb_log("selftest: wifi: h1: no saved network on the phone: diagnostics only (no join)");
        return;
    }
    ub_init(&u);
    ub_s(&u, "selftest: wifi: h1: the saved network (name not logged): 2.4 GHz ");
    if (sv_heard24) {
        ub_sdec(&u, sv_rssi24);
        ub_s(&u, " dBm");
    } else {
        ub_s(&u, "not heard");
    }
    ub_s(&u, ", 5 GHz ");
    if (sv_heard5) {
        ub_sdec(&u, sv_rssi5);
        ub_s(&u, " dBm");
    } else {
        ub_s(&u, "not heard");
    }
    for (int i = 0; i < nnets; ++i)
        if (nets[i].ssid_len == sv_len && !memcmp(nets[i].ssid, sv_ssid, sv_len) && nsec[i] >= 0 && nsec[i] < 9) {
            ub_s(&u, ", security ");
            ub_s(&u, sn[(int)nsec[i]]);
        }
    sb_log(u.b);
}

static int h1_saved_index(void)
{
    for (int i = 0; i < nnets; ++i)
        if (nets[i].ssid_len == sv_len && !memcmp(nets[i].ssid, sv_ssid, sv_len))
            return i;
    return -1;
}

static void h1_say(const char *what)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: wifi: h1: ");
    ub_s(&u, h1_band == 1 ? "2.4 GHz: " : "5 GHz: ");
    ub_s(&u, what);
    ub_s(&u, " (");
    ub_dec(&u, sb_ms() - h1_t);
    ub_s(&u, " ms)");
    sb_log(u.b);
}

static void h1_btc_restore(void)
{
    if (h1_btc_set && h1_btc_was != 0xffffffffu && w_ready && !w->dead) {
        hw_wdt_feed();
        (void)bcmw_iovar_set_u32(w, "btc_mode", h1_btc_was);
        logn("selftest: wifi: h1: btc_mode back to ", h1_btc_was, "");
    }
    h1_btc_set = 0;
}

/* stages: 1 start a join on h1_band, 2 joining (to CONNECTED and the clock), 3 the fetch, 4 PM 0 idle window,
 * 5 PM 2 idle window, 6 pings under PM 2, 7 leave, settle, next band; 9 done */
static int h1_step(int *done)
{
    uint64_t now = sb_ms();
    if (now - h1_t0 > 240000U) {
        sb_log("selftest: wifi: h1: the whole run passed 240 s: stopping");
        ++h1_fails;
        h1 = 9;
    }
    switch (h1) {
    case 1: {
        int i = h1_saved_index();
        if (i < 0 || (h1_band == 1 && !sv_heard24) || (h1_band == 2 && !sv_heard5)) {
            h1_say("the saved network was not heard on this band: skipped");
            h1 = 7;
            h1_t = now;
            break;
        }
        w->cfg.join_band = (unsigned)h1_band;
        h1_t = now;
        if (wifi_join(i, 0)) {
            h1_say("the join was refused");
            h1 = 7;
            break;
        }
        h1_say("joining (the band forced)");
        h1 = 2;
        break;
    }
    case 2:
        if (st == SBOS_WIFI_CONNECTED && (ntp_st == 3 || ntp_st == -1)) {
            h1_say(pinged == 2 ? "JOINED: an address, the router answers" : "JOINED: an address (no ping answer yet)");
            h1_say(ntp_st == 3 ? "the clock checked over the network" : "no time server answer");
            if (h1_band == 1)
                h1_ok24 = 1;
            else
                h1_ok5 = 1;
            h1_t = now;
            if (wifi_fetch(H1_URL)) {
                h1_say("the HTTPS GET did not start");
                h1 = h1_band == 2 ? 4 : 7;
            } else {
                h1 = 3;
            }
        } else if (st == SBOS_WIFI_UP || st == SBOS_WIFI_FAILED || st == SBOS_WIFI_OFF || now - h1_t > 40000U) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "did NOT join: ");
            ub_s(&u, why[0] ? why : st == SBOS_WIFI_DHCP ? "no address in 40 s" : "no result in 40 s");
            h1_say(u.b);
            if (st != SBOS_WIFI_UP && st != SBOS_WIFI_FAILED && st != SBOS_WIFI_OFF)
                (void)wifi_leave();
            if (h1_band == 1 && !h1_btc_tried && st == SBOS_WIFI_UP) {
                /* once more with Bluetooth coexistence off (Bluetooth is not powered here; a runtime setting, gone
                 * at the radio's next reset) */
                h1_btc_tried = 1;
                hw_wdt_feed();
                if (bcmw_iovar_get_u32(w, "btc_mode", &h1_btc_was) != BCMW_OK)
                    h1_btc_was = 0xffffffffu;
                hw_wdt_feed();
                if (bcmw_iovar_set_u32(w, "btc_mode", 0) == BCMW_OK) {
                    h1_btc_set = 1;
                    logn("selftest: wifi: h1: 2.4 GHz again with btc_mode 0 (it was ", h1_btc_was, ")");
                    h1 = 1;
                    break;
                }
                sb_log("selftest: wifi: h1: btc_mode 0 refused: no second 2.4 GHz try");
            }
            h1 = 7;
            h1_t = now;
        }
        break;
    case 3:
        if (fx_state == 2 || now - h1_t > 30000U) {
            struct sbos_wifi_fetch f;
            if (fx_state != 2)
                wifi_fetch_cancel();
            wifi_fetch_status(&f);
            struct ub u;
            ub_init(&u);
            ub_s(&u, f.result == 0 ? "HTTPS WORKS: status " : "HTTPS FAILED: ");
            if (f.result == 0) {
                ub_dec(&u, (uint64_t)f.status);
                ub_s(&u, ", bytes ");
                ub_dec(&u, f.bytes);
                ub_s(&u, ", TLS ms ");
                ub_dec(&u, f.tls_ms);
                ub_s(&u, ", total ms ");
                ub_dec(&u, f.total_ms);
            } else {
                ub_s(&u, f.why);
                if (f.tls_err) {
                    ub_s(&u, " (TLS error ");
                    ub_dec(&u, (uint64_t)f.tls_err);
                    ub_s(&u, ")");
                }
            }
            h1_say(u.b);
            h1_t = now;
            h1_irq0 = irq_n;
            h1 = h1_band == 2 ? 4 : 7;
        }
        break;
    case 4:                                 /* 5 GHz: 5 s idle with PM 0 (the join's setting) */
        if (now - h1_t >= 5000U) {
            logn("selftest: wifi: h1: power: radio interrupts in 5 s idle with PM 0: ", irq_n - h1_irq0, "");
            uint32_t two = 2;
            int fe = 0;
            hw_wdt_feed();
            if (bcmw_ioctl(w, 0, BCMW_C_SET_PM, &two, 4, 1, &fe) != BCMW_OK) {
                sb_logx("selftest: wifi: h1: power: PM 2 refused, error ", (uint64_t)(uint32_t)fe);
                h1 = 7;
            } else {
                h1 = 5;
            }
            h1_t = now;
            h1_irq0 = irq_n;
        }
        break;
    case 5:
        if (now - h1_t >= 5000U) {
            logn("selftest: wifi: h1: power: radio interrupts in 5 s idle with PM 2: ", irq_n - h1_irq0, "");
            h1_t = now;
            h1_pings = net.ping_replies;
            (void)sbnet_ping(&net, net.gateway);
            h1 = 6;
        }
        break;
    case 6: {
        unsigned sent = (unsigned)((now - h1_t) / 1000U);
        static unsigned last_sent;
        if (sent != last_sent && sent < 5 && st == SBOS_WIFI_CONNECTED) {
            last_sent = sent;
            (void)sbnet_ping(&net, net.gateway);
        }
        if (now - h1_t >= 6000U) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "selftest: wifi: h1: power: under PM 2 the router answered ");
            ub_dec(&u, net.ping_replies - h1_pings);
            ub_s(&u, " of 5 pings, the last in ");
            ub_dec(&u, net.ping_rtt_ms);
            ub_s(&u, " ms");
            sb_log(u.b);
            last_sent = 0;
            h1 = 7;
            h1_t = now;
        }
        break;
    }
    case 7:
        if (st == SBOS_WIFI_JOINING || st == SBOS_WIFI_DHCP || st == SBOS_WIFI_CONNECTED)
            (void)wifi_leave();
        if (h1_band == 1)
            h1_btc_restore();
        if (now - h1_t < 1500U)
            break;                          /* the firmware settles after the leave */
        if (h1_band == 1) {
            h1_band = 2;
            h1 = 1;
        } else {
            h1 = 9;
        }
        break;
    default:
        break;
    }
    if (h1 != 9)
        return 1;
    h1_btc_restore();
    if (w_ready)
        w->cfg.join_band = 0;
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: wifi: h1: summary: 2.4 GHz ");
        ub_s(&u, !sv_ok || !sv_heard24 ? "not tried" : h1_ok24 ? "JOINED" : "did not join");
        ub_s(&u, h1_btc_tried ? " (also tried with btc_mode 0)" : "");
        ub_s(&u, ", 5 GHz ");
        ub_s(&u, !sv_ok || !sv_heard5 ? "not tried" : h1_ok5 ? "JOINED" : "did not join");
        sb_log(u.b);
    }
    if (sv_ok && !h1_ok24 && sv_heard24) {
        sb_log("selftest: WARNING: wifi: h1: 2.4 GHz did not join (the open point; the log above says how far)");
        ++selftest_warnings;
    }
    int ok = nnets > 0 && !h1_fails && (!sv_ok || !sv_heard5 || h1_ok5);
    wifi_power(0);
    *done = 1;
    return ok;
}

/* the self test: on, the bring-up, one scan; 0.6.1: then, if the saved network ("wifinet") was heard, a join with
 * its passphrase (WPA2 or open), an address, the router's ping and the clock check; then off. 1 passed, 0 failed */
int wifi_selftest_scan_only;                /* Next's Wi-Fi self test: the scan only, never a join */

int wifi_selftest_phase(int *done)
{
    static int started, jstate;            /* jstate: 0 not yet, 1 joining, 2 done */
    static int seen_dhcp;                   /* 1.2-alpha25: the join and its handshake done, the address asked for */
    static uint64_t jt0;
    *done = 0;
    if (h1)
        return h1_step(done);
    if (jstate == 1) {
        if (st == SBOS_WIFI_DHCP)
            seen_dhcp = 1;
        int failed = st == SBOS_WIFI_UP || st == SBOS_WIFI_FAILED || st == SBOS_WIFI_OFF;
        int finished = st == SBOS_WIFI_CONNECTED && (ntp_st == 3 || ntp_st == -1);   /* the clock checked, or no answer */
        if (failed || finished || sb_ms() - jt0 > 45000U) {
            jstate = 2;
            if (st == SBOS_WIFI_CONNECTED) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "selftest: wifi: JOINED the saved network, an address; the router answered: ");
                ub_s(&u, pinged == 2 ? "yes" : "no");
                ub_s(&u, "; the clock checked: ");
                ub_s(&u, ntp_st == 3 ? "yes" : "no");
                ub_s(&u, "; ");
                ub_dec(&u, sb_ms() - jt0);
                ub_s(&u, " ms");
                sb_log(u.b);
            } else if (seen_dhcp) {
                /* 1.2-alpha25 (the main session): the join and the 4-way handshake are ours and passed; the address
                 * is the router's: a warning, not a failure (alpha24 on the phone: three DISCOVERs unanswered) */
                sb_log("selftest: WARNING: wifi: joined (the handshake done) but no address (DHCP; the router)");
                ++selftest_warnings;
            } else {
                sb_logf("selftest: wifi: the saved network did NOT join: ", why[0] ? why : "no address in 45 s");
            }
            int ok = st == SBOS_WIFI_CONNECTED || seen_dhcp;
            wifi_power(0);
            *done = 1;
            return ok;
        }
        return 1;
    }
    if (!started) {
        started = 1;
        wifi_power(0);                      /* from off: a radio already on would make t_on stale */
        if (wifi_selftest_scan_only) {
            /* Next: TLS's own check first (no network): known answers and two real certificate chains */
            uint64_t t0 = sb_ms();
            hw_wdt_feed();
            int tr = wifi_tls_selfcheck();
            hw_wdt_feed();
            logn(tr ? "selftest: wifi: the TLS self-check FAILED at check " : "selftest: wifi: the TLS self-check passed in ",
                 tr ? (uint64_t)tr : sb_ms() - t0, tr ? "" : " ms");
            if (tr) {
                *done = 1;
                return 0;
            }
        }
    }
    if (st == SBOS_WIFI_OFF) {
        if (wifi_power(1)) {
            *done = 1;
            return 0;
        }
        return 1;
    }
    if (st == SBOS_WIFI_FAILED) {
        *done = 1;
        return 0;
    }
    if (st == SBOS_WIFI_UP && !scan_retry) {
        static int up_logged;               /* once: h1 comes back here while it waits for its second scan */
        if (!up_logged++) {
            logn("selftest: wifi: up and scanned in ", sb_ms() - t_on, " ms");
            logn("selftest: wifi: networks heard: ", (uint64_t)nnets, " (names not logged)");
            logn("selftest: wifi: interrupts during the bring-up and scan: ", irq_n,
                 irq_storms ? " (a STORM: off, polling)" : irq_on ? " (on)" : " (polling)");
            if (irq_missed)
                logn("selftest: wifi: work found without an interrupt, times: ", irq_missed, "");
        }
        int heard = -1;
        if (wifi_selftest_scan_only == 2) {
            /* h1b: a second scan 2 s after the first (the first one starts while the bring-up's own may still run) */
            static int rescan;
            static uint64_t rescan_t;
            if (rescan == 0) {
                rescan = 1;
                rescan_t = sb_ms();
                sb_log("selftest: wifi: h1: a second scan in 2 s (does the first one miss access points?)");
                return 1;
            }
            if (rescan == 1) {
                if (sb_ms() - rescan_t < 2000U)
                    return 1;
                rescan = 2;
                if (wifi_scan() == 0)
                    return 1;               /* back here when it has ended (state UP again) */
            }
            logn("selftest: wifi: h1: networks (names) after the second scan: ", (uint64_t)nnets, "");
            h1_diag();
            h1_t0 = h1_t = sb_ms();
            if (!sv_ok || (!sv_heard24 && !sv_heard5)) {
                if (sv_ok)
                    sb_log("selftest: wifi: h1: the saved network was not heard: no join");
                h1 = 9;
            } else {
                h1_band = sv_heard24 ? 1 : 2;
                h1 = 1;
            }
            return h1_step(done);
        }
        if (wifi_selftest_scan_only) {
            int secure = 0;
            for (int i = 0; i < nnets; ++i)
                secure += nets[i].secure != 0;
            logn("selftest: wifi: secured networks among them: ", (uint64_t)secure, "");
            sb_log(nnets ? "selftest: wifi: passed: scan only (no join in this test)" :
                           "selftest: wifi: FAILED: the scan heard nothing");
            int ok = nnets > 0;             /* before wifi_power(0), which empties the list */
            wifi_power(0);
            *done = 1;
            return ok;
        }
        for (int i = 0; sv_ok && jstate == 0 && i < nnets; ++i)
            if (nets[i].ssid_len == sv_len && !memcmp(nets[i].ssid, sv_ssid, sv_len))
                heard = i;
        if (heard >= 0) {
            sb_log(nets[heard].secure ? "selftest: wifi: the saved network was heard: joining it (WPA2; name not logged)"
                                      : "selftest: wifi: the saved network was heard: joining it (open; name not logged)");
            jt0 = sb_ms();
            if (wifi_join(heard, 0) == 0) {
                jstate = 1;
                return 1;
            }
            sb_log("selftest: wifi: the join was refused");
            wifi_power(0);
            *done = 1;
            return 0;
        }
        sb_log(sv_ok ? "selftest: wifi: the saved network was not heard: no join" : "selftest: wifi: no saved network: no join");
        wifi_power(0);
        *done = 1;
        return 1;
    }
    if (jstate == 0 && sb_ms() - t_on > 30000U) {
        sb_log("selftest: wifi: not up within 30 s");
        wifi_power(0);
        *done = 1;
        return 0;
    }
    return 1;
}
