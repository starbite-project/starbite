/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link, Mac viewer: the SBLINK v1 viewer session (starbite/link/PROTOCOL.md sections 3-5).
 *
 * Transport-free and thread-free: the caller (link.m) feeds received bytes, calls sbv_tick() a few times a
 * second, and gives a send callback (one call = one USB transfer / one TCP write). Keeps the composed
 * frame buffer (BGRA, what the Starbite Display shows), applies RECTs into it as they arrive, and calls
 * the frame callback on FRAME_END with the frame's damage, then ACKs. Not thread safe: one queue only.
 */
#ifndef SBV_H
#define SBV_H

#include <stddef.h>
#include <stdint.h>
#include "../proto/sblink.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBV_MAX_DIM     8192u   /* largest display side accepted */
#define SBV_MAX_DAMAGE  64      /* damage rects per frame before collapsing to the bounding box */
#define SBV_PING_US     1000000ull
#define SBV_DEAD_US     5000000ull

/* sbv_* error codes (beyond the core's SBLK_E_*) */
#define SBV_E_CLOSED   (-100)   /* no session (not started, or already closed) */
#define SBV_E_PEER_BYE (-101)   /* the source sent BYE (reason in s->bye_reason) */
#define SBV_E_DEAD     (-102)   /* 5 s without a message */
#define SBV_E_SEND     (-103)   /* the send callback failed */
#define SBV_E_NOMEM    (-104)
#define SBV_E_PROTO    (-105)   /* session-level protocol error (order of messages, wrong role, ...) */

enum sbv_state { SBV_IDLE = 0, SBV_HELLO_SENT = 1, SBV_RUNNING = 2, SBV_CLOSED = 3 };

/* session events (for status/logging) */
enum sbv_event { SBV_EV_HELLO = 1, SBV_EV_MODE = 2, SBV_EV_BYE = 3, SBV_EV_ERROR = 4, SBV_EV_PONG = 5,
                 SBV_EV_LOG = 6 };

typedef struct sbv_frame_info {
    uint32_t frame_id;
    uint32_t width, height;
    size_t   stride;
    const uint8_t *fb;           /* the composed frame (all rects of this frame applied) */
    const sblk_rect *damage;     /* areas changed by this frame (bounding box if there were too many) */
    int      ndamage;
    int      keyframe;           /* FRAME_BEGIN had KEYFRAME */
    int      resized;            /* the frame buffer was (re)allocated for this frame: treat all as damaged */
    int      valid;              /* a keyframe has been received for this size (else content undefined) */
    uint32_t nrects;
    uint32_t bytes;              /* payload bytes of this frame's RECTs */
    uint64_t capture_us;         /* source clock */
} sbv_frame_info;

typedef struct sbv_events {
    void *ctx;
    /* one call = one transfer; returns 0 or < 0 on failure */
    int  (*send)(void *ctx, const uint8_t *p, size_t n);
    /* monotonic microseconds */
    uint64_t (*now_us)(void *ctx);
    /* FRAME_END: copy what you need out of fi->fb now; the ACK is sent when this returns */
    void (*frame)(void *ctx, const sbv_frame_info *fi);
    /* SBV_EV_*, with a human readable line */
    void (*event)(void *ctx, int ev, const char *text);
} sbv_events;

typedef struct sbv {
    sbv_events ev;
    int state;
    sblk_parser parser;
    uint8_t *pbuf;               /* parser payload buffer (SBLK_MAX_PAYLOAD) */
    uint32_t seq;                /* next outgoing sequence number */

    sblk_hello mine, peer;
    uint32_t caps;               /* mine.caps & peer.caps */

    /* composed frame */
    uint8_t *fb, *scratch;
    uint32_t fb_w, fb_h;
    size_t   fb_cap;
    int      fb_valid;           /* a keyframe arrived since the last resize */
    int      key_asked;          /* ACK(0) sent, waiting for the keyframe */

    /* frame in progress */
    int      in_frame;
    int      resized;
    sblk_frame_begin fbeg;
    uint32_t rects_left, frame_bytes;
    sblk_rect damage[SBV_MAX_DAMAGE];
    int      ndamage;
    uint64_t t_begin;

    /* mode */
    uint16_t want_w, want_h;     /* the size the viewer would like (0 = none) */
    uint16_t asked_w, asked_h;   /* the last MODE REQUEST sent this session */
    sblk_mode last_mode;
    int      have_mode;

    /* timing */
    uint64_t last_rx_us, last_ping_us;
    uint32_t ping_id;
    uint64_t rtt_us;             /* last round trip */
    int64_t  clock_off_us;       /* source clock - viewer clock */
    int      have_off;
    uint64_t lag_us;             /* last frame: capture (source clock, translated) to presented */

    /* input state */
    uint16_t mods;               /* SBLK_MOD_* (HID modifier byte + locks) */
    uint8_t  down[32];           /* keyboard page usages 0..255 held down */
    uint32_t buttons;
    int32_t  px, py;

    /* statistics, cumulative per sbv_init */
    uint64_t rx_bytes, tx_bytes, frames, rects, enc_count[3], enc_bytes[3], sessions, pongs;
    uint32_t bye_reason;
    char     bye_text[SBLK_BYE_TEXT_MAX + 1];
    char     err_text[160];
} sbv;

/* name: this Mac's name for HELLO (UTF-8, cut to 31 bytes on a character boundary). pref_w/h: preferred
 * display size, 0 = none. */
int  sbv_init(sbv *s, const sbv_events *ev, const char *name, uint16_t pref_w, uint16_t pref_h,
              uint32_t refresh_mhz);
void sbv_free(sbv *s);

/* New session: drops all session state, sends HELLO (as its own transfer). */
int  sbv_start(sbv *s);
/* Received bytes (any chunking). Returns 0, or < 0: the session is over (BYE already sent if it was our
 * error); close the transport (TCP) or restart at PROTOCOL.md 6.3 (USB). */
int  sbv_feed(sbv *s, const uint8_t *p, size_t n);
/* Call every 100-500 ms: PING every second, dead link after 5 s silence (returns SBV_E_DEAD). */
int  sbv_tick(sbv *s);
/* Sends BYE (if a session is up) and closes the session. */
int  sbv_bye(sbv *s, uint32_t reason, const char *text);

/* Input (dropped silently unless the session is running and both ends have the capability).
 * locks: SBLK_MOD_CAPS / SBLK_MOD_NUM bits for the lock state after the event. */
int  sbv_key(sbv *s, uint16_t page, uint16_t usage, int down, uint16_t locks);
int  sbv_pointer(sbv *s, int32_t x, int32_t y, uint32_t buttons, int16_t wdx, int16_t wdy);
int  sbv_text(sbv *s, const char *utf8, size_t len);
int  sbv_release_all(sbv *s, uint16_t locks);   /* focus lost: up for every key/button held */
int  sbv_request_mode(sbv *s, uint16_t w, uint16_t h, uint32_t refresh_mhz);
int  sbv_ping(sbv *s);
int  sbv_ack_keyframe(sbv *s);                  /* ACK(0): ask for a keyframe */

int  sbv_running(const sbv *s);
const char *sbv_strerror(int e);

/* FNV-1a 64 over the w*4 visible bytes of each row (the selftest's image checksum). */
uint64_t sbv_fb_hash(const uint8_t *fb, size_t stride, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif
#endif
