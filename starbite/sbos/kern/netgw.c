/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Linux guest's network (Track H). The guest's virtual NIC (Track E, starbite/guest) hands its
 * Ethernet frames to kgw_guest_rx and takes ours through the to_guest callback it gave kgw_start; between them sits
 * sbgw (starbite/wifi/gw.c: DHCP 10.0.2.15, the router 10.0.2.2, DNS 10.0.2.3, NAT), whose uplink is the phone's
 * current one (wifi_uplink: Wi-Fi when joined, else mobile data). A change of uplink flushes the NAT table (the
 * guest's connections are reset at once and it reconnects over the new one).
 *
 * kgw_tick: from Wi-Fi's tick and from the guest's own poll (Track E), at most every 100 ms; it follows the uplink.
 * Logged: starts, stops, uplink changes (which kind) and counts. Never an address, a port or a name. */
#include "sb.h"
#include "kern.h"
#include "net.h"
#include "tcp.h"
#include "gw.h"

static struct sbgw *gw;
static int on, up_kind;
static struct sbnet *up_net;
static uint64_t last_tick;
static int (*guest_tx)(void *ctx, const uint8_t *frame, uint32_t len);
static void *guest_ctx;

static int gw_to_guest(void *ctx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    return guest_tx ? guest_tx(guest_ctx, frame, len) : -1;
}

static uint64_t gw_now(void *ctx)
{
    (void)ctx;
    return sb_ms();
}

static uint32_t gw_random(void *ctx)
{
    (void)ctx;
    static uint64_t x = 0x2545f4914f6cdd1dULL;
    x ^= sb_ticks() + (x << 13);
    x ^= x >> 7;
    x ^= x << 17;
    return (uint32_t)x;
}

static void gw_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("guestnet: ", line);
}

static void follow_uplink(void)
{
    struct sbnet *n = 0;
    int k = wifi_uplink(&n, 0);
    if (!k)
        n = 0;
    if (n == up_net && k == up_kind)
        return;
    up_net = n;
    up_kind = k;
    sbgw_set_uplink(gw, n);
    sb_log(k == 1 ? "guestnet: the uplink is Wi-Fi" : k == 2 ? "guestnet: the uplink is mobile data" :
                    "guestnet: no uplink (Wi-Fi not joined, mobile data off)");
}

int kgw_start(int (*to_guest)(void *ctx, const uint8_t *frame, uint32_t len), void *ctx)
{
    if (!to_guest)
        return SBOS_EINVAL;
    if (!gw) {
        gw = sb_alloc(sizeof(*gw));
        if (!gw)
            return SBOS_EAGAIN;
    }
    struct sbgw_os o;
    memset(&o, 0, sizeof(o));
    o.to_guest = gw_to_guest, o.now_ms = gw_now, o.random = gw_random, o.log = gw_log;
    guest_tx = to_guest;
    guest_ctx = ctx;
    sbgw_init(gw, &o);
    up_net = 0;
    up_kind = -1;
    on = 1;
    sb_log("guestnet: the guest's network is up (10.0.2.0/24 behind the phone's uplink)");
    follow_uplink();
    return 0;
}

void kgw_stop(void)
{
    if (!on)
        return;
    sbgw_set_uplink(gw, 0);                 /* unhooks the uplink */
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guestnet: stopped; packets out ");
    ub_dec(&u, gw->out);
    ub_s(&u, ", in ");
    ub_dec(&u, gw->in);
    ub_s(&u, ", dropped ");
    ub_dec(&u, gw->dropped);
    sb_log(u.b);
    on = 0;
    guest_tx = 0;
    guest_ctx = 0;
    up_net = 0;
}

int kgw_on(void)
{
    return on;
}

void kgw_guest_rx(const uint8_t *frame, uint32_t len)
{
    if (!on)
        return;
    follow_uplink();
    sbgw_guest_rx(gw, frame, len);
}

void kgw_tick(void)
{
    if (!on)
        return;
    uint64_t now = sb_ms();
    if (now - last_tick < 100)
        return;
    last_tick = now;
    follow_uplink();
    sbgw_tick(gw);
}

/* before an uplink's sbnet goes away (Wi-Fi off, mobile data off): no packets into it from here on */
void kgw_uplink_gone(void)
{
    if (on && up_net) {
        up_net = 0;
        up_kind = -1;
        sbgw_set_uplink(gw, 0);
    }
}

unsigned kgw_conns(void)
{
    return on ? sbgw_conns(gw) : 0;
}
