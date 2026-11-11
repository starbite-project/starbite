/* SPDX-License-Identifier: APSL-2.0 */
/*
 * MIPI DSIM0 command FIFO (see sbd_dsim.h). Register facts: NOTES.md (build/em-research-brightness.md holds the
 * derivation; the loader's EngineerMode sends the same packets on this phone). Written for this driver.
 */
#include "sbd_dsim.h"
#include "sbd_regs.h"

#define DSIM    SBD_BLK_DSIM

bool
sbd_dsim_ready(const struct sbd_io *io)
{
	return (sbd_rd(io, DSIM, DSIM_CLK_CTRL) & DSIM_TX_REQUEST_HSCLK) != 0 &&
	       (sbd_rd(io, DSIM, DSIM_CONFIG) & DSIM_VIDEO_MODE) == 0;
}

/* The packet has left when the header FIFO is empty: the interrupt bit, or the FIFO's own state. */
static int
wait_header_sent(const struct sbd_io *io, uint32_t timeout_us)
{
	for (uint32_t t = 0; ; t += SBD_DSIM_POLL_US) {
		uint32_t src = sbd_rd(io, DSIM, DSIM_INTSRC), fifo = sbd_rd(io, DSIM, DSIM_FIFOCTRL);

		if ((src & DSIM_INT_PH_EMPTY) || ((fifo & DSIM_FIFO_PH_EMPTY) && DSIM_FIFO_PH_COUNT(fifo) == 0)) {
			sbd_wr(io, DSIM, DSIM_INTSRC, DSIM_INT_PH_EMPTY);
			return SBD_DSIM_OK;
		}
		if (t >= timeout_us) {
			return SBD_DSIM_TIMEOUT;        /* no reset of the DSIM: the caller retries later */
		}
		io->udelay(io->ctx, SBD_DSIM_POLL_US);
	}
}

static void
write_header(const struct sbd_io *io, uint32_t type, uint32_t d0, uint32_t d1, bool bta)
{
	uint32_t keep = sbd_rd(io, DSIM, DSIM_PKTHDR) & DSIM_PKTHDR_KEEP;

	sbd_wr(io, DSIM, DSIM_PKTHDR, keep | (bta ? DSIM_PKTHDR_BTA : 0) | ((d1 & 0xffu) << 16) | ((d0 & 0xffu) << 8) |
	    (type & 0xffu));
}

int
sbd_dsim_dcs_write(const struct sbd_io *io, const uint8_t *bytes, uint32_t n, struct sbd_dsim_stats *st)
{
	int err;

	if (bytes == NULL || n == 0 || n > SBD_DSIM_MAX_PAYLOAD) {
		return SBD_DSIM_BAD_ARG;
	}
	if (!sbd_dsim_ready(io)) {
		return SBD_DSIM_NOT_READY;
	}
	/* a header still queued (a frame in flight holds commands back): let it go first */
	if (DSIM_FIFO_PH_COUNT(sbd_rd(io, DSIM, DSIM_FIFOCTRL)) != 0 &&
	    wait_header_sent(io, SBD_DSIM_TX_TIMEOUT_US) != SBD_DSIM_OK) {
		if (st != NULL) {
			st->timeouts++;
		}
		return SBD_DSIM_TIMEOUT;
	}
	sbd_wr(io, DSIM, DSIM_INTSRC, DSIM_INT_PH_EMPTY);       /* clear a stale one */
	if (n == 1) {
		write_header(io, DSI_DCS_SHORT, bytes[0], 0, false);
	} else {
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t word = 0;

			for (uint32_t k = 0; k < 4 && i + k < n; k++) {
				word |= (uint32_t)bytes[i + k] << (8 * k);
			}
			sbd_wr(io, DSIM, DSIM_PAYLOAD, word);
		}
		write_header(io, DSI_DCS_LONG, n & 0xffu, n >> 8, false);
	}
	err = wait_header_sent(io, SBD_DSIM_TX_TIMEOUT_US);
	if (st != NULL) {
		st->packets++;
		if (err != SBD_DSIM_OK) {
			st->timeouts++;
		}
	}
	return err;
}

static void
drain_rx(const struct sbd_io *io)
{
	for (int i = 0; i < SBD_DSIM_RX_WORDS && !(sbd_rd(io, DSIM, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY); i++) {
		(void)sbd_rd(io, DSIM, DSIM_RXFIFO);
	}
}

int
sbd_dsim_dcs_read(const struct sbd_io *io, uint8_t reg, uint8_t *out, uint32_t n, struct sbd_dsim_stats *st)
{
	if (out == NULL || n == 0 || n > 16) {
		return SBD_DSIM_BAD_ARG;
	}
	if (!sbd_dsim_ready(io)) {
		return SBD_DSIM_NOT_READY;
	}
	drain_rx(io);
	sbd_wr(io, DSIM, DSIM_INTSRC, DSIM_INT_RX_DONE | DSIM_INT_PH_EMPTY);
	/* the panel answers with at most n bytes */
	write_header(io, DSI_SET_MAX_RETURN, n, 0, false);
	if (wait_header_sent(io, SBD_DSIM_TX_TIMEOUT_US) != SBD_DSIM_OK) {
		goto timeout;
	}
	sbd_wr(io, DSIM, DSIM_INTSRC, DSIM_INT_RX_DONE);
	write_header(io, DSI_DCS_READ, reg, 0, true);           /* the bus is turned round for the answer */
	for (uint32_t t = 0; !(sbd_rd(io, DSIM, DSIM_INTSRC) & DSIM_INT_RX_DONE); t += SBD_DSIM_POLL_US) {
		if (t >= SBD_DSIM_RX_TIMEOUT_US) {
			goto timeout;
		}
		io->udelay(io->ctx, SBD_DSIM_POLL_US);
	}
	sbd_wr(io, DSIM, DSIM_INTSRC, DSIM_INT_RX_DONE | DSIM_INT_PH_EMPTY);
	if (st != NULL) {
		st->packets++;
	}
	for (int i = 0; i < SBD_DSIM_RX_WORDS && !(sbd_rd(io, DSIM, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY); i++) {
		uint32_t word = sbd_rd(io, DSIM, DSIM_RXFIFO);

		switch (word & 0xffu) {
		case DSI_RX_DCS_LONG:
		case DSI_RX_GEN_LONG: {
			uint32_t count = (word >> 8) & 0xffffu, got = 0;

			for (uint32_t off = 0; off < count; off += 4) {
				uint32_t data = sbd_rd(io, DSIM, DSIM_RXFIFO);

				for (uint32_t k = 0; k < 4 && off + k < count; k++) {
					if (got < n) {
						out[got++] = (uint8_t)(data >> (8 * k));
					}
				}
			}
			return (int)got;
		}
		case DSI_RX_DCS_SHORT_1:
		case DSI_RX_GEN_SHORT_1:
			out[0] = (uint8_t)(word >> 8);
			return 1;
		case DSI_RX_DCS_SHORT_2:
		case DSI_RX_GEN_SHORT_2:
			out[0] = (uint8_t)(word >> 8);
			if (n > 1) {
				out[1] = (uint8_t)(word >> 16);
			}
			return n > 1 ? 2 : 1;
		case DSI_RX_ACK_ERROR:
			if (st != NULL) {
				st->rx_errors++;
			}
			return SBD_DSIM_RX_ERROR;
		default:                /* end of transmission and the like */
			break;
		}
	}
timeout:
	if (st != NULL) {
		st->timeouts++;
	}
	return SBD_DSIM_TIMEOUT;
}
