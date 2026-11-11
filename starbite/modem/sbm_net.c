/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: a very small IPv4 client over the CP's raw-IP data channel (docs/MODEM.md 7.9): the attended SIM test's
 * proof that mobile data works. One DNS A-record query (UDP to the PDP context's DNS server), then one HTTP/1.1 GET
 * over a minimal TCP client (in-order receive only, a fixed window, retransmission of the SYN, the request and the
 * FIN; no options but the MSS). No TLS. Packets are raw IPv4 (no link header): the CP's PDP channel carries them as
 * they are.
 *
 * Freestanding (no C library, no allocation); the caller owns struct sbm_net and gives it a transmit callback, the
 * received packets and a millisecond clock. Nothing here logs an address: the caller logs counters and results
 * (the DNS answer for a public name and the HTTP status are not personal; the phone's own address is never shown).
 */
#include "sbm.h"

static uint16_t csum_add(uint32_t sum, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)b[i] << 8 | b[i + 1];
    if (n & 1)
        sum += (uint32_t)b[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static uint16_t csum(const uint8_t *b, uint32_t n) { return (uint16_t)~csum_add(0, b, n); }

/* the TCP/UDP checksum over the pseudo header and the segment */
static uint16_t l4_csum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, const uint8_t *seg, uint32_t n)
{
    uint8_t ph[12];
    sbm_memcpy(ph, src, 4);
    sbm_memcpy(ph + 4, dst, 4);
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = (uint8_t)(n >> 8);
    ph[11] = (uint8_t)n;
    uint32_t s = csum_add(0, ph, 12);
    s = csum_add(s, seg, n);
    return (uint16_t)~s;
}

static void put16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int send_ip(struct sbm_net *n, uint8_t proto, const uint8_t dst[4], uint32_t l4len)
{
    uint8_t *ip = n->pkt;
    uint32_t tot = 20 + l4len;
    if (tot > sizeof(n->pkt) || tot > n->mtu)
        return SBM_ERANGE;
    ip[0] = 0x45;
    ip[1] = 0;
    put16be(ip + 2, (uint16_t)tot);
    put16be(ip + 4, n->ip_id++);
    put16be(ip + 6, 0x4000);              /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16be(ip + 10, 0);
    sbm_memcpy(ip + 12, n->ip, 4);
    sbm_memcpy(ip + 16, dst, 4);
    put16be(ip + 10, csum(ip, 20));
    int r = n->tx(n->ctx, ip, tot);
    if (r < 0) {
        n->tx_errors++;
        return r;
    }
    n->tx_packets++;
    return 0;
}

void sbm_net_init(struct sbm_net *n, const uint8_t ip[4], const uint8_t dns[4], uint16_t mtu,
                  int (*tx)(void *ctx, const uint8_t *pkt, uint32_t len), void *ctx, uint32_t seed)
{
    sbm_memset(n, 0, sizeof(*n));
    sbm_memcpy(n->ip, ip, 4);
    sbm_memcpy(n->dns, dns, 4);
    n->mtu = mtu >= 576 && mtu <= 1500 ? mtu : 1500;
    n->tx = tx;
    n->ctx = ctx;
    n->seed = seed ? seed : 0x2545F491u;
    n->ip_id = (uint16_t)n->seed;
}

static uint32_t rnd(struct sbm_net *n)
{
    n->seed ^= n->seed << 13;
    n->seed ^= n->seed >> 17;
    n->seed ^= n->seed << 5;
    return n->seed;
}

/* ---- DNS --------------------------------------------------------------------------------------- */

static int dns_send(struct sbm_net *n)
{
    uint8_t *u = n->pkt + 20, *q = u + 8;
    uint32_t o = 12;
    put16be(q, n->dns_id);
    put16be(q + 2, 0x0100);              /* recursion desired */
    put16be(q + 4, 1);
    put16be(q + 6, 0);
    put16be(q + 8, 0);
    put16be(q + 10, 0);
    for (const char *s = n->dns_name; *s;) {
        const char *e = s;
        while (*e && *e != '.')
            ++e;
        uint32_t l = (uint32_t)(e - s);
        if (l == 0 || l > 63 || o + l + 6 > 300)
            return SBM_EINVAL;
        q[o++] = (uint8_t)l;
        sbm_memcpy(q + o, s, l);
        o += l;
        s = *e ? e + 1 : e;
    }
    q[o++] = 0;
    put16be(q + o, 1);                   /* A */
    put16be(q + o + 2, 1);               /* IN */
    o += 4;
    put16be(u, n->dns_port);
    put16be(u + 2, 53);
    put16be(u + 4, (uint16_t)(8 + o));
    put16be(u + 6, 0);
    uint16_t c = l4_csum(n->ip, n->dns, 17, u, 8 + o);
    put16be(u + 6, c ? c : 0xFFFF);
    n->dns_sent++;
    return send_ip(n, 17, n->dns, 8 + o);
}

int sbm_net_dns(struct sbm_net *n, const char *name, uint32_t now)
{
    uint32_t l = sbm_strlen(name);
    if (l == 0 || l >= sizeof(n->dns_name))
        return SBM_EINVAL;
    sbm_memcpy(n->dns_name, name, l + 1);
    n->dns_id = (uint16_t)rnd(n);
    n->dns_port = (uint16_t)(49152 + (rnd(n) & 0x3FFF));
    n->dns_state = SBM_NET_WAITING;
    n->dns_at = now;
    n->dns_tries = 1;
    return dns_send(n);
}

static void dns_rx(struct sbm_net *n, const uint8_t *u, uint32_t len)
{
    if (n->dns_state != SBM_NET_WAITING || len < 8 + 12 || get16be(u + 2) != n->dns_port || get16be(u) != 53)
        return;
    const uint8_t *d = u + 8;
    uint32_t dl = len - 8, o = 12;
    if (get16be(d) != n->dns_id || !(d[2] & 0x80))
        return;
    n->dns_rcode = d[3] & 0x0F;
    unsigned qd = get16be(d + 4), an = get16be(d + 6);
    for (unsigned i = 0; i < qd; ++i) {                  /* skip the question(s) */
        while (o < dl && d[o] && !(d[o] & 0xC0))
            o += d[o] + 1u;
        o += o < dl && (d[o] & 0xC0) ? 2 : 1;
        o += 4;
    }
    for (unsigned i = 0; i < an && o < dl; ++i) {
        while (o < dl && d[o] && !(d[o] & 0xC0))
            o += d[o] + 1u;
        o += o < dl && (d[o] & 0xC0) ? 2 : 1;
        if (o + 10 > dl)
            break;
        uint16_t type = get16be(d + o), cls = get16be(d + o + 2), rl = get16be(d + o + 8);
        o += 10;
        if (o + rl > dl)
            break;
        if (type == 1 && cls == 1 && rl == 4) {
            sbm_memcpy(n->dns_answer, d + o, 4);
            n->dns_state = SBM_NET_DONE;
            return;
        }
        o += rl;
    }
    n->dns_state = SBM_NET_FAILED;
}

/* ---- TCP + HTTP -------------------------------------------------------------------------------- */

enum { T_CLOSED, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT, T_DONE };

static int tcp_send(struct sbm_net *n, uint8_t flags, uint32_t seq, const uint8_t *data, uint32_t dlen)
{
    uint8_t *t = n->pkt + 20;
    uint32_t hl = flags & 0x02 ? 24 : 20;   /* SYN carries the MSS */
    if (20 + hl + dlen > sizeof(n->pkt))
        return SBM_ERANGE;
    put16be(t, n->sport);
    put16be(t + 2, n->dport);
    put32be(t + 4, seq);
    put32be(t + 8, flags & 0x10 ? n->rcv_nxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    put16be(t + 14, SBM_NET_WINDOW);
    put16be(t + 16, 0);
    put16be(t + 18, 0);
    if (hl == 24) {
        t[20] = 2;
        t[21] = 4;
        put16be(t + 22, (uint16_t)(n->mtu - 40));
    }
    if (dlen)
        sbm_memcpy(t + hl, data, dlen);
    put16be(t + 16, l4_csum(n->ip, n->peer, 6, t, hl + dlen));
    n->tcp_sent++;
    return send_ip(n, 6, n->peer, hl + dlen);
}

int sbm_net_http_get(struct sbm_net *n, const uint8_t ip[4], const char *host, const char *path, uint32_t now)
{
    uint32_t o = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host,
                            "\r\nUser-Agent: starbite-modem-test\r\nConnection: close\r\n\r\n" };
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t l = sbm_strlen(parts[i]);
        if (o + l > sizeof(n->req))
            return SBM_ERANGE;
        sbm_memcpy(n->req + o, parts[i], l);
        o += l;
    }
    n->req_len = (uint16_t)o;
    sbm_memcpy(n->peer, ip, 4);
    n->sport = (uint16_t)(49152 + (rnd(n) & 0x3FFF));
    n->dport = 80;
    n->iss = rnd(n);
    n->snd_nxt = n->iss + 1;
    n->rcv_nxt = 0;
    n->tcp_state = T_SYN_SENT;
    n->http_state = SBM_NET_WAITING;
    n->http_code = 0;
    n->http_bytes = 0;
    n->http_hdr_len = 0;
    n->tcp_at = now;
    n->tcp_tries = 1;
    return tcp_send(n, 0x02, n->iss, 0, 0);
}

static void http_data(struct sbm_net *n, const uint8_t *d, uint32_t l)
{
    n->http_bytes += l;
    for (uint32_t i = 0; i < l && n->http_hdr_len + 1 < sizeof(n->http_head); ++i)
        n->http_head[n->http_hdr_len++] = (char)d[i];
    n->http_head[n->http_hdr_len] = 0;
    if (!n->http_code && n->http_hdr_len >= 12 && sbm_memcmp(n->http_head, "HTTP/1.", 7) == 0) {
        const char *c = n->http_head + 9;
        if (c[0] >= '1' && c[0] <= '5' && c[1] >= '0' && c[1] <= '9' && c[2] >= '0' && c[2] <= '9')
            n->http_code = (c[0] - '0') * 100 + (c[1] - '0') * 10 + (c[2] - '0');
    }
}

static void tcp_rx(struct sbm_net *n, const uint8_t *t, uint32_t len)
{
    if (n->tcp_state == T_CLOSED || n->tcp_state == T_DONE || len < 20 || get16be(t) != n->dport ||
        get16be(t + 2) != n->sport)
        return;
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4;
    if (hl < 20 || hl > len)
        return;
    uint8_t fl = t[13];
    uint32_t seq = get32be(t + 4), ack = get32be(t + 8);
    const uint8_t *d = t + hl;
    uint32_t dl = len - hl;
    if (fl & 0x04) {                                     /* RST */
        n->tcp_state = T_DONE;
        n->tcp_reset = 1;
        if (n->http_state == SBM_NET_WAITING)
            n->http_state = n->http_code ? SBM_NET_DONE : SBM_NET_FAILED;
        return;
    }
    if (n->tcp_state == T_SYN_SENT) {
        if ((fl & 0x12) != 0x12 || ack != n->iss + 1)
            return;
        n->rcv_nxt = seq + 1;
        n->snd_una = ack;
        n->tcp_state = T_ESTABLISHED;
        n->tcp_connected = 1;
        tcp_send(n, 0x18, n->snd_nxt, n->req, n->req_len);   /* ACK + PSH with the request */
        n->snd_nxt += n->req_len;
        return;
    }
    if (fl & 0x10) {
        if ((int32_t)(ack - n->snd_una) > 0)
            n->snd_una = ack;
    }
    int ack_now = 0;
    if (dl) {
        if (seq == n->rcv_nxt) {
            http_data(n, d, dl);
            n->rcv_nxt += dl;
        } else {
            n->tcp_ooo++;                                /* out of order: dropped, the ACK repeats rcv_nxt */
        }
        ack_now = 1;
    }
    if ((fl & 0x01) && seq + dl == n->rcv_nxt) {         /* FIN in order */
        n->rcv_nxt += 1;
        ack_now = 0;
        if (n->tcp_state == T_ESTABLISHED) {
            tcp_send(n, 0x11, n->snd_nxt, 0, 0);         /* FIN + ACK */
            n->snd_nxt += 1;
            n->tcp_state = T_FIN_WAIT;
        } else {
            tcp_send(n, 0x10, n->snd_nxt, 0, 0);
        }
        n->http_state = n->http_code ? SBM_NET_DONE : SBM_NET_FAILED;
    }
    if (ack_now)
        tcp_send(n, 0x10, n->snd_nxt, 0, 0);
    if (n->tcp_state == T_FIN_WAIT && n->snd_una == n->snd_nxt)
        n->tcp_state = T_DONE;
}

void sbm_net_rx(struct sbm_net *n, const uint8_t *p, uint32_t len)
{
    n->rx_packets++;
    if (len < 20 || (p[0] >> 4) != 4) {
        n->rx_other++;
        return;
    }
    uint32_t ihl = (uint32_t)(p[0] & 0x0F) * 4, tot = get16be(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || csum(p, ihl) != 0 || sbm_memcmp(p + 16, n->ip, 4) != 0 ||
        (get16be(p + 6) & 0x3FFF)) {
        n->rx_other++;
        return;
    }
    const uint8_t *l4 = p + ihl;
    uint32_t l4len = tot - ihl;
    if (l4_csum(p + 12, p + 16, p[9], l4, l4len) != 0 && !(p[9] == 17 && l4len >= 8 && get16be(l4 + 6) == 0)) {
        n->rx_bad_csum++;
        return;
    }
    if (p[9] == 17 && l4len >= 8 && sbm_memcmp(p + 12, n->dns, 4) == 0)
        dns_rx(n, l4, l4len);
    else if (p[9] == 6 && sbm_memcmp(p + 12, n->peer, 4) == 0)
        tcp_rx(n, l4, l4len);
    else
        n->rx_other++;
}

void sbm_net_tick(struct sbm_net *n, uint32_t now)
{
    if (n->dns_state == SBM_NET_WAITING && now - n->dns_at >= 2000) {
        if (n->dns_tries >= 4) {
            n->dns_state = SBM_NET_FAILED;
        } else {
            n->dns_tries++;
            n->dns_at = now;
            dns_send(n);
        }
    }
    if ((n->tcp_state == T_SYN_SENT || n->tcp_state == T_ESTABLISHED || n->tcp_state == T_FIN_WAIT) &&
        now - n->tcp_at >= 2000) {
        n->tcp_at = now;
        if (n->tcp_tries >= 6) {
            n->tcp_state = T_DONE;
            if (n->http_state == SBM_NET_WAITING)
                n->http_state = SBM_NET_FAILED;
        } else if (n->tcp_state == T_SYN_SENT) {
            n->tcp_tries++;
            tcp_send(n, 0x02, n->iss, 0, 0);
        } else if (n->snd_una != n->snd_nxt) {          /* the request or our FIN not acknowledged yet */
            n->tcp_tries++;
            if (n->tcp_state == T_ESTABLISHED)
                tcp_send(n, 0x18, n->iss + 1, n->req, n->req_len);
            else
                tcp_send(n, 0x11, n->snd_nxt - 1, 0, 0);
        } else if (n->tcp_state == T_FIN_WAIT) {
            n->tcp_state = T_DONE;
        }
    }
}
