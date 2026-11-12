/* SPDX-License-Identifier: MIT
 * sbgw: the Linux guest's gateway (gw.h). Written for Starbite; RFC 826 (ARP), 2131/2132 (DHCP server side), 792
 * (ICMP), 1191 (fragmentation needed), 3022 and 4787 (NAPT, UDP mapping), 5382 (TCP NAT), 1624 (incremental
 * checksums), 793 (resets). Every byte from either side is bounds-checked: the guest is not trusted, the uplink
 * carries the Internet. */
#include "gw.h"

#define ETH_ARP 0x0806
#define ETH_IP 0x0800
#define P_ICMP 1
#define P_TCP 6
#define P_UDP 17
#define T_FIN 0x01
#define T_SYN 0x02
#define T_RST 0x04
#define T_ACK 0x10

static const uint8_t gw_ip[4] = { 10, 0, 2, 2 }, dns_ip[4] = { 10, 0, 2, 3 }, guest_ip[4] = { 10, 0, 2, 15 };
static const uint8_t net_bcast[4] = { 10, 0, 2, 255 }, mask24[4] = { 255, 255, 255, 0 };
static const uint8_t all_ones[4] = { 255, 255, 255, 255 }, bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ---- bytes ------------------------------------------------------------------------------------------------ */

static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static void set(uint8_t *d, uint8_t v, uint32_t n) { while (n--) *d++ = v; }
static int eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}
static int zero4(const uint8_t *a) { return !(a[0] | a[1] | a[2] | a[3]); }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static int after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t s)
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
/* RFC 1624 (eqn. 3): HC' = ~(~HC + ~m + m') for one 16-bit word m -> m' */
static void adj16(uint8_t *ck, uint16_t m, uint16_t m2)
{
    uint32_t s = (uint32_t)(uint16_t)~g16(ck) + (uint16_t)~m + m2;
    s = (s & 0xffff) + (s >> 16);
    s = (s & 0xffff) + (s >> 16);
    p16(ck, (uint16_t)~s);
}
static void adj32(uint8_t *ck, const uint8_t *a, const uint8_t *b)
{
    adj16(ck, g16(a), g16(b));
    adj16(ck, g16(a + 2), g16(b + 2));
}
static void ip_sum(uint8_t *ip)
{
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4;
    p16(ip + 10, 0);
    p16(ip + 10, fold(sum16(ip, ihl, 0)));
}
/* the TCP/UDP checksum over the pseudo-header and len bytes of the segment at l4 */
static uint16_t l4_sum(const uint8_t *ip, uint8_t proto, const uint8_t *l4, uint32_t len)
{
    uint32_t s = sum16(ip + 12, 8, 0) + proto + len;
    return fold(sum16(l4, len, s));
}

static void say(struct sbgw *gw, const char *t)
{
    if (gw->os.log)
        gw->os.log(gw->os.ctx, t);
}
static uint64_t now(struct sbgw *gw) { return gw->os.now_ms(gw->os.ctx); }
static int in_subnet(const uint8_t *a) { return a[0] == 10 && a[1] == 0 && a[2] == 2; }

/* ---- frames to the guest ------------------------------------------------------------------------------------- */

/* f: 14 bytes of Ethernet header room, then the IPv4 packet (total bytes; its checksums done) */
static void to_guest_ip(struct sbgw *gw, uint8_t *f, uint32_t total, const uint8_t *dmac)
{
    uint32_t len = 14 + total;
    cpy(f, dmac ? dmac : gw->guest_mac, 6);
    cpy(f + 6, gw->mac, 6);
    p16(f + 12, ETH_IP);
    if (len < 60) {
        set(f + len, 0, 60 - len);
        len = 60;
    }
    gw->guest_tx++;
    if (gw->os.to_guest(gw->os.ctx, f, len))
        gw->dropped++;
}

/* a fresh IPv4 header at ip: src -> dst, proto, total bytes, TTL 64 */
static void ip_hdr(struct sbgw *gw, uint8_t *ip, const uint8_t *src, const uint8_t *dst, uint8_t proto, uint32_t total)
{
    ip[0] = 0x45, ip[1] = 0;
    p16(ip + 2, (uint16_t)total);
    p16(ip + 4, gw->ip_id++);
    p16(ip + 6, 0);
    ip[8] = 64, ip[9] = proto;
    cpy(ip + 12, src, 4), cpy(ip + 16, dst, 4);
    ip_sum(ip);
}

/* an ICMP error to the guest about its packet `orig` (from `src`): its header and the first bytes quoted */
static void icmp_error(struct sbgw *gw, uint8_t type, uint8_t code, uint16_t mtu, const uint8_t *src, const uint8_t *orig,
                       uint32_t orig_total)
{
    uint8_t f[14 + 20 + 8 + 548];
    uint8_t *ip = f + 14, *c = ip + 20;
    uint32_t q = orig_total < 548 ? orig_total : 548;
    c[0] = type, c[1] = code, p16(c + 2, 0), p16(c + 4, 0), p16(c + 6, mtu);
    cpy(c + 8, orig, q);
    p16(c + 2, fold(sum16(c, 8 + q, 0)));
    ip_hdr(gw, ip, src, orig + 12, P_ICMP, 20 + 8 + q);
    to_guest_ip(gw, f, 20 + 8 + q, 0);
}

