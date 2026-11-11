/* SPDX-License-Identifier: MIT
 * sbtcp: TCP for sbnet (tcp.h). Written for Starbite from RFC 9293 (TCP), 6298 (the retransmission timer),
 * 5681 (congestion control), 6582 (NewReno), 5961 (RST and SYN checks), 1122 and 6691 (MSS). Every received byte is bounds-checked:
 * segments come from the air. */
#include "tcp.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define RTO_INIT 1000u
#define RTO_MIN 300u
#define RTO_MAX 30000u
#define SYN_TRIES 6u
#define DATA_TRIES 10u
#define TIME_WAIT_MS 2000u
#define FIN_WAIT_2_MS 30000u

static void cpy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static void set(uint8_t *d, uint8_t v, uint32_t n) { while (n--) *d++ = v; }
static int eq4(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max32(uint32_t a, uint32_t b) { return a > b ? a : b; }

/* sequence numbers wrap: compare by difference */
static int lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

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

static uint64_t now(struct sbtcp *t) { return t->net->os.now_ms(t->net->os.ctx); }
static uint32_t rnd(struct sbtcp *t) { return t->net->os.random(t->net->os.ctx); }

/* ---- state helpers ----------------------------------------------------------------------------------------------- */

static int synced(int st) { return st >= SBTCP_ESTABLISHED && st <= SBTCP_TIME_WAIT; }
static int can_send_data(int st) { return st == SBTCP_ESTABLISHED || st == SBTCP_CLOSE_WAIT || st == SBTCP_FIN_WAIT_1 ||
                                          st == SBTCP_CLOSING || st == SBTCP_LAST_ACK; }
static uint32_t tx_base(const struct sbtcp_sock *s) { return s->iss + 1u + s->tx_acked; }
static uint32_t fin_seq(const struct sbtcp_sock *s) { return tx_base(s) + s->tx_len; }
static int fin_sent(const struct sbtcp_sock *s) { return s->fin_queued && lt(fin_seq(s), s->snd_max); }
static int fin_acked(const struct sbtcp_sock *s) { return s->fin_queued && lt(fin_seq(s), s->snd_una); }
static uint32_t rcv_wnd(const struct sbtcp_sock *s) { return min32(SBTCP_RXBUF - s->rx_len, 65535u); }

static void end(struct sbtcp_sock *s, int state, int err)
{
    s->state = state;
    if (err && !s->err)
        s->err = err;
    s->rto_at = 0;
    if (state == SBTCP_CLOSED && s->app_closed)
        s->state = SBTCP_FREE;
}

/* ---- segments out ------------------------------------------------------------------------------------------------ */

static void raw_out(struct sbtcp *t, const uint8_t *rip, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    uint8_t flags, uint16_t win, const uint8_t *data, uint32_t len, int mss_opt)
{
    uint8_t f[14 + 20 + 24 + SBTCP_MSS];
    uint8_t *h = f + 34;
    uint32_t hl = mss_opt ? 24 : 20, s;
    if (len > SBTCP_MSS)
        return;
    p16(h, sport), p16(h + 2, dport), p32(h + 4, seq), p32(h + 8, ack);
    h[12] = (uint8_t)(hl / 4) << 4, h[13] = flags;
    p16(h + 14, win), p16(h + 16, 0), p16(h + 18, 0);
    if (mss_opt)
        h[20] = 2, h[21] = 4, p16(h + 22, SBTCP_MSS);
    if (len)
        cpy(h + hl, data, len);
    s = sum16(t->net->ip, 4, 0);
    s = sum16(rip, 4, s);
    s += 6u + hl + len;
    p16(h + 16, fold(sum16(h, hl + len, s)));
    t->tx_segments++;
    sbnet_ip_send(t->net, rip, 6, f, hl + len);
}

static void seg_out(struct sbtcp *t, struct sbtcp_sock *s, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len)
{
    uint16_t win = (uint16_t)rcv_wnd(s);
    if (s->state != SBTCP_SYN_SENT)
        flags |= F_ACK;
    s->rcv_wnd_sent = win;
    raw_out(t, s->rip, s->lport, s->rport, seq, s->state == SBTCP_SYN_SENT ? 0 : s->rcv_nxt, flags, win, data, len,
            (flags & F_SYN) != 0);
}

static void ack_now(struct sbtcp *t, struct sbtcp_sock *s) { seg_out(t, s, s->snd_nxt, 0, 0, 0); }

static void timer_on(struct sbtcp *t, struct sbtcp_sock *s)
{
    if (!s->rto_at)
        s->rto_at = now(t) + s->rto;
}

/* the first unacknowledged segment again (fast retransmit, a partial ACK) */
static void retransmit_first(struct sbtcp *t, struct sbtcp_sock *s)
{
    uint32_t off = s->snd_una - tx_base(s);
    if (!lt(s->snd_una, tx_base(s)) && off < s->tx_len) {
        t->retransmits++;
        seg_out(t, s, s->snd_una, 0, s->tx + off, min32(s->tx_len - off, s->mss));
    } else if (fin_sent(s) && s->snd_una == fin_seq(s)) {
        t->retransmits++;
        seg_out(t, s, s->snd_una, F_FIN, 0, 0);
    }
}

/* send what the windows allow (and the FIN after the last byte); force: an ACK even when nothing else goes */
static void output(struct sbtcp *t, struct sbtcp_sock *s, int force)
{
    int sent = 0;
    if (!can_send_data(s->state)) {
        if (force && synced(s->state))
            ack_now(t, s);
        return;
    }
    for (;;) {
        uint32_t off = s->snd_nxt - tx_base(s), inflight = s->snd_nxt - s->snd_una;
        uint32_t wnd = min32(s->snd_wnd, s->cwnd);
        if (lt(s->snd_nxt, tx_base(s)))
            break;                              /* only the SYN outstanding (never here once synced) */
        if (off < s->tx_len) {
            uint32_t n = min32(s->tx_len - off, s->mss);
            if (wnd <= inflight)
                break;
            n = min32(n, wnd - inflight);
            seg_out(t, s, s->snd_nxt, off + n == s->tx_len ? F_PSH : 0, s->tx + off, n);
            if (!s->rtt_on && s->snd_nxt == s->snd_max) {
                s->rtt_on = 1, s->rtt_seq = s->snd_nxt + n, s->rtt_at = now(t);
            }
            s->snd_nxt += n;
            if (lt(s->snd_max, s->snd_nxt))
                s->snd_max = s->snd_nxt;
            timer_on(t, s);
            sent = 1;
            continue;
        }
        if (s->fin_queued && s->snd_nxt == fin_seq(s)) {
            seg_out(t, s, s->snd_nxt, F_FIN, 0, 0);
            s->snd_nxt++;
            if (lt(s->snd_max, s->snd_nxt))
                s->snd_max = s->snd_nxt;
            timer_on(t, s);
            sent = 1;
        }
        break;
    }
    if (!sent && force)
        ack_now(t, s);
    /* a zero window with bytes waiting: the timer probes it */
    if (s->snd_wnd == 0 && s->snd_una == s->snd_max && (s->tx_len || (s->fin_queued && !fin_sent(s))))
        timer_on(t, s);
}

/* ---- the round trip (RFC 6298) ----------------------------------------------------------------------------------- */

static void rtt_sample(struct sbtcp_sock *s, uint32_t r)
{
    if (!s->srtt) {
        s->srtt = r ? r : 1;
        s->rttvar = r / 2;
    } else {
        uint32_t d = s->srtt > r ? s->srtt - r : r - s->srtt;
        s->rttvar = (3 * s->rttvar + d) / 4;
        s->srtt = (7 * s->srtt + r) / 8;
        if (!s->srtt)
            s->srtt = 1;
    }
    s->rto = s->srtt + max32(10, 4 * s->rttvar);
    if (s->rto < RTO_MIN)
        s->rto = RTO_MIN;
    if (s->rto > RTO_MAX)
        s->rto = RTO_MAX;
}

/* ---- segments in -------------------------------------------------------------------------------------------------- */

static uint16_t parse_mss(const uint8_t *h, uint32_t hl)
{
    uint32_t o = 20;
    while (o < hl) {
        uint8_t k = h[o];
        if (k == 0)
            break;
        if (k == 1) {
            o++;
            continue;
        }
        if (o + 1 >= hl || h[o + 1] < 2 || o + h[o + 1] > hl)
            break;
        if (k == 2 && h[o + 1] == 4)
            return g16(h + o + 2);
        o += h[o + 1];
    }
    return 536;                                 /* RFC 9293's default */
}

static void reset_reply(struct sbtcp *t, const uint8_t *from, const uint8_t *h, uint32_t plen)
{
    uint8_t fl = h[13];
    if (fl & F_RST)
        return;
    t->resets_sent++;
    if (fl & F_ACK)
        raw_out(t, from, g16(h + 2), g16(h), g32(h + 8), 0, F_RST, 0, 0, 0, 0);
    else
        raw_out(t, from, g16(h + 2), g16(h), 0, g32(h + 4) + plen + !!(fl & F_SYN) + !!(fl & F_FIN), F_RST | F_ACK, 0,
                0, 0, 0);
}

static int find(struct sbtcp *t, const uint8_t *from, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < SBTCP_SOCKS; i++) {
        struct sbtcp_sock *s = &t->s[i];
        if (s->state != SBTCP_FREE && s->state != SBTCP_LISTEN && s->state != SBTCP_CLOSED && s->lport == dport &&
            s->rport == sport && eq4(s->rip, from))
            return i;
    }
    return -1;
}

