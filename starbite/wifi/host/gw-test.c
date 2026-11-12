/* SPDX-License-Identifier: MIT
 * sbgw's host test (gw.c): a guest (sbnet + sbtcp, its DHCP client too) on the gateway's virtual link; two uplinks,
 * Wi-Fi (sbnet on Ethernet, 192.168.7.50/24) and mobile data (raw IP, 100.64.0.7, MTU 1400); a simulated router
 * behind them with a far host (sbnet + sbtcp, 203.0.113.5), a DNS server (203.0.113.53: "example.test"), an echo
 * responder (198.51.100.1) and a host that answers everything with ICMP port unreachable (198.51.100.99).
 * Every packet the gateway emits, either way, is checked: the IPv4 header checksum and the TCP/UDP/ICMP checksum
 * recomputed from scratch (the reference for the incremental updates). Then 200k mutated packets on both sides
 * under ASan/UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net.h"
#include "../tcp.h"
#include "../gw.h"

static unsigned fails, checks;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

static uint64_t t_ms = 1000;
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t r32(void)
{
    rs ^= rs << 13, rs ^= rs >> 7, rs ^= rs << 17;
    return (uint32_t)rs;
}
static double rf(void) { return (r32() >> 8) / 16777216.0; }

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static uint32_t sum(const uint8_t *p, uint32_t len, uint32_t s)
{
    for (; len > 1; p += 2, len -= 2)
        s += g16(p);
    if (len)
        s += (uint32_t)p[0] << 8;
    return s;
}
static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}
static uint16_t ipsum(const uint8_t *ip) { return fold(sum(ip, (uint32_t)(ip[0] & 15) * 4, 0)); }
static void ip_fix(uint8_t *ip)
{
    p16(ip + 10, 0);
    p16(ip + 10, ipsum(ip));
}
static void l4_fix(uint8_t *ip)
{
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, n = g16(ip + 2) - ihl;
    uint8_t *l = ip + ihl;
    if (ip[9] == 6 && n >= 20) {
        p16(l + 16, 0);
        p16(l + 16, fold(sum(l, n, sum(ip + 12, 8, 0) + 6 + n)));
    } else if (ip[9] == 17 && n >= 8) {
        uint16_t s;
        p16(l + 6, 0);
        s = fold(sum(l, n, sum(ip + 12, 8, 0) + 17 + n));
        p16(l + 6, s ? s : 0xffff);
    } else if (ip[9] == 1 && n >= 4) {
        p16(l + 2, 0);
        p16(l + 2, fold(sum(l, n, 0)));
    }
}

/* ---- the reference check of everything the gateway sends ------------------------------------------------------- */

static unsigned ck_checked, ck_bad, ck_ref_differs;
static int fuzzing;
static void verify(const uint8_t *ip, uint32_t len, const char *where)
{
    uint32_t ihl, total, n;
    const uint8_t *l;
    if (fuzzing)
        return;
    ck_checked++;
    if (len < 20 || (ip[0] >> 4) != 4) {
        ck_bad++;
        printf("  %s: not IPv4 (%u bytes)\n", where, len);
        return;
    }
    ihl = (uint32_t)(ip[0] & 15) * 4, total = g16(ip + 2);
    if (ihl < 20 || total > len || total < ihl || ipsum(ip) != 0) {
        ck_bad++;
        printf("  %s: a bad IPv4 header\n", where);
        return;
    }
    if (g16(ip + 6) & 0x3fff)
        return;                                 /* a fragment: the router's reassembly checks it */
    l = ip + ihl, n = total - ihl;
    if (ip[9] == 6 || (ip[9] == 17 && n >= 8 && g16(l + 6))) {
        uint8_t c[1600];
        uint16_t field, ref;
        int off = ip[9] == 6 ? 16 : 6;
        if (n < (uint32_t)(ip[9] == 6 ? 20 : 8)) {
            ck_bad++;
            return;
        }
        if (fold(sum(l, n, sum(ip + 12, 8, 0) + ip[9] + n)) != 0) {
            ck_bad++;
            printf("  %s: a bad %s checksum\n", where, ip[9] == 6 ? "TCP" : "UDP");
            return;
        }
        /* the incremental result against a from-scratch recomputation: equal, or the other zero (0x0000 / 0xffff) */
        memcpy(c, l, n);
        field = g16(c + off);
        p16(c + off, 0);
        ref = fold(sum(c, n, sum(ip + 12, 8, 0) + ip[9] + n));
        if (ip[9] == 17 && !ref)
            ref = 0xffff;
        if (field != ref && !((field == 0xffff && ref == 0) || (field == 0 && ref == 0xffff)))
            ck_ref_differs++;
    } else if (ip[9] == 1 && n >= 4 && fold(sum(l, n, 0)) != 0) {
        ck_bad++;
        printf("  %s: a bad ICMP checksum\n", where);
    }
}

/* ---- the hosts and the links ---------------------------------------------------------------------------------- */

enum { TO_G, TO_GW, TO_ROUTER_ETH, TO_ROUTER_RAW, TO_U, TO_U2, TO_F };
struct host {
    struct sbnet net;
    struct sbtcp tcp;
    int id;
};
static struct host G, U, U2, F;                 /* the guest, Wi-Fi, mobile data, the far host */
static struct sbgw gw;

static const uint8_t g_mac[6] = { 2, 0, 0, 0, 0, 0x15 }, u_mac[6] = { 2, 0, 0, 0, 0, 0x50 },
                     f_mac[6] = { 2, 0, 0, 0, 0, 0xf5 }, r_mac[6] = { 2, 0, 0, 0, 0, 0x01 };
static const uint8_t u_ip[4] = { 192, 168, 7, 50 }, u_gw[4] = { 192, 168, 7, 1 }, mask24[4] = { 255, 255, 255, 0 };
static const uint8_t u2_ip[4] = { 100, 64, 0, 7 }, host32[4] = { 255, 255, 255, 255 }, none[4] = { 0, 0, 0, 0 };
static const uint8_t f_ip[4] = { 203, 0, 113, 5 }, f_gw[4] = { 203, 0, 113, 1 }, dns_srv[4] = { 203, 0, 113, 53 };
static const uint8_t echo_ip[4] = { 198, 51, 100, 1 }, unreach_ip[4] = { 198, 51, 100, 99 }, answer[4] = { 192, 0, 2, 10 };
static const uint8_t gw_ip[4] = { 10, 0, 2, 2 }, dns_ip[4] = { 10, 0, 2, 3 }, guest_ip[4] = { 10, 0, 2, 15 };

static struct {
    double loss;                                /* on the uplinks, both ways */
    unsigned jitter;
} L;

#define QN 8192
static struct fr { uint64_t at; int to; uint32_t len; uint8_t b[1600]; } q[QN];
static unsigned qn, q_full;