/* a TCP segment to the guest from (src, sport) with no data: the resets */
static void tcp_to_guest(struct sbgw *gw, const uint8_t *src, uint16_t sport, const uint8_t *dst, uint16_t dport,
                         uint32_t seq, uint32_t ack, uint8_t flags)
{
    uint8_t f[60];                              /* 14 + 40, padded to the minimum frame */
    uint8_t *ip = f + 14, *t = ip + 20;
    set(t, 0, 20);
    p16(t, sport), p16(t + 2, dport), p32(t + 4, seq), p32(t + 8, ack);
    t[12] = 5 << 4, t[13] = flags;
    ip_hdr(gw, ip, src, dst, P_TCP, 40);
    p16(t + 16, l4_sum(ip, P_TCP, t, 20));
    gw->rst_sent += !!(flags & T_RST);
    to_guest_ip(gw, f, 40, 0);
}

/* the reset RFC 793 sends for a segment without a connection (ip: the guest's segment, t its TCP header) */
static void rst_reply(struct sbgw *gw, const uint8_t *ip, const uint8_t *t, uint32_t tlen)
{
    uint8_t fl = t[13];
    uint32_t doff = (uint32_t)(t[12] >> 4) * 4;
    if (fl & T_RST)
        return;
    if (fl & T_ACK)
        tcp_to_guest(gw, ip + 16, g16(t + 2), ip + 12, g16(t), g32(t + 8), 0, T_RST);
    else
        tcp_to_guest(gw, ip + 16, g16(t + 2), ip + 12, g16(t), 0,
                     g32(t + 4) + (tlen - doff) + !!(fl & T_SYN) + !!(fl & T_FIN), T_RST | T_ACK);
}

/* ---- the table ------------------------------------------------------------------------------------------------- */

static void conn_reset_guest(struct sbgw *gw, struct sbgw_conn *c)
{
    if (c->proto == P_TCP && !((c->flags & SBGW_F_FIN_G) && (c->flags & SBGW_F_FIN_R)))
        tcp_to_guest(gw, c->gdst, c->gdport, c->gsrc, c->gport, (c->flags & SBGW_F_HAVE_R) ? c->rnxt : 0, c->gnxt,
                     T_RST | T_ACK);
}

static void flush(struct sbgw *gw)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SBGW_CONNS; i++)
        if (gw->c[i].proto) {
            conn_reset_guest(gw, &gw->c[i]);
            gw->c[i].proto = 0;
            n++;
        }
    if (n) {
        gw->flushed += n;
        say(gw, "gw: the uplink changed: the guest's connections reset");
    }
}

static uint32_t timeout(const struct sbgw_conn *c)
{
    if (c->proto == P_TCP) {
        if ((c->flags & SBGW_F_FIN_G) && (c->flags & SBGW_F_FIN_R))
            return SBGW_T_TCP_CLOSED;
        if (c->flags & (SBGW_F_FIN_G | SBGW_F_FIN_R))
            return SBGW_T_TCP_FIN;
        return c->tcp == SBGW_TCP_EST ? SBGW_T_TCP_EST : SBGW_T_TCP_SYN;
    }
    if (c->proto == P_UDP)
        return c->rdport == 53 ? SBGW_T_UDP_DNS : (c->flags & SBGW_F_REPLIED) ? SBGW_T_UDP_REPLIED : SBGW_T_UDP;
    return SBGW_T_ICMP;
}

static int port_used(const struct sbgw *gw, uint8_t proto, uint16_t port)
{
    if (proto == P_ICMP && gw->up && port == gw->up->ping_id)
        return 1;                               /* sbnet's own pings keep their replies */
    for (unsigned i = 0; i < SBGW_CONNS; i++)
        if (gw->c[i].proto == proto && gw->c[i].mport == port)
            return 1;
    return 0;
}

static uint16_t port_pick(struct sbgw *gw, uint8_t proto)
{
    uint32_t span = SBGW_PORT_HI - SBGW_PORT_LO + 1, start = gw->os.random(gw->os.ctx) % span;
    for (uint32_t i = 0; i < span; i++) {
        uint16_t p = (uint16_t)(SBGW_PORT_LO + (start + i) % span);
        if (!port_used(gw, proto, p))
            return p;
    }
    return 0;                                   /* cannot happen: SBGW_CONNS < span */
}