static int free_slot(struct sbtcp *t)
{
    for (int i = 0; i < SBTCP_SOCKS; i++)
        if (t->s[i].state == SBTCP_FREE)
            return i;
    return -1;
}

static void slot_init(struct sbtcp *t, struct sbtcp_sock *s)
{
    set((uint8_t *)s, 0, (uint32_t)((uint8_t *)s->rx - (uint8_t *)s));   /* the buffers need no clearing */
    s->iss = rnd(t);
    s->snd_una = s->iss;
    s->snd_nxt = s->snd_max = s->iss + 1;
    s->rto = RTO_INIT;
    s->mss = 536;
    s->listener = -1;
    s->ssthresh = 65535;
}

/* bytes from seq after a gap: written where they belong in rx (only within the free space) and their range
 * remembered, merged with its neighbours; with no room for another range they are simply not kept */
static void ooo_store(struct sbtcp_sock *s, uint32_t seq, const uint8_t *d, uint32_t n)
{
    uint32_t off = seq - s->rcv_nxt, room = SBTCP_RXBUF - s->rx_len, a, b;
    unsigned i, j;
    if (off >= room)
        return;
    n = min32(n, room - off);
    a = seq, b = seq + n;
    for (i = 0; i < s->ooo_n; i++)
        if (le(s->ooo_s[i], a) && le(b, s->ooo_e[i]))
            return;                             /* already here */
    for (i = 0; i < s->ooo_n;) {
        if (le(s->ooo_s[i], b) && le(a, s->ooo_e[i])) {     /* overlaps or touches: merged */
            if (lt(s->ooo_s[i], a))
                a = s->ooo_s[i];
            if (lt(b, s->ooo_e[i]))
                b = s->ooo_e[i];
            for (j = i; j + 1 < s->ooo_n; j++)
                s->ooo_s[j] = s->ooo_s[j + 1], s->ooo_e[j] = s->ooo_e[j + 1];
            s->ooo_n--;
        } else {
            i++;
        }
    }
    if (s->ooo_n >= SBTCP_OOO)
        return;
    for (uint32_t k = 0; k < n; k++)
        s->rx[(s->rx_head + s->rx_len + off + k) % SBTCP_RXBUF] = d[k];
    s->ooo_s[s->ooo_n] = a, s->ooo_e[s->ooo_n] = b;
    s->ooo_n++;
}

