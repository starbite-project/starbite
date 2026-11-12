/* sim_net: sbnet's stand-in for Track G's NAT (included by sim_v3.c inside sbsim.c: puts_, putu, now_ms, memcpy).
 * It answers the guest's IPv4 itself, whatever the destination address:
 *   - ICMP echo request -> echo reply (any address: "the internet" answers every ping);
 *   - UDP to port 7 -> the same datagram back (echo; the throughput test floods it);
 *   - UDP to 10.77.0.1:53 -> a DNS stub: every A query of class IN is answered with 10.77.0.1 (TTL 60), any other
 *     query with NOERROR and no answer;
 *   - UDP to 10.77.0.1:5 "flap MS" -> link 0 (sbg_net_config) for MS milliseconds, then link 1 again with the
 *     uplink switched (wifi <-> cell): the guest's test of a lost uplink and of a config change.
 * Everything else (TCP, IPv6, other ports) is counted and dropped. 300 ms after the first tick it configures the
 * link: sbg_net_config(1, WIFI, 10.77.0.2, 10.77.0.1, 10.77.0.1, 30). Packets are read and written byte-wise: the
 * MMU is off in sbsim (Device memory, no unaligned access). */
#include "../sbshm-net.h"
static const uint8_t sim_ip_guest[4] = {10, 77, 0, 2}, sim_ip_gw[4] = {10, 77, 0, 1};
static uint8_t sim_out[SBSHM_NET_MTU + 64] __attribute__((aligned(8)));
static struct {
    uint32_t in, icmp, udp_echo, dns, ctl, other, v6, sent, refused, bytes_in, bytes_out;
} sn, sn_said;
static uint64_t sim_net_t0, sim_net_said_t, sim_net_flap_end;
static int sim_net_configured, sim_net_uplink = SBSHM_NET_UPLINK_WIFI;

static unsigned sn_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void sn_put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static uint32_t sn_sum(uint32_t s, const uint8_t *p, unsigned n)
{
    unsigned i = 0;
    for (; i + 1 < n; i += 2)
        s += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1U)
        s += (uint32_t)p[n - 1] << 8;
    return s;
}
static unsigned sn_fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xffffU) + (s >> 16);
    return ~s & 0xffffU;
}
static int sn_ip_eq(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static void sim_net_link(int link)
{
    sbg_net_config(link, link ? sim_net_uplink : SBSHM_NET_UPLINK_NONE, sim_ip_guest, sim_ip_gw, sim_ip_gw, 30);
}

/* the IPv4 header of a reply in sim_out (no options): from src to dst, proto, l4 bytes after it; then send */
static void sn_send_ip(const uint8_t *src, const uint8_t *dst, unsigned proto, unsigned l4)
{
    static unsigned id;
    uint8_t *h = sim_out;
    unsigned tot = 20U + l4;
    h[0] = 0x45;
    h[1] = 0;
    sn_put16(h + 2, tot);
    sn_put16(h + 4, ++id);
    sn_put16(h + 6, 0);
    h[8] = 64;
    h[9] = (uint8_t)proto;
    sn_put16(h + 10, 0);
    for (unsigned i = 0; i < 4; ++i) {
        h[12 + i] = src[i];
        h[16 + i] = dst[i];
    }
    sn_put16(h + 10, sn_fold(sn_sum(0, h, 20)));
    if (sbg_net_send(sim_out, tot)) {
        sn.sent++;
        sn.bytes_out += tot;
    } else {
        sn.refused++;
    }
}

/* a UDP reply from src:sport to dst:dport with n payload bytes already at sim_out + 28 */
static void sn_send_udp(const uint8_t *src, unsigned sport, const uint8_t *dst, unsigned dport, unsigned n)
{
    uint8_t *u = sim_out + 20;
    sn_put16(u, sport);
    sn_put16(u + 2, dport);
    sn_put16(u + 4, 8U + n);
    sn_put16(u + 6, 0);
    uint32_t s = sn_sum(0, src, 4);                 /* the pseudo header */
    s = sn_sum(s, dst, 4);
    s += 17U + 8U + n;
    unsigned c = sn_fold(sn_sum(s, u, 8U + n));
    sn_put16(u + 6, c ? c : 0xffffU);
    sn_send_ip(src, dst, 17, 8U + n);
}

/* the DNS stub: q = the query (n bytes); the answer goes to sim_out + 28 */
static void sn_dns(const uint8_t *q, unsigned n, const uint8_t *client, unsigned cport)
{
    if (n < 12 || (q[2] & 0x80U) || sn_be16(q + 4) != 1)
        return;
    unsigned o = 12;
    while (o < n && q[o]) {                         /* the question's name: labels up to the root */
        if ((q[o] & 0xc0U) || o + 1U + q[o] >= n)
            return;
        o += 1U + q[o];
    }
    if (o + 5U > n)
        return;
    unsigned qtype = sn_be16(q + o + 1), qclass = sn_be16(q + o + 3), qend = o + 5U;
    int a = qtype == 1 && qclass == 1;
    if (qend + 16U > sizeof(sim_out) - 28U)
        return;
    uint8_t *r = sim_out + 28;
    memcpy(r, q, qend);                             /* the header and the question */
    r[2] = (uint8_t)(0x80U | (q[2] & 0x79U));       /* QR, the opcode and RD back */
    r[3] = 0x80;                                    /* RA, NOERROR */
    sn_put16(r + 6, a ? 1U : 0U);
    sn_put16(r + 8, 0);
    sn_put16(r + 10, 0);
    unsigned m = qend;
    if (a) {
        static const uint8_t ans[16] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 77, 0, 1};
        memcpy(r + m, ans, sizeof(ans));
        m += sizeof(ans);
    }
    sn.dns++;
    sn_send_udp(sim_ip_gw, 53, client, cport, m);
}

