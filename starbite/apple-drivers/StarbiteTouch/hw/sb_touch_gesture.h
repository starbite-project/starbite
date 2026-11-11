/* SPDX-License-Identifier: APSL-2.0 */
/*
 * What the S9's touch screen means to Apple's HID event system. Plain C, no IOKit; host-tested.
 *
 * macOS (a pointer; MacBookAir10,1 has no touch-screen UI): the event logic of today's patches, kept as it was
 * proven on Ventura:
 *   0397 direct touch (SB_TG_DIRECT, default): the pointer goes where the finger is and follows it. It is
 *        steered by relative events (WindowServer only moves the pointer for those): every tick (16 ms) the
 *        drawn pointer (when the display tells us where it is) or a dead-reckoned estimate is compared with the
 *        finger and the difference is sent. The hotspot is measured once by pushing the pointer into the
 *        top-left corner. A short tap (< 300 ms, < 40 px) clicks there once the pointer has arrived; touch,
 *        hold still 350 ms, then move = drag (button 1 held until the finger lifts).
 *   0394 two fingers: the first finger's movement scrolls (24 panel px a line, natural direction like a Mac
 *        trackpad); a short two-finger tap = secondary click (button 2).
 *   0373 absolute pointer (SB_TG_ABSOLUTE, boot-arg): x/y normalized over the shown canvas, first finger holds
 *        button 1 (touch = click, drag = drag), a second finger turns movement into scroll lines.
 * iOS (a digitizer, 0323 + 0071): one collection event per change with a child per finger down; contacts that
 *   start within 48 px of an edge carry FromEdgeTip for life (home / app-switcher gestures need it).
 */
#ifndef SB_TOUCH_GESTURE_H
#define SB_TOUCH_GESTURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_TG_FINGERS 10

enum sb_tg_mode { SB_TG_DIRECT = 0, SB_TG_ABSOLUTE = 1 };
enum sb_tg_kind { SB_TG_REL = 1, SB_TG_ABS, SB_TG_SCROLL };

/* One HID action. REL: dx, dy (canvas points) with the new button state. ABS: nx, ny (16.16, 0..0xffff over
 * the canvas) with the new button state. SCROLL: lines (x, y), already in the event system's sign. */
struct sb_tg_action {
	enum sb_tg_kind kind;
	int32_t dx, dy;
	uint32_t buttons;
	uint32_t nx, ny;
	int32_t lines_x, lines_y;
};

struct sb_tg_config {
	int mode;                       /* enum sb_tg_mode */
	int panel_w, panel_h;           /* the contacts' coordinate space (panel pixels) */
	int strip_w;                    /* panel pixels the shown canvas covers, centred, full height */
	int canvas_w, canvas_h;         /* the canvas in points (WindowServer's display, e.g. 828 x 1792) */
	int scroll_step;                /* panel pixels per line; > 0 natural (content follows the fingers), < 0 flipped */
	unsigned tap_ms, tap_travel;    /* 300, 40 */
	unsigned hold_ms, hold_travel;  /* 350, 20: touch, hold still, move = drag */
	int click_wait_ticks;           /* 20: a click waits this many ticks for the pointer to arrive */
};
void sb_tg_default_config(struct sb_tg_config *c);

/* What the display last drew: the pointer layer's origin and a sequence number bumped per drawn frame. */
struct sb_tg_seen {
	bool valid;
	uint32_t seq;
	int32_t x, y;
};

struct sb_tg {
	struct sb_tg_config cfg;
	/* direct mode (0397) */
	int primary, last_x, last_y, travel, acc_x, acc_y;
	uint32_t down_mask;
	bool two, dragging, active;
	uint64_t down_at;
	int32_t tgt_x, tgt_y;
	int cal, cal_ticks;             /* 0 hotspot unknown, 1 pushed to the corner, 2 known */
	int32_t hot_x, hot_y;
	uint32_t sent_seq;
	int stale;
	int32_t est_x, est_y;
	bool est_valid;
	uint32_t buttons, click_button;
	int click_ticks;
	/* absolute mode (0373) */
	bool a_down[SB_TG_FINGERS];
	uint32_t a_x[SB_TG_FINGERS], a_y[SB_TG_FINGERS];
	bool a_scroll;
	int a_primary, a_px, a_py, a_acc_x, a_acc_y;
	uint32_t a_buttons;
	/* the last notable thing, for the driver's log; NULL when nothing */
	const char *note;
	/* statistics */
	unsigned taps, secondary, drags, scroll_events, moves;
};

void sb_tg_init(struct sb_tg *g, const struct sb_tg_config *c);
/* A contact change (finger 0..9, panel pixels). Returns the number of actions written (max >= 2). *kick asks
 * the caller to run sb_tg_tick soon (1 ms) when the steering timer is not running. */
int  sb_tg_contact(struct sb_tg *g, uint64_t now_ns, int finger, bool down, int x, int y,
    struct sb_tg_action *out, int max, bool *kick);
/* The steering tick (direct mode). Returns actions written (max >= 3); *again = run it again in 16 ms. */
int  sb_tg_tick(struct sb_tg *g, uint64_t now_ns, const struct sb_tg_seen *seen,
    struct sb_tg_action *out, int max, bool *again);

/* ---- iOS digitizer ---- */
#define SB_DIG_RANGE            (1u << 0)       /* kIOHIDDigitizerEventRange */
#define SB_DIG_TOUCH            (1u << 1)       /* kIOHIDDigitizerEventTouch */
#define SB_DIG_POSITION         (1u << 2)       /* kIOHIDDigitizerEventPosition */
#define SB_DIG_IDENTITY         (1u << 5)       /* kIOHIDDigitizerEventIdentity */
#define SB_DIG_FROM_EDGE_TIP    (1u << 11)      /* kIOHIDDigitizerEventFromEdgeTip */
#define SB_DIG_EDGE_PX          48

struct sb_dig_config {
	int panel_w, panel_h;           /* contacts' space */
	int logical_w, logical_h;       /* the shown display in panel pixels, centred (may exceed the panel) */
};

struct sb_dig_contact {
	int index;                      /* finger */
	uint32_t identity;              /* finger + 2, as Apple's multitouch */
	bool touch;
	uint32_t nx, ny;                /* 16.16, 0..0xffff */
	uint32_t mask;
};

struct sb_dig_frame {
	int count;
	struct sb_dig_contact c[SB_TG_FINGERS];
	uint32_t nx, ny;                /* the changed finger */
	bool any;                       /* some finger is down */
	uint32_t mask;                  /* OR of the children's masks */
	int finger;
	bool changed_touch;             /* the finger went down or up (for the log) */
};

struct sb_dig {
	struct sb_dig_config cfg;
	int off_x, off_y;
	bool down[SB_TG_FINGERS], edge[SB_TG_FINGERS];
	uint32_t x[SB_TG_FINGERS], y[SB_TG_FINGERS];
	unsigned frames;
};

void sb_dig_init(struct sb_dig *d, const struct sb_dig_config *c);
/* Returns true and fills *f when the change produces a frame (a new state or a moved finger). */
bool sb_dig_contact(struct sb_dig *d, int finger, bool down, int x, int y, struct sb_dig_frame *f);

#ifdef __cplusplus
}
#endif
#endif /* SB_TOUCH_GESTURE_H */