static struct sbgw_conn *conn_new(struct sbgw *gw, uint8_t proto, const uint8_t *gsrc, uint16_t gport, const uint8_t *gdst,
                                  uint16_t gdport, const uint8_t *rdst)
{
    struct sbgw_conn *c = 0;
    uint16_t mport = 0;
    for (unsigned i = 0; i < SBGW_CONNS && !c; i++)
        if (!gw->c[i].proto)
            c = &gw->c[i];
    if (!c) {                                   /* full: the one idle longest goes */
        c = &gw->c[0];
        for (unsigned i = 1; i < SBGW_CONNS; i++)
            if (gw->c[i].last < c->last)
                c = &gw->c[i];
        conn_reset_guest(gw, c);
        c->proto = 0;
        gw->evicted++;
    }
    if (proto == P_UDP) {                       /* RFC 4787: one mapping per guest address and port */
        for (unsigned i = 0; i < SBGW_CONNS && !mport; i++) {
            const struct sbgw_conn *o = &gw->c[i];
            if (o->proto == P_UDP && o->gport == gport && eq(o->gsrc, gsrc, 4))
                mport = o->mport;
        }
        for (unsigned i = 0; i < SBGW_CONNS && mport; i++) {
            const struct sbgw_conn *o = &gw->c[i];
            if (o->proto == P_UDP && o->mport == mport && o->rdport == gdport && eq(o->rdst, rdst, 4))
                mport = 0;                      /* that far end is already another flow's on this port */
        }
    }
    if (!mport)
        mport = port_pick(gw, proto);
    set((uint8_t *)c, 0, sizeof(*c));
    c->proto = proto;
    cpy(c->gsrc, gsrc, 4), cpy(c->gdst, gdst, 4), cpy(c->rdst, rdst, 4);
    c->gport = gport, c->gdport = gdport, c->rdport = gdport, c->mport = mport;
    c->last = now(gw);
    return c;
}

static unsigned uplink_ok(struct sbgw *gw)
{
    struct sbnet *up = gw->up;
    if (!up || up->state < SBNET_BOUND || zero4(up->ip))
        return 0;
    if (!eq(up->ip, gw->up_ip, 4)) {            /* a new address (a new lease, a new network): the old mappings are void */
        flush(gw);
        cpy(gw->up_ip, up->ip, 4);
    }
    return 1;
}

/* ---- translation ------------------------------------------------------------------------------------------------ */

/* the packet's addresses and ports (ICMP echo: sport is the id) to these, its checksums following: the IPv4 header
 * recomputed, TCP / UDP / ICMP incrementally (RFC 1624) */
static void rewrite(uint8_t *ip, const uint8_t *src, const uint8_t *dst, uint16_t sport, uint16_t dport)
{
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, l4len = g16(ip + 2) - ihl;
    uint8_t *l4 = ip + ihl, *ck = 0;
    if (ip[9] == P_TCP && l4len >= 20)
        ck = l4 + 16;
    else if (ip[9] == P_UDP && l4len >= 8 && g16(l4 + 6))
        ck = l4 + 6;
    if (ck) {
        adj32(ck, ip + 12, src);
        adj32(ck, ip + 16, dst);
        adj16(ck, g16(l4), sport);
        adj16(ck, g16(l4 + 2), dport);
        if (ip[9] == P_UDP && !g16(ck))
            p16(ck, 0xffff);
    }
    if (ip[9] == P_TCP || ip[9] == P_UDP)
        p16(l4, sport), p16(l4 + 2, dport);
    if (ip[9] == P_ICMP && l4len >= 8) {
        adj16(l4 + 2, g16(l4 + 4), sport);
        p16(l4 + 4, sport);
    }
    cpy(ip + 12, src, 4), cpy(ip + 16, dst, 4);
    ip_sum(ip);
}

/* a SYN's MSS option down to max (the checksum following) */
static void mss_clamp(uint8_t *t, uint32_t doff, uint16_t max)
{
    for (uint32_t o = 20; o < doff;) {
        uint8_t kind = t[o];
        if (kind == 0)
            break;
        if (kind == 1) {
            o++;
            continue;
        }
        if (o + 1 >= doff || t[o + 1] < 2 || o + t[o + 1] > doff)
            break;
        if (kind == 2 && t[o + 1] == 4 && g16(t + o + 2) > max) {
            /* the 16-bit words of the header that hold the value (it may straddle two) */
            uint32_t a = (o + 2) & ~1u, b = (o + 3) & ~1u;
            uint16_t w1 = g16(t + a), w2 = g16(t + b);
            p16(t + o + 2, max);
            adj16(t + 16, w1, g16(t + a));
            if (b != a)
                adj16(t + 16, w2, g16(t + b));
        }
        o += t[o + 1];
    }
}

static uint32_t link_limit(const struct sbgw *gw)
{
    uint32_t m = gw->up ? gw->up->mtu : SBGW_MTU;
    return m < 576 ? 576 : m < SBGW_MTU ? m : SBGW_MTU;
}

/* the translated packet (total bytes at f + 14) onto the uplink, in fragments when it is larger than the uplink's
 * MTU (only called then for packets without DF, a plain 20-byte header) */
