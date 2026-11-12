/* SPDX-License-Identifier: GPL-2.0-only
 * net-nat-e2e: sbnet's host side with Track G's real NAT, end to end, without QEMU. This program plays the guest's
 * kernel on the rings (packets as Linux builds them: full checksums, TTL 64, source 10.77.0.2); sbg_net.c (inside
 * sbghost.c) hands them to an sbg_os.net_rx that runs sbnat_from_guest; SBNAT_SEND goes to a fake uplink (sbsim's
 * responder, sim_net.c: ICMP echo, UDP echo on 7, a DNS server), whose answers run through sbnat_from_uplink and,
 * SBNAT_TO_GUEST, sbg_net_send back onto the ring; SBNAT_LOCAL pings get sbnat_echo_reply. The guest checks every
 * answer: addresses restored, IP and L4 checksums whole, TTL decremented by the NAT (64 -> 63), payloads intact.
 *   NAT=<dir with sbnat.h and sbnat.c> (starbite/modem/nat; from another branch:
 *       git show 1c2e473:starbite/modem/nat/sbnat.c > $NAT/sbnat.c, the same for sbnat.h)
 *   cc -Wall -Wextra -O2 -I.. -I$NAT v3test/net-nat-e2e.c $NAT/sbnat.c -o /tmp/net-nat-e2e && /tmp/net-nat-e2e
 * Last line "net-nat-e2e: PASS|FAIL". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../host/sbghost.c"
#include "sbnat.h"

static uint64_t fake_ms = 1000;
static uint64_t now_ms(void) { return fake_ms; }
static void puts_(const char *s) { fputs(s, stdout); }
static void putu(uint64_t v) { printf("%llu", (unsigned long long)v); }
/* the fake uplink's answers are captured (sim_net.c's sbg_net_send calls), not put on the guest's ring */
static uint8_t up_reply[2048];
static unsigned up_reply_len;
static int uplink_send(const uint8_t *p, unsigned len)
{
    memcpy(up_reply, p, len);
    up_reply_len = len;
    return 1;
}
#define sbg_net_send uplink_send
#define sbg_net_config(l, ...) ((void)(l))
#include "../sbsim/sim_net.c"
#undef sbg_net_send
#undef sbg_net_config

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf(" (line %d)\n", __LINE__); } } while (0)

static const uint8_t UPLINK[4] = {192, 168, 1, 50}, UPDNS[4] = {192, 168, 1, 1};
static struct sbnat nat;
static struct { unsigned send, local, drop, to_guest, not_ours; uint8_t ttl_out; } ns;

/* the fake uplink: the internet answers (sim_net's responder); its DNS server is UPDNS (sim_net answers DNS at
 * 10.77.0.1, so the address is swapped around the call and the checksums redone) */
static void fix_udp_ip(uint8_t *p)
{
    unsigned hl = (p[0] & 15U) * 4U, ul = sn_be16(p + hl + 4);
    sn_put16(p + 10, 0);
    sn_put16(p + 10, sn_fold(sn_sum(0, p, hl)));
    sn_put16(p + hl + 6, 0);
    uint32_t s = sn_sum(sn_sum(0, p + 12, 8), p + hl, ul) + 17U + ul;
    unsigned c = sn_fold(s);
    sn_put16(p + hl + 6, c ? c : 0xffffU);
}
static void uplink(uint8_t *p, unsigned len)
{
    int dns = p[9] == 17 && !memcmp(p + 16, UPDNS, 4);
    ns.ttl_out = p[8];
    if (dns) {
        memcpy(p + 16, sim_ip_gw, 4);
        fix_udp_ip(p);
    }
    up_reply_len = 0;
    sim_net_rx(p, len);
    if (!up_reply_len)
        return;
    if (dns) {
        memcpy(up_reply + 12, UPDNS, 4);
        fix_udp_ip(up_reply);
    }
    int r = sbnat_from_uplink(&nat, up_reply, up_reply_len, fake_ms);
    if (r == SBNAT_TO_GUEST) {
        ns.to_guest++;
        sbg_net_send(up_reply, up_reply_len);
    } else {
        ns.not_ours++;
    }
}
/* sbg_os.net_rx: what Track G's glue does with a packet from the guest */
static void nat_rx(const uint8_t *pkt, unsigned len)
{
    static uint8_t p[2048];
    memcpy(p, pkt, len);                            /* sbnat rewrites in place; pkt is sbghost's private copy */
    int r = sbnat_from_guest(&nat, p, len, fake_ms);
    if (r == SBNAT_SEND) {
        ns.send++;
        uplink(p, len);
    } else if (r == SBNAT_LOCAL) {
        ns.local++;
        uint32_t n = sbnat_echo_reply(p, len);
        if (n)
            sbg_net_send(p, n);
    } else {
        ns.drop++;
    }
}

static void host_log(const char *l) { printf("  host| %s\n", l); }
static uint64_t ms_cb(void) { return fake_ms; }
static const struct sbg_os tos = {.ms = ms_cb, .log = host_log, .net_rx = nat_rx};