static void enqueue(int to, const uint8_t *f, uint32_t len)
{
    if (qn >= QN || len > 1600) {
        q_full++;
        return;
    }
    q[qn].at = t_ms + 1 + ((to >= TO_ROUTER_ETH && L.jitter) ? r32() % L.jitter : 0);
    q[qn].to = to;
    q[qn].len = len;
    memcpy(q[qn].b, f, len);
    qn++;
}

/* captures: frames the gateway sent the guest, packets the router got */
#define CAP 64
static uint8_t gcap[CAP][1600];
static uint32_t gcap_len[CAP];
static unsigned gcap_n;
static unsigned raw_oversize, router_rx, syn_mss_seen, synack_mss_seen;

/* the fuzz corpus: real packets seen at each side */
#define CORP 256
static uint8_t corp_g[CORP][1600], corp_u[CORP][1600];
static uint32_t corp_g_len[CORP], corp_u_len[CORP];
static unsigned corp_g_n, corp_u_n;
static void corpus(uint8_t (*c)[1600], uint32_t *cl, unsigned *cn, const uint8_t *f, uint32_t len)
{
    unsigned k = *cn < CORP ? *cn : r32() % CORP;
    if (len > 1600)
        return;
    memcpy(c[k], f, len), cl[k] = len;
    if (*cn < CORP)
        (*cn)++;
}

static int h_tx(void *ctx, const uint8_t *f, uint32_t len)
{
    struct host *h = ctx;
    if (h == &G) {
        corpus(corp_g, corp_g_len, &corp_g_n, f, len);
        enqueue(TO_GW, f, len);
    } else if (h == &U || h == &U2) {
        const uint8_t *ip = h == &U ? f + 14 : f;
        uint32_t n = h == &U ? len - 14 : len;
        if (h == &U2 && len > 1400)
            raw_oversize++;
        if (h == &U && g16(f + 12) == 0x0800)
            verify(ip, n, "uplink (Wi-Fi)");
        else if (h == &U2)
            verify(ip, n, "uplink (mobile)");
        if (L.loss && rf() < L.loss)
            return 0;
        enqueue(h == &U ? TO_ROUTER_ETH : TO_ROUTER_RAW, f, len);
    } else {
        if (L.loss && rf() < L.loss)
            return 0;
        enqueue(TO_ROUTER_ETH, f, len);         /* the far host's frames go to the router too */
    }
    return 0;
}
static int to_guest(void *ctx, const uint8_t *f, uint32_t len)
{
    (void)ctx;
    if (len >= 34 && g16(f + 12) == 0x0800)
        verify(f + 14, len - 14, "to the guest");
    memcpy(gcap[gcap_n % CAP], f, len < 1600 ? len : 1600);
    gcap_len[gcap_n % CAP] = len;
    gcap_n++;
    enqueue(TO_G, f, len);
    return 0;
}
static uint64_t now_ms(void *ctx) { (void)ctx; return t_ms; }
static uint32_t rnd(void *ctx) { (void)ctx; return r32(); }
static int verbose;
static void logl(void *ctx, const char *l)
{
    (void)ctx;
    if (verbose)
        printf("    %s\n", l);
}
static void tcp_in(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len)
{
    struct host *h = ctx;
    sbtcp_input(&h->tcp, from, seg, len);
}

/* UDP: the far host echoes (port 7) and remembers who asked; the guest remembers what came back */
static uint8_t f_from[4];
static uint16_t f_from_port;
static unsigned f_udp_n, g_udp_n;
static uint16_t g_udp_port, g_udp_from_port;
static uint8_t g_udp_from[4];
static void udp_in(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *d, uint32_t len)
{
    struct host *h = ctx;
    if (h == &F) {
        memcpy(f_from, from, 4), f_from_port = from_port, f_udp_n++;
        if (port == 7)
            sbnet_udp_send(&F.net, from, from_port, 7, d, len);
    } else if (h == &G) {
        g_udp_n++, g_udp_port = port, g_udp_from_port = from_port;
        memcpy(g_udp_from, from, 4);
    }
}

/* the router: answers ARP for anything (proxy ARP), forwards between the uplinks and the far host, and is the DNS
 * server, the echo responder and the unreachable host; it reassembles fragments to check them */
static uint8_t reasm[65536];
static uint32_t reasm_have, reasm_total;
static uint16_t reasm_id;
static unsigned reasm_ok, frags_seen;

static void router_send(const uint8_t *ip, uint32_t len)
{
    uint8_t f[1600];
    if (len + 14 > sizeof(f))
        return;
    if (!memcmp(ip + 16, u2_ip, 4)) {
        enqueue(TO_U2, ip, len);
        return;
    }
    memcpy(f + 14, ip, len);
    memcpy(f + 6, r_mac, 6), p16(f + 12, 0x0800);
    if (!memcmp(ip + 16, u_ip, 4)) {
        memcpy(f, u_mac, 6);
        enqueue(TO_U, f, 14 + len);
    } else if (!memcmp(ip + 16, f_ip, 4)) {
        memcpy(f, f_mac, 6);
        enqueue(TO_F, f, 14 + len);
    }
}

static void router_reply(const uint8_t *req, uint8_t proto, const uint8_t *l4, uint32_t n)
{
    uint8_t p[1600];
    memset(p, 0, 20);
    p[0] = 0x45, p16(p + 2, (uint16_t)(20 + n)), p[8] = 60, p[9] = proto;
    memcpy(p + 12, req + 16, 4), memcpy(p + 16, req + 12, 4);
    memcpy(p + 20, l4, n);
    l4_fix(p);
    ip_fix(p);
    if (!memcmp(p + 16, u_ip, 4) || !memcmp(p + 16, u2_ip, 4))
        corpus(corp_u, corp_u_len, &corp_u_n, p, 20 + n);
    router_send(p, 20 + n);
}

