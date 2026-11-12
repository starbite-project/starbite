/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbnat: IPv4 NAT for the Starbite OS Linux guest. See sbnat.h.
 *
 * Mappings: one table entry per flow (proto, guest port, remote address, remote port, DNS-redirected or not).
 * The external port is endpoint-independent: every flow of one (proto, guest port, DNS flag) group shares
 * one external port, and different groups never share one, so (proto, external port, remote address,
 * remote port) finds exactly one entry for packets from the uplink. Packets from remotes the guest has not
 * sent to are not translated (address-and-port-dependent filtering): they go to the host's stack.
 * All multi-byte fields are read and written bytewise (no alignment assumptions: -mstrict-align).
 */
#include "sbnat.h"

#define P_ICMP 1
#define P_TCP  6
#define P_UDP  17

/* sbnat_map.flags */
#define F_DNAT   0x01   /* guest sent to 10.77.0.1:53; the remote is the uplink DNS server */
#define F_IN     0x02   /* traffic seen from the remote */

/* sbnat_map.tcp */
#define T_SYNACK 0x01   /* SYN+ACK from the remote */
#define T_ACKOUT 0x02   /* a non-SYN segment from the guest */
#define T_ACKIN  0x04   /* a non-SYN segment from the remote */
#define T_FINOUT 0x08
#define T_FININ  0x10
#define T_RST    0x20

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_ACK 0x10

static const uint8_t guest_addr[4] = SBNAT_GUEST_ADDR;
static const uint8_t gw_addr[4] = SBNAT_GW_ADDR;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int eq4(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }
static void cp4(uint8_t *d, const uint8_t *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
static int zero4(const uint8_t *a) { return !(a[0] | a[1] | a[2] | a[3]); }

/* ---- checksums ---- */

static uint32_t sum16(const uint8_t *p, uint32_t len)
{
    uint32_t s = 0;
    while (len > 1) { s += rd16(p); p += 2; len -= 2; }
    if (len)
        s += (uint32_t)p[0] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)s;
}

/* RFC 1624 eqn 3: HC' = ~(~HC + ~m + m') for one 16-bit word m -> m' */
static void ck16(uint8_t *ck, uint16_t o, uint16_t nw)
{
    uint32_t s = (uint32_t)(uint16_t)~rd16(ck) + (uint16_t)~o + nw;
    wr16(ck, (uint16_t)~fold(s));
}

/* an L4 checksum: UDP 0 means "none" and stays 0; a computed 0 is sent as 0xffff */
static void l4ck16(uint8_t *ck, int udp, uint16_t o, uint16_t nw)
{
    if (!ck || o == nw)
        return;
    if (udp && rd16(ck) == 0)
        return;
    ck16(ck, o, nw);
    if (udp && rd16(ck) == 0)
        wr16(ck, 0xffff);
}

/* rewrite a 4-byte address in an IP header, fixing the header checksum and the L4 one (pseudo-header) */
static void set_addr(uint8_t *f, const uint8_t *nw, uint8_t *ipck, uint8_t *l4ck, int udp)
{
    uint16_t o0 = rd16(f), o1 = rd16(f + 2);
    cp4(f, nw);
    ck16(ipck, o0, rd16(f));
    ck16(ipck, o1, rd16(f + 2));
    l4ck16(l4ck, udp, o0, rd16(f));
    l4ck16(l4ck, udp, o1, rd16(f + 2));
}

static void set_port(uint8_t *f, uint16_t nw, uint8_t *l4ck, int udp)
{
    uint16_t o = rd16(f);
    wr16(f, nw);
    l4ck16(l4ck, udp, o, nw);
}

static void dec_ttl(uint8_t *ip)
{
    uint16_t o = rd16(ip + 8);
    ip[8]--;
    ck16(ip + 10, o, rd16(ip + 8));
}

/* the IPv4 header: version, lengths, checksum. Returns the header length or 0; *tot = total length */
static uint32_t ip_ok(struct sbnat *n, const uint8_t *p, uint32_t len, uint32_t *tot)
{
    uint32_t hl;
    if (len < 20 || (p[0] >> 4) != 4) {
        n->st.drop_hdr++;
        return 0;
    }
    hl = (uint32_t)(p[0] & 15) * 4;
    *tot = rd16(p + 2);
    if (hl < 20 || *tot < hl || *tot > len) {
        n->st.drop_hdr++;
        return 0;
    }
    if (fold(sum16(p, hl)) != 0xffff) {
        n->st.drop_csum++;
        return 0;
    }
    return hl;
}

