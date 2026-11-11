/* SPDX-License-Identifier: MIT
 * sbtcp's host test (tcp.c): two hosts, each sbnet + sbtcp, on a simulated link that can lose, duplicate, delay,
 * reorder and corrupt frames. Bulk transfers both ways (checked byte for byte), a refused port, a peer that
 * vanishes, a receiver that stops reading (zero window), a reset, an aborted connection, then random segments
 * under ASan/UBSan. Real-stack interop is tcp-tap.c (in the VM, against Linux). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net.h"
#include "../tcp.h"

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
static uint64_t rs = 0x1234567;
static uint32_t r32(void)
{
    rs ^= rs << 13, rs ^= rs >> 7, rs ^= rs << 17;
    return (uint32_t)rs;
}
static double rf(void) { return (r32() >> 8) / 16777216.0; }

/* the link */
static struct {
    double loss, dup, corrupt;
    unsigned delay, jitter;
    int cut;                                    /* everything lost */
} L;
#define QN 4096
static struct fr { uint64_t at; int to; uint32_t len; uint8_t b[1600]; } q[QN];
static unsigned qn;
static int raw_link;                            /* the hosts are on a raw-IP link (mobile data): no Ethernet */
static unsigned oversize;                       /* raw IP: packets above the link's MTU (must stay 0) */

struct host {
    struct sbnet net;
    struct sbtcp tcp;
    int id;
};
static struct host H[2];

static void enqueue(int to, const uint8_t *f, uint32_t len)
{
    if (qn >= QN || len > 1600)
        return;
    q[qn].at = t_ms + L.delay + (L.jitter ? r32() % L.jitter : 0);
    q[qn].to = to;
    q[qn].len = len;
    memcpy(q[qn].b, f, len);
    if (L.corrupt && rf() < L.corrupt && len > 40)
        q[qn].b[(raw_link ? 20 : 34) + r32() % (len - (raw_link ? 20 : 34))] ^= (uint8_t)(1 + r32() % 255);
    qn++;
}

static int tx(void *ctx, const uint8_t *f, uint32_t len)
{
    struct host *h = ctx;
    if (raw_link && len > 1400)              /* either way: the peer must follow our MSS too */
        oversize++;
    if (L.cut || (L.loss && rf() < L.loss))
        return 0;
    enqueue(1 - h->id, f, len);
    if (L.dup && rf() < L.dup)
        enqueue(1 - h->id, f, len);
    return 0;
}
static uint64_t now_ms(void *ctx) { (void)ctx; return t_ms; }
static uint32_t rnd(void *ctx) { (void)ctx; return r32(); }
static void tcp_in(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len)
{
    struct host *h = ctx;
    sbtcp_input(&h->tcp, from, seg, len);
}

static const uint8_t ip_a[4] = { 10, 0, 0, 1 }, ip_b[4] = { 10, 0, 0, 2 }, mask[4] = { 255, 255, 255, 0 };
static const uint8_t none[4] = { 0, 0, 0, 0 };

static void setup(void)
{
    static const uint8_t mac[2][6] = { { 2, 0, 0, 0, 0, 1 }, { 2, 0, 0, 0, 0, 2 } };
    memset(&L, 0, sizeof(L));
    L.delay = 2;
    qn = 0;
    for (int i = 0; i < 2; i++) {
        struct sbnet_os os = { 0 };
        H[i].id = i;
        os.ctx = &H[i];
        os.tx = tx, os.now_ms = now_ms, os.random = rnd, os.tcp = tcp_in;
        static const uint8_t host32[4] = { 255, 255, 255, 255 };
        if (raw_link)
            sbnet_init_raw(&H[i].net, &os);
        else
            sbnet_init(&H[i].net, &os, mac[i]);
        sbnet_static(&H[i].net, i ? ip_b : ip_a, raw_link ? host32 : mask, none, none);
        if (raw_link)
            H[i].net.mtu = i ? 1500 : 1400;     /* an LTE-sized MTU on the phone's side, a full one at the peer */
        sbtcp_init(&H[i].tcp, &H[i].net);
    }
}