/* rcv_nxt moved: the kept ranges it reached join the stream */
static void ooo_join(struct sbtcp_sock *s)
{
    int moved = 1;
    while (moved) {
        moved = 0;
        for (unsigned i = 0; i < s->ooo_n; i++) {
            if (le(s->ooo_s[i], s->rcv_nxt)) {
                if (lt(s->rcv_nxt, s->ooo_e[i])) {
                    uint32_t d = s->ooo_e[i] - s->rcv_nxt;
                    s->rx_len += d;
                    s->rcv_nxt += d;
                }
                for (unsigned j = i; j + 1 < s->ooo_n; j++)
                    s->ooo_s[j] = s->ooo_s[j + 1], s->ooo_e[j] = s->ooo_e[j + 1];
                s->ooo_n--;
                moved = 1;
                break;
            }
        }
    }
}

static void got_ack(struct sbtcp *t, struct sbtcp_sock *s, uint32_t ack)
{
    uint32_t acked = ack - s->snd_una, data;
    uint32_t in_tx = ack - tx_base(s);
    /* the data bytes it covers (not the SYN before them, not the FIN after them) */
    data = lt(ack, tx_base(s)) ? 0 : min32(in_tx, s->tx_len);
    if (data) {
        uint32_t i;
        for (i = 0; i + data < s->tx_len; i++)
            s->tx[i] = s->tx[i + data];
        s->tx_len -= data;
        s->tx_acked += data;
    }
    s->snd_una = ack;
    if (lt(s->snd_nxt, s->snd_una))
        s->snd_nxt = s->snd_una;
    if (s->rtt_on && le(s->rtt_seq, ack)) {
        rtt_sample(s, (uint32_t)(now(t) - s->rtt_at));
        s->rtt_on = 0;
    }
    if (s->dupacks >= 3 && lt(ack, s->recover)) {
        /* a partial ACK in fast recovery: the next hole at once, the window deflated (RFC 6582 3.2 step 3) */
        retransmit_first(t, s);
        s->cwnd = s->cwnd > acked ? s->cwnd - acked + s->mss : s->mss;
        s->retries = 0;
        s->rto_at = 0;
        timer_on(t, s);
        return;
    }
    if (s->dupacks >= 3)
        s->cwnd = s->ssthresh;                  /* the end of fast recovery */
    else if (s->cwnd < s->ssthresh)
        s->cwnd += min32(acked, s->mss);
    else
        s->cwnd += max32(1, (uint32_t)s->mss * s->mss / s->cwnd);
    s->dupacks = 0;
    s->retries = 0;
    s->rto_at = 0;
    if (s->snd_una != s->snd_max)
        timer_on(t, s);
}

