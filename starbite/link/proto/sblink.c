/* SPDX-License-Identifier: GPL-2.0-only
 * SBLINK v1 portable core: see sblink.h and starbite/link/PROTOCOL.md.
 */
#include "sblink.h"

#include <string.h>

#include "lz4/lz4.h"

/* ---- little-endian helpers -------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

const char *sblk_strerror(int err)
{
    switch (err) {
    case SBLK_OK:             return "ok";
    case SBLK_E_ARG:          return "bad argument";
    case SBLK_E_SPACE:        return "output buffer too small";
    case SBLK_E_MAGIC:        return "bad magic";
    case SBLK_E_VERSION:      return "unsupported header version";
    case SBLK_E_TOO_BIG:      return "payload too big";
    case SBLK_E_SHORT:        return "payload too short";
    case SBLK_E_FIELD:        return "invalid field";
    case SBLK_E_UNKNOWN_TYPE: return "unknown must-understand type";
    case SBLK_E_LZ4:          return "corrupt LZ4 data";
    case SBLK_E_BOUNDS:       return "rect outside frame buffer";
    default:                  return "?";
    }
}

const char *sblk_type_name(int type)
{
    switch (type) {
    case SBLK_T_HELLO:         return "HELLO";
    case SBLK_T_MODE:          return "MODE";
    case SBLK_T_FRAME_BEGIN:   return "FRAME_BEGIN";
    case SBLK_T_RECT:          return "RECT";
    case SBLK_T_FRAME_END:     return "FRAME_END";
    case SBLK_T_ACK:           return "ACK";
    case SBLK_T_INPUT_KEY:     return "INPUT_KEY";
    case SBLK_T_INPUT_POINTER: return "INPUT_POINTER";
    case SBLK_T_INPUT_TEXT:    return "INPUT_TEXT";
    case SBLK_T_PING:          return "PING";
    case SBLK_T_PONG:          return "PONG";
    case SBLK_T_BYE:           return "BYE";
    default:                   return "?";
    }
}

int sblk_type_known(int type) { return sblk_type_name(type)[0] != '?'; }

/* ---- validation shared by encoder and decoder ------------------------------------------------ */

static uint32_t rect_raw_bytes(const sblk_rect *r) { return (uint32_t)r->w * r->h * 4u; }

static int check_rect(const sblk_rect_msg *rm)
{
    if (rm->r.w == 0 || rm->r.h == 0) return SBLK_E_FIELD;
    if ((uint32_t)rm->r.x + rm->r.w > 0xFFFFu || (uint32_t)rm->r.y + rm->r.h > 0xFFFFu) return SBLK_E_FIELD;
    uint64_t raw = (uint64_t)rm->r.w * rm->r.h * 4u;
    switch (rm->encoding) {
    case SBLK_ENC_RAW:   if (rm->data_len != raw) return SBLK_E_FIELD; break;
    case SBLK_ENC_LZ4:   if (rm->data_len == 0 || raw > SBLK_MAX_PAYLOAD * 256ull) return SBLK_E_FIELD; break;
    case SBLK_ENC_SOLID: if (rm->data_len != 4) return SBLK_E_FIELD; break;
    default:             return SBLK_E_FIELD;
    }
    if (rm->data_len > SBLK_MAX_PAYLOAD - SBLK_RECT_HEAD) return SBLK_E_TOO_BIG;
    return SBLK_OK;
}

static int check_hello(const sblk_hello *h)
{
    if (h->role != SBLK_ROLE_SOURCE && h->role != SBLK_ROLE_VIEWER) return SBLK_E_FIELD;
    if (h->max_payload > SBLK_MAX_PAYLOAD) return SBLK_E_FIELD;
    return SBLK_OK;
}