static void up_send(struct sbgw *gw, uint8_t *f, uint32_t total)
{
    uint32_t lim = link_limit(gw);
    if (total <= lim) {
        if (sbnet_ip_out(gw->up, f, total))
            gw->dropped++;
        else
            gw->out++;
        return;
    }
    {
        /* in place: fragment k's header (and the Ethernet room) go over bytes fragment k - 1 already took */
        uint8_t hdr[20];
        uint32_t pay = total - 20, chunk = (lim - 20) & ~7u;
        cpy(hdr, f + 14, 20);
        for (uint32_t off = 0; off < pay; off += chunk) {
            uint32_t n = pay - off < chunk ? pay - off : chunk;
            uint8_t *q = f + 14 + off;
            cpy(q, hdr, 20);
            p16(q + 2, (uint16_t)(20 + n));
            p16(q + 6, (uint16_t)((off + n < pay ? 0x2000 : 0) | (off / 8)));
            ip_sum(q);
            if (sbnet_ip_out(gw->up, f + off, 20 + n))
                gw->dropped++;
            else
                gw->out++;
        }
        gw->fragmented++;
    }
}

/* TCP state from a segment (from_guest: which way) */
static int tcp_track(struct sbgw_conn *c, const uint8_t *t, uint32_t tlen, int from_guest)
{
    uint8_t fl = t[13];
    uint32_t doff = (uint32_t)(t[12] >> 4) * 4, seq = g32(t + 4);
    uint32_t end = seq + (tlen - doff) + !!(fl & T_SYN) + !!(fl & T_FIN);
    if (fl & T_RST)
        return 1;                               /* over: forwarded, then forgotten */
    if (from_guest) {
        if ((fl & (T_SYN | T_ACK)) == T_SYN)
            c->gnxt = end;
        else if (after(end, c->gnxt))
            c->gnxt = end;
        if ((fl & T_ACK) && (!(c->flags & SBGW_F_HAVE_R) || after(g32(t + 8), c->rnxt))) {
            c->rnxt = g32(t + 8);
            c->flags |= SBGW_F_HAVE_R;
        }
        if (fl & T_FIN)
            c->flags |= SBGW_F_FIN_G;
    } else {
        if ((fl & (T_SYN | T_ACK)) == (T_SYN | T_ACK) && c->tcp == SBGW_TCP_SYN) {
            c->tcp = SBGW_TCP_EST;
            c->rnxt = end;
            c->flags |= SBGW_F_HAVE_R;
        } else if ((c->flags & SBGW_F_HAVE_R) && seq == c->rnxt) {
            c->rnxt = end;                      /* in order only: the guest's own ACKs fill in after a gap */
        }
        if (fl & T_FIN)
            c->flags |= SBGW_F_FIN_R;
    }
    return 0;
}

static void conn_free(struct sbgw_conn *c) { c->proto = 0; }

/* a packet from the guest to the world (the frame's IPv4 packet at ip, total bytes; rdst: where it really goes) */
static void nat_out(struct sbgw *gw, const uint8_t *ip, uint32_t total, const uint8_t *rdst)
{
    uint8_t f[14 + SBGW_MTU];
    uint8_t *q = f + 14, *l4;
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, l4len = total - ihl, doff = 0;
    uint8_t proto = ip[9], fl = 0;
    uint16_t sport, dport;
    struct sbgw_conn *c = 0;
    const uint8_t *g = ip + ihl;
    /* the transport header, checked */
    if (proto == P_TCP) {
        doff = l4len >= 20 ? (uint32_t)(g[12] >> 4) * 4 : 0;
        if (doff < 20 || doff > l4len) {
            gw->dropped++;
            return;
        }
        fl = g[13];
        sport = g16(g), dport = g16(g + 2);
    } else if (proto == P_UDP) {
        if (l4len < 8 || g16(g + 4) < 8 || g16(g + 4) > l4len) {
            gw->dropped++;
            return;
        }
        sport = g16(g), dport = g16(g + 2);
    } else if (proto == P_ICMP) {
        if (l4len < 8 || g[0] != 8 || g[1] != 0) { /* echo requests only: the guest's ICMP errors stay here */
            gw->dropped++;
            return;
        }
        sport = g16(g + 4), dport = 0;
    } else {
        gw->dropped++;                          /* other protocols cannot be translated */
        return;
    }
    if (!uplink_ok(gw)) {
        gw->no_uplink++;
        if (proto == P_TCP && (fl & (T_SYN | T_ACK)) != T_SYN)
            rst_reply(gw, ip, g, l4len);
        else
            icmp_error(gw, 3, 0, 0, gw_ip, ip, total);     /* network unreachable: a connect() fails at once */
        return;
    }
    if (zero4(rdst)) {                          /* the DNS relay without a DNS server on the uplink */
        icmp_error(gw, 3, 3, 0, dns_ip, ip, total);
        return;
    }
    if (ip[8] <= 1) {
        icmp_error(gw, 11, 0, 0, gw_ip, ip, total);        /* traceroute's first hop */
        return;
    }
    if (total > link_limit(gw) && ((g16(ip + 6) & 0x4000) || ihl != 20)) {
        gw->too_big++;
        icmp_error(gw, 3, 4, (uint16_t)link_limit(gw), gw_ip, ip, total);
        return;
    }
    for (unsigned i = 0; i < SBGW_CONNS; i++) {
        struct sbgw_conn *o = &gw->c[i];
        if (o->proto == proto && o->gport == sport && o->gdport == dport && eq(o->gsrc, ip + 12, 4) &&
            eq(o->gdst, ip + 16, 4)) {
            c = o;
            break;
        }
    }
    if (proto == P_TCP && (fl & (T_SYN | T_ACK | T_RST)) == T_SYN) {
        if (c && (c->flags & (SBGW_F_FIN_G | SBGW_F_FIN_R))) {
            c->tcp = 0, c->flags = 0;           /* the 4-tuple again after a close: a new connection, same port */
        }
    } else if (proto == P_TCP && !c) {
        rst_reply(gw, ip, g, l4len);            /* nothing known (flushed, evicted, expired): fail at once */
        return;
    }
    if (!c)
        c = conn_new(gw, proto, ip + 12, sport, ip + 16, dport, rdst);
    if (proto == P_TCP && !c->tcp)
        c->tcp = SBGW_TCP_SYN;
    cpy(q, ip, total);
    l4 = q + ihl;
    q[8]--;
    if (proto == P_TCP) {
        if (fl & T_SYN)
            mss_clamp(l4, doff, (uint16_t)(link_limit(gw) - 40));
        if (tcp_track(c, l4, l4len, 1))
            conn_free(c);
    }
    rewrite(q, gw->up->ip, c->rdst, c->mport, c->rdport);
    c->last = now(gw);
    up_send(gw, f, total);
}