void sbtcp_input(struct sbtcp *t, const uint8_t from[4], const uint8_t *h, uint32_t len)
{
    uint32_t hl, plen, seq, ack, seglen, wnd;
    uint16_t sport, dport, win;
    uint8_t fl;
    const uint8_t *data;
    int i, need_ack = 0;
    struct sbtcp_sock *s;
    t->rx_segments++;
    if (len < 20) {
        t->rx_bad++;
        return;
    }
    {
        uint32_t c = sum16(from, 4, 0);
        c = sum16(t->net->ip, 4, c);
        c += 6u + len;
        if (fold(sum16(h, len, c)) != 0) {
            t->rx_bad++;
            return;
        }
    }
    hl = (uint32_t)(h[12] >> 4) * 4;
    if (hl < 20 || hl > len) {
        t->rx_bad++;
        return;
    }
    sport = g16(h), dport = g16(h + 2), seq = g32(h + 4), ack = g32(h + 8), fl = h[13], win = g16(h + 14);
    data = h + hl, plen = len - hl;
    seglen = plen + !!(fl & F_SYN) + !!(fl & F_FIN);
    if (!sport || !dport) {
        t->rx_bad++;
        return;
    }

    i = find(t, from, sport, dport);
    if (i < 0) {
        /* a new connection to a listener */
        if ((fl & (F_SYN | F_ACK | F_RST | F_FIN)) == F_SYN) {
            for (int l = 0; l < SBTCP_SOCKS; l++) {
                if (t->s[l].state != SBTCP_LISTEN || t->s[l].lport != dport)
                    continue;
                int k = free_slot(t);
                if (k < 0)
                    break;                      /* no room: no answer (the peer retries) */
                s = &t->s[k];
                slot_init(t, s);
                s->state = SBTCP_SYN_RCVD;
                cpy(s->rip, from, 4);
                s->rport = sport, s->lport = dport;
                s->rcv_nxt = seq + 1;
                s->mss = (uint16_t)min32(parse_mss(h, hl), SBTCP_MSS);
                if (s->mss < 64)
                    s->mss = 64;
                s->snd_wnd = win, s->wl1 = seq, s->wl2 = s->iss;
                s->cwnd = min32(4u * s->mss, max32(2u * s->mss, 4380));
                s->listener = l;
                s->syn_at = now(t);
                seg_out(t, s, s->iss, F_SYN, 0, 0);
                timer_on(t, s);
                return;
            }
        }
        t->rx_dropped++;
        reset_reply(t, from, h, plen);
        return;
    }
    s = &t->s[i];

    if (s->state == SBTCP_SYN_SENT) {
        int ack_ok = (fl & F_ACK) && ack == s->iss + 1;
        if ((fl & F_ACK) && !ack_ok) {
            t->rx_dropped++;
            reset_reply(t, from, h, plen);
            return;
        }
        if (fl & F_RST) {
            if (ack_ok)
                end(s, SBTCP_CLOSED, SBTCP_E_REFUSED);
            return;
        }
        if (!(fl & F_SYN) || !ack_ok) {
            t->rx_dropped++;                    /* no simultaneous open */
            return;
        }
        s->rcv_nxt = seq + 1;
        s->mss = (uint16_t)min32(parse_mss(h, hl), SBTCP_MSS);
        if (s->mss < 64)
            s->mss = 64;
        s->snd_una = ack;
        s->snd_wnd = win, s->wl1 = seq, s->wl2 = ack;
        s->cwnd = min32(4u * s->mss, max32(2u * s->mss, 4380));
        s->state = SBTCP_ESTABLISHED;
        s->retries = 0, s->rto_at = 0;
        if (!s->srtt)
            rtt_sample(s, (uint32_t)(now(t) - s->syn_at));
        if (s->fin_queued)
            s->state = SBTCP_FIN_WAIT_1;
        output(t, s, 1);
        return;
    }

    /* the sequence check (RFC 9293 3.10.7.4) */
    wnd = rcv_wnd(s);
    {
        int ok;
        if (seglen == 0)
            ok = wnd == 0 ? seq == s->rcv_nxt : le(s->rcv_nxt, seq) && lt(seq, s->rcv_nxt + wnd);
        else if (wnd == 0)
            ok = 0;
        else
            ok = (le(s->rcv_nxt, seq) && lt(seq, s->rcv_nxt + wnd)) ||
                 (le(s->rcv_nxt, seq + seglen - 1) && lt(seq + seglen - 1, s->rcv_nxt + wnd));
        if (seq == s->rcv_nxt)
            ok = 1;                             /* zero-window probes and pure ACKs at the edge */
        if (!ok) {
            t->rx_dropped++;
            if (!(fl & F_RST))
                ack_now(t, s);
            return;
        }
    }
    if (fl & F_RST) {
        if (seq == s->rcv_nxt) {
            if (s->state == SBTCP_SYN_RCVD && s->listener >= 0) {
                s->state = SBTCP_FREE;          /* back to listening only */
                return;
            }
            end(s, SBTCP_CLOSED, s->state == SBTCP_TIME_WAIT ? 0 : SBTCP_E_RESET);
        } else {
            ack_now(t, s);                      /* RFC 5961's challenge ACK */
        }
        return;
    }
    if (fl & F_SYN) {
        if (s->state == SBTCP_SYN_RCVD && seq + 1 == s->rcv_nxt)
            seg_out(t, s, s->iss, F_SYN, 0, 0); /* our SYN-ACK was lost */
        else
            ack_now(t, s);
        return;
    }
    if (!(fl & F_ACK)) {
        t->rx_dropped++;
        return;
    }
    s->retries = 0;                             /* the peer is there */
    if (s->state == SBTCP_SYN_RCVD) {
        if (!(lt(s->snd_una, ack) && le(ack, s->snd_max))) {
            reset_reply(t, from, h, plen);
            return;
        }
        s->state = SBTCP_ESTABLISHED;
        if (!s->srtt)
            rtt_sample(s, (uint32_t)(now(t) - s->syn_at));
        s->rtt_on = 0;
    }

    /* the acknowledgement */
    if (lt(s->snd_max, ack)) {
        t->rx_dropped++;
        ack_now(t, s);                          /* it acknowledges what we never sent */
        return;
    }
    if (lt(s->snd_una, ack)) {
        got_ack(t, s, ack);
    } else if (ack == s->snd_una && plen == 0 && !(fl & F_FIN) && win == s->snd_wnd && s->snd_una != s->snd_max) {
        if (++s->dupacks == 3) {
            /* fast retransmit: the first unacknowledged segment, then fast recovery until `recover` */
            uint32_t inflight = s->snd_max - s->snd_una;
            s->ssthresh = max32(inflight / 2, 2u * s->mss);
            s->cwnd = s->ssthresh + 3u * s->mss;
            s->recover = s->snd_max;
            s->rtt_on = 0;
            retransmit_first(t, s);
        } else if (s->dupacks > 3) {
            s->cwnd += s->mss;
        }
    }
    /* the send window (RFC 9293's SND.WL1 / SND.WL2) */
    if (lt(s->wl1, seq) || (s->wl1 == seq && le(s->wl2, ack))) {
        s->snd_wnd = win, s->wl1 = seq, s->wl2 = ack;
    }
    /* our FIN acknowledged */
    if (fin_acked(s)) {
        if (s->state == SBTCP_FIN_WAIT_1) {
            s->state = SBTCP_FIN_WAIT_2;
            s->tw_at = now(t) + FIN_WAIT_2_MS;
        } else if (s->state == SBTCP_CLOSING) {
            s->state = SBTCP_TIME_WAIT;
            s->tw_at = now(t) + TIME_WAIT_MS;
        } else if (s->state == SBTCP_LAST_ACK) {
            end(s, SBTCP_CLOSED, 0);
            return;
        }
    }

    /* the data, in order only */
    if (plen && (s->state == SBTCP_ESTABLISHED || s->state == SBTCP_FIN_WAIT_1 || s->state == SBTCP_FIN_WAIT_2)) {
        uint32_t skip = 0, take;
        if (lt(seq, s->rcv_nxt))
            skip = s->rcv_nxt - seq;
        if (skip < plen) {
            if (seq + skip != s->rcv_nxt) {
                /* after a gap: kept at its place, a duplicate ACK asks for the gap */
                if (!s->app_closed)
                    ooo_store(s, seq + skip, data + skip, plen - skip);
                t->rx_dropped++;
                ack_now(t, s);
                return;
            }
            take = plen - skip;
            if (s->app_closed) {
                s->rcv_nxt += take;             /* nobody reads it: acknowledged and dropped */
                s->ooo_n = 0;
            } else {
                take = min32(take, SBTCP_RXBUF - s->rx_len);
                for (uint32_t k = 0; k < take; k++)
                    s->rx[(s->rx_head + s->rx_len + k) % SBTCP_RXBUF] = data[skip + k];
                s->rx_len += take;
                s->rcv_nxt += take;
                ooo_join(s);
            }
        }
        need_ack = 1;
    } else if (plen) {
        need_ack = 1;
    }
    /* the peer's FIN: only once everything before it arrived */
    if ((fl & F_FIN) && seq + plen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_fin = 1;
        need_ack = 1;
        switch (s->state) {
        case SBTCP_ESTABLISHED:
            s->state = SBTCP_CLOSE_WAIT;
            break;
        case SBTCP_FIN_WAIT_1:
            s->state = fin_acked(s) ? SBTCP_TIME_WAIT : SBTCP_CLOSING;
            s->tw_at = now(t) + TIME_WAIT_MS;
            break;
        case SBTCP_FIN_WAIT_2:
            s->state = SBTCP_TIME_WAIT;
            s->tw_at = now(t) + TIME_WAIT_MS;
            break;
        default:
            break;
        }
    } else if ((fl & F_FIN) && s->state == SBTCP_TIME_WAIT) {
        need_ack = 1;                           /* our last ACK was lost */
        s->tw_at = now(t) + TIME_WAIT_MS;
    }
    output(t, s, need_ack);
}

