/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Samsung S6SY761 touch controller (Galaxy S9 "sec,sec_ts", firmware y761_star1) at I2C 0x48: event decoding
 * and the chip protocol over an sb_i2c bus. Plain C, no IOKit; host-tested.
 *
 * Protocol (today's starlte_touch.c 0302, which follows the chip's public documentation in mainline Linux's
 * s6sy761.c as facts): BOOT_STATUS 0x55 (0x20 = application), DEVICE_ID 0x52 (3 bytes), PANEL_INFO 0x23
 * (11 bytes: max x, max y big-endian, channels at 8/9), SENSE_ON 0x10, CLEAR_EVENT_STACK 0x62,
 * READ_ONE_EVENT 0x60 (8 bytes; byte 0 = 0 when the stack is empty). An event:
 *   b0 [1:0] EID (0 = coordinate), [5:2] TID (finger + 1), [7:6] state (1 press, 2 move, 3 release)
 *   x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf, b4 major, b5 minor, b6 [5:0] z, b7 [5:0] events left
 * INT (GPA1-0) is low while the stack holds events.
 */
#ifndef SB_S6SY761_H
#define SB_S6SY761_H

#include <stdint.h>
#include <stdbool.h>
#include "sb_hsi2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_S6_I2C_ADDR          0x48u
#define SB_S6_SENSE_ON          0x10u
#define SB_S6_PANEL_INFO        0x23u
#define SB_S6_DEVICE_ID         0x52u
#define SB_S6_BOOT_STATUS       0x55u
#define SB_S6_READ_ONE_EVENT    0x60u
#define SB_S6_CLEAR_EVENT_STACK 0x62u
#define SB_S6_BOOT_APP          0x20u
#define SB_S6_FINGERS           10
#define SB_S6_EVENT_LEN         8
#define SB_S6_BURST             32      /* events read per service before the stack is cleared */
#define SB_S6_STUCK_POLLS       200     /* INT low with an empty stack this many polls: clear it */
#define SB_S6_ATTEMPTS          10      /* the chip NACKs while it scans: retry, 200 us apart */

enum sb_s6_kind {
	SB_S6_EMPTY = 0,        /* byte 0 == 0: the stack is empty */
	SB_S6_COORD,            /* a finger */
	SB_S6_STATUS,           /* EID != 0: status / gesture / vendor event, ignored */
	SB_S6_BAD,              /* a coordinate event with TID 0 or > 10, or state 0 */
};

enum sb_s6_state { SB_S6_PRESS = 1, SB_S6_MOVE = 2, SB_S6_RELEASE = 3 };

struct sb_s6_event {
	enum sb_s6_kind kind;
	int tid;                /* finger 0..9 */
	unsigned state;         /* enum sb_s6_state */
	unsigned rx, ry;        /* chip units, 0..max */
	uint8_t major, minor, z, left;
};

/* Decodes one 8-byte event. */
enum sb_s6_kind sb_s6_decode(const uint8_t e[SB_S6_EVENT_LEN], struct sb_s6_event *ev);
/* Encodes one coordinate event (scripted input, self-test, host tests). */
void sb_s6_encode(uint8_t e[SB_S6_EVENT_LEN], int tid, unsigned state, unsigned rx, unsigned ry);

/* Where the decoded contacts go: finger, down, panel pixels. */
typedef void (*sb_s6_contact_fn)(void *ctx, int finger, bool down, int x, int y);

struct sb_s6 {
	struct sb_i2c bus;
	uint8_t addr;
	unsigned panel_w, panel_h;      /* panel pixels the chip's range maps to */
	unsigned max_x, max_y;          /* chip range: PANEL_INFO, default 4096 */
	sb_s6_contact_fn contact;
	void *contact_ctx;
	bool down[SB_S6_FINGERS];
	int x[SB_S6_FINGERS], y[SB_S6_FINGERS];
	unsigned stuck;
	/* what the probe read */
	uint8_t boot_status, id[3], chan_x, chan_y;
	bool have_id, have_panel;
	/* statistics */
	unsigned events, status_events, bad_events, retries, lost, clears;
	int last_err;
};

void sb_s6_setup(struct sb_s6 *s, const struct sb_i2c *bus, unsigned panel_w, unsigned panel_h,
    sb_s6_contact_fn contact, void *ctx);
/* Register write (command) / register read, each with SB_S6_ATTEMPTS attempts. 0 or an sb_i2c_err. */
int  sb_s6_cmd(struct sb_s6 *s, uint8_t reg);
int  sb_s6_read(struct sb_s6 *s, uint8_t reg, uint8_t *buf, unsigned len);
/* BOOT_STATUS (must answer), DEVICE_ID, PANEL_INFO. 0 when the chip answered; boot_status says the mode. */
int  sb_s6_identify(struct sb_s6 *s);
/* SENSE_ON, then (after the caller's >= 20 ms) CLEAR_EVENT_STACK. */
int  sb_s6_sense_on(struct sb_s6 *s);
int  sb_s6_clear(struct sb_s6 *s);
/* Decodes and delivers one event (also the self-test's entry). Returns its kind. */
enum sb_s6_kind sb_s6_feed(struct sb_s6 *s, const uint8_t e[SB_S6_EVENT_LEN]);
/* One poll: with INT low, reads events until the stack is empty (at most SB_S6_BURST); a full burst or a
 * stack that stays "pending" but empty releases every finger and clears the stack. Returns events read. */
unsigned sb_s6_service(struct sb_s6 *s, bool int_low);
/* Releases every finger that is down (sends the ups). */
void sb_s6_release_all(struct sb_s6 *s);

#ifdef __cplusplus
}
#endif
#endif /* SB_S6SY761_H */