/* payload size of m, or < 0 */
static long payload_size(const sblk_msg *m)
{
    int r;
    switch (m->type) {
    case SBLK_T_HELLO:
        if ((r = check_hello(&m->u.hello)) < 0) return r;
        return SBLK_HELLO_SIZE;
    case SBLK_T_MODE:
        if (m->u.mode.status > SBLK_MODE_REJECTED) return SBLK_E_FIELD;
        return SBLK_MODE_SIZE;
    case SBLK_T_FRAME_BEGIN:   return SBLK_FRAME_BEGIN_SIZE;
    case SBLK_T_RECT:
        if ((r = check_rect(&m->u.rect)) < 0) return r;
        if (!m->u.rect.data) return SBLK_E_ARG;
        return SBLK_RECT_HEAD + (long)m->u.rect.data_len;
    case SBLK_T_FRAME_END:     return SBLK_FRAME_END_SIZE;
    case SBLK_T_ACK:           return SBLK_ACK_SIZE;
    case SBLK_T_INPUT_KEY:
        if (m->u.key.down > 1) return SBLK_E_FIELD;
        return SBLK_INPUT_KEY_SIZE;
    case SBLK_T_INPUT_POINTER: return SBLK_INPUT_POINTER_SIZE;
    case SBLK_T_INPUT_TEXT:
        if (m->u.text.len == 0 || m->u.text.len > SBLK_TEXT_MAX) return SBLK_E_FIELD;
        if (!m->u.text.utf8) return SBLK_E_ARG;
        return (long)m->u.text.len;
    case SBLK_T_PING:          return SBLK_PING_SIZE;
    case SBLK_T_PONG:          return SBLK_PONG_SIZE;
    case SBLK_T_BYE:
        if (m->u.bye.text.len > SBLK_BYE_TEXT_MAX) return SBLK_E_FIELD;
        if (m->u.bye.text.len && !m->u.bye.text.utf8) return SBLK_E_ARG;
        return SBLK_BYE_SIZE + (long)m->u.bye.text.len;
    default:
        /* unknown types can be sent raw (tests, future extensions) */
        if (m->u.unknown.len > SBLK_MAX_PAYLOAD) return SBLK_E_TOO_BIG;
        if (m->u.unknown.len && !m->u.unknown.data) return SBLK_E_ARG;
        return (long)m->u.unknown.len;
    }
}

/* ---- encode ------------------------------------------------------------------------------------ */

void sblk_put_header(uint8_t out[SBLK_HDR_SIZE], uint8_t type, uint16_t flags, uint32_t len, uint32_t seq)
{
    put32(out, SBLK_MAGIC);
    out[4] = SBLK_VERSION;
    out[5] = type;
    put16(out + 6, flags);
    put32(out + 8, len);
    put32(out + 12, seq);
}

long sblk_encoded_size(const sblk_msg *m)
{
    if (!m) return SBLK_E_ARG;
    long n = payload_size(m);
    return n < 0 ? n : (long)SBLK_HDR_SIZE + n;
}