/* ---- timers ------------------------------------------------------------------------------------------------------- */

void sbtcp_tick(struct sbtcp *t)
{
    uint64_t n = now(t);
    for (int i = 0; i < SBTCP_SOCKS; i++) {
        struct sbtcp_sock *s = &t->s[i];
        if (s->state == SBTCP_FREE || s->state == SBTCP_LISTEN)
            continue;
        if (s->state == SBTCP_CLOSED) {
            if (s->app_closed)
                s->state = SBTCP_FREE;
            continue;
        }
        if (s->state == SBTCP_TIME_WAIT || (s->state == SBTCP_FIN_WAIT_2 && s->app_closed)) {
            if (n >= s->tw_at)
                end(s, SBTCP_CLOSED, 0);
            continue;
        }
        if (s->rto_at && n >= s->rto_at) {
            unsigned limit = (s->state == SBTCP_SYN_SENT || s->state == SBTCP_SYN_RCVD) ? SYN_TRIES : DATA_TRIES;
            s->rto_at = 0;
            if (++s->retries > limit) {
                if (synced(s->state))
                    seg_out(t, s, s->snd_nxt, F_RST, 0, 0);
                if (s->state == SBTCP_SYN_RCVD && s->listener >= 0)
                    s->state = SBTCP_FREE;
                else
                    end(s, SBTCP_CLOSED, SBTCP_E_TIMEOUT);
                continue;
            }
            s->rto = min32(s->rto * 2, RTO_MAX);
            s->rtt_on = 0;
            t->retransmits++;
            if (s->state == SBTCP_SYN_SENT || s->state == SBTCP_SYN_RCVD) {
                seg_out(t, s, s->iss, F_SYN, 0, 0);
                timer_on(t, s);
                continue;
            }
            if (s->snd_wnd == 0) {
                /* the window is shut: probe it with the first unacknowledged byte (the peer's ACKs keep the
                 * connection alive: input() clears retries; a peer that is gone times out as for a loss) */
                uint32_t off = s->snd_una - tx_base(s);
                if (!lt(s->snd_una, tx_base(s)) && off < s->tx_len) {
                    seg_out(t, s, s->snd_una, 0, s->tx + off, 1);
                    s->snd_nxt = s->snd_una + 1;
                    if (lt(s->snd_max, s->snd_nxt))
                        s->snd_max = s->snd_nxt;
                } else {
                    s->snd_nxt = s->snd_una;    /* only the FIN: it needs no window */
                    output(t, s, 1);
                }
                timer_on(t, s);
                continue;
            }
            /* a loss: back to the first unacknowledged byte, one segment's window (RFC 5681 3.1) */
            s->ssthresh = max32((s->snd_max - s->snd_una) / 2, 2u * s->mss);
            s->cwnd = s->mss;
            s->dupacks = 0;
            s->snd_nxt = s->snd_una;
            output(t, s, 0);
            timer_on(t, s);
        }
    }
}

