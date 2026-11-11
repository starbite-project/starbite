/* SPDX-License-Identifier: MIT
 * netio: sbhttp's network (struct sbhttp_net) on sbnet (DNS) and sbtcp (connections), for Starbite OS and the
 * test images. The caller keeps sbnet and sbtcp ticking; netio only adapts calls. */
#ifndef SBNETIO_H
#define SBNETIO_H
#include "net.h"
#include "tcp.h"
#include "http.h"

struct sbnetio {
    struct sbnet *net;
    struct sbtcp *tcp;
    void *ctx;                              /* for the time and log callbacks */
    uint64_t (*unix_time)(void *ctx);
    void (*log)(void *ctx, const char *line);
    /* the DNS lookup in flight (sbnet asks one name at a time) */
    char dns_name[256];
    int dns_tries;
    uint32_t connect_ms;                    /* a connection that is not up in this time fails (default 15 s) */
    uint64_t conn_t0[SBTCP_SOCKS];
};

/* fills `out` (its ctx is io) */
void sbnetio_init(struct sbnetio *io, struct sbhttp_net *out);

#endif
