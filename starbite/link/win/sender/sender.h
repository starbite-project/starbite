/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd, the Starbite Link sender (the phone's side of SBLINK v1, starbite/link/PROTOCOL.md).
 *
 * Portable parts (built for Windows ARM64 and for the Mac host tests):
 *   pipeline.c   dirty-region accumulator + frame encoder (sblk_dirty_rects / split / build_rect)
 *   flow         at most 2 frames in flight (sbl_flow_*, below)
 *   input_map.c  HID usage -> scan code / VK tables, absolute pointer mapping, key/button state
 *   session.c    SBLINK session: HELLO, ACK, MODE, PING, BYE, input dispatch, capture/send loop
 *   tcp.c        TCP listener (winsock / BSD sockets)
 * Platform parts: capture_win.c (DXGI duplication, GDI fallback), inject_win.c (SendInput),
 * usbfn_win.c (GenericUsbFn), and for the host: capture_synth.c, inject_rec.c.
 */
#ifndef SBL_SENDER_H
#define SBL_SENDER_H

#include "plat.h"
#include "../../proto/sblink.h"

/* ---- dirty region accumulator --------------------------------------------------------------- */

#define SBL_REGION_MAX 64
typedef struct sbl_region {
    int n;
    sblk_rect r[SBL_REGION_MAX];
} sbl_region;

void sbl_region_clear(sbl_region *g);
/* Adds x,y,w,h clipped to fw x fh. Contained rects are dropped; a rect containing others replaces them;
 * when full, everything collapses into the bounding box. */
void sbl_region_add(sbl_region *g, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fw, uint32_t fh);
void sbl_region_full(sbl_region *g, uint32_t fw, uint32_t fh);

/* ---- frame encoder ------------------------------------------------------------------------------ */

#define SBL_MAX_RECTS 4096     /* dirty rects per frame before falling back to one bounding box */
#define SBL_MAX_STRIPS 8192

typedef struct sbl_enc {
    uint8_t *prev;             /* shadow of what the viewer has (same stride as the capture) */
    size_t   stride;
    uint32_t w, h;
    uint8_t *scratch;          /* SBLK_MAX_PAYLOAD: one rect gathered for LZ4 */
    void    *lz4state;
    sblk_rect *dirty, *strips;
    uint8_t *out;              /* the encoded frame: FRAME_BEGIN, RECTs, FRAME_END */
    size_t   outcap, outlen;
    int      lz4_accel;
    /* last frame */
    int      keyframe;
    uint32_t nrects, nsolid, nlz4, nraw;
    uint64_t area;             /* pixels sent */
} sbl_enc;

int  sbl_enc_init(sbl_enc *e, int lz4_accel);
void sbl_enc_free(sbl_enc *e);
/* Forgets the shadow: the next frame is a keyframe. */
void sbl_enc_reset(sbl_enc *e);

/* Encodes one frame of cur (w x h BGRA, stride bytes) into e->out.
 * keyframe: send everything (also forced when size/stride changed or after sbl_enc_reset).
 * Otherwise only the tiles inside dirty (the OS's dirty rects) that really differ from the shadow.
 * caps: the encodings both ends have; max_payload: the viewer's HELLO max_payload.
 * *seq: sequence number of the first message, advanced past the last one written.
 * Returns the frame size (> 0), 0 when nothing changed (nothing to send, *seq untouched), or < 0. */
long sbl_enc_frame(sbl_enc *e, const uint8_t *cur, size_t stride, uint32_t w, uint32_t h,
                   const sbl_region *dirty, int keyframe, uint32_t caps, uint32_t max_payload,
                   uint32_t frame_id, uint64_t capture_us, uint32_t *seq);

/* ---- flow control (PROTOCOL.md: ACK) ------------------------------------------------------------ */

/* Frame ids are 1, 2, ... wrapping to 1 (0 is never used), so in flight is counted explicitly
 * instead of last_sent - last_acked (which is one too high across the wrap). */
typedef struct sbl_flow {
    uint32_t next_id;          /* id of the next frame */
    uint32_t last_sent;        /* valid when sent_any */
    int      sent_any;
    uint32_t ids[SBLK_MAX_IN_FLIGHT];
    uint64_t ts[SBLK_MAX_IN_FLIGHT];   /* when each one was sent (plat_now_us) */
    int      n;                /* frames in flight */
} sbl_flow;

void sbl_flow_reset(sbl_flow *f);            /* ids start again at 1 */
/* A new session: nothing in flight, but the ids go on from where they were, so an ACK the old session's
 * viewer sent late (or a stale one after a restart) can never acknowledge one of the new frames. */
void sbl_flow_restart(sbl_flow *f);
static inline int sbl_flow_can_send(const sbl_flow *f) { return f->n < (int)SBLK_MAX_IN_FLIGHT; }
uint32_t sbl_flow_take_id(sbl_flow *f);      /* the id for the next frame (call only when it is sent) */
/* Record a frame as in flight BEFORE it is handed to the link: a fast viewer's ACK can arrive while the
 * send is still returning (seen on the S9: "ACK(35) for a frame not sent", then flow control stuck). */
void sbl_flow_sent(sbl_flow *f, uint32_t id, uint64_t now_us);
static inline uint64_t sbl_flow_oldest_us(const sbl_flow *f) { return f->n ? f->ts[0] : 0; }
/* ACK(id), id != 0: 1 = frames acknowledged, 0 = old/duplicate ACK, -1 = id never sent */
int  sbl_flow_ack(sbl_flow *f, uint32_t id);

/* ---- input --------------------------------------------------------------------------------------- */

/* Geometry for absolute pointer mapping: the captured output inside the virtual desktop. */
typedef struct sbl_geom {
    int32_t  out_x, out_y, out_w, out_h;      /* the captured output, virtual-desktop pixels */
    int32_t  virt_x, virt_y, virt_w, virt_h;  /* SM_XVIRTUALSCREEN.. (filled by the injector) */
    uint32_t disp_w, disp_h;                  /* the frame size the viewer sees */
} sbl_geom;

enum sbl_ev_kind {
    SBL_EV_KEY_SCAN = 1,   /* code = set-1 scan code, flags EXT for the E0 prefix */
    SBL_EV_KEY_VK,         /* code = Windows virtual key (Pause, media keys) */
    SBL_EV_UNICODE,        /* code = one UTF-16 unit */
    SBL_EV_MOVE_ABS,       /* x, y = 0..65535 over the virtual desktop */
    SBL_EV_BUTTON,         /* code = SBL_BTN_*, flags UP */
    SBL_EV_WHEEL,          /* data = 1/120 notches, + = away from the user */
    SBL_EV_HWHEEL          /* data = 1/120 notches, + = right */
};
#define SBL_EVF_UP  0x01
#define SBL_EVF_EXT 0x02
enum { SBL_BTN_L = 0, SBL_BTN_R, SBL_BTN_M, SBL_BTN_X1, SBL_BTN_X2 };

typedef struct sbl_ev {
    uint8_t  kind, flags;
    uint16_t code;
    int32_t  x, y, data;
} sbl_ev;

typedef struct sbl_input {
    uint8_t  keys[32];         /* HID keyboard usages (page 7) we pressed and have not released */
    uint16_t cons[8];          /* consumer usages held */
    int      ncons;
    uint32_t buttons;          /* SBLK_BTN_* we hold down */
} sbl_input;

/* HID keyboard usage -> set-1 scan code (0x100 = E0 prefix), 0 = none. */
uint16_t sbl_hid_scan(uint16_t usage);
/* Keys injected by virtual key instead: Pause (page 7) and the consumer page. 0 = none. */
uint8_t  sbl_hid_vk(uint16_t page, uint16_t usage);
/* Display pixel -> 0..65535 normalized virtual-desktop coordinate (SendInput ABSOLUTE|VIRTUALDESK),
 * a quarter pixel into the target pixel (right whether Windows truncates or rounds); x/y outside the
 * display are clamped to it. */
void sbl_map_abs(const sbl_geom *g, int32_t x, int32_t y, int32_t *nx, int32_t *ny);

#define SBL_MAX_EV_KEY 16
#define SBL_MAX_EV_PTR 16
int sbl_in_key(sbl_input *st, const sblk_input_key *k, sbl_ev *out, int max);
int sbl_in_pointer(sbl_input *st, const sblk_input_pointer *p, const sbl_geom *g, sbl_ev *out, int max);
/* UTF-8 -> UTF-16 key down/up pairs (\n, \r = Enter, \t = Tab by VK); invalid UTF-8 = U+FFFD.
 * max >= 4 * len is always enough. */
int sbl_in_text(const uint8_t *s, uint32_t n, sbl_ev *out, int max);
int sbl_in_release_all(sbl_input *st, sbl_ev *out, int max);
#define SBL_MAX_EV_RELEASE (256 + 8 + 5)

/* platform injector (inject_win.c / inject_rec.c) */
int  sbl_inject(const sbl_ev *ev, int n);    /* returns the events injected (SendInput's count) */
void sbl_inject_geom(sbl_geom *g);           /* fills virt_* (physical pixels: the thread is made DPI aware) */
int  sbl_follow_input_desktop(int force);    /* Windows: thread onto the input desktop (inject_win.c); host: 0 */

/* What the injector did since the last take (the 10 s stats line). Counters reset on each take; the
 * desktop and cursor fields keep their last value. */
typedef struct sbl_inj_stats {
    uint32_t calls;            /* SendInput batches */
    uint32_t events;           /* events handed to SendInput */
    uint32_t injected;         /* events SendInput reported as injected */
    uint32_t short_calls;      /* batches where fewer were injected */
    uint32_t first_err;        /* GetLastError right after the first short SendInput (before any other call) */
    uint32_t last_err;         /* ... after the last short one (or after its retry) */
    uint32_t desk_moves;       /* injecting threads moved to another desktop */
    uint32_t desk_fails;       /* OpenInputDesktop / SetThreadDesktop failures */
    uint32_t desk_err;         /* the last such failure's error */
    char     desk[32];         /* desktop of the thread that injected last ("Default", "Winlogon") */
    int      cur_valid;        /* GetCursorInfo worked (at most once a second after pointer events) */
    int32_t  cur_x, cur_y;
    uint32_t cur_flags;        /* CURSOR_SHOWING 1, CURSOR_SUPPRESSED 2; 0 = hidden */
    int      mouse_present;    /* GetSystemMetrics(SM_MOUSEPRESENT) */
} sbl_inj_stats;
void sbl_inject_take_stats(sbl_inj_stats *out);

/* ---- configuration ------------------------------------------------------------------------------- */

typedef struct sbl_cfg {
    int  tcp_port;             /* 0 = no TCP listener */
    char tcp_bind[64];         /* "127.0.0.1" by default; "0.0.0.0" to accept from the network */
    int  usb;                  /* serve GenericUsbFn */
    int  fps;                  /* frame rate cap (30) */
    int  gdi_fps;              /* GDI fallback poll rate (10) */
    int  dead_ms;              /* no message for this long = link dead (5000) */
    int  stall_ms;             /* frames in flight unacknowledged this long = forget them, keyframe (3000) */
    int  keep_awake;           /* keep Windows and the display awake while a viewer is connected (1) */
    int  diag;                 /* log every OS frame's dirty/move rects (--diag) */
    int  ping_ms;              /* PING when nothing was sent for this long (1000) */
    int  lz4_accel;            /* 1 */
    int  force_gdi;
    int  usb_chunk;            /* bytes per USB IN transfer (256 KiB) */
    char output[128];          /* "" = Starbite Display else primary; "primary"; "\\.\DISPLAYn"; a name */
    char name[32];             /* HELLO name */
    int  verbose;
} sbl_cfg;

void sbl_cfg_default(sbl_cfg *c);

/* ---- capture (capture_win.c / capture_synth.c) --------------------------------------------------- */

typedef struct sbl_cap sbl_cap;
typedef struct sbl_cap_info {
    uint32_t width, height, refresh_mhz;
    int      is_starbite;      /* the Starbite Display (IddCx): EXTEND, MODE */
    int      can_mode;         /* sbl_cap_set_mode() may work */
    sbl_geom geom;
    char     name[96];         /* monitor name, UTF-8 */
    const char *method;        /* "dxgi", "gdi", "synth" */
} sbl_cap_info;

#define SBL_CAP_RESET (-100)   /* re-open the capture (access lost, mode changed, desktop switch) */

/* 0 = open, -2 = access denied (secure desktop, a session not ready yet: retry soon), other < 0 = error */
int  sbl_cap_open(sbl_cap **c, const sbl_cfg *cfg, sbl_cap_info *info);
void sbl_cap_close(sbl_cap *c);
/* Waits up to timeout_ms for the next OS frame and adds its changed areas to *dirty.
 * 1 = changes added, 0 = none (timeout), SBL_CAP_RESET, other < 0 = error. */
int  sbl_cap_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us);
int  sbl_cap_valid(sbl_cap *c);   /* a whole image has been captured (keyframes possible) */
int  sbl_cap_map(sbl_cap *c, const uint8_t **px, size_t *stride);
void sbl_cap_unmap(sbl_cap *c);
/* 1 = change started (sbl_cap_next will return SBL_CAP_RESET), 0 = already that size, < 0 = refused. */
int  sbl_cap_set_mode(sbl_cap *c, uint32_t w, uint32_t h, uint32_t refresh_mhz);

