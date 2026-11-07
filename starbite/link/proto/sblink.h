/* SPDX-License-Identifier: GPL-2.0-only
 * SBLINK v1, the Starbite Link wire protocol (starbite/link/PROTOCOL.md): the portable core.
 *
 * Shared by the Windows side (the phone: IddCx display source + GenericUsbFn / TCP), the Mac viewer
 * and the host tests. Plain C99, no OS calls, no allocation: every buffer comes from the caller.
 * Uses only <stdint.h>, <stddef.h>, <string.h> (memcpy/memcmp/memset) and the embedded LZ4
 * (lz4/lz4.c, BSD-2-Clause, Yann Collet, licence in lz4/LICENSE).
 *
 *   wire       sblk_encode() / sblk_decode*(): every message, little endian, exact layouts below
 *   stream     sblk_parser: feed any byte chunks (USB reads, TCP recv), get whole messages out
 *   pixels     sblk_lz4_*, sblk_dirty_rects() (64x64 tiles, merged), sblk_solid_color(),
 *              sblk_build_rect() (picks solid / LZ4 / raw), sblk_apply_rect() (viewer side)
 *
 * Build: cc -std=c99 -c sblink.c lz4/lz4.c   (tests: build.sh in this directory)
 */
#ifndef SBLINK_H
#define SBLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- framing ---------------------------------------------------------------------------------- */

#define SBLK_MAGIC        0x4B4C4253u     /* bytes 'S' 'B' 'L' 'K' read as a little-endian u32 */
#define SBLK_VERSION      1u              /* header (framing) version */
#define SBLK_PROTO_MAJOR  1u              /* HELLO protocol version */
#define SBLK_PROTO_MINOR  0u
#define SBLK_HDR_SIZE     16u
#define SBLK_MAX_PAYLOAD  (1u << 20)      /* 1 MiB, hard limit */
#define SBLK_TCP_PORT     47474u

/* USB identity (PROTOCOL.md section 6) */
#define SBLK_USB_VID      0x1209u
#define SBLK_USB_PID      0x0002u
#define SBLK_USB_EP_IN    0x81u           /* phone -> Mac */
#define SBLK_USB_EP_OUT   0x01u           /* Mac -> phone */
#define SBLK_USB_MPS      512u

/* header flags */
#define SBLK_F_MUST_UNDERSTAND  0x0001u   /* receiver that does not know this type must close */

enum sblk_type {
    SBLK_T_HELLO         = 0x01,
    SBLK_T_MODE          = 0x02,
    SBLK_T_FRAME_BEGIN   = 0x10,
    SBLK_T_RECT          = 0x11,
    SBLK_T_FRAME_END     = 0x12,
    SBLK_T_ACK           = 0x13,
    SBLK_T_INPUT_KEY     = 0x20,
    SBLK_T_INPUT_POINTER = 0x21,
    SBLK_T_INPUT_TEXT    = 0x22,
    SBLK_T_PING          = 0x30,
    SBLK_T_PONG          = 0x31,
    SBLK_T_BYE           = 0x3F
};

/* return codes: >= 0 success, < 0 error */
enum sblk_err {
    SBLK_OK             =  0,
    SBLK_E_ARG          = -1,   /* bad argument from the caller (encode side) */
    SBLK_E_SPACE        = -2,   /* output buffer too small */
    SBLK_E_MAGIC        = -3,   /* header magic is not 'SBLK' */
    SBLK_E_VERSION      = -4,   /* header version is not 1 */
    SBLK_E_TOO_BIG      = -5,   /* payload length > limit */
    SBLK_E_SHORT        = -6,   /* payload shorter than the type's fixed fields */
    SBLK_E_FIELD        = -7,   /* a field value is invalid (sizes, encodings, lengths) */
    SBLK_E_UNKNOWN_TYPE = -8,   /* unknown type with SBLK_F_MUST_UNDERSTAND */
    SBLK_E_LZ4          = -9,   /* LZ4 data corrupt or wrong decompressed size */
    SBLK_E_BOUNDS       = -10   /* rect outside the frame buffer (sblk_apply_rect) */
};
const char *sblk_strerror(int err);
const char *sblk_type_name(int type);   /* "HELLO", ..., "?" */

