/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link, Mac viewer: the SBLINK v1 viewer session. See sbv.h.
 */
#include "sbv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEWER_CAPS (SBLK_CAP_ENC_RAW | SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID | SBLK_CAP_MODE | \
                     SBLK_CAP_INPUT_KEY | SBLK_CAP_INPUT_PTR | SBLK_CAP_INPUT_TEXT)

const char *sbv_strerror(int e)
{
    switch (e) {
    case SBV_E_CLOSED:   return "no session";
    case SBV_E_PEER_BYE: return "peer sent BYE";
    case SBV_E_DEAD:     return "link dead (5 s without a message)";
    case SBV_E_SEND:     return "send failed";
    case SBV_E_NOMEM:    return "out of memory";
    case SBV_E_PROTO:    return "protocol error (session)";
    }
    return sblk_strerror(e);
}

static void evf(sbv *s, int ev, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (ev == SBV_EV_ERROR) snprintf(s->err_text, sizeof s->err_text, "%s", line);
    if (s->ev.event) s->ev.event(s->ev.ctx, ev, line);
}

static uint64_t now(sbv *s) { return s->ev.now_us(s->ev.ctx); }

/* encodes m with the next sequence number and sends it as one transfer */
static int send_msg(sbv *s, sblk_msg *m)
{
    uint8_t buf[SBLK_HDR_SIZE + SBLK_TEXT_MAX + 64];
    m->seq = s->seq;
    long n = sblk_encode(m, buf, sizeof buf);
    if (n < 0) return (int)n;
    s->seq++;
    if (s->ev.send(s->ev.ctx, buf, (size_t)n) < 0) return SBV_E_SEND;
    s->tx_bytes += (uint64_t)n;
    return 0;
}

