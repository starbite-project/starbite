/* SPDX-License-Identifier: MIT
 * sbgw: the Linux guest's gateway in Starbite OS (like QEMU's slirp, but NAT on our own stack, sbnet). The guest
 * sits on a virtual Ethernet link with the gateway; everything it sends beyond that link is translated (NAPT) onto
 * the phone's current uplink, an sbnet (Wi-Fi: Ethernet; mobile data: raw IP), with the uplink's own address.
 *
 * The guest's link (fixed, as slirp's):  10.0.2.0/24, the gateway 10.0.2.2 (ARP, ICMP echo, DHCP server), the DNS
 * server 10.0.2.3 (relayed through the NAT to the uplink's DNS server), the guest 10.0.2.15 (lease 1 day).
 * The guest's MTU is 1500 (no DHCP option 26: the uplink can change under a lease); TCP SYNs both ways have their
 * MSS clamped to the uplink's mtu - 40, larger DF packets get ICMP "fragmentation needed" with the uplink's MTU,
 * larger non-DF packets are fragmented on the way out. Fragments the guest sends are dropped (counted in frags);
 * fragments from the uplink are dropped by sbnet already.
 *
 * Freestanding C: no C library, no allocation, no globals; the caller owns `struct sbgw` (about 12 KiB) and
 *   sbgw_guest_rx() each Ethernet frame the guest sent (from the virtual NIC),
 *   sbgw_tick()     every few hundred ms (timeouts, the uplink's state: a lost or changed address flushes),
 *   sbgw_set_uplink() when the uplink changes (NULL: none); it hooks the uplink's sbnet (sbnet.ip_hook).
 * Frames to the guest go out through os.to_guest. Nothing logged names an address, a port or a name. */
#ifndef SBGW_H
#define SBGW_H
#include <stdint.h>
#include "net.h"

struct sbgw_os {
    void *ctx;
    int (*to_guest)(void *ctx, const uint8_t *frame, uint32_t len);    /* an Ethernet frame to the guest; 0 = sent */
    uint64_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);
    void (*log)(void *ctx, const char *line);                           /* optional */
};

#define SBGW_CONNS 256                      /* the NAT table: when full the connection idle longest goes */
#define SBGW_PORT_LO 40000u                 /* the uplink's ports (and ICMP echo ids) the NAT maps to: below sbtcp's */
#define SBGW_PORT_HI 48999u                 /* and sbnet's DNS (49152-65535) and NTP (50123) */
#define SBGW_MTU 1500u                      /* the guest's link */

/* timeouts (ms of silence both ways) */
#define SBGW_T_TCP_SYN 30000u               /* a SYN without the far end's SYN-ACK yet */
#define SBGW_T_TCP_EST 7200000u             /* established: 2 h */
#define SBGW_T_TCP_FIN 240000u              /* one side finished */
#define SBGW_T_TCP_CLOSED 10000u            /* both finished */
#define SBGW_T_UDP 60000u                   /* nothing back yet */
#define SBGW_T_UDP_REPLIED 180000u          /* an answer came back */
#define SBGW_T_UDP_DNS 15000u               /* to port 53 */
#define SBGW_T_ICMP 30000u

enum { SBGW_TCP_SYN = 1, SBGW_TCP_EST };
#define SBGW_F_REPLIED 1u                   /* something came back */
#define SBGW_F_FIN_G 2u                     /* the guest's FIN went out */
#define SBGW_F_FIN_R 4u                     /* the far end's FIN came in */
#define SBGW_F_HAVE_R 8u                    /* rnxt is known */

struct sbgw_conn {
    uint64_t last;                          /* the last packet either way */
    uint8_t proto;                          /* 0 free; 1 ICMP echo, 6 TCP, 17 UDP */
    uint8_t tcp, flags;
    uint8_t gsrc[4], gdst[4], rdst[4];      /* the guest, what it addressed, the far end (rdst != gdst: DNS relay) */
    uint16_t gport, gdport, rdport, mport;  /* ICMP: gport the guest's echo id, mport ours; no others */
    uint32_t gnxt, rnxt;                    /* TCP: the guest's next sequence number and its peer's (for RSTs) */
};

struct sbgw {
    struct sbgw_os os;
    struct sbnet *up;                       /* the uplink (0 none) */
    uint8_t up_ip[4];                       /* its address when the table was filled: another one flushes it */
    uint8_t mac[6];                         /* the gateway's on the guest's link: 52:55:0a:00:02:02 */
    uint8_t guest_mac[6];
    int guest_known;
    uint64_t lease_at;                      /* the guest's lease, given at (0: none) */
    uint16_t ip_id;
    struct sbgw_conn c[SBGW_CONNS];
    /* counters */
    uint32_t guest_rx, guest_tx, out, in, dropped, frags, too_big, fragmented, evicted, flushed, no_uplink,
        dhcp_acks, arp_replies, echo_replies, rst_sent, icmp_in;
};

void sbgw_init(struct sbgw *gw, const struct sbgw_os *os);
/* an Ethernet frame the guest sent */
void sbgw_guest_rx(struct sbgw *gw, const uint8_t *frame, uint32_t len);
void sbgw_tick(struct sbgw *gw);
/* the uplink (an sbnet that is or will be bound; 0 none): the table is flushed, the guest's TCP connections get a
 * RST at once; it may be called again with the same uplink (nothing changes then) */
void sbgw_set_uplink(struct sbgw *gw, struct sbnet *up);
/* connections in the table */
unsigned sbgw_conns(const struct sbgw *gw);

#endif
