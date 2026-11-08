/* SPDX-License-Identifier: GPL-2.0-only
 * Host tests for the SBLINK v1 core (sblink.c). Built and run by build.sh (ASan + UBSan, then -O2 for
 * the timings). Exit status 0 = all passed.
 *
 *   wire      every message type: encode -> decode field by field -> re-encode byte-identical
 *             (2000 random messages per type), plus the documented error cases
 *   parser    a 3000-message stream fed whole, in 1-byte chunks and in random chunks
 *   lz4       synthetic desktop-like images and random data, through sblk_lz4_* and
 *             sblk_build_rect -> parser -> sblk_apply_rect
 *   dirty     random edits on random-size frames: every changed pixel covered, every rect made only of
 *             changed tiles, no overlaps, roi, UPDATE_PREV, overflow -> bounding box; and a full
 *             source -> viewer pipeline that must reproduce the frame exactly
 *   fuzz      10000 random buffers + 10000 mutated valid streams through the parser, sblk_decode,
 *             sblk_apply_rect and the LZ4 decoder (no crash; sanitizers watch)
 */
#include "sblink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 30) { \
    printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rs = 0x5B11A4C0FFEEull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t rn(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static const int known_types[] = {
    SBLK_T_HELLO, SBLK_T_MODE, SBLK_T_FRAME_BEGIN, SBLK_T_RECT, SBLK_T_FRAME_END, SBLK_T_ACK,
    SBLK_T_INPUT_KEY, SBLK_T_INPUT_POINTER, SBLK_T_INPUT_TEXT, SBLK_T_PING, SBLK_T_PONG, SBLK_T_BYE,
};
#define NKNOWN (int)(sizeof known_types / sizeof known_types[0])

/* random valid message of the given type; variable data goes into buf (>= 64 KiB) */
static void rand_msg(sblk_msg *m, int type, uint8_t *buf)
{
    memset(m, 0, sizeof *m);
    m->type = (uint8_t)type;
    m->seq = (uint32_t)rnd();
    switch (type) {
    case SBLK_T_HELLO:
        m->u.hello.role = rn(2) ? SBLK_ROLE_SOURCE : SBLK_ROLE_VIEWER;
        m->u.hello.proto_major = 1; m->u.hello.proto_minor = (uint8_t)rn(3);
        m->u.hello.caps = (uint32_t)rnd();
        m->u.hello.width = (uint16_t)rnd(); m->u.hello.height = (uint16_t)rnd();
        m->u.hello.refresh_mhz = (uint32_t)rnd(); m->u.hello.pixel_format = SBLK_PIXFMT_BGRA8888;
        m->u.hello.max_payload = rn(SBLK_MAX_PAYLOAD + 1);
        for (int i = 0; i < 32; i++) m->u.hello.name[i] = (char)rnd();
        break;
    case SBLK_T_MODE:
        m->u.mode.width = (uint16_t)rnd(); m->u.mode.height = (uint16_t)rnd();
        m->u.mode.refresh_mhz = (uint32_t)rnd(); m->u.mode.status = (uint8_t)rn(3);
        break;
    case SBLK_T_FRAME_BEGIN:
        m->u.frame_begin.frame_id = (uint32_t)rnd();
        m->u.frame_begin.width = (uint16_t)rnd(); m->u.frame_begin.height = (uint16_t)rnd();
        m->u.frame_begin.nrects = (uint16_t)rnd(); m->u.frame_begin.fflags = (uint16_t)rnd();
        break;
    case SBLK_T_RECT: {
        sblk_rect_msg *r = &m->u.rect;
        r->r.w = (uint16_t)(1 + rn(60)); r->r.h = (uint16_t)(1 + rn(60));
        r->r.x = (uint16_t)(rn(2) ? rn(190) : rn(0xFFFFu - r->r.w)); r->r.y = (uint16_t)(rn(2) ? rn(190) : rn(0xFFFFu - r->r.h));
        r->encoding = (uint8_t)rn(3);
        uint32_t raw = (uint32_t)r->r.w * r->r.h * 4;
        if (r->encoding == SBLK_ENC_RAW) { for (uint32_t i = 0; i < raw; i++) buf[i] = (uint8_t)rnd(); r->data_len = raw; }
        else if (r->encoding == SBLK_ENC_SOLID) { for (int i = 0; i < 4; i++) buf[i] = (uint8_t)rnd(); r->data_len = 4; }
        else {
            static uint8_t src[60 * 60 * 4];
            for (uint32_t i = 0; i < raw; i++) src[i] = (uint8_t)(rn(4) ? i / 37 : rnd());
            int c = sblk_lz4_compress(src, (int)raw, buf, 65536, 1, NULL);
            r->data_len = (uint32_t)c;
        }
        r->data = buf;
        break;
    }
    case SBLK_T_FRAME_END:
        m->u.frame_end.frame_id = (uint32_t)rnd(); m->u.frame_end.capture_us = rnd();
        break;
    case SBLK_T_ACK:
        m->u.ack.frame_id = (uint32_t)rnd(); m->u.ack.present_us = (uint32_t)rnd();
        break;
    case SBLK_T_INPUT_KEY:
        m->u.key.usage_page = (uint16_t)rnd(); m->u.key.usage = (uint16_t)rnd();
        m->u.key.down = (uint8_t)rn(2); m->u.key.modifiers = (uint16_t)rnd();
        break;
    case SBLK_T_INPUT_POINTER:
        m->u.pointer.x = (int32_t)rnd(); m->u.pointer.y = (int32_t)rnd();
        m->u.pointer.buttons = (uint32_t)rnd();
        m->u.pointer.wheel_dx = (int16_t)rnd(); m->u.pointer.wheel_dy = (int16_t)rnd();
        break;
    case SBLK_T_INPUT_TEXT:
        m->u.text.len = 1 + rn(SBLK_TEXT_MAX);
        for (uint32_t i = 0; i < m->u.text.len; i++) buf[i] = (uint8_t)rnd();
        m->u.text.utf8 = buf;
        break;
    case SBLK_T_PING:
        m->u.ping.id = (uint32_t)rnd(); m->u.ping.t_us = rnd();
        break;
    case SBLK_T_PONG:
        m->u.pong.id = (uint32_t)rnd(); m->u.pong.echo_us = rnd(); m->u.pong.t_us = rnd();
        break;
    case SBLK_T_BYE:
        m->u.bye.reason = (uint32_t)rnd();
        m->u.bye.text.len = rn(SBLK_BYE_TEXT_MAX + 1);
        for (uint32_t i = 0; i < m->u.bye.text.len; i++) buf[i] = (uint8_t)rnd();
        m->u.bye.text.utf8 = m->u.bye.text.len ? buf : NULL;
        break;
    default:   /* unknown, non-critical */
        m->u.unknown.len = rn(300);
        for (uint32_t i = 0; i < m->u.unknown.len; i++) buf[i] = (uint8_t)rnd();
        m->u.unknown.data = m->u.unknown.len ? buf : NULL;
        break;
    }
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n) { return n == 0 || memcmp(a, b, n) == 0; }

/* field-by-field equality */
static int msg_eq(const sblk_msg *a, const sblk_msg *b)
{
    if (a->type != b->type || a->flags != b->flags || a->seq != b->seq) return 0;
    switch (a->type) {
    case SBLK_T_HELLO: {
        const sblk_hello *x = &a->u.hello, *y = &b->u.hello;
        return x->role == y->role && x->proto_major == y->proto_major && x->proto_minor == y->proto_minor &&
               x->caps == y->caps && x->width == y->width && x->height == y->height &&
               x->refresh_mhz == y->refresh_mhz && x->pixel_format == y->pixel_format &&
               x->max_payload == y->max_payload && memcmp(x->name, y->name, 32) == 0;
    }
    case SBLK_T_MODE:
        return a->u.mode.width == b->u.mode.width && a->u.mode.height == b->u.mode.height &&
               a->u.mode.refresh_mhz == b->u.mode.refresh_mhz && a->u.mode.status == b->u.mode.status;
    case SBLK_T_FRAME_BEGIN:
        return a->u.frame_begin.frame_id == b->u.frame_begin.frame_id &&
               a->u.frame_begin.width == b->u.frame_begin.width && a->u.frame_begin.height == b->u.frame_begin.height &&
               a->u.frame_begin.nrects == b->u.frame_begin.nrects && a->u.frame_begin.fflags == b->u.frame_begin.fflags;
    case SBLK_T_RECT:
        return a->u.rect.r.x == b->u.rect.r.x && a->u.rect.r.y == b->u.rect.r.y && a->u.rect.r.w == b->u.rect.r.w &&
               a->u.rect.r.h == b->u.rect.r.h && a->u.rect.encoding == b->u.rect.encoding &&
               a->u.rect.data_len == b->u.rect.data_len && bytes_eq(a->u.rect.data, b->u.rect.data, a->u.rect.data_len);
    case SBLK_T_FRAME_END:
        return a->u.frame_end.frame_id == b->u.frame_end.frame_id && a->u.frame_end.capture_us == b->u.frame_end.capture_us;
    case SBLK_T_ACK:
        return a->u.ack.frame_id == b->u.ack.frame_id && a->u.ack.present_us == b->u.ack.present_us;
    case SBLK_T_INPUT_KEY:
        return a->u.key.usage_page == b->u.key.usage_page && a->u.key.usage == b->u.key.usage &&
               a->u.key.down == b->u.key.down && a->u.key.modifiers == b->u.key.modifiers;
    case SBLK_T_INPUT_POINTER:
        return a->u.pointer.x == b->u.pointer.x && a->u.pointer.y == b->u.pointer.y &&
               a->u.pointer.buttons == b->u.pointer.buttons && a->u.pointer.wheel_dx == b->u.pointer.wheel_dx &&
               a->u.pointer.wheel_dy == b->u.pointer.wheel_dy;
    case SBLK_T_INPUT_TEXT:
        return a->u.text.len == b->u.text.len && bytes_eq(a->u.text.utf8, b->u.text.utf8, a->u.text.len);
    case SBLK_T_PING:
        return a->u.ping.id == b->u.ping.id && a->u.ping.t_us == b->u.ping.t_us;
    case SBLK_T_PONG:
        return a->u.pong.id == b->u.pong.id && a->u.pong.echo_us == b->u.pong.echo_us && a->u.pong.t_us == b->u.pong.t_us;
    case SBLK_T_BYE:
        return a->u.bye.reason == b->u.bye.reason && a->u.bye.text.len == b->u.bye.text.len &&
               bytes_eq(a->u.bye.text.utf8, b->u.bye.text.utf8, a->u.bye.text.len);
    default:
        return a->u.unknown.len == b->u.unknown.len && bytes_eq(a->u.unknown.data, b->u.unknown.data, a->u.unknown.len);
    }
}

static uint8_t g_data[1 << 17];
static uint8_t g_enc[1 << 17], g_enc2[1 << 17];

static void test_wire(void)
{
    int n0 = checks, f0 = failures;
    for (int t = 0; t <= NKNOWN; t++) {
        int type = t < NKNOWN ? known_types[t] : 0x55;   /* last: an unknown type */
        for (int i = 0; i < 2000; i++) {
            sblk_msg m, d;
            rand_msg(&m, type, g_data);
            long n = sblk_encode(&m, g_enc, sizeof g_enc);
            CHECK(n >= (long)SBLK_HDR_SIZE, "%s encode %ld", sblk_type_name(type), n);
            if (n < 0) continue;
            CHECK(n == sblk_encoded_size(&m), "size");
            long u = sblk_decode(g_enc, (size_t)n, &d);
            CHECK(u == n, "%s decode %ld (%s)", sblk_type_name(type), u, sblk_strerror((int)u));
            if (u != n) continue;
            CHECK(msg_eq(&m, &d), "%s fields differ", sblk_type_name(type));
            long n2 = sblk_encode(&d, g_enc2, sizeof g_enc2);
            CHECK(n2 == n && memcmp(g_enc, g_enc2, (size_t)n) == 0, "%s re-encode", sblk_type_name(type));
            CHECK(sblk_decode(g_enc, (size_t)n - 1, &d) == 0, "short buffer must be need-more");
            if (type == SBLK_T_RECT) {   /* zero-copy head == the head sblk_encode wrote */
                uint8_t head[SBLK_RECT_MSG_HEAD];
                CHECK(sblk_encode_rect_head(head, &m.u.rect.r, m.u.rect.encoding, m.u.rect.data_len, m.seq) == SBLK_RECT_MSG_HEAD
                      && memcmp(head, g_enc, SBLK_RECT_MSG_HEAD) == 0, "rect head");
            }
        }
    }

    /* the header layout, byte for byte (PROTOCOL.md section 2) */
    sblk_msg m; memset(&m, 0, sizeof m);
    m.type = SBLK_T_PING; m.seq = 0x04030201; m.u.ping.id = 7; m.u.ping.t_us = 0x1122334455667788ull;
    long n = sblk_encode(&m, g_enc, sizeof g_enc);
    static const uint8_t want[] = { 'S','B','L','K', 1, 0x30, 0,0, 16,0,0,0, 1,2,3,4,
                                    7,0,0,0, 0,0,0,0, 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11 };
    CHECK(n == 32 && memcmp(g_enc, want, 32) == 0, "PING bytes");

    /* error cases */
    sblk_msg d; uint8_t b[64];
    memcpy(b, want, 32);
    b[0] = 'X';                CHECK(sblk_decode(b, 32, &d) == SBLK_E_MAGIC, "magic");
    memcpy(b, want, 32); b[4] = 2;  CHECK(sblk_decode(b, 32, &d) == SBLK_E_VERSION, "version");
    memcpy(b, want, 32); b[11] = 0x01; CHECK(sblk_decode(b, 32, &d) == SBLK_E_TOO_BIG, "too big (16 MiB)");
    sblk_put_header(b, SBLK_T_PING, 0, 8, 0); CHECK(sblk_decode(b, 24, &d) == SBLK_E_SHORT, "short PING");
    sblk_put_header(b, 0x55, 0, 0, 0);  CHECK(sblk_decode(b, 16, &d) == 16 && d.type == 0x55, "unknown ignorable");
    sblk_put_header(b, 0x55, SBLK_F_MUST_UNDERSTAND, 0, 0); CHECK(sblk_decode(b, 16, &d) == SBLK_E_UNKNOWN_TYPE, "unknown critical");
    /* trailing bytes after a fixed message are ignored (minor-version growth) */
    sblk_put_header(b, SBLK_T_PING, 0, 24, 9); memcpy(b + 16, want + 16, 16); memset(b + 32, 0xEE, 8);
    CHECK(sblk_decode(b, 40, &d) == 40 && d.u.ping.id == 7 && d.u.ping.t_us == 0x1122334455667788ull, "trailing ignored");
    /* RECT: raw length must be exactly w*h*4 */
    sblk_rect r = { 0, 0, 2, 2 };
    uint8_t px[16] = {0};
    sblk_msg rm; memset(&rm, 0, sizeof rm); rm.type = SBLK_T_RECT; rm.u.rect.r = r; rm.u.rect.encoding = SBLK_ENC_RAW;
    rm.u.rect.data = px; rm.u.rect.data_len = 15;
    CHECK(sblk_encode(&rm, g_enc, sizeof g_enc) == SBLK_E_FIELD, "raw len 15 for 2x2 refused by encoder");
    rm.u.rect.data_len = 16; n = sblk_encode(&rm, g_enc, sizeof g_enc);
    g_enc[8] = 27; CHECK(sblk_decode(g_enc, (size_t)n - 1, &d) == SBLK_E_FIELD, "raw len 15 refused by decoder");
    rm.u.rect.encoding = 3; CHECK(sblk_encode(&rm, g_enc, sizeof g_enc) == SBLK_E_FIELD, "bad encoding");
    rm.u.rect.encoding = SBLK_ENC_RAW; rm.u.rect.r.w = 0; CHECK(sblk_encode(&rm, g_enc, sizeof g_enc) == SBLK_E_FIELD, "w=0");
    memset(&m, 0, sizeof m); m.type = SBLK_T_HELLO; m.u.hello.role = 3;
    CHECK(sblk_encode(&m, g_enc, sizeof g_enc) == SBLK_E_FIELD, "bad role");
    m.u.hello.role = 1; CHECK(sblk_encode(&m, g_enc, 71) == SBLK_E_SPACE, "space");
    memset(&m, 0, sizeof m); m.type = SBLK_T_INPUT_TEXT;
    CHECK(sblk_encode(&m, g_enc, sizeof g_enc) == SBLK_E_FIELD, "empty text");

    printf("wire:   %d checks, %d failed (12 types + unknown x 2000 round trips, header bytes, 13 error cases)\n",
           checks - n0, failures - f0);
}

/* ---- parser ---- */

#define NSTREAM 3000
static uint8_t *g_stream;
static size_t g_stream_len;
static uint32_t g_off[NSTREAM + 1];

static void make_stream(void)
{
    g_stream = malloc(NSTREAM * 1200 + 70000);
    g_stream_len = 0;
    for (int i = 0; i < NSTREAM; i++) {
        sblk_msg m;
        int type = rn(14) == 0 ? 0x60 + (int)rn(10) : known_types[rn(NKNOWN)];
        rand_msg(&m, type, g_data);
        g_off[i] = (uint32_t)g_stream_len;
        long n = sblk_encode(&m, g_stream + g_stream_len, 70000);
        if (n < 0) { printf("make_stream encode failed\n"); exit(2); }
        g_stream_len += (size_t)n;
    }
    g_off[NSTREAM] = (uint32_t)g_stream_len;
}

/* feeds the stream in chunks chosen by mode (0 whole, 1 one byte, 2 random 1..4096, 3 random 1..70000);
 * checks every message re-encodes to the original bytes */
static int run_parser(int mode, int copy_chunks)
{
    static uint8_t pbuf[SBLK_MAX_PAYLOAD];
    sblk_parser p;
    sblk_parser_init(&p, pbuf, sizeof pbuf);
    int got = 0, bad = 0;
    size_t pos = 0;
    uint8_t *chunkcopy = malloc(70000);
    while (pos < g_stream_len) {
        size_t c = mode == 0 ? g_stream_len : mode == 1 ? 1 : mode == 2 ? 1 + rn(4096) : 1 + rn(70000);
        if (c > g_stream_len - pos) c = g_stream_len - pos;
        const uint8_t *data = g_stream + pos;
        if (copy_chunks && c <= 70000) {   /* chunk lives in a reused buffer, like a USB read buffer */
            memcpy(chunkcopy, data, c);
            data = chunkcopy;
        }
        size_t rem = c;
        while (rem) {
            size_t used; sblk_msg m;
            int r = sblk_parser_feed(&p, data, rem, &used, &m);
            if (r < 0) { printf("parser error %s at msg %d\n", sblk_strerror(r), got); free(chunkcopy); return -1; }
            data += used; rem -= used;
            if (r == 1) {
                long n = sblk_encode(&m, g_enc, sizeof g_enc);
                uint32_t want = g_off[got + 1] - g_off[got];
                if (n != (long)want || memcmp(g_enc, g_stream + g_off[got], want) != 0) bad++;
                got++;
            } else if (used == 0 && rem) { printf("parser made no progress\n"); free(chunkcopy); return -1; }
        }
        pos += c;
    }
    free(chunkcopy);
    CHECK(p.msgs == (uint64_t)got && p.bytes == g_stream_len, "parser stats %llu %llu",
          (unsigned long long)p.msgs, (unsigned long long)p.bytes);
    return bad ? -bad : got;
}

static void test_parser(void)
{
    int n0 = checks, f0 = failures;
    make_stream();
    static const char *names[] = { "whole", "1-byte", "random 1..4096", "random 1..70000" };
    for (int mode = 0; mode < 4; mode++)
        for (int cp = 0; cp < 2; cp++) {
            int r = run_parser(mode, cp);
            CHECK(r == NSTREAM, "parser mode %s copy %d: %d", names[mode], cp, r);
        }

    /* sticky error + reset */
    static uint8_t pbuf[4096];
    sblk_parser p; sblk_parser_init(&p, pbuf, sizeof pbuf);
    size_t used; sblk_msg m;
    CHECK(sblk_parser_feed(&p, (const uint8_t *)"SBLX", 4, &used, &m) == SBLK_E_MAGIC && used == 4, "early magic");
    CHECK(sblk_parser_feed(&p, g_stream, 16, &used, &m) == SBLK_E_MAGIC && used == 0, "sticky");
    sblk_parser_reset(&p);
    int r = sblk_parser_feed(&p, g_stream, g_off[1], &used, &m);
    CHECK(r == 1 && used == g_off[1], "after reset");
    /* payload larger than the parser's buffer -> TOO_BIG, not overflow */
    uint8_t h[16]; sblk_put_header(h, SBLK_T_INPUT_TEXT, 0, 5000, 0);
    sblk_parser_reset(&p);
    CHECK(sblk_parser_feed(&p, h, 16, &used, &m) == SBLK_E_TOO_BIG, "payload > buffer");
    printf("parser: %d checks, %d failed (%d msgs, %zu bytes; whole/1-byte/random chunks, in place and copied)\n",
           checks - n0, failures - f0, NSTREAM, g_stream_len);
}

/* ---- synthetic desktop ---- */

static void fill_rect(uint8_t *f, size_t stride, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) memcpy(f + (size_t)j * stride + (size_t)i * 4, &c, 4);
}