static void router_ip(const uint8_t *ip, uint32_t len)
{
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, total, n;
    const uint8_t *l;
    router_rx++;
    if (len < 20 || ihl < 20 || (total = g16(ip + 2)) > len || total < ihl)
        return;
    l = ip + ihl, n = total - ihl;
    if (g16(ip + 6) & 0x3fff) {                 /* a fragment: reassembled (one datagram at a time) */
        uint32_t off = (uint32_t)(g16(ip + 6) & 0x1fff) * 8;
        frags_seen++;
        if (off == 0 || g16(ip + 4) != reasm_id)
            reasm_id = g16(ip + 4), reasm_have = 0, reasm_total = 0;
        if (off + n <= sizeof(reasm) - 20) {
            memcpy(reasm + 20 + off, l, n);
            reasm_have += n;
            if (off == 0)
                memcpy(reasm, ip, 20);
            if (!(g16(ip + 6) & 0x2000))
                reasm_total = off + n;
            if (reasm_total && reasm_have == reasm_total && reasm[9] == 17) {
                p16(reasm + 2, (uint16_t)(20 + reasm_total)), p16(reasm + 6, 0);
                ip_fix(reasm);
                if (fold(sum(reasm + 20, reasm_total, sum(reasm + 12, 8, 0) + 17 + reasm_total)) == 0)
                    reasm_ok++;
            }
        }
        return;
    }
    if (ip[9] == 6 && n >= 20 && (l[13] & 0x02)) {      /* SYNs: their MSS option */
        for (uint32_t o = 20; o + 4 <= (uint32_t)(l[12] >> 4) * 4;) {
            if (l[o] == 1) {
                o++;
                continue;
            }
            if (l[o] == 2) {
                if (l[13] & 0x10)
                    synack_mss_seen = g16(l + o + 2);
                else
                    syn_mss_seen = g16(l + o + 2);
                break;
            }
            if (l[o] == 0 || l[o + 1] < 2)
                break;
            o += l[o + 1];
        }
    }
    if (!memcmp(ip + 16, dns_srv, 4) && ip[9] == 17 && n >= 20 && g16(l + 2) == 53) {
        uint8_t m[600];
        uint32_t qlen = g16(l + 4) - 8, o;
        int known;
        if (qlen < 12 || qlen > 300)
            return;
        p16(m, g16(l + 2)), p16(m + 2, g16(l));
        memcpy(m + 8, l + 8, qlen);
        known = qlen > 12 + 14 && !memcmp(m + 8 + 12, "\x07" "example" "\x04" "test", 13);
        m[8 + 2] = 0x81, m[8 + 3] = known ? 0x80 : 0x83;
        p16(m + 8 + 6, known ? 1 : 0);
        o = 8 + qlen;
        if (known) {
            m[o++] = 0xc0, m[o++] = 12;
            p16(m + o, 1), p16(m + o + 2, 1), o += 4;
            m[o++] = 0, m[o++] = 0, m[o++] = 0, m[o++] = 60;
            p16(m + o, 4), o += 2;
            memcpy(m + o, answer, 4), o += 4;
        }
        p16(m + 4, (uint16_t)o);
        router_reply(ip, 17, m, o);
        return;
    }
    if (!memcmp(ip + 16, echo_ip, 4) && ip[9] == 1 && n >= 8 && l[0] == 8) {
        uint8_t m[1600];
        memcpy(m, l, n);
        m[0] = 0;
        router_reply(ip, 1, m, n);
        return;
    }
    if (!memcmp(ip + 16, unreach_ip, 4)) {      /* port unreachable, quoting the header and 28 bytes */
        uint8_t m[100];
        uint32_t qn2 = total < ihl + 28 ? total : ihl + 28;
        memset(m, 0, 8);
        m[0] = 3, m[1] = 3;
        memcpy(m + 8, ip, qn2);
        router_reply(ip, 1, m, 8 + qn2);
        return;
    }
    if (!memcmp(ip + 16, u_ip, 4) || !memcmp(ip + 16, u2_ip, 4))
        corpus(corp_u, corp_u_len, &corp_u_n, ip, total);
    router_send(ip, total);
}

static void router_eth(const uint8_t *f, uint32_t len)
{
    if (len >= 42 && g16(f + 12) == 0x0806 && g16(f + 20) == 1) {
        const uint8_t *a = f + 14;
        uint8_t r[60];
        int from_u = !memcmp(a + 14, u_ip, 4);
        if (!memcmp(a + 24, a + 14, 4))
            return;                             /* a gratuitous one */
        memset(r, 0, sizeof(r));
        memcpy(r, a + 8, 6), memcpy(r + 6, r_mac, 6), p16(r + 12, 0x0806);
        p16(r + 14, 1), p16(r + 16, 0x0800), r[18] = 6, r[19] = 4, p16(r + 20, 2);
        memcpy(r + 22, r_mac, 6), memcpy(r + 28, a + 24, 4), memcpy(r + 32, a + 8, 6), memcpy(r + 38, a + 14, 4);
        enqueue(from_u ? TO_U : TO_F, r, 60);
        return;
    }
    if (len >= 34 && g16(f + 12) == 0x0800 && !memcmp(f, r_mac, 6))
        router_ip(f + 14, len - 14);
}

static void deliver(const struct fr *f)
{
    switch (f->to) {
    case TO_G: sbnet_rx(&G.net, f->b, f->len); break;
    case TO_GW: sbgw_guest_rx(&gw, f->b, f->len); break;
    case TO_ROUTER_ETH: router_eth(f->b, f->len); break;
    case TO_ROUTER_RAW: router_ip(f->b, f->len); break;
    case TO_U: sbnet_rx(&U.net, f->b, f->len); break;
    case TO_U2: sbnet_rx_ip(&U2.net, f->b, f->len); break;
    case TO_F: sbnet_rx(&F.net, f->b, f->len); break;
    }
}

static void step(void)
{
    static struct fr f;
    t_ms++;
    for (;;) {
        unsigned k = qn;
        for (unsigned i = 0; i < qn; i++)
            if (q[i].at <= t_ms) {
                k = i;
                break;
            }
        if (k == qn)
            break;
        f = q[k];
        memmove(&q[k], &q[k + 1], (qn - k - 1) * sizeof(q[0]));
        qn--;
        deliver(&f);
    }
    if (t_ms % 10 == 0) {
        sbtcp_tick(&G.tcp), sbtcp_tick(&F.tcp), sbtcp_tick(&U.tcp);
        sbnet_tick(&G.net), sbnet_tick(&F.net), sbnet_tick(&U.net), sbnet_tick(&U2.net);
    }
    if (t_ms % 100 == 0)
        sbgw_tick(&gw);
}
static void run(uint64_t ms)
{
    for (uint64_t i = 0; i < ms; i++)
        step();
}
static int run_until(int (*done)(void), uint64_t limit)
{
    for (uint64_t i = 0; i < limit && !done(); i++)
        step();
    return done();
}

static void host_init(struct host *h, int raw, const uint8_t *mac)
{
    struct sbnet_os os = { 0 };
    os.ctx = h;
    os.tx = h_tx, os.now_ms = now_ms, os.random = rnd, os.log = logl, os.udp = udp_in, os.tcp = tcp_in;
    if (raw)
        sbnet_init_raw(&h->net, &os);
    else
        sbnet_init(&h->net, &os, mac);
    sbtcp_init(&h->tcp, &h->net);
}