int sbv_init(sbv *s, const sbv_events *ev, const char *name, uint16_t pref_w, uint16_t pref_h,
             uint32_t refresh_mhz)
{
    memset(s, 0, sizeof *s);
    s->ev = *ev;
    s->pbuf = malloc(SBLK_MAX_PAYLOAD);
    if (!s->pbuf) return SBV_E_NOMEM;
    sblk_parser_init(&s->parser, s->pbuf, SBLK_MAX_PAYLOAD);
    s->mine.role = SBLK_ROLE_VIEWER;
    s->mine.proto_major = SBLK_PROTO_MAJOR;
    s->mine.proto_minor = SBLK_PROTO_MINOR;
    s->mine.caps = VIEWER_CAPS;
    s->mine.width = pref_w;
    s->mine.height = pref_h;
    s->mine.refresh_mhz = refresh_mhz;
    s->mine.pixel_format = SBLK_PIXFMT_BGRA8888;
    s->mine.max_payload = SBLK_MAX_PAYLOAD;
    /* cut on a UTF-8 character boundary, NUL padded */
    size_t n = name ? strlen(name) : 0;
    if (n > 31) {
        n = 31;
        while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(s->mine.name, name, n);
    s->want_w = pref_w;
    s->want_h = pref_h;
    s->state = SBV_IDLE;
    return 0;
}

void sbv_free(sbv *s)
{
    free(s->pbuf);
    free(s->fb);
    free(s->scratch);
    memset(s, 0, sizeof *s);
}

static void drop_session(sbv *s)
{
    sblk_parser_reset(&s->parser);
    s->seq = 0;
    s->caps = 0;
    memset(&s->peer, 0, sizeof s->peer);
    s->in_frame = 0;
    s->fb_valid = 0;
    s->key_asked = 0;
    s->asked_w = s->asked_h = 0;
    s->have_mode = 0;
    s->have_off = 0;
    s->mods &= (uint16_t)(SBLK_MOD_CAPS | SBLK_MOD_NUM);
    memset(s->down, 0, sizeof s->down);
    s->buttons = 0;
}

int sbv_start(sbv *s)
{
    drop_session(s);
    s->state = SBV_HELLO_SENT;
    s->last_rx_us = s->last_ping_us = now(s);
    s->sessions++;
    s->err_text[0] = 0;
    s->bye_reason = 0;
    s->bye_text[0] = 0;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello = s->mine;
    if (s->want_w && s->want_h) {
        m.u.hello.width = s->want_w;
        m.u.hello.height = s->want_h;
    }
    int e = send_msg(s, &m);
    if (e < 0) { s->state = SBV_CLOSED; evf(s, SBV_EV_ERROR, "HELLO: %s", sbv_strerror(e)); }
    return e;
}

int sbv_running(const sbv *s) { return s->state == SBV_RUNNING; }

int sbv_bye(sbv *s, uint32_t reason, const char *text)
{
    int e = 0;
    if (s->state == SBV_HELLO_SENT || s->state == SBV_RUNNING) {
        sblk_msg m;
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_BYE;
        m.u.bye.reason = reason;
        if (text && *text) {
            size_t n = strlen(text);
            if (n > SBLK_BYE_TEXT_MAX) n = SBLK_BYE_TEXT_MAX;
            m.u.bye.text.len = (uint32_t)n;
            m.u.bye.text.utf8 = (const uint8_t *)text;
        }
        e = send_msg(s, &m);
    }
    s->state = SBV_CLOSED;
    s->in_frame = 0;
    return e;
}

/* our side found an error: BYE(reason) best effort, close */
static int fail(sbv *s, uint32_t reason, int err, const char *what)
{
    evf(s, SBV_EV_ERROR, "%s: %s", what, sbv_strerror(err));
    sbv_bye(s, reason, what);
    return err < 0 ? err : SBV_E_PROTO;
}

static int ensure_fb(sbv *s, uint32_t w, uint32_t h)
{
    if (w == s->fb_w && h == s->fb_h && s->fb) return 0;
    size_t need = (size_t)w * h * 4;
    if (need > s->fb_cap) {
        uint8_t *fb = realloc(s->fb, need), *sc;
        if (!fb) return SBV_E_NOMEM;
        s->fb = fb;
        sc = realloc(s->scratch, need);
        if (!sc) return SBV_E_NOMEM;
        s->scratch = sc;
        s->fb_cap = need;
    }
    s->fb_w = w;
    s->fb_h = h;
    /* contents undefined until a keyframe: show black (A = 0xFF) */
    uint32_t *px = (uint32_t *)(void *)s->fb;
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] = 0xFF000000u;
    s->fb_valid = 0;
    s->resized = 1;
    return 0;
}

static void add_damage(sbv *s, const sblk_rect *r)
{
    if (s->ndamage < SBV_MAX_DAMAGE) { s->damage[s->ndamage++] = *r; return; }
    /* too many: collapse everything into one bounding box */
    sblk_rect *b = &s->damage[0];
    uint32_t x0 = b->x, y0 = b->y, x1 = (uint32_t)b->x + b->w, y1 = (uint32_t)b->y + b->h;
    for (int i = 1; i < s->ndamage; i++) {
        const sblk_rect *d = &s->damage[i];
        if (d->x < x0) x0 = d->x;
        if (d->y < y0) y0 = d->y;
        if ((uint32_t)d->x + d->w > x1) x1 = (uint32_t)d->x + d->w;
        if ((uint32_t)d->y + d->h > y1) y1 = (uint32_t)d->y + d->h;
    }
    if (r->x < x0) x0 = r->x;
    if (r->y < y0) y0 = r->y;
    if ((uint32_t)r->x + r->w > x1) x1 = (uint32_t)r->x + r->w;
    if ((uint32_t)r->y + r->h > y1) y1 = (uint32_t)r->y + r->h;
    b->x = (uint16_t)x0; b->y = (uint16_t)y0; b->w = (uint16_t)(x1 - x0); b->h = (uint16_t)(y1 - y0);
    s->ndamage = 1;
}

static int send_ack(sbv *s, uint32_t id, uint32_t present_us)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_ACK;
    m.u.ack.frame_id = id;
    m.u.ack.present_us = present_us;
    return send_msg(s, &m);
}