/* ---- the table ---- */

static uint32_t timeout_ms(const struct sbnat_map *m)
{
    if (m->proto == P_TCP) {
        if ((m->tcp & T_RST) || (m->tcp & (T_FINOUT | T_FININ)) == (T_FINOUT | T_FININ))
            return SBNAT_TCP_CLOSE_MS;
        if ((m->tcp & T_ACKOUT) && (m->tcp & (T_SYNACK | T_ACKIN)))
            return SBNAT_TCP_EST_MS;
        return SBNAT_TCP_SYN_MS;
    }
    if (m->proto == P_UDP)
        return (m->flags & F_IN) ? SBNAT_UDP_BOTH_MS : SBNAT_UDP_MS;
    return SBNAT_ICMP_MS;
}

static void drop_entry(struct sbnat *n, struct sbnat_map *m)
{
    m->proto = 0;
    n->used--;
}

/* is the entry in use at now? frees it (and counts it) if it has timed out */
static int live(struct sbnat *n, struct sbnat_map *m, uint64_t now)
{
    if (!m->proto)
        return 0;
    if (now > m->last_ms && now - m->last_ms >= timeout_ms(m)) {
        drop_entry(n, m);
        n->st.expired++;
        return 0;
    }
    return 1;
}

static int in_range(const struct sbnat *n, uint16_t port) { return port >= n->port_lo && port <= n->port_hi; }

/* the guest side of a flow; *group = the external port of the flow's group, or 0 */
static struct sbnat_map *find_out(struct sbnat *n, uint8_t proto, uint16_t gport, const uint8_t *raddr,
                                  uint16_t rport, uint8_t dnat, uint64_t now, uint16_t *group)
{
    struct sbnat_map *hit = 0;
    uint32_t i;
    *group = 0;
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        struct sbnat_map *m = &n->map[i];
        if (m->proto != proto || !live(n, m, now))
            continue;
        if (m->gport != gport || (m->flags & F_DNAT) != dnat)
            continue;
        *group = m->eport;
        if (m->rport == rport && eq4(m->raddr, raddr))
            hit = m;
    }
    return hit;
}

/* the uplink side of a flow */
static struct sbnat_map *find_in(struct sbnat *n, uint8_t proto, uint16_t eport, const uint8_t *raddr,
                                 uint16_t rport, uint64_t now)
{
    uint32_t i;
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        struct sbnat_map *m = &n->map[i];
        if (m->proto == proto && m->eport == eport && m->rport == rport && eq4(m->raddr, raddr) &&
            live(n, m, now))
            return m;
    }
    return 0;
}

static int port_used(struct sbnat *n, uint8_t proto, uint16_t eport, uint64_t now)
{
    uint32_t i;
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        struct sbnat_map *m = &n->map[i];
        if (m->proto == proto && m->eport == eport && live(n, m, now))
            return 1;
    }
    return 0;
}

/* a free slot: a free or expired one, else the least recently used live UDP / ICMP one, else none */
static struct sbnat_map *slot(struct sbnat *n, uint64_t now)
{
    struct sbnat_map *old = 0;
    uint32_t i;
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        struct sbnat_map *m = &n->map[i];
        if (!live(n, m, now))
            return m;
        if (m->proto != P_TCP && (!old || m->last_ms < old->last_ms))
            old = m;
    }
    if (old) {
        drop_entry(n, old);
        n->st.evicted++;
    }
    return old;
}

static struct sbnat_map *create(struct sbnat *n, uint8_t proto, uint16_t gport, const uint8_t *raddr,
                                uint16_t rport, uint8_t dnat, uint16_t group, uint64_t now)
{
    struct sbnat_map *m = slot(n, now);
    uint32_t span = (uint32_t)n->port_hi - n->port_lo + 1, i;
    uint16_t eport = 0;
    if (!m) {
        n->st.table_full++;
        return 0;
    }
    /* the group's port, even if slot() just evicted the group's last other flow: nobody else took it */
    if (group)
        eport = group;
    else if (in_range(n, gport) && !port_used(n, proto, gport, now))
        eport = gport;
    else {
        for (i = 0; i < span && !eport; i++) {
            uint16_t p = (uint16_t)(n->port_lo + (n->cursor + i) % span);
            if (!port_used(n, proto, p, now)) {
                eport = p;
                n->cursor = (n->cursor + i + 1) % span;
            }
        }
    }
    if (!eport) {   /* only with a range smaller than the table */
        n->st.table_full++;
        return 0;
    }
    m->proto = proto;
    m->gport = gport;
    m->eport = eport;
    m->rport = rport;
    cp4(m->raddr, raddr);
    m->flags = dnat;
    m->tcp = 0;
    m->last_ms = now;
    n->used++;
    n->st.mapped++;
    return m;
}