/* capture diagnostics, cumulative since the open (for the stats line and --diag) */
typedef struct sbl_cap_stats {
    uint64_t os_frames;        /* AcquireNextFrame calls that returned a frame */
    uint64_t accumulated;      /* sum of AccumulatedFrames (> os_frames: DWM presented more than we took) */
    uint64_t ptr_only;         /* frames with only a pointer update */
    uint64_t move_rects, dirty_rects;
    uint64_t full;             /* whole-image copies (first frame, metadata missing or unreadable) */
    uint64_t no_meta;          /* presents without any metadata (handled as full) */
} sbl_cap_stats;
void sbl_cap_get_stats(sbl_cap *c, sbl_cap_stats *st);

/* ---- links and the sender ------------------------------------------------------------------------ */

#define SBL_RXBUF (64u * 1024u)   /* largest viewer payload accepted (HELLO max_payload) */

typedef struct sbl_link sbl_link;
struct sbl_link {
    char name[64];
    int  is_usb;
    int  (*send)(sbl_link *l, const uint8_t *p, size_t n);   /* one batch; 0 ok, < 0 failed */
    void (*close)(sbl_link *l);                              /* TCP: shut the socket; USB: nothing */
    void *ctx;
    /* owned by session.c */
    sblk_parser parser;
    uint8_t  pbuf[SBL_RXBUF];
    int      hunting;          /* USB: ignore OUT data until a transfer starts with HELLO */
    /* watchdog: a receive loop that wakes at least every few seconds (USB) sets watch_rx and stamps
     * rx_beat_us on every turn; one that stops turning is stuck in the driver or on a lock */
    int      watch_rx;
    volatile uint64_t rx_beat_us;
    /* optional: one line of transport state for the stats line (USB: bus state, last event) */
    void (*describe)(sbl_link *l, char *buf, size_t n);
};
void sbl_link_init(sbl_link *l, const char *name, int is_usb);

