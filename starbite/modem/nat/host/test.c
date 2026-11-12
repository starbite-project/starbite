/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbnat host test. Every packet is built here with checksums computed from scratch by an independent
 * implementation (64-bit byte sums, no incremental updates), and every packet sbnat rewrites is checked the
 * same way. Fixed-seed PRNG: reproducible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbnat.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKEQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { fails++; printf("FAIL %s:%d: %s = %lld (0x%llx), want %lld (0x%llx)\n", __FILE__, __LINE__, \
        #a, _a, (unsigned long long)_a, _b, (unsigned long long)_b); } } while (0)

static const uint8_t GUEST[4] = { 10, 77, 0, 2 }, GW[4] = { 10, 77, 0, 1 };
static const uint8_t UP[4] = { 192, 168, 1, 23 }, DNS[4] = { 192, 168, 1, 1 };
static const uint8_t UP2[4] = { 100, 64, 7, 9 }, DNS2[4] = { 100, 64, 0, 53 };
static const uint8_t WEB[4] = { 93, 184, 216, 34 }, WEB2[4] = { 151, 101, 1, 69 }, RTR[4] = { 172, 16, 0, 1 };

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

/* ---- independent packet code ---- */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] * 256u + p[1]); }
static void p16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ones-complement sum of bytes, start offset parity relative to the 16-bit grid of the data */
static uint16_t refsum(const uint8_t *p, size_t len, uint64_t acc)
{
    size_t i;
    for (i = 0; i < len; i++)
        acc += (i & 1) ? p[i] : (uint64_t)p[i] << 8;
    while (acc >> 16)
        acc = (acc & 0xffff) + (acc >> 16);
    return (uint16_t)acc;
}

static unsigned hlen(const uint8_t *ip) { return (ip[0] & 15u) * 4u; }
static unsigned tlen(const uint8_t *ip) { return g16(ip + 2); }
static uint8_t *l4p(uint8_t *ip) { return ip + hlen(ip); }

static uint64_t pseudo(const uint8_t *ip, unsigned l4len)
{
    return (uint64_t)g16(ip + 12) + g16(ip + 14) + g16(ip + 16) + g16(ip + 18) + ip[9] + l4len;
}

/* checks the IP header and the L4 checksum over 'avail' bytes of a complete datagram */
static int ip_hdr_ok(const uint8_t *ip) { return refsum(ip, hlen(ip), 0) == 0xffff; }

static int l4_ok(const uint8_t *ip, unsigned l4len)
{
    const uint8_t *l4 = ip + hlen(ip);
    if (ip[9] == 17) {
        if (g16(l4 + 6) == 0)
            return 1;
        return refsum(l4, l4len, pseudo(ip, l4len)) == 0xffff;
    }
    if (ip[9] == 6)
        return refsum(l4, l4len, pseudo(ip, l4len)) == 0xffff;
    if (ip[9] == 1)
        return refsum(l4, l4len, 0) == 0xffff;
    return 1;
}

static int pkt_ok(const uint8_t *ip) { return ip_hdr_ok(ip) && l4_ok(ip, tlen(ip) - hlen(ip)); }

static void finish(uint8_t *ip, int udp_nocsum)
{
    uint8_t *l4 = ip + hlen(ip);
    unsigned l4len = tlen(ip) - hlen(ip);
    p16(ip + 10, 0);
    p16(ip + 10, (uint16_t)~refsum(ip, hlen(ip), 0));
    if (ip[9] == 6) {
        p16(l4 + 16, 0);
        p16(l4 + 16, (uint16_t)~refsum(l4, l4len, pseudo(ip, l4len)));
    } else if (ip[9] == 17) {
        uint16_t c;
        p16(l4 + 6, 0);
        if (udp_nocsum)
            return;
        c = (uint16_t)~refsum(l4, l4len, pseudo(ip, l4len));
        p16(l4 + 6, c ? c : 0xffff);
    } else if (ip[9] == 1) {
        p16(l4 + 2, 0);
        p16(l4 + 2, (uint16_t)~refsum(l4, l4len, 0));
    }
}

/* the IP header checksum alone */
static void ipfix(uint8_t *ip)
{
    p16(ip + 10, 0);
    p16(ip + 10, (uint16_t)~refsum(ip, hlen(ip), 0));
}

/* IP header, optional options (opt bytes, multiple of 4) */
static uint8_t *iphdr(uint8_t *b, const uint8_t *src, const uint8_t *dst, uint8_t proto, unsigned l4len, unsigned opt)
{
    unsigned i;
    memset(b, 0, 20 + opt + l4len);
    b[0] = (uint8_t)(0x40 | ((20 + opt) / 4));
    p16(b + 2, 20 + opt + l4len);
    p16(b + 4, rnd() & 0xffff);
    p16(b + 6, 0x4000);     /* DF */
    b[8] = 64;
    b[9] = proto;
    memcpy(b + 12, src, 4);
    memcpy(b + 16, dst, 4);
    for (i = 0; i < opt; i++)
        b[20 + i] = 1;      /* NOPs */
    return b + 20 + opt;
}

static unsigned mk_tcp(uint8_t *b, const uint8_t *src, unsigned sport, const uint8_t *dst, unsigned dport,
                       uint8_t flags, unsigned paylen)
{
    uint8_t *t = iphdr(b, src, dst, 6, 20 + paylen, 0);
    unsigned i;
    p16(t, sport);
    p16(t + 2, dport);
    p16(t + 4, rnd()); p16(t + 6, rnd());
    p16(t + 8, rnd()); p16(t + 10, rnd());
    t[12] = 0x50;
    t[13] = flags;
    p16(t + 14, 65535);
    for (i = 0; i < paylen; i++)
        t[20 + i] = (uint8_t)rnd();
    finish(b, 0);
    return tlen(b);
}

static unsigned mk_udp(uint8_t *b, const uint8_t *src, unsigned sport, const uint8_t *dst, unsigned dport,
                       unsigned paylen, int nocsum)
{
    uint8_t *u = iphdr(b, src, dst, 17, 8 + paylen, 0);
    unsigned i;
    p16(u, sport);
    p16(u + 2, dport);
    p16(u + 4, 8 + paylen);
    for (i = 0; i < paylen; i++)
        u[8 + i] = (uint8_t)rnd();
    finish(b, nocsum);
    return tlen(b);
}