/* an ICMP error from the uplink about one of the guest's packets: its quote translated back (in place in q) */
static int icmp_error_in(struct sbgw *gw, uint8_t *q, uint32_t total)
{
    uint32_t ihl = (uint32_t)(q[0] & 15) * 4, len = total - ihl, iihl, qlen;
    uint8_t *c = q + ihl, *in = c + 8, *il4;
    uint16_t isport, idport = 0;
    struct sbgw_conn *k = 0;
    if (len < 8 + 20 + 8)
        return 0;
    iihl = (uint32_t)(in[0] & 15) * 4;
    qlen = len - 8;
    if ((in[0] >> 4) != 4 || iihl < 20 || iihl + 8 > qlen || !eq(in + 12, gw->up->ip, 4))
        return 0;
    il4 = in + iihl;
    if (in[9] == P_TCP || in[9] == P_UDP)
        isport = g16(il4), idport = g16(il4 + 2);
    else if (in[9] == P_ICMP && il4[0] == 8)
        isport = g16(il4 + 4);
    else
        return 0;
    if (isport < SBGW_PORT_LO || isport > SBGW_PORT_HI)
        return 0;
    for (unsigned i = 0; i < SBGW_CONNS; i++) {
        struct sbgw_conn *o = &gw->c[i];
        if (o->proto == in[9] && o->mport == isport && o->rdport == idport && eq(o->rdst, in + 16, 4)) {
            k = o;
            break;
        }
    }
    if (!k)
        return 1;                               /* ours, but over: dropped */
    /* the quote: addresses and ports back to the guest's view; its checksums where the quote has them */
    {
        uint32_t il4len = qlen - iihl;
        uint8_t *ck = 0;
        if (in[9] == P_TCP && il4len >= 18)
            ck = il4 + 16;
        else if (in[9] == P_UDP && g16(il4 + 6))
            ck = il4 + 6;
        else if (in[9] == P_ICMP)
            ck = il4 + 2;
        if (ck && in[9] != P_ICMP) {
            adj32(ck, in + 12, k->gsrc);
            adj32(ck, in + 16, k->gdst);
            adj16(ck, isport, k->gport);
            adj16(ck, idport, k->gdport);
        } else if (ck) {
            adj16(ck, isport, k->gport);
        }
        if (in[9] == P_ICMP) {
            p16(il4 + 4, k->gport);
        } else {
            p16(il4, k->gport);
            p16(il4 + 2, k->gdport);
        }
        cpy(in + 12, k->gsrc, 4), cpy(in + 16, k->gdst, 4);
        if (iihl <= qlen) {
            p16(in + 10, 0);
            p16(in + 10, fold(sum16(in, iihl, 0)));
        }
    }
    cpy(q + 16, k->gsrc, 4);
    p16(c + 2, 0);
    p16(c + 2, fold(sum16(c, len, 0)));
    ip_sum(q);
    gw->icmp_in++;
    return 2;
}