/* a Windows-11-ish desktop: gradient wallpaper, taskbar, windows with title bars, "text" lines, icons */
static void make_desktop(uint8_t *f, int w, int h, uint32_t seed)
{
    size_t stride = (size_t)w * 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = f + (size_t)y * stride + (size_t)x * 4;
            p[0] = (uint8_t)(200 - y * 120 / h); p[1] = (uint8_t)(120 + x * 60 / w); p[2] = (uint8_t)(40 + (x + y) * 40 / (w + h)); p[3] = 0xFF;
        }
    fill_rect(f, stride, 0, h - 48, w, 48, 0xFF202020);                       /* taskbar */
    for (int i = 0; i < 8; i++) fill_rect(f, stride, w / 2 - 200 + i * 50, h - 40, 32, 32, 0xFF000000u | (seed * (i + 3) * 2654435761u >> 8));
    uint64_t s = seed;
    for (int win = 0; win < 3; win++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        int ww = w / 3 + (int)((s >> 40) % (uint64_t)(w / 3)), wh = h / 3 + (int)((s >> 20) % (uint64_t)(h / 3));
        int wx = (int)((s >> 8) % (uint64_t)(w - ww)), wy = (int)((s >> 30) % (uint64_t)(h - 48 - wh));
        fill_rect(f, stride, wx, wy, ww, wh, 0xFFF3F3F3);                      /* window body */
        fill_rect(f, stride, wx, wy, ww, 32, 0xFFE0E0E0);                      /* title bar */
        for (int ly = wy + 48; ly + 14 < wy + wh; ly += 20)                     /* text lines */
            for (int lx = wx + 16; lx < wx + ww - 40; lx += 7) {
                s = s * 6364136223846793005ull + 1;
                if ((s >> 60) < 3) continue;                                    /* word gap */
                for (int gy = 0; gy < 12; gy++)
                    for (int gx = 0; gx < 6; gx++)
                        if ((s >> (gx * 3 + gy * 2 % 40)) & 1) {
                            uint8_t *p = f + (size_t)(ly + gy) * stride + (size_t)(lx + gx) * 4;
                            uint8_t v = (uint8_t)(30 + ((s >> gy) & 63));      /* anti-aliased-ish grey */
                            p[0] = p[1] = p[2] = v;
                        }
            }
    }
}