long sblk_encode(const sblk_msg *m, uint8_t *out, size_t cap)
{
    if (!m || !out) return SBLK_E_ARG;
    long n = payload_size(m);
    if (n < 0) return n;
    if ((size_t)n + SBLK_HDR_SIZE > cap) return SBLK_E_SPACE;
    sblk_put_header(out, m->type, m->flags, (uint32_t)n, m->seq);
    uint8_t *p = out + SBLK_HDR_SIZE;
    memset(p, 0, (size_t)n < 64 ? (size_t)n : 64);   /* reserved bytes = 0 */

    switch (m->type) {
    case SBLK_T_HELLO: {
        const sblk_hello *h = &m->u.hello;
        p[0] = h->role; p[1] = h->proto_major; p[2] = h->proto_minor; p[3] = 0;
        put32(p + 4, h->caps);
        put16(p + 8, h->width); put16(p + 10, h->height);
        put32(p + 12, h->refresh_mhz);
        put32(p + 16, h->pixel_format);
        put32(p + 20, h->max_payload);
        memcpy(p + 24, h->name, 32);
        break;
    }
    case SBLK_T_MODE:
        put16(p, m->u.mode.width); put16(p + 2, m->u.mode.height);
        put32(p + 4, m->u.mode.refresh_mhz);
        p[8] = m->u.mode.status;
        break;
    case SBLK_T_FRAME_BEGIN:
        put32(p, m->u.frame_begin.frame_id);
        put16(p + 4, m->u.frame_begin.width); put16(p + 6, m->u.frame_begin.height);
        put16(p + 8, m->u.frame_begin.nrects); put16(p + 10, m->u.frame_begin.fflags);
        break;
    case SBLK_T_RECT: {
        const sblk_rect_msg *rm = &m->u.rect;
        put16(p, rm->r.x); put16(p + 2, rm->r.y); put16(p + 4, rm->r.w); put16(p + 6, rm->r.h);
        p[8] = rm->encoding; p[9] = p[10] = p[11] = 0;
        memcpy(p + SBLK_RECT_HEAD, rm->data, rm->data_len);
        break;
    }
    case SBLK_T_FRAME_END:
        put32(p, m->u.frame_end.frame_id); put32(p + 4, 0);
        put64(p + 8, m->u.frame_end.capture_us);
        break;
    case SBLK_T_ACK:
        put32(p, m->u.ack.frame_id); put32(p + 4, m->u.ack.present_us);
        break;
    case SBLK_T_INPUT_KEY:
        put16(p, m->u.key.usage_page); put16(p + 2, m->u.key.usage);
        p[4] = m->u.key.down; p[5] = 0;
        put16(p + 6, m->u.key.modifiers);
        break;
    case SBLK_T_INPUT_POINTER:
        put32(p, (uint32_t)m->u.pointer.x); put32(p + 4, (uint32_t)m->u.pointer.y);
        put32(p + 8, m->u.pointer.buttons);
        put16(p + 12, (uint16_t)m->u.pointer.wheel_dx); put16(p + 14, (uint16_t)m->u.pointer.wheel_dy);
        break;
    case SBLK_T_INPUT_TEXT:
        memcpy(p, m->u.text.utf8, m->u.text.len);
        break;
    case SBLK_T_PING:
        put32(p, m->u.ping.id); put32(p + 4, 0); put64(p + 8, m->u.ping.t_us);
        break;
    case SBLK_T_PONG:
        put32(p, m->u.pong.id); put32(p + 4, 0);
        put64(p + 8, m->u.pong.echo_us); put64(p + 16, m->u.pong.t_us);
        break;
    case SBLK_T_BYE:
        put32(p, m->u.bye.reason);
        if (m->u.bye.text.len) memcpy(p + 4, m->u.bye.text.utf8, m->u.bye.text.len);
        break;
    default:
        if (m->u.unknown.len) memcpy(p, m->u.unknown.data, m->u.unknown.len);
        break;
    }
    return (long)SBLK_HDR_SIZE + n;
}

int sblk_encode_rect_head(uint8_t out[SBLK_RECT_MSG_HEAD], const sblk_rect *r, uint8_t encoding,
                          uint32_t data_len, uint32_t seq)
{
    if (!out || !r) return SBLK_E_ARG;
    sblk_rect_msg rm = { *r, encoding, data_len, NULL };
    int e = check_rect(&rm);
    if (e < 0) return e;
    sblk_put_header(out, SBLK_T_RECT, 0, SBLK_RECT_HEAD + data_len, seq);
    uint8_t *p = out + SBLK_HDR_SIZE;
    put16(p, r->x); put16(p + 2, r->y); put16(p + 4, r->w); put16(p + 6, r->h);
    p[8] = encoding; p[9] = p[10] = p[11] = 0;
    return (int)SBLK_RECT_MSG_HEAD;
}

/* ---- decode ------------------------------------------------------------------------------------ */

int sblk_decode_header(const uint8_t in[SBLK_HDR_SIZE], uint32_t max_payload, sblk_hdr *h)
{
    if (!in || !h) return SBLK_E_ARG;
    h->magic = get32(in);
    h->version = in[4];
    h->type = in[5];
    h->flags = get16(in + 6);
    h->len = get32(in + 8);
    h->seq = get32(in + 12);
    if (h->magic != SBLK_MAGIC) return SBLK_E_MAGIC;
    if (h->version != SBLK_VERSION) return SBLK_E_VERSION;
    if (max_payload > SBLK_MAX_PAYLOAD) max_payload = SBLK_MAX_PAYLOAD;
    if (h->len > max_payload) return SBLK_E_TOO_BIG;
    return SBLK_OK;
}