/* sbnet's hook: an IPv4 packet to the uplink's address; 1 = the guest's (taken) */
static int hook(void *ctx, struct sbnet *n, const uint8_t *ip, uint32_t len)
{
    struct sbgw *gw = ctx;
    uint8_t f[14 + SBGW_MTU];
    uint8_t *q = f + 14, *l4;
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, total = len, l4len;
    uint8_t proto = ip[9];
    uint16_t sport, dport;
    struct sbgw_conn *c = 0;
    const uint8_t *s = ip + ihl;
    if (n != gw->up || !uplink_ok(gw) || total < ihl || ihl < 20)
        return 0;
    l4len = total - ihl;
    if (proto == P_TCP || proto == P_UDP) {
        if (l4len < (proto == P_TCP ? 20u : 8u))
            return 0;
        sport = g16(s), dport = g16(s + 2);
        if (dport < SBGW_PORT_LO || dport > SBGW_PORT_HI)
            return 0;
    } else if (proto == P_ICMP && l4len >= 8 && s[0] == 0 && s[1] == 0) {
        sport = 0, dport = g16(s + 4);
        if (dport < SBGW_PORT_LO || dport > SBGW_PORT_HI || dport == n->ping_id)
            return 0;
    } else if (proto == P_ICMP && l4len >= 8 && (s[0] == 3 || s[0] == 11 || s[0] == 12)) {
        int r;
        if (total > SBGW_MTU || !gw->guest_known)
            return 0;
        cpy(q, ip, total);
        if (fold(sum16(q + ihl, l4len, 0)) != 0)
            return 0;
        r = icmp_error_in(gw, q, total);
        if (r == 2 && q[8] > 1) {
            q[8]--;
            ip_sum(q);
            to_guest_ip(gw, f, total, 0);
        }
        return r != 0;
    } else {
        return 0;
    }
    for (unsigned i = 0; i < SBGW_CONNS; i++) {
        struct sbgw_conn *o = &gw->c[i];
        if (o->proto == proto && o->mport == dport && o->rdport == sport && eq(o->rdst, ip + 12, 4)) {
            c = o;
            break;
        }
    }
    if (!c || total > SBGW_MTU || ip[8] <= 1) {
        gw->dropped++;                          /* the NAT's ports: nobody else's, dropped */
        return 1;
    }
    cpy(q, ip, total);
    l4 = q + ihl;
    q[8]--;
    if (proto == P_TCP) {
        uint32_t doff = (uint32_t)(l4[12] >> 4) * 4;
        if (doff < 20 || doff > l4len) {
            gw->dropped++;
            return 1;
        }
        if (l4[13] & T_SYN)
            mss_clamp(l4, doff, (uint16_t)(link_limit(gw) - 40));
        if (tcp_track(c, l4, l4len, 0))
            conn_free(c);
    } else if (proto == P_UDP && (g16(l4 + 4) < 8 || g16(l4 + 4) > l4len)) {
        gw->dropped++;
        return 1;
    }
    rewrite(q, c->gdst, c->gsrc, proto == P_ICMP ? c->gport : c->gdport, c->gport);
    c->flags |= SBGW_F_REPLIED;
    c->last = now(gw);
    gw->in++;
    to_guest_ip(gw, f, total, 0);
    return 1;
}

/* ---- the gateway's own services: ARP, ICMP echo, DHCP --------------------------------------------------------- */

static void arp_rx(struct sbgw *gw, const uint8_t *fr, uint32_t len)
{
    const uint8_t *a = fr + 14;
    uint8_t f[60];
    uint8_t *r = f + 14;
    if (len < 14 + 28 || g16(a) != 1 || g16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4 || g16(a + 6) != 1)
        return;
    if (in_subnet(a + 14) && a[17] && a[17] != 255 && !eq(a + 14, gw_ip, 4) && !eq(a + 14, dns_ip, 4) &&
        !(a[8] & 1)) {
        cpy(gw->guest_mac, a + 8, 6);           /* the guest's address, as it says */
        gw->guest_known = 1;
    }
    if (!eq(a + 24, gw_ip, 4) && !eq(a + 24, dns_ip, 4))
        return;                                 /* only our two addresses: never the guest's (its own probes) */
    p16(r, 1), p16(r + 2, ETH_IP), r[4] = 6, r[5] = 4, p16(r + 6, 2);
    cpy(r + 8, gw->mac, 6), cpy(r + 14, a + 24, 4);
    cpy(r + 18, a + 8, 6), cpy(r + 24, a + 14, 4);
    cpy(f, a + 8, 6), cpy(f + 6, gw->mac, 6), p16(f + 12, ETH_ARP);
    set(f + 42, 0, 18);
    gw->arp_replies++;
    gw->guest_tx++;
    if (gw->os.to_guest(gw->os.ctx, f, 60))
        gw->dropped++;
}

static void echo_reply(struct sbgw *gw, const uint8_t *ip, uint32_t total)
{
    uint8_t f[14 + SBGW_MTU];
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, n = total - ihl;
    uint8_t *q = f + 14, *c = q + 20;
    if (n < 8 || ip[ihl] != 8 || ip[ihl + 1] != 0 || fold(sum16(ip + ihl, n, 0)) != 0)
        return;
    cpy(c, ip + ihl, n);
    c[0] = 0;
    adj16(c + 2, 0x0800, 0x0000);
    ip_hdr(gw, q, ip + 16, ip + 12, P_ICMP, 20 + n);
    gw->echo_replies++;
    to_guest_ip(gw, f, 20 + n, 0);
}

/* DHCP option `code` in the message's options: its value (and length), or 0 */
static const uint8_t *dhcp_opt(const uint8_t *m, uint32_t len, uint8_t code, uint8_t *olen)
{
    uint32_t o = 240;
    while (o < len) {
        uint8_t k = m[o];
        if (k == 0) {
            o++;
            continue;
        }
        if (k == 255 || o + 1 >= len || o + 2u + m[o + 1] > len)
            return 0;
        if (k == code) {
            *olen = m[o + 1];
            return m + o + 2;
        }
        o += 2u + m[o + 1];
    }
    return 0;
}