/* ---- API ---- */

static void clear_stats(struct sbnat_stats *s)
{
    s->sent = s->to_guest = s->icmp_errors = s->local = s->not_ours = 0;
    s->mapped = s->expired = s->evicted = s->table_full = s->flushed = s->uplink_changes = s->in_frags = 0;
    s->drop_hdr = s->drop_csum = s->drop_ttl = s->drop_frag = s->drop_src = s->drop_dst = 0;
    s->drop_proto = s->drop_icmp = s->drop_short = s->drop_nomap = s->drop_no_uplink = s->drop_no_dns = 0;
}

void sbnat_init(struct sbnat *n, const struct sbnat_cfg *cfg)
{
    uint32_t i;
    cp4(n->uplink, cfg->uplink);
    cp4(n->dns, cfg->dns);
    n->port_lo = cfg->port_lo;
    n->port_hi = cfg->port_hi;
    if (n->port_lo == 0 || n->port_hi < n->port_lo) {
        n->port_lo = SBNAT_PORT_LO;
        n->port_hi = SBNAT_PORT_HI;
    }
    n->cursor = (uint32_t)(cfg->now_ms % ((uint32_t)n->port_hi - n->port_lo + 1));
    n->used = 0;
    clear_stats(&n->st);
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        n->map[i].proto = 0;
        n->map[i].last_ms = 0;
    }
}

void sbnat_uplink_changed(struct sbnat *n, const uint8_t addr[4], const uint8_t dns[4])
{
    static const uint8_t none[4] = { 0, 0, 0, 0 };
    uint32_t i;
    cp4(n->uplink, addr ? addr : none);
    cp4(n->dns, dns ? dns : none);
    for (i = 0; i < SBNAT_ENTRIES; i++) {
        if (n->map[i].proto) {
            drop_entry(n, &n->map[i]);
            n->st.flushed++;
        }
    }
    n->st.uplink_changes++;
}

void sbnat_tick(struct sbnat *n, uint64_t now_ms)
{
    uint32_t i;
    for (i = 0; i < SBNAT_ENTRIES; i++)
        (void)live(n, &n->map[i], now_ms);
}

uint32_t sbnat_count(struct sbnat *n, uint64_t now_ms)
{
    sbnat_tick(n, now_ms);
    return n->used;
}

static int bad_dst(const struct sbnat *n, const uint8_t *d)
{
    if (d[0] == 0 || d[0] == 127 || d[0] >= 224)
        return 1;
    if (d[0] == 10 && d[1] == 77 && d[2] == 0 && d[3] < 4)
        return 1;
    return eq4(d, n->uplink);
}