/* ---- the guest ---- */
static volatile struct sbshm_net *N;
static uint32_t g_tx_head, g_rx_tail;
static volatile struct sbshm_net_slot *slot(uint32_t ring, uint32_t i)
{
    return (volatile struct sbshm_net_slot *)((uintptr_t)N + ring + (i % SBSHM_NET_SLOTS) * SBSHM_NET_SLOT);
}
static void g_send(const uint8_t *p, unsigned len)
{
    volatile struct sbshm_net_slot *s = slot(SBSHM_NET_TX_OFF, g_tx_head);
    for (unsigned i = 0; i < len; ++i)
        s->data[i] = p[i];
    s->len = (uint16_t)len;
    s->flags = 0;
    N->tx_head = ++g_tx_head;
    sbg_net_poll();
}
static unsigned g_recv(uint8_t *p)
{
    if (N->rx_head == g_rx_tail)
        return 0;
    volatile struct sbshm_net_slot *s = slot(SBSHM_NET_RX_OFF, g_rx_tail);
    unsigned len = s->len;
    for (unsigned i = 0; i < len; ++i)
        p[i] = s->data[i];
    N->rx_tail = ++g_rx_tail;
    return len;
}
static unsigned csum(const uint8_t *p, unsigned n, uint32_t s) { return sn_fold(sn_sum(s, p, n)); }
static unsigned ip_hdr(uint8_t *b, const uint8_t src[4], const uint8_t dst[4], unsigned proto, unsigned l4)
{
    static unsigned id = 100;
    memset(b, 0, 20);
    b[0] = 0x45;
    sn_put16(b + 2, 20 + l4);
    sn_put16(b + 4, ++id);
    sn_put16(b + 6, 0x4000);                        /* DF, as Linux sets it */
    b[8] = 64;
    b[9] = (uint8_t)proto;
    memcpy(b + 12, src, 4);
    memcpy(b + 16, dst, 4);
    sn_put16(b + 10, csum(b, 20, 0));
    return 20 + l4;
}
static unsigned udp_pkt(uint8_t *b, const uint8_t dst[4], unsigned sport, unsigned dport, const void *pl, unsigned n)
{
    uint8_t *u = b + 20;
    sn_put16(u, sport);
    sn_put16(u + 2, dport);
    sn_put16(u + 4, 8 + n);
    sn_put16(u + 6, 0);
    memcpy(u + 8, pl, n);
    uint32_t s = sn_sum(sn_sum(0, sim_ip_guest, 4), dst, 4) + 17U + 8U + n;
    unsigned c = csum(u, 8 + n, s);
    sn_put16(u + 6, c ? c : 0xffffU);
    return ip_hdr(b, sim_ip_guest, dst, 17, 8 + n);
}
static unsigned icmp_echo(uint8_t *b, const uint8_t src[4], const uint8_t dst[4], unsigned id, unsigned n)
{
    uint8_t *ic = b + 20;
    memset(ic, 0, 8);
    ic[0] = 8;
    sn_put16(ic + 4, id);
    sn_put16(ic + 6, 1);
    for (unsigned i = 8; i < n; ++i)
        ic[i] = (uint8_t)(i * 3);
    sn_put16(ic + 2, csum(ic, n, 0));
    return ip_hdr(b, src, dst, 1, n);
}
/* a reply to the guest: to 10.77.0.2 from `from`, IP checksum whole, L4 checksum whole */
static int reply_ok(const uint8_t *r, unsigned n, const uint8_t from[4], unsigned proto)
{
    if (n < 28 || csum(r, 20, 0) != 0 || r[9] != proto || memcmp(r + 16, sim_ip_guest, 4) || memcmp(r + 12, from, 4))
        return 0;
    unsigned l4 = sn_be16(r + 2) - 20U;
    if (proto == 1)
        return csum(r + 20, l4, 0) == 0;
    uint32_t s = sn_sum(0, r + 12, 8) + 17U + l4;
    return csum(r + 20, l4, s) == 0;
}