static void test_lz4(void)
{
    int n0 = checks, f0 = failures;
    static uint8_t state[SBLK_LZ4_STATE_SIZE] __attribute__((aligned(8)));
    const int W = 1280, H = 720;
    size_t sz = (size_t)W * H * 4;
    uint8_t *img = malloc(sz), *cmp = malloc((size_t)sblk_lz4_bound((int)sz)), *back = malloc(sz);

    for (int k = 0; k < 4; k++) {
        make_desktop(img, W, H, 1000u + k);
        double t0 = now_s();
        int c = sblk_lz4_compress(img, (int)sz, cmp, sblk_lz4_bound((int)sz), 1, k & 1 ? state : NULL);
        double t1 = now_s();
        CHECK(c > 0, "desktop compress");
        CHECK(sblk_lz4_decompress(cmp, c, back, (int)sz) == SBLK_OK && memcmp(img, back, sz) == 0, "desktop round trip");
        double t2 = now_s();
        if (k == 0)
            printf("lz4:    desktop %dx%d %zu -> %d bytes (%.1f%%), compress %.2f ms, decompress %.2f ms\n",
                   W, H, sz, c, 100.0 * c / sz, (t1 - t0) * 1e3, (t2 - t1) * 1e3);
        CHECK(sblk_lz4_decompress(cmp, c, back, (int)sz - 1) == SBLK_E_LZ4, "wrong size must fail");
    }
    /* random data: incompressible, must still round-trip */
    for (int k = 0; k < 50; k++) {
        int n = 1 + (int)rn(200000);
        for (int i = 0; i < n; i++) img[i] = (uint8_t)rnd();
        int c = sblk_lz4_compress(img, n, cmp, sblk_lz4_bound(n), 1 + (int)rn(8), state);
        CHECK(c > 0 && c <= sblk_lz4_bound(n), "random compress");
        CHECK(sblk_lz4_decompress(cmp, c, back, n) == SBLK_OK && memcmp(img, back, (size_t)n) == 0, "random round trip");
        CHECK(sblk_lz4_compress(img, n, cmp, n / 2, 1, state) == 0, "random does not fit in n/2");
    }
    /* mixed: desktop tiles at many sizes through build_rect -> encode -> decode -> apply */
    make_desktop(img, W, H, 77);
    uint8_t *fb = calloc(1, sz), *scratch = malloc(sz), *out = malloc(SBLK_MAX_PAYLOAD + 64);
    int counts[3] = {0};
    size_t total_raw = 0, total_wire = 0;
    for (int k = 0; k < 400; k++) {
        sblk_rect r;
        r.w = (uint16_t)(1 + rn(W)); r.h = (uint16_t)(1 + rn(H));
        r.x = (uint16_t)rn(W - r.w + 1); r.y = (uint16_t)rn(H - r.h + 1);
        sblk_rect parts[64];
        int np = sblk_split_rect(&r, SBLK_MAX_PAYLOAD, parts, 64);
        CHECK(np > 0, "split");
        for (int i = 0; i < np; i++) {
            uint8_t enc;
            long n = sblk_build_rect(out, SBLK_MAX_PAYLOAD + 64, img, (size_t)W * 4, &parts[i], SBLK_CAP_V1_ALL, 1,
                                     state, scratch, (uint32_t)k, &enc);
            CHECK(n > 0 && n <= (long)(SBLK_MAX_PAYLOAD + SBLK_HDR_SIZE), "build %ld", n);
            if (n <= 0) continue;
            counts[enc]++;
            total_raw += (size_t)parts[i].w * parts[i].h * 4; total_wire += (size_t)n;
            sblk_msg m;
            CHECK(sblk_decode(out, (size_t)n, &m) == n, "decode built rect");
            CHECK(sblk_apply_rect(fb, (size_t)W * 4, W, H, &m.u.rect, scratch + 0) == SBLK_OK, "apply");
        }
        /* the applied area must equal the source */
        int ok = 1;
        for (int y = r.y; y < r.y + r.h && ok; y++)
            ok = memcmp(fb + (size_t)y * W * 4 + (size_t)r.x * 4, img + (size_t)y * W * 4 + (size_t)r.x * 4, (size_t)r.w * 4) == 0;
        CHECK(ok, "rect %d pixels after apply", k);
    }
    /* solid detection and solid rects */
    fill_rect(img, (size_t)W * 4, 100, 100, 300, 200, 0xFF123456);
    uint32_t col = 0;
    CHECK(sblk_solid_color(img + 100 * W * 4 + 100 * 4, (size_t)W * 4, 300, 200, &col) == 1 && col == 0xFF123456, "solid");
    CHECK(sblk_solid_color(img + 100 * W * 4 + 100 * 4, (size_t)W * 4, 301, 200, &col) == 0, "not solid (1 px wider)");
    CHECK(sblk_solid_color(img + 100 * W * 4 + 100 * 4, (size_t)W * 4, 1, 1, &col) == 1, "1x1 solid");
    img[(150 * W + 399) * 4 + 1] ^= 1;
    CHECK(sblk_solid_color(img + 100 * W * 4 + 100 * 4, (size_t)W * 4, 300, 200, &col) == 0, "one odd pixel at the end of an odd row");
    /* encoding choice: solid area -> SOLID; caps without LZ4/SOLID -> RAW; noise -> RAW fallback */
    {
        sblk_rect sr = { 100, 100, 64, 64 }, nr = { 0, 0, 64, 64 };
        uint8_t enc = 9; sblk_msg m;
        long n = sblk_build_rect(out, SBLK_MAX_PAYLOAD, img, (size_t)W * 4, &sr, SBLK_CAP_V1_ALL, 1, state, scratch, 1, &enc);
        CHECK(n == SBLK_RECT_MSG_HEAD + 4 && enc == SBLK_ENC_SOLID, "solid chosen: %ld %d", n, enc);
        CHECK(sblk_decode(out, (size_t)n, &m) == n && sblk_apply_rect(fb, (size_t)W * 4, W, H, &m.u.rect, NULL) == SBLK_OK, "solid apply");
        CHECK(fb[(130 * W + 130) * 4 + 0] == 0x56 && fb[(130 * W + 130) * 4 + 2] == 0x12, "solid colour bytes");
        n = sblk_build_rect(out, SBLK_MAX_PAYLOAD, img, (size_t)W * 4, &sr, SBLK_CAP_ENC_RAW, 1, state, scratch, 1, &enc);
        CHECK(n == SBLK_RECT_MSG_HEAD + 64 * 64 * 4 && enc == SBLK_ENC_RAW, "raw only");
        for (int y = 0; y < 64; y++) for (int x = 0; x < 256; x++) img[(size_t)y * W * 4 + x] = (uint8_t)rnd();
        n = sblk_build_rect(out, SBLK_MAX_PAYLOAD, img, (size_t)W * 4, &nr, SBLK_CAP_V1_ALL, 1, state, scratch, 1, &enc);
        CHECK(n == SBLK_RECT_MSG_HEAD + 64 * 64 * 4 && enc == SBLK_ENC_RAW, "noise falls back to raw");
        CHECK(sblk_decode(out, (size_t)n, &m) == n && sblk_apply_rect(fb, (size_t)W * 4, W, H, &m.u.rect, NULL) == SBLK_OK &&
              memcmp(fb, img, 256) == 0, "raw apply");
        sblk_rect big = { 0, 0, 1280, 720 };
        CHECK(sblk_build_rect(out, SBLK_MAX_PAYLOAD, img, (size_t)W * 4, &big, SBLK_CAP_V1_ALL, 1, state, scratch, 1, &enc) == SBLK_E_TOO_BIG, "unsplit big rect");
        sblk_rect wrap = { 0, 0, 32768, 32768 };   /* w*h*4 = 2^32: wraps a u32 to 0 */
        CHECK(sblk_build_rect(out, SBLK_MAX_PAYLOAD, img, (size_t)W * 4, &wrap, SBLK_CAP_ENC_RAW, 1, state, scratch, 1, &enc) == SBLK_E_TOO_BIG, "u32-wrapping rect size");
        CHECK(sblk_in_flight(2, 0) == 2 && sblk_in_flight(5, 5) == 0 && sblk_in_flight(1, 0xFFFFFFFFu) == 1 &&
              sblk_in_flight(2, 0xFFFFFFFEu) == 3, "in flight across the frame id wrap (0 skipped)");
        sblk_rect off = { 1200, 700, 100, 30 };   /* past the frame buffer edge */
        uint8_t px4[4] = {1,2,3,4};
        sblk_rect_msg rm = { off, SBLK_ENC_SOLID, 4, px4 };
        CHECK(sblk_apply_rect(fb, (size_t)W * 4, W, H, &rm, scratch) == SBLK_E_BOUNDS, "bounds");
    }
    printf("lz4:    %d checks, %d failed (4 desktops, 50 random buffers, 400 rects -> %d raw / %d lz4 / %d solid, "
           "%zu raw bytes -> %zu on the wire = %.1f%%)\n", checks - n0, failures - f0, counts[0], counts[1], counts[2],
           total_raw, total_wire, 100.0 * total_wire / total_raw);
    free(img); free(cmp); free(back); free(fb); free(scratch); free(out);
}