/* fixed part of each known type; trailing bytes beyond it are ignored (forward compatibility) */
static uint32_t fixed_size(int type)
{
    switch (type) {
    case SBLK_T_HELLO:         return SBLK_HELLO_SIZE;
    case SBLK_T_MODE:          return SBLK_MODE_SIZE;
    case SBLK_T_FRAME_BEGIN:   return SBLK_FRAME_BEGIN_SIZE;
    case SBLK_T_RECT:          return SBLK_RECT_HEAD;
    case SBLK_T_FRAME_END:     return SBLK_FRAME_END_SIZE;
    case SBLK_T_ACK:           return SBLK_ACK_SIZE;
    case SBLK_T_INPUT_KEY:     return SBLK_INPUT_KEY_SIZE;
    case SBLK_T_INPUT_POINTER: return SBLK_INPUT_POINTER_SIZE;
    case SBLK_T_INPUT_TEXT:    return 1;
    case SBLK_T_PING:          return SBLK_PING_SIZE;
    case SBLK_T_PONG:          return SBLK_PONG_SIZE;
    case SBLK_T_BYE:           return SBLK_BYE_SIZE;
    default:                   return 0;
    }
}

int sblk_decode_payload(const sblk_hdr *h, const uint8_t *p, sblk_msg *m)
{
    if (!h || !m || (h->len && !p)) return SBLK_E_ARG;
    memset(m, 0, sizeof *m);
    m->type = h->type;
    m->flags = h->flags;
    m->seq = h->seq;
    if (!sblk_type_known(h->type)) {
        if (h->flags & SBLK_F_MUST_UNDERSTAND) return SBLK_E_UNKNOWN_TYPE;
        m->u.unknown.len = h->len;
        m->u.unknown.data = p;
        return SBLK_OK;
    }
    if (h->len < fixed_size(h->type)) return SBLK_E_SHORT;

    switch (h->type) {
    case SBLK_T_HELLO: {
        sblk_hello *o = &m->u.hello;
        o->role = p[0]; o->proto_major = p[1]; o->proto_minor = p[2];
        o->caps = get32(p + 4);
        o->width = get16(p + 8); o->height = get16(p + 10);
        o->refresh_mhz = get32(p + 12);
        o->pixel_format = get32(p + 16);
        o->max_payload = get32(p + 20);
        memcpy(o->name, p + 24, 32);
        return check_hello(o);
    }
    case SBLK_T_MODE:
        m->u.mode.width = get16(p); m->u.mode.height = get16(p + 2);
        m->u.mode.refresh_mhz = get32(p + 4);
        m->u.mode.status = p[8];
        return m->u.mode.status > SBLK_MODE_REJECTED ? SBLK_E_FIELD : SBLK_OK;
    case SBLK_T_FRAME_BEGIN:
        m->u.frame_begin.frame_id = get32(p);
        m->u.frame_begin.width = get16(p + 4); m->u.frame_begin.height = get16(p + 6);
        m->u.frame_begin.nrects = get16(p + 8); m->u.frame_begin.fflags = get16(p + 10);
        return SBLK_OK;
    case SBLK_T_RECT: {
        sblk_rect_msg *o = &m->u.rect;
        o->r.x = get16(p); o->r.y = get16(p + 2); o->r.w = get16(p + 4); o->r.h = get16(p + 6);
        o->encoding = p[8];
        o->data_len = h->len - SBLK_RECT_HEAD;
        o->data = p + SBLK_RECT_HEAD;
        return check_rect(o);
    }
    case SBLK_T_FRAME_END:
        m->u.frame_end.frame_id = get32(p);
        m->u.frame_end.capture_us = get64(p + 8);
        return SBLK_OK;
    case SBLK_T_ACK:
        m->u.ack.frame_id = get32(p); m->u.ack.present_us = get32(p + 4);
        return SBLK_OK;
    case SBLK_T_INPUT_KEY:
        m->u.key.usage_page = get16(p); m->u.key.usage = get16(p + 2);
        m->u.key.down = p[4];
        m->u.key.modifiers = get16(p + 6);
        return m->u.key.down > 1 ? SBLK_E_FIELD : SBLK_OK;
    case SBLK_T_INPUT_POINTER:
        m->u.pointer.x = (int32_t)get32(p); m->u.pointer.y = (int32_t)get32(p + 4);
        m->u.pointer.buttons = get32(p + 8);
        m->u.pointer.wheel_dx = (int16_t)get16(p + 12); m->u.pointer.wheel_dy = (int16_t)get16(p + 14);
        return SBLK_OK;
    case SBLK_T_INPUT_TEXT:
        if (h->len > SBLK_TEXT_MAX) return SBLK_E_FIELD;
        m->u.text.len = h->len; m->u.text.utf8 = p;
        return SBLK_OK;
    case SBLK_T_PING:
        m->u.ping.id = get32(p); m->u.ping.t_us = get64(p + 8);
        return SBLK_OK;
    case SBLK_T_PONG:
        m->u.pong.id = get32(p); m->u.pong.echo_us = get64(p + 8); m->u.pong.t_us = get64(p + 16);
        return SBLK_OK;
    case SBLK_T_BYE:
        m->u.bye.reason = get32(p);
        m->u.bye.text.len = h->len - SBLK_BYE_SIZE;
        m->u.bye.text.utf8 = m->u.bye.text.len ? p + SBLK_BYE_SIZE : NULL;
        return m->u.bye.text.len > SBLK_BYE_TEXT_MAX ? SBLK_E_FIELD : SBLK_OK;
    }
    return SBLK_E_ARG;   /* not reached */
}

