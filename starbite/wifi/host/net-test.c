/* SPDX-License-Identifier: MIT
 * sbnet's host test (net.c): a simulated LAN answers what sbnet sends. The router 192.168.7.1 is also the DHCP
 * server (offers .50/24, lease 600 s) and the DNS server ("example.test" -> 192.0.2.10, anything else NXDOMAIN),
 * answers ARP and pings. Then a stranger's ARP and ping to us, a renewal, a NAK, and mangled frames under ASan.
 * Every frame sbnet sends is checked: Ethernet source, IPv4 header checksum, UDP and ICMP checksums. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net.h"

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

static const uint8_t me[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t gw_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t stranger_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x09 };
static const uint8_t gw_ip[4] = { 192, 168, 7, 1 }, my_ip[4] = { 192, 168, 7, 50 }, stranger_ip[4] = { 192, 168, 7, 9 };
static const uint8_t far_ip[4] = { 192, 0, 2, 10 };

static uint64_t t_ms = 1000;
static uint32_t rnd = 12345;
static int nak_next, silent;
static unsigned n_discover, n_request, n_arp_req_gw, n_arp_reply, n_ping_to_gw, n_echo_reply_to_stranger, n_dns, n_udp_far;
static unsigned bad_sum, bad_src;

/* frames waiting for sbnet (the LAN's answers): delivered after the tx call returns */
static uint8_t inbox[16][1600];
static uint32_t inbox_len[16];
static unsigned inbox_n;

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static uint16_t csum(const uint8_t *p, uint32_t len, uint32_t s)
{
    for (; len > 1; p += 2, len -= 2)
        s += g16(p);
    if (len)
        s += (uint32_t)p[0] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static void deliver(const uint8_t *f, uint32_t len)
{
    if (inbox_n < 16 && len <= sizeof(inbox[0])) {
        memcpy(inbox[inbox_n], f, len);
        inbox_len[inbox_n++] = len;
    }
}

/* an IPv4 frame from src (mac, ip) to dst with a UDP or ICMP payload */
static void lan_ip(const uint8_t *dmac, const uint8_t *smac, const uint8_t *sip, const uint8_t *dip, uint8_t proto,
                   uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len)
{
    uint8_t f[1600];
    uint8_t *ip = f + 14, *u = ip + 20;
    uint32_t pl = proto == 17 ? 8 + len : len;
    memcpy(f, dmac, 6), memcpy(f + 6, smac, 6), p16(f + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45, p16(ip + 2, (uint16_t)(20 + pl)), ip[8] = 64, ip[9] = proto;
    memcpy(ip + 12, sip, 4), memcpy(ip + 16, dip, 4);
    p16(ip + 10, csum(ip, 20, 0));
    if (proto == 17) {
        uint32_t s = 0;
        p16(u, sport), p16(u + 2, dport), p16(u + 4, (uint16_t)(8 + len)), p16(u + 6, 0);
        memcpy(u + 8, data, len);
        for (unsigned i = 0; i < 8; i += 2)
            s += g16(ip + 12 + i);
        s += 17 + 8 + len;
        p16(u + 6, csum(u, 8 + len, s));
    } else {
        memcpy(u, data, len);
    }
    deliver(f, 14 + 20 + pl);
}

static void dhcp_answer(const uint8_t *req, uint32_t len, int type)
{
    uint8_t m[300];
    uint32_t o = 240;
    memset(m, 0, sizeof(m));
    m[0] = 2, m[1] = 1, m[2] = 6;
    memcpy(m + 4, req + 4, 4);                  /* xid */
    memcpy(m + 16, my_ip, 4);                   /* yiaddr */
    memcpy(m + 28, req + 28, 16);
    m[236] = 99, m[237] = 130, m[238] = 83, m[239] = 99;
    m[o++] = 53, m[o++] = 1, m[o++] = (uint8_t)type;
    m[o++] = 54, m[o++] = 4, memcpy(m + o, gw_ip, 4), o += 4;
    if (type != 6) {
        m[o++] = 0;                             /* a pad on the way */
        m[o++] = 1, m[o++] = 4, m[o++] = 255, m[o++] = 255, m[o++] = 255, m[o++] = 0;
        m[o++] = 3, m[o++] = 4, memcpy(m + o, gw_ip, 4), o += 4;
        m[o++] = 6, m[o++] = 4, memcpy(m + o, gw_ip, 4), o += 4;
        m[o++] = 51, m[o++] = 4, m[o++] = 0, m[o++] = 0, m[o++] = 0x02, m[o++] = 0x58;   /* 600 s */
    }
    m[o++] = 255;
    (void)len;
    lan_ip(me, gw_mac, gw_ip, (req[10] & 0x80) ? (const uint8_t *)"\xff\xff\xff\xff" : my_ip, 17, 67, 68, m, o);
}

static void dns_answer(const uint8_t *q, uint32_t len, uint16_t sport)
{
    uint8_t m[400];
    int known = len > 12 + 14 && !memcmp(q + 12, "\x07" "example" "\x04" "test", 13);
    memcpy(m, q, len);
    m[2] = 0x81, m[3] = known ? 0x80 : 0x83;    /* a response; NXDOMAIN for unknown names */
    p16(m + 6, known ? 2 : 0);
    if (known) {
        uint32_t o = len;
        /* a CNAME-like record first (type 5), then the A record, both with compressed names */
        m[o++] = 0xc0, m[o++] = 12, p16(m + o, 5), p16(m + o + 2, 1), o += 4;
        m[o++] = 0, m[o++] = 0, m[o++] = 1, m[o++] = 0, p16(m + o, 2), o += 2, m[o++] = 0xc0, m[o++] = 12;
        m[o++] = 0xc0, m[o++] = 12, p16(m + o, 1), p16(m + o + 2, 1), o += 4;
        m[o++] = 0, m[o++] = 0, m[o++] = 1, m[o++] = 0, p16(m + o, 4), o += 2;
        memcpy(m + o, far_ip, 4), o += 4;
        len = o;
    }
    lan_ip(me, gw_mac, gw_ip, my_ip, 17, 53, sport, m, len);
}

/* the LAN: what sbnet sent */
static int lan_tx(void *ctx, const uint8_t *f, uint32_t len)
{
    (void)ctx;
    if (len < 60) {
        bad_src++;                              /* every frame at least the minimum length */
        return 0;
    }
    if (memcmp(f + 6, me, 6))
        bad_src++;
    if (silent)
        return 0;
    if (g16(f + 12) == 0x0806) {
        const uint8_t *a = f + 14;
        if (g16(a + 6) == 1 && !memcmp(a + 24, gw_ip, 4)) {
            uint8_t r[60];
            n_arp_req_gw++;
            memset(r, 0, sizeof(r));
            memcpy(r, me, 6), memcpy(r + 6, gw_mac, 6), p16(r + 12, 0x0806);
            p16(r + 14, 1), p16(r + 16, 0x0800), r[18] = 6, r[19] = 4, p16(r + 20, 2);
            memcpy(r + 22, gw_mac, 6), memcpy(r + 28, gw_ip, 4), memcpy(r + 32, me, 6), memcpy(r + 38, my_ip, 4);
            deliver(r, 60);
        } else if (g16(a + 6) == 2) {
            n_arp_reply++;
            CHECK(!memcmp(f, stranger_mac, 6) && !memcmp(a + 14, my_ip, 4) && !memcmp(a + 24, stranger_ip, 4),
                  "the ARP answer to the stranger");
        }
        return 0;
    }
    if (g16(f + 12) != 0x0800)
        return 0;
    {
        const uint8_t *ip = f + 14, *u = ip + 20;
        uint32_t total = g16(ip + 2);
        if (csum(ip, 20, 0) != 0)
            bad_sum++;
        if (ip[9] == 17) {
            uint32_t s = 0, ulen = g16(u + 4);
            for (unsigned i = 0; i < 8; i += 2)
                s += g16(ip + 12 + i);
            s += 17 + ulen;
            if (csum(u, ulen, s) != 0)
                bad_sum++;
            if (g16(u + 2) == 67) {
                const uint8_t *m = u + 8;
                int type = 0;
                for (uint32_t o = 240; o + 2 < ulen - 8; o += 2u + m[o + 1])
                    if (m[o] == 53) {
                        type = m[o + 2];
                        break;
                    }
                if (type == 1) {
                    n_discover++;
                    CHECK(!memcmp(f, "\xff\xff\xff\xff\xff\xff", 6) && !memcmp(ip + 12, "\0\0\0\0", 4), "DISCOVER broadcast from 0.0.0.0");
                    dhcp_answer(m, ulen - 8, 2);
                } else if (type == 3) {
                    n_request++;
                    dhcp_answer(m, ulen - 8, nak_next ? 6 : 5);
                    nak_next = 0;
                }
            } else if (g16(u + 2) == 53) {
                n_dns++;
                CHECK(!memcmp(f, gw_mac, 6), "DNS to the router's address");
                dns_answer(u + 8, g16(u + 4) - 8, g16(u));
            } else if (!memcmp(ip + 16, far_ip, 4)) {
                n_udp_far++;
                CHECK(!memcmp(f, gw_mac, 6), "an off-subnet datagram through the router");
            }
        } else if (ip[9] == 1) {
            if (csum(u, total - 20, 0) != 0)
                bad_sum++;
            if (u[0] == 8 && !memcmp(ip + 16, gw_ip, 4)) {
                uint8_t r[64];
                n_ping_to_gw++;
                memcpy(r, u, total - 20 < sizeof(r) ? total - 20 : sizeof(r));
                r[0] = 0, p16(r + 2, 0), p16(r + 2, csum(r, total - 20, 0));
                lan_ip(me, gw_mac, gw_ip, my_ip, 1, 0, 0, r, total - 20);
            } else if (u[0] == 0 && !memcmp(ip + 16, stranger_ip, 4)) {
                n_echo_reply_to_stranger++;
            }
        }
    }
    return 0;
}

static uint64_t os_now(void *ctx) { (void)ctx; return t_ms; }
static uint32_t os_random(void *ctx) { (void)ctx; rnd = rnd * 1103515245u + 12345u; return rnd >> 3; }
static char logbuf[4096];
static void os_log(void *ctx, const char *l)
{
    (void)ctx;
    if (strlen(logbuf) + strlen(l) + 2 < sizeof(logbuf))
        strcat(strcat(logbuf, l), "\n");
}
static unsigned n_udp_in;
static void os_udp(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    n_udp_in++;
    CHECK(port == 5000 && !memcmp(from, stranger_ip, 4) && from_port == 6000 && len == 5 && !memcmp(d, "hello", 5),
          "the datagram to our port");
}

static void pump(struct sbnet *n, unsigned ms)
{
    for (unsigned k = 0; k < ms / 100 + 1; k++) {
        while (inbox_n) {
            uint8_t f[1600];
            uint32_t l = inbox_len[0];
            memcpy(f, inbox[0], l);
            memmove(inbox, inbox + 1, (inbox_n - 1) * sizeof(inbox[0]));
            memmove(inbox_len, inbox_len + 1, (inbox_n - 1) * sizeof(inbox_len[0]));
            inbox_n--;
            sbnet_rx(n, f, l);
        }
        sbnet_tick(n);
        t_ms += 100;
    }
}

int main(void)
{
    static struct sbnet n;
    struct sbnet_os os = { 0, lan_tx, os_now, os_random, os_log, os_udp, 0 };

    sbnet_init(&n, &os, me);
    sbnet_start(&n);
    pump(&n, 500);
    CHECK(n.state == SBNET_BOUND && !memcmp(n.ip, my_ip, 4) && !memcmp(n.gateway, gw_ip, 4) && !memcmp(n.dns, gw_ip, 4) &&
          n.mask[3] == 0 && n.mask[2] == 255 && n.lease_s == 600, "bound: state %d, lease %u", n.state, n.lease_s);
    CHECK(n_discover == 1 && n_request == 1, "one DISCOVER (%u), one REQUEST (%u)", n_discover, n_request);

    /* a ping to the router: ARP first, the echo held and sent with the answer */
    CHECK(sbnet_ping(&n, gw_ip) == 0, "a ping");
    pump(&n, 300);
    CHECK(n_arp_req_gw == 1 && n_ping_to_gw == 1 && n.ping_replies == 1, "ARP %u, echo %u, replies %u", n_arp_req_gw,
          n_ping_to_gw, n.ping_replies);
    CHECK(sbnet_ping(&n, gw_ip) == 0, "a second ping");
    pump(&n, 200);
    CHECK(n_arp_req_gw == 1 && n.ping_replies == 2, "the second ping from the ARP cache (%u ARP)", n_arp_req_gw);

    /* DNS */
    CHECK(sbnet_dns_query(&n, "example.test") == 0, "a DNS query");
    pump(&n, 300);
    CHECK(n.dns_state == SBNET_DNS_DONE && !memcmp(n.dns_result, far_ip, 4), "DNS answered (state %d)", n.dns_state);
    CHECK(sbnet_dns_query(&n, "nothing.test") == 0, "a query for an unknown name");
    pump(&n, 300);
    CHECK(n.dns_state == SBNET_DNS_FAILED, "NXDOMAIN reported (state %d)", n.dns_state);
    CHECK(sbnet_dns_query(&n, "bad..name") == -1 && sbnet_dns_query(&n, "") == -1, "malformed names refused");
    silent = 1;
    CHECK(sbnet_dns_query(&n, "example.test") == 0, "a query nobody answers");
    pump(&n, 6000);
    silent = 0;
    CHECK(n.dns_state == SBNET_DNS_FAILED, "a DNS timeout after 5 s");

    /* UDP out, off the subnet: through the router */
    CHECK(sbnet_udp_send(&n, far_ip, 9999, 5000, (const uint8_t *)"data", 4) == 0, "UDP out");
    pump(&n, 100);
    CHECK(n_udp_far == 1, "the datagram reached the router (%u)", n_udp_far);

    /* a stranger on the LAN: ARP for us, a ping to us, a datagram to our port */
    {
        uint8_t r[60], ping[40];
        memset(r, 0, sizeof(r));
        memcpy(r, "\xff\xff\xff\xff\xff\xff", 6), memcpy(r + 6, stranger_mac, 6), p16(r + 12, 0x0806);
        p16(r + 14, 1), p16(r + 16, 0x0800), r[18] = 6, r[19] = 4, p16(r + 20, 1);
        memcpy(r + 22, stranger_mac, 6), memcpy(r + 28, stranger_ip, 4), memcpy(r + 38, my_ip, 4);
        deliver(r, 60);
        memset(ping, 'x', sizeof(ping));
        ping[0] = 8, ping[1] = 0, p16(ping + 2, 0), p16(ping + 4, 7), p16(ping + 6, 1), p16(ping + 2, csum(ping, 40, 0));
        lan_ip(me, stranger_mac, stranger_ip, my_ip, 1, 0, 0, ping, 40);
        lan_ip(me, stranger_mac, stranger_ip, my_ip, 17, 6000, 5000, (const uint8_t *)"hello", 5);
        pump(&n, 200);
        CHECK(n_arp_reply == 1 && n_echo_reply_to_stranger == 1 && n_udp_in == 1, "the stranger: ARP %u, echo %u, UDP %u",
              n_arp_reply, n_echo_reply_to_stranger, n_udp_in);
    }

    /* the renewal at half the lease (unicast to the server), then a NAK at the next one */
    t_ms += 300 * 1000;
    pump(&n, 200);
    CHECK(n.state == SBNET_BOUND && n_request == 2 && n_discover == 1, "renewed (state %d, REQUEST %u)", n.state, n_request);
    nak_next = 1;
    t_ms += 300 * 1000;
    pump(&n, 300);
    CHECK(n.state == SBNET_BOUND && n_discover == 2 && n_request == 4, "a NAK restarted it (DISCOVER %u, REQUEST %u)", n_discover,
          n_request);

    /* the lease running out with nobody answering */
    silent = 1;
    t_ms += 700 * 1000;
    pump(&n, 200);
    CHECK(n.state == SBNET_DISCOVERING && n.ip[0] == 0, "the lease ran out: discovering again (state %d)", n.state);
    silent = 0;
    pump(&n, 40000);
    CHECK(n.state == SBNET_BOUND, "bound again (state %d)", n.state);

    CHECK(bad_sum == 0 && bad_src == 0, "every frame out: checksums (%u bad), source and length (%u bad)", bad_sum, bad_src);
    CHECK(!strstr(logbuf, "192.168") && !strstr(logbuf, "example"), "an address or a name in the log");

    /* mangled frames: valid ones with random bytes changed, random lengths, truncations (ASan/UBSan watch) */
    {
        uint8_t base[4][400], f[400];
        uint32_t bl[4];
        unsigned nb = 0;
        /* a DHCP ACK, a DNS answer, an ARP, an ICMP echo as seeds */
        inbox_n = 0;
        {
            uint8_t req[300];
            memset(req, 0, sizeof(req));
            req[4] = (uint8_t)(n.xid >> 24), req[5] = (uint8_t)(n.xid >> 16), req[6] = (uint8_t)(n.xid >> 8), req[7] = (uint8_t)n.xid;
            memcpy(req + 28, me, 6);
            dhcp_answer(req, sizeof(req), 5);
        }
        n.dns_state = SBNET_DNS_WAITING;
        {
            uint8_t q[40] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1 };
            dns_answer(q, 30, n.dns_port);
        }
        for (unsigned i = 0; i < inbox_n && nb < 4; i++) {
            memcpy(base[nb], inbox[i], inbox_len[i] < 400 ? inbox_len[i] : 400);
            bl[nb++] = inbox_len[i] < 400 ? inbox_len[i] : 400;
        }
        inbox_n = 0;
        for (unsigned it = 0; it < 20000; it++) {
            unsigned b = it % nb;
            uint32_t l = bl[b];
            memcpy(f, base[b], l);
            for (unsigned k = 0; k < 1 + it % 6; k++)
                f[os_random(0) % l] = (uint8_t)os_random(0);
            if (it % 3 == 0)
                l = 14 + os_random(0) % (l - 13);
            n.dns_state = SBNET_DNS_WAITING;
            sbnet_rx(&n, f, l);
            inbox_n = 0;
        }
        for (unsigned it = 0; it < 5000; it++) {
            uint32_t l = os_random(0) % 400;
            for (uint32_t k = 0; k < l; k++)
                f[k] = (uint8_t)os_random(0);
            if (l >= 14 && it % 2)
                memcpy(f, me, 6), p16(f + 12, it % 4 == 1 ? 0x0800 : 0x0806);
            sbnet_rx(&n, f, l);
            inbox_n = 0;
        }
        CHECK(1, "mangled frames survived");
    }

    printf("%s: %u checks, %u failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