int sbv_ack_keyframe(sbv *s)
{
    if (s->state != SBV_RUNNING) return 0;
    s->key_asked = 1;
    return send_ack(s, 0, 0);
}

static void maybe_send_mode(sbv *s)
{
    if (s->state != SBV_RUNNING || !(s->caps & SBLK_CAP_MODE) || !s->want_w || !s->want_h) return;
    if (s->want_w == s->asked_w && s->want_h == s->asked_h) return;
    uint16_t cur_w = s->have_mode ? s->last_mode.width : s->peer.width;
    uint16_t cur_h = s->have_mode ? s->last_mode.height : s->peer.height;
    s->asked_w = s->want_w;
    s->asked_h = s->want_h;
    if (s->want_w == cur_w && s->want_h == cur_h) return;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_MODE;
    m.u.mode.width = s->want_w;
    m.u.mode.height = s->want_h;
    m.u.mode.refresh_mhz = s->mine.refresh_mhz;
    m.u.mode.status = SBLK_MODE_REQUEST;
    if (send_msg(s, &m) == 0) evf(s, SBV_EV_LOG, "MODE request %ux%u", s->want_w, s->want_h);
}

int sbv_request_mode(sbv *s, uint16_t w, uint16_t h, uint32_t refresh_mhz)
{
    (void)refresh_mhz;
    s->want_w = w;
    s->want_h = h;
    maybe_send_mode(s);
    return 0;
}

int sbv_ping(sbv *s)
{
    if (s->state != SBV_RUNNING) return 0;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_PING;
    m.u.ping.id = ++s->ping_id;
    m.u.ping.t_us = now(s);
    s->last_ping_us = m.u.ping.t_us;
    return send_msg(s, &m);
}