typedef struct sblk_hdr {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t len;        /* payload length */
    uint32_t seq;
} sblk_hdr;

/* ---- messages ----------------------------------------------------------------------------------- */

/* HELLO roles */
#define SBLK_ROLE_SOURCE  1u   /* the phone's Windows: sends frames, takes input */
#define SBLK_ROLE_VIEWER  2u   /* the Mac app: shows frames, sends input */

/* HELLO capability bits */
#define SBLK_CAP_ENC_RAW    (1u << 0)
#define SBLK_CAP_ENC_LZ4    (1u << 1)
#define SBLK_CAP_ENC_SOLID  (1u << 2)
#define SBLK_CAP_MODE       (1u << 3)   /* source: can change size on MODE; viewer: sends MODE */
#define SBLK_CAP_INPUT_KEY  (1u << 4)
#define SBLK_CAP_INPUT_PTR  (1u << 5)
#define SBLK_CAP_INPUT_TEXT (1u << 6)
#define SBLK_CAP_EXTEND     (1u << 7)   /* source: the frames are an extra (virtual) monitor */
#define SBLK_CAP_V1_ALL     0x7Fu       /* every v1 feature except EXTEND */

#define SBLK_PIXFMT_BGRA8888 1u         /* bytes B,G,R,A per pixel; A ignored (0xFF sent) */

#define SBLK_HELLO_SIZE  56u
typedef struct sblk_hello {
    uint8_t  role;            /* SBLK_ROLE_* */
    uint8_t  proto_major;     /* 1 */
    uint8_t  proto_minor;     /* 0 */
    uint32_t caps;            /* SBLK_CAP_* */
    uint16_t width, height;   /* source: current display size; viewer: preferred size, 0 = none */
    uint32_t refresh_mhz;     /* millihertz: 60000 = 60 Hz; 0 = unknown */
    uint32_t pixel_format;    /* SBLK_PIXFMT_BGRA8888 */
    uint32_t max_payload;     /* largest payload this end accepts, <= SBLK_MAX_PAYLOAD */
    char     name[32];        /* UTF-8, NUL padded (need not be NUL terminated on the wire) */
} sblk_hello;

#define SBLK_MODE_REQUEST  0u   /* viewer -> source */
#define SBLK_MODE_APPLIED  1u   /* source -> viewer: size now in effect (also unsolicited) */
#define SBLK_MODE_REJECTED 2u   /* source -> viewer: request refused, width/height = size kept */
#define SBLK_MODE_SIZE     12u
typedef struct sblk_mode {
    uint16_t width, height;
    uint32_t refresh_mhz;
    uint8_t  status;          /* SBLK_MODE_* */
} sblk_mode;

#define SBLK_FB_KEYFRAME 0x0001u   /* the rects of this frame cover the whole display */
#define SBLK_FRAME_BEGIN_SIZE 12u
typedef struct sblk_frame_begin {
    uint32_t frame_id;
    uint16_t width, height;   /* display size this frame is drawn on */
    uint16_t nrects;          /* number of RECT messages that follow before FRAME_END */
    uint16_t fflags;          /* SBLK_FB_* */
} sblk_frame_begin;

#define SBLK_ENC_RAW   0u     /* w*h*4 bytes BGRA, rows top to bottom, no padding */
#define SBLK_ENC_LZ4   1u     /* one LZ4 block that decompresses to exactly w*h*4 bytes */
#define SBLK_ENC_SOLID 2u     /* 4 bytes: B,G,R,A; every pixel of the rect */
#define SBLK_RECT_HEAD 12u    /* fixed part of the RECT payload */
#define SBLK_RECT_MSG_HEAD (SBLK_HDR_SIZE + SBLK_RECT_HEAD)

typedef struct sblk_rect {
    uint16_t x, y, w, h;
} sblk_rect;

typedef struct sblk_rect_msg {
    sblk_rect r;
    uint8_t   encoding;       /* SBLK_ENC_* */
    uint32_t  data_len;
    const uint8_t *data;      /* decode: points into the payload; encode: the caller's bytes */
} sblk_rect_msg;