static void setup(void)
{
    struct sbgw_os o = { 0 };
    memset(&L, 0, sizeof(L));
    qn = 0;
    host_init(&G, 0, g_mac);
    host_init(&U, 0, u_mac);
    host_init(&U2, 1, 0);
    host_init(&F, 0, f_mac);
    sbnet_static(&U.net, u_ip, mask24, u_gw, dns_srv);
    sbnet_static(&U2.net, u2_ip, host32, none, dns_srv);
    U2.net.mtu = 1400;
    sbnet_static(&F.net, f_ip, mask24, f_gw, none);
    o.to_guest = to_guest, o.now_ms = now_ms, o.random = rnd, o.log = logl;
    sbgw_init(&gw, &o);
    sbgw_set_uplink(&gw, &U.net);
}

/* ---- crafted guest packets ----------------------------------------------------------------------------------- */

static void guest_frame(uint8_t *f, const uint8_t *dst_ip, uint8_t proto, uint32_t l4len, uint8_t ttl, uint16_t frag)
{
    uint8_t *ip = f + 14;
    memcpy(f, gw.mac, 6), memcpy(f + 6, g_mac, 6), p16(f + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45, p16(ip + 2, (uint16_t)(20 + l4len)), p16(ip + 4, (uint16_t)r32()), p16(ip + 6, frag);
    ip[8] = ttl, ip[9] = proto;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, dst_ip, 4);
    l4_fix(ip);
    ip_fix(ip);
}
static void guest_send(const uint8_t *f, uint32_t len)
{
    corpus(corp_g, corp_g_len, &corp_g_n, f, len);
    enqueue(TO_GW, f, len);
}
/* the last frame to the guest that was ICMP of this type */
static const uint8_t *last_icmp(uint8_t type)
{
    for (unsigned k = 0; k < CAP && k < gcap_n; k++) {
        const uint8_t *f = gcap[(gcap_n - 1 - k) % CAP];
        if (g16(f + 12) == 0x0800 && f[14 + 9] == 1 && f[14 + 20] == type)
            return f;
    }
    return 0;
}
static const uint8_t *last_tcp_to_guest(uint8_t flags)
{
    for (unsigned k = 0; k < CAP && k < gcap_n; k++) {
        const uint8_t *f = gcap[(gcap_n - 1 - k) % CAP];
        if (g16(f + 12) == 0x0800 && f[14 + 9] == 6 && (f[14 + 20 + 13] & flags) == flags)
            return f;
    }
    return 0;
}

/* ---- the transfers ---------------------------------------------------------------------------------------------- */

static uint8_t pat(uint32_t i, int dir) { return (uint8_t)(i * 2654435761u >> (dir ? 13 : 7)); }

/* `a` connects to the far host's port and both sides send `size` bytes; checked byte for byte, closed by both */
static uint64_t bulk(const char *name, struct host *a, uint16_t port, uint32_t size, uint64_t limit_ms)
{
    int l = sbtcp_listen(&F.tcp, port), c, s = -1;
    uint32_t sent[2] = { 0, 0 }, got[2] = { 0, 0 }, bad[2] = { 0, 0 };
    int closed[2] = { 0, 0 };
    uint64_t t0 = t_ms;
    uint8_t buf[3000];
    struct sbtcp *tp[2] = { &a->tcp, &F.tcp };
    CHECK(l >= 0, "%s: listen", name);
    c = sbtcp_connect(&a->tcp, f_ip, port);
    CHECK(c >= 0, "%s: connect", name);
    while (c >= 0 && t_ms - t0 < limit_ms) {
        step();
        if (s < 0)
            s = sbtcp_accept(&F.tcp, l);
        int sk[2] = { c, s };
        for (int d = 0; d < 2; d++) {
            if (sk[d] < 0)
                continue;
            while (sent[d] < size) {
                uint32_t n = 1 + r32() % 2999;
                if (n > size - sent[d])
                    n = size - sent[d];
                for (uint32_t k = 0; k < n; k++)
                    buf[k] = pat(sent[d] + k, d);
                int r = sbtcp_send(tp[d], sk[d], buf, n);
                if (r <= 0)
                    break;
                sent[d] += (uint32_t)r;
            }
            for (;;) {
                int r = !closed[d] ? sbtcp_recv(tp[d], sk[d], buf, 1 + r32() % 3000) : 0;
                if (r <= 0)
                    break;
                for (int k = 0; k < r; k++)
                    if (buf[k] != pat(got[d] + (uint32_t)k, 1 - d))
                        bad[d]++;
                got[d] += (uint32_t)r;
            }
            if (sent[d] == size && got[d] == size && !closed[d]) {
                sbtcp_close(tp[d], sk[d]);
                closed[d] = 1;
            }
        }
        if (closed[0] && closed[1] && a->tcp.s[c].state == SBTCP_FREE && F.tcp.s[s].state == SBTCP_FREE)
            break;
    }
    CHECK(got[0] == size && got[1] == size, "%s: received %u and %u of %u (states %d %d)", name, got[0], got[1], size,
          c >= 0 ? a->tcp.s[c].state : -1, s >= 0 ? F.tcp.s[s].state : -1);
    CHECK(!bad[0] && !bad[1], "%s: %u and %u wrong bytes", name, bad[0], bad[1]);
    CHECK(c >= 0 && s >= 0 && a->tcp.s[c].state == SBTCP_FREE && F.tcp.s[s].state == SBTCP_FREE,
          "%s: both slots free at the end", name);
    printf("gw: %s: %u bytes each way in %llu simulated ms; retransmits %u/%u\n", name, size,
           (unsigned long long)(t_ms - t0), a->tcp.retransmits, F.tcp.retransmits);
    sbtcp_close(&F.tcp, l);
    return t_ms - t0;
}

static int g_bound(void) { return G.net.state == SBNET_BOUND; }
static int g_dns_done(void) { return G.net.dns_state != SBNET_DNS_WAITING; }
static int u_dns_done(void) { return U.net.dns_state != SBNET_DNS_WAITING; }
static int c_g;
static int g_est(void) { return sbtcp_state(&G.tcp, c_g) == SBTCP_ESTABLISHED; }
static int g_closed(void) { return sbtcp_state(&G.tcp, c_g) == SBTCP_CLOSED; }
static unsigned want_udp;
static int g_udp_got(void) { return g_udp_n >= want_udp; }

static void dhcp_guest(void)
{
    sbnet_start(&G.net);
    CHECK(run_until(g_bound, 60000), "DHCP: the guest is bound (state %d)", G.net.state);
}

/* ---- the tests ----------------------------------------------------------------------------------------------------- */