/* ---- the calls ---------------------------------------------------------------------------------------------------- */

void sbtcp_init(struct sbtcp *t, struct sbnet *net)
{
    set((uint8_t *)t, 0, (uint32_t)((uint8_t *)&t->s[0] - (uint8_t *)t));
    for (int i = 0; i < SBTCP_SOCKS; i++)
        t->s[i].state = SBTCP_FREE;
    t->rx_segments = t->rx_bad = t->rx_dropped = t->tx_segments = t->retransmits = t->resets_sent = 0;
    t->net = net;
    t->next_port = (uint16_t)(49152u + rnd(t) % 16384u);
}

void sbtcp_down(struct sbtcp *t)
{
    for (int i = 0; i < SBTCP_SOCKS; i++) {
        struct sbtcp_sock *s = &t->s[i];
        if (s->state != SBTCP_FREE && s->state != SBTCP_LISTEN && s->state != SBTCP_CLOSED)
            end(s, SBTCP_CLOSED, SBTCP_E_UNREACHABLE);
    }
}

static int port_used(struct sbtcp *t, uint16_t p)
{
    for (int i = 0; i < SBTCP_SOCKS; i++)
        if (t->s[i].state != SBTCP_FREE && t->s[i].lport == p)
            return 1;
    return 0;
}