enum { DHCPDISCOVER = 1, DHCPOFFER, DHCPREQUEST, DHCPDECLINE, DHCPACK, DHCPNAK, DHCPRELEASE, DHCPINFORM };

static void dhcp_rx(struct sbgw *gw, const uint8_t *fr, const uint8_t *m, uint32_t len)
{
    uint8_t f[14 + 20 + 8 + 300];
    uint8_t *ip = f + 14, *u = ip + 20, *r = u + 8;
    const uint8_t *v, *dst, *dmac;
    uint8_t l = 0, type, reply;
    uint32_t o = 240;
    if (len < 240 || m[0] != 1 || m[1] != 1 || m[2] != 6 || g32(m + 236) != 0x63825363)
        return;
    v = dhcp_opt(m, len, 53, &l);
    if (!v || l != 1)
        return;
    type = v[0];
    if ((v = dhcp_opt(m, len, 54, &l)) && l == 4 && !eq(v, gw_ip, 4))
        return;                                 /* to another server */
    switch (type) {
    case DHCPDISCOVER:
        reply = DHCPOFFER;
        break;
    case DHCPREQUEST: {
        const uint8_t *want = m + 12;           /* ciaddr (renewing), else the requested address */
        if ((v = dhcp_opt(m, len, 50, &l)) && l == 4)
            want = v;
        reply = eq(want, guest_ip, 4) ? DHCPACK : DHCPNAK;
        break;
    }
    case DHCPINFORM:
        reply = DHCPACK;
        break;
    case DHCPRELEASE:
        gw->lease_at = 0;
        return;
    default:
        return;                                 /* DECLINE: nothing else to offer */
    }
    set(r, 0, 300);
    r[0] = 2, r[1] = 1, r[2] = 6;
    cpy(r + 4, m + 4, 4);                       /* xid */
    cpy(r + 10, m + 10, 2);                     /* flags */
    cpy(r + 12, m + 12, 4);                     /* ciaddr */
    if (reply != DHCPNAK && type != DHCPINFORM)
        cpy(r + 16, guest_ip, 4);               /* yiaddr */
    cpy(r + 24, m + 24, 4);                     /* giaddr */
    cpy(r + 28, m + 28, 16);                    /* chaddr */
    p32(r + 236, 0x63825363);
    r[o++] = 53, r[o++] = 1, r[o++] = reply;
    r[o++] = 54, r[o++] = 4, cpy(r + o, gw_ip, 4), o += 4;
    if (reply != DHCPNAK) {
        if (type != DHCPINFORM) {
            r[o++] = 51, r[o++] = 4, p32(r + o, 86400), o += 4;
            r[o++] = 58, r[o++] = 4, p32(r + o, 43200), o += 4;
            r[o++] = 59, r[o++] = 4, p32(r + o, 75600), o += 4;
        }
        r[o++] = 1, r[o++] = 4, cpy(r + o, mask24, 4), o += 4;
        r[o++] = 3, r[o++] = 4, cpy(r + o, gw_ip, 4), o += 4;
        r[o++] = 6, r[o++] = 4, cpy(r + o, dns_ip, 4), o += 4;
        r[o++] = 28, r[o++] = 4, cpy(r + o, net_bcast, 4), o += 4;
    }
    r[o++] = 255;
    /* RFC 2131 4.1: to ciaddr when the client has one; else broadcast (or to its hardware address) */
    if (reply != DHCPNAK && !zero4(m + 12)) {
        dst = m + 12, dmac = fr + 6;
    } else {
        dst = all_ones;
        dmac = (reply == DHCPNAK || (m[10] & 0x80)) ? bcast_mac : m + 28;
    }
    p16(u, 67), p16(u + 2, 68), p16(u + 4, 8 + 300), p16(u + 6, 0);
    ip_hdr(gw, ip, gw_ip, dst, P_UDP, 20 + 8 + 300);
    p16(u + 6, l4_sum(ip, P_UDP, u, 8 + 300));
    if (reply == DHCPACK && type != DHCPINFORM) {
        cpy(gw->guest_mac, m + 28, 6);          /* the guest: remembered */
        gw->guest_known = 1;
        gw->lease_at = now(gw) | 1;
        gw->dhcp_acks++;
        say(gw, "gw: DHCP: the guest has its address");
    }
    to_guest_ip(gw, f, 20 + 8 + 300, dmac);
}

/* ---- the guest's frames ---------------------------------------------------------------------------------------- */