static void t_dhcp_arp_ping(void)
{
    uint8_t f[600];
    uint32_t replies;
    setup();
    dhcp_guest();
    CHECK(!memcmp(G.net.ip, guest_ip, 4) && !memcmp(G.net.gateway, gw_ip, 4) && !memcmp(G.net.dns, dns_ip, 4) &&
              !memcmp(G.net.mask, mask24, 4), "DHCP: 10.0.2.15/24, router 10.0.2.2, DNS 10.0.2.3");
    CHECK(G.net.lease_s == 86400, "DHCP: a day's lease (%u)", G.net.lease_s);
    CHECK(gw.dhcp_acks == 1 && gw.guest_known && !memcmp(gw.guest_mac, g_mac, 6), "DHCP: the guest's MAC remembered");
    /* a renewal: T1 at 12 h, unicast to the server, ACKed */
    t_ms += 43200u * 1000;
    run(2000);
    CHECK(G.net.state == SBNET_BOUND && gw.dhcp_acks == 2, "DHCP: renewed at T1 (state %d, acks %u)", G.net.state,
          gw.dhcp_acks);
    /* a request for another address: NAK */
    {
        uint8_t *u, *m;
        memset(f, 0, sizeof(f));
        guest_frame(f, host32, 17, 8 + 300, 64, 0);
        memset(f, 0xff, 6);
        memcpy(f + 14 + 12, none, 4);
        u = f + 34, m = u + 8;
        p16(u, 68), p16(u + 2, 67), p16(u + 4, 308);
        m[0] = 1, m[1] = 1, m[2] = 6, m[4] = 0x12, m[10] = 0x80;
        memcpy(m + 28, g_mac, 6);
        m[236] = 99, m[237] = 130, m[238] = 83, m[239] = 99;
        m[240] = 53, m[241] = 1, m[242] = 3, m[243] = 50, m[244] = 4, m[245] = 10, m[246] = 0, m[247] = 2, m[248] = 99;
        m[249] = 255;
        l4_fix(f + 14), ip_fix(f + 14);
        guest_send(f, 14 + 20 + 308);
        run(5);
        const uint8_t *r = gcap[(gcap_n - 1) % CAP];
        CHECK(g16(r + 34 + 2) == 68 && r[42 + 240 + 2] == 6, "DHCP: a request for 10.0.2.99 is NAKed");
    }
    /* ARP for the guest's own address (a probe): no answer; for the gateway: answered */
    {
        uint8_t a[60];
        unsigned before = gw.arp_replies;
        memset(a, 0, sizeof(a));
        memset(a, 0xff, 6), memcpy(a + 6, g_mac, 6), p16(a + 12, 0x0806);
        p16(a + 14, 1), p16(a + 16, 0x0800), a[18] = 6, a[19] = 4, p16(a + 20, 1);
        memcpy(a + 22, g_mac, 6), memcpy(a + 38, guest_ip, 4);
        guest_send(a, 60);
        run(5);
        CHECK(gw.arp_replies == before, "ARP: a probe for the guest's address is not answered");
        memcpy(a + 38, dns_ip, 4);
        guest_send(a, 60);
        run(5);
        const uint8_t *r = gcap[(gcap_n - 1) % CAP];
        CHECK(gw.arp_replies == before + 1 && g16(r + 12) == 0x0806 && !memcmp(r + 22, gw.mac, 6) &&
                  !memcmp(r + 28, dns_ip, 4), "ARP: 10.0.2.3 answered with the gateway's MAC");
    }
    /* pings: the gateway, then through the NAT (the uplink's own pings still its own) */
    replies = G.net.ping_replies;
    sbnet_ping(&G.net, gw_ip);
    run(50);
    CHECK(G.net.ping_replies == replies + 1 && gw.echo_replies == 1, "ICMP: 10.0.2.2 answers pings");
    /* the uplink's own ping while it still resolves its router: the NAT's packet never displaces sbnet's own one
     * held for ARP (the guest's is dropped then: it retries) */
    sbnet_ping(&U.net, echo_ip);
    sbnet_ping(&G.net, echo_ip);
    run(50);
    CHECK(U.net.ping_replies == 1, "ICMP: the uplink's own ping still answered to it (%u)", U.net.ping_replies);
    sbnet_ping(&G.net, echo_ip);
    run(50);
    CHECK(G.net.ping_replies == replies + 2, "ICMP: a ping through the NAT answered (%u)", G.net.ping_replies);
    sbnet_ping(&G.net, dns_ip);
    run(50);
    CHECK(G.net.ping_replies == replies + 3, "ICMP: 10.0.2.3 answers pings");
}

static void t_dns_udp(void)
{
    static const uint8_t hello[5] = { 104, 101, 108, 108, 111 };
    setup();
    dhcp_guest();
    /* DNS: the guest asks 10.0.2.3; the uplink asks its own server at the same time */
    CHECK(sbnet_dns_query(&G.net, "example.test") == 0, "DNS: the guest's query sent");
    CHECK(sbnet_dns_query(&U.net, "example.test") == 0, "DNS: the uplink's own query sent");
    run_until(g_dns_done, 6000);
    run_until(u_dns_done, 6000);
    CHECK(G.net.dns_state == SBNET_DNS_DONE && !memcmp(G.net.dns_result, answer, 4), "DNS: the guest's answer (%d)",
          G.net.dns_state);
    CHECK(U.net.dns_state == SBNET_DNS_DONE && !memcmp(U.net.dns_result, answer, 4), "DNS: the uplink's own answer");
    CHECK(sbnet_dns_query(&G.net, "nowhere.test") == 0 && run_until(g_dns_done, 6000) &&
              G.net.dns_state == SBNET_DNS_FAILED, "DNS: NXDOMAIN passed back");
    /* UDP: an echo; the far host sees the uplink's address and a port in the NAT's range */
    g_udp_n = 0, want_udp = 1;
    sbnet_udp_send(&G.net, f_ip, 7, 5000, hello, 5);
    run_until(g_udp_got, 1000);
    CHECK(g_udp_n == 1 && g_udp_port == 5000 && g_udp_from_port == 7 && !memcmp(g_udp_from, f_ip, 4),
          "UDP: the echo came back to the guest's port from the far host");
    CHECK(!memcmp(f_from, u_ip, 4) && f_from_port >= SBGW_PORT_LO && f_from_port <= SBGW_PORT_HI,
          "UDP: the far host saw the uplink's address and a port in %u-%u (%u)", SBGW_PORT_LO, SBGW_PORT_HI, f_from_port);
    {
        uint16_t first = f_from_port;
        sbnet_udp_send(&G.net, f_ip, 9, 5000, hello, 5);
        run(20);
        CHECK(f_from_port == first, "UDP: the same guest port keeps its mapping to another far port (RFC 4787)");
        sbnet_udp_send(&G.net, f_ip, 7, 5001, hello, 5);
        run(20);
        CHECK(f_from_port != first, "UDP: another guest port, another mapping");
    }
    /* the uplink's own UDP (an NTP-like port) is not the NAT's */
    {
        unsigned before = f_udp_n;
        sbnet_udp_send(&U.net, f_ip, 7, 50123, hello, 5);
        run(20);
        CHECK(f_udp_n == before + 1 && f_from_port == 50123, "UDP: the uplink's own sockets keep their ports");
    }
    /* a UDP port unreachable from the Internet comes back translated (the quote too) */
    {
        const uint8_t *e;
        sbnet_udp_send(&G.net, unreach_ip, 33434, 6000, hello, 5);
        run(20);
        e = last_icmp(3);
        CHECK(e && e[14 + 21] == 3 && !memcmp(e + 14 + 16, guest_ip, 4) && !memcmp(e + 14 + 28 + 12, guest_ip, 4) &&
                  g16(e + 14 + 28 + 20) == 6000 && !memcmp(e + 14 + 28 + 16, unreach_ip, 4) &&
                  fold(sum(e + 14 + 28, 20, 0)) == 0,
              "ICMP: a port unreachable for a guest's UDP, translated back with its quote");
        CHECK(gw.icmp_in >= 1, "ICMP: errors in counted (%u)", gw.icmp_in);
    }
}