int sbtcp_connect(struct sbtcp *t, const uint8_t ip[4], uint16_t port)
{
    int k = free_slot(t);
    struct sbtcp_sock *s;
    if (k < 0 || t->net->state < SBNET_BOUND || !port)
        return -1;
    s = &t->s[k];
    slot_init(t, s);
    cpy(s->rip, ip, 4);
    s->rport = port;
    do {
        t->next_port = (uint16_t)(t->next_port + 1);
        if (t->next_port < 49152)
            t->next_port = 49152;
    } while (port_used(t, t->next_port));
    s->lport = t->next_port;
    s->state = SBTCP_SYN_SENT;
    s->syn_at = now(t);
    seg_out(t, s, s->iss, F_SYN, 0, 0);
    timer_on(t, s);
    return k;
}

int sbtcp_listen(struct sbtcp *t, uint16_t port)
{
    int k = free_slot(t);
    if (k < 0 || !port || port_used(t, port))
        return -1;
    slot_init(t, &t->s[k]);
    t->s[k].state = SBTCP_LISTEN;
    t->s[k].lport = port;
    return k;
}

int sbtcp_accept(struct sbtcp *t, int l)
{
    for (int i = 0; i < SBTCP_SOCKS; i++) {
        struct sbtcp_sock *s = &t->s[i];
        if (s->listener == l && !s->accepted && s->state != SBTCP_FREE && s->state != SBTCP_LISTEN &&
            s->state != SBTCP_SYN_RCVD) {
            s->accepted = 1;
            return i;
        }
    }
    return -1;
}

