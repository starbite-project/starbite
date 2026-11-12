/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 * sbnet (contract v3, sbshm.h SBSHM_NET_OFF, host_services & SBSHM_SVC_NET): the Linux guest's IP link to Starbite OS.
 *
 * A point-to-point L3 link: raw IP packets (no Ethernet header), IPv4 first (IPv6 packets may be sent by the guest;
 * a host without IPv6 drops them and counts them in rx_drop). The guest is sbnet0 with a static address from this
 * struct (default 10.77.0.2/30, gateway and DNS 10.77.0.1); Starbite OS (Track G's NAT) rewrites the guest's TCP /
 * UDP / ICMP onto whichever uplink is active (Wi-Fi when joined, else mobile data) and relays DNS at the gateway.
 *
 * Two single-producer single-consumer rings of fixed slots, in the same style as sbblk:
 *   tx: guest -> host (the guest's kernel produces: tx_head; the host consumes: tx_tail)
 *   rx: host -> guest (the host produces: rx_head; the guest consumes: rx_tail)
 * Producer: write slot[head % SBSHM_NET_SLOTS] (len, then data), dmb, head + 1, then pend the other side's doorbell
 * (guest: hdr.db_to_host; host: hdr.db_to_guest). A full ring drops the packet (TCP retransmits) and counts it.
 * Consumer: when tail != head: dmb, copy slot[tail % N], dmb, tail + 1. Each index has its own 64-byte line.
 * The region is non-cacheable on both sides: copy with plain loads / stores (no exclusives, no DC ops needed).
 *
 * Config: the host writes it under cfg_seq (seqlock: odd while writing) before CPU_ON and whenever the uplink changes;
 * then it pends the guest's doorbell. link = 0 means no uplink: the guest keeps its address (sockets stay open) but
 * the host drops what it sends (and answers nothing); link = 1 again resumes.
 */
#ifndef SBSHM_NET_H
#define SBSHM_NET_H
#include <stdint.h>

#define SBSHM_NET_SLOTS     256U                /* per ring, power of two */
#define SBSHM_NET_SLOT      2048U               /* bytes per slot: 8-byte head + up to 2040 bytes of packet */
#define SBSHM_NET_MTU       1400U               /* the guest's MTU (fits LTE's and Wi-Fi's after the host's headers) */
#define SBSHM_NET_TX_OFF    0x1000U             /* from SBSHM_NET_OFF: tx slots (512 KiB) */
#define SBSHM_NET_RX_OFF    (SBSHM_NET_TX_OFF + SBSHM_NET_SLOTS * SBSHM_NET_SLOT)   /* rx slots (512 KiB) */

enum { SBSHM_NET_UPLINK_NONE = 0, SBSHM_NET_UPLINK_WIFI = 1, SBSHM_NET_UPLINK_CELL = 2 };

struct sbshm_net_slot {
    uint16_t len;                   /* packet bytes in data (1..SBSHM_NET_MTU) */
    uint16_t flags;                 /* 0 */
    uint32_t pad;
    uint8_t data[SBSHM_NET_SLOT - 8];
};

struct sbshm_net {
    /* ---- host writes (seqlocked by cfg_seq) ---- */
    volatile uint32_t cfg_seq;
    uint32_t link;                  /* 1: an uplink is up, packets flow; 0: none */
    uint32_t uplink;                /* SBSHM_NET_UPLINK_* (for the guest's status only) */
    uint32_t mtu;                   /* SBSHM_NET_MTU */
    uint8_t guest_ip[4];            /* network order: 10.77.0.2 */
    uint8_t gw_ip[4];               /* 10.77.0.1: the host's end, the default route */
    uint8_t dns_ip[4];              /* 10.77.0.1: the host relays DNS */
    uint32_t prefix;                /* 30 */
    uint32_t pad0[8];               /* to 64 bytes */
    /* ---- ring indices, one cache line each ---- */
    volatile uint32_t tx_head;      /* guest */
    uint32_t tx_drop;               /* guest: packets dropped on a full tx ring */
    uint32_t pad1[14];
    volatile uint32_t tx_tail;      /* host */
    uint32_t pad2[15];
    volatile uint32_t rx_head;      /* host */
    uint32_t rx_drop;               /* host: packets dropped (full rx ring, no uplink, unsupported protocol) */
    uint32_t pad3[14];
    volatile uint32_t rx_tail;      /* guest */
    uint32_t pad4[15];
};                                  /* 320 bytes, then the slots at SBSHM_NET_TX_OFF / SBSHM_NET_RX_OFF */

#endif