/* ---- dirty rects ---- */

static int tile_has_change(const uint8_t *a, const uint8_t *b, size_t stride, uint32_t w, uint32_t h,
                           uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (uint32_t y = y0; y < y1; y++)
        if (memcmp(a + y * stride + x0 * 4, b + y * stride + x0 * 4, (x1 - x0) * 4)) return 1;
    return 0;
}

static void test_dirty(void)
{
    int n0 = checks, f0 = failures;
    int total_rects = 0, overflows = 0;
    for (int it = 0; it < 300; it++) {
        uint32_t w = 1 + rn(it < 20 ? 2000 : 700), h = 1 + rn(it < 20 ? 1200 : 500);
        size_t stride = (size_t)w * 4 + 4 * rn(3);          /* sometimes padded rows */
        size_t sz = stride * h;
        uint8_t *prev = malloc(sz), *cur = malloc(sz), *orig = malloc(sz);
        for (size_t i = 0; i < sz; i++) prev[i] = (uint8_t)(i * 7 / 5);
        memcpy(cur, prev, sz);
        int nedits = (int)rn(12);
        for (int e = 0; e < nedits; e++) {
            if (rn(3) == 0) {   /* single pixel */
                uint32_t x = rn(w), y = rn(h);
                cur[y * stride + x * 4 + rn(4)] ^= (uint8_t)(1 + rn(255));
            } else {
                uint32_t ew = 1 + rn(w / 3 + 1), eh = 1 + rn(h / 3 + 1);
                if (ew > w) ew = w;
                if (eh > h) eh = h;
                uint32_t ex = rn(w - ew + 1), ey = rn(h - eh + 1);
                fill_rect(cur, stride, (int)ex, (int)ey, (int)ew, (int)eh, (uint32_t)rnd() | 1);
            }
        }
        memcpy(orig, prev, sz);
        sblk_rect out[64];
        int max_out = it % 10 == 9 ? 2 : 64;
        int n = sblk_dirty_rects(prev, cur, stride, w, h, NULL, 0, out, max_out);
        CHECK(n >= 0, "dirty rc %d", n);
        CHECK(memcmp(prev, orig, sz) == 0, "prev untouched without UPDATE_PREV");
        total_rects += n;
        /* every changed pixel covered */
        int uncovered = 0;
        uint8_t *covmap = calloc(1, (size_t)w * h);
        for (int i = 0; i < n; i++)
            for (uint32_t y = out[i].y; y < (uint32_t)out[i].y + out[i].h && y < h; y++)
                for (uint32_t x = out[i].x; x < (uint32_t)out[i].x + out[i].w && x < w; x++) covmap[(size_t)y * w + x] = 1;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (!covmap[(size_t)y * w + x] && memcmp(prev + y * stride + x * 4, cur + y * stride + x * 4, 4)) uncovered++;
        free(covmap);
        CHECK(uncovered == 0, "it %d: %d changed pixels not covered", it, uncovered);
        int raw_tiles_dirty = 0;
        for (uint32_t ty = 0; ty < h; ty += 64)
            for (uint32_t tx = 0; tx < w; tx += 64) raw_tiles_dirty += tile_has_change(prev, cur, stride, w, h, tx, ty, tx + 64, ty + 64);
        if (n == 1 && raw_tiles_dirty > 0 && max_out == 2) overflows++;
        for (int i = 0; i < n; i++) {
            CHECK(out[i].w > 0 && out[i].h > 0 && out[i].x + out[i].w <= w && out[i].y + out[i].h <= h, "rect in frame");
            /* rects start on the tile grid; no overlaps; each covered tile changed (unless overflow box) */
            CHECK(out[i].x % 64 == 0 && out[i].y % 64 == 0, "grid aligned");
            for (int j = i + 1; j < n; j++) {
                int ov = out[i].x < out[j].x + out[j].w && out[j].x < out[i].x + out[i].w &&
                         out[i].y < out[j].y + out[j].h && out[j].y < out[i].y + out[i].h;
                CHECK(!ov, "overlap");
            }
            if (max_out == 64)
                for (uint32_t ty = out[i].y; ty < (uint32_t)out[i].y + out[i].h; ty += 64)
                    for (uint32_t tx = out[i].x; tx < (uint32_t)out[i].x + out[i].w; tx += 64)
                        CHECK(tile_has_change(prev, cur, stride, w, h, tx, ty, tx + 64, ty + 64), "clean tile inside a rect");
        }
        if (nedits == 0) CHECK(n == 0, "no edits -> no rects");
        /* UPDATE_PREV makes prev == cur, and a second pass finds nothing */
        sblk_rect out2[64];
        int n2 = sblk_dirty_rects(prev, cur, stride, w, h, NULL, SBLK_DIRTY_UPDATE_PREV, out2, 64);
        CHECK(n2 >= 0 && memcmp(prev, cur, sz) == 0, "update_prev");
        CHECK(sblk_dirty_rects(prev, cur, stride, w, h, NULL, 0, out2, 64) == 0, "second pass clean");
        /* roi: an edit inside a roi is found and clipped to it; outside the roi nothing is compared */
        if (w > 10 && h > 10) {
            sblk_rect roi = { (uint16_t)rn(w / 2), (uint16_t)rn(h / 2), (uint16_t)(1 + rn(w / 2)), (uint16_t)(1 + rn(h / 2)) };
            uint32_t px = roi.x + rn(roi.w), py = roi.y + rn(roi.h);
            cur[py * stride + px * 4] ^= 0x80;
            cur[0] ^= (roi.x > 0 || roi.y > 0) ? 0x40 : 0;      /* a change outside the roi (if (0,0) is outside) */
            int n3 = sblk_dirty_rects(prev, cur, stride, w, h, &roi, SBLK_DIRTY_UPDATE_PREV, out2, 64);
            int cov = 0, inside = 1;
            for (int i = 0; i < n3; i++) {
                cov |= px >= out2[i].x && px < (uint32_t)out2[i].x + out2[i].w && py >= out2[i].y && py < (uint32_t)out2[i].y + out2[i].h;
                inside &= out2[i].x >= roi.x && out2[i].y >= roi.y && out2[i].x + out2[i].w <= roi.x + roi.w && out2[i].y + out2[i].h <= roi.y + roi.h;
            }
            CHECK(n3 == 1 && cov && inside, "roi: n=%d cov=%d inside=%d", n3, cov, inside);
            CHECK((prev[0] == cur[0]) == !(roi.x > 0 || roi.y > 0), "outside roi untouched");
        }
        free(prev); free(cur); free(orig);
    }

    /* full pipeline: source frames with edits -> dirty rects -> split -> build -> parser in random
     * chunks -> apply on a viewer frame buffer; after each frame the viewer equals the source */
    const uint32_t W = 1280, H = 720;
    size_t sz = (size_t)W * H * 4;
    uint8_t *src = malloc(sz), *shadow = malloc(sz), *view = malloc(sz), *scratch = malloc(sz);
    uint8_t *wire = malloc(32u << 20), *pbuf = malloc(SBLK_MAX_PAYLOAD);
    static uint8_t state[SBLK_LZ4_STATE_SIZE] __attribute__((aligned(8)));
    make_desktop(src, (int)W, (int)H, 5);
    memset(shadow, 0, sz); memset(view, 0, sz);
    sblk_parser p; sblk_parser_init(&p, pbuf, SBLK_MAX_PAYLOAD);
    size_t wire_total = 0; int frames_ok = 0; double t_src = 0;
    for (int f = 0; f < 40; f++) {
        if (f > 0) {   /* typing, a moving window, a solid fill */
            int k = (int)rn(4);
            if (k == 0) fill_rect(src, (size_t)W * 4, (int)rn(W - 8), (int)rn(H - 16), 7, 12, (uint32_t)rnd());
            else if (k == 1) make_desktop(src, (int)W, (int)H, 5 + (uint32_t)f);
            else if (k == 2) fill_rect(src, (size_t)W * 4, (int)rn(W / 2), (int)rn(H / 2), (int)rn(W / 2) + 1, (int)rn(H / 2) + 1, 0xFF0078D4);
        }
        double t0 = now_s();
        sblk_rect rects[64];
        int n = sblk_dirty_rects(shadow, src, (size_t)W * 4, W, H, NULL, SBLK_DIRTY_UPDATE_PREV, rects, 64);
        size_t wl = 0;
        sblk_msg m; memset(&m, 0, sizeof m);
        m.type = SBLK_T_FRAME_BEGIN; m.u.frame_begin.frame_id = (uint32_t)f + 1; m.u.frame_begin.width = W; m.u.frame_begin.height = H;
        int nr = 0; sblk_rect parts[256];
        for (int i = 0; i < n; i++) {
            int k = sblk_split_rect(&rects[i], SBLK_MAX_PAYLOAD, parts + nr, 256 - nr);
            CHECK(k > 0, "split"); if (k > 0) nr += k;
        }
        m.u.frame_begin.nrects = (uint16_t)nr;
        wl += (size_t)sblk_encode(&m, wire + wl, 64);
        for (int i = 0; i < nr; i++) {
            long b = sblk_build_rect(wire + wl, (32u << 20) - wl, src, (size_t)W * 4, &parts[i], SBLK_CAP_V1_ALL, 1, state, scratch, 0, NULL);
            CHECK(b > 0, "build"); if (b > 0) wl += (size_t)b;
        }
        memset(&m, 0, sizeof m); m.type = SBLK_T_FRAME_END; m.u.frame_end.frame_id = (uint32_t)f + 1;
        wl += (size_t)sblk_encode(&m, wire + wl, 64);
        t_src += now_s() - t0;
        wire_total += wl;
        /* viewer */
        size_t pos = 0; int ended = 0;
        while (pos < wl) {
            size_t c = 1 + rn(100000); if (c > wl - pos) c = wl - pos;
            size_t rem = c; const uint8_t *d = wire + pos;
            while (rem) {
                size_t used; sblk_msg vm;
                int r = sblk_parser_feed(&p, d, rem, &used, &vm);
                CHECK(r >= 0, "viewer parse %d", r); if (r < 0) break;
                d += used; rem -= used;
                if (r == 1 && vm.type == SBLK_T_RECT) CHECK(sblk_apply_rect(view, (size_t)W * 4, W, H, &vm.u.rect, scratch) == SBLK_OK, "viewer apply");
                if (r == 1 && vm.type == SBLK_T_FRAME_END) ended = 1;
            }
            pos += c;
        }
        frames_ok += ended && memcmp(view, src, sz) == 0;
    }
    CHECK(frames_ok == 40, "pipeline frames equal: %d/40", frames_ok);
    printf("dirty:  %d checks, %d failed (300 random frames, %d rects, %d overflow boxes; pipeline 40 frames 1280x720: "
           "%d exact, %.1f MB on the wire, source side %.1f ms/frame here)\n",
           checks - n0, failures - f0, total_rects, overflows, frames_ok, wire_total / 1e6, t_src * 1e3 / 40);
    free(src); free(shadow); free(view); free(scratch); free(wire); free(pbuf);
}