static int valid(const struct sbtcp *t, int i)
{
    return i >= 0 && i < SBTCP_SOCKS && t->s[i].state != SBTCP_FREE && !t->s[i].app_closed;
}

int sbtcp_send(struct sbtcp *t, int i, const void *data, uint32_t len)
{
    struct sbtcp_sock *s;
    uint32_t n;
    if (!valid(t, i))
        return -1;
    s = &t->s[i];
    if (s->fin_queued || !(s->state == SBTCP_SYN_SENT || s->state == SBTCP_SYN_RCVD || s->state == SBTCP_ESTABLISHED ||
                           s->state == SBTCP_CLOSE_WAIT))
        return -1;
    n = min32(len, SBTCP_TXBUF - s->tx_len);
    cpy(s->tx + s->tx_len, (const uint8_t *)data, n);
    s->tx_len += n;
    output(t, s, 0);
    return (int)n;
}

int sbtcp_recv(struct sbtcp *t, int i, void *buf, uint32_t cap)
{
    struct sbtcp_sock *s;
    uint32_t n;
    uint8_t *b = (uint8_t *)buf;
    if (!valid(t, i))
        return -2;
    s = &t->s[i];
    if (!s->rx_len) {
        if (s->peer_fin)
            return -1;
        if (s->err || s->state == SBTCP_CLOSED)
            return -2;
        return 0;
    }
    n = min32(cap, s->rx_len);
    for (uint32_t k = 0; k < n; k++)
        b[k] = s->rx[(s->rx_head + k) % SBTCP_RXBUF];
    s->rx_head = (s->rx_head + n) % SBTCP_RXBUF;
    s->rx_len -= n;
    /* a window update when it opened a lot (or from almost shut) */
    if (synced(s->state) && !s->peer_fin) {
        uint32_t w = rcv_wnd(s);
        if (w >= s->rcv_wnd_sent + SBTCP_RXBUF / 4 || (s->rcv_wnd_sent < s->mss && w >= s->mss))
            ack_now(t, s);
    }
    return (int)n;
}

int sbtcp_state(const struct sbtcp *t, int i) { return i >= 0 && i < SBTCP_SOCKS ? t->s[i].state : SBTCP_FREE; }
int sbtcp_error(const struct sbtcp *t, int i) { return i >= 0 && i < SBTCP_SOCKS ? t->s[i].err : SBTCP_OK; }
uint32_t sbtcp_unacked(const struct sbtcp *t, int i) { return valid(t, i) ? t->s[i].tx_len : 0; }

void sbtcp_close(struct sbtcp *t, int i)
{
    struct sbtcp_sock *s;
    if (!valid(t, i))
        return;
    s = &t->s[i];
    s->app_closed = 1;
    s->rx_len = 0;
    switch (s->state) {
    case SBTCP_LISTEN:
    case SBTCP_SYN_SENT:
    case SBTCP_CLOSED:
        s->state = SBTCP_FREE;
        for (int k = 0; k < SBTCP_SOCKS; k++)
            if (t->s[k].listener == i && t->s[k].state == SBTCP_SYN_RCVD)
                t->s[k].state = SBTCP_FREE;
        return;
    case SBTCP_SYN_RCVD:
    case SBTCP_ESTABLISHED:
        s->fin_queued = 1;
        s->state = SBTCP_FIN_WAIT_1;
        break;
    case SBTCP_CLOSE_WAIT:
        s->fin_queued = 1;
        s->state = SBTCP_LAST_ACK;
        break;
    case SBTCP_FIN_WAIT_2:
        s->tw_at = now(t) + FIN_WAIT_2_MS;
        return;
    default:
        return;
    }
    output(t, s, 0);
}

void sbtcp_abort(struct sbtcp *t, int i)
{
    struct sbtcp_sock *s;
    if (i < 0 || i >= SBTCP_SOCKS || t->s[i].state == SBTCP_FREE)
        return;
    s = &t->s[i];
    if (synced(s->state) || s->state == SBTCP_SYN_RCVD)
        seg_out(t, s, s->snd_nxt, F_RST, 0, 0);
    s->state = SBTCP_FREE;
}