/* input received in one stats window */
typedef struct sbl_in_counts {
    uint32_t ptr, btn, whl, key, txt;   /* POINTER messages, button edges, wheel steps, KEY, TEXT */
    uint32_t nocap;                     /* INPUT_* ignored: the capability was not agreed */
    uint32_t early;                     /* INPUT_* from a link without a session (before HELLO) */
    uint32_t evs;                       /* injection events produced */
} sbl_in_counts;

typedef struct sbl_sender {
    sbl_cfg cfg;
    volatile int stop;
    plat_mutex txlk;           /* held for every send; a frame is sent as one batch */
    plat_mutex lk;             /* session state below */
    plat_event wake;           /* HELLO, ACK, MODE, BYE, link end -> capture loop */

    sbl_link *owner;           /* the link that has the viewer (TCP: from accept; USB: from HELLO) */
    int      active;           /* HELLO exchanged */
    uint32_t gen;              /* session generation */
    sblk_hello peer;
    uint32_t caps;             /* ours & the viewer's */
    uint32_t tx_seq;
    sbl_flow flow;
    int      need_key;
    int      mode_req;         /* a MODE REQUEST waits for the capture loop */
    sblk_mode mode_want;
    int      mode_pending;     /* a mode change is under way: answer APPLIED after the re-open */
    uint64_t last_rx_us, last_tx_us;
    uint32_t ping_id;
    int      pong_pending;     /* a PONG the receive thread could not send (txlk busy): the loop sends it */
    sblk_pong pong_q;
    sbl_input in;
    sbl_geom geom;

    /* display as announced (from the capture loop) */
    uint32_t disp_w, disp_h, disp_refresh;
    uint32_t hello_w, hello_h;  /* the size our HELLO announced this session */
    uint32_t our_caps;         /* the bits we offer */
    int      cap_ok;

    /* statistics */
    uint64_t st_frames, st_bytes, st_enc_us, st_rects, st_blocked, st_keys;
    uint64_t st_renumbered;    /* frames renumbered because a PONG/MODE went out while encoding */
    uint64_t st_stalls;        /* flow control stalls broken by the stall timeout */
    uint64_t st_ack_bad;       /* ACKs for frames not in flight (logged the first few times) */
    uint64_t st_t0;
    /* input received in the current 10 s window (lk; logged and reset by the stats line) */
    sbl_in_counts in_win;
    sbl_in_counts in_total;    /* earlier windows (sbl_input_counts adds the current one) */
    int      in_logged;        /* this session: 1 = first pointer logged, 2 = first button edge logged */
    uint64_t in_last_us;       /* last input message (any session) */

    /* watchdog (sbl_watchdog) */
    volatile uint64_t loop_beat_us;    /* the capture loop's last turn */
    volatile uint64_t send_since_us;   /* a link send in progress since (0 = none) */
    sbl_link *watch[4];
    int      nwatch;
    char     restart_why[96];  /* set by a transport that cannot recover by itself (sbl_request_restart) */
    int      awake;            /* keep-awake currently requested */

    /* shutdown: sbl_sender_stop calls stop_hook (the USB thread's wait wakes at once); transports that
     * must tear down before the process exits count themselves in alive (sbl_sender_shutdown waits) */
    void   (*stop_hook)(void *ctx);
    void    *stop_hook_ctx;
    volatile long alive;
} sbl_sender;