#define SBLK_FRAME_END_SIZE 16u
typedef struct sblk_frame_end {
    uint32_t frame_id;
    uint64_t capture_us;      /* source clock, microseconds */
} sblk_frame_end;

#define SBLK_ACK_SIZE 8u
typedef struct sblk_ack {
    uint32_t frame_id;        /* acknowledges every frame id <= this one (serial arithmetic) */
    uint32_t present_us;      /* viewer: decode + present time of that frame, 0 = unknown */
} sblk_ack;

/* INPUT_KEY modifier bits: the HID boot-keyboard modifier byte, plus lock states */
#define SBLK_MOD_LCTRL  0x0001u
#define SBLK_MOD_LSHIFT 0x0002u
#define SBLK_MOD_LALT   0x0004u
#define SBLK_MOD_LGUI   0x0008u
#define SBLK_MOD_RCTRL  0x0010u
#define SBLK_MOD_RSHIFT 0x0020u
#define SBLK_MOD_RALT   0x0040u
#define SBLK_MOD_RGUI   0x0080u
#define SBLK_MOD_CAPS   0x0100u
#define SBLK_MOD_NUM    0x0200u
#define SBLK_HID_PAGE_KEYBOARD 0x07u
#define SBLK_HID_PAGE_CONSUMER 0x0Cu
#define SBLK_INPUT_KEY_SIZE 8u
typedef struct sblk_input_key {
    uint16_t usage_page;      /* 0x07 keyboard, 0x0C consumer (media keys) */
    uint16_t usage;
    uint8_t  down;            /* 1 = pressed, 0 = released */
    uint16_t modifiers;       /* SBLK_MOD_* state AFTER this event */
} sblk_input_key;

#define SBLK_BTN_LEFT    0x01u
#define SBLK_BTN_RIGHT   0x02u
#define SBLK_BTN_MIDDLE  0x04u
#define SBLK_BTN_BACK    0x08u
#define SBLK_BTN_FORWARD 0x10u
#define SBLK_WHEEL_DELTA 120      /* one notch, as Windows WHEEL_DELTA */
#define SBLK_INPUT_POINTER_SIZE 16u
typedef struct sblk_input_pointer {
    int32_t  x, y;            /* absolute, display pixels of the Starbite display (may be outside) */
    uint32_t buttons;         /* SBLK_BTN_* state after this event */
    int16_t  wheel_dx;        /* 1/120 notch units, + = right */
    int16_t  wheel_dy;        /* 1/120 notch units, + = up/away from the user (Windows sign) */
} sblk_input_pointer;

#define SBLK_TEXT_MAX 1024u
typedef struct sblk_text {    /* INPUT_TEXT payload, and BYE's optional message */
    uint32_t len;
    const uint8_t *utf8;      /* decode: points into the payload */
} sblk_text;

#define SBLK_PING_SIZE 16u
typedef struct sblk_ping {
    uint32_t id;
    uint64_t t_us;            /* sender's clock */
} sblk_ping;

#define SBLK_PONG_SIZE 24u
typedef struct sblk_pong {
    uint32_t id;
    uint64_t echo_us;         /* the PING's t_us, unchanged */
    uint64_t t_us;            /* responder's clock when it answered */
} sblk_pong;

#define SBLK_BYE_NORMAL    0u
#define SBLK_BYE_PROTOCOL  1u   /* the peer sent something invalid */
#define SBLK_BYE_VERSION   2u   /* unsupported protocol version */
#define SBLK_BYE_SHUTDOWN  3u   /* this end is going away (Windows shutting down, app quit) */
#define SBLK_BYE_BUSY      4u   /* another viewer is connected */
#define SBLK_BYE_SIZE      4u
#define SBLK_BYE_TEXT_MAX  256u
typedef struct sblk_bye {
    uint32_t  reason;         /* SBLK_BYE_* */
    sblk_text text;           /* optional human-readable detail, may be empty */
} sblk_bye;