/* one millisecond: frames due are delivered (in queue order: jitter reorders), timers every 10 ms */
static void step(void)
{
    t_ms++;
    /* frames due now, in the order sent (jitter is what reorders) */
    for (;;) {
        unsigned k = qn;
        for (unsigned i = 0; i < qn; i++)
            if (q[i].at <= t_ms) {
                k = i;
                break;
            }
        if (k == qn)
            break;
        static struct fr f;
        f = q[k];
        memmove(&q[k], &q[k + 1], (qn - k - 1) * sizeof(q[0]));
        qn--;
        if (raw_link)
            sbnet_rx_ip(&H[f.to].net, f.b, f.len);
        else
            sbnet_rx(&H[f.to].net, f.b, f.len);
    }
    if (t_ms % 10 == 0) {
        sbtcp_tick(&H[0].tcp), sbtcp_tick(&H[1].tcp);
        sbnet_tick(&H[0].net), sbnet_tick(&H[1].net);
    }
}

/* ---- a bulk transfer both ways, closed cleanly ----------------------------------------------------------------- */

static uint8_t pat(uint32_t i, int dir) { return (uint8_t)(i * 2654435761u >> (dir ? 13 : 7)); }

static void bulk(const char *name, uint32_t size, uint64_t limit_ms)
{
    int l = sbtcp_listen(&H[1].tcp, 443), c, a = -1;
    uint32_t sent[2] = { 0, 0 }, got[2] = { 0, 0 }, bad[2] = { 0, 0 };
    int eof[2] = { 0, 0 }, closed[2] = { 0, 0 };
    uint64_t t0 = t_ms;
    uint8_t buf[3000];
    CHECK(l >= 0, "%s: listen", name);
    c = sbtcp_connect(&H[0].tcp, ip_b, 443);
    CHECK(c >= 0, "%s: connect", name);
    while (t_ms - t0 < limit_ms) {
        step();
        if (a < 0)
            a = sbtcp_accept(&H[1].tcp, l);
        int sk[2] = { c, a };
        for (int d = 0; d < 2; d++) {
            int s = sk[d];
            struct sbtcp *tp = &H[d].tcp;
            if (s < 0)
                continue;
            /* send in odd-sized pieces */
            while (sent[d] < size) {
                uint32_t n = 1 + r32() % 2999;
                if (n > size - sent[d])
                    n = size - sent[d];
                for (uint32_t k = 0; k < n; k++)
                    buf[k] = pat(sent[d] + k, d);
                int r = sbtcp_send(tp, s, buf, n);
                if (r <= 0)
                    break;
                sent[d] += (uint32_t)r;
            }
            for (;;) {
                int r = !closed[d] ? sbtcp_recv(tp, s, buf, 1 + r32() % 3000) : 0;
                if (r > 0) {
                    for (int k = 0; k < r; k++)
                        if (buf[k] != pat(got[d] + (uint32_t)k, 1 - d))
                            bad[d]++;
                    got[d] += (uint32_t)r;
                    continue;
                }
                if (r == -1)
                    eof[d] = 1;
                break;
            }
            /* each side closes once it has sent and received everything (its FIN after its last byte) */
            if (sent[d] == size && got[d] == size && !closed[d]) {
                sbtcp_close(tp, s);
                closed[d] = 1;
            }
        }
        if (closed[0] && closed[1] && H[0].tcp.s[c].state == SBTCP_FREE && H[1].tcp.s[a].state == SBTCP_FREE)
            break;
    }
    if (got[0] != size || got[1] != size) {
        for (int d = 0; d < 2; d++) {
            int k = d ? a : c;
            if (k < 0)
                continue;
            struct sbtcp_sock *s = &H[d].tcp.s[k];
            printf("  host %d: state %d una %u nxt %u max %u wnd %u cwnd %u ssthresh %u rcv_nxt %u rx %u tx %u rto %u "
                   "rto_at %llu (now %llu) retries %u sent %u got %u\n",
                   d, s->state, s->snd_una - s->iss, s->snd_nxt - s->iss, s->snd_max - s->iss, s->snd_wnd, s->cwnd,
                   s->ssthresh, s->rcv_nxt, s->rx_len, s->tx_len, s->rto, (unsigned long long)s->rto_at,
                   (unsigned long long)t_ms, s->retries, sent[d], got[d]);
        }
    }
    CHECK(got[0] == size && got[1] == size, "%s: received %u and %u of %u", name, got[0], got[1], size);
    CHECK(!bad[0] && !bad[1], "%s: %u and %u wrong bytes", name, bad[0], bad[1]);
    CHECK(closed[0] && closed[1], "%s: closed %d %d (eof %d %d)", name, closed[0], closed[1], eof[0], eof[1]);
    CHECK(c >= 0 && a >= 0 && H[0].tcp.s[c].state == SBTCP_FREE && H[1].tcp.s[a].state == SBTCP_FREE,
          "%s: both slots free at the end (states %d %d)", name, c >= 0 ? H[0].tcp.s[c].state : -1,
          a >= 0 ? H[1].tcp.s[a].state : -1);
    printf("tcp: %s: %u bytes each way in %llu ms; retransmits %u/%u, bad segments %u/%u\n", name, size,
           (unsigned long long)(t_ms - t0), H[0].tcp.retransmits, H[1].tcp.retransmits, H[0].tcp.rx_bad,
           H[1].tcp.rx_bad);
    sbtcp_close(&H[1].tcp, l);
}

