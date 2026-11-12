/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbnat: a small IPv4 NAT (masquerade) for Starbite OS's Linux guest. Freestanding C99, no libc, no allocation.
 *
 * The guest sits on a point-to-point packet ring carrying raw IPv4 packets (no Ethernet header):
 *   guest 10.77.0.2/30, host side 10.77.0.1 (the guest's gateway and DNS server).
 * The host has one uplink at a time (Wi-Fi or LTE) with a single IPv4 address, shared by the host's own stack
 * and the guest. Guest traffic is rewritten onto that address with external ports (and ICMP echo ids) from
 * a reserved range (default 40000..47999) that the host's own sockets must never use.
 *
 *   sbnat_from_guest   SBNAT_SEND      rewritten for the uplink; send it there
 *                      SBNAT_LOCAL     for 10.77.0.1 itself (not DNS); untouched. ICMP echo: sbnat_echo_reply
 *                      SBNAT_DROP      bad / unsupported / no mapping possible (see the stats)
 *   sbnat_from_uplink  SBNAT_TO_GUEST  rewritten for the guest; put it on the ring
 *                      SBNAT_NOT_OURS  untouched; the host's own stack takes it
 *                      SBNAT_DROP      a bad IPv4 header (checksum, lengths), a bad ICMP error checksum on a
 *                                      flow of ours, or TTL <= 1 on a flow of ours
 *
 * Packets to 10.77.0.1:53 (UDP and TCP) go to the uplink's DNS server; the replies come back from 10.77.0.1:53.
 * Packets are rewritten in place; their length never changes. Checksums are updated incrementally
 * (RFC 1624); a UDP checksum of 0 stays 0. Forwarding decrements the TTL in both directions.
 *
 * The guest's link driver must hand over complete checksums (no partial / offloaded checksums).
 * Nothing here logs; there are counters only (struct sbnat_stats). Not thread-safe: one caller at a time.
 */
#ifndef SBNAT_H
#define SBNAT_H

#include <stdint.h>

#define SBNAT_DROP       0
#define SBNAT_SEND       1
#define SBNAT_LOCAL      2
#define SBNAT_TO_GUEST   3
#define SBNAT_NOT_OURS   4

#define SBNAT_ENTRIES    512
#define SBNAT_PORT_LO    40000   /* default external port / ICMP id range, inclusive */
#define SBNAT_PORT_HI    47999

/* the guest link, as bytes */
#define SBNAT_GUEST_ADDR { 10, 77, 0, 2 }
#define SBNAT_GW_ADDR    { 10, 77, 0, 1 }

/* idle timeouts, ms */
#define SBNAT_TCP_EST_MS     7440000u  /* established */
#define SBNAT_TCP_CLOSE_MS     60000u  /* FIN seen both ways, or any RST */
#define SBNAT_TCP_SYN_MS      120000u  /* not established yet */
#define SBNAT_UDP_MS           60000u  /* guest -> remote only */
#define SBNAT_UDP_BOTH_MS     180000u  /* traffic seen both ways */
#define SBNAT_ICMP_MS          30000u

struct sbnat_cfg {
    uint8_t uplink[4];      /* uplink address; 0.0.0.0 = no uplink (guest traffic is dropped) */
    uint8_t dns[4];         /* uplink DNS server; 0.0.0.0 = none (guest DNS is dropped) */
    uint16_t port_lo;       /* external range; 0 / invalid = SBNAT_PORT_LO..SBNAT_PORT_HI */
    uint16_t port_hi;
    uint64_t now_ms;        /* seeds the port cursor */
};

struct sbnat_stats {
    uint64_t sent;          /* SBNAT_SEND */
    uint64_t to_guest;      /* SBNAT_TO_GUEST (all, ICMP errors included) */
    uint64_t icmp_errors;   /* ICMP errors translated to the guest */
    uint64_t local;         /* SBNAT_LOCAL */
    uint64_t not_ours;      /* SBNAT_NOT_OURS */
    uint32_t mapped;        /* mappings created */
    uint32_t expired;       /* mappings timed out */
    uint32_t evicted;       /* live UDP / ICMP mappings evicted for a new one */
    uint32_t table_full;    /* new mapping refused (table full of live TCP) */
    uint32_t flushed;       /* mappings dropped by sbnat_uplink_changed */
    uint32_t uplink_changes;
    uint32_t in_frags;      /* non-first fragments from the uplink, passed to the host (NOT_OURS) */
    /* drops */
    uint32_t drop_hdr;      /* not IPv4, bad header / total length */
    uint32_t drop_csum;     /* bad IP header checksum; bad ICMP checksum on an ICMP error of ours */
    uint32_t drop_ttl;      /* TTL <= 1 when forwarding */
    uint32_t drop_frag;     /* non-first fragment from the guest */
    uint32_t drop_src;      /* guest source address is not 10.77.0.2 */
    uint32_t drop_dst;      /* guest destination not routable (0/8, 127/8, 224/3, the link /30, the uplink) */
    uint32_t drop_proto;    /* guest protocol other than TCP / UDP / ICMP echo request */
    uint32_t drop_icmp;     /* guest ICMP other than an echo request */
    uint32_t drop_short;    /* guest TCP / UDP / ICMP header truncated */
    uint32_t drop_nomap;    /* guest TCP RST without a mapping */
    uint32_t drop_no_uplink;
    uint32_t drop_no_dns;
};

struct sbnat_map {          /* internal */
    uint64_t last_ms;
    uint8_t raddr[4];       /* remote address (for DNS: the uplink DNS server) */
    uint16_t gport;         /* guest port / ICMP id */
    uint16_t eport;         /* external port / ICMP id */
    uint16_t rport;         /* remote port; 0 for ICMP */
    uint8_t proto;          /* 0 = free, 1, 6, 17 */
    uint8_t flags;
    uint8_t tcp;
};

struct sbnat {
    uint8_t uplink[4];
    uint8_t dns[4];
    uint16_t port_lo, port_hi;
    uint32_t cursor;
    uint32_t used;
    struct sbnat_stats st;
    struct sbnat_map map[SBNAT_ENTRIES];
};

void sbnat_init(struct sbnat *n, const struct sbnat_cfg *cfg);
int sbnat_from_guest(struct sbnat *n, uint8_t *pkt, uint32_t len, uint64_t now_ms);
int sbnat_from_uplink(struct sbnat *n, uint8_t *pkt, uint32_t len, uint64_t now_ms);
/* new uplink address / DNS (dns may be 0): every mapping is dropped; old connections die, as on a phone */
void sbnat_uplink_changed(struct sbnat *n, const uint8_t addr[4], const uint8_t dns[4]);
void sbnat_tick(struct sbnat *n, uint64_t now_ms);
/* number of live mappings at now_ms (expires stale ones) */
uint32_t sbnat_count(struct sbnat *n, uint64_t now_ms);
/* turns a valid ICMP echo request 10.77.0.2 -> 10.77.0.1 (a SBNAT_LOCAL packet) into the reply, in place.
 * Returns the reply's length (send that many bytes back to the guest), or 0 if pkt is not such a request. */
uint32_t sbnat_echo_reply(uint8_t *pkt, uint32_t len);

#endif