static unsigned mk_echo(uint8_t *b, const uint8_t *src, const uint8_t *dst, uint8_t type, unsigned id, unsigned seq,
                        unsigned paylen)
{
    uint8_t *c = iphdr(b, src, dst, 1, 8 + paylen, 0);
    unsigned i;
    c[0] = type;
    p16(c + 4, id);
    p16(c + 6, seq);
    for (i = 0; i < paylen; i++)
        c[8 + i] = (uint8_t)rnd();
    finish(b, 0);
    return tlen(b);
}

/* ICMP error from 'src' to 'dst' quoting the first 'quote' bytes of the packet 'orig' */
static unsigned mk_err(uint8_t *b, const uint8_t *src, const uint8_t *dst, uint8_t type, uint8_t code, unsigned mtu,
                       const uint8_t *orig, unsigned quote)
{
    uint8_t *c = iphdr(b, src, dst, 1, 8 + quote, 0);
    c[0] = type;
    c[1] = code;
    p16(c + 6, mtu);
    memcpy(c + 8, orig, quote);
    b[8] = 250;
    finish(b, 0);
    return tlen(b);
}

/* swap a packet into its reply (ports/addresses), new flags/payload, full checksums */
static unsigned reply_tcp(uint8_t *b, const uint8_t *out, uint8_t flags, unsigned paylen)
{
    uint8_t s[4], d[4];
    const uint8_t *t = out + hlen(out);
    unsigned sp = g16(t), dp = g16(t + 2);
    memcpy(s, out + 12, 4);
    memcpy(d, out + 16, 4);
    return mk_tcp(b, d, dp, s, sp, flags, paylen);   /* b may be out */
}

static unsigned reply_udp(uint8_t *b, const uint8_t *out, unsigned paylen, int nocsum)
{
    uint8_t s[4], d[4];
    const uint8_t *u = out + hlen(out);
    unsigned sp = g16(u), dp = g16(u + 2);
    memcpy(s, out + 12, 4);
    memcpy(d, out + 16, 4);
    return mk_udp(b, d, dp, s, sp, paylen, nocsum);
}

static struct sbnat nat;
static uint64_t now;

static void reset(void)
{
    struct sbnat_cfg c;
    memset(&c, 0xa5, sizeof(c));
    memcpy(c.uplink, UP, 4);
    memcpy(c.dns, DNS, 4);
    c.port_lo = 0;
    c.port_hi = 0;
    c.now_ms = 12345;
    memset(&nat, 0x5a, sizeof(nat));    /* sbnat_init must not rely on zeroed memory */
    sbnat_init(&nat, &c);
    now = 1000000;
}

static int G(uint8_t *b, unsigned len) { return sbnat_from_guest(&nat, b, len, now); }
static int U(uint8_t *b, unsigned len) { return sbnat_from_uplink(&nat, b, len, now); }
static unsigned sport(uint8_t *b) { return g16(l4p(b)); }
static unsigned dport(uint8_t *b) { return g16(l4p(b) + 2); }
static int inrange(unsigned p) { return p >= SBNAT_PORT_LO && p <= SBNAT_PORT_HI; }

/* ---- tests ---- */

static void test_tcp_roundtrip(void)
{
    uint8_t b[2000], o[2000];
    unsigned len, ep, i;
    static const uint8_t seq[][2] = { { 0x02, 0 }, { 0x12, 1 }, { 0x10, 0 }, { 0x18, 0 }, { 0x18, 1 }, { 0x10, 0 },
                                      { 0x11, 0 }, { 0x11, 1 }, { 0x10, 0 } };  /* flags, 1 = from the remote */
    reset();
    len = mk_tcp(b, GUEST, 51000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(pkt_ok(b));
    CHECK(!memcmp(b + 12, UP, 4) && !memcmp(b + 16, WEB, 4));
    ep = sport(b);
    CHECK(inrange(ep));
    CHECKEQ(dport(b), 443);
    CHECKEQ(b[8], 63);
    CHECKEQ(nat.st.mapped, 1);
    memcpy(o, b, len);
    for (i = 1; i < sizeof(seq) / sizeof(seq[0]); i++) {
        unsigned pl = (seq[i][0] & 0x08) ? 1 + rnd() % 1400 : 0;
        now += 10;
        if (seq[i][1]) {
            len = reply_tcp(b, o, seq[i][0], pl);
            CHECKEQ(U(b, len), SBNAT_TO_GUEST);
            CHECK(pkt_ok(b));
            CHECK(!memcmp(b + 12, WEB, 4) && !memcmp(b + 16, GUEST, 4));
            CHECKEQ(sport(b), 443);
            CHECKEQ(dport(b), 51000);
            CHECKEQ(b[8], 63);
        } else {
            len = mk_tcp(b, GUEST, 51000, WEB, 443, seq[i][0], pl);
            CHECKEQ(G(b, len), SBNAT_SEND);
            CHECK(pkt_ok(b));
            CHECKEQ(sport(b), ep);
            CHECK(!memcmp(b + 12, UP, 4));
        }
    }
    CHECKEQ(nat.st.mapped, 1);
    CHECKEQ(sbnat_count(&nat, now), 1);
    /* FIN both ways: 60 s */
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS - 1), 1);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS), 0);
    CHECKEQ(nat.st.expired, 1);
    now += SBNAT_TCP_CLOSE_MS;
    len = reply_tcp(b, o, 0x10, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);

    /* the guest's port is kept when it is in the range and free */
    len = mk_tcp(b, GUEST, 41234, WEB, 80, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sport(b), 41234);
    CHECK(pkt_ok(b));
    /* IP options are fine */
    {
        uint8_t *t = iphdr(b, GUEST, WEB, 6, 20, 8);
        p16(t, 41234); p16(t + 2, 80); t[12] = 0x50; t[13] = 0x10;
        finish(b, 0);
        CHECKEQ(G(b, tlen(b)), SBNAT_SEND);
        CHECK(pkt_ok(b));
        CHECKEQ(sport(b), 41234);
    }
}