long sblk_decode(const uint8_t *in, size_t len, sblk_msg *m)
{
    if (!in || !m) return SBLK_E_ARG;
    if (len < SBLK_HDR_SIZE) return 0;
    sblk_hdr h;
    int e = sblk_decode_header(in, SBLK_MAX_PAYLOAD, &h);
    if (e < 0) return e;
    if (len - SBLK_HDR_SIZE < h.len) return 0;
    e = sblk_decode_payload(&h, in + SBLK_HDR_SIZE, m);
    if (e < 0) return e;
    return (long)SBLK_HDR_SIZE + (long)h.len;
}

/* ---- stream parser ----------------------------------------------------------------------------- */

void sblk_parser_init(sblk_parser *p, uint8_t *buf, uint32_t cap)
{
    memset(p, 0, sizeof *p);
    p->buf = buf;
    p->cap = buf ? cap : 0;
    p->max_payload = p->cap < SBLK_MAX_PAYLOAD ? p->cap : SBLK_MAX_PAYLOAD;
}

void sblk_parser_reset(sblk_parser *p)
{
    p->hn = 0; p->have_hdr = 0; p->pn = 0; p->err = 0;
}

int sblk_parser_feed(sblk_parser *p, const uint8_t *data, size_t len, size_t *used, sblk_msg *m)
{
    size_t off = 0;
    *used = 0;
    if (p->err) return p->err;
    if (len && !data) return SBLK_E_ARG;

    if (!p->have_hdr) {
        const uint8_t *hp;
        if (p->hn == 0 && len >= SBLK_HDR_SIZE) {
            hp = data;
            off = SBLK_HDR_SIZE;
        } else {
            size_t take = SBLK_HDR_SIZE - p->hn;
            if (take > len) take = len;
            memcpy(p->hb + p->hn, data, take);
            p->hn += (uint32_t)take;
            off = take;
            /* fail early on garbage: the magic is known after 4 bytes */
            if (p->hn >= 4 && get32(p->hb) != SBLK_MAGIC) { *used = off; return p->err = SBLK_E_MAGIC; }
            if (p->hn < SBLK_HDR_SIZE) { *used = off; p->bytes += off; return 0; }
            hp = p->hb;
        }
        int e = sblk_decode_header(hp, p->max_payload, &p->h);
        if (e < 0) { *used = off; return p->err = e; }
        p->have_hdr = 1;
        p->pn = 0;
    }

    const uint8_t *payload;
    if (p->pn == 0 && len - off >= p->h.len) {
        payload = data + off;               /* whole payload in this chunk: no copy */
        off += p->h.len;
    } else {
        size_t take = p->h.len - p->pn;
        if (take > len - off) take = len - off;
        if (take) memcpy(p->buf + p->pn, data + off, take);
        p->pn += (uint32_t)take;
        off += take;
        if (p->pn < p->h.len) { *used = off; p->bytes += off; return 0; }
        payload = p->buf;
    }
    p->have_hdr = 0;
    p->hn = 0;
    p->pn = 0;
    *used = off;
    p->bytes += off;
    int e = sblk_decode_payload(&p->h, payload, m);
    if (e < 0) return p->err = e;
    p->msgs++;
    return 1;
}

