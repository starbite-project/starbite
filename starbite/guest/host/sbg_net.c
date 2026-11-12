/* SPDX-License-Identifier: GPL-2.0-only */
/* sbg_net: the host side of sbnet (contract v3, ../sbshm-net.h): the guest's raw IP packets to the host's NAT
 * (Track G) and back. Built inside sbghost.c (#include at its end), so it uses sbghost's text helpers and state.
 *
 *   tx ring (guest -> host): sbg_net_poll() drains at most NET_PER_POLL packets per poll, each copied once into a
 *     private buffer (the guest could rewrite the slot under us), tx_tail advanced, then sbg_os.net_rx(buf, len).
 *     A packet of a bad length, or any packet while link = 0, is dropped and counted in rx_drop.
 *   rx ring (host -> guest): sbg_net_send() writes one packet into the next slot (data, then len, dmb, rx_head) and
 *     pends the guest's doorbell. 0 (and rx_drop + 1) when the ring is full, there is no link, the length is not
 *     1..mtu, or the guest is not loaded / running.
 *   config: sbg_net_config() keeps the NAT's view (link, uplink, addresses) here, so it may be called at any time
 *     (also before the first sbg_load: the next load lays it out); while the guest is up it rewrites the struct
 *     under cfg_seq (odd while writing) and pends the guest's doorbell. Until the first call: link 0 (no uplink),
 *     10.77.0.2/30, gateway and DNS 10.77.0.1. NULL addresses / prefix 0 keep the current ones.
 * The host keeps its own ring indices (net_tx_tail, net_rx_head) and never trusts the guest's copies beyond a
 * range check: a tx_head more than a ring ahead stops the service (logged once). */
#include "sbghost_int.h"
#include "../sbshm-net.h"

#define NET_PER_POLL 64U                            /* tx packets per sbg_poll (64 x 1400 bytes = 88 KiB) */
#define NET_PKT_MAX (SBSHM_NET_SLOT - 8U)

static struct {
    uint32_t link, uplink, prefix;
    uint8_t guest[4], gw[4], dns[4];
} ncfg = {0, SBSHM_NET_UPLINK_NONE, 30, {10, 77, 0, 2}, {10, 77, 0, 1}, {10, 77, 0, 1}};
static uint32_t net_on, net_bad, net_seq, net_tx_tail, net_rx_head, net_rx_drop;
static const uint32_t net_mtu = SBSHM_NET_MTU;
static uint32_t net_buf[NET_PKT_MAX / 4];           /* the private copy handed to net_rx */

#define NETS ((volatile struct sbshm_net *)sbg_shm(SBSHM_NET_OFF))

static uintptr_t net_slot(uint32_t ring_off, uint32_t i)
{
    return sbg_shm(SBSHM_NET_OFF + ring_off + (i & (SBSHM_NET_SLOTS - 1U)) * SBSHM_NET_SLOT);
}

/* the service is offered in this boot (laid out by sbg_net_load) */
static int net_live(void) { return net_on && (sbg_services() & SBSHM_SVC_NET); }
/* ... and the guest is loaded or running: the rings may be written and the doorbell pended */
static int net_up(void)
{
    return net_live() && (st.state == SBG_LOADED || st.state == SBG_BOOTING || st.state == SBG_RUNNING);
}

static void net_cfg_write(void)
{
    volatile struct sbshm_net *n = NETS;
    n->cfg_seq = ++net_seq;                         /* odd: writing */
    sbshm_dmb();
    n->link = ncfg.link;
    n->uplink = ncfg.uplink;
    n->mtu = net_mtu;
    for (unsigned i = 0; i < 4; ++i) {
        n->guest_ip[i] = ncfg.guest[i];
        n->gw_ip[i] = ncfg.gw[i];
        n->dns_ip[i] = ncfg.dns[i];
    }
    n->prefix = ncfg.prefix;
    sbshm_dmb();
    n->cfg_seq = ++net_seq;                         /* even: whole */
}

static void t_ip(struct tb *t, const uint8_t ip[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        if (i)
            t_s(t, ".");
        t_u(t, ip[i]);
    }
}

static void net_say_cfg(const char *what)
{
    struct tb t = {.n = 0};
    t_s(&t, "guest: sbnet: ");
    t_s(&t, what);
    t_s(&t, ": link ");
    t_u(&t, ncfg.link);
    t_s(&t, ncfg.uplink == SBSHM_NET_UPLINK_WIFI ? " (wifi), " : ncfg.uplink == SBSHM_NET_UPLINK_CELL ? " (cell), "
                                                                                                    : " (none), ");
    t_ip(&t, ncfg.guest);
    t_s(&t, "/");
    t_u(&t, ncfg.prefix);
    t_s(&t, " gw ");
    t_ip(&t, ncfg.gw);
    t_s(&t, " dns ");
    t_ip(&t, ncfg.dns);
    t_s(&t, " mtu ");
    t_u(&t, net_mtu);
    say(&t);
}