int sbnat_from_guest(struct sbnat *n, uint8_t *p, uint32_t len, uint64_t now)
{
    uint32_t hl, tot, l4len;
    uint8_t *l4, *l4ck = 0, proto, dnat = 0;
    uint16_t gport, rport, group;
    const uint8_t *raddr;
    struct sbnat_map *m;
    int udp;

    hl = ip_ok(n, p, len, &tot);
    if (!hl)
        return SBNAT_DROP;
    if (!eq4(p + 12, guest_addr)) {
        n->st.drop_src++;
        return SBNAT_DROP;
    }
    if (rd16(p + 6) & 0x1fff) {
        n->st.drop_frag++;
        return SBNAT_DROP;
    }
    proto = p[9];
    l4 = p + hl;
    l4len = tot - hl;
    if (eq4(p + 16, gw_addr)) {
        if ((proto == P_TCP || proto == P_UDP) && l4len >= 4 && rd16(l4 + 2) == 53) {
            dnat = F_DNAT;
        } else {
            n->st.local++;
            return SBNAT_LOCAL;
        }
    } else if (bad_dst(n, p + 16)) {
        n->st.drop_dst++;
        return SBNAT_DROP;
    }
    if (zero4(n->uplink)) {
        n->st.drop_no_uplink++;
        return SBNAT_DROP;
    }
    if (dnat && zero4(n->dns)) {
        n->st.drop_no_dns++;
        return SBNAT_DROP;
    }
    if (p[8] <= 1) {
        n->st.drop_ttl++;
        return SBNAT_DROP;
    }
    udp = proto == P_UDP;
    if (proto == P_TCP || proto == P_UDP) {
        if (l4len < (udp ? 8u : 20u)) {
            n->st.drop_short++;
            return SBNAT_DROP;
        }
        gport = rd16(l4);
        rport = rd16(l4 + 2);
        l4ck = l4 + (udp ? 6 : 16);
    } else if (proto == P_ICMP) {
        if (l4len < 8) {
            n->st.drop_short++;
            return SBNAT_DROP;
        }
        if (l4[0] != 8 || l4[1] != 0) {
            n->st.drop_icmp++;
            return SBNAT_DROP;
        }
        gport = rd16(l4 + 4);
        rport = 0;
    } else {
        n->st.drop_proto++;
        return SBNAT_DROP;
    }
    raddr = dnat ? n->dns : p + 16;
    m = find_out(n, proto, gport, raddr, rport, dnat, now, &group);
    if (!m) {
        if (proto == P_TCP && (l4[13] & TH_RST)) {
            n->st.drop_nomap++;
            return SBNAT_DROP;
        }
        m = create(n, proto, gport, raddr, rport, dnat, group, now);
        if (!m)
            return SBNAT_DROP;
    }
    if (proto == P_TCP) {
        uint8_t fl = l4[13];
        if ((fl & (TH_SYN | TH_ACK)) == TH_SYN && (m->tcp & (T_RST | T_FINOUT | T_FININ)))
            m->tcp = 0;     /* a new connection on a closing 4-tuple */
        if (fl & TH_RST)
            m->tcp |= T_RST;
        if (fl & TH_FIN)
            m->tcp |= T_FINOUT;
        if (!(fl & TH_SYN))
            m->tcp |= T_ACKOUT;
    }
    m->last_ms = now;

    dec_ttl(p);
    set_addr(p + 12, n->uplink, p + 10, l4ck, udp);
    if (dnat)
        set_addr(p + 16, n->dns, p + 10, l4ck, udp);
    if (proto == P_ICMP)
        set_port(l4 + 4, m->eport, l4 + 2, 0);
    else
        set_port(l4, m->eport, l4ck, udp);
    n->st.sent++;
    return SBNAT_SEND;
}

static int not_ours(struct sbnat *n)
{
    n->st.not_ours++;
    return SBNAT_NOT_OURS;
}

/* ICMP destination unreachable / time exceeded / parameter problem about a packet we sent for the guest */
static int icmp_error(struct sbnat *n, uint8_t *p, uint8_t *icmp, uint32_t icmplen, uint64_t now)
{
    uint8_t *ip2 = icmp + 8, *l42, *l4ck = 0, pr2;
    uint32_t rem = icmplen - 8, hl2, rem2;
    uint16_t eport, rport;
    struct sbnat_map *m;
    int udp;

    if (icmplen < 8 + 20 || (ip2[0] >> 4) != 4)
        return not_ours(n);
    hl2 = (uint32_t)(ip2[0] & 15) * 4;
    if (hl2 < 20 || hl2 + 8 > rem || !eq4(ip2 + 12, n->uplink) || (rd16(ip2 + 6) & 0x1fff))
        return not_ours(n);
    pr2 = ip2[9];
    l42 = ip2 + hl2;
    rem2 = rem - hl2;
    udp = pr2 == P_UDP;
    if (pr2 == P_TCP || pr2 == P_UDP) {
        eport = rd16(l42);
        rport = rd16(l42 + 2);
        if (udp)
            l4ck = l42 + 6;
        else if (rem2 >= 20)
            l4ck = l42 + 16;    /* only when the whole TCP header came back, as Linux does */
    } else if (pr2 == P_ICMP && l42[0] == 8) {
        eport = rd16(l42 + 4);
        rport = 0;
    } else {
        return not_ours(n);
    }
    if (!in_range(n, eport))
        return not_ours(n);
    m = find_in(n, pr2, eport, ip2 + 16, rport, now);
    if (!m)
        return not_ours(n);
    if (fold(sum16(icmp, icmplen)) != 0xffff) {
        n->st.drop_csum++;
        return SBNAT_DROP;
    }
    if (p[8] <= 1) {
        n->st.drop_ttl++;
        return SBNAT_DROP;
    }
    /* the embedded packet: as the guest sent it */
    set_addr(ip2 + 12, guest_addr, ip2 + 10, l4ck, udp);
    if (m->flags & F_DNAT)
        set_addr(ip2 + 16, gw_addr, ip2 + 10, l4ck, udp);
    if (pr2 == P_ICMP)
        set_port(l42 + 4, m->gport, l42 + 2, 0);
    else
        set_port(l42, m->gport, l4ck, udp);
    /* the outer header: to the guest; from 10.77.0.1 if the DNS server itself sent it */
    dec_ttl(p);
    set_addr(p + 16, guest_addr, p + 10, 0, 0);
    if ((m->flags & F_DNAT) && eq4(p + 12, n->dns))
        set_addr(p + 12, gw_addr, p + 10, 0, 0);
    /* verified above, so a fresh checksum is the same as an incremental one */
    wr16(icmp + 2, 0);
    wr16(icmp + 2, (uint16_t)~fold(sum16(icmp, icmplen)));
    n->st.icmp_errors++;
    n->st.to_guest++;
    return SBNAT_TO_GUEST;
}

