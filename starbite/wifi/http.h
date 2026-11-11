/* SPDX-License-Identifier: MIT
 * sbhttp: one HTTP/1.1 GET at a time, over TLS 1.2 (BearSSL, starbite/wifi/tls) for https:// URLs or plain TCP for
 * http://, non-blocking: sbhttp_start() then sbhttp_poll() from the OS's loop until it is no longer SBHTTP_BUSY.
 * Certificates are checked against the trust anchors in tls/ta.c (the CA roots, made by tls/mk-ta.sh) at the OS's
 * time: with no known time the request fails (SBHTTP_E_CLOCK) rather than skip the check. Redirects (301, 302,
 * 303, 307, 308) are followed up to 3 times. The body goes to the caller's buffer (cut at its size; the full
 * length is counted); chunked and Content-Length bodies and bodies to the end of the connection are understood.
 *
 * The network is the caller's (struct sbhttp_net): sbnet + sbtcp on the phone (netio.c), BSD sockets in the host
 * tests. Freestanding C: no C library, no allocation; `struct sbhttp` is about 52 KiB.
 * Logged (os->log): steps, times, sizes, the TLS version and cipher, error codes. Never the URL's path or query,
 * never addresses; the host name only if the caller allows it (log_host). */
#ifndef SBHTTP_H
#define SBHTTP_H
#include <stdint.h>
#include "bearssl.h"

struct sbhttp_net {
    void *ctx;
    /* the name's IPv4 address: 1 done (ip set), 0 not yet (called again), -1 failed */
    int (*resolve)(void *ctx, const char *host, uint8_t ip[4]);
    /* a TCP connection: a handle >= 0, or -1 */
    int (*connect)(void *ctx, const uint8_t ip[4], uint16_t port);
    /* 0 connecting, 1 connected, -1 failed (refused, unreachable, timed out) */
    int (*connected)(void *ctx, int h);
    /* bytes taken (0..n), -1 the connection cannot send */
    int (*send)(void *ctx, int h, const uint8_t *d, uint32_t n);
    /* bytes read (1..cap), 0 none yet, -1 the peer ended the stream, -2 an error */
    int (*recv)(void *ctx, int h, uint8_t *d, uint32_t cap);
    void (*close)(void *ctx, int h);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*unix_time)(void *ctx);       /* seconds since 1970 (UTC), 0 not known */
    void (*log)(void *ctx, const char *line);   /* optional */
    int log_host;                           /* the host name may appear in the log */
    const br_x509_trust_anchor *tas;        /* optional: other trust anchors (tests); default sbtls_tas */
    unsigned tas_num;
};

enum {
    SBHTTP_BUSY = 0,
    SBHTTP_DONE = 1,                        /* a complete answer (any status: see status) */
    SBHTTP_E_URL = -1,                      /* not an http(s) URL this understands */
    SBHTTP_E_DNS = -2,
    SBHTTP_E_CONNECT = -3,
    SBHTTP_E_TLS = -4,                      /* the handshake or the record layer failed: see tls_err */
    SBHTTP_E_CERT = -5,                     /* the server's certificate was not accepted: see tls_err */
    SBHTTP_E_CLOCK = -6,                    /* https needs the time (certificates' validity) */
    SBHTTP_E_PROTOCOL = -7,                 /* the answer is not HTTP as understood here */
    SBHTTP_E_CLOSED = -8,                   /* the connection ended before the answer did */
    SBHTTP_E_TIMEOUT = -9,
    SBHTTP_E_REDIRECTS = -10,               /* more than 3 */
    SBHTTP_E_ABORTED = -11,
    SBHTTP_E_NETWORK = -12,                 /* the connection failed after it was made */
};

#define SBHTTP_URL_MAX 512
#define SBHTTP_HEAD_MAX 8192

struct sbhttp {
    struct sbhttp_net net;
    int state, result, conn, tls, redirects;
    char url[SBHTTP_URL_MAX];
    char host[256];
    const char *path;                       /* within url */
    uint16_t port;
    uint8_t ip[4];
    uint64_t t_start, t_phase, deadline, timeout_ms;
    /* what came back */
    int status;                             /* the HTTP status code */
    int tls_err;                            /* BearSSL's error (BR_ERR_*), 0 none */
    unsigned tls_version, tls_suite;        /* the negotiated protocol (0x0303 = TLS 1.2) and cipher suite */
    uint32_t dns_ms, connect_ms, tls_ms, first_byte_ms, total_ms;
    uint64_t body_total;                    /* the body's full length */
    uint32_t body_len, body_cap;            /* what is in body (at most body_cap) */
    uint8_t *body;
    int close_notify;                       /* the server ended TLS properly (when the body ran to the end) */
    /* the response parser */
    uint8_t head[SBHTTP_HEAD_MAX];
    uint32_t head_len;
    int64_t content_length;                 /* -1 none */
    int chunked, chunk_state, eof_body, no_body;
    uint64_t chunk_left;
    char location[SBHTTP_URL_MAX];
    /* the request */
    char req[SBHTTP_URL_MAX + 384];
    uint32_t req_len, req_sent;
    /* TLS */
    uint8_t seed[32];
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    uint8_t iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* a GET of url into body (body_cap bytes); seed: 32 unpredictable bytes (TLS's randomness; see the OS's entropy
 * notes); timeout_ms for the whole request (0: 30 s). SBHTTP_BUSY, or an error at once (bad URL). */
int sbhttp_start(struct sbhttp *h, const struct sbhttp_net *net, const char *url, uint8_t *body, uint32_t body_cap,
                 const uint8_t seed[32], uint32_t timeout_ms);
/* the work that can be done now: SBHTTP_BUSY, or the result (stays the result) */
int sbhttp_poll(struct sbhttp *h);
void sbhttp_abort(struct sbhttp *h);
/* a short English text for a result (SBHTTP_*) */
const char *sbhttp_why(int result);

/* tls/ta.c */
extern const br_x509_trust_anchor sbtls_tas[];
extern const unsigned sbtls_tas_num;

#endif