static int handle(sbv *s, const sblk_msg *m)
{
    uint64_t t = now(s);
    s->last_rx_us = t;

    if (s->state == SBV_HELLO_SENT && m->type != SBLK_T_HELLO && m->type != SBLK_T_BYE &&
        sblk_type_known(m->type))
        return fail(s, SBLK_BYE_PROTOCOL, SBV_E_PROTO, "message before HELLO");

    switch (m->type) {
    case SBLK_T_HELLO: {
        const sblk_hello *h = &m->u.hello;
        if (h->role != SBLK_ROLE_SOURCE) return fail(s, SBLK_BYE_PROTOCOL, SBLK_E_FIELD, "HELLO role");
        if (h->proto_major != SBLK_PROTO_MAJOR) {
            evf(s, SBV_EV_ERROR, "source speaks protocol %u.%u", h->proto_major, h->proto_minor);
            sbv_bye(s, SBLK_BYE_VERSION, "protocol major");
            return SBV_E_PROTO;
        }
        if (h->pixel_format != SBLK_PIXFMT_BGRA8888)
            return fail(s, SBLK_BYE_VERSION, SBLK_E_FIELD, "pixel format");
        /* The source answers our HELLO; a second one is the answer to a restart (we never answer it,
         * PROTOCOL.md section 4: the viewer speaks first). Either way: fresh session state. */
        int was_running = s->state == SBV_RUNNING;
        s->peer = *h;
        s->caps = s->mine.caps & h->caps;
        s->state = SBV_RUNNING;
        s->in_frame = 0;
        s->fb_valid = 0;
        s->key_asked = 0;
        s->asked_w = s->asked_h = 0;
        s->have_mode = 0;
        char name[33];
        memcpy(name, h->name, 32);
        name[32] = 0;
        evf(s, SBV_EV_HELLO, "%sHELLO from \"%s\": %ux%u @ %u.%03u Hz, protocol %u.%u, caps 0x%02x%s",
            was_running ? "re-" : "", name, h->width, h->height, h->refresh_mhz / 1000,
            h->refresh_mhz % 1000, h->proto_major, h->proto_minor, h->caps,
            (h->caps & SBLK_CAP_EXTEND) ? " (extended display)" : " (mirror)");
        maybe_send_mode(s);
        return sbv_ping(s);
    }
    case SBLK_T_MODE:
        if (m->u.mode.status == SBLK_MODE_REQUEST)
            return fail(s, SBLK_BYE_PROTOCOL, SBLK_E_FIELD, "MODE REQUEST from the source");
        s->last_mode = m->u.mode;
        s->have_mode = 1;
        evf(s, SBV_EV_MODE, "MODE %s %ux%u", m->u.mode.status == SBLK_MODE_APPLIED ? "APPLIED" : "REJECTED",
            m->u.mode.width, m->u.mode.height);
        return 0;

    case SBLK_T_FRAME_BEGIN: {
        const sblk_frame_begin *b = &m->u.frame_begin;
        if (s->in_frame) return fail(s, SBLK_BYE_PROTOCOL, SBV_E_PROTO, "FRAME_BEGIN inside a frame");
        if (!b->width || !b->height || b->width > SBV_MAX_DIM || b->height > SBV_MAX_DIM || !b->frame_id)
            return fail(s, SBLK_BYE_PROTOCOL, SBLK_E_FIELD, "FRAME_BEGIN fields");
        s->resized = 0;
        int e = ensure_fb(s, b->width, b->height);
        if (e < 0) return fail(s, SBLK_BYE_SHUTDOWN, e, "frame buffer");
        s->fbeg = *b;
        s->in_frame = 1;
        s->rects_left = b->nrects;
        s->frame_bytes = 0;
        s->ndamage = 0;
        s->t_begin = t;
        return 0;
    }
    case SBLK_T_RECT: {
        const sblk_rect_msg *rm = &m->u.rect;
        if (!s->in_frame || !s->rects_left)
            return fail(s, SBLK_BYE_PROTOCOL, SBV_E_PROTO, "RECT outside a frame");
        int e = sblk_apply_rect(s->fb, (size_t)s->fb_w * 4, s->fb_w, s->fb_h, rm, s->scratch);
        if (e < 0) return fail(s, SBLK_BYE_PROTOCOL, e, "RECT");
        s->rects_left--;
        s->rects++;
        s->frame_bytes += rm->data_len + SBLK_RECT_HEAD;
        if (rm->encoding < 3) { s->enc_count[rm->encoding]++; s->enc_bytes[rm->encoding] += rm->data_len; }
        add_damage(s, &rm->r);
        return 0;
    }
    case SBLK_T_FRAME_END: {
        const sblk_frame_end *fe = &m->u.frame_end;
        if (!s->in_frame || s->rects_left || fe->frame_id != s->fbeg.frame_id)
            return fail(s, SBLK_BYE_PROTOCOL, SBV_E_PROTO, "FRAME_END does not match FRAME_BEGIN");
        s->in_frame = 0;
        int key = (s->fbeg.fflags & SBLK_FB_KEYFRAME) != 0;
        if (key) { s->fb_valid = 1; s->key_asked = 0; }
        s->frames++;
        sbv_frame_info fi;
        memset(&fi, 0, sizeof fi);
        fi.frame_id = fe->frame_id;
        fi.width = s->fb_w;
        fi.height = s->fb_h;
        fi.stride = (size_t)s->fb_w * 4;
        fi.fb = s->fb;
        fi.damage = s->damage;
        fi.ndamage = s->ndamage;
        fi.keyframe = key;
        fi.resized = s->resized;
        fi.valid = s->fb_valid;
        fi.nrects = s->fbeg.nrects;
        fi.bytes = s->frame_bytes;
        fi.capture_us = fe->capture_us;
        s->resized = 0;
        if (s->ev.frame) s->ev.frame(s->ev.ctx, &fi);
        uint64_t t2 = now(s);
        if (s->have_off) {
            int64_t cap_local = (int64_t)fe->capture_us - s->clock_off_us;
            s->lag_us = (int64_t)t2 > cap_local ? (uint64_t)((int64_t)t2 - cap_local) : 0;
        }
        uint64_t pu = t2 - s->t_begin;
        int e = send_ack(s, fe->frame_id, pu > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)pu);
        if (e < 0) return e;
        /* a delta frame on a buffer that never got a keyframe: ask for one (once) */
        if (!s->fb_valid && !s->key_asked) return sbv_ack_keyframe(s);
        return 0;
    }
    case SBLK_T_PING: {
        sblk_msg r;
        memset(&r, 0, sizeof r);
        r.type = SBLK_T_PONG;
        r.u.pong.id = m->u.ping.id;
        r.u.pong.echo_us = m->u.ping.t_us;
        r.u.pong.t_us = t;
        return send_msg(s, &r);
    }
    case SBLK_T_PONG:
        if (m->u.pong.id == s->ping_id && t >= m->u.pong.echo_us) {
            s->rtt_us = t - m->u.pong.echo_us;
            s->clock_off_us = (int64_t)m->u.pong.t_us - (int64_t)(m->u.pong.echo_us + s->rtt_us / 2);
            s->have_off = 1;
            s->pongs++;
            evf(s, SBV_EV_PONG, "PONG %u rtt %llu us", m->u.pong.id, (unsigned long long)s->rtt_us);
        }
        return 0;
    case SBLK_T_BYE: {
        size_t n = m->u.bye.text.len;
        if (n > SBLK_BYE_TEXT_MAX) n = SBLK_BYE_TEXT_MAX;
        if (n) memcpy(s->bye_text, m->u.bye.text.utf8, n);
        s->bye_text[n] = 0;
        s->bye_reason = m->u.bye.reason;
        s->state = SBV_CLOSED;
        evf(s, SBV_EV_BYE, "BYE from the source: reason %u%s%s", s->bye_reason, n ? ": " : "", s->bye_text);
        return SBV_E_PEER_BYE;
    }
    default:
        /* viewer-bound traffic only; anything else of the source's (ACK/INPUT: wrong direction, unknown
         * types without MUST_UNDERSTAND) is skipped */
        return 0;
    }
}