/* ---- fuzz ---- */

static long fz_msgs, fz_rects, fz_applied, fz_err[16];
static void fuzz_one(const uint8_t *buf, size_t n, uint8_t *pbuf, uint8_t *fb, uint8_t *scratch)
{
    sblk_parser p; sblk_parser_init(&p, pbuf, SBLK_MAX_PAYLOAD);
    size_t pos = 0;
    while (pos < n) {
        size_t c = 1 + rn(512); if (c > n - pos) c = n - pos;
        const uint8_t *d = buf + pos; size_t rem = c;
        while (rem) {
            size_t used; sblk_msg m;
            int r = sblk_parser_feed(&p, d, rem, &used, &m);
            if (r < 0) { fz_err[-r & 15]++; pos = n; break; }
            d += used; rem -= used;
            if (r == 1) {
                fz_msgs++;
                if (m.type == SBLK_T_RECT) { fz_rects++; fz_applied += sblk_apply_rect(fb, 256 * 4, 256, 256, &m.u.rect, scratch) == SBLK_OK; }
                long e = sblk_encode(&m, g_enc, sizeof g_enc);   /* anything decoded must re-encode */
                CHECK(e > 0 || m.u.unknown.len > sizeof g_enc, "decoded msg does not re-encode: %ld", e);
            }
        }
        if (pos < n) pos += c;
    }
    sblk_msg m;
    size_t off = 0;
    while (off < n) {   /* one-shot decoder walking the same bytes */
        long u = sblk_decode(buf + off, n - off, &m);
        if (u <= 0) break;
        off += (size_t)u;
    }
    (void)sblk_lz4_decompress(buf, (int)n, scratch, (int)rn(300000));
}

