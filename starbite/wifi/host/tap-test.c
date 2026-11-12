/* SPDX-License-Identifier: MIT
 * The phone's network stack end to end on Linux (run by host/run-tap.sh, as root, in the VM): sbnet + sbtcp +
 * netio + sbhttp + BearSSL exchange real Ethernet frames through a TAP device with Linux's own stack, which is
 * the peer (TCP), the router (NAT to the Internet) and the servers' host. Frames can be dropped on the way in
 * and out (TAP_LOSS=0.05) to make Linux and sbtcp retransmit.
 *   tap-test <tap name> <dns ip> <bulk port> <http port> <tls port> <test CA .der> [live]
 * Addresses: 10.77.0.2/24 for sbnet, 10.77.0.1 Linux. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "../net.h"
#include "../tcp.h"
#include "../netio.h"
#include "../http.h"

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

static int tap;
static double loss;
static struct sbnet net;
static struct sbtcp tcp;
static struct sbnetio io;
static int verbose;
static unsigned dropped;

static uint64_t ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
static int lose(void) { return loss > 0 && (double)random() / RAND_MAX < loss && ++dropped; }

static int n_tx(void *ctx, const uint8_t *f, uint32_t len)
{
    (void)ctx;
    if (lose())
        return 0;
    return write(tap, f, len) == (ssize_t)len ? 0 : -1;
}
static uint64_t n_now(void *ctx) { (void)ctx; return ms(); }
static uint32_t n_rnd(void *ctx)
{
    uint32_t r;
    (void)ctx;
    if (getrandom(&r, sizeof(r), 0) != sizeof(r))
        r = (uint32_t)random();
    return r;
}
static void n_log(void *ctx, const char *l)
{
    (void)ctx;
    if (verbose)
        printf("    %s\n", l);
}
static void n_tcp(void *ctx, const uint8_t from[4], const uint8_t *s, uint32_t len)
{
    (void)ctx;
    sbtcp_input(&tcp, from, s, len);
}
static uint64_t n_unix(void *ctx) { (void)ctx; return (uint64_t)time(0); }

/* frames in, timers: up to `wait` ms */
static void pump(int wait)
{
    static uint64_t last_tick;
    struct pollfd p = { tap, POLLIN, 0 };
    uint8_t f[2048];
    if (poll(&p, 1, wait) > 0) {
        for (;;) {
            ssize_t n = read(tap, f, sizeof(f));
            if (n <= 0)
                break;
            if (!lose())
                sbnet_rx(&net, f, (uint32_t)n);
        }
    }
    if (ms() - last_tick >= 10) {
        last_tick = ms();
        sbtcp_tick(&tcp);
        sbnet_tick(&net);
    }
}

static const uint8_t lin[4] = { 10, 77, 0, 1 };

/* sbtcp alone: N bytes up, the server's SHA-256 of them back, M bytes down */
static void bulk(uint16_t port, uint32_t up, uint32_t down)
{
    static uint8_t out[8 << 20], in[8 << 20];
    br_sha256_context sc;
    uint8_t want[32], hdr[8];
    uint32_t sent = 0, got = 0, i;
    int c = sbtcp_connect(&tcp, lin, port), eof = 0;
    uint64_t t0 = ms();
    for (i = 0; i < up; i++)
        out[i] = (uint8_t)(i * 31 + (i >> 9));
    br_sha256_init(&sc);
    br_sha256_update(&sc, out, up);
    br_sha256_out(&sc, want);
    hdr[0] = (uint8_t)(up >> 24), hdr[1] = (uint8_t)(up >> 16), hdr[2] = (uint8_t)(up >> 8), hdr[3] = (uint8_t)up;
    hdr[4] = (uint8_t)(down >> 24), hdr[5] = (uint8_t)(down >> 16), hdr[6] = (uint8_t)(down >> 8), hdr[7] = (uint8_t)down;
    CHECK(c >= 0, "bulk: connect");
    while (ms() - t0 < 120000 && !eof) {
        pump(1);
        if (sbtcp_state(&tcp, c) == SBTCP_SYN_SENT)
            continue;
        if (sent < 8) {
            int r = sbtcp_send(&tcp, c, hdr + sent, 8 - sent);
            if (r > 0)
                sent += (uint32_t)r;
        }
        while (sent >= 8 && sent - 8 < up) {
            int r = sbtcp_send(&tcp, c, out + sent - 8, up - (sent - 8));
            if (r <= 0)
                break;
            sent += (uint32_t)r;
        }
        for (;;) {
            int r = sbtcp_recv(&tcp, c, in + got, sizeof(in) - got);
            if (r > 0) {
                got += (uint32_t)r;
                continue;
            }
            if (r < 0)
                eof = 1;
            break;
        }
    }
    {
        uint32_t bad = 0;
        CHECK(got == 32 + down, "bulk: %u bytes back of %u (state %d, error %d)", got, 32 + down, sbtcp_state(&tcp, c),
              sbtcp_error(&tcp, c));
        CHECK(got >= 32 && !memcmp(in, want, 32), "bulk: Linux's SHA-256 of what we sent matches");
        for (i = 0; i + 32 < got; i++)
            if (in[32 + i] != (uint8_t)(i * 17 + (i >> 11)))
                bad++;
        CHECK(!bad, "bulk: %u wrong bytes from Linux", bad);
        printf("tap: TCP with Linux: %u bytes up, %u down in %llu ms (loss %.0f%%: %u frames dropped; sbtcp "
               "retransmitted %u)\n", up, down, (unsigned long long)(ms() - t0), loss * 100, dropped, tcp.retransmits);
    }
    sbtcp_close(&tcp, c);
    for (int k = 0; k < 300; k++)
        pump(10);
    CHECK(sbtcp_state(&tcp, c) == SBTCP_FREE, "bulk: the slot is free after the close (state %d)", sbtcp_state(&tcp, c));
}

