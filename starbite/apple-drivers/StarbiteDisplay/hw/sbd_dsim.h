/* SPDX-License-Identifier: APSL-2.0 */
/*
 * MIPI DSIM0 command FIFO (Exynos 9810), command mode: DCS writes and reads while the DECON keeps sending
 * frames. Only the packet FIFO registers are touched; the link (PLL, clocks, escape mode, FIFOCTRL, resets)
 * stays as S-Boot left it. Plain C, shared by the kext and the host tests.
 */
#ifndef SBD_DSIM_H
#define SBD_DSIM_H

#include "sbd_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBD_DSIM_TX_TIMEOUT_US  50000u          /* the vendor's 50 ms: a frame holds commands back ~10 ms */
#define SBD_DSIM_RX_TIMEOUT_US  100000u
#define SBD_DSIM_POLL_US        100u
#define SBD_DSIM_RX_WORDS       64
#define SBD_DSIM_MAX_PAYLOAD    512u

enum sbd_dsim_err {
	SBD_DSIM_OK        = 0,
	SBD_DSIM_NOT_READY = -1,        /* the link is not in HS command mode */
	SBD_DSIM_TIMEOUT   = -2,        /* the header FIFO did not drain / no answer */
	SBD_DSIM_BAD_ARG   = -3,
	SBD_DSIM_RX_ERROR  = -4         /* the panel answered with an error report */
};

struct sbd_dsim_stats {
	uint32_t packets, timeouts, rx_errors;
};

/* Did the loader leave the link usable (HS clock requested, command mode)? Reads only. */
bool sbd_dsim_ready(const struct sbd_io *io);

/*
 * One DCS write. n == 1: a short write (0x05, the command alone). n >= 2: a long write (0x39), which is how
 * the vendor driver and S-Boot send every command with parameters on this panel, two-byte ones included.
 * Payload words go into the FIFO first, then the header; then the header FIFO is waited empty.
 */
int sbd_dsim_dcs_write(const struct sbd_io *io, const uint8_t *bytes, uint32_t n, struct sbd_dsim_stats *st);

/* DCS read of `n` bytes (1..16) of register `reg` from offset 0. Returns the count read or an error. */
int sbd_dsim_dcs_read(const struct sbd_io *io, uint8_t reg, uint8_t *out, uint32_t n, struct sbd_dsim_stats *st);

#ifdef __cplusplus
}
#endif
#endif /* SBD_DSIM_H */
