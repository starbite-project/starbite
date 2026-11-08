/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm host test: a fake CP. It attaches to the SBD layout the AP built in a memory buffer, answers the FMT
 * requests the AP sends, and pushes notifications, with canned SIM / network scenarios. It also models the
 * registers the read-only probe looks at. Nothing here is a real identity: the "home" scenario uses the
 * 001-01 test network. */
#ifndef FAKECP_H
#define FAKECP_H

#include "../sbm.h"

enum fake_scenario {
    FAKE_NOSIM,     /* this phone today: SIM absent, CS/PS emergency-only on GSM */
    FAKE_HOME,      /* SIM ready, registered home on LTE, PLMN 00101, 3 bars */
    FAKE_SEARCH,    /* SIM ready, searching, no PLMN */
};

struct fakecp {
    struct sbm_sbd sbd;
    enum fake_scenario sc;
    uint8_t mseq;
    uint32_t n_req, n_resp, n_noti, n_unknown, n_bad, n_dropped;
    uint32_t n_call;            /* CALL-group frames seen from the AP: must stay 0 */
    uint32_t n_forbidden;       /* anything sbm_ipc_allowed() refuses, seen on the wire: must stay 0 */
    uint8_t last_ch;
};

int fakecp_attach(struct fakecp *fc, uint8_t *ipc, uint32_t size, enum fake_scenario sc);
/* The notifications the CP sends after PHONE_START (the captured order). */
void fakecp_boot_notis(struct fakecp *fc);
/* Reads every UL frame on the FMT channels and answers it. Returns the number of requests handled. */
int fakecp_step(struct fakecp *fc);
/* Queues one IPC message (NOTI or RESP) as a frame on a FMT channel's DL ring. */
int fakecp_send(struct fakecp *fc, uint8_t ch, uint8_t aseq, uint16_t cmd, uint8_t type, const uint8_t *d, uint16_t n);
/* Same, split into multi-frame fragments of at most frag payload bytes. */
int fakecp_send_multi(struct fakecp *fc, uint8_t ch, uint8_t id, uint16_t cmd, uint8_t type, const uint8_t *d,
                      uint16_t n, uint32_t frag);
/* Puts raw bytes on a DL ring (malformed-frame tests). */
int fakecp_raw(struct fakecp *fc, uint8_t ch, const uint8_t *b, uint32_t n);
void fakecp_set_scenario(struct fakecp *fc, enum fake_scenario sc);   /* + NET_REGIST / SEC notifications */

/* Register model for the probe */
struct fakeregs {
    uint32_t pmu[0x600 / 4];
    uint32_t mbox[SBM_MBOX_SIZE / 4];
    uint8_t *shm_head;          /* modem_if + 0 .. 0x400 (NULL: the carve-out faults) */
    uint8_t ipc_head[8];
    int mbox_faults;
    /* every address read, for the "only reads what it says" check */
    uint64_t trace[512];
    unsigned ntrace;
    unsigned shm_reads;
};
int fakeregs_probe32(void *ctx, uint64_t pa, uint32_t *v);

#endif