static void run_to(int (*done)(void), uint64_t limit)
{
    uint64_t t0 = t_ms;
    while (t_ms - t0 < limit && !done())
        step();
}

static int c_g;
static int is_closed(void) { return sbtcp_state(&H[0].tcp, c_g) == SBTCP_CLOSED; }
static int is_est(void) { return sbtcp_state(&H[0].tcp, c_g) == SBTCP_ESTABLISHED; }

int main(void)
{
    /* 1. a clean link, then worse ones */
    setup();
    bulk("clean", 1u << 20, 120000);
    setup();
    L.loss = 0.05, L.dup = 0.02, L.jitter = 20;
    bulk("5% loss, duplicates, reordering", 300000, 600000);
    setup();
    L.loss = 0.15, L.corrupt = 0.05, L.jitter = 40, L.delay = 30;
    bulk("15% loss, corruption, 30-70 ms", 100000, 1200000);
    setup();
    L.delay = 150;
    bulk("a long path (300 ms round trip)", 200000, 600000);
    /* the same on a raw-IP link (the modem's PDP channel: bare IPv4, no ARP) */
    raw_link = 1;
    setup();
    bulk("raw IP (mobile data), clean", 1u << 20, 120000);
    setup();
    L.loss = 0.05, L.dup = 0.02, L.jitter = 20, L.delay = 25;
    bulk("raw IP, 5% loss, 25-45 ms (LTE-like)", 300000, 600000);
    CHECK(H[0].net.tx_frames > 0 && H[1].net.rx_ignored == 0 && H[0].net.held_len == 0, "raw IP: no ARP, nothing ignored");
    CHECK(oversize == 0, "raw IP: %u packets above the 1400 MTU (the MSS must follow it both ways)", oversize);
    raw_link = 0;

    /* 2. a closed port: refused */
    setup();
    c_g = sbtcp_connect(&H[0].tcp, ip_b, 81);
    run_to(is_closed, 5000);
    CHECK(sbtcp_state(&H[0].tcp, c_g) == SBTCP_CLOSED && sbtcp_error(&H[0].tcp, c_g) == SBTCP_E_REFUSED,
          "refused: state %d error %d", sbtcp_state(&H[0].tcp, c_g), sbtcp_error(&H[0].tcp, c_g));
    CHECK(sbtcp_recv(&H[0].tcp, c_g, 0, 0) == -2 && sbtcp_send(&H[0].tcp, c_g, "x", 1) == -1, "refused: no I/O");
    sbtcp_close(&H[0].tcp, c_g);
    CHECK(sbtcp_state(&H[0].tcp, c_g) == SBTCP_FREE, "refused: closed frees the slot");

    /* 3. a peer that vanishes: SYN timeout, then a data timeout */
    setup();
    L.cut = 1;
    c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
    run_to(is_closed, 300000);
    CHECK(sbtcp_error(&H[0].tcp, c_g) == SBTCP_E_TIMEOUT, "no answer to SYN: error %d after %llu s",
          sbtcp_error(&H[0].tcp, c_g), (unsigned long long)t_ms / 1000);
    setup();
    {
        int l = sbtcp_listen(&H[1].tcp, 443);
        c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
        run_to(is_est, 5000);
        CHECK(is_est() && l >= 0, "vanish: established first");
        L.cut = 1;
        sbtcp_send(&H[0].tcp, c_g, "hello", 5);
        uint64_t t0 = t_ms;
        run_to(is_closed, 900000);
        CHECK(sbtcp_error(&H[0].tcp, c_g) == SBTCP_E_TIMEOUT, "vanish: error %d", sbtcp_error(&H[0].tcp, c_g));
        printf("tcp: a vanished peer timed out after %llu s\n", (unsigned long long)(t_ms - t0) / 1000);
    }

    /* 4. a receiver that stops reading: the window shuts, probes, then everything arrives */
    setup();
    {
        int l = sbtcp_listen(&H[1].tcp, 443), a = -1;
        static uint8_t big[100000], back[100000];
        uint32_t sent = 0, got = 0;
        for (uint32_t i = 0; i < sizeof(big); i++)
            big[i] = pat(i, 0);
        c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
        for (int i = 0; i < 20000; i++) {
            step();
            if (a < 0)
                a = sbtcp_accept(&H[1].tcp, l);
            if (sent < sizeof(big)) {
                int r = sbtcp_send(&H[0].tcp, c_g, big + sent, sizeof(big) - sent);
                if (r > 0)
                    sent += (uint32_t)r;
            }
        }
        CHECK(a >= 0 && H[0].tcp.s[c_g].snd_wnd == 0, "zero window: the window shut (snd_wnd %u)",
              H[0].tcp.s[c_g].snd_wnd);
        CHECK(H[1].tcp.s[a].rx_len == SBTCP_RXBUF, "zero window: the receiver holds a full buffer (%u)",
              H[1].tcp.s[a].rx_len);
        for (int i = 0; i < 200000 && got < sizeof(big); i++) {
            step();
            if (sent < sizeof(big)) {
                int r = sbtcp_send(&H[0].tcp, c_g, big + sent, sizeof(big) - sent);
                if (r > 0)
                    sent += (uint32_t)r;
            }
            if (i % 50 == 0) {
                int r = sbtcp_recv(&H[1].tcp, a, back + got, sizeof(back) - got);
                if (r > 0)
                    got += (uint32_t)r;
            }
        }
        CHECK(got == sizeof(big) && !memcmp(big, back, sizeof(big)), "zero window: all %u bytes after reading resumed",
              got);
    }

    /* 5. a reset: the exact sequence number ends the connection, another one only gets a challenge ACK */
    setup();
    {
        int l = sbtcp_listen(&H[1].tcp, 443), a;
        c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
        run_to(is_est, 5000);
        for (int i = 0; i < 20; i++)
            step();
        a = sbtcp_accept(&H[1].tcp, l);
        CHECK(a >= 0, "reset: accepted");
        uint8_t seg[20] = { 0 };
        struct sbtcp_sock *s = &H[0].tcp.s[c_g];
        uint32_t tx0 = H[0].tcp.tx_segments;
        /* a forged RST one byte off: dropped with a challenge ACK */
        seg[0] = 443 >> 8, seg[1] = 443 & 255, seg[2] = (uint8_t)(s->lport >> 8), seg[3] = (uint8_t)s->lport;
        uint32_t sq = s->rcv_nxt + 1;
        seg[4] = (uint8_t)(sq >> 24), seg[5] = (uint8_t)(sq >> 16), seg[6] = (uint8_t)(sq >> 8), seg[7] = (uint8_t)sq;
        seg[12] = 0x50, seg[13] = 0x04;
        {
            uint32_t c = 0;
            const uint8_t *ps[2] = { ip_b, ip_a };
            for (int k = 0; k < 2; k++)
                c += (uint32_t)(ps[k][0] << 8 | ps[k][1]) + (uint32_t)(ps[k][2] << 8 | ps[k][3]);
            c += 6 + 20;
            for (int k = 0; k < 20; k += 2)
                c += (uint32_t)(seg[k] << 8 | seg[k + 1]);
            while (c >> 16)
                c = (c & 0xffff) + (c >> 16);
            c = ~c & 0xffff;
            seg[16] = (uint8_t)(c >> 8), seg[17] = (uint8_t)c;
        }
        sbtcp_input(&H[0].tcp, ip_b, seg, 20);
        CHECK(s->state == SBTCP_ESTABLISHED && H[0].tcp.tx_segments == tx0 + 1, "reset off by one: kept, a challenge");
        /* the peer aborts: a real RST */
        sbtcp_abort(&H[1].tcp, a);
        run_to(is_closed, 1000);
        CHECK(sbtcp_error(&H[0].tcp, c_g) == SBTCP_E_RESET, "reset: error %d", sbtcp_error(&H[0].tcp, c_g));
    }

    /* 6. random segments (and mangled real ones) at an established connection, under ASan/UBSan */
    setup();
    {
        int l = sbtcp_listen(&H[1].tcp, 443);
        c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
        run_to(is_est, 5000);
        (void)l;
        uint8_t seg[1600];
        for (int i = 0; i < 200000; i++) {
            uint32_t n = r32() % 1600;
            for (uint32_t k = 0; k < n; k++)
                seg[k] = (uint8_t)r32();
            if (n >= 20 && (i & 1)) {
                /* aimed at the connection, with a valid checksum, so the state machine sees it */
                struct sbtcp_sock *s = &H[0].tcp.s[c_g];
                seg[0] = 443 >> 8, seg[1] = 443 & 255, seg[2] = (uint8_t)(s->lport >> 8), seg[3] = (uint8_t)s->lport;
                seg[12] = (uint8_t)((5 + r32() % 11) << 4);
                if (i & 2) {
                    uint32_t sq = s->rcv_nxt + (r32() % 3 ? 0 : r32() % 100000);
                    seg[4] = (uint8_t)(sq >> 24), seg[5] = (uint8_t)(sq >> 16), seg[6] = (uint8_t)(sq >> 8), seg[7] = (uint8_t)sq;
                }
                seg[16] = seg[17] = 0;
                uint32_t c = (10 << 8 | 0) + (0 << 8 | 2) + (10 << 8 | 0) + (0 << 8 | 1) + 6 + n;
                for (uint32_t k = 0; k + 1 < n; k += 2)
                    c += (uint32_t)(seg[k] << 8 | seg[k + 1]);
                if (n & 1)
                    c += (uint32_t)seg[n - 1] << 8;
                while (c >> 16)
                    c = (c & 0xffff) + (c >> 16);
                c = ~c & 0xffff;
                seg[16] = (uint8_t)(c >> 8), seg[17] = (uint8_t)c;
            }
            sbtcp_input(&H[0].tcp, ip_b, seg, n);
            if (i % 100 == 0)
                step();
            if (sbtcp_state(&H[0].tcp, c_g) != SBTCP_ESTABLISHED && sbtcp_state(&H[0].tcp, c_g) != SBTCP_CLOSE_WAIT) {
                sbtcp_abort(&H[0].tcp, c_g);
                c_g = sbtcp_connect(&H[0].tcp, ip_b, 443);
                run_to(is_est, 5000);
            }
            /* the invariants */
            for (int k = 0; k < SBTCP_SOCKS; k++) {
                struct sbtcp_sock *s = &H[0].tcp.s[k];
                if (s->state == SBTCP_FREE)
                    continue;
                if (s->rx_len > SBTCP_RXBUF || s->tx_len > SBTCP_TXBUF ||
                    (int32_t)(s->snd_nxt - s->snd_una) < 0 || (int32_t)(s->snd_max - s->snd_nxt) < 0) {
                    CHECK(0, "fuzz: an invariant broke at %d (rx %u tx %u una %u nxt %u max %u)", i, s->rx_len,
                          s->tx_len, s->snd_una, s->snd_nxt, s->snd_max);
                    i = 1 << 30;
                    break;
                }
            }
        }
        printf("tcp: 200000 random segments: %u bad, %u dropped, %u resets sent\n", H[0].tcp.rx_bad,
               H[0].tcp.rx_dropped, H[0].tcp.resets_sent);
    }

    printf("tcp: %u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