static void test_two_flows(void)
{
    uint8_t a[200], b[200], r[200];
    unsigned la, lb, ea, eb, len;
    reset();
    la = mk_tcp(a, GUEST, 50000, WEB, 443, 0x02, 0);
    lb = mk_tcp(b, GUEST, 50001, WEB, 443, 0x02, 0);
    CHECKEQ(G(a, la), SBNAT_SEND);
    CHECKEQ(G(b, lb), SBNAT_SEND);
    ea = sport(a);
    eb = sport(b);
    CHECK(ea != eb);
    CHECK(inrange(ea) && inrange(eb));
    len = reply_tcp(r, b, 0x12, 0);
    CHECKEQ(U(r, len), SBNAT_TO_GUEST);
    CHECKEQ(dport(r), 50001);
    CHECK(pkt_ok(r));
    len = reply_tcp(r, a, 0x12, 0);
    CHECKEQ(U(r, len), SBNAT_TO_GUEST);
    CHECKEQ(dport(r), 50000);
    CHECK(pkt_ok(r));
    /* the same guest port to another destination: the same external port (endpoint-independent) */
    lb = mk_tcp(b, GUEST, 50000, WEB2, 443, 0x02, 0);
    CHECKEQ(G(b, lb), SBNAT_SEND);
    CHECKEQ(sport(b), ea);
    CHECKEQ(sbnat_count(&nat, now), 3);
    len = reply_tcp(r, b, 0x12, 0);
    CHECKEQ(U(r, len), SBNAT_TO_GUEST);
    CHECK(!memcmp(r + 12, WEB2, 4));
    CHECKEQ(dport(r), 50000);
    /* a third party to that port: not translated (address-dependent filtering) */
    len = mk_tcp(r, RTR, 443, UP, ea, 0x12, 0);
    CHECKEQ(U(r, len), SBNAT_NOT_OURS);
    /* UDP and TCP port spaces are separate: guest UDP 50000 may get the same number */
    lb = mk_udp(b, GUEST, 50000, WEB, 443, 10, 0);
    CHECKEQ(G(b, lb), SBNAT_SEND);
    CHECK(inrange(sport(b)));
}