static void sim_net_rx(const uint8_t *p, unsigned len)
{
    sn.in++;
    sn.bytes_in += len;
    if (len < 20 || (p[0] >> 4) != 4) {
        if (len && (p[0] >> 4) == 6)
            sn.v6++;
        else
            sn.other++;
        return;
    }
    unsigned ihl = (p[0] & 15U) * 4U, tot = sn_be16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || (sn_be16(p + 6) & 0x3fffU)) {   /* no fragments here */
        sn.other++;
        return;
    }
    const uint8_t *src = p + 12, *dst = p + 16, *l4 = p + ihl;
    unsigned n = tot - ihl;
    if (p[9] == 1 && n >= 8 && l4[0] == 8) {        /* ICMP echo request */
        uint8_t *ic = sim_out + 20;
        memcpy(ic, l4, n);
        ic[0] = 0;
        sn_put16(ic + 2, 0);
        sn_put16(ic + 2, sn_fold(sn_sum(0, ic, n)));
        sn.icmp++;
        sn_send_ip(dst, src, 1, n);
        return;
    }
    if (p[9] == 17 && n >= 8) {
        unsigned sport = sn_be16(l4), dport = sn_be16(l4 + 2), ul = sn_be16(l4 + 4);
        if (ul < 8 || ul > n) {
            sn.other++;
            return;
        }
        const uint8_t *pl = l4 + 8;
        unsigned pn = ul - 8U;
        if (dport == 7) {
            memcpy(sim_out + 28, pl, pn);
            sn.udp_echo++;
            sn_send_udp(dst, 7, src, sport, pn);
            return;
        }
        if (dport == 53 && sn_ip_eq(dst, sim_ip_gw)) {
            sn_dns(pl, pn, src, sport);
            return;
        }
        if (dport == 5 && sn_ip_eq(dst, sim_ip_gw) && pn > 5 && pl[0] == 'f' && pl[1] == 'l' && pl[2] == 'a' &&
            pl[3] == 'p' && pl[4] == ' ') {
            uint64_t ms = 0;
            for (unsigned i = 5; i < pn && pl[i] >= '0' && pl[i] <= '9'; ++i)
                ms = ms * 10U + (uint64_t)(pl[i] - '0');
            if (ms > 60000)
                ms = 60000;
            sn.ctl++;
            puts_("sbsim: sim_net: the guest asked for a link flap of ");
            putu(ms);
            puts_(" ms\n");
            sim_net_uplink = sim_net_uplink == SBSHM_NET_UPLINK_WIFI ? SBSHM_NET_UPLINK_CELL : SBSHM_NET_UPLINK_WIFI;
            sim_net_link(0);
            sim_net_flap_end = now_ms() + ms;
            return;
        }
    }
    sn.other++;
}

static void sim_net_tick(uint64_t t)
{
    if (!sim_net_t0)
        sim_net_t0 = t ? t : 1;
    if (!sim_net_configured && t - sim_net_t0 >= 300) {
        sim_net_configured = 1;
        sim_net_link(1);
    }
    if (sim_net_flap_end && t >= sim_net_flap_end) {
        sim_net_flap_end = 0;
        sim_net_link(1);
    }
    if (t - sim_net_said_t >= 5000 && sn.in != sn_said.in) {   /* what the stand-in saw, every 5 s while busy */
        sim_net_said_t = t;
        sn_said = sn;
        puts_("sbsim: sim_net: from the guest ");
        putu(sn.in);
        puts_(" packets (");
        putu(sn.bytes_in);
        puts_(" bytes): icmp echo ");
        putu(sn.icmp);
        puts_(", udp echo ");
        putu(sn.udp_echo);
        puts_(", dns ");
        putu(sn.dns);
        puts_(", ctl ");
        putu(sn.ctl);
        puts_(", ipv6 ");
        putu(sn.v6);
        puts_(", other ");
        putu(sn.other);
        puts_("; to the guest ");
        putu(sn.sent);
        puts_(" (");
        putu(sn.bytes_out);
        puts_(" bytes), refused ");
        putu(sn.refused);
        puts_("\n");
    }
}
