/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: the DWC3 at 0x10c00000 as a USB 2.0 high-speed device. Core start and stop, the event
 * buffer, endpoint 0, and one TRB ring per data endpoint. Plain C; one caller at a time (the kext's work loop).
 *
 * Two ways to run the data endpoints (flag SBUSB_F_RING):
 *   single (default)  one TRB armed at a time, LST set, restarted on XferComplete: the loader's model, which
 *                     is the one that has run on this phone;
 *   ring              TRBs without LST, a link TRB closing the ring, Start Transfer once then Update Transfer,
 *                     completions by XferInProgress: the DWC3 databook's model, not yet run on this phone.
 * The bookkeeping (enqueue, dequeue, wrap) is the same in both.
 */
#ifndef SBUSB_DWC3_H
#define SBUSB_DWC3_H
#include "sbusb_io.h"
#include "sbusb_ep0.h"
#include "sbusb_phy.h"

#define SBUSB_F_RING            0x1u    /* ring mode for the data endpoints */
#define SBUSB_F_IRQ             0x2u    /* leave the event interrupt unmasked (the caller has the nub's interrupt) */

/* DMA block layout (offsets) */
#define SBUSB_EVT_BYTES         4096u
#define SBUSB_DMA_EVT           0x0000u
#define SBUSB_DMA_TRB_EP0       0x1000u         /* two TRBs: physical endpoint 0 and 1 */
#define SBUSB_RING_TRBS         8u              /* 7 transfer TRBs + the link TRB */
#define SBUSB_RING_USABLE       (SBUSB_RING_TRBS - 1u)
#define SBUSB_DMA_RING(i)       (0x1100u + 0x100u * (i))
#define SBUSB_DMA_SETUP         0x1400u
#define SBUSB_DMA_EP0BUF        0x1600u         /* 512 bytes */
#define SBUSB_DMA_CLASS         0x1800u         /* from here: the class layer's buffers (sbusb_func.h) */

#define SBUSB_RING_NOTIFY       0u
#define SBUSB_RING_IN           1u
#define SBUSB_RING_OUT          2u
#define SBUSB_RINGS             3u

enum {
	SBUSB_EV_RESET = 1,     /* bus reset: every data transfer is gone */
	SBUSB_EV_CONNECT,       /* value: 1 full speed, 0 high speed */
	SBUSB_EV_DISCONNECT,
	SBUSB_EV_CONFIGURED,    /* value: configuration (0 = deconfigured); data transfers are gone */
	SBUSB_EV_ALT,           /* value: the data interface's alternate setting; bulk transfers are gone */
	SBUSB_EV_DONE,          /* ring, cookie, value = bytes moved, status = TRB status */
	SBUSB_EV_REARM,         /* ring: a halt was cleared, its transfers are gone */
	SBUSB_EV_SUSPEND,
	SBUSB_EV_RESUME,
	SBUSB_EV_RESTART,       /* the link was restarted from the PHY up; everything is gone */
};

struct sbusb_ev {
	int type;
	unsigned ring, cookie;
	uint32_t value, status;
};

struct sbusb_ring {
	uint32_t off;                   /* DMA offset of TRB 0 */
	uint8_t phys, on;               /* physical endpoint; configured */
	uint8_t enq, deq, inflight;     /* indexes 0..SBUSB_RING_USABLE-1 */
	uint8_t started, res;           /* a transfer resource is active; its index */
	uint32_t wraps;
	struct { uint32_t buf, len; uint16_t cookie; } m[SBUSB_RING_USABLE];
};

struct sbusb_stats {
	uint32_t events, resets, conndone, disconnects, setups, stalls, e0_timeouts, set_configs;
	uint32_t cmd_fail, last_cmd_fail, restarts, suspends, unknown_events;
};

struct sbusb_dwc3 {
	struct sbusb_io io;
	struct sbusb_dma dma;
	struct sbusb_state st;
	struct sbusb_phy_report phy;
	unsigned flags;
	int started, connected, suspended;
	int want_up;                    /* the caller started it and has not stopped it: the link watch may restart it */
	uint32_t id, evt_pos;
	/* endpoint 0 */
	int e0, e0_three, e0_phys, e0_zlp, e0_pending;
	uint8_t e0_busy[2], e0_res[2];
	uint32_t e0_len[2];
	uint8_t setup[8];
	uint64_t e0_t0;
	struct sbusb_ep0_res res;
	struct sbusb_ring ring[SBUSB_RINGS];
	/* events for the class layer */
	struct sbusb_ev q[32];
	unsigned qh, qn;
	/* link watch */
	uint32_t resets_no_setup, restart_wait_ms;
	uint64_t restart_after;
	struct sbusb_stats n;
	const char *why;
};

void sbusb_dwc3_init(struct sbusb_dwc3 *d, const struct sbusb_io *io, const struct sbusb_dma *dma,
    const char *serial, unsigned flags);
/* PHY, then the core, endpoint 0 armed, pull-up on. Bounded: about 0.35 s at worst. 0, or a negative stage
 * with d->why: -1 PHY, -2 no DWC3 (GSNPSID), -3 soft reset stuck, -4 ep0 refused, -5 did not leave halt,
 * -6 DMA block too small. */
int sbusb_dwc3_start(struct sbusb_dwc3 *d);
/* Pull-up off, transfers ended, core soft reset, event buffer taken away. Safe to call twice. */
void sbusb_dwc3_stop(struct sbusb_dwc3 *d);
/* Service the controller; 1 with an event in *ev. Call until 0. */
int sbusb_dwc3_poll(struct sbusb_dwc3 *d, struct sbusb_ev *ev);
/* One transfer on a data ring: len bytes at DMA offset buf (OUT: a multiple of the max packet size).
 * 0 queued, -1 not configured or the ring is full. */
int sbusb_dwc3_queue(struct sbusb_dwc3 *d, unsigned ring, uint32_t buf, uint32_t len, unsigned cookie);
unsigned sbusb_dwc3_ring_free(const struct sbusb_dwc3 *d, unsigned ring);

#endif