int sbv_feed(sbv *s, const uint8_t *p, size_t n)
{
    if (s->state != SBV_HELLO_SENT && s->state != SBV_RUNNING) return SBV_E_CLOSED;
    s->rx_bytes += n;
    while (n) {
        size_t used = 0;
        sblk_msg m;
        int r = sblk_parser_feed(&s->parser, p, n, &used, &m);
        p += used;
        n -= used;
        if (r < 0) return fail(s, SBLK_BYE_PROTOCOL, r, "stream");
        if (r == 0) break;
        int e = handle(s, &m);
        if (e < 0) return e;
        if (s->state == SBV_CLOSED) return SBV_E_CLOSED;
    }
    return 0;
}

int sbv_tick(sbv *s)
{
    if (s->state != SBV_HELLO_SENT && s->state != SBV_RUNNING) return 0;
    uint64_t t = now(s);
    if (t - s->last_rx_us >= SBV_DEAD_US) {
        evf(s, SBV_EV_ERROR, "%s", sbv_strerror(SBV_E_DEAD));
        s->state = SBV_CLOSED;
        return SBV_E_DEAD;
    }
    if (s->state == SBV_RUNNING && t - s->last_ping_us >= SBV_PING_US) return sbv_ping(s);
    return 0;
}

/* ---- input ---------------------------------------------------------------------------------------- */

static int send_key(sbv *s, uint16_t page, uint16_t usage, int down)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_KEY;
    m.u.key.usage_page = page;
    m.u.key.usage = usage;
    m.u.key.down = down ? 1 : 0;
    m.u.key.modifiers = s->mods;
    return send_msg(s, &m);
}

