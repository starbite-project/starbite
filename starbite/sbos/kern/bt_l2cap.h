/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ACL data and L2CAP basic mode for one BR/EDR link (bt_l2cap.c). Pure C, no hardware: bt_pair.c feeds it
 * the controller's ACL packets and sends what it gives back; the host test (kern/host/bthid-test.c) runs it against a
 * scripted keyboard.
 *
 * Sources (Bluetooth Core spec 5.x): HCI ACL data packets vol 4 part E 5.4.2 (handle 12 bits, Packet_Boundary flag
 * bits 12-13: 0b10 first fragment, 0b01 continuation; length le16); L2CAP vol 3 part A: the basic frame (length le16,
 * channel ID le16, payload) 3.1, the signalling channel 0x0001 and its commands 4.1-4.11 (Command Reject, Connection
 * Request / Response, Configuration Request / Response with the MTU option 5.1, Disconnection Request / Response, Echo,
 * Information), dynamic channel IDs from 0x0040. Only basic mode (no retransmission / flow control modes, no FCS).
 *
 * What it does: reassembles fragmented ACL packets into L2CAP frames (one frame at a time; a frame longer than
 * L2_RX_MAX, a continuation without a start, or a broken signalling command is dropped and counted, never a crash);
 * fragments outgoing frames to the controller's ACL packet length; opens channels both ways (an incoming request can be
 * held "pending" until the link is encrypted), configures them with our MTU (L2_MTU) and accepts the peer's options
 * (unknown non-hint options are answered "unknown option"), answers Echo and Information requests, closes channels. */
#ifndef SBOS_BT_L2CAP_H
#define SBOS_BT_L2CAP_H
#include <stdint.h>

#define L2_SIG_CID 0x0001
#define L2_MTU 672U                         /* what we take on a channel (the spec's default) */
#define L2_RX_MAX 1024U                     /* the longest reassembled frame payload kept */
#define L2_NCH 4
#define L2_RTX_MS 5000U                     /* a signalling request unanswered this long: the channel is given up */

/* signalling codes */
#define L2_CMD_REJECT 0x01
#define L2_CONN_REQ 0x02
#define L2_CONN_RSP 0x03
#define L2_CONF_REQ 0x04
#define L2_CONF_RSP 0x05
#define L2_DISC_REQ 0x06
#define L2_DISC_RSP 0x07
#define L2_ECHO_REQ 0x08
#define L2_ECHO_RSP 0x09
#define L2_INFO_REQ 0x0A
#define L2_INFO_RSP 0x0B

/* Connection Response results */
#define L2_CR_SUCCESS 0x0000
#define L2_CR_PEND 0x0001
#define L2_CR_BAD_PSM 0x0002
#define L2_CR_SEC_BLOCK 0x0003
#define L2_CR_NO_MEM 0x0004

enum { L2C_FREE = 0, L2C_WAIT_CONN, L2C_WAIT_SEC, L2C_CONFIG, L2C_OPEN, L2C_WAIT_DISC };

struct l2ch {
    uint8_t state, incoming;
    uint8_t conf_out;                       /* 0 not sent, 1 sent, 2 accepted */
    uint8_t conf_in;                        /* 1: the peer's request answered "success" (its last part) */
    uint8_t ident, req_ident;               /* our request in flight; the peer's held Connection Request */
    uint16_t psm, lcid, rcid, rmtu;
    uint64_t dead;                          /* ms: our request unanswered by then */
    uint16_t why;                           /* why it closed: a Connection Response result, or 0xFFFF timeout */
};

struct l2ops {
    /* one ACL packet (header + data) to the controller */
    void (*send_acl)(void *ctx, const uint8_t *pkt, unsigned n);
    /* an incoming Connection Request: L2_CR_SUCCESS, L2_CR_PEND (held until l2_secure), or a refusal */
    uint16_t (*accept)(void *ctx, uint16_t psm);
    void (*opened)(void *ctx, struct l2ch *c);
    void (*closed)(void *ctx, struct l2ch *c);     /* also for a channel that never opened (c->why) */
    void (*data)(void *ctx, struct l2ch *c, const uint8_t *p, unsigned n);
};

struct l2cap {
    const struct l2ops *ops;
    void *ctx;
    int up;
    uint16_t handle, acl_mtu;
    uint64_t now;                           /* ms, from l2_tick */
    struct l2ch ch[L2_NCH];
    uint8_t next_ident;
    uint16_t next_cid;
    /* reassembly */
    uint8_t rx[4 + L2_RX_MAX];
    unsigned rx_have, rx_need, rx_dropping;
    /* counters (for the log) */
    unsigned frames_in, frames_out, acl_out, dropped, malformed, rejects;
};

void l2_init(struct l2cap *l, const struct l2ops *ops, void *ctx);
void l2_link_up(struct l2cap *l, uint16_t handle, uint16_t acl_mtu);
void l2_link_down(struct l2cap *l);         /* every channel closed (closed() called), nothing sent */
void l2_tick(struct l2cap *l, uint64_t now_ms);
/* one ACL packet from the controller (its 4-byte header included) */
void l2_acl_in(struct l2cap *l, const uint8_t *pkt, unsigned n);
struct l2ch *l2_connect(struct l2cap *l, uint16_t psm);      /* 0 if no free channel / no link */
void l2_secure(struct l2cap *l, int ok);    /* the held incoming requests: accepted (and configured) or refused */
int l2_send(struct l2cap *l, struct l2ch *c, const uint8_t *p, unsigned n);   /* 1 sent, 0 not open / too long */
void l2_disconnect(struct l2cap *l, struct l2ch *c);
struct l2ch *l2_find_psm(struct l2cap *l, uint16_t psm);
/* a whole L2CAP frame (header + payload) out on the link, fragmented to acl_mtu */
void l2_frame_out(struct l2cap *l, uint16_t cid, const uint8_t *p, unsigned n);
#endif