int sbnat_from_uplink(struct sbnat *n, uint8_t *p, uint32_t len, uint64_t now)
{
    uint32_t hl, tot, l4len;
    uint8_t *l4, *l4ck = 0, proto;
    uint16_t eport, rport;
    struct sbnat_map *m;
    int udp;

    if (len >= 1 && (p[0] >> 4) != 4)
        return not_ours(n);     /* IPv6 and the like: the host's */
    hl = ip_ok(n, p, len, &tot);
    if (!hl)
        return SBNAT_DROP;
    if (zero4(n->uplink) || !eq4(p + 16, n->uplink))
        return not_ours(n);
    if (rd16(p + 6) & 0x1fff) {
        n->st.in_frags++;
        return not_ours(n);
    }
    proto = p[9];
    l4 = p + hl;
    l4len = tot - hl;
    udp = proto == P_UDP;
    if (proto == P_TCP || proto == P_UDP) {
        if (l4len < (udp ? 8u : 20u))
            return not_ours(n);
        rport = rd16(l4);
        eport = rd16(l4 + 2);
        l4ck = l4 + (udp ? 6 : 16);
    } else if (proto == P_ICMP) {
        if (l4len < 8)
            return not_ours(n);
        if (l4[0] == 3 || l4[0] == 11 || l4[0] == 12)
            return icmp_error(n, p, l4, l4len, now);
        if (l4[0] != 0 || l4[1] != 0)
            return not_ours(n);
        eport = rd16(l4 + 4);
        rport = 0;
    } else {
        return not_ours(n);
    }
    if (!in_range(n, eport))
        return not_ours(n);
    m = find_in(n, proto, eport, p + 12, rport, now);
    if (!m)
        return not_ours(n);
    if (p[8] <= 1) {
        n->st.drop_ttl++;
        return SBNAT_DROP;
    }
    if (proto == P_TCP) {
        uint8_t fl = l4[13];
        if (fl & TH_RST)
            m->tcp |= T_RST;
        if (fl & TH_FIN)
            m->tcp |= T_FININ;
        if ((fl & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK))
            m->tcp |= T_SYNACK;
        if (!(fl & TH_SYN))
            m->tcp |= T_ACKIN;
    }
    m->flags |= F_IN;
    m->last_ms = now;

    dec_ttl(p);
    set_addr(p + 16, guest_addr, p + 10, l4ck, udp);
    if (m->flags & F_DNAT)
        set_addr(p + 12, gw_addr, p + 10, l4ck, udp);
    if (proto == P_ICMP)
        set_port(l4 + 4, m->gport, l4 + 2, 0);
    else
        set_port(l4 + 2, m->gport, l4ck, udp);
    n->st.to_guest++;
    return SBNAT_TO_GUEST;
}

uint32_t sbnat_echo_reply(uint8_t *p, uint32_t len)
{
    uint32_t hl, tot;
    uint8_t *icmp;
    if (len < 20 || (p[0] >> 4) != 4)
        return 0;
    hl = (uint32_t)(p[0] & 15) * 4;
    tot = rd16(p + 2);
    if (hl < 20 || tot < hl + 8 || tot > len || fold(sum16(p, hl)) != 0xffff)
        return 0;
    if (p[9] != P_ICMP || (rd16(p + 6) & 0x3fff) || !eq4(p + 12, guest_addr) || !eq4(p + 16, gw_addr))
        return 0;
    icmp = p + hl;
    if (icmp[0] != 8 || icmp[1] != 0 || fold(sum16(icmp, tot - hl)) != 0xffff)
        return 0;
    cp4(p + 12, gw_addr);
    cp4(p + 16, guest_addr);
    p[8] = 64;
    wr16(p + 10, 0);
    wr16(p + 10, (uint16_t)~fold(sum16(p, hl)));
    icmp[0] = 0;
    ck16(icmp + 2, 0x0800, 0x0000);
    return tot;
}
