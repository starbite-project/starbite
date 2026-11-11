/* SPDX-License-Identifier: MIT
 * sbhttp's host test (http.c + BearSSL + tls/ta.c) on the host's own sockets (non-blocking, polled as on the
 * phone):
 *   1. a local server (host/http-serve.py) with crafted answers: Content-Length, chunked with extensions and
 *      trailers, to the end of the connection, 100 Continue, a redirect, a byte at a time, broken answers;
 *   2. the same over TLS with a test CA (host/http-serve.py --tls), its root given as the only trust anchor; a
 *      certificate for another name, then the real roots (which must refuse the test CA);
 *   3. with SBHTTP_LIVE=1: real sites (example.com, a Let's Encrypt site, a big chunked page, a redirect) and
 *      badssl.com's broken certificates (expired, wrong host, self-signed, untrusted root, TLS 1.0 only).
 * Usage: http-test <port> <port-tls> <test CA .der>   (run by host/run-http.sh) */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
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

static int verbose;
static uint64_t ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* ---- the host's sockets as sbhttp_net ------------------------------------------------------------------- */

static int fds[8];
static int h_resolve(void *ctx, const char *host, uint8_t ip[4])
{
    struct addrinfo hints = { 0 }, *r = 0;
    (void)ctx;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, 0, &hints, &r) || !r)
        return -1;
    memcpy(ip, &((struct sockaddr_in *)r->ai_addr)->sin_addr, 4);
    freeaddrinfo(r);
    return 1;
}
static int h_connect(void *ctx, const uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in a = { 0 };
    int fd = socket(AF_INET, SOCK_STREAM, 0), k;
    (void)ctx;
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    memcpy(&a.sin_addr, ip, 4);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    for (k = 0; k < 8; k++)
        if (!fds[k]) {
            fds[k] = fd;
            return k;
        }
    close(fd);
    return -1;
}
static int h_connected(void *ctx, int h)
{
    struct pollfd p = { fds[h], POLLOUT, 0 };
    int e = 0;
    socklen_t l = sizeof(e);
    (void)ctx;
    if (poll(&p, 1, 0) <= 0)
        return 0;
    getsockopt(fds[h], SOL_SOCKET, SO_ERROR, &e, &l);
    return e ? -1 : 1;
}
static int h_send(void *ctx, int h, const uint8_t *d, uint32_t n)
{
    ssize_t r = send(fds[h], d, n, 0);
    (void)ctx;
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return (int)r;
}
static int dribble;                             /* the reads one byte at a time */
static int h_recv(void *ctx, int h, uint8_t *d, uint32_t cap)
{
    ssize_t r = recv(fds[h], d, dribble ? 1 : cap, 0);
    (void)ctx;
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -2;
    if (r == 0)
        return -1;
    return (int)r;
}
static void h_close(void *ctx, int h)
{
    (void)ctx;
    close(fds[h]);
    fds[h] = 0;
}
static uint64_t h_now(void *ctx) { (void)ctx; return ms(); }
static uint64_t fake_time;
static uint64_t h_unix(void *ctx) { (void)ctx; return fake_time ? fake_time : (uint64_t)time(0); }
static void h_log(void *ctx, const char *l)
{
    (void)ctx;
    if (verbose)
        printf("    %s\n", l);
}

static struct sbhttp H;
static uint8_t body[1 << 20];

static int get(const char *url, const br_x509_trust_anchor *tas, unsigned ntas)
{
    struct sbhttp_net net = { 0 };
    uint8_t seed[32];
    int r;
    net.resolve = h_resolve, net.connect = h_connect, net.connected = h_connected, net.send = h_send;
    net.recv = h_recv, net.close = h_close, net.now_ms = h_now, net.unix_time = h_unix, net.log = h_log;
    net.log_host = 1;
    net.tas = tas, net.tas_num = ntas;
    arc4random_buf(seed, sizeof(seed));
    memset(body, 0, sizeof(body));
    r = sbhttp_start(&H, &net, url, body, sizeof(body) - 1, seed, 20000);
    while (r == SBHTTP_BUSY) {
        usleep(200);
        r = sbhttp_poll(&H);
    }
    return r;
}

static int contains(const char *needle)
{
    return strstr((const char *)body, needle) != 0;
}

/* ---- a trust anchor from a DER certificate (the test CA) ----------------------------------------------- */