static void t_tcp(void)
{
    setup();
    dhcp_guest();
    bulk("TCP through the NAT on Wi-Fi", &G, 80, 1u << 20, 60000);
    CHECK(syn_mss_seen == 1460, "MSS: Wi-Fi: the guest's SYN keeps 1460 (%u)", syn_mss_seen);
    CHECK(sbgw_conns(&gw) >= 1, "TCP: the closed connection still in the table (%u)", sbgw_conns(&gw));
    run(SBGW_T_TCP_CLOSED + 500);
    CHECK(sbgw_conns(&gw) == 0, "TCP: a closed connection expires after %u ms (%u left)", SBGW_T_TCP_CLOSED,
          sbgw_conns(&gw));
    /* the uplink's own TCP alongside (sbtcp's ports are not the NAT's) */
    bulk("the uplink's own TCP", &U, 81, 100000, 30000);
    /* lossy, reordering uplink */
    L.loss = 0.03, L.jitter = 15;
    bulk("TCP through the NAT, 3% loss and reordering on the uplink", &G, 82, 300000, 600000);
    L.loss = 0, L.jitter = 0;
    /* a refused port: the far host's RST reaches the guest */
    c_g = sbtcp_connect(&G.tcp, f_ip, 9);
    run(100);
    CHECK(sbtcp_state(&G.tcp, c_g) == SBTCP_CLOSED && sbtcp_error(&G.tcp, c_g) == SBTCP_E_REFUSED,
          "TCP: a refused port refuses through the NAT (state %d)", sbtcp_state(&G.tcp, c_g));
    sbtcp_close(&G.tcp, c_g);
    /* a gateway port: refused at once */
    c_g = sbtcp_connect(&G.tcp, gw_ip, 22);
    run(20);
    CHECK(sbtcp_error(&G.tcp, c_g) == SBTCP_E_REFUSED, "TCP: 10.0.2.2:22 refused by the gateway");
    sbtcp_close(&G.tcp, c_g);
}

static void t_raw_mss(void)
{
    setup();
    sbgw_set_uplink(&gw, &U2.net);
    dhcp_guest();
    raw_oversize = 0, syn_mss_seen = synack_mss_seen = 0;
    bulk("TCP through the NAT on mobile data (raw IP, MTU 1400)", &G, 80, 1u << 20, 60000);
    CHECK(syn_mss_seen == 1360, "MSS: the guest's SYN clamped to 1360 on the way out (%u)", syn_mss_seen);
    CHECK(raw_oversize == 0, "raw IP: %u packets above 1400 on the uplink", raw_oversize);
    /* the far host's SYN-ACK said 1460: the guest must have heard 1360 (else its segments would not fit) */
    c_g = sbtcp_connect(&G.tcp, f_ip, 9);
    run(50);
    sbtcp_close(&G.tcp, c_g);
    {
        int l = sbtcp_listen(&F.tcp, 83);
        c_g = sbtcp_connect(&G.tcp, f_ip, 83);
        run_until(g_est, 2000);
        CHECK(G.tcp.s[c_g].mss == 1360, "MSS: the far end's SYN-ACK clamped on the way in (the guest sends %u)",
              G.tcp.s[c_g].mss);
        sbtcp_abort(&G.tcp, c_g);
        run(50);
        sbtcp_close(&F.tcp, l);
    }
    /* DNS and ping over raw IP */
    CHECK(sbnet_dns_query(&G.net, "example.test") == 0 && run_until(g_dns_done, 6000) &&
              G.net.dns_state == SBNET_DNS_DONE, "DNS: relayed over mobile data");
    {
        uint32_t r = G.net.ping_replies;
        sbnet_ping(&G.net, echo_ip);
        run(50);
        CHECK(G.net.ping_replies == r + 1, "ICMP: a ping over mobile data");
    }
    /* UDP larger than 1400 with DF: fragmentation needed (MTU 1400) to the guest; without DF: fragmented */
    {
        static uint8_t f[1600];
        const uint8_t *e;
        unsigned ok = reasm_ok;
        guest_frame(f, f_ip, 17, 1460, 64, 0x4000);
        p16(f + 34, 7000), p16(f + 36, 7), p16(f + 38, 1460);
        for (int i = 8; i < 1460; i++)
            f[34 + i] = (uint8_t)(i * 7);
        l4_fix(f + 14);
        guest_send(f, 14 + 20 + 1460);
        run(10);
        e = last_icmp(3);
        CHECK(e && e[14 + 21] == 4 && g16(e + 14 + 26) == 1400 && !memcmp(e + 14 + 12, gw_ip, 4),
              "ICMP: fragmentation needed, MTU 1400, for a DF packet larger than mobile data's MTU");
        CHECK(gw.too_big == 1, "too_big counted (%u)", gw.too_big);
        p16(f + 14 + 6, 0);
        ip_fix(f + 14);
        guest_send(f, 14 + 20 + 1460);
        run(10);
        CHECK(gw.fragmented == 1 && reasm_ok == ok + 1, "IPv4: a non-DF packet fragmented on the way out, reassembled "
              "with a good UDP checksum (%u, %u fragments)", gw.fragmented, frags_seen);
        /* fragments from the guest: dropped */
        p16(f + 14 + 6, 0x2000);
        ip_fix(f + 14);
        guest_send(f, 14 + 20 + 1460);
        run(10);
        CHECK(gw.frags == 1, "IPv4: the guest's fragments dropped (%u)", gw.frags);
    }
}

