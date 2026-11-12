/* SPDX-License-Identifier: MIT
 * sbhttp (http.h): a GET over TLS 1.2 (BearSSL) or plain TCP. Written for Starbite; RFC 9110/9112 (HTTP/1.1),
 * BearSSL's client API. The response comes from the network: every length is bounded, every number checked. */
#include "http.h"

enum { S_IDLE, S_RESOLVE, S_CONNECT, S_TALK, S_DONE };
enum { CS_SIZE, CS_EXT, CS_SIZE_LF, CS_DATA, CS_DATA_CR, CS_DATA_LF, CS_TRAILER, CS_TRAILER_LF, CS_DONE };

/* ---- small helpers (no C library) ---------------------------------------------------------------------------- */

static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void mcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ieq(const char *a, uint32_t an, const char *b)
{
    uint32_t i;
    for (i = 0; i < an && b[i]; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return i == an && !b[i];
}

struct ub { char b[160]; uint32_t n; };
static void u_s(struct ub *u, const char *s)
{
    while (*s && u->n + 1 < sizeof(u->b))
        u->b[u->n++] = *s++;
    u->b[u->n] = 0;
}
static void u_d(struct ub *u, uint64_t v)
{
    char t[24];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i--) {
        char c[2] = { t[i], 0 };
        u_s(u, c);
    }
}
static void u_x(struct ub *u, uint32_t v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        char c[2] = { hx[(v >> (4 * i)) & 15], 0 };
        u_s(u, c);
    }
}

static uint64_t now(struct sbhttp *h) { return h->net.now_ms(h->net.ctx); }
static void say(struct sbhttp *h, struct ub *u)
{
    if (h->net.log)
        h->net.log(h->net.ctx, u->b);
}
static void says(struct sbhttp *h, const char *s)
{
    struct ub u = { .n = 0 };
    u_s(&u, "http: ");
    u_s(&u, s);
    say(h, &u);
}

const char *sbhttp_why(int r)
{
    switch (r) {
    case SBHTTP_BUSY: return "busy";
    case SBHTTP_DONE: return "done";
    case SBHTTP_E_URL: return "not a URL this understands";
    case SBHTTP_E_DNS: return "the name was not found";
    case SBHTTP_E_CONNECT: return "no connection to the server";
    case SBHTTP_E_TLS: return "the secure connection failed";
    case SBHTTP_E_CERT: return "the server's certificate was not accepted";
    case SBHTTP_E_CLOCK: return "the time is not known (needed to check certificates)";
    case SBHTTP_E_PROTOCOL: return "the answer was not understood";
    case SBHTTP_E_CLOSED: return "the server closed the connection early";
    case SBHTTP_E_TIMEOUT: return "no answer in time";
    case SBHTTP_E_REDIRECTS: return "too many redirects";
    case SBHTTP_E_ABORTED: return "stopped";
    case SBHTTP_E_NETWORK: return "the connection broke";
    default: return "?";
    }
}

/* ---- the URL ----------------------------------------------------------------------------------------------------- */

static int ipv4_literal(const char *s, uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) {
        unsigned v = 0, d = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++d > 3 || v > 255)
                return 0;
        }
        if (!d)
            return 0;
        ip[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    return *s == 0;
}

/* url (in h->url) into tls, host, port, path */
static int parse_url(struct sbhttp *h)
{
    const char *p = h->url;
    uint32_t n = 0;
    if (ieq(p, 8, "https://"))
        h->tls = 1, p += 8, h->port = 443;
    else if (ieq(p, 7, "http://"))
        h->tls = 0, p += 7, h->port = 80;
    else
        return -1;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        char c = *p++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ||
            n >= sizeof(h->host) - 1)
            return -1;                          /* no user info, no IPv6 literals, no IDN (punycode is fine) */
        h->host[n++] = lower(c);
    }
    h->host[n] = 0;
    if (!n)
        return -1;
    if (*p == ':') {
        unsigned v = 0, d = 0;
        p++;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p++ - '0');
            if (++d > 5 || v > 65535)
                return -1;
        }
        if (!d || !v)
            return -1;
        h->port = (uint16_t)v;
    }
    if (*p == '#')
        return -1;
    h->path = *p ? p : "/";
    for (const char *q = h->path; *q; q++)
        if ((unsigned char)*q <= ' ' || (unsigned char)*q >= 127)
            return -1;                          /* the request line would break */
    return 0;
}