static void test_fuzz(void)
{
    int n0 = checks, f0 = failures;
    uint8_t *pbuf = malloc(SBLK_MAX_PAYLOAD), *fb = malloc(256 * 256 * 4), *scratch = malloc(1u << 20);
    uint8_t *buf = malloc(1 << 16);
    for (int i = 0; i < 10000; i++) {   /* pure random */
        size_t n = rn(1 << 16);
        for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)rnd();
        if (rn(2) && n >= 16) sblk_put_header(buf, (uint8_t)rnd(), (uint16_t)rnd(), rn(2) ? (uint32_t)rnd() : rn((uint32_t)n), 0);
        fuzz_one(buf, n, pbuf, fb, scratch);
    }
    for (int i = 0; i < 10000; i++) {   /* mutated slices of the valid stream: headers mostly valid */
        size_t start = g_off[rn(NSTREAM - 40)], n = rn(1 << 16);
        if (n > g_stream_len - start) n = g_stream_len - start;
        memcpy(buf, g_stream + start, n);
        int flips = 1 + (int)rn(8);
        for (int k = 0; k < flips && n; k++) buf[rn((uint32_t)n)] ^= (uint8_t)(1u << rn(8));
        fuzz_one(buf, n, pbuf, fb, scratch);
    }
    printf("fuzz:   %d checks, %d failed (10000 random + 10000 mutated buffers up to 64 KiB, parser + decode + apply + lz4)\n",
           checks - n0, failures - f0);
    printf("fuzz:   parser delivered %ld msgs (%ld RECT, %ld applied in a 256x256 fb); errors:", fz_msgs, fz_rects, fz_applied);
    for (int e = 1; e < 16; e++) if (fz_err[e]) printf(" %s=%ld", sblk_strerror(-e), fz_err[e]);
    printf("\n");
    free(pbuf); free(fb); free(scratch); free(buf);
}

int main(void)
{
    printf("SBLINK v1 core tests\n");
    test_wire();
    test_parser();
    test_lz4();
    test_dirty();
    test_fuzz();
    printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "PASSED", checks, failures);
    free(g_stream);
    return failures ? 1 : 0;
}
