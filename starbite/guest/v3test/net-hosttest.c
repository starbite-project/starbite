/* SPDX-License-Identifier: GPL-2.0-only
 * net-hosttest: sbnet's host side without QEMU: host/sbg_net.c (inside sbghost.c) with sbsim's NAT stand-in
 * (sbsim/sim_net.c) on a plain buffer as the shared region; this program plays the guest's kernel on the rings.
 *   cc -Wall -Wextra -O2 -I.. v3test/net-hosttest.c -o /tmp/net-hosttest && /tmp/net-hosttest
 * Checks: the layout and the default config, sbg_net_config's seqlock, ICMP / UDP echo / DNS answers (checksums
 * verified), the per-poll bound, a full rx ring, link 0, a broken tx ring. Last line "net-hosttest: PASS|FAIL". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../host/sbghost.c"

static uint64_t fake_ms;
static uint64_t now_ms(void) { return fake_ms; }
static void puts_(const char *s) { fputs(s, stdout); }
static void putu(uint64_t v) { printf("%llu", (unsigned long long)v); }
#include "../sbsim/sim_net.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf(" (%s:%d)\n", __FILE__, __LINE__); } } while (0)

static void host_log(const char *l) { printf("  host| %s\n", l); }
static uint64_t ms_cb(void) { return fake_ms; }
static const struct sbg_os tos = {.ms = ms_cb, .log = host_log, .net_rx = sim_net_rx};

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
static unsigned g_pending(void) { return N->rx_head - g_rx_tail; }

static unsigned csum(const uint8_t *p, unsigned n, uint32_t s) { return sn_fold(sn_sum(s, p, n)); }
static unsigned ip_hdr(uint8_t *b, const uint8_t dst[4], unsigned proto, unsigned l4)
{
    memset(b, 0, 20);
    b[0] = 0x45;
    sn_put16(b + 2, 20 + l4);
    b[8] = 64;
    b[9] = (uint8_t)proto;
    memcpy(b + 12, sim_ip_guest, 4);
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
    return ip_hdr(b, dst, 17, 8 + n);
}
static int udp_ok(const uint8_t *b, unsigned len)   /* IP header and UDP checksums of a reply */
{
    if (len < 28 || csum(b, 20, 0) != 0)
        return 0;
    unsigned ul = sn_be16(b + 24);
    uint32_t s = sn_sum(0, b + 12, 8) + 17 + ul;
    return csum(b + 20, ul, s) == 0;
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
    uint8_t b[2048], r[2048];
    unsigned n;

    printf("1 layout + default config\n");
    sbg_net_load();
    st.state = SBG_RUNNING;
    CHECK(sizeof(struct sbshm_net) == 320, "struct size %zu", sizeof(struct sbshm_net));
    CHECK(N->cfg_seq == 2 && N->link == 0 && N->mtu == 1400 && N->prefix == 30, "seq %u link %u mtu %u prefix %u",
          N->cfg_seq, N->link, N->mtu, N->prefix);
    CHECK(N->guest_ip[0] == 10 && N->guest_ip[3] == 2 && N->gw_ip[3] == 1 && N->dns_ip[3] == 1, "default IPs");
    CHECK(sbg_net_send((const uint8_t *)"\x45", 1) == 0 && N->rx_drop == 1, "send without link must drop");

    printf("2 sim_net configures link 1 after 300 ms\n");
    fake_ms = 1000;
    sim_net_tick(fake_ms);
    fake_ms = 1300;
    sim_net_tick(fake_ms);
    CHECK(N->cfg_seq == 4 && N->link == 1 && N->uplink == SBSHM_NET_UPLINK_WIFI, "seq %u link %u uplink %u",
          N->cfg_seq, N->link, N->uplink);

    printf("3 ICMP echo to 1.1.1.1\n");
    static const uint8_t one[4] = {1, 1, 1, 1}, gw[4] = {10, 77, 0, 1};
    uint8_t *ic = b + 20;
    memset(ic, 0, 64);
    ic[0] = 8;
    sn_put16(ic + 4, 0x1234);
    sn_put16(ic + 6, 7);
    for (int i = 8; i < 64; ++i)
        ic[i] = (uint8_t)i;
    sn_put16(ic + 2, csum(ic, 64, 0));
    g_send(b, ip_hdr(b, one, 1, 64));
    sbg_net_poll();
    n = g_recv(r);
    CHECK(n == 84 && r[9] == 1 && r[20] == 0 && !memcmp(r + 12, one, 4) && !memcmp(r + 16, sim_ip_guest, 4) &&
              csum(r, 20, 0) == 0 && csum(r + 20, 64, 0) == 0 && !memcmp(r + 24, b + 24, 60),
          "echo reply (len %u type %u)", n, r[20]);

    printf("4 UDP echo to 9.9.9.9:7\n");
    static const uint8_t nine[4] = {9, 9, 9, 9};
    char pl[1372];
    for (unsigned i = 0; i < sizeof(pl); ++i)
        pl[i] = (char)(i * 7);
    g_send(b, udp_pkt(b, nine, 40000, 7, pl, sizeof(pl)));
    sbg_net_poll();
    n = g_recv(r);
    CHECK(n == 1400 && udp_ok(r, n) && sn_be16(r + 20) == 7 && sn_be16(r + 22) == 40000 && !memcmp(r + 28, pl, sizeof(pl)),
          "udp echo (len %u)", n);

    printf("5 DNS A and AAAA at 10.77.0.1:53\n");
    static const uint8_t q[] = {0xab, 0xcd, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c',
                                'o', 'm', 0, 0, 1, 0, 1};
    g_send(b, udp_pkt(b, gw, 33333, 53, q, sizeof(q)));
    sbg_net_poll();
    n = g_recv(r);
    const uint8_t *d = r + 28;
    CHECK(n == 28 + sizeof(q) + 16 && udp_ok(r, n) && d[0] == 0xab && d[2] == 0x81 && d[3] == 0x80 && sn_be16(d + 6) == 1 &&
              d[n - 28 - 4] == 10 && d[n - 28 - 1] == 1 && !memcmp(r + 12, gw, 4),
          "dns A answer (len %u)", n);
    uint8_t q6[sizeof(q)];
    memcpy(q6, q, sizeof(q));
    q6[sizeof(q) - 3] = 28;
    g_send(b, udp_pkt(b, gw, 33334, 53, q6, sizeof(q6)));
    sbg_net_poll();
    n = g_recv(r);
    CHECK(n == 28 + sizeof(q) && udp_ok(r, n) && sn_be16(r + 28 + 6) == 0, "dns AAAA: no answer (len %u)", n);

    printf("6 bound per poll: 100 echoes queued\n");
    for (int i = 0; i < 100; ++i)
        g_send(b, udp_pkt(b, nine, 40001, 7, pl, 100));
    sbg_net_poll();
    CHECK(g_pending() == NET_PER_POLL && N->tx_tail == g_tx_head - (100 - NET_PER_POLL), "first poll: %u", g_pending());
    sbg_net_poll();
    CHECK(g_pending() == 100 && N->tx_tail == g_tx_head, "second poll: %u", g_pending());
    while (g_recv(r))
        ;

    printf("7 the rx ring full: 300 sends, the guest reads nothing\n");
    uint32_t drop0 = N->rx_drop;
    unsigned ok = 0;
    for (int i = 0; i < 300; ++i)
        ok += (unsigned)sbg_net_send(b, 100);
    CHECK(ok == SBSHM_NET_SLOTS && N->rx_drop - drop0 == 300 - SBSHM_NET_SLOTS, "accepted %u, dropped %u", ok,
          N->rx_drop - drop0);
    while (g_recv(r))
        ;
    CHECK(sbg_net_send(b, 100) == 1, "room again");
    g_recv(r);

    printf("8 link flap via the control port (link 0, then 1 with the other uplink)\n");
    static const char flap[] = "flap 2000";
    g_send(b, udp_pkt(b, gw, 40002, 5, flap, sizeof(flap) - 1));
    g_send(b, udp_pkt(b, nine, 40003, 7, pl, 10));     /* after the flap request: dropped (no link) */
    drop0 = N->rx_drop;
    sbg_net_poll();
    CHECK(N->link == 0 && g_pending() == 0 && N->rx_drop == drop0 + 1, "link %u pending %u drops +%u", N->link,
          g_pending(), N->rx_drop - drop0);
    fake_ms += 2500;
    sim_net_tick(fake_ms);
    CHECK(N->link == 1 && N->uplink == SBSHM_NET_UPLINK_CELL && !(N->cfg_seq & 1), "link %u uplink %u seq %u", N->link,
          N->uplink, N->cfg_seq);

    printf("9 bad lengths are dropped, a broken tx ring stops the service\n");
    drop0 = N->rx_drop;
    volatile struct sbshm_net_slot *s = slot(SBSHM_NET_TX_OFF, g_tx_head);
    s->len = 1401;
    N->tx_head = ++g_tx_head;
    s = slot(SBSHM_NET_TX_OFF, g_tx_head);
    s->len = 0;
    N->tx_head = ++g_tx_head;
    sbg_net_poll();
    CHECK(N->rx_drop == drop0 + 2 && N->tx_tail == g_tx_head, "bad lengths: drops +%u", N->rx_drop - drop0);
    N->tx_head = g_tx_head + 1000;
    sbg_net_poll();
    CHECK(net_bad == 1 && N->tx_tail == g_tx_head, "broken ring");
    sbg_net_send(b, 100);

    printf("10 config while the guest is off: the region only (no doorbell), no sends\n");
    st.state = SBG_OFF;
    uint32_t seq0 = N->cfg_seq;
    sbg_net_config(1, SBSHM_NET_UPLINK_WIFI, 0, 0, 0, 0);
    CHECK(N->cfg_seq == seq0 + 2, "while off the region is still written (harmless), seq %u", N->cfg_seq);
    CHECK(sbg_net_send(b, 100) == 0, "no send while the guest is off");

    printf("%s\n", fails ? "net-hosttest: FAIL" : "net-hosttest: PASS");
    free(shm);
    return fails ? 1 : 0;
}