typedef struct sblk_msg {
    uint8_t  type;            /* SBLK_T_*; an unknown type decodes into .u.unknown */
    uint16_t flags;           /* header flags */
    uint32_t seq;             /* header sequence */
    union {
        sblk_hello         hello;
        sblk_mode          mode;
        sblk_frame_begin   frame_begin;
        sblk_rect_msg      rect;
        sblk_frame_end     frame_end;
        sblk_ack           ack;
        sblk_input_key     key;
        sblk_input_pointer pointer;
        sblk_text          text;
        sblk_ping          ping;
        sblk_pong          pong;
        sblk_bye           bye;
        struct { uint32_t len; const uint8_t *data; } unknown;
    } u;
} sblk_msg;

int sblk_type_known(int type);

/* ---- encode / decode ----------------------------------------------------------------------------- */

/* Total encoded size (header + payload) of m, or < 0 if m is invalid. */
long sblk_encoded_size(const sblk_msg *m);

/* Writes m (header + payload) to out. Returns bytes written (> 0) or < 0. Validates like the decoder,
 * so anything sblk_encode() accepts, sblk_decode() accepts. */
long sblk_encode(const sblk_msg *m, uint8_t *out, size_t cap);

/* Header + fixed RECT fields only (SBLK_RECT_MSG_HEAD = 28 bytes); the caller sends data_len bytes of
 * rect data right after (zero-copy send of a big rect). */
int sblk_encode_rect_head(uint8_t out[SBLK_RECT_MSG_HEAD], const sblk_rect *r, uint8_t encoding,
                          uint32_t data_len, uint32_t seq);

void sblk_put_header(uint8_t out[SBLK_HDR_SIZE], uint8_t type, uint16_t flags, uint32_t len, uint32_t seq);

/* Header checks: magic, version, len <= max_payload. */
int sblk_decode_header(const uint8_t in[SBLK_HDR_SIZE], uint32_t max_payload, sblk_hdr *h);

/* Payload of a checked header into m. Pointers in m (rect data, text) point into payload. */
int sblk_decode_payload(const sblk_hdr *h, const uint8_t *payload, sblk_msg *m);

/* One whole message from a contiguous buffer. Returns bytes used (> 0), 0 if in holds less than a whole
 * message (need more), or < 0 on error. */
long sblk_decode(const uint8_t *in, size_t len, sblk_msg *m);

/* ---- incremental stream parser ------------------------------------------------------------------- */

typedef struct sblk_parser {
    uint8_t  hb[SBLK_HDR_SIZE];
    uint32_t hn;              /* header bytes collected */
    int      have_hdr;
    sblk_hdr h;
    uint8_t *buf;             /* caller's payload buffer (needed only for payloads split across feeds) */
    uint32_t cap;
    uint32_t pn;              /* payload bytes collected */
    uint32_t max_payload;
    int      err;             /* sticky: once set, every feed returns it */
    uint64_t msgs, bytes;     /* statistics */
} sblk_parser;

/* buf/cap: room for the largest payload accepted; max_payload = min(cap, SBLK_MAX_PAYLOAD). */
void sblk_parser_init(sblk_parser *p, uint8_t *buf, uint32_t cap);
void sblk_parser_reset(sblk_parser *p);   /* new session: drops partial data and the sticky error */

/* Consumes bytes from data until one message is complete or data is used up.
 *   returns 1: *m holds a message (pointers valid until the next feed or until data is freed),
 *              *used = bytes consumed; call again with data + *used for the rest
 *   returns 0: all of data consumed (*used = len), message incomplete
 *   returns <0: protocol error (sticky); the connection must be closed (PROTOCOL.md section 5)
 * A payload wholly inside data is returned in place (no copy). */
int sblk_parser_feed(sblk_parser *p, const uint8_t *data, size_t len, size_t *used, sblk_msg *m);

/* ---- LZ4 (block format) --------------------------------------------------------------------------- */

#define SBLK_LZ4_STATE_SIZE 16416u   /* >= LZ4_sizeofState() (checked at run time), 8-byte aligned */
int sblk_lz4_bound(int n);
/* state: NULL (16 KiB on the stack) or SBLK_LZ4_STATE_SIZE bytes, 8-byte aligned. accel 1 = best ratio,
 * higher = faster. Returns compressed size, or 0 if it does not fit in cap. */
