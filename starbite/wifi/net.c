/* SPDX-License-Identifier: MIT
 * sbnet: a small IPv4 layer for the bare-metal Wi-Fi core (net.h). Written for Starbite; RFC 791 (IPv4), 826 (ARP),
 * 792 (ICMP), 768 (UDP), 2131/2132 (DHCP), 1035 (DNS). Every received byte is bounds-checked: frames come from the
 * air. */
#include "net.h"

#define ETH_ARP 0x0806
#define ETH_IP 0x0800
#define IP_ICMP 1
#define IP_UDP 17
#define IP_TCP 6
#define DHCP_SERVER 67
#define DHCP_CLIENT 68
#define DNS_PORT 53

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

static const uint8_t bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t bcast_ip[4] = { 255, 255, 255, 255 };
static const uint8_t any_ip[4] = { 0, 0, 0, 0 };
static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

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

static void say(struct sbnet *n, const char *t)
{
    if (n->os.log)
        n->os.log(n->os.ctx, t);
}
static uint64_t now(struct sbnet *n) { return n->os.now_ms(n->os.ctx); }

/* ---- frames out ---------------------------------------------------------------------------------------------- */

static void eth_send(struct sbnet *n, const uint8_t *dst, uint16_t type, uint8_t *f, uint32_t len)
{
    cpy(f, dst, 6);
    cpy(f + 6, n->mac, 6);
    p16(f + 12, type);
    if (len < 60) {                             /* the minimum frame */
        set(f + len, 0, 60 - len);
        len = 60;
    }
    n->tx_frames++;
    if (n->os.tx(n->os.ctx, f, len))
        n->tx_errors++;
}

static void arp_send(struct sbnet *n, uint16_t op, const uint8_t *tha, const uint8_t *tpa, const uint8_t *dst)
{
    uint8_t f[60];
    uint8_t *a = f + 14;
    p16(a, 1), p16(a + 2, ETH_IP), a[4] = 6, a[5] = 4, p16(a + 6, op);
    cpy(a + 8, n->mac, 6);
    cpy(a + 14, n->state >= SBNET_BOUND ? n->ip : any_ip, 4);
    cpy(a + 18, tha, 6);
    cpy(a + 24, tpa, 4);
    eth_send(n, dst, ETH_ARP, f, 14 + 28);
}

static const uint8_t *arp_find(struct sbnet *n, const uint8_t *ip)
{
    for (unsigned i = 0; i < SBNET_ARP_ENTRIES; i++)
        if (n->arp[i].used && eq(n->arp[i].ip, ip, 4))
            return n->arp[i].mac;
    return 0;
}

static void arp_learn(struct sbnet *n, const uint8_t *ip, const uint8_t *mac)
{
    unsigned k = 0;
    uint64_t oldest = ~0ull;
    if (zero4(ip) || (mac[0] & 1))
        return;
    for (unsigned i = 0; i < SBNET_ARP_ENTRIES; i++) {
        if (n->arp[i].used && eq(n->arp[i].ip, ip, 4)) {
            k = i;
            break;
        }
        if (!n->arp[i].used) {
            k = i, oldest = 0;
        } else if (oldest && n->arp[i].at < oldest) {
            k = i, oldest = n->arp[i].at;
        }
    }
    cpy(n->arp[k].ip, ip, 4);
    cpy(n->arp[k].mac, mac, 6);
    n->arp[k].at = now(n);
    n->arp[k].used = 1;
}

/* the next hop for dst: dst itself on our subnet, else the gateway */
static void next_hop(struct sbnet *n, const uint8_t *dst, uint8_t *hop)
{
    int local = 1;
    for (unsigned i = 0; i < 4; i++)
        if ((dst[i] & n->mask[i]) != (n->ip[i] & n->mask[i]))
            local = 0;
    cpy(hop, local || zero4(n->gateway) ? dst : n->gateway, 4);
}