/* ---- LZ4 ----------------------------------------------------------------------------------------- */

int sblk_lz4_bound(int n) { return LZ4_compressBound(n); }

int sblk_lz4_compress(const uint8_t *src, int n, uint8_t *dst, int cap, int accel, void *state)
{
    if (!src || !dst || n < 0 || cap < 0) return 0;
    if (state && (size_t)LZ4_sizeofState() <= SBLK_LZ4_STATE_SIZE && ((uintptr_t)state & 7) == 0)
        return LZ4_compress_fast_extState(state, (const char *)src, (char *)dst, n, cap, accel);
    return LZ4_compress_fast((const char *)src, (char *)dst, n, cap, accel);
}

int sblk_lz4_decompress(const uint8_t *src, int n, uint8_t *dst, int dst_size)
{
    if (!src || !dst || n <= 0 || dst_size < 0) return SBLK_E_LZ4;
    int r = LZ4_decompress_safe((const char *)src, (char *)dst, n, dst_size);
    return r == dst_size ? SBLK_OK : SBLK_E_LZ4;
}

/* ---- pixels ------------------------------------------------------------------------------------- */

int sblk_solid_color(const uint8_t *px, size_t stride, uint32_t w, uint32_t h, uint32_t *bgra)
{
    if (!px || w == 0 || h == 0) return 0;
    uint32_t c;
    memcpy(&c, px, 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * stride;
        uint32_t x = 0;
        /* 8 bytes at a time: two pixels */
        uint64_t cc = ((uint64_t)c << 32) | c;
        for (; x + 2 <= w; x += 2) {
            uint64_t v;
            memcpy(&v, row + (size_t)x * 4, 8);
            if (v != cc) return 0;
        }
        for (; x < w; x++) {
            uint32_t v;
            memcpy(&v, row + (size_t)x * 4, 4);
            if (v != c) return 0;
        }
    }
    if (bgra) *bgra = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
    return 1;
}

#define MAX_TILES_X ((0xFFFFu + SBLK_TILE - 1) / SBLK_TILE)   /* 1024 */