void sbgw_guest_rx(struct sbgw *gw, const uint8_t *fr, uint32_t len)
{
    const uint8_t *ip = fr + 14, *dst;
    uint32_t ihl, total;
    uint8_t proto;
    gw->guest_rx++;
    if (len < 14 || (fr[6] & 1) || (!(fr[0] & 1) && !eq(fr, gw->mac, 6))) {
        gw->dropped++;
        return;
    }
    if (g16(fr + 12) == ETH_ARP) {
        arp_rx(gw, fr, len);
        return;
    }
    if (g16(fr + 12) != ETH_IP || len < 34) {
        gw->dropped++;                          /* IPv6 and the rest: not here */
        return;
    }
    ihl = (uint32_t)(ip[0] & 15) * 4;
    total = g16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len - 14 || total > SBGW_MTU ||
        fold(sum16(ip, ihl, 0)) != 0) {
        gw->dropped++;
        return;
    }
    proto = ip[9], dst = ip + 16;
    if (g16(ip + 6) & 0x3fff) {
        gw->frags++;                            /* fragments: not reassembled */
        return;
    }
    if (proto == P_UDP && total - ihl >= 8 && g16(ip + ihl + 2) == 67 && g16(ip + ihl) == 68) {
        uint32_t ulen = g16(ip + ihl + 4);
        if (ulen >= 8 && ulen <= total - ihl)
            dhcp_rx(gw, fr, ip + ihl + 8, ulen - 8);
        return;
    }
    /* from here on only the guest's subnet addresses as sources (not ours, not the network's or the broadcast) */
    if (!in_subnet(ip + 12) || ip[15] == 0 || ip[15] == 255 || eq(ip + 12, gw_ip, 4) || eq(ip + 12, dns_ip, 4)) {
        gw->dropped++;
        return;
    }
    if (!(fr[0] & 1) && (!gw->guest_known || !eq(gw->guest_mac, fr + 6, 6))) {
        cpy(gw->guest_mac, fr + 6, 6);          /* a static guest (no DHCP) or a new NIC */
        gw->guest_known = 1;
    }
    if (fr[0] & 1 || dst[0] >= 224 || eq(dst, all_ones, 4) || zero4(dst) || dst[0] == 127 || dst[0] == 0) {
        gw->dropped++;
        return;
    }
    if (eq(dst, gw_ip, 4) || eq(dst, dns_ip, 4)) {
        const uint8_t *l4 = ip + ihl;
        uint32_t l4len = total - ihl;
        if (proto == P_ICMP) {
            echo_reply(gw, ip, total);
        } else if (eq(dst, dns_ip, 4) && (proto == P_UDP || proto == P_TCP) && l4len >= 4 && g16(l4 + 2) == 53) {
            nat_out(gw, ip, total, gw->up && uplink_ok(gw) ? gw->up->dns : dns_ip);
        } else if (proto == P_TCP && l4len >= 20 && (uint32_t)(l4[12] >> 4) * 4 >= 20 &&
                   (uint32_t)(l4[12] >> 4) * 4 <= l4len) {
            rst_reply(gw, ip, l4, l4len);       /* no services on the gateway: refused */
        } else if (proto == P_UDP) {
            icmp_error(gw, 3, 3, 0, dst, ip, total);
        }
        return;
    }
    if (in_subnet(dst)) {
        gw->dropped++;                          /* nobody else on the guest's link */
        return;
    }
    nat_out(gw, ip, total, dst);
}

/* ---- the rest ---------------------------------------------------------------------------------------------------- */

void sbgw_set_uplink(struct sbgw *gw, struct sbnet *up)
{
    if (up == gw->up) {
        if (up) {
            up->ip_hook = hook;
            up->hook_ctx = gw;
        }
        return;
    }
    if (gw->up && gw->up->hook_ctx == gw) {
        gw->up->ip_hook = 0;
        gw->up->hook_ctx = 0;
    }
    flush(gw);
    gw->up = up;
    set(gw->up_ip, 0, 4);
    if (up) {
        up->ip_hook = hook;
        up->hook_ctx = gw;
        if (up->state >= SBNET_BOUND)
            cpy(gw->up_ip, up->ip, 4);
        say(gw, up->raw_ip ? "gw: the uplink is mobile data" : "gw: the uplink is Wi-Fi");
    } else {
        say(gw, "gw: no uplink");
    }
}

void sbgw_tick(struct sbgw *gw)
{
    uint64_t t = now(gw);
    if (gw->up) {
        if (gw->up->ip_hook != hook || gw->up->hook_ctx != gw) {
            gw->up->ip_hook = hook;             /* sbnet_init cleared it (the link came back) */
            gw->up->hook_ctx = gw;
        }
        if (!uplink_ok(gw) && !zero4(gw->up_ip)) {
            flush(gw);                          /* the uplink lost its address: what was mapped is gone */
            set(gw->up_ip, 0, 4);
        }
    }
    for (unsigned i = 0; i < SBGW_CONNS; i++) {
        struct sbgw_conn *c = &gw->c[i];
        if (c->proto && t - c->last > timeout(c))
            c->proto = 0;
    }
}

unsigned sbgw_conns(const struct sbgw *gw)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SBGW_CONNS; i++)
        n += gw->c[i].proto != 0;
    return n;
}

void sbgw_init(struct sbgw *gw, const struct sbgw_os *os)
{
    static const uint8_t mac[6] = { 0x52, 0x55, 10, 0, 2, 2 };
    set((uint8_t *)gw, 0, sizeof(*gw));
    gw->os = *os;
    cpy(gw->mac, mac, 6);
    gw->ip_id = (uint16_t)os->random(os->ctx);
}