static uint8_t dn_buf[1024];
static size_t dn_len;
static void dn_append(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    if (dn_len + n <= sizeof(dn_buf)) {
        memcpy(dn_buf + dn_len, d, n);
        dn_len += n;
    }
}
static br_x509_trust_anchor test_ta;
static uint8_t key_copy[1024];
static int load_ta(const char *path)
{
    static uint8_t der[8192];
    br_x509_decoder_context dc;
    br_x509_pkey *pk;
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(der, 1, sizeof(der), f);
    fclose(f);
    br_x509_decoder_init(&dc, dn_append, 0);
    br_x509_decoder_push(&dc, der, n);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || pk->key_type != BR_KEYTYPE_EC || pk->key.ec.qlen > sizeof(key_copy))
        return -1;
    memcpy(key_copy, pk->key.ec.q, pk->key.ec.qlen);
    test_ta.dn.data = dn_buf, test_ta.dn.len = dn_len;
    test_ta.flags = BR_X509_TA_CA;
    test_ta.pkey.key_type = BR_KEYTYPE_EC;
    test_ta.pkey.key.ec.curve = pk->key.ec.curve;
    test_ta.pkey.key.ec.q = key_copy;
    test_ta.pkey.key.ec.qlen = pk->key.ec.qlen;
    return 0;
}