int sbv_key(sbv *s, uint16_t page, uint16_t usage, int down, uint16_t locks)
{
    if (s->state != SBV_RUNNING || !(s->caps & SBLK_CAP_INPUT_KEY)) return 0;
    s->mods = (uint16_t)((s->mods & 0xFFu) | (locks & (SBLK_MOD_CAPS | SBLK_MOD_NUM)));
    if (page == SBLK_HID_PAGE_KEYBOARD && usage < 256) {
        if (down) s->down[usage >> 3] |= (uint8_t)(1u << (usage & 7));
        else s->down[usage >> 3] &= (uint8_t)~(1u << (usage & 7));
        if (usage >= 0xE0 && usage <= 0xE7) {
            uint16_t bit = (uint16_t)(1u << (usage - 0xE0));
            if (down) s->mods |= bit; else s->mods &= (uint16_t)~bit;
        }
    }
    return send_key(s, page, usage, down);
}

int sbv_pointer(sbv *s, int32_t x, int32_t y, uint32_t buttons, int16_t wdx, int16_t wdy)
{
    if (s->state != SBV_RUNNING || !(s->caps & SBLK_CAP_INPUT_PTR)) return 0;
    s->px = x;
    s->py = y;
    s->buttons = buttons;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_POINTER;
    m.u.pointer.x = x;
    m.u.pointer.y = y;
    m.u.pointer.buttons = buttons;
    m.u.pointer.wheel_dx = wdx;
    m.u.pointer.wheel_dy = wdy;
    return send_msg(s, &m);
}

int sbv_text(sbv *s, const char *utf8, size_t len)
{
    if (s->state != SBV_RUNNING || !(s->caps & SBLK_CAP_INPUT_TEXT) || !len) return 0;
    while (len) {
        size_t n = len > SBLK_TEXT_MAX ? SBLK_TEXT_MAX : len;
        /* never split a UTF-8 sequence */
        if (n < len) while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80) n--;
        if (!n) return SBLK_E_ARG;
        sblk_msg m;
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_INPUT_TEXT;
        m.u.text.len = (uint32_t)n;
        m.u.text.utf8 = (const uint8_t *)utf8;
        int e = send_msg(s, &m);
        if (e < 0) return e;
        utf8 += n;
        len -= n;
    }
    return 0;
}

int sbv_release_all(sbv *s, uint16_t locks)
{
    if (s->state != SBV_RUNNING) return 0;
    int e = 0;
    /* ordinary keys first, modifiers last, so no shortcut fires on the way up */
    for (int pass = 0; pass < 2 && e == 0; pass++) {
        /* Windows opens the Start menu (Windows key) or the menu bar (Alt) when that key goes up with no other
         * key pressed since it went down. Focus loss on the Mac with Cmd held (Cmd+Tab, Cmd+Space, the
         * Ctrl+Option+Cmd release) must not do that: press Left Ctrl first, as Windows' own menu mask key
         * does; it goes up below with the other modifiers (0xE0 before 0xE2/0xE3). */
        if (pass == 1 && (s->mods & (SBLK_MOD_LGUI | SBLK_MOD_RGUI | SBLK_MOD_LALT | SBLK_MOD_RALT)))
            e = sbv_key(s, SBLK_HID_PAGE_KEYBOARD, 0xE0, 1, locks);
        for (unsigned u = 0; u < 256 && e == 0; u++) {
            int is_mod = u >= 0xE0 && u <= 0xE7;
            if (is_mod != pass) continue;
            if (s->down[u >> 3] & (1u << (u & 7))) e = sbv_key(s, SBLK_HID_PAGE_KEYBOARD, (uint16_t)u, 0, locks);
        }
    }
    if (e == 0 && s->buttons) e = sbv_pointer(s, s->px, s->py, 0, 0, 0);
    return e;
}

uint64_t sbv_fb_hash(const uint8_t *fb, size_t stride, uint32_t w, uint32_t h)
{
    uint64_t x = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = fb + (size_t)y * stride;
        for (size_t i = 0; i < (size_t)w * 4; i++) { x ^= row[i]; x *= 0x100000001b3ull; }
    }
    return x;
}