int sblk_dirty_rects(uint8_t *prev, const uint8_t *cur, size_t stride, uint32_t width, uint32_t height,
                     const sblk_rect *roi, unsigned flags, sblk_rect *out, int max_out)
{
    if (!prev || !cur || !out || max_out <= 0) return SBLK_E_ARG;
    if (width == 0 || height == 0 || width > 0xFFFFu || height > 0xFFFFu || stride < (size_t)width * 4)
        return SBLK_E_ARG;

    uint32_t x0 = 0, y0 = 0, x1 = width, y1 = height;   /* compared area [x0,x1) x [y0,y1) */
    if (roi) {
        x0 = roi->x; y0 = roi->y;
        x1 = (uint32_t)roi->x + roi->w; y1 = (uint32_t)roi->y + roi->h;
        if (x1 > width) x1 = width;
        if (y1 > height) y1 = height;
        if (x0 >= x1 || y0 >= y1) return 0;
    }

    uint8_t dirty[MAX_TILES_X];
    int open_prev[MAX_TILES_X / 2 + 1], open_cur[MAX_TILES_X / 2 + 1];
    int nprev = 0, n = 0, overflow = 0;
    uint32_t bx0 = 0xFFFFFFFFu, by0 = 0xFFFFFFFFu, bx1 = 0, by1 = 0;   /* bounding box of changes */
    uint32_t tx0 = x0 / SBLK_TILE, tx1 = (x1 + SBLK_TILE - 1) / SBLK_TILE;

    for (uint32_t ty = y0 / SBLK_TILE; ty * SBLK_TILE < y1; ty++) {
        uint32_t ry0 = ty * SBLK_TILE, ry1 = ry0 + SBLK_TILE;
        if (ry0 < y0) ry0 = y0;
        if (ry1 > y1) ry1 = y1;

        for (uint32_t tx = tx0; tx < tx1; tx++) {
            uint32_t cx0 = tx * SBLK_TILE, cx1 = cx0 + SBLK_TILE;
            if (cx0 < x0) cx0 = x0;
            if (cx1 > x1) cx1 = x1;
            size_t nb = (size_t)(cx1 - cx0) * 4;
            int d = 0;
            for (uint32_t y = ry0; y < ry1; y++) {
                size_t off = (size_t)y * stride + (size_t)cx0 * 4;
                if (!d) {
                    if (memcmp(prev + off, cur + off, nb) == 0) continue;
                    d = 1;
                    if (!(flags & SBLK_DIRTY_UPDATE_PREV)) break;
                }
                memcpy(prev + off, cur + off, nb);   /* rows from the first difference on */
            }
            dirty[tx - tx0] = (uint8_t)d;
        }

        /* runs of dirty tiles in this tile row */
        int ncur = 0;
        for (uint32_t tx = tx0; tx < tx1;) {
            if (!dirty[tx - tx0]) { tx++; continue; }
            uint32_t s = tx;
            while (tx < tx1 && dirty[tx - tx0]) tx++;
            uint32_t rx0 = s * SBLK_TILE, rx1 = tx * SBLK_TILE;
            if (rx0 < x0) rx0 = x0;
            if (rx1 > x1) rx1 = x1;
            if (rx0 < bx0) bx0 = rx0;
            if (rx1 > bx1) bx1 = rx1;
            if (ry0 < by0) by0 = ry0;
            if (ry1 > by1) by1 = ry1;
            if (overflow) continue;
            int hit = -1;
            for (int i = 0; i < nprev; i++) {
                sblk_rect *o = &out[open_prev[i]];
                if (o->x == rx0 && o->w == rx1 - rx0 && (uint32_t)o->y + o->h == ry0) { hit = open_prev[i]; break; }
            }
            if (hit >= 0) {
                out[hit].h = (uint16_t)(ry1 - out[hit].y);
            } else {
                if (n == max_out) { overflow = 1; continue; }
                out[n].x = (uint16_t)rx0; out[n].y = (uint16_t)ry0;
                out[n].w = (uint16_t)(rx1 - rx0); out[n].h = (uint16_t)(ry1 - ry0);
                hit = n++;
            }
            open_cur[ncur++] = hit;
        }
        memcpy(open_prev, open_cur, (size_t)ncur * sizeof open_cur[0]);
        nprev = ncur;
    }

    if (overflow) {
        out[0].x = (uint16_t)bx0; out[0].y = (uint16_t)by0;
        out[0].w = (uint16_t)(bx1 - bx0); out[0].h = (uint16_t)(by1 - by0);
        return 1;
    }
    return n;
}

int sblk_split_rect(const sblk_rect *r, uint32_t max_payload, sblk_rect *out, int max_out)
{
    if (!r || !out || r->w == 0 || r->h == 0) return SBLK_E_ARG;
    if (max_payload > SBLK_MAX_PAYLOAD) max_payload = SBLK_MAX_PAYLOAD;
    uint32_t row = (uint32_t)r->w * 4;
    if (max_payload < SBLK_RECT_HEAD + row) return SBLK_E_ARG;
    uint32_t rows = (max_payload - SBLK_RECT_HEAD) / row;
    int n = 0;
    for (uint32_t y = 0; y < r->h; y += rows) {
        if (n == max_out) return SBLK_E_SPACE;
        out[n].x = r->x; out[n].y = (uint16_t)(r->y + y); out[n].w = r->w;
        out[n].h = (uint16_t)(r->h - y < rows ? r->h - y : rows);
        n++;
    }
    return n;
}