static void build_request(struct sbhttp *h)
{
    uint32_t n = 0;
#define ADD(s)                                                  \
    do {                                                        \
        const char *s_ = (s);                                   \
        while (*s_ && n < sizeof(h->req) - 1)                   \
            h->req[n++] = *s_++;                                \
    } while (0)
    ADD("GET ");
    if (h->path[0] == '?')
        ADD("/");
    ADD(h->path);
    ADD(" HTTP/1.1\r\nHost: ");
    ADD(h->host);
    if ((h->tls && h->port != 443) || (!h->tls && h->port != 80)) {
        char pb[8];
        int i = 7;
        unsigned v = h->port;
        pb[i] = 0;
        do {
            pb[--i] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        ADD(":");
        ADD(pb + i);
    }
    ADD("\r\nUser-Agent: StarbiteOS/1 (sbhttp)\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
#undef ADD
    h->req[n] = 0;
    h->req_len = n;
    h->req_sent = 0;
}

/* ---- the response ------------------------------------------------------------------------------------------------- */

static void store(struct sbhttp *h, const uint8_t *d, uint32_t n)
{
    uint32_t k = h->body_cap - h->body_len;
    if (k > n)
        k = n;
    if (k && h->body) {
        mcpy(h->body + h->body_len, d, k);
        h->body_len += k;
    }
    h->body_total += n;
}

static int complete(const struct sbhttp *h)
{
    if (h->head_len != ~0u)
        return 0;
    if (h->no_body)
        return 1;
    if (h->chunked)
        return h->chunk_state == CS_DONE;
    if (h->content_length >= 0)
        return h->body_total >= (uint64_t)h->content_length;
    return 0;
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* body bytes: 0 more wanted, 1 complete, <0 an error */
static int body_feed(struct sbhttp *h, const uint8_t *d, uint32_t n)
{
    if (!h->chunked) {
        if (h->content_length >= 0) {
            uint64_t left = (uint64_t)h->content_length - h->body_total;
            if (n > left)
                n = (uint32_t)left;             /* anything after the body is ignored */
        }
        store(h, d, n);
        return complete(h);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        switch (h->chunk_state) {
        case CS_SIZE:
            if (hexv(c) >= 0) {
                if (h->chunk_left >> 56)
                    return SBHTTP_E_PROTOCOL;
                h->chunk_left = h->chunk_left << 4 | (uint64_t)hexv(c);
            } else if (c == ';' || c == ' ' || c == '\t') {
                h->chunk_state = CS_EXT;
            } else if (c == '\r') {
                h->chunk_state = CS_SIZE_LF;
            } else {
                return SBHTTP_E_PROTOCOL;
            }
            break;
        case CS_EXT:
            if (c == '\r')
                h->chunk_state = CS_SIZE_LF;
            break;
        case CS_SIZE_LF:
            if (c != '\n')
                return SBHTTP_E_PROTOCOL;
            h->chunk_state = h->chunk_left ? CS_DATA : CS_TRAILER;
            break;
        case CS_DATA: {
            uint64_t k = n - i;
            if (k > h->chunk_left)
                k = h->chunk_left;
            store(h, d + i, (uint32_t)k);
            h->chunk_left -= k;
            i += (uint32_t)k - 1;
            if (!h->chunk_left)
                h->chunk_state = CS_DATA_CR;
            break;
        }
        case CS_DATA_CR:
            if (c != '\r')
                return SBHTTP_E_PROTOCOL;
            h->chunk_state = CS_DATA_LF;
            break;
        case CS_DATA_LF:
            if (c != '\n')
                return SBHTTP_E_PROTOCOL;
            h->chunk_state = CS_SIZE;
            h->chunk_left = 0;
            break;
        case CS_TRAILER:                        /* trailer lines until an empty one */
            if (c == '\r')
                h->chunk_state = CS_TRAILER_LF;
            else
                h->chunk_left = 1;              /* (reused: this line is not empty) */
            break;
        case CS_TRAILER_LF:
            if (c != '\n')
                return SBHTTP_E_PROTOCOL;
            if (!h->chunk_left) {
                h->chunk_state = CS_DONE;
                return 1;
            }
            h->chunk_left = 0;
            h->chunk_state = CS_TRAILER;
            break;
        case CS_DONE:
            return 1;
        }
    }
    return complete(h);
}

/* the header block is in head[0, end): status, Content-Length, Transfer-Encoding, Location */
static int parse_head(struct sbhttp *h, uint32_t end)
{
    const char *s = (const char *)h->head;
    uint32_t i = 0, line_end;
    if (end < 12 || !ieq(s, 5, "HTTP/") || s[5] != '1' || s[6] != '.' || s[8] != ' ')
        return SBHTTP_E_PROTOCOL;
    if (s[9] < '1' || s[9] > '5' || s[10] < '0' || s[10] > '9' || s[11] < '0' || s[11] > '9')
        return SBHTTP_E_PROTOCOL;
    h->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    h->content_length = -1;
    h->chunked = 0;
    h->location[0] = 0;
    while (i < end && s[i] != '\n')
        i++;
    i++;
    while (i < end) {
        uint32_t name = i, colon = 0, v;
        while (i < end && s[i] != '\n') {
            if (!colon && s[i] == ':')
                colon = i;
            i++;
        }
        line_end = i;
        if (line_end > name && s[line_end - 1] == '\r')
            line_end--;
        i++;
        if (!colon || colon >= line_end)
            continue;
        v = colon + 1;
        while (v < line_end && (s[v] == ' ' || s[v] == '\t'))
            v++;
        while (line_end > v && (s[line_end - 1] == ' ' || s[line_end - 1] == '\t'))
            line_end--;
        if (ieq(s + name, colon - name, "content-length")) {
            int64_t cl = 0;
            if (v == line_end)
                return SBHTTP_E_PROTOCOL;
            for (uint32_t k = v; k < line_end; k++) {
                if (s[k] < '0' || s[k] > '9' || cl > (int64_t)1 << 50)
                    return SBHTTP_E_PROTOCOL;
                cl = cl * 10 + (s[k] - '0');
            }
            if (h->content_length >= 0 && h->content_length != cl)
                return SBHTTP_E_PROTOCOL;       /* two different lengths */
            h->content_length = cl;
        } else if (ieq(s + name, colon - name, "transfer-encoding")) {
            /* the last coding must be chunked (anything else: to the end of the connection) */
            uint32_t k = line_end;
            while (k > v && s[k - 1] != ',' && s[k - 1] != ' ')
                k--;
            if (ieq(s + k, line_end - k, "chunked"))
                h->chunked = 1;
        } else if (ieq(s + name, colon - name, "location")) {
            uint32_t k = line_end - v;
            if (k < sizeof(h->location)) {
                mcpy(h->location, s + v, k);
                h->location[k] = 0;
            }
        }
    }
    if (h->chunked)
        h->content_length = -1;                 /* RFC 9112 6.3: chunked wins */
    h->no_body = h->status / 100 == 1 || h->status == 204 || h->status == 304;
    h->eof_body = !h->no_body && !h->chunked && h->content_length < 0;
    h->chunk_state = CS_SIZE;
    h->chunk_left = 0;
    return 0;
}

static int redirect(struct sbhttp *h);

/* response bytes: 0 more wanted, 1 complete, 2 restarted (a redirect), <0 an error */
static int feed(struct sbhttp *h, const uint8_t *d, uint32_t n)
{
    if (!h->first_byte_ms && n)
        h->first_byte_ms = (uint32_t)(now(h) - h->t_start);
    while (h->head_len != ~0u && n) {
        uint32_t k = SBHTTP_HEAD_MAX - h->head_len, end = 0;
        if (k > n)
            k = n;
        if (!k)
            return SBHTTP_E_PROTOCOL;           /* headers larger than 8 KiB */
        mcpy(h->head + h->head_len, d, k);
        {
            uint32_t from = h->head_len > 3 ? h->head_len - 3 : 0;
            h->head_len += k;
            for (uint32_t i = from; i + 3 < h->head_len; i++)
                if (h->head[i] == '\r' && h->head[i + 1] == '\n' && h->head[i + 2] == '\r' && h->head[i + 3] == '\n') {
                    end = i + 4;
                    break;
                }
        }
        if (!end) {
            d += k, n -= k;
            continue;
        }
        {
            int r = parse_head(h, end);
            uint32_t extra = h->head_len - end;     /* body bytes that came with the head */
            const uint8_t *rest = d + k - extra;
            if (r)
                return r;
            {
                struct ub u = { .n = 0 };
                u_s(&u, "http: status ");
                u_d(&u, (uint64_t)h->status);
                if (h->chunked)
                    u_s(&u, ", chunked");
                else if (h->content_length >= 0)
                    u_s(&u, ", "), u_d(&u, (uint64_t)h->content_length), u_s(&u, " bytes");
                else if (h->eof_body)
                    u_s(&u, ", to the end of the connection");
                u_s(&u, ", first byte after ");
                u_d(&u, h->first_byte_ms);
                u_s(&u, " ms");
                say(h, &u);
            }
            if (h->status / 100 == 1) {         /* 100 Continue and friends: the real answer follows */
                h->head_len = 0;
                return feed(h, rest, extra + (n - k));
            }
            if ((h->status == 301 || h->status == 302 || h->status == 303 || h->status == 307 || h->status == 308) &&
                h->location[0])
                return redirect(h) ? SBHTTP_E_REDIRECTS : 2;
            h->head_len = ~0u;                  /* the head is done */
            if (h->no_body)
                return 1;
            n = extra + (n - k);                /* the body's first bytes: the piece's rest */
            d = rest;
            if (!n)
                return complete(h);
            break;
        }
    }
    if (!n)
        return complete(h);
    return body_feed(h, d, n);
}

/* ---- the connection ------------------------------------------------------------------------------------------------ */

static void finish(struct sbhttp *h, int r)
{
    struct ub u = { .n = 0 };
    if (h->conn >= 0) {
        h->net.close(h->net.ctx, h->conn);
        h->conn = -1;
    }
    h->state = S_DONE;
    h->result = r;
    h->total_ms = (uint32_t)(now(h) - h->t_start);
    if (r == SBHTTP_DONE) {
        u_s(&u, "http: done: status ");
        u_d(&u, (uint64_t)h->status);
        u_s(&u, ", ");
        u_d(&u, h->body_total);
        u_s(&u, " bytes in ");
        u_d(&u, h->total_ms);
        u_s(&u, " ms");
        if (h->tls)
            u_s(&u, h->close_notify || !h->eof_body ? "" : " (no close_notify)");
    } else {
        u_s(&u, "http: FAILED: ");
        u_s(&u, sbhttp_why(r));
        if (h->tls_err) {
            u_s(&u, " (BearSSL error ");
            u_d(&u, (uint64_t)h->tls_err);
            u_s(&u, ")");
        }
    }
    say(h, &u);
}

static void begin(struct sbhttp *h)
{
    h->status = 0;
    h->head_len = 0;
    h->content_length = -1;
    h->chunked = h->eof_body = h->no_body = 0;
    h->chunk_state = CS_SIZE;
    h->chunk_left = 0;
    h->body_total = 0;
    h->body_len = 0;
    h->close_notify = 0;
    h->tls_err = 0;
    h->first_byte_ms = 0;
    h->conn = -1;
    build_request(h);
    {
        struct ub u = { .n = 0 };
        u_s(&u, "http: GET ");
        u_s(&u, h->tls ? "https://" : "http://");
        u_s(&u, h->net.log_host ? h->host : "(host not logged)");
        u_s(&u, " (path not logged), port ");
        u_d(&u, h->port);
        say(h, &u);
    }
    if (ipv4_literal(h->host, h->ip)) {
        h->state = S_CONNECT;
    } else {
        h->state = S_RESOLVE;
    }
    h->t_phase = now(h);
}

/* a redirect to h->location: 0 restarted, -1 too many / not followable */
static int redirect(struct sbhttp *h)
{
    char nu[SBHTTP_URL_MAX];
    uint32_t n = 0;
    const char *l = h->location;
    if (++h->redirects > 3)
        return -1;
    if (ieq(l, 8, "https://") || ieq(l, 7, "http://")) {
        n = slen(l);
        if (n >= sizeof(nu))
            return -1;
        mcpy(nu, l, n + 1);
    } else if (l[0] == '/' && l[1] != '/') {
        /* the same scheme, host and port */
        const char *sch = h->tls ? "https://" : "http://";
        uint32_t a = slen(sch), b = slen(h->host), c = slen(l);
        char pb[8] = { 0 };
        uint32_t pl = 0;
        if ((h->tls && h->port != 443) || (!h->tls && h->port != 80)) {
            unsigned v = h->port;
            char t[6];
            int i = 0;
            do {
                t[i++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            pb[pl++] = ':';
            while (i--)
                pb[pl++] = t[i];
        }
        if (a + b + pl + c >= sizeof(nu))
            return -1;
        mcpy(nu, sch, a), mcpy(nu + a, h->host, b), mcpy(nu + a + b, pb, pl), mcpy(nu + a + b + pl, l, c + 1);
    } else {
        return -1;                              /* relative paths: not followed */
    }
    says(h, "redirected");
    if (h->conn >= 0) {
        h->net.close(h->net.ctx, h->conn);
        h->conn = -1;
    }
    mcpy(h->url, nu, slen(nu) + 1);
    if (parse_url(h))
        return -1;
    begin(h);
    return 0;
}

static int tls_setup(struct sbhttp *h)
{
    uint64_t t = h->net.unix_time ? h->net.unix_time(h->net.ctx) : 0;
    if (t < 1700000000ull)                      /* before 2023: not a real clock */
        return SBHTTP_E_CLOCK;
    if (h->net.tas)
        br_ssl_client_init_full(&h->cc, &h->xc, h->net.tas, h->net.tas_num);
    else
        br_ssl_client_init_full(&h->cc, &h->xc, sbtls_tas, sbtls_tas_num);
    br_ssl_engine_set_versions(&h->cc.eng, BR_TLS12, BR_TLS12);
    br_x509_minimal_set_time(&h->xc, (uint32_t)(t / 86400 + 719528), (uint32_t)(t % 86400));
    br_ssl_engine_set_buffer(&h->cc.eng, h->iobuf, sizeof(h->iobuf), 1);
    br_ssl_engine_inject_entropy(&h->cc.eng, h->seed, sizeof(h->seed));
    if (!br_ssl_client_reset(&h->cc, h->host, 0))
        return SBHTTP_E_TLS;
    return 0;
}

static int tls_fail(struct sbhttp *h)
{
    int err = br_ssl_engine_last_error(&h->cc.eng);
    h->tls_err = err;
    if (err >= BR_ERR_X509_OK + 1 && err < BR_ERR_X509_OK + 32)
        return SBHTTP_E_CERT;
    return SBHTTP_E_TLS;
}

/* the TLS engine against the connection: BUSY, DONE or an error */
static int tls_pump(struct sbhttp *h)
{
    br_ssl_engine_context *e = &h->cc.eng;
    for (int rounds = 0; rounds < 64; rounds++) {
        unsigned st = br_ssl_engine_current_state(e);
        int progress = 0;
        if (st & BR_SSL_CLOSED) {
            if (br_ssl_engine_last_error(e) != BR_ERR_OK)
                return tls_fail(h);
            h->close_notify = 1;
            if (complete(h) || (h->head_len == ~0u && h->eof_body))
                return SBHTTP_DONE;
            return SBHTTP_E_CLOSED;
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            uint8_t *b = br_ssl_engine_sendrec_buf(e, &len);
            int n = h->net.send(h->net.ctx, h->conn, b, (uint32_t)len);
            if (n < 0)
                return SBHTTP_E_NETWORK;
            if (n > 0) {
                br_ssl_engine_sendrec_ack(e, (size_t)n);
                progress = 1;
            }
        }
        if ((st & BR_SSL_SENDAPP) && !h->tls_ms) {
            struct ub u = { .n = 0 };
            h->tls_ms = (uint32_t)(now(h) - h->t_phase);
            h->tls_version = br_ssl_engine_get_version(e);
            h->tls_suite = e->session.cipher_suite;
            u_s(&u, "http: TLS ");
            u_s(&u, h->tls_version == BR_TLS12 ? "1.2" : "?");
            u_s(&u, " in ");
            u_d(&u, h->tls_ms);
            u_s(&u, " ms, cipher suite 0x");
            u_x(&u, h->tls_suite, 4);
            u_s(&u, ", the certificate accepted");
            say(h, &u);
        }
        if ((st & BR_SSL_SENDAPP) && h->req_sent < h->req_len) {
            size_t len;
            uint8_t *b = br_ssl_engine_sendapp_buf(e, &len);
            uint32_t k = h->req_len - h->req_sent;
            if (k > len)
                k = (uint32_t)len;
            mcpy(b, h->req + h->req_sent, k);
            br_ssl_engine_sendapp_ack(e, k);
            h->req_sent += k;
            if (h->req_sent == h->req_len)
                br_ssl_engine_flush(e, 0);
            progress = 1;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t len;
            uint8_t *b = br_ssl_engine_recvapp_buf(e, &len);
            int r = feed(h, b, (uint32_t)len);
            if (r == 2)
                return SBHTTP_BUSY;             /* redirected: a new connection (the engine is set up again) */
            br_ssl_engine_recvapp_ack(e, len);
            if (r < 0)
                return r;
            if (r == 1)
                return SBHTTP_DONE;
            progress = 1;
        }
        if (st & BR_SSL_RECVREC) {
            size_t len;
            uint8_t *b = br_ssl_engine_recvrec_buf(e, &len);
            int n = h->net.recv(h->net.ctx, h->conn, b, (uint32_t)len);
            if (n == -1) {
                /* the connection ended without close_notify */
                if (complete(h) || (h->head_len == ~0u && h->eof_body))
                    return SBHTTP_DONE;
                if (!h->tls_ms)
                    return tls_fail(h) == SBHTTP_E_CERT ? SBHTTP_E_CERT : SBHTTP_E_TLS;
                return SBHTTP_E_CLOSED;
            }
            if (n < -1)
                return SBHTTP_E_NETWORK;
            if (n > 0) {
                br_ssl_engine_recvrec_ack(e, (size_t)n);
                progress = 1;
            }
        }
        if (!progress)
            break;
    }
    return SBHTTP_BUSY;
}

static int plain_pump(struct sbhttp *h)
{
    uint8_t b[1460];
    while (h->req_sent < h->req_len) {
        int n = h->net.send(h->net.ctx, h->conn, (const uint8_t *)h->req + h->req_sent, h->req_len - h->req_sent);
        if (n < 0)
            return SBHTTP_E_NETWORK;
        if (!n)
            break;
        h->req_sent += (uint32_t)n;
    }
    for (int rounds = 0; rounds < 64; rounds++) {
        int n = h->net.recv(h->net.ctx, h->conn, b, sizeof(b));
        if (n == -1)
            return complete(h) || (h->head_len == ~0u && h->eof_body) ? SBHTTP_DONE : SBHTTP_E_CLOSED;
        if (n < -1)
            return SBHTTP_E_NETWORK;
        if (!n)
            break;
        {
            int r = feed(h, b, (uint32_t)n);
            if (r == 2)
                return SBHTTP_BUSY;
            if (r)
                return r < 0 ? r : SBHTTP_DONE;
        }
    }
    return SBHTTP_BUSY;
}

/* ---- the calls ------------------------------------------------------------------------------------------------------ */

int sbhttp_start(struct sbhttp *h, const struct sbhttp_net *net, const char *url, uint8_t *body, uint32_t body_cap,
                 const uint8_t seed[32], uint32_t timeout_ms)
{
    uint32_t n = slen(url);
    h->net = *net;
    h->conn = -1;
    h->state = S_IDLE;
    h->result = SBHTTP_BUSY;
    h->redirects = 0;
    h->body = body;
    h->body_cap = body ? body_cap : 0;
    h->dns_ms = h->connect_ms = h->tls_ms = h->first_byte_ms = h->total_ms = 0;
    h->tls_version = h->tls_suite = 0;
    mcpy(h->seed, seed, 32);
    h->t_start = now(h);
    h->timeout_ms = timeout_ms ? timeout_ms : 30000;
    h->deadline = h->t_start + h->timeout_ms;
    if (n >= sizeof(h->url)) {
        finish(h, SBHTTP_E_URL);
        return h->result;
    }
    mcpy(h->url, url, n + 1);
    if (parse_url(h)) {
        finish(h, SBHTTP_E_URL);
        return h->result;
    }
    begin(h);
    return SBHTTP_BUSY;
}

int sbhttp_poll(struct sbhttp *h)
{
    int r;
    if (h->state == S_DONE || h->state == S_IDLE)
        return h->result;
    if (now(h) >= h->deadline) {
        finish(h, SBHTTP_E_TIMEOUT);
        return h->result;
    }
    if (h->state == S_RESOLVE) {
        r = h->net.resolve(h->net.ctx, h->host, h->ip);
        if (r < 0) {
            finish(h, SBHTTP_E_DNS);
            return h->result;
        }
        if (!r)
            return SBHTTP_BUSY;
        h->dns_ms = (uint32_t)(now(h) - h->t_phase);
        {
            struct ub u = { .n = 0 };
            u_s(&u, "http: the name resolved in ");
            u_d(&u, h->dns_ms);
            u_s(&u, " ms");
            say(h, &u);
        }
        h->state = S_CONNECT;
        h->t_phase = now(h);
    }
    if (h->state == S_CONNECT) {
        if (h->conn < 0) {
            h->conn = h->net.connect(h->net.ctx, h->ip, h->port);
            if (h->conn < 0) {
                finish(h, SBHTTP_E_CONNECT);
                return h->result;
            }
        }
        r = h->net.connected(h->net.ctx, h->conn);
        if (r < 0) {
            finish(h, SBHTTP_E_CONNECT);
            return h->result;
        }
        if (!r)
            return SBHTTP_BUSY;
        h->connect_ms = (uint32_t)(now(h) - h->t_phase);
        {
            struct ub u = { .n = 0 };
            u_s(&u, "http: connected in ");
            u_d(&u, h->connect_ms);
            u_s(&u, " ms");
            say(h, &u);
        }
        h->t_phase = now(h);
        h->tls_ms = 0;
        if (h->tls) {
            r = tls_setup(h);
            if (r) {
                finish(h, r);
                return h->result;
            }
        }
        h->state = S_TALK;
    }
    if (h->state == S_TALK) {
        int conn = h->conn;
        r = h->tls ? tls_pump(h) : plain_pump(h);
        if (r == SBHTTP_BUSY && h->conn != conn)
            return SBHTTP_BUSY;                 /* a redirect started over */
        if (r != SBHTTP_BUSY)
            finish(h, r);
    }
    return h->state == S_DONE ? h->result : SBHTTP_BUSY;
}

void sbhttp_abort(struct sbhttp *h)
{
    if (h->state != S_DONE && h->state != S_IDLE)
        finish(h, SBHTTP_E_ABORTED);
}
