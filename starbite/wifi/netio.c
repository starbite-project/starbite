/* SPDX-License-Identifier: MIT
 * netio (netio.h): sbhttp over sbnet + sbtcp. */
#include "netio.h"

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int io_resolve(void *ctx, const char *host, uint8_t ip[4])
{
    struct sbnetio *io = ctx;
    struct sbnet *n = io->net;
    if (n->state < SBNET_BOUND)
        return -1;
    if (same(io->dns_name, host) && io->dns_tries) {
        if (n->dns_state == SBNET_DNS_DONE) {
            for (int i = 0; i < 4; i++)
                ip[i] = n->dns_result[i];
            io->dns_name[0] = 0, io->dns_tries = 0;
            return 1;
        }
        if (n->dns_state == SBNET_DNS_WAITING)
            return 0;
        if (io->dns_tries >= 3) {               /* 3 x 5 s without an answer, or "no such name" */
            io->dns_name[0] = 0, io->dns_tries = 0;
            return -1;
        }
    } else {
        unsigned k = 0;
        while (host[k] && k < sizeof(io->dns_name) - 1) {
            io->dns_name[k] = host[k];
            k++;
        }
        io->dns_name[k] = 0;
        io->dns_tries = 0;
    }
    io->dns_tries++;
    if (sbnet_dns_query(n, host) < 0) {
        io->dns_name[0] = 0, io->dns_tries = 0;
        return -1;
    }
    return 0;
}

static uint64_t io_now(void *ctx)
{
    struct sbnetio *io = ctx;
    return io->net->os.now_ms(io->net->os.ctx);
}

static int io_connect(void *ctx, const uint8_t ip[4], uint16_t port)
{
    struct sbnetio *io = ctx;
    int h = sbtcp_connect(io->tcp, ip, port);
    if (h >= 0)
        io->conn_t0[h] = io_now(ctx);
    return h;
}

static int io_connected(void *ctx, int h)
{
    struct sbnetio *io = ctx;
    int st = sbtcp_state(io->tcp, h);
    if (st == SBTCP_ESTABLISHED || st == SBTCP_CLOSE_WAIT)
        return 1;
    if (st == SBTCP_SYN_SENT) {
        if (io_now(ctx) - io->conn_t0[h] > (io->connect_ms ? io->connect_ms : 15000u)) {
            sbtcp_abort(io->tcp, h);
            return -1;
        }
        return 0;
    }
    return -1;
}

static int io_send(void *ctx, int h, const uint8_t *d, uint32_t n)
{
    struct sbnetio *io = ctx;
    return sbtcp_send(io->tcp, h, d, n);
}

static int io_recv(void *ctx, int h, uint8_t *d, uint32_t cap)
{
    struct sbnetio *io = ctx;
    return sbtcp_recv(io->tcp, h, d, cap);
}

static void io_close(void *ctx, int h)
{
    struct sbnetio *io = ctx;
    sbtcp_close(io->tcp, h);
}

static uint64_t io_unix(void *ctx)
{
    struct sbnetio *io = ctx;
    return io->unix_time ? io->unix_time(io->ctx) : 0;
}

static void io_log(void *ctx, const char *line)
{
    struct sbnetio *io = ctx;
    if (io->log)
        io->log(io->ctx, line);
}

void sbnetio_init(struct sbnetio *io, struct sbhttp_net *out)
{
    io->dns_name[0] = 0;
    io->dns_tries = 0;
    out->ctx = io;
    out->resolve = io_resolve;
    out->connect = io_connect;
    out->connected = io_connected;
    out->send = io_send;
    out->recv = io_recv;
    out->close = io_close;
    out->now_ms = io_now;
    out->unix_time = io_unix;
    out->log = io_log;
    out->log_host = 0;
    out->tas = 0;
    out->tas_num = 0;
}