static void test_dns(void)
{
    uint8_t b[600], o[600];
    unsigned len, ep;
    uint32_t mapped;
    reset();
    len = mk_udp(b, GUEST, 53000, GW, 53, 33, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(pkt_ok(b));
    CHECK(!memcmp(b + 12, UP, 4) && !memcmp(b + 16, DNS, 4));
    CHECKEQ(dport(b), 53);
    ep = sport(b);
    CHECK(inrange(ep));
    memcpy(o, b, len);
    len = reply_udp(b, o, 120, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(pkt_ok(b));
    CHECK(!memcmp(b + 12, GW, 4) && !memcmp(b + 16, GUEST, 4));
    CHECKEQ(sport(b), 53);
    CHECKEQ(dport(b), 53000);
    /* a reply from somewhere else to that port is not ours */
    len = mk_udp(b, RTR, 53, UP, ep, 20, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    /* the guest asking the uplink DNS directly from the same port: a separate group, a separate port */
    len = mk_udp(b, GUEST, 53000, DNS, 53, 33, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(sport(b) != ep);
    len = reply_udp(b, b, 50, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(!memcmp(b + 12, DNS, 4));     /* not rewritten to 10.77.0.1 */
    CHECK(pkt_ok(b));
    /* TCP DNS */
    len = mk_tcp(b, GUEST, 53001, GW, 53, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(pkt_ok(b));
    CHECK(!memcmp(b + 16, DNS, 4));
    memcpy(o, b, len);
    len = reply_tcp(b, o, 0x12, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(pkt_ok(b));
    CHECK(!memcmp(b + 12, GW, 4));
    CHECKEQ(sport(b), 53);
    CHECKEQ(dport(b), 53001);
    /* not DNS to 10.77.0.1: LOCAL, untouched */
    len = mk_udp(b, GUEST, 53000, GW, 67, 10, 0);
    memcpy(o, b, len);
    CHECKEQ(G(b, len), SBNAT_LOCAL);
    CHECK(!memcmp(o, b, len));
    len = mk_tcp(b, GUEST, 53000, GW, 22, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_LOCAL);
    /* no uplink DNS */
    sbnat_uplink_changed(&nat, UP, 0);
    mapped = nat.st.mapped;
    len = mk_udp(b, GUEST, 53000, GW, 53, 33, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_no_dns, 1);
    CHECKEQ(nat.st.mapped, mapped);
}

static void test_icmp_echo(void)
{
    uint8_t b[600], o[600];
    unsigned len, ep;
    reset();
    len = mk_echo(b, GUEST, WEB, 8, 0x1234, 1, 56);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(pkt_ok(b));
    ep = g16(l4p(b) + 4);
    CHECK(inrange(ep));
    memcpy(o, b, len);
    len = mk_echo(b, WEB, UP, 0, ep, 1, 56);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(pkt_ok(b));
    CHECKEQ(g16(l4p(b) + 4), 0x1234);
    CHECK(!memcmp(b + 16, GUEST, 4) && !memcmp(b + 12, WEB, 4));
    /* the reply from another host: not ours */
    len = mk_echo(b, WEB2, UP, 0, ep, 1, 56);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    /* an echo request from the uplink is the host's */
    len = mk_echo(b, WEB, UP, 8, ep, 1, 56);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    /* the guest pinging its gateway: LOCAL, then the reply helper */
    len = mk_echo(b, GUEST, GW, 8, 77, 3, 56);
    CHECKEQ(G(b, len), SBNAT_LOCAL);
    CHECKEQ(sbnat_echo_reply(b, len), len);
    CHECK(pkt_ok(b));
    CHECKEQ(l4p(b)[0], 0);
    CHECK(!memcmp(b + 12, GW, 4) && !memcmp(b + 16, GUEST, 4));
    CHECKEQ(g16(l4p(b) + 4), 77);
    CHECKEQ(sbnat_echo_reply(b, len), 0);               /* a reply is not a request */
    len = mk_echo(b, GUEST, GW, 8, 77, 3, 56);
    l4p(b)[9] ^= 1;
    CHECKEQ(sbnat_echo_reply(b, len), 0);               /* bad ICMP checksum */
    len = mk_echo(b, GUEST, WEB, 8, 77, 3, 56);
    CHECKEQ(sbnat_echo_reply(b, len), 0);               /* not to the gateway */
    /* other guest ICMP is dropped */
    len = mk_echo(b, GUEST, WEB, 0, 77, 3, 8);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_icmp, 1);
    /* ICMP expiry: 30 s */
    CHECKEQ(sbnat_count(&nat, now + SBNAT_ICMP_MS - 1), 1);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_ICMP_MS), 0);
}

/* an ICMP error quoting packet 'out' (as it went on the uplink); checks the translation */
static void err_case(const uint8_t *out, unsigned quote, uint8_t type, uint8_t code, unsigned mtu,
                     const uint8_t *from, const uint8_t *want_isrc, const uint8_t *want_idst, unsigned want_gport,
                     const uint8_t *want_osrc, int inner_complete)
{
    uint8_t b[2000];
    unsigned len = mk_err(b, from, UP, type, code, mtu, out, quote);
    uint8_t *ic = l4p(b), *in = ic + 8;
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(pkt_ok(b));       /* outer IP + ICMP checksums */
    CHECK(!memcmp(b + 16, GUEST, 4));
    CHECK(!memcmp(b + 12, want_osrc, 4));
    CHECKEQ(ic[0], type);
    CHECKEQ(ic[1], code);
    CHECKEQ(g16(ic + 6), mtu);
    CHECK(ip_hdr_ok(in));
    CHECK(!memcmp(in + 12, want_isrc, 4));
    CHECK(!memcmp(in + 16, want_idst, 4));
    if (in[9] == 1)
        CHECKEQ(g16(l4p(in) + 4), want_gport);
    else
        CHECKEQ(g16(l4p(in)), want_gport);
    if (inner_complete)
        CHECK(l4_ok(in, tlen(in) - hlen(in)));
}

static void test_icmp_errors(void)
{
    uint8_t t[2000], u[600], e[600], d[600], z[600], b[2000];
    unsigned lt, lu, le, ld, lz, len;
    reset();
    lt = mk_tcp(t, GUEST, 51500, WEB, 443, 0x18, 0);    /* a 40-byte segment: quoted whole */
    CHECKEQ(G(t, lt), SBNAT_SEND);
    lu = mk_udp(u, GUEST, 33434, WEB2, 33435, 0, 0);    /* traceroute-style, 28 bytes */
    CHECKEQ(G(u, lu), SBNAT_SEND);
    le = mk_echo(e, GUEST, WEB2, 8, 0x4242, 9, 0);
    CHECKEQ(G(e, le), SBNAT_SEND);
    ld = mk_udp(d, GUEST, 40404, GW, 53, 0, 0);
    CHECKEQ(G(d, ld), SBNAT_SEND);
    lz = mk_udp(z, GUEST, 33500, WEB2, 33435, 0, 1);    /* no UDP checksum */
    CHECKEQ(G(z, lz), SBNAT_SEND);
    CHECKEQ(g16(l4p(z) + 6), 0);
    /* frag-needed (PMTU) for TCP, whole segment quoted: the TCP checksum is fixed too */
    err_case(t, lt, 3, 4, 1280, RTR, GUEST, WEB, 51500, RTR, 1);
    /* frag-needed with only 8 bytes of TCP (the checksum is not there): ports restored */
    err_case(t, 28, 3, 4, 1400, RTR, GUEST, WEB, 51500, RTR, 0);
    /* time exceeded for UDP, port unreachable from the destination itself */
    err_case(u, lu, 11, 0, 0, RTR, GUEST, WEB2, 33434, RTR, 1);
    err_case(u, lu, 3, 3, 0, WEB2, GUEST, WEB2, 33434, WEB2, 1);
    /* time exceeded for an echo request (ping traceroute) */
    err_case(e, le, 11, 0, 0, RTR, GUEST, WEB2, 0x4242, RTR, 1);
    /* the DNS redirect: inner dst back to 10.77.0.1, and the outer source if the DNS server sent it */
    err_case(d, ld, 3, 3, 0, DNS, GUEST, GW, 40404, GW, 1);
    err_case(d, ld, 3, 1, 0, RTR, GUEST, GW, 40404, RTR, 1);
    /* a UDP checksum of 0 stays 0 inside an ICMP error */
    len = mk_err(b, RTR, UP, 11, 0, 0, z, lz);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECK(pkt_ok(b));
    CHECKEQ(g16(l4p(l4p(b) + 8) + 6), 0);
    CHECKEQ(g16(l4p(l4p(b) + 8)), 33500);
    CHECKEQ(nat.st.icmp_errors, 8);
    /* a bad ICMP checksum on one of ours: dropped */
    len = mk_err(b, RTR, UP, 3, 4, 1280, t, lt);
    l4p(b)[3] ^= 0x40;
    CHECKEQ(U(b, len), SBNAT_DROP);
    /* an error about a flow we don't have, or about the host's own port: not ours */
    {
        uint8_t h[200];
        unsigned lh = mk_udp(h, UP, 5353, WEB2, 33435, 0, 0);
        len = mk_err(b, RTR, UP, 11, 0, 0, h, lh);
        CHECKEQ(U(b, len), SBNAT_NOT_OURS);
        lh = mk_udp(h, UP, 41999, WEB2, 33435, 0, 0);
        len = mk_err(b, RTR, UP, 11, 0, 0, h, lh);
        CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    }
    /* quote too short (IP header + 4) */
    len = mk_err(b, RTR, UP, 11, 0, 0, u, 24);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
}

static void test_uplink_change(void)
{
    uint8_t b[200], o[200];
    unsigned len;
    reset();
    len = mk_tcp(b, GUEST, 50000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    memcpy(o, b, len);
    len = mk_udp(b, GUEST, 50000, GW, 53, 20, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now), 2);
    sbnat_uplink_changed(&nat, UP2, DNS2);
    CHECKEQ(sbnat_count(&nat, now), 0);
    CHECKEQ(nat.st.flushed, 2);
    CHECKEQ(nat.st.uplink_changes, 1);
    len = reply_tcp(b, o, 0x12, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);                 /* to the old address */
    memcpy(b + 16, UP2, 4);
    finish(b, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);                 /* to the new address: no mapping */
    /* old connection's ACK from the guest: picked up again on the new address; RST without mapping dropped */
    len = mk_tcp(b, GUEST, 50000, WEB, 443, 0x04, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_nomap, 1);
    len = mk_tcp(b, GUEST, 50000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(!memcmp(b + 12, UP2, 4));
    CHECK(pkt_ok(b));
    len = mk_udp(b, GUEST, 50000, GW, 53, 20, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(!memcmp(b + 16, DNS2, 4));
    CHECK(pkt_ok(b));
    /* no uplink at all */
    sbnat_uplink_changed(&nat, 0, 0);
    len = mk_udp(b, GUEST, 50000, WEB, 53, 20, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_no_uplink, 1);
    len = mk_udp(b, WEB, 53, UP2, 50000, 20, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
}

static void test_expiry(void)
{
    uint8_t b[200], o[200];
    unsigned len;
    reset();
    /* UDP one way: 60 s */
    len = mk_udp(b, GUEST, 50000, WEB, 123, 48, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_UDP_MS - 1), 1);
    sbnat_tick(&nat, now + SBNAT_UDP_MS);
    CHECKEQ(sbnat_count(&nat, now), 0);
    CHECKEQ(nat.st.expired, 1);
    /* UDP both ways: 180 s, refreshed by traffic */
    len = mk_udp(b, GUEST, 50000, WEB, 123, 48, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    memcpy(o, b, len);
    len = reply_udp(b, o, 48, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    now += SBNAT_UDP_BOTH_MS - 1;
    len = reply_udp(b, o, 48, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    now += SBNAT_UDP_BOTH_MS - 1;
    CHECKEQ(sbnat_count(&nat, now), 1);
    now += 1;
    len = reply_udp(b, o, 48, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);                 /* expired lazily, even without a tick */
    /* TCP SYN only: 120 s */
    len = mk_tcp(b, GUEST, 50001, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    memcpy(o, b, len);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_SYN_MS - 1), 1);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_SYN_MS), 0);
    /* established: 7440 s */
    len = mk_tcp(b, GUEST, 50001, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    memcpy(o, b, len);
    len = reply_tcp(b, o, 0x12, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_SYN_MS), 0);      /* SYN-ACK alone is not established */
    len = mk_tcp(b, GUEST, 50001, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    memcpy(o, b, len);                                  /* a new mapping: maybe a new port */
    len = reply_tcp(b, o, 0x12, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    len = mk_tcp(b, GUEST, 50001, WEB, 443, 0x10, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_EST_MS - 1), 1);
    /* FIN from one side only: still established */
    len = mk_tcp(b, GUEST, 50001, WEB, 443, 0x11, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS), 1);
    /* RST from the remote: 60 s */
    len = reply_tcp(b, o, 0x04, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS - 1), 1);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS), 0);
    /* a new SYN on a closed 4-tuple starts fresh (SYN-only timeout, not the closing one) */
    now += SBNAT_TCP_CLOSE_MS;
    len = mk_tcp(b, GUEST, 50002, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    len = reply_tcp(b, b, 0x14, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    len = mk_tcp(b, GUEST, 50002, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_CLOSE_MS), 1);
    /* mid-stream pickup (loose): non-SYN both ways = established */
    len = mk_tcp(b, GUEST, 50003, WEB, 443, 0x18, 100);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_SYN_MS - 1), 2);
    len = reply_tcp(b, b, 0x10, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_SYN_MS), 1);     /* 50002's SYN expired, 50003 established */
    CHECKEQ(sbnat_count(&nat, now + SBNAT_TCP_EST_MS - 1), 1);
}

static void test_table_full(void)
{
    uint8_t b[200];
    unsigned len, i, ports[SBNAT_ENTRIES];
    reset();
    /* 500 TCP + 12 UDP fill the table */
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        now += 1;
        if (i < 500)
            len = mk_tcp(b, GUEST, 20000 + i, WEB, 443, 0x02, 0);
        else
            len = mk_udp(b, GUEST, 20000 + i, WEB, 443, 4, 0);
        CHECKEQ(G(b, len), SBNAT_SEND);
        ports[i] = sport(b);
    }
    CHECKEQ(sbnat_count(&nat, now), SBNAT_ENTRIES);
    for (i = 1; i < SBNAT_ENTRIES; i++)
        if (ports[i] == ports[i - 1])
            break;
    CHECKEQ(i, SBNAT_ENTRIES);
    /* a new flow evicts the least recently used UDP one (the first, 20500) */
    now += 1;
    len = mk_tcp(b, GUEST, 30000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(nat.st.evicted, 1);
    len = mk_udp(b, WEB, 443, UP, ports[500], 4, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    len = mk_udp(b, WEB, 443, UP, ports[501], 4, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);                 /* refreshes 20501 */
    for (i = 0; i < 11; i++) {
        now += 1;
        len = mk_tcp(b, GUEST, 30001 + i, WEB, 443, 0x02, 0);
        CHECKEQ(G(b, len), SBNAT_SEND);
    }
    CHECKEQ(nat.st.evicted, 12);
    /* all TCP now: refused */
    len = mk_tcp(b, GUEST, 31000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.table_full, 1);
    len = mk_echo(b, GUEST, WEB, 8, 9, 9, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.table_full, 2);
    /* existing flows still work */
    len = mk_tcp(b, GUEST, 20007, WEB, 443, 0x10, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sport(b), ports[7]);
    /* once the oldest SYNs expire, their slots are reused */
    now = 1000000 + 10 + SBNAT_TCP_SYN_MS;
    len = mk_tcp(b, GUEST, 31000, WEB, 443, 0x02, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECK(nat.st.expired >= 1);
    CHECKEQ(sbnat_count(&nat, now), SBNAT_ENTRIES - 9 + 1); /* 20000..20009 expired, but for the refreshed 20007 */
}

static void test_misc_drops(void)
{
    uint8_t b[200], o[200];
    unsigned len, ep;
    reset();
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    ep = sport(b);
    /* outside the range, or an unmapped port in it, or not to the uplink: NOT_OURS, untouched */
    len = mk_udp(b, WEB, 443, UP, 8080, 4, 0);
    memcpy(o, b, len);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    CHECK(!memcmp(o, b, len));
    len = mk_udp(b, WEB, 443, UP, SBNAT_PORT_HI + 1, 4, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    len = mk_udp(b, WEB, 443, UP, ep == SBNAT_PORT_LO ? ep + 1 : SBNAT_PORT_LO, 4, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    len = mk_udp(b, WEB, 443, UP2, ep, 4, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    len = mk_tcp(b, WEB, 443, UP, ep, 0x12, 0);           /* the UDP port, but TCP */
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    len = mk_udp(b, WEB, 443, UP, ep, 4, 0);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    /* bad IP checksum: DROP both ways */
    len = mk_udp(b, WEB, 443, UP, ep, 4, 0);
    b[10] ^= 1;
    CHECKEQ(U(b, len), SBNAT_DROP);
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    b[11] ^= 0x80;
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_csum, 2);
    /* TTL */
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    b[8] = 1;
    finish(b, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_ttl, 1);
    len = mk_echo(b, GUEST, GW, 8, 1, 1, 0);           /* TTL 1 to the gateway itself is fine */
    b[8] = 1;
    finish(b, 0);
    CHECKEQ(G(b, len), SBNAT_LOCAL);
    len = mk_udp(b, WEB, 443, UP, ep, 4, 0);
    b[8] = 1;
    finish(b, 0);
    CHECKEQ(U(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_ttl, 2);
    /* fragments: a first fragment is translated, others dropped */
    len = mk_udp(b, GUEST, 50000, WEB, 443, 64, 0);
    p16(b + 6, 0x2000);
    finish(b, 0);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(sport(b), ep);
    len = mk_udp(b, GUEST, 50000, WEB, 443, 64, 0);
    p16(b + 6, 0x0010);
    finish(b, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_frag, 1);
    len = mk_udp(b, WEB, 443, UP, ep, 64, 0);
    p16(b + 6, 0x0010);
    finish(b, 0);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    CHECKEQ(nat.st.in_frags, 1);
    /* not IPv4 */
    len = mk_udp(b, GUEST, 50000, WEB, 443, 64, 0);
    b[0] = 0x60;
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(U(b, len), SBNAT_NOT_OURS);
    /* bad source, bad destinations, other protocols */
    len = mk_udp(b, GW, 50000, WEB, 443, 4, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_src, 1);
    {
        static const uint8_t bad[][4] = { { 0, 1, 2, 3 }, { 127, 0, 0, 1 }, { 224, 0, 0, 251 }, { 255, 255, 255, 255 },
                                          { 10, 77, 0, 3 }, { 10, 77, 0, 0 }, { 10, 77, 0, 2 }, { 192, 168, 1, 23 } };
        unsigned i;
        for (i = 0; i < sizeof(bad) / 4; i++) {
            len = mk_udp(b, GUEST, 50000, bad[i], 443, 4, 0);
            CHECKEQ(G(b, len), SBNAT_DROP);
        }
        CHECKEQ(nat.st.drop_dst, sizeof(bad) / 4);
    }
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    b[9] = 47;  /* GRE */
    finish(b, 0);
    CHECKEQ(G(b, len), SBNAT_DROP);
    CHECKEQ(nat.st.drop_proto, 1);
    /* total length beyond the buffer; header length beyond total */
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    CHECKEQ(G(b, len - 1), SBNAT_DROP);
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    p16(b + 2, 19);
    ipfix(b);
    CHECKEQ(G(b, len), SBNAT_DROP);
    /* TCP header cut short */
    len = mk_tcp(b, GUEST, 50000, WEB, 443, 0x02, 0);
    p16(b + 2, 36);
    ipfix(b);
    CHECKEQ(G(b, 36), SBNAT_DROP);
    CHECKEQ(nat.st.drop_short, 1);
    /* a buffer longer than the total length (link padding) is fine */
    len = mk_udp(b, GUEST, 50000, WEB, 443, 4, 0);
    CHECKEQ(G(b, len + 20), SBNAT_SEND);
    CHECK(pkt_ok(b));
}

static void test_udp_zero(void)
{
    uint8_t b[600], o[600];
    unsigned len;
    reset();
    len = mk_udp(b, GUEST, 50000, WEB, 5000, 100, 1);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(g16(l4p(b) + 6), 0);
    CHECK(pkt_ok(b));
    memcpy(o, b, len);
    len = reply_udp(b, o, 100, 1);
    CHECKEQ(U(b, len), SBNAT_TO_GUEST);
    CHECKEQ(g16(l4p(b) + 6), 0);
    CHECK(pkt_ok(b));
    /* and DNS without a checksum */
    len = mk_udp(b, GUEST, 50001, GW, 53, 30, 1);
    CHECKEQ(G(b, len), SBNAT_SEND);
    CHECKEQ(g16(l4p(b) + 6), 0);
    /* a checksummed datagram never comes out as 0: many random ones */
    {
        unsigned i, zeros = 0;
        for (i = 0; i < 4000; i++) {
            len = mk_udp(b, GUEST, 50000, WEB, 5000, rnd() % 40, 0);
            CHECKEQ(G(b, len), SBNAT_SEND);
            zeros += g16(l4p(b) + 6) == 0;
            if (!pkt_ok(b)) {
                CHECK(pkt_ok(b));
                break;
            }
        }
        CHECKEQ(zeros, 0);
    }
    /* crafted: a datagram whose rewritten checksum computes to 0 must carry 0xffff, both ways */
    {
        uint8_t t[100];
        unsigned ep, w, dir;
        len = mk_udp(b, GUEST, 50000, WEB, 5000, 2, 0);
        CHECKEQ(G(b, len), SBNAT_SEND);
        ep = sport(b);
        for (dir = 0; dir < 2; dir++) {
            /* the packet as it will look after the rewrite, payload 0, no checksum: pick the payload word */
            if (dir == 0)
                mk_udp(t, UP, ep, WEB, 5000, 2, 1);
            else
                mk_udp(t, WEB, 5000, GUEST, 50000, 2, 1);
            t[28] = t[29] = 0;
            w = 0xffffu - refsum(l4p(t), 10, pseudo(t, 10));
            if (dir == 0)
                len = mk_udp(b, GUEST, 50000, WEB, 5000, 2, 0);
            else
                len = mk_udp(b, WEB, 5000, UP, ep, 2, 0);
            b[28] = (uint8_t)(w >> 8);
            b[29] = (uint8_t)w;
            finish(b, 0);
            CHECK(g16(l4p(b) + 6) != 0);
            CHECKEQ(dir == 0 ? G(b, len) : U(b, len), dir == 0 ? SBNAT_SEND : SBNAT_TO_GUEST);
            CHECKEQ(g16(l4p(b) + 6), 0xffff);
            CHECK(pkt_ok(b));
        }
    }
}

/* the first 'cut' bytes of o; mode 1: a consistent header (total length = cut, IP + ICMP checksums redone) */
static void prep(uint8_t *p, const uint8_t *o, unsigned flen, unsigned cut, unsigned mode)
{
    memset(p, 0, cut);
    memcpy(p, o, cut < flen ? cut : flen);
    if (mode == 1 && cut >= 20) {
        p16(p + 2, cut);
        if (p[9] == 1 && cut >= 24) {
            p16(p + 22, 0);
            p16(p + 22, (uint16_t)~refsum(p + 20, cut - 20, 0));
        }
        p16(p + 10, 0);
        p16(p + 10, (uint16_t)~refsum(p, 20, 0));
    }
}

/* every length 0..60 in an exactly-sized heap buffer (ASan catches any read past it), for many headers */
static void test_truncated(void)
{
    uint8_t full[200], o[200];
    unsigned kinds, cut, mode;
    reset();
    /* a live mapping of each kind so the uplink side gets past the lookup where possible */
    {
        uint8_t b[200];
        unsigned len = mk_tcp(b, GUEST, 41000, WEB, 443, 0x02, 0);
        G(b, len);
        len = mk_udp(b, GUEST, 41001, WEB, 53, 0, 0);
        G(b, len);
        len = mk_echo(b, GUEST, WEB, 8, 41002, 1, 0);
        G(b, len);
        len = mk_udp(b, GUEST, 41003, GW, 53, 0, 0);
        G(b, len);
    }
    for (kinds = 0; kinds < 10; kinds++) {
        unsigned flen;
        switch (kinds) {
        case 0: flen = mk_tcp(full, GUEST, 41000, WEB, 443, 0x10, 20); break;
        case 1: flen = mk_udp(full, GUEST, 41001, WEB, 53, 20, 0); break;
        case 2: flen = mk_echo(full, GUEST, WEB, 8, 41002, 1, 20); break;
        case 3: flen = mk_udp(full, GUEST, 41003, GW, 53, 20, 0); break;
        case 4: flen = mk_echo(full, GUEST, GW, 8, 5, 1, 20); break;
        case 5: flen = mk_tcp(full, WEB, 443, UP, 41000, 0x10, 20); break;
        case 6: flen = mk_udp(full, WEB, 53, UP, 41001, 20, 0); break;
        case 7: flen = mk_echo(full, WEB, UP, 0, 41002, 1, 20); break;
        case 8: { uint8_t q[100]; unsigned lq = mk_tcp(q, UP, 41000, WEB, 443, 0x10, 0);
                  flen = mk_err(full, RTR, UP, 3, 4, 1280, q, lq); break; }
        default: { uint8_t q[100]; unsigned lq = mk_echo(q, UP, WEB, 8, 41002, 1, 0);
                   flen = mk_err(full, RTR, UP, 11, 0, 0, q, lq); break; }
        }
        memcpy(o, full, flen);
        for (mode = 0; mode < 2; mode++) {
            for (cut = 0; cut <= 60; cut++) {
                uint8_t *p = malloc(cut ? cut : 1);
                unsigned r;
                prep(p, o, flen, cut, mode);
                r = (unsigned)sbnat_from_guest(&nat, p, cut, now);
                CHECK(r <= SBNAT_NOT_OURS && r != SBNAT_TO_GUEST && r != SBNAT_NOT_OURS);
                prep(p, o, flen, cut, mode);
                r = (unsigned)sbnat_from_uplink(&nat, p, cut, now);
                CHECK(r == SBNAT_DROP || r == SBNAT_TO_GUEST || r == SBNAT_NOT_OURS);
                if (r == SBNAT_TO_GUEST)
                    CHECK(ip_hdr_ok(p));    /* (the cut L4 checksums were wrong to begin with) */
                prep(p, o, flen, cut, mode);
                r = sbnat_echo_reply(p, cut);
                CHECK(r == 0 || (kinds == 4 && r <= cut && pkt_ok(p)));
                free(p);
            }
        }
    }
    /* the ICMP error with a cut quote keeps every return value sane */
    {
        uint8_t q[100];
        unsigned lq = mk_udp(q, UP, 41001, WEB, 53, 0, 0), qlen, r;
        for (qlen = 0; qlen <= lq; qlen++) {
            uint8_t b[200];
            unsigned len = mk_err(b, RTR, UP, 3, 3, 0, q, qlen);
            uint8_t *p = malloc(len);
            memcpy(p, b, len);
            r = (unsigned)sbnat_from_uplink(&nat, p, len, now);
            CHECK(r == (qlen >= 28 ? SBNAT_TO_GUEST : SBNAT_NOT_OURS));
            if (r == SBNAT_TO_GUEST)
                CHECK(pkt_ok(p));
            free(p);
        }
    }
}

/* ---- randomized round trip ---- */

struct flow { uint8_t proto, dnat; unsigned gport, rport, eport; uint8_t raddr[4]; };

static void test_fuzz(void)
{
    static const uint8_t dsts[][4] = { { 93, 184, 216, 34 }, { 151, 101, 1, 69 }, { 8, 8, 8, 8 },
                                       { 192, 168, 1, 50 }, { 1, 1, 1, 1 } };
    struct flow fl[400];
    unsigned nfl = 0, iter, bad = 0, sent = 0, back = 0, errs = 0, lim = 300;
    uint8_t b[1600], o[1600], r[1600];
    reset();
    for (iter = 0; iter < 60000; iter++) {
        struct flow *f;
        int isnew = 0;
        unsigned len, k, pl = rnd() % 200, ep;
        int nocs = (rnd() & 7) == 0, res;
        now += rnd() % 3;
        if (nfl == lim) {   /* flush before the table can fill */
            sbnat_uplink_changed(&nat, UP, DNS);
            nfl = 0;
        }
        if (nfl == 0 || (rnd() % 4) == 0) {
            f = &fl[nfl++];
            isnew = 1;
            f->proto = (uint8_t)((const uint8_t[]){ 6, 17, 1 })[rnd() % 3];
            f->dnat = f->proto != 1 && (rnd() % 6) == 0;
            f->gport = (rnd() % 3) ? 32768 + rnd() % 28232 : 40000 + rnd() % 8000;
            if (rnd() % 8 == 0 && nfl > 1)
                f->gport = fl[rnd() % (nfl - 1)].gport;     /* a shared guest port */
            f->rport = f->proto == 1 ? 0 : f->dnat ? 53 : 1 + rnd() % 65535;
            memcpy(f->raddr, f->dnat ? DNS : dsts[rnd() % 5], 4);
            for (k = 0; k + 1 < nfl; k++) {     /* a duplicate flow: reuse the old one */
                struct flow *g = &fl[k];
                if (g->proto == f->proto && g->dnat == f->dnat && g->gport == f->gport && g->rport == f->rport &&
                    !memcmp(g->raddr, f->raddr, 4)) {
                    nfl--;
                    f = g;
                    isnew = 0;
                    break;
                }
            }
            if (isnew)
                f->eport = 0;
        } else {
            f = &fl[rnd() % nfl];
        }
        /* guest -> uplink */
        if (f->proto == 6)
            len = mk_tcp(b, GUEST, f->gport, f->dnat ? GW : f->raddr, f->rport, f->eport ? 0x18 : 0x02, pl);
        else if (f->proto == 17)
            len = mk_udp(b, GUEST, f->gport, f->dnat ? GW : f->raddr, f->rport, pl, nocs);
        else
            len = mk_echo(b, GUEST, f->raddr, 8, f->gport, iter & 0xffff, pl);
        res = G(b, len);
        if (res != SBNAT_SEND || !pkt_ok(b) || memcmp(b + 12, UP, 4) || memcmp(b + 16, f->raddr, 4)) {
            bad++;
            continue;
        }
        sent++;
        ep = f->proto == 1 ? g16(l4p(b) + 4) : sport(b);
        if (!inrange(ep) || (f->eport && ep != f->eport) || (f->proto != 1 && dport(b) != f->rport))
            bad++;
        f->eport = ep;
        /* the external port belongs to exactly one (proto, guest port, dnat) group */
        for (k = 0; k < nfl; k++) {
            struct flow *g = &fl[k];
            if (g != f && g->eport && g->proto == f->proto && g->eport == ep &&
                (g->gport != f->gport || g->dnat != f->dnat))
                bad++;
            if (g != f && g->eport && g->proto == f->proto && g->gport == f->gport && g->dnat == f->dnat &&
                g->eport != ep)
                bad++;
        }
        memcpy(o, b, len);
        /* uplink -> guest: the reply, or now and then an ICMP error about it */
        if (rnd() % 5 == 0) {
            unsigned q = len < 28 ? len : 28 + rnd() % (len - 27);
            len = mk_err(r, RTR, UP, (rnd() & 1) ? 3 : 11, (uint8_t)(rnd() % 5), rnd() & 0xffff, o, q);
            res = U(r, len);
            if (res != SBNAT_TO_GUEST || !pkt_ok(r) || !ip_hdr_ok(l4p(r) + 8) || memcmp(r + 16, GUEST, 4)) {
                bad++;
                continue;
            }
            {
                uint8_t *in = l4p(r) + 8, *il4 = l4p(in);
                unsigned ip = f->proto == 1 ? g16(il4 + 4) : g16(il4);
                if (memcmp(in + 12, GUEST, 4) || memcmp(in + 16, f->dnat ? GW : f->raddr, 4) || ip != f->gport)
                    bad++;
                if (tlen(in) == q && !l4_ok(in, q - hlen(in)))    /* complete quote: inner L4 checksum right */
                    bad++;
            }
            errs++;
            continue;
        }
        if (f->proto == 6)
            len = reply_tcp(r, o, 0x10 | (rnd() & 0x08), rnd() % 200);
        else if (f->proto == 17)
            len = reply_udp(r, o, rnd() % 200, (rnd() & 7) == 0);
        else
            len = mk_echo(r, f->raddr, UP, 0, ep, 1, rnd() % 100);
        res = U(r, len);
        if (res != SBNAT_TO_GUEST || !pkt_ok(r) || memcmp(r + 16, GUEST, 4) ||
            memcmp(r + 12, f->dnat ? GW : f->raddr, 4)) {
            bad++;
            continue;
        }
        if ((f->proto == 1 ? g16(l4p(r) + 4) : dport(r)) != f->gport ||
            (f->proto != 1 && sport(r) != f->rport))
            bad++;
        back++;
        /* and a corrupted copy of it must not crash nor be accepted with a broken header */
        len = reply_udp(r, o, rnd() % 50, 0);
        {
            unsigned nflip = 1 + rnd() % 4, j;
            for (j = 0; j < nflip; j++)
                r[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
            res = (rnd() & 1) ? U(r, len) : G(r, len);
            if ((res == SBNAT_SEND || res == SBNAT_TO_GUEST) && !ip_hdr_ok(r))
                bad++;
        }
    }
    printf("  fuzz: %u sent, %u back, %u icmp errors, %u mapped\n", sent, back, errs, nat.st.mapped);
    CHECKEQ(bad, 0);
    CHECK(sent > 50000);
    CHECK(back > 30000);
    CHECK(errs > 8000);
    CHECKEQ(nat.st.table_full, 0);
    checks += sent + back + errs;
}

/* random bytes, both directions: never crash, never emit a bad IP header */
static void test_garbage(void)
{
    uint8_t b[1600];
    unsigned iter, bad = 0;
    reset();
    for (iter = 0; iter < 100000; iter++) {
        unsigned len = rnd() % 120, i, res;
        uint8_t *p = malloc(len ? len : 1);
        for (i = 0; i < len; i++)
            b[i] = (uint8_t)rnd();
        if (len >= 20) {
            b[0] = (uint8_t)(0x45 + (rnd() % 3 == 0 ? rnd() % 4 : 0));
            if (rnd() & 1) p16(b + 2, len);
            b[9] = (uint8_t)((const uint8_t[]){ 1, 6, 17, 1 })[rnd() % 4];
            if (rnd() & 1) memcpy(b + 12, GUEST, 4);
            if (rnd() & 1) memcpy(b + 16, (rnd() & 1) ? UP : GW, 4);
            if (rnd() & 1) {
                b[6] &= 0xe0;
                b[7] = 0;
            }
            p16(b + 10, 0);
            p16(b + 10, (uint16_t)~refsum(b, (b[0] & 15u) * 4 <= len ? (b[0] & 15u) * 4 : 20, 0));
        }
        memcpy(p, b, len);
        res = (unsigned)((iter & 1) ? sbnat_from_uplink(&nat, p, len, now) : sbnat_from_guest(&nat, p, len, now));
        if ((res == SBNAT_SEND || res == SBNAT_TO_GUEST) && !ip_hdr_ok(p))
            bad++;
        free(p);
        now += 7;
    }
    CHECKEQ(bad, 0);
}

int main(void)
{
    test_tcp_roundtrip();
    test_two_flows();
    test_dns();
    test_icmp_echo();
    test_icmp_errors();
    test_uplink_change();
    test_expiry();
    test_table_full();
    test_misc_drops();
    test_udp_zero();
    test_truncated();
    test_fuzz();
    test_garbage();
    printf("sbnat host test: %u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