int main(void)
{
    uint8_t *shm = aligned_alloc(4096, SBG_QEMU_SHM_SIZE);
    memset(shm, 0, SBG_QEMU_SHM_SIZE);
    struct sbg_cfg c = {.shm_base = (uintptr_t)shm, .shm_size = SBG_QEMU_SHM_SIZE, .v2 = 1, .services = SBSHM_SVC_NET};
    sbg_init(&tos, &c, 1);
    HDR->version = SBSHM_VERSION_V3;
    HDR->host_services = SBSHM_SVC_NET;
    N = (volatile struct sbshm_net *)(shm + SBSHM_NET_OFF);
    sbg_net_load();
    st.state = SBG_RUNNING;
    struct sbnat_cfg nc = {.now_ms = fake_ms};
    memcpy(nc.uplink, UPLINK, 4);
    memcpy(nc.dns, UPDNS, 4);
    sbnat_init(&nat, &nc);
    sbg_net_config(1, SBSHM_NET_UPLINK_WIFI, 0, 0, 0, 0);
    (void)sim_net_tick;
    uint8_t b[2048], r[2048];
    unsigned n;
    static const uint8_t one[4] = {1, 1, 1, 1}, nine[4] = {9, 9, 9, 9}, gw[4] = {10, 77, 0, 1};

    printf("1 ping 1.1.1.1 through the NAT (1400-byte packet)\n");
    g_send(b, icmp_echo(b, sim_ip_guest, one, 0x4242, 1380));
    n = g_recv(r);
    CHECK(n == 1400 && reply_ok(r, n, one, 1) && r[20] == 0 && sn_be16(r + 24) == 0x4242 && r[8] == 63 && ns.ttl_out == 63 &&
              !memcmp(r + 28, b + 28, 1372),
          "echo reply: len %u ttl %u (out %u), id %#x", n, r[8], ns.ttl_out, sn_be16(r + 24));

    printf("2 UDP echo 9.9.9.9:7 from port 50000, 1372 bytes\n");
    char pl[1372];
    for (unsigned i = 0; i < sizeof(pl); ++i)
        pl[i] = (char)(i * 11 + 5);
    g_send(b, udp_pkt(b, nine, 50000, 7, pl, sizeof(pl)));
    n = g_recv(r);
    CHECK(n == 1400 && reply_ok(r, n, nine, 17) && sn_be16(r + 20) == 7 && sn_be16(r + 22) == 50000 && r[8] == 63 &&
              !memcmp(r + 28, pl, sizeof(pl)),
          "udp echo: len %u, ports %u -> %u", n, sn_be16(r + 20), sn_be16(r + 22));

    printf("3 DNS to 10.77.0.1:53 (the NAT forwards it to the uplink's server)\n");
    static const uint8_t q[] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p',
                                'l', 'e', 3, 'o', 'r', 'g', 0, 0, 1, 0, 1};
    g_send(b, udp_pkt(b, gw, 41000, 53, q, sizeof(q)));
    n = g_recv(r);
    CHECK(n == 28 + sizeof(q) + 16 && reply_ok(r, n, gw, 17) && sn_be16(r + 20) == 53 && sn_be16(r + 22) == 41000 &&
              r[28] == 0x12 && r[31] == 0x80 && r[n - 4] == 10 && r[n - 1] == 1,
          "dns answer: len %u from %u.%u.%u.%u:%u", n, r[12], r[13], r[14], r[15], sn_be16(r + 20));

    printf("4 ping 10.77.0.1 (local: sbnat_echo_reply)\n");
    g_send(b, icmp_echo(b, sim_ip_guest, gw, 7, 64));
    n = g_recv(r);
    CHECK(n == 84 && reply_ok(r, n, gw, 1) && r[20] == 0, "local echo: len %u", n);

    printf("5 a source other than 10.77.0.2 is dropped by the NAT\n");
    static const uint8_t other[4] = {10, 77, 0, 3};
    unsigned d0 = ns.drop;
    g_send(b, icmp_echo(b, other, one, 9, 64));
    CHECK(ns.drop == d0 + 1 && !g_recv(r) && nat.st.drop_src == 1, "drop_src %u", nat.st.drop_src);

    printf("6 1000 UDP echoes of 1372 bytes in bursts of 64 (sbg_net_poll's bound)\n");
    unsigned got = 0, bad = 0;
    for (unsigned k = 0; k < 1000; k += 64) {
        unsigned m = k + 64 > 1000 ? 1000 - k : 64;
        for (unsigned i = 0; i < m; ++i) {
            pl[0] = (char)(k + i);
            volatile struct sbshm_net_slot *s = slot(SBSHM_NET_TX_OFF, g_tx_head);
            unsigned len = udp_pkt(b, nine, 50001, 7, pl, sizeof(pl));
            for (unsigned j = 0; j < len; ++j)
                s->data[j] = b[j];
            s->len = (uint16_t)len;
            N->tx_head = ++g_tx_head;
        }
        sbg_net_poll();
        fake_ms += 2;
        while ((n = g_recv(r))) {
            if (reply_ok(r, n, nine, 17) && n == 1400)
                got++;
            else
                bad++;
        }
    }
    CHECK(got == 1000 && !bad && N->rx_drop == 0, "got %u bad %u host drops %u", got, bad, N->rx_drop);

    printf("NAT: sent %llu, to guest %llu, local %llu, mapped %u, drops: src %u csum %u hdr %u frag %u; glue: send %u "
           "local %u drop %u to_guest %u not_ours %u\n",
           (unsigned long long)nat.st.sent, (unsigned long long)nat.st.to_guest, (unsigned long long)nat.st.local,
           nat.st.mapped, nat.st.drop_src, nat.st.drop_csum, nat.st.drop_hdr, nat.st.drop_frag, ns.send, ns.local, ns.drop,
           ns.to_guest, ns.not_ours);
    printf("%s\n", fails ? "net-nat-e2e: FAIL" : "net-nat-e2e: PASS");
    free(shm);
    return fails ? 1 : 0;
}
