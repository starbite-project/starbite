/* SPDX-License-Identifier: MIT
 * sbnet: a small IPv4 layer on Ethernet frames for the bare-metal Wi-Fi core (Starbite OS, the libsb test image):
 * DHCP (discover, request, renewal at half the lease, rebinding, a NAK restarts), ARP (answers, a small cache,
 * resolution with one frame held per destination), ICMP echo (answers and pings), UDP (send, receive through a
 * callback) and DNS (one A-record query at a time). No TCP yet.
 *
 * Freestanding C: no C library, no allocation, no globals; the caller owns `struct sbnet` and calls
 *   sbnet_rx()   for every received Ethernet frame (from the core's os->rx),
 *   sbnet_tick() every few hundred ms or more often (timers: DHCP, ARP retries, DNS timeouts),
 *   sbnet_start() once the link is up (bcmw_join returned BCMW_OK), sbnet_stop() when it goes down.
 * Nothing here logs addresses or names; the log callback gets fixed texts and numbers only. */
#ifndef SBNET_H
#define SBNET_H
#include <stdint.h>

struct sbnet_os {
    void *ctx;
    int (*tx)(void *ctx, const uint8_t *frame, uint32_t len);   /* an Ethernet frame out (bcmw_tx); 0 = sent */
    uint64_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);
    void (*log)(void *ctx, const char *line);                     /* optional */
    /* optional: a UDP datagram to one of our ports (not DHCP/DNS, which sbnet keeps) */
    void (*udp)(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *data, uint32_t len);
};

enum { SBNET_DOWN, SBNET_DISCOVERING, SBNET_REQUESTING, SBNET_BOUND, SBNET_RENEWING, SBNET_REBINDING };
enum { SBNET_DNS_IDLE, SBNET_DNS_WAITING, SBNET_DNS_DONE, SBNET_DNS_FAILED };

#define SBNET_ARP_ENTRIES 8
#define SBNET_HELD_MAX 1600

struct sbnet {
    struct sbnet_os os;
    uint8_t mac[6];
    int state;
    /* the lease (all zero until bound) */
    uint8_t ip[4], mask[4], gateway[4], dns[4], server[4];
    uint32_t lease_s;
    uint64_t bound_at, t1_at, t2_at, end_at;
    /* DHCP */
    uint32_t xid;
    uint8_t offered[4];
    uint64_t next_send;
    unsigned tries;
    /* ARP */
    struct { uint8_t ip[4], mac[6]; uint64_t at; int used; } arp[SBNET_ARP_ENTRIES];
    uint8_t held[SBNET_HELD_MAX];       /* one IPv4 packet waiting for its next hop's address */
    uint32_t held_len;
    uint8_t held_hop[4];
    uint64_t held_since, arp_next;
    unsigned arp_tries;
    /* ICMP */
    uint16_t ping_id;
    uint32_t ping_sent, ping_replies;
    uint64_t ping_at, ping_rtt_ms;
    /* DNS */
    int dns_state;
    uint16_t dns_id, dns_port;
    uint8_t dns_result[4];
    uint64_t dns_deadline;
    /* counters */
    uint32_t rx_frames, rx_ignored, tx_frames, tx_errors, ip_id;
};

void sbnet_init(struct sbnet *n, const struct sbnet_os *os, const uint8_t mac[6]);
void sbnet_start(struct sbnet *n);
void sbnet_stop(struct sbnet *n);
void sbnet_rx(struct sbnet *n, const uint8_t *frame, uint32_t len);
void sbnet_tick(struct sbnet *n);
/* a UDP datagram to ip:port (from our port sport); 0 sent or held for ARP, -1 not bound or too large */
int sbnet_udp_send(struct sbnet *n, const uint8_t ip[4], uint16_t port, uint16_t sport, const uint8_t *data, uint32_t len);
/* an ICMP echo request to ip; replies count in ping_replies (the last round trip in ping_rtt_ms) */
int sbnet_ping(struct sbnet *n, const uint8_t ip[4]);
/* an A-record query for name (dotted, at most 253 characters) to the lease's DNS server; then dns_state */
int sbnet_dns_query(struct sbnet *n, const char *name);

#endif