/* an IPv4 packet out: f has room for the Ethernet header in front; payload len bytes at f + 34 */
static int ip_send(struct sbnet *n, const uint8_t *dst, uint8_t proto, uint8_t *f, uint32_t plen, int broadcast)
{
    uint8_t *ip = f + 14, hop[4];
    const uint8_t *mac;
    uint32_t total = 20 + plen;
    if (total > 1500)
        return -1;
    ip[0] = 0x45, ip[1] = 0;
    p16(ip + 2, (uint16_t)total);
    p16(ip + 4, (uint16_t)n->ip_id++);
    p16(ip + 6, 0x4000);                        /* don't fragment */
    ip[8] = 64, ip[9] = proto;
    p16(ip + 10, 0);
    cpy(ip + 12, broadcast && n->state < SBNET_BOUND ? any_ip : n->ip, 4);
    cpy(ip + 16, dst, 4);
    p16(ip + 10, fold(sum16(ip, 20, 0)));
    if (broadcast || eq(dst, bcast_ip, 4)) {
        eth_send(n, bcast_mac, ETH_IP, f, 14 + total);
        return 0;
    }
    next_hop(n, dst, hop);
    mac = arp_find(n, hop);
    if (mac) {
        eth_send(n, mac, ETH_IP, f, 14 + total);
        return 0;
    }
    /* held until the next hop answers (one packet; a newer one replaces it) */
    if (14 + total > SBNET_HELD_MAX)
        return -1;
    cpy(f + 6, n->mac, 6);                      /* the header but its destination, filled when the hop answers */
    p16(f + 12, ETH_IP);
    cpy(n->held, f, 14 + total);
    n->held_len = 14 + total;
    if (n->held_len < 60) {
        set(n->held + n->held_len, 0, 60 - n->held_len);
        n->held_len = 60;
    }
    cpy(n->held_hop, hop, 4);
    n->held_since = now(n);
    n->arp_tries = 1;
    n->arp_next = now(n) + 1000;
    arp_send(n, 1, zero_mac, hop, bcast_mac);
    return 0;
}

static int udp_out(struct sbnet *n, const uint8_t *dst, uint16_t dport, uint16_t sport, const uint8_t *data, uint32_t len,
                   int broadcast)
{
    uint8_t f[14 + 20 + 8 + 1472];
    uint8_t *u = f + 34;
    uint32_t s;
    const uint8_t *src = broadcast && n->state < SBNET_BOUND ? any_ip : n->ip;
    if (len > 1472)
        return -1;
    p16(u, sport), p16(u + 2, dport), p16(u + 4, (uint16_t)(8 + len)), p16(u + 6, 0);
    cpy(u + 8, data, len);
    s = sum16(src, 4, 0);
    s = sum16(dst, 4, s);
    s += IP_UDP + 8 + len;
    s = sum16(u, 8 + len, s);
    s = fold(s);
    p16(u + 6, s ? (uint16_t)s : 0xffff);
    return ip_send(n, dst, IP_UDP, f, 8 + len, broadcast);
}

/* ---- DHCP --------------------------------------------------------------------------------------------------- */

enum { DHCPDISCOVER = 1, DHCPOFFER, DHCPREQUEST, DHCPDECLINE, DHCPACK, DHCPNAK };

static void dhcp_send(struct sbnet *n, int type)
{
    uint8_t m[300];
    uint32_t o = 240;
    int renew = n->state == SBNET_RENEWING || n->state == SBNET_REBINDING;
    set(m, 0, sizeof(m));
    m[0] = 1, m[1] = 1, m[2] = 6;
    p32(m + 4, n->xid);
    p16(m + 10, renew ? 0 : 0x8000);            /* broadcast answers until we have an address */
    if (renew)
        cpy(m + 12, n->ip, 4);                  /* ciaddr */
    cpy(m + 28, n->mac, 6);
    m[236] = 99, m[237] = 130, m[238] = 83, m[239] = 99;
    m[o++] = 53, m[o++] = 1, m[o++] = (uint8_t)type;
    m[o++] = 61, m[o++] = 7, m[o++] = 1;        /* client id: the address */
    cpy(m + o, n->mac, 6), o += 6;
    if (type == DHCPREQUEST && n->state == SBNET_REQUESTING) {
        m[o++] = 50, m[o++] = 4, cpy(m + o, n->offered, 4), o += 4;
        m[o++] = 54, m[o++] = 4, cpy(m + o, n->server, 4), o += 4;
    }
    m[o++] = 55, m[o++] = 6;                    /* asked for: mask, router, DNS, lease, T1, T2 */
    m[o++] = 1, m[o++] = 3, m[o++] = 6, m[o++] = 51, m[o++] = 58, m[o++] = 59;
    m[o++] = 255;
    if (n->state == SBNET_RENEWING)
        udp_out(n, n->server, DHCP_SERVER, DHCP_CLIENT, m, o < 300 ? 300 : o, 0);
    else
        udp_out(n, bcast_ip, DHCP_SERVER, DHCP_CLIENT, m, o < 300 ? 300 : o, 1);
}