static struct sbhttp H;
static uint8_t body[1 << 20];
static br_x509_trust_anchor test_ta;
static uint8_t dn_buf[1024], key_copy[256];
static size_t dn_len;
static void dn_append(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    if (dn_len + n <= sizeof(dn_buf)) {
        memcpy(dn_buf + dn_len, d, n);
        dn_len += n;
    }
}
static void load_ta(const char *path)
{
    static uint8_t der[8192];
    br_x509_decoder_context dc;
    br_x509_pkey *pk;
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(der, 1, sizeof(der), f) : 0;
    if (f)
        fclose(f);
    br_x509_decoder_init(&dc, dn_append, 0);
    br_x509_decoder_push(&dc, der, n);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || pk->key_type != BR_KEYTYPE_EC)
        return;
    memcpy(key_copy, pk->key.ec.q, pk->key.ec.qlen);
    test_ta.dn.data = dn_buf, test_ta.dn.len = dn_len, test_ta.flags = BR_X509_TA_CA;
    test_ta.pkey.key_type = BR_KEYTYPE_EC;
    test_ta.pkey.key.ec.curve = pk->key.ec.curve, test_ta.pkey.key.ec.q = key_copy;
    test_ta.pkey.key.ec.qlen = pk->key.ec.qlen;
}

static int get(const char *url, int test_ca)
{
    struct sbhttp_net hn;
    uint8_t seed[32];
    int r;
    sbnetio_init(&io, &hn);
    hn.log_host = 1;
    if (test_ca)
        hn.tas = &test_ta, hn.tas_num = 1;
    if (getrandom(seed, sizeof(seed), 0) != sizeof(seed))
        return -100;
    memset(body, 0, sizeof(body));
    r = sbhttp_start(&H, &hn, url, body, sizeof(body) - 1, seed, 30000);
    while (r == SBHTTP_BUSY) {
        pump(1);
        r = sbhttp_poll(&H);
    }
    for (int k = 0; k < 50; k++)
        pump(2);                                /* the close's last segments */
    return r;
}

int main(int argc, char **argv)
{
    struct ifreq ifr;
    struct sbnet_os os = { 0 };
    uint8_t dns[4];
    static const uint8_t mac[6] = { 0x02, 0x5b, 0x00, 0x00, 0x00, 0x02 };
    static const uint8_t me[4] = { 10, 77, 0, 2 }, mask[4] = { 255, 255, 255, 0 };
    char u[256];
    int r;
    if (argc < 7) {
        printf("usage: tap-test <tap> <dns ip> <bulk port> <http port> <tls port> <test CA .der> [live]\n");
        return 2;
    }
    verbose = getenv("TAP_VERBOSE") != 0;
    loss = getenv("TAP_LOSS") ? atof(getenv("TAP_LOSS")) : 0;
    srandom((unsigned)ms());
    inet_pton(AF_INET, argv[2], dns);
    tap = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
    if (tap < 0 || ioctl(tap, TUNSETIFF, &ifr) < 0) {
        perror("tap");
        return 2;
    }
    os.tx = n_tx, os.now_ms = n_now, os.random = n_rnd, os.log = n_log, os.tcp = n_tcp;
    sbnet_init(&net, &os, mac);
    sbnet_static(&net, me, mask, lin, dns);
    sbtcp_init(&tcp, &net);
    io.net = &net, io.tcp = &tcp, io.unix_time = n_unix, io.log = n_log;
    load_ta(argv[6]);

    bulk((uint16_t)atoi(argv[3]), 5u << 20, 7u << 20);

    snprintf(u, sizeof(u), "http://10.77.0.1:%s/big", argv[4]);
    r = get(u, 0);
    CHECK(r == SBHTTP_DONE && H.body_total == 3000000, "http big: %d (%s) %llu bytes", r, sbhttp_why(r),
          (unsigned long long)H.body_total);
    snprintf(u, sizeof(u), "http://10.77.0.1:%s/chunked", argv[4]);
    r = get(u, 0);
    CHECK(r == SBHTTP_DONE && !strcmp((char *)body, "hello, chunked world"), "http chunked: %d", r);
    snprintf(u, sizeof(u), "https://localhost:%s/big", argv[5]);
    /* "localhost" resolves through sbnet's DNS only on the real network: use the address, with the name check */
    snprintf(u, sizeof(u), "https://10.77.0.1:%s/big", argv[5]);
    r = get(u, 1);
    CHECK(r == SBHTTP_E_CERT, "https to an address the certificate does not name: %d (BearSSL %d)", r, H.tls_err);

    if (argc > 7) {
        static const char *L[] = { "https://example.com/", "https://www.google.com/", "https://github.com/",
                                   "https://letsencrypt.org/", "https://www.wikipedia.org/" };
        for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++) {
            r = get(L[i], 0);
            CHECK(r == SBHTTP_DONE && H.status == 200, "live %s: %d (%s, BearSSL %d) status %d", L[i], r,
                  sbhttp_why(r), H.tls_err, H.status);
            printf("tap: live %-30s %-14s status %3d, %7llu bytes; DNS %4u, TCP %4u, TLS %4u, total %5u ms\n", L[i],
                   sbhttp_why(r), H.status, (unsigned long long)H.body_total, H.dns_ms, H.connect_ms, H.tls_ms,
                   H.total_ms);
        }
    }
    printf("tap: sbtcp: %u segments in, %u out, %u retransmitted, %u bad, %u dropped; %u frames lost on purpose\n",
           tcp.rx_segments, tcp.tx_segments, tcp.retransmits, tcp.rx_bad, tcp.rx_dropped, dropped);
    printf("tap: %u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