void sbg_net_load(void)
{
    net_on = (sbg_services() & SBSHM_SVC_NET) != 0;
    net_bad = net_seq = net_tx_tail = net_rx_head = net_rx_drop = 0;
    if (!net_on)
        return;
    net_cfg_write();                                /* the region was zeroed by load: indices 0, drops 0 */
    net_say_cfg("laid out");
}

void sbg_net_poll(void)
{
    if (!net_live() || net_bad)
        return;
    volatile struct sbshm_net *n = NETS;
    const struct sbg_os *o = sbg_os_();
    for (unsigned k = 0; k < NET_PER_POLL; ++k) {
        uint32_t head = n->tx_head;
        if (head == net_tx_tail)
            break;
        if (head - net_tx_tail > SBSHM_NET_SLOTS) {     /* not a ring any more: stop serving, say so once */
            net_bad = 1;
            struct tb t = {.n = 0};
            t_s(&t, "guest: sbnet: the tx ring is broken (tx_head ");
            t_u(&t, head);
            t_s(&t, ", tx_tail ");
            t_u(&t, net_tx_tail);
            t_s(&t, "): net service stopped");
            say(&t);
            return;
        }
        sbshm_dmb();
        uintptr_t s = net_slot(SBSHM_NET_TX_OFF, net_tx_tail);
        uint32_t len = rd32(s) & 0xffffU;
        int ok = len && len <= net_mtu && ncfg.link && o->net_rx;
        if (ok)
            for (uint32_t i = 0; i < (len + 3U) / 4U; ++i)
                net_buf[i] = rd32(s + 8U + 4U * i);
        sbshm_dmb();                                /* the copy, then the slot back to the guest */
        n->tx_tail = ++net_tx_tail;
        if (!ok) {
            n->rx_drop = ++net_rx_drop;
            continue;
        }
        o->net_rx((const uint8_t *)net_buf, len);
    }
}

int sbg_net_send(const uint8_t *pkt, unsigned len)
{
    if (!net_up())
        return 0;
    volatile struct sbshm_net *n = NETS;
    if (!ncfg.link || !len || len > net_mtu || net_rx_head - n->rx_tail >= SBSHM_NET_SLOTS) {
        n->rx_drop = ++net_rx_drop;
        return 0;
    }
    uintptr_t s = net_slot(SBSHM_NET_RX_OFF, net_rx_head), d = s + 8U;
    unsigned w = len / 4U, i;
    if (!((uintptr_t)pkt & 3U)) {
        typedef uint32_t __attribute__((may_alias)) u32a;
        const u32a *p = (const u32a *)(const void *)pkt;
        for (i = 0; i < w; ++i)
            wr32(d + 4U * i, p[i]);
    } else {
        for (i = 0; i < w; ++i) {
            const uint8_t *p = pkt + 4U * i;
            wr32(d + 4U * i, (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        }
    }
    if (len & 3U) {                                 /* the tail, without reading past the caller's packet */
        uint32_t v = 0;
        for (unsigned b = 0; b < (len & 3U); ++b)
            v |= (uint32_t)pkt[4U * w + b] << (8 * b);
        wr32(d + 4U * w, v);
    }
    wr32(s, len);                                   /* len, flags 0 */
    wr32(s + 4U, 0);
    sbshm_dmb();
    n->rx_head = ++net_rx_head;
    sbg_ring_guest();
    return 1;
}

void sbg_net_config(int link, int uplink, const uint8_t guest_ip[4], const uint8_t gw_ip[4], const uint8_t dns_ip[4],
                    unsigned prefix)
{
    ncfg.link = link ? 1U : 0U;
    ncfg.uplink = (uint32_t)uplink;
    for (unsigned i = 0; i < 4; ++i) {
        if (guest_ip)
            ncfg.guest[i] = guest_ip[i];
        if (gw_ip)
            ncfg.gw[i] = gw_ip[i];
        if (dns_ip)
            ncfg.dns[i] = dns_ip[i];
    }
    if (prefix && prefix <= 32)
        ncfg.prefix = prefix;
    net_say_cfg(net_live() ? "config" : "config (for the next guest start)");
    if (!net_live())
        return;
    net_cfg_write();
    if (net_up())
        sbg_ring_guest();
}
