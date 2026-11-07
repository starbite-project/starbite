/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link, Mac viewer: the link (transports + session + reconnect). C API for the Swift app.
 *
 * One serial "link queue" owns the session (sbv), the transports and the frame buffer. Transports:
 *   USB  IOUSBHost: VID 0x1209 PID 0x0002 interface 0, bulk IN 0x81 / OUT 0x01 (PROTOCOL.md section 6),
 *        found and re-found by IOKit matching (unplug/replug), session restart per 6.3/6.4
 *   TCP  host:port (default 127.0.0.1:47474), reconnect every 1 s
 * Mode AUTO uses USB whenever the device is present, TCP otherwise.
 * Every sbl_link_* call is thread safe (input calls are queued onto the link queue).
 */
#ifndef SBL_LINK_H
#define SBL_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "sbv.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sbl_mode { SBL_MODE_AUTO = 0, SBL_MODE_USB = 1, SBL_MODE_TCP = 2 };

enum sbl_conn {
    SBL_CONN_IDLE = 0,        /* stopped */
    SBL_CONN_SEARCHING = 1,   /* waiting for the USB device / retrying TCP */
    SBL_CONN_HANDSHAKE = 2,   /* transport up, HELLO sent */
    SBL_CONN_RUNNING = 3      /* HELLO answered */
};

typedef struct sbl_config {
    int      mode;            /* SBL_MODE_* */
    char     host[256];       /* TCP host (name or address) */
    uint16_t port;            /* TCP port, 0 = 47474 */
    char     name[64];        /* this Mac's name for HELLO */
    uint16_t pref_w, pref_h;  /* preferred display size in HELLO, 0 = none */
    uint32_t refresh_mhz;
    int      verbose;         /* log every session event to stderr too */
} sbl_config;

typedef struct sbl_status {
    int      conn;            /* SBL_CONN_* */
    char     transport[96];   /* "USB 1209:0002" / "TCP 127.0.0.1:47474" / "" */
    char     peer[33];        /* source's HELLO name */
    uint32_t peer_caps;
    uint32_t width, height;   /* current frame buffer size */
    uint32_t refresh_mhz;
    double   fps;             /* frames presented per second (last ~1 s) */
    double   mbps;            /* Mbit/s received (last ~1 s) */
    double   rtt_ms;          /* last PING round trip */
    double   lag_ms;          /* last frame: capture to presented (needs a PONG for the clock offset) */
    uint64_t frames, sessions, pongs, rx_bytes;
    int      usb_present;
    char     last_error[160];
} sbl_status;

/* called on the link queue */
typedef struct sbl_callbacks {
    void *ctx;
    void (*frame)(void *ctx, const sbv_frame_info *fi);   /* copy out what you need before returning */
    void (*log)(void *ctx, const char *line);
} sbl_callbacks;

typedef struct sbl_link sbl_link;

sbl_link *sbl_link_create(const sbl_config *cfg, const sbl_callbacks *cb);
void sbl_link_start(sbl_link *l);
/* Sends BYE(reason) if a session is up, closes the transport, stops reconnecting. Waits (<= 300 ms) for
 * the BYE to leave. */
void sbl_link_stop(sbl_link *l, uint32_t bye_reason);
void sbl_link_destroy(sbl_link *l);

void sbl_link_status(sbl_link *l, sbl_status *out);

void sbl_link_key(sbl_link *l, uint16_t page, uint16_t usage, int down, uint16_t locks);
void sbl_link_pointer(sbl_link *l, int32_t x, int32_t y, uint32_t buttons, int16_t wdx, int16_t wdy);
void sbl_link_text(sbl_link *l, const char *utf8);
void sbl_link_release_all(sbl_link *l, uint16_t locks);
void sbl_link_request_mode(sbl_link *l, uint16_t w, uint16_t h);
void sbl_link_ping(sbl_link *l);
void sbl_link_request_keyframe(sbl_link *l);
/* USB: reset the device (it re-enumerates; the phone's sender gets Reset + Configured) and open the
 * interface again. For "Reconnect" when the phone does not answer. No-op in TCP mode. */
void sbl_link_reset_usb(sbl_link *l);
/* Runs block-free work on the link queue and waits for it: everything queued before has been sent. */
void sbl_link_flush(sbl_link *l);

#ifdef __cplusplus
}
#endif
#endif
