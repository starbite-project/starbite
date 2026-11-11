/* SPDX-License-Identifier: MIT
 * sbtcp: TCP for sbnet (net.h), written for Starbite (RFC 9293). A few connections, active open (connect) and
 * passive (listen, for the tests and local services), segments after a gap kept in the receive buffer (up to
 * SBTCP_OOO ranges) while a duplicate ACK asks for the gap, retransmission with Karn's rule and RFC 6298's timer,
 * slow start and congestion avoidance with fast retransmit and NewReno's recovery (RFC 6582), zero-window probes,
 * MSS option only (no window scaling, timestamps or SACK: the window is at most 64 KiB).
 *
 * Freestanding C: no C library, no allocation; the caller owns `struct sbtcp` (about SBTCP_SOCKS x 40 KiB) and
 *   sets sbnet_os.tcp to a function that calls sbtcp_input(),
 *   calls sbtcp_tick() every few to few tens of ms (timers; it also sends what the window now allows).
 * Nothing logged names addresses or ports. */
#ifndef SBTCP_H
#define SBTCP_H
#include <stdint.h>
#include "net.h"

#define SBTCP_SOCKS 4
#define SBTCP_RXBUF 32768u                  /* the receive window: what the application has not read yet */
#define SBTCP_TXBUF 16384u                  /* sent-but-unacknowledged plus not-yet-sent bytes */
#define SBTCP_MSS 1460u
#define SBTCP_OOO 8                         /* ranges held after a gap (their bytes wait in rx at their place) */

enum {
    SBTCP_FREE, SBTCP_LISTEN, SBTCP_SYN_SENT, SBTCP_SYN_RCVD, SBTCP_ESTABLISHED, SBTCP_FIN_WAIT_1, SBTCP_FIN_WAIT_2,
    SBTCP_CLOSE_WAIT, SBTCP_CLOSING, SBTCP_LAST_ACK, SBTCP_TIME_WAIT, SBTCP_CLOSED
};
/* why a connection ended badly (sbtcp_error) */
enum { SBTCP_OK, SBTCP_E_RESET, SBTCP_E_REFUSED, SBTCP_E_TIMEOUT, SBTCP_E_UNREACHABLE };

struct sbtcp_sock {
    int state, err, app_closed;             /* app_closed: freed once the connection is over */
    uint8_t rip[4];
    uint16_t rport, lport;
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, rcv_nxt, rcv_wnd_sent;
    uint32_t wl1, wl2;                      /* the segment that last set snd_wnd (RFC 9293's SND.WL1/WL2) */
    uint32_t tx_acked;                      /* data bytes acknowledged so far: tx[0] is at iss + 1 + tx_acked */
    int peer_fin;                           /* the peer's FIN arrived (after everything before it) */
    uint32_t recover;                       /* fast recovery (NewReno) until this is acknowledged */
    uint32_t ooo_s[SBTCP_OOO], ooo_e[SBTCP_OOO];
    unsigned ooo_n;
    uint32_t cwnd, ssthresh;
    uint16_t mss;                           /* what we send per segment */
    unsigned dupacks, retries;
    int fin_queued;                         /* close() asked: a FIN after the queued bytes */
    int rtt_on;                             /* a round trip being timed (Karn: never a retransmitted one) */
    uint32_t rtt_seq;
    uint64_t rtt_at, rto_at, tw_at, ack_at, syn_at;
    uint32_t srtt, rttvar, rto;             /* ms; srtt 0 = no sample yet */
    uint32_t rx_head, rx_len, tx_len;
    int listener;                           /* SYN_RCVD from a listen(): the listening slot (-1 none) */
    int accepted;
    /* the buffers last: slot_init clears everything before them */
    uint8_t rx[SBTCP_RXBUF];
    uint8_t tx[SBTCP_TXBUF];                /* tx[0] is the data byte at iss + 1 + tx_acked */
};

struct sbtcp {
    struct sbnet *net;
    struct sbtcp_sock s[SBTCP_SOCKS];
    uint16_t next_port;
    uint32_t rx_segments, rx_bad, rx_dropped, tx_segments, retransmits, resets_sent;
};

void sbtcp_init(struct sbtcp *t, struct sbnet *net);
/* the network went down (sbnet_stop): every connection ends with SBTCP_E_UNREACHABLE */
void sbtcp_down(struct sbtcp *t);
void sbtcp_input(struct sbtcp *t, const uint8_t from[4], const uint8_t *seg, uint32_t len);
void sbtcp_tick(struct sbtcp *t);

/* a connection to ip:port: its slot, or -1 (no free slot, not bound) */
int sbtcp_connect(struct sbtcp *t, const uint8_t ip[4], uint16_t port);
/* listen on port: the listening slot, or -1; sbtcp_accept gives each established connection's slot once */
int sbtcp_listen(struct sbtcp *t, uint16_t port);
int sbtcp_accept(struct sbtcp *t, int listener);
/* bytes queued (0..len): fewer when the send buffer is full; -1 the connection cannot send (closed, error) */
int sbtcp_send(struct sbtcp *t, int i, const void *data, uint32_t len);
/* bytes read (1..cap); 0 nothing yet; -1 the peer finished (end of stream, everything read); -2 an error */
int sbtcp_recv(struct sbtcp *t, int i, void *buf, uint32_t cap);
/* the state (SBTCP_*), the error (SBTCP_E_*), bytes queued but not yet acknowledged */
int sbtcp_state(const struct sbtcp *t, int i);
int sbtcp_error(const struct sbtcp *t, int i);
uint32_t sbtcp_unacked(const struct sbtcp *t, int i);
/* the end: close() sends what is queued, then a FIN, and frees the slot when the connection is over; abort()
 * sends a RST at once and frees it */
void sbtcp_close(struct sbtcp *t, int i);
void sbtcp_abort(struct sbtcp *t, int i);

#endif