static void dhcp_discover(struct sbnet *n)
{
    n->state = SBNET_DISCOVERING;
    n->xid = n->os.random(n->os.ctx);
    n->tries = 0;
    set(n->ip, 0, 4);
    n->next_send = 0;                           /* at the next tick */
}

/* option `code` among options [o, end) of message m: its value (and length), or 0 */
static const uint8_t *dhcp_opt(const uint8_t *m, uint32_t len, uint8_t code, uint8_t *olen)
{
    uint32_t o = 240;
    while (o < len) {
        uint8_t c = m[o];
        if (c == 0) {
            o++;
            continue;
        }
        if (c == 255 || o + 1 >= len)
            return 0;
        if (o + 2u + m[o + 1] > len)
            return 0;
        if (c == code) {
            *olen = m[o + 1];
            return m + o + 2;
        }
        o += 2u + m[o + 1];
    }
    return 0;
}

static void dhcp_rx(struct sbnet *n, const uint8_t *m, uint32_t len)
{
    const uint8_t *v;
    uint8_t l = 0;
    int type;
    if (len < 241 || m[0] != 2 || g32(m + 4) != n->xid || !eq(m + 28, n->mac, 6) ||
        m[236] != 99 || m[237] != 130 || m[238] != 83 || m[239] != 99)
        return;
    v = dhcp_opt(m, len, 53, &l);
    if (!v || l != 1)
        return;
    type = v[0];
    if (type == DHCPOFFER && n->state == SBNET_DISCOVERING) {
        v = dhcp_opt(m, len, 54, &l);
        if (!v || l != 4 || zero4(m + 16))
            return;
        cpy(n->offered, m + 16, 4);
        cpy(n->server, v, 4);
        n->state = SBNET_REQUESTING;
        n->tries = 0;
        n->next_send = 0;
        say(n, "net: DHCP: an offer; asking for it");
        return;
    }
    if (type == DHCPNAK && n->state != SBNET_DISCOVERING && n->state != SBNET_DOWN) {
        say(n, "net: DHCP: refused (NAK): starting again");
        dhcp_discover(n);
        return;
    }
    if (type == DHCPACK && (n->state == SBNET_REQUESTING || n->state == SBNET_RENEWING || n->state == SBNET_REBINDING)) {
        uint32_t lease = 3600, t1, t2;
        uint64_t t = now(n);
        if (zero4(m + 16))
            return;
        cpy(n->ip, m + 16, 4);
        set(n->mask, 0, 4), set(n->gateway, 0, 4), set(n->dns, 0, 4);
        if ((v = dhcp_opt(m, len, 1, &l)) && l == 4)
            cpy(n->mask, v, 4);
        else
            n->mask[0] = n->mask[1] = n->mask[2] = 255;
        if ((v = dhcp_opt(m, len, 3, &l)) && l >= 4)
            cpy(n->gateway, v, 4);
        if ((v = dhcp_opt(m, len, 6, &l)) && l >= 4)
            cpy(n->dns, v, 4);
        if ((v = dhcp_opt(m, len, 54, &l)) && l == 4)
            cpy(n->server, v, 4);
        if ((v = dhcp_opt(m, len, 51, &l)) && l == 4)
            lease = g32(v);
        if (lease < 60)
            lease = 60;
        if (lease > 0x7fffffff / 1000)
            lease = 0x7fffffff / 1000;
        t1 = lease / 2, t2 = lease / 8 * 7;
        if ((v = dhcp_opt(m, len, 58, &l)) && l == 4 && g32(v) < lease)
            t1 = g32(v);
        if ((v = dhcp_opt(m, len, 59, &l)) && l == 4 && g32(v) < lease && g32(v) > t1)
            t2 = g32(v);
        say(n, n->state == SBNET_REQUESTING ? "net: DHCP: bound (an address, a router and DNS; not logged)"
                                            : "net: DHCP: the lease renewed");
        n->state = SBNET_BOUND;
        n->lease_s = lease;
        n->bound_at = t;
        n->t1_at = t + (uint64_t)t1 * 1000;
        n->t2_at = t + (uint64_t)t2 * 1000;
        n->end_at = t + (uint64_t)lease * 1000;
        n->tries = 0;
        /* the server answered us; the router learns our address from its first packet to it */
    }
}

/* ---- DNS ------------------------------------------------------------------------------------------------------ */

