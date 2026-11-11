/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: the NCM function on top of the controller: who owns which buffer, when NTBs go out and come
 * in, the two notifications, the link state. Plain C; the kext only copies frames in and out.
 */
#ifndef SBUSB_FUNC_H
#define SBUSB_FUNC_H
#include "sbusb_dwc3.h"
#include "sbusb_ncm.h"

#define SBUSB_TX_SLOTS          4u
#define SBUSB_RX_SLOTS          4u
#define SBUSB_DMA_NOTIFY(i)     (SBUSB_DMA_CLASS + 0x40u * (i))         /* two 64-byte lines */
#define SBUSB_DMA_TX(i)         (0x2000u + SBUSB_NTB_MAX * (i))
#define SBUSB_DMA_RX(i)         (SBUSB_DMA_TX(SBUSB_TX_SLOTS) + SBUSB_NTB_MAX * (i))
#define SBUSB_DMA_BYTES         SBUSB_DMA_RX(SBUSB_RX_SLOTS)            /* 0x22000: what the kext allocates */

struct sbusb_func_ops {
	void *ctx;
	void (*rx)(void *ctx, const uint8_t *frame, uint32_t len);      /* one Ethernet frame from the host */
	void (*link)(void *ctx, int up, int full_speed);                /* the data interface opened / closed */
	void (*tx_ready)(void *ctx);                                    /* sbusb_func_tx_begin can succeed again */
};

struct sbusb_func_stats {
	uint32_t tx_ntb, tx_frames, tx_stalls, rx_ntb, rx_frames, rx_bad, rx_last_error, notifies;
};

struct sbusb_func {
	struct sbusb_dwc3 d;
	struct sbusb_func_ops ops;
	int up;                         /* configured, data interface alt 1, not suspended */
	int alt1;
	uint8_t tx_state[SBUSB_TX_SLOTS];       /* 0 free, 1 open (being filled), 2 with the controller */
	int tx_open;                    /* slot being filled, or -1 */
	unsigned tx_inflight;
	int tx_was_full;
	uint16_t seq;
	struct sbusb_ntb_w w;
	struct sbusb_func_stats n;
};

void sbusb_func_init(struct sbusb_func *f, const struct sbusb_io *io, const struct sbusb_dma *dma, const char *serial,
    unsigned flags, const struct sbusb_func_ops *ops);
int sbusb_func_start(struct sbusb_func *f);             /* sbusb_dwc3_start's result */
void sbusb_func_stop(struct sbusb_func *f);
/* Service everything once. Returns the number of controller events handled. */
unsigned sbusb_func_poll(struct sbusb_func *f);
/* Room for one frame of len bytes to the host: copy it there, then sbusb_func_tx_commit(). NULL: the link is
 * down (drop the frame: *stall = 0) or every NTB is in flight (keep it: *stall = 1, ops.tx_ready comes later). */
uint8_t *sbusb_func_tx_begin(struct sbusb_func *f, uint32_t len, int *stall);
void sbusb_func_tx_commit(struct sbusb_func *f);

#endif