long sblk_build_rect(uint8_t *out, size_t cap, const uint8_t *frame, size_t stride, const sblk_rect *r,
                     uint32_t caps, int lz4_accel, void *lz4_state, uint8_t *scratch, uint32_t seq,
                     uint8_t *encoding)
{
    if (!out || !frame || !r || r->w == 0 || r->h == 0) return SBLK_E_ARG;
    /* 64-bit: w*h*4 wraps a u32 (32768 x 32768 -> 0) and would pass the size check */
    if ((uint64_t)r->w * r->h * 4u > SBLK_MAX_PAYLOAD - SBLK_RECT_HEAD) return SBLK_E_TOO_BIG;
    uint32_t raw = rect_raw_bytes(r);
    const uint8_t *src = frame + (size_t)r->y * stride + (size_t)r->x * 4;
    uint8_t *data = out + SBLK_RECT_MSG_HEAD;
    uint8_t enc;
    uint32_t dlen;

    if (cap < SBLK_RECT_MSG_HEAD + 4) return SBLK_E_SPACE;
    uint32_t color;
    if ((caps & SBLK_CAP_ENC_SOLID) && sblk_solid_color(src, stride, r->w, r->h, &color)) {
        enc = SBLK_ENC_SOLID;
        put32(data, color);
        dlen = 4;
    } else {
        if (cap < SBLK_RECT_MSG_HEAD + (size_t)raw) return SBLK_E_SPACE;
        int done = 0;
        if ((caps & SBLK_CAP_ENC_LZ4) && scratch) {
            for (uint32_t y = 0; y < r->h; y++)
                memcpy(scratch + (size_t)y * r->w * 4, src + (size_t)y * stride, (size_t)r->w * 4);
            int lim = (int)(raw - raw / 8);
            int c = sblk_lz4_compress(scratch, (int)raw, data, lim, lz4_accel, lz4_state);
            if (c > 0) { enc = SBLK_ENC_LZ4; dlen = (uint32_t)c; done = 1; }
            else {
                memcpy(data, scratch, raw);
                enc = SBLK_ENC_RAW; dlen = raw; done = 1;
            }
        }
        if (!done) {
            for (uint32_t y = 0; y < r->h; y++)
                memcpy(data + (size_t)y * r->w * 4, src + (size_t)y * stride, (size_t)r->w * 4);
            enc = SBLK_ENC_RAW; dlen = raw;
        }
    }
    int e = sblk_encode_rect_head(out, r, enc, dlen, seq);
    if (e < 0) return e;
    if (encoding) *encoding = enc;
    return (long)SBLK_RECT_MSG_HEAD + dlen;
}

int sblk_apply_rect(uint8_t *fb, size_t stride, uint32_t fb_w, uint32_t fb_h, const sblk_rect_msg *rm,
                    uint8_t *scratch)
{
    if (!fb || !rm || !rm->data || stride < (size_t)fb_w * 4) return SBLK_E_ARG;
    int e = check_rect(rm);
    if (e < 0) return e;
    const sblk_rect *r = &rm->r;
    if ((uint32_t)r->x + r->w > fb_w || (uint32_t)r->y + r->h > fb_h) return SBLK_E_BOUNDS;
    uint8_t *dst = fb + (size_t)r->y * stride + (size_t)r->x * 4;
    size_t row = (size_t)r->w * 4;
    uint32_t raw = rect_raw_bytes(r);

    switch (rm->encoding) {
    case SBLK_ENC_RAW:
        for (uint32_t y = 0; y < r->h; y++) memcpy(dst + y * stride, rm->data + y * row, row);
        return SBLK_OK;
    case SBLK_ENC_SOLID:
        for (uint32_t x = 0; x < r->w; x++) memcpy(dst + (size_t)x * 4, rm->data, 4);
        for (uint32_t y = 1; y < r->h; y++) memcpy(dst + y * stride, dst, row);
        return SBLK_OK;
    case SBLK_ENC_LZ4:
        if (row == stride || r->h == 1)   /* contiguous in the frame buffer: decompress in place */
            return sblk_lz4_decompress(rm->data, (int)rm->data_len, dst, (int)raw);
        if (!scratch) return SBLK_E_ARG;
        if ((e = sblk_lz4_decompress(rm->data, (int)rm->data_len, scratch, (int)raw)) < 0) return e;
        for (uint32_t y = 0; y < r->h; y++) memcpy(dst + y * stride, scratch + y * row, row);
        return SBLK_OK;
    }
    return SBLK_E_FIELD;
}