/* skips a name at m[o] (labels or a compression pointer); the offset after it, or 0 */
static uint32_t dns_skip(const uint8_t *m, uint32_t len, uint32_t o)
{
    for (unsigned guard = 0; o < len && guard < 128; guard++) {
        uint8_t c = m[o];
        if (c == 0)
            return o + 1;
        if ((c & 0xc0) == 0xc0)
            return o + 2 <= len ? o + 2 : 0;
        if (c & 0xc0)
            return 0;
        o += 1u + c;
    }
    return 0;
}

static void dns_rx(struct sbnet *n, const uint8_t *m, uint32_t len)
{
    uint32_t o, an;
    if (n->dns_state != SBNET_DNS_WAITING || len < 12 || g16(m) != n->dns_id || !(m[2] & 0x80))
        return;
    if ((m[3] & 0x0f) != 0 || g16(m + 4) != 1) {
        n->dns_state = SBNET_DNS_FAILED;
        say(n, "net: DNS: no such name (or an error)");
        return;
    }
    an = g16(m + 6);
    o = dns_skip(m, len, 12);
    if (!o || o + 4 > len)
        return;
    o += 4;
    for (uint32_t i = 0; i < an && i < 32; i++) {
        uint32_t rd;
        o = dns_skip(m, len, o);
        if (!o || o + 10 > len)
            break;
        rd = g16(m + o + 8);
        if (o + 10 + rd > len)
            break;
        if (g16(m + o) == 1 && g16(m + o + 2) == 1 && rd == 4) {
            cpy(n->dns_result, m + o + 10, 4);
            n->dns_state = SBNET_DNS_DONE;
            say(n, "net: DNS: an answer (not logged)");
            return;
        }
        o += 10 + rd;
    }
    n->dns_state = SBNET_DNS_FAILED;
    say(n, "net: DNS: no address in the answer");
}

int sbnet_dns_query(struct sbnet *n, const char *name)
{
    uint8_t m[300];
    uint32_t o = 12, start;
    if (n->state < SBNET_BOUND || zero4(n->dns))
        return -1;
    set(m, 0, 12);
    n->dns_id = (uint16_t)n->os.random(n->os.ctx);
    n->dns_port = (uint16_t)(49152 + n->os.random(n->os.ctx) % 16000);
    p16(m, n->dns_id);
    m[2] = 0x01;                                /* recursion desired */
    p16(m + 4, 1);
    for (const char *p = name; *p;) {
        start = o++;
        while (*p && *p != '.') {
            if (o >= 12 + 254)
                return -1;
            m[o++] = (uint8_t)*p++;
        }
        if (o - start - 1 == 0 || o - start - 1 > 63)
            return -1;
        m[start] = (uint8_t)(o - start - 1);
        if (*p == '.')
            p++;
    }
    if (o == 12)
        return -1;
    m[o++] = 0;
    p16(m + o, 1), p16(m + o + 2, 1), o += 4;   /* A, IN */
    n->dns_state = SBNET_DNS_WAITING;
    n->dns_deadline = now(n) + 5000;
    return udp_out(n, n->dns, DNS_PORT, n->dns_port, m, o, 0);
}

/* ---- frames in ------------------------------------------------------------------------------------------------ */