static void t_switch(void)
{
    const uint8_t *r;
    uint8_t buf[100];
    int l, s;
    setup();
    dhcp_guest();
    l = sbtcp_listen(&F.tcp, 80);
    c_g = sbtcp_connect(&G.tcp, f_ip, 80);
    CHECK(run_until(g_est, 2000), "switch: connected on Wi-Fi");
    sbtcp_send(&G.tcp, c_g, "abc", 3);
    run(50);
    {
        s = sbtcp_accept(&F.tcp, l);
        CHECK(s >= 0 && sbtcp_recv(&F.tcp, s, buf, sizeof(buf)) == 3, "switch: data through before");
        sbtcp_send(&F.tcp, s, "defg", 4);       /* the RST must follow what the guest has received */
        run(50);
        CHECK(sbtcp_recv(&G.tcp, c_g, buf, sizeof(buf)) == 4, "switch: data back before");
    }
    sbgw_set_uplink(&gw, &U2.net);
    CHECK(run_until(g_closed, 100) && sbtcp_error(&G.tcp, c_g) == SBTCP_E_RESET,
          "switch: the guest's connection reset at once (state %d, error %d)", sbtcp_state(&G.tcp, c_g),
          sbtcp_error(&G.tcp, c_g));
    CHECK(sbgw_conns(&gw) == 0 && gw.flushed >= 1, "switch: the table flushed");
    sbtcp_close(&G.tcp, c_g);
    sbtcp_close(&F.tcp, l);
    sbtcp_abort(&F.tcp, s);                     /* the far end never heard (it would time out) */
    run(3000);
    bulk("TCP after the switch to mobile data", &G, 84, 200000, 60000);
    /* no uplink: a SYN gets network unreachable, an ACK a RST */
    sbgw_set_uplink(&gw, 0);
    c_g = sbtcp_connect(&G.tcp, f_ip, 80);
    run(20);
    r = last_icmp(3);
    CHECK(r && r[14 + 21] == 0 && gw.no_uplink >= 1, "no uplink: a SYN gets ICMP network unreachable");
    sbtcp_abort(&G.tcp, c_g);
    {
        uint8_t f[100];
        unsigned before = gw.rst_sent;
        guest_frame(f, f_ip, 6, 20, 64, 0x4000);
        p16(f + 34, 3333), p16(f + 36, 80), f[34 + 12] = 0x50, f[34 + 13] = 0x10;
        f[34 + 8] = 0x11, f[34 + 9] = 0x22, f[34 + 10] = 0x33, f[34 + 11] = 0x44;
        l4_fix(f + 14);
        guest_send(f, 54);
        run(5);
        r = last_tcp_to_guest(0x04);
        CHECK(gw.rst_sent == before + 1 && r && g32(r + 34 + 4) == 0x11223344,
              "no uplink: an ACK without a connection gets a RST with seq = its ack");
    }
    /* the uplink back; it loses its address: flushed; a new address: flushed */
    sbgw_set_uplink(&gw, &U.net);
    sbgw_set_uplink(&gw, &U.net);               /* the same again: nothing changes */
    l = sbtcp_listen(&F.tcp, 85);
    c_g = sbtcp_connect(&G.tcp, f_ip, 85);
    CHECK(run_until(g_est, 2000), "uplink back: connected");
    {
        static const uint8_t other[4] = { 192, 168, 7, 51 };
        sbnet_static(&U.net, other, mask24, u_gw, dns_srv);
        run(200);
        CHECK(g_closed() && sbgw_conns(&gw) == 0, "a new uplink address: the guest's connection reset");
    }
    sbtcp_close(&G.tcp, c_g);
    s = sbtcp_accept(&F.tcp, l);
    if (s >= 0)
        sbtcp_abort(&F.tcp, s);
    sbtcp_close(&F.tcp, l);
    /* sbnet_init on the uplink (the link came back) clears its hook: the next tick puts it back */
    host_init(&U, 0, u_mac);
    sbnet_static(&U.net, u_ip, mask24, u_gw, dns_srv);
    run(200);
    CHECK(U.net.ip_hook != 0 && U.net.hook_ctx == &gw, "the hook back after sbnet_init");
    run(3000);
    /* the uplink goes down (sbnet_stop: Wi-Fi lost): the next tick resets the guest's connections */
    l = sbtcp_listen(&F.tcp, 87);
    c_g = sbtcp_connect(&G.tcp, f_ip, 87);
    CHECK(run_until(g_est, 2000), "uplink down: connected first");
    sbnet_stop(&U.net);
    run_until(g_closed, 200);
    run(200);
    CHECK(g_closed() && sbtcp_error(&G.tcp, c_g) == SBTCP_E_RESET && sbgw_conns(&gw) == 0,
          "uplink down: the guest's connection reset within a tick (state %d err %d conns %u)", sbtcp_state(&G.tcp, c_g), sbtcp_error(&G.tcp, c_g), sbgw_conns(&gw));
    sbtcp_close(&G.tcp, c_g);
    s = sbtcp_accept(&F.tcp, l);
    if (s >= 0)
        sbtcp_abort(&F.tcp, s);
    sbtcp_close(&F.tcp, l);
    sbnet_static(&U.net, u_ip, mask24, u_gw, dns_srv);
    run(3000);
    bulk("TCP after the uplink came back", &G, 86, 100000, 60000);
}

static void t_table(void)
{
    static const uint8_t x[3] = { 120, 121, 122 };
    setup();
    dhcp_guest();
    /* a TCP connection first (it will be the oldest: its eviction must reset it) */
    {
        int l = sbtcp_listen(&F.tcp, 80);
        c_g = sbtcp_connect(&G.tcp, f_ip, 80);
        run_until(g_est, 2000);
        (void)l;
    }
    run(10);
    g_udp_n = 0;
    for (unsigned i = 0; i < 300; i++) {
        sbnet_udp_send(&G.net, f_ip, 7, (uint16_t)(10000 + i), x, 3);
        run(3);
    }
    run(50);
    CHECK(sbgw_conns(&gw) == SBGW_CONNS, "table: full at %u (%u)", SBGW_CONNS, sbgw_conns(&gw));
    CHECK(gw.evicted == 300 + 1 - SBGW_CONNS, "table: %u evicted, the oldest first (want %u)", gw.evicted,
          300 + 1 - SBGW_CONNS);
    CHECK(g_closed() && sbtcp_error(&G.tcp, c_g) == SBTCP_E_RESET, "table: the evicted TCP connection reset (state %d)",
          sbtcp_state(&G.tcp, c_g));
    CHECK(g_udp_n == 300, "table: every echo came back (%u)", g_udp_n);
    g_udp_n = 0;
    sbnet_udp_send(&G.net, f_ip, 7, 10299, x, 3);
    run(20);
    CHECK(g_udp_n == 1, "table: the newest flow still mapped");
    /* idle: UDP expires (180 s once answered) */
    for (unsigned i = 0; i < 200; i++)
        run(1000);
    CHECK(sbgw_conns(&gw) == 0, "table: UDP flows expired (%u left)", sbgw_conns(&gw));
}

