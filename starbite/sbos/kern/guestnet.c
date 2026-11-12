/* SPDX-License-Identifier: GPL-2.0-only
 * The Linux guest's network (Track G, with Track E's sbnet link: starbite/guest/sbshm-net.h, sbghost's sbg_net_*).
 *
 * The guest is 10.77.0.2/30 on a point-to-point ring of raw IPv4 packets; Starbite OS is 10.77.0.1, its gateway and
 * DNS. starbite/modem/nat/sbnat.c masquerades the guest onto whichever uplink is active (Wi-Fi when joined, else
 * mobile data: the same rule as wifi_fetch), using external ports 40000-47999 (the host's own sbtcp uses 49152+).
 *   guest -> sbos_nat_from_guest (sbghost's net_rx, from sbg_poll) -> sbnat_from_guest -> sbnet_ip_send on the uplink
 *            (the IP header is rebuilt by sbnet: TTL 64, DF; the transport checksum stays valid: same addresses)
 *   uplink -> guestnet_rx (modem.c's raw-IP rx, wifi.c's frames) -> sbnat_from_uplink -> sbg_net_send, else the
 *            host's own stack
 * Uplink changes (guestnet_uplink) drop every mapping and tell the guest (sbg_net_config). Only counters are logged:
 * never an address, a port or a byte of a packet.
 * Built empty until the guest contract v3 (SBSHM_VERSION_V3, Track E) is in the tree. */
#include "sb.h"
#include "kern.h"
#include "net.h"
#include "../../modem/nat/sbnat.h"
#include "../../guest/sbshm.h"              /* SBSHM_VERSION_V3: Track E's contract with the net rings */

#ifdef SBSHM_VERSION_V3
#include "../../guest/host/sbghost.h"
#include "../../guest/sbshm-net.h"

static struct sbnat gn;
static int gn_init, gn_up;                  /* gn_up: the active uplink (SBSHM_NET_UPLINK_*) */
static struct sbnet *gn_net[3];             /* [WIFI], [CELL]: the interfaces while they are up */
static uint8_t gn_buf[14 + 20 + 2048];      /* a packet being forwarded: sbnet wants 34 bytes of headroom */
static uint32_t gn_tx, gn_tx_fail, gn_rx;
static const uint8_t gn_guest[4] = SBNAT_GUEST_ADDR, gn_gw[4] = SBNAT_GW_ADDR;

static void gn_pick(void)
{
    int up = gn_net[SBSHM_NET_UPLINK_WIFI] ? SBSHM_NET_UPLINK_WIFI :
             gn_net[SBSHM_NET_UPLINK_CELL] ? SBSHM_NET_UPLINK_CELL : SBSHM_NET_UPLINK_NONE;
    static const uint8_t none[4] = { 0, 0, 0, 0 };
    if (!gn_init) {
        struct sbnat_cfg c;
        memset(&c, 0, sizeof(c));
        c.now_ms = sb_ms();
        sbnat_init(&gn, &c);
        gn_init = 1;
    }
    struct sbnet *n = up ? gn_net[up] : 0;
    sbnat_uplink_changed(&gn, n ? n->ip : none, n ? n->dns : none);
    gn_up = up;
    sbg_net_config(up != SBSHM_NET_UPLINK_NONE, up, gn_guest, gn_gw, gn_gw, 30);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guestnet: the Linux guest's uplink: ");
    ub_s(&u, up == SBSHM_NET_UPLINK_WIFI ? "Wi-Fi" : up == SBSHM_NET_UPLINK_CELL ? "mobile data" : "none");
    ub_s(&u, " (forwarded ");
    ub_dec(&u, gn_tx);
    ub_s(&u, " out, ");
    ub_dec(&u, gn_rx);
    ub_s(&u, " in so far)");
    sb_log(u.b);
}

void guestnet_uplink(int which, struct sbnet *n)
{
    if (which != SBSHM_NET_UPLINK_WIFI && which != SBSHM_NET_UPLINK_CELL)
        return;
    if (gn_net[which] == n && gn_init)
        return;
    gn_net[which] = n;
    gn_pick();
}

/* the guest started: it begins with link 0; tell it about an uplink that is already up */
void guestnet_guest_started(void)
{
    gn_pick();
}

int guestnet_rx(int which, const uint8_t *ip, uint32_t len)
{
    if (!gn_init || which != gn_up || len < 20 || len > sizeof(gn_buf) - 34)
        return 0;
    /* only what can be ours: TCP / UDP to the external range, ICMP (echo replies and errors) */
    uint8_t *p = gn_buf + 34;
    memcpy(p, ip, len);
    int r = sbnat_from_uplink(&gn, p, len, sb_ms());
    if (r == SBNAT_TO_GUEST) {
        uint32_t total = (uint32_t)p[2] << 8 | p[3];
        if (total > len)
            total = len;
        sbg_net_send(p, total);
        ++gn_rx;
        return 1;
    }
    return r == SBNAT_DROP;                 /* a damaged packet of a guest flow: the host's stack gets nothing */
}

void sbos_nat_from_guest(const uint8_t *pkt, unsigned len)
{
    if (!gn_init)
        gn_pick();
    if (len < 20 || len > sizeof(gn_buf) - 34)
        return;
    uint8_t *p = gn_buf + 34;
    memcpy(p, pkt, len);
    uint64_t now = sb_ms();
    int r = sbnat_from_guest(&gn, p, len, now);
    if (r == SBNAT_LOCAL) {
        uint32_t n = sbnat_echo_reply(p, len);
        if (n)
            sbg_net_send(p, n);
        return;
    }
    if (r != SBNAT_SEND || !gn_up || !gn_net[gn_up])
        return;
    uint32_t ihl = (uint32_t)(p[0] & 15) * 4, total = (uint32_t)p[2] << 8 | p[3];
    if (ihl < 20 || total < ihl || total > len) {
        ++gn_tx_fail;
        return;
    }
    uint8_t dst[4], proto = p[9];
    memcpy(dst, p + 16, 4);
    /* the payload goes right after sbnet's 34 bytes of headroom (Ethernet + a 20-byte IP header) */
    memmove(gn_buf + 34, p + ihl, total - ihl);
    if (sbnet_ip_send(gn_net[gn_up], dst, proto, gn_buf, total - ihl) == 0)
        ++gn_tx;
    else
        ++gn_tx_fail;
    sbnat_tick(&gn, now);
}
#else
void guestnet_uplink(int which, struct sbnet *n) { (void)which; (void)n; }
void guestnet_guest_started(void) { }
int guestnet_rx(int which, const uint8_t *ip, uint32_t len) { (void)which; (void)ip; (void)len; return 0; }
#endif