static void arp_rx(struct sbnet *n, const uint8_t *a, uint32_t len)
{
    if (len < 28 || g16(a) != 1 || g16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
        return;
    if (n->state >= SBNET_BOUND && eq(a + 24, n->ip, 4)) {
        arp_learn(n, a + 14, a + 8);
        if (g16(a + 6) == 1)
            arp_send(n, 2, a + 8, a + 14, a + 8);
    } else if (arp_find(n, a + 14)) {
        arp_learn(n, a + 14, a + 8);           /* refresh what we already use */
    }
    if (g16(a + 6) == 2 && n->held_len && eq(a + 14, n->held_hop, 4)) {
        arp_learn(n, a + 14, a + 8);
        cpy(n->held, a + 8, 6);                 /* the waiting packet's destination */
        n->tx_frames++;
        if (n->os.tx(n->os.ctx, n->held, n->held_len))
            n->tx_errors++;
        n->held_len = 0;
    }
}

static void icmp_rx(struct sbnet *n, const uint8_t *ip, uint32_t ihl, const uint8_t *c, uint32_t len)
{
    if (len < 8 || fold(sum16(c, len, 0)) != 0)
        return;
    if (c[0] == 8 && c[1] == 0 && n->state >= SBNET_BOUND && eq(ip + 16, n->ip, 4) && len <= 1472) {
        uint8_t f[14 + 20 + 1480];
        uint8_t *r = f + 34;
        cpy(r, c, len);
        r[0] = 0, p16(r + 2, 0);
        p16(r + 2, fold(sum16(r, len, 0)));
        ip_send(n, ip + 12, IP_ICMP, f, len, 0);
        return;
    }
    if (c[0] == 0 && c[1] == 0 && g16(c + 4) == n->ping_id) {
        n->ping_replies++;
        n->ping_rtt_ms = now(n) - n->ping_at;
    }
    (void)ihl;
}

void sbnet_rx(struct sbnet *n, const uint8_t *f, uint32_t len)
{
    const uint8_t *ip, *u;
    uint32_t ihl, total;
    n->rx_frames++;
    if (len < 14 || n->state == SBNET_DOWN || (!(f[0] & 1) && !eq(f, n->mac, 6))) {
        n->rx_ignored++;
        return;
    }
    if (g16(f + 12) == ETH_ARP) {
        arp_rx(n, f + 14, len - 14);
        return;
    }
    if (g16(f + 12) != ETH_IP || len < 34) {
        n->rx_ignored++;
        return;
    }
    ip = f + 14;
    ihl = (uint32_t)(ip[0] & 15) * 4;
    total = g16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || 14 + total > len || fold(sum16(ip, ihl, 0)) != 0 ||
        (g16(ip + 6) & 0x3fff)) {               /* no fragments */
        n->rx_ignored++;
        return;
    }
    if (n->state >= SBNET_BOUND && !eq(ip + 16, n->ip, 4) && !eq(ip + 16, bcast_ip, 4) && !(ip[16] >= 224)) {
        int directed = 1;                       /* the subnet's broadcast */
        for (unsigned i = 0; i < 4; i++)
            if ((ip[16 + i] | n->mask[i]) != 255)
                directed = 0;
        if (!directed) {
            n->rx_ignored++;
            return;
        }
    }
    if (ip[9] == IP_ICMP) {
        icmp_rx(n, ip, ihl, ip + ihl, total - ihl);
        return;
    }
    if (ip[9] == IP_TCP) {                      /* to sbtcp (tcp.c), only what is addressed to us */
        if (n->os.tcp && n->state >= SBNET_BOUND && eq(ip + 16, n->ip, 4))
            n->os.tcp(n->os.ctx, ip + 12, ip + ihl, total - ihl);
        else
            n->rx_ignored++;
        return;
    }
    if (ip[9] != IP_UDP || total - ihl < 8) {
        n->rx_ignored++;
        return;
    }
    u = ip + ihl;
    {
        uint32_t ulen = g16(u + 4);
        uint16_t dport = g16(u + 2), sport = g16(u);
        if (ulen < 8 || ulen > total - ihl) {
            n->rx_ignored++;
            return;
        }
        if (g16(u + 6)) {                       /* checksum present: checked */
            uint32_t s = sum16(ip + 12, 8, 0) + IP_UDP + ulen;
            if (fold(sum16(u, ulen, s)) != 0) {
                n->rx_ignored++;
                return;
            }
        }
        if (dport == DHCP_CLIENT && sport == DHCP_SERVER)
            dhcp_rx(n, u + 8, ulen - 8);
        else if (sport == DNS_PORT && dport == n->dns_port)
            dns_rx(n, u + 8, ulen - 8);
        else if (n->os.udp && n->state >= SBNET_BOUND)
            n->os.udp(n->os.ctx, dport, ip + 12, sport, u + 8, ulen - 8);
    }
}

/* ---- the public calls ------------------------------------------------------------------------------------- */

int sbnet_ip_send(struct sbnet *n, const uint8_t dst[4], uint8_t proto, uint8_t *f, uint32_t plen)
{
    if (n->state < SBNET_BOUND || zero4(dst) || eq(dst, bcast_ip, 4))
        return -1;
    return ip_send(n, dst, proto, f, plen, 0);
}

void sbnet_static(struct sbnet *n, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gateway[4],
                  const uint8_t dns[4])
{
    set((uint8_t *)n->arp, 0, sizeof(n->arp));
    n->held_len = 0;
    n->dns_state = SBNET_DNS_IDLE;
    cpy(n->ip, ip, 4), cpy(n->mask, mask, 4), cpy(n->gateway, gateway, 4), cpy(n->dns, dns, 4);
    set(n->server, 0, 4);
    n->lease_s = 0;
    n->bound_at = now(n);
    n->t1_at = n->t2_at = n->end_at = ~0ull;    /* no lease: never renewed, never expires */
    n->state = SBNET_BOUND;
}