int  sbl_sender_init(sbl_sender *s, const sbl_cfg *cfg);
void sbl_sender_run(sbl_sender *s);          /* the capture/send loop; returns after sbl_sender_stop */
void sbl_sender_stop(sbl_sender *s);
void sbl_sender_bye(sbl_sender *s, uint32_t reason, const char *text);   /* BYE + end session */
/* stop + wait up to ms for the transports to tear down (USB: requests cancelled, bus deactivated, handle
 * closed). 0 = all down, -1 = timed out. Safe to call more than once. */
int  sbl_sender_shutdown(sbl_sender *s, int ms);
/* input received since sbl_sender_init (all windows) */
void sbl_input_counts(sbl_sender *s, sbl_in_counts *out);

/* Watchdog. Transports register links whose receive loop must keep turning. sbl_watchdog() returns 1 (and
 * the reason) when something is stuck beyond recovery inside the process: a send that has not returned
 * for 20 s, a receive loop that has not turned for 20 s, a capture loop that has not turned for 30 s, or
 * a transport's request. The caller then restarts the process (main.c). */
void sbl_watch_link(sbl_sender *s, sbl_link *l);
void sbl_request_restart(sbl_sender *s, const char *why);
int  sbl_watchdog(sbl_sender *s, uint64_t now_us, char *why, size_t n);
#define SBL_WD_SEND_US (20000000u)
#define SBL_WD_RX_US   (20000000u)
#define SBL_WD_LOOP_US (30000000u)

/* called by the transports */
int  sbl_link_begin(sbl_sender *s, sbl_link *l);     /* TCP connection: 0 = ours, < 0 = busy (BYE sent) */
/* Stream bytes from l. transfer_start: the data starts a USB transfer. < 0: close the link (TCP). */
int  sbl_rx(sbl_sender *s, sbl_link *l, const uint8_t *p, size_t n, int transfer_start);
void sbl_link_end(sbl_sender *s, sbl_link *l, const char *why);   /* connection gone / USB reset */

/* transports */
int  sbl_tcp_start(sbl_sender *s, const char *bind_addr, int port, int *bound_port);
int  sbl_usb_start(sbl_sender *s);           /* Windows only; host build returns -1 */

#endif