int sblk_lz4_compress(const uint8_t *src, int n, uint8_t *dst, int cap, int accel, void *state);
/* Decompresses exactly dst_size bytes. Returns SBLK_OK or SBLK_E_LZ4. Safe on any input. */
int sblk_lz4_decompress(const uint8_t *src, int n, uint8_t *dst, int dst_size);

/* ---- pixels --------------------------------------------------------------------------------------- */

#define SBLK_TILE 64u
#define SBLK_DIRTY_UPDATE_PREV 0x1u   /* copy changed tiles from cur into prev while comparing */

/* Compares two BGRA frames (same size and stride) in 64x64 tiles on the absolute tile grid, only inside
 * roi (NULL = whole frame; the source passes the OS's dirty rects here so unchanged areas cost nothing).
 * Changed tiles are merged into runs per tile row, and runs with the same x span in consecutive tile
 * rows into one rect; rects are clipped to the frame and the roi. Returns the number of rects written
 * (0 = nothing changed). If more than max_out would be needed, writes ONE rect: the bounding box of all
 * changes. With SBLK_DIRTY_UPDATE_PREV prev becomes equal to cur inside the compared area. */
int sblk_dirty_rects(uint8_t *prev, const uint8_t *cur, size_t stride, uint32_t width, uint32_t height,
                     const sblk_rect *roi, unsigned flags, sblk_rect *out, int max_out);

/* 1 if every pixel of the w x h area is the same 32-bit value (stored in *bgra as B | G<<8 | R<<16 |
 * A<<24), else 0. */
int sblk_solid_color(const uint8_t *px, size_t stride, uint32_t w, uint32_t h, uint32_t *bgra);

/* Splits r into full-width horizontal strips whose raw RECT payload fits max_payload. Returns the
 * number of strips (<= max_out), or < 0 if max_out is too small. */
int sblk_split_rect(const sblk_rect *r, uint32_t max_payload, sblk_rect *out, int max_out);

/* Builds one whole RECT message (header + payload) for area r of a frame into out:
 * solid fill if allowed by caps and the area is one colour, else LZ4 if allowed and it saves at least
 * 1/8, else raw. caps = the encodings the viewer announced (SBLK_CAP_ENC_*); raw is always allowed.
 * scratch: r->w * r->h * 4 bytes (rect gathered into rows); lz4_state as for sblk_lz4_compress.
 * out needs SBLK_RECT_MSG_HEAD + w*h*4 bytes. Returns message size or < 0 (SBLK_E_TOO_BIG if the raw
 * rect would exceed SBLK_MAX_PAYLOAD: split it first). *encoding (optional) gets the choice. */
long sblk_build_rect(uint8_t *out, size_t cap, const uint8_t *frame, size_t stride, const sblk_rect *r,
                     uint32_t caps, int lz4_accel, void *lz4_state, uint8_t *scratch, uint32_t seq,
                     uint8_t *encoding);

/* Viewer side: draws a decoded RECT into a BGRA frame buffer (fb_w x fb_h, stride bytes).
 * scratch: w*h*4 bytes, needed only for LZ4 when the rect is not full-width contiguous (may be NULL
 * otherwise, then SBLK_E_ARG if it was needed). Returns SBLK_OK, SBLK_E_BOUNDS, SBLK_E_LZ4, SBLK_E_FIELD. */
int sblk_apply_rect(uint8_t *fb, size_t stride, uint32_t fb_w, uint32_t fb_h, const sblk_rect_msg *rm,
                    uint8_t *scratch);

/* ---- flow control helper -------------------------------------------------------------------------- */

#define SBLK_MAX_IN_FLIGHT 2u
/* Frames sent (FRAME_END written) whose ACK has not come back; frame ids are serial numbers that skip 0
 * when they wrap (0xFFFFFFFF -> 1), so a span across the wrap is one shorter than the plain difference. */
static inline uint32_t sblk_in_flight(uint32_t last_sent_id, uint32_t last_acked_id)
{
    uint32_t d = last_sent_id - last_acked_id;
    return (last_sent_id < last_acked_id && d) ? d - 1 : d;
}

#ifdef __cplusplus
}
#endif
#endif /* SBLINK_H */