void sbnet_init(struct sbnet *n, const struct sbnet_os *os, const uint8_t mac[6])
{
    set((uint8_t *)n, 0, sizeof(*n));
    n->os = *os;
    cpy(n->mac, mac, 6);
    n->state = SBNET_DOWN;
    n->ip_id = os->random(os->ctx);
    n->ping_id = (uint16_t)os->random(os->ctx);
}

void sbnet_start(struct sbnet *n)
{
    set((uint8_t *)n->arp, 0, sizeof(n->arp));
    n->held_len = 0;
    n->dns_state = SBNET_DNS_IDLE;
    say(n, "net: DHCP: asking for an address");
    dhcp_discover(n);
    sbnet_tick(n);
}

void sbnet_stop(struct sbnet *n)
{
    n->state = SBNET_DOWN;
    set(n->ip, 0, 4), set(n->gateway, 0, 4), set(n->dns, 0, 4);
    n->held_len = 0;
}

void sbnet_tick(struct sbnet *n)
{
    uint64_t t = now(n);
    switch (n->state) {
    case SBNET_DISCOVERING:
    case SBNET_REQUESTING:
        if (t >= n->next_send) {
            unsigned wait = 4u << (n->tries < 3 ? n->tries : 3);   /* 4, 8, 16, 32 s */
            if (n->state == SBNET_REQUESTING && n->tries >= 4) {
                say(n, "net: DHCP: no answer to the request: starting again");
                dhcp_discover(n);
                return;
            }
            dhcp_send(n, n->state == SBNET_DISCOVERING ? DHCPDISCOVER : DHCPREQUEST);
            n->tries++;
            n->next_send = t + (uint64_t)wait * 1000;
        }
        break;
    case SBNET_BOUND:
        if (t >= n->t1_at) {
            n->state = SBNET_RENEWING;
            n->xid = n->os.random(n->os.ctx);
            n->next_send = 0;
        }
        break;
    default:
        break;
    }
    if (n->state == SBNET_RENEWING || n->state == SBNET_REBINDING) {
        if (t >= n->end_at) {
            say(n, "net: DHCP: the lease ran out: starting again");
            set((uint8_t *)n->arp, 0, sizeof(n->arp));
            dhcp_discover(n);
            return;
        }
        if (n->state == SBNET_RENEWING && t >= n->t2_at) {
            n->state = SBNET_REBINDING;
            n->next_send = 0;
        }
        if (t >= n->next_send) {
            dhcp_send(n, DHCPREQUEST);
            n->next_send = t + 10000;           /* every 10 s until an answer or the next stage */
        }
    }
    if (n->held_len && t >= n->arp_next) {
        if (n->arp_tries >= 3) {
            n->held_len = 0;                    /* nobody answers: dropped */
        } else {
            n->arp_tries++;
            n->arp_next = t + 1000;
            arp_send(n, 1, zero_mac, n->held_hop, bcast_mac);
        }
    }
    if (n->dns_state == SBNET_DNS_WAITING && t >= n->dns_deadline) {
        n->dns_state = SBNET_DNS_FAILED;
        say(n, "net: DNS: no answer in 5 s");
    }
}

int sbnet_udp_send(struct sbnet *n, const uint8_t ip[4], uint16_t port, uint16_t sport, const uint8_t *data, uint32_t len)
{
    if (n->state < SBNET_BOUND)
        return -1;
    return udp_out(n, ip, port, sport, data, len, 0);
}

int sbnet_ping(struct sbnet *n, const uint8_t ip[4])
{
    uint8_t f[14 + 20 + 40];
    uint8_t *c = f + 34;
    if (n->state < SBNET_BOUND)
        return -1;
    c[0] = 8, c[1] = 0, p16(c + 2, 0);
    p16(c + 4, n->ping_id);
    p16(c + 6, (uint16_t)++n->ping_sent);
    for (unsigned i = 8; i < 40; i++)
        c[i] = (uint8_t)('a' + i % 26);
    p16(c + 2, fold(sum16(c, 40, 0)));
    n->ping_at = now(n);
    return ip_send(n, ip, IP_ICMP, f, 40, 0);
}
