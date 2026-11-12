/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: one classic Bluetooth HID keyboard: paging, pairing (Secure Simple Pairing passkey entry, Just Works,
 * the legacy PIN), encryption, the HID control / interrupt channels, boot protocol, reconnects (bt_pair.c).
 * Pure C: bt.c gives it the controller's events and ACL packets and sends the H4 bytes it hands back; the kernel /
 * shell API is in bt.h ("Pairing and HID keyboards"). This header is bt.c's glue and the host test's view. */
#ifndef SBOS_BT_PAIR_H
#define SBOS_BT_PAIR_H
#include <stdint.h>
#include "bt_hci.h"

struct btp_io {
    void (*h4)(const uint8_t *p, unsigned n);  /* a whole H4 packet (a command or an ACL packet) to the controller */
    uint64_t (*now_ms)(void);
    void (*log)(const char *s);
    int (*others_cmd)(void);                /* 1 while bt.c's own command is in flight (ours waits) */
    int (*chip_ready)(void);                /* 1: the controller is up and not scanning (new links may start) */
    int (*chip_on)(void);                   /* 1: the controller is up (scanning or not) */
    uint32_t (*random)(void);               /* for the legacy PIN */
};

void btp_init(const struct btp_io *io);
void btp_chip_off(void);                    /* the chip went off: links and queues forgotten (no HCI sent) */
int btp_event(const uint8_t *ev);           /* 1 taken */
void btp_acl(const uint8_t *pkt, unsigned n);
void btp_step(void);                        /* timers, the command / ACL queues; bounded */
int btp_cmd_busy(void);                     /* 1 while our command is in flight (bt.c's waits) */
unsigned btp_poll_ms(void);                 /* 0: nothing to poll; else step again within this many ms */
int btp_scan_ok(void);                      /* 0 while a pairing / connection is being made */
int btp_pair_dev(const struct bt_dev *d, int pairing);   /* bt_pair / bt_hid_reconnect */

/* the host test's view */
struct btp_stats {
    unsigned cmds, acl_out, acl_in, acl_queued_max, acl_dropped, l2_malformed, l2_dropped, hid_reports;
    uint16_t acl_mtu, acl_credits;
};
void btp_stats(struct btp_stats *s);
#endif