static void t_crafted(void)
{
    uint8_t f[200];
    const uint8_t *r;
    setup();
    dhcp_guest();
    /* TTL 1: time exceeded from 10.0.2.2 */
    guest_frame(f, f_ip, 17, 12, 1, 0x4000);
    p16(f + 34, 1234), p16(f + 36, 33434), p16(f + 38, 12);
    l4_fix(f + 14);
    guest_send(f, 46);
    run(5);
    r = last_icmp(11);
    CHECK(r && !memcmp(r + 14 + 12, gw_ip, 4), "TTL 1: time exceeded from the gateway");
    /* a SYN with its MSS at an odd offset (NOP first) and above the clamp: the checksum must follow */
    sbgw_set_uplink(&gw, &U2.net);
    run(5);
    syn_mss_seen = 0;
    {
        unsigned bad = ck_bad, differs = ck_ref_differs;
        guest_frame(f, f_ip, 6, 28, 64, 0x4000);
        p16(f + 34, 4321), p16(f + 36, 9), f[34 + 4] = 1;
        f[34 + 12] = 0x70, f[34 + 13] = 0x02;
        f[54] = 1, f[55] = 2, f[56] = 4, p16(f + 57, 65000), f[59] = 1, f[60] = 1, f[61] = 0;
        l4_fix(f + 14);
        guest_send(f, 62);
        run(5);
        CHECK(syn_mss_seen == 1360 && ck_bad == bad && ck_ref_differs == differs,
              "MSS: an odd-offset MSS option clamped with a correct checksum (%u)", syn_mss_seen);
    }
    /* random-sized UDP and TCP packets with random ports, odd lengths: the reference recomputation agrees */
    {
        unsigned checked = ck_checked;
        static uint8_t b[1600];
        for (unsigned i = 0; i < 2000; i++) {
            uint32_t n = 8 + r32() % 1300;
            uint8_t proto = (r32() & 1) ? 17 : 6;
            if (proto == 6 && n < 20)
                n = 20;
            guest_frame(b, f_ip, proto, n, 64, 0x4000);
            for (uint32_t k = 0; k < n; k++)
                b[34 + k] = (uint8_t)r32();
            if (proto == 6) {
                b[34 + 12] = 0x50, b[34 + 13] = (r32() & 1) ? 0x02 : 0x18;
            } else {
                p16(b + 38, (uint16_t)n);
            }
            l4_fix(b + 14);
            guest_send(b, 14 + 20 + n);
            run(2);
        }
        run(20);
        CHECK(ck_checked > checked + 2000 && ck_bad == 0 && ck_ref_differs == 0,
              "checksums: %u packets, %u bad, %u differ from the reference", ck_checked - checked, ck_bad,
              ck_ref_differs);
    }
}

/* 200k packets from both sides: real ones (the corpus) mutated, and random bytes; nothing may crash */
static void t_fuzz(void)
{
    static uint8_t b[1600];
    unsigned n_guest = 0, n_up = 0;
    uint32_t before_bad = ck_bad;
    fuzzing = 1;                                /* the NAT passes bad transport checksums on as they are: not checked */
    for (unsigned i = 0; i < 200000; i++) {
        int side = i & 1;
        uint32_t len;
        unsigned cn = side ? corp_u_n : corp_g_n;
        if (cn && r32() % 8) {
            unsigned k = r32() % cn;
            len = side ? corp_u_len[k] : corp_g_len[k];
            memcpy(b, side ? corp_u[k] : corp_g[k], len);
            for (unsigned m = 1 + r32() % 4; m; m--)
                b[r32() % len] ^= (uint8_t)(1 + r32() % 255);
            if (r32() % 4 == 0)
                len = r32() % (len + 1);
            if (r32() % 4 == 0 && len > 600)   /* longer than it says */
                len += r32() % 200;
        } else {
            len = r32() % 1600;
            for (uint32_t k = 0; k < len; k++)
                b[k] = (uint8_t)r32();
            if (!side && len > 14 && (r32() & 1))
                memcpy(b, gw.mac, 6), b[12] = 8, b[13] = 0, b[14] = 0x45;
            else if (side && len > 0)
                b[0] = 0x45;
        }
        if (len > 1600)
            len = 1600;
        /* often fix the checksums up, so the mutations get past the first checks */
        {
            uint8_t *ip = side ? b : b + 14;
            uint32_t avail = side ? len : (len > 14 ? len - 14 : 0);
            if (avail >= 20 && (ip[0] >> 4) == 4 && (ip[0] & 15) >= 5 && (uint32_t)(ip[0] & 15) * 4 <= avail &&
                (r32() % 4)) {
                if (side && r32() % 2)
                    memcpy(ip + 16, (r32() & 1) ? u_ip : u2_ip, 4);
                if (g16(ip + 2) <= avail && g16(ip + 2) >= (uint32_t)(ip[0] & 15) * 4 && (r32() & 1))
                    l4_fix(ip);
                ip_fix(ip);
            }
        }
        if (side) {
            n_up++;
            if (r32() & 1)
                sbnet_rx_ip(&U2.net, b, len);
            else {
                static uint8_t e[1614];
                memcpy(e, u_mac, 6), memcpy(e + 6, r_mac, 6), e[12] = 8, e[13] = 0;
                memcpy(e + 14, b, len);
                sbnet_rx(&U.net, e, 14 + len);
            }
        } else {
            n_guest++;
            sbgw_guest_rx(&gw, b, len);
        }
        if (i % 64 == 0) {
            qn = 0;                             /* what the answers would have caused: dropped */
            t_ms += 50;
            sbgw_tick(&gw);
            if (i % 50000 == 0)
                sbgw_set_uplink(&gw, (i / 50000) & 1 ? &U2.net : &U.net);
        }
    }
    qn = 0;
    printf("gw: fuzz: %u frames from the guest, %u packets from the uplinks (corpus %u + %u); %u conns, dropped %u\n",
           n_guest, n_up, corp_g_n, corp_u_n, sbgw_conns(&gw), gw.dropped);
    fuzzing = 0;
    CHECK(ck_bad == before_bad, "fuzz: the checks before it untouched");
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    t_dhcp_arp_ping();
    t_dns_udp();
    t_tcp();
    t_raw_mss();
    t_switch();
    t_table();
    t_crafted();
    {
        unsigned bad = ck_bad;
        CHECK(bad == 0 && ck_ref_differs == 0, "checksums: %u packets from the gateway checked: %u bad, %u differ from "
              "the reference", ck_checked, bad, ck_ref_differs);
    }
    t_fuzz();
    /* after the fuzz: the gateway still works */
    setup();
    dhcp_guest();
    bulk("TCP after the fuzz", &G, 80, 100000, 60000);
    CHECK(q_full == 0, "the simulated links never overflowed (%u)", q_full);
    printf("gw: %u checks, %u failed; %u packets checksum-verified\n", checks, fails, ck_checked);
    return fails ? 1 : 0;
}