int main(int argc, char **argv)
{
    char u[256];
    int r, port, tport;
    verbose = getenv("SBHTTP_VERBOSE") != 0;
    if (argc < 4) {
        printf("usage: http-test <port> <tls port> <test CA .der>\n");
        return 2;
    }
    port = atoi(argv[1]), tport = atoi(argv[2]);
    CHECK(load_ta(argv[3]) == 0, "the test CA");

    /* 1. plain HTTP against crafted answers */
    {
        static const struct { const char *path; int result, status; const char *body; long total; } T[] = {
            { "/length", SBHTTP_DONE, 200, "hello, length", 13 },
            { "/chunked", SBHTTP_DONE, 200, "hello, chunked world", 20 },
            { "/eof", SBHTTP_DONE, 200, "to the end", 10 },
            { "/continue", SBHTTP_DONE, 200, "after continue", 14 },
            { "/redirect", SBHTTP_DONE, 200, "hello, length", 13 },
            { "/redirect-loop", SBHTTP_E_REDIRECTS, 302, 0, -1 },
            { "/no-content", SBHTTP_DONE, 204, "", 0 },
            { "/big", SBHTTP_DONE, 200, 0, 3000000 },
            { "/bad-status", SBHTTP_E_PROTOCOL, 0, 0, -1 },
            { "/bad-chunk", SBHTTP_E_PROTOCOL, 200, 0, -1 },
            { "/two-lengths", SBHTTP_E_PROTOCOL, 0, 0, -1 },
            { "/short", SBHTTP_E_CLOSED, 200, 0, -1 },
            { "/huge-head", SBHTTP_E_PROTOCOL, 0, 0, -1 },
            { "/silent", SBHTTP_E_TIMEOUT, 0, 0, -1 },
        };
        for (int d = 0; d < 2; d++) {
            dribble = d;
            for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
                if (d && (!strcmp(T[i].path, "/big") || !strcmp(T[i].path, "/silent")))
                    continue;
                snprintf(u, sizeof(u), "http://127.0.0.1:%d%s", port, T[i].path);
                r = get(u, 0, 0);
                CHECK(r == T[i].result, "%s%s: result %d (%s), wanted %d", T[i].path, d ? " (byte by byte)" : "", r,
                      sbhttp_why(r), T[i].result);
                if (r == SBHTTP_DONE || T[i].status) {
                    CHECK(H.status == T[i].status, "%s: status %d, wanted %d", T[i].path, H.status, T[i].status);
                }
                if (T[i].body)
                    CHECK(!strcmp((char *)body, T[i].body), "%s: body \"%.40s\"", T[i].path, (char *)body);
                if (T[i].total >= 0)
                    CHECK(H.body_total == (uint64_t)T[i].total, "%s: %llu bytes, wanted %ld", T[i].path,
                          (unsigned long long)H.body_total, T[i].total);
            }
        }
        dribble = 0;
        /* a body bigger than the buffer: cut, counted in full */
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/big", port);
        r = get(u, 0, 0);
        CHECK(r == SBHTTP_DONE && H.body_len == sizeof(body) - 1 && H.body_total == 3000000, "big: kept %u of %llu",
              H.body_len, (unsigned long long)H.body_total);
        CHECK(get("ftp://x/", 0, 0) == SBHTTP_E_URL && get("https://a b/", 0, 0) == SBHTTP_E_URL &&
              get("http://h:99999/", 0, 0) == SBHTTP_E_URL && get("http:///x", 0, 0) == SBHTTP_E_URL,
              "bad URLs refused");
        CHECK(get("http://127.0.0.1:1/", 0, 0) == SBHTTP_E_CONNECT, "a closed port");
        CHECK(get("http://no-such-name.invalid/", 0, 0) == SBHTTP_E_DNS, "an unknown name");
    }

    /* 2. TLS with the test CA */
    {
        snprintf(u, sizeof(u), "https://localhost:%d/chunked", tport);
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_DONE && H.status == 200 && !strcmp((char *)body, "hello, chunked world") &&
              H.tls_version == BR_TLS12, "TLS: result %d (%s, BearSSL %d) status %d", r, sbhttp_why(r), H.tls_err,
              H.status);
        printf("http: TLS to the test server: %u ms handshake, suite 0x%04x\n", H.tls_ms, H.tls_suite);
        snprintf(u, sizeof(u), "https://localhost:%d/big", tport);
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_DONE && H.body_total == 3000000, "TLS big: %d, %llu bytes", r,
              (unsigned long long)H.body_total);
        snprintf(u, sizeof(u), "https://localhost:%d/eof", tport);
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_DONE && !strcmp((char *)body, "to the end"), "TLS eof body: %d", r);
        /* the certificate is for localhost: another name is refused */
        snprintf(u, sizeof(u), "https://127.0.0.1:%d/length", tport);
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_E_CERT, "TLS wrong name: %d (BearSSL %d)", r, H.tls_err);
        /* the real roots do not know the test CA */
        snprintf(u, sizeof(u), "https://localhost:%d/length", tport);
        r = get(u, 0, 0);
        CHECK(r == SBHTTP_E_CERT, "TLS untrusted root: %d (BearSSL %d)", r, H.tls_err);
        /* no clock, a clock in 2050 (the certificate expired) */
        fake_time = 1;
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_E_CLOCK, "TLS without a clock: %d", r);
        fake_time = 2524608000ull;
        r = get(u, &test_ta, 1);
        CHECK(r == SBHTTP_E_CERT, "TLS in 2050: %d (BearSSL %d)", r, H.tls_err);
        fake_time = 0;
    }

    /* 3. the real Internet */
    if (getenv("SBHTTP_LIVE")) {
        static const struct { const char *url; int result, status; const char *needle; } L[] = {
            { "https://example.com/", SBHTTP_DONE, 200, "Example Domain" },
            { "http://example.com/", SBHTTP_DONE, 200, "Example Domain" },
            { "https://letsencrypt.org/", SBHTTP_DONE, 200, "Let's Encrypt" },
            { "https://valid-isrgrootx2.letsencrypt.org/", SBHTTP_DONE, 0, 0 },
            { "https://www.google.com/", SBHTTP_DONE, 200, "google" },
            { "https://github.com/", SBHTTP_DONE, 200, "GitHub" },
            { "https://www.wikipedia.org/", SBHTTP_DONE, 200, "Wikipedia" },
            { "https://www.cloudflare.com/cdn-cgi/trace", SBHTTP_DONE, 200, "tls=" },
            { "http://google.com/", SBHTTP_DONE, 200, "google" },
            { "https://expired.badssl.com/", SBHTTP_E_CERT, 0, 0 },
            { "https://wrong.host.badssl.com/", SBHTTP_E_CERT, 0, 0 },
            { "https://self-signed.badssl.com/", SBHTTP_E_CERT, 0, 0 },
            { "https://untrusted-root.badssl.com/", SBHTTP_E_CERT, 0, 0 },
            { "https://tls-v1-0.badssl.com:1010/", SBHTTP_E_TLS, 0, 0 },
            { "https://sha256.badssl.com/", SBHTTP_DONE, 200, "badssl" },
            { "https://ecc256.badssl.com/", SBHTTP_DONE, 200, "badssl" },
            { "https://rsa4096.badssl.com/", SBHTTP_DONE, 200, "badssl" },
        };
        for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++) {
            r = get(L[i].url, 0, 0);
            CHECK(r == L[i].result && (!L[i].status || H.status == L[i].status) && (!L[i].needle || contains(L[i].needle)),
                  "live %s: %d (%s, BearSSL %d), status %d", L[i].url, r, sbhttp_why(r), H.tls_err, H.status);
            printf("http: live %-44s %-30s status %3d, %7llu bytes, TLS %4u ms, total %5u ms\n", L[i].url,
                   sbhttp_why(r), H.status, (unsigned long long)H.body_total, H.tls_ms, H.total_ms);
        }
    }
    printf("http: %u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
