/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: the SBLINK v1 source session (PROTOCOL.md sections 3-6) and the capture/send loop.
 *
 * Threads: each transport has a receive thread that calls sbl_rx() (input is injected right there, so
 * input latency never waits for capture or encoding); the capture loop (sbl_sender_run, the main
 * thread) captures, encodes and sends frames. Locks: txlk is held for every send so a frame goes out
 * as one batch with consecutive sequence numbers and nothing in between (frames are encoded before
 * txlk is taken, and renumbered if a PONG went out meanwhile); lk guards the session state.
 * Order: txlk before lk, never the other way round.
 */
#include "sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sbl_cfg_default(sbl_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->tcp_port = 0;
    snprintf(c->tcp_bind, sizeof c->tcp_bind, "127.0.0.1");
    c->fps = 30;
    c->gdi_fps = 10;
    c->dead_ms = 5000;
    c->stall_ms = 3000;
    c->keep_awake = 1;
    c->ping_ms = 1000;
    c->lz4_accel = 1;
    c->usb_chunk = 64 * 1024;    /* matches the Mac's 64 KiB reads; UFX completed 256 KiB transfers short on the S9 */
    snprintf(c->name, sizeof c->name, "Starbite S9");
}

void sbl_link_init(sbl_link *l, const char *name, int is_usb)
{
    snprintf(l->name, sizeof l->name, "%s", name);
    l->is_usb = is_usb;
    sblk_parser_init(&l->parser, l->pbuf, SBL_RXBUF);
    l->hunting = is_usb;
}

int sbl_sender_init(sbl_sender *s, const sbl_cfg *cfg)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    plat_mutex_init(&s->txlk);
    plat_mutex_init(&s->lk);
    s->wake = plat_event_new();
    sbl_flow_reset(&s->flow);
    s->st_t0 = plat_now_us();
    s->our_caps = SBLK_CAP_ENC_RAW | SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID | SBLK_CAP_INPUT_KEY |
                  SBLK_CAP_INPUT_PTR | SBLK_CAP_INPUT_TEXT;
    return s->wake ? 0 : -1;
}

void sbl_sender_stop(sbl_sender *s)
{
    s->stop = 1;
    plat_event_set(s->wake);
}

/* ---- sending (txlk held) -------------------------------------------------------------------------- */

/* every send goes through here (txlk held): the watchdog sees a send that never returns */
static int link_send_tx(sbl_sender *s, sbl_link *l, const uint8_t *p, size_t n)
{
    s->send_since_us = plat_now_us();
    int r = l->send(l, p, n);
    s->send_since_us = 0;
    return r;
}

static int send_msg_tx(sbl_sender *s, sbl_link *l, sblk_msg *m, uint32_t seq)
{
    uint8_t b[SBLK_HDR_SIZE + 512];
    m->seq = seq;
    long n = sblk_encode(m, b, sizeof b);
    if (n < 0) { sbl_log("encode %s: %s", sblk_type_name(m->type), sblk_strerror((int)n)); return (int)n; }
    int r = link_send_tx(s, l, b, (size_t)n);
    if (r == 0) {
        plat_mutex_lock(&s->lk);
        s->last_tx_us = plat_now_us();
        plat_mutex_unlock(&s->lk);
    }
    return r;
}

/* a message of the current session on its link (txlk held) */
static int send_session_tx(sbl_sender *s, sblk_msg *m)
{
    plat_mutex_lock(&s->lk);
    sbl_link *l = s->active ? s->owner : NULL;
    uint32_t seq = s->tx_seq++;
    plat_mutex_unlock(&s->lk);
    return l ? send_msg_tx(s, l, m, seq) : -1;
}

static void send_bye_tx(sbl_sender *s, sbl_link *l, uint32_t reason, const char *text, uint32_t seq)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_BYE;
    m.u.bye.reason = reason;
    m.u.bye.text.len = text ? (uint32_t)strlen(text) : 0;
    if (m.u.bye.text.len > SBLK_BYE_TEXT_MAX) m.u.bye.text.len = SBLK_BYE_TEXT_MAX;
    m.u.bye.text.utf8 = (const uint8_t *)text;
    send_msg_tx(s, l, &m, seq);
}

/* rewrites the sequence numbers of an encoded batch of messages to start at seq */
static void renumber(uint8_t *p, size_t n, uint32_t seq)
{
    size_t off = 0;
    while (n - off >= SBLK_HDR_SIZE) {
        uint8_t *h = p + off;
        uint32_t len = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
        h[12] = (uint8_t)seq; h[13] = (uint8_t)(seq >> 8); h[14] = (uint8_t)(seq >> 16); h[15] = (uint8_t)(seq >> 24);
        seq++;
        if (len > n - off - SBLK_HDR_SIZE) break;
        off += SBLK_HDR_SIZE + len;
    }
}

/* ---- session state (lk held) ---------------------------------------------------------------------- */

static void release_input_locked(sbl_sender *s)
{
    sbl_ev ev[SBL_MAX_EV_RELEASE];
    int n = sbl_in_release_all(&s->in, ev, SBL_MAX_EV_RELEASE);
    if (n) sbl_inject(ev, n);
}

static void end_session_locked(sbl_sender *s, const char *why)
{
    if (s->active) {
        uint64_t secs = (plat_now_us() - s->st_t0) / 1000000u;
        sbl_log("session end (%s): %llu frames, %llu KiB, %llu keyframes, %llu s", why,
                (unsigned long long)s->st_frames, (unsigned long long)(s->st_bytes >> 10),
                (unsigned long long)s->st_keys, (unsigned long long)secs);
    }
    release_input_locked(s);
    s->active = 0;
    s->gen++;
    sbl_flow_restart(&s->flow);
    s->mode_req = 0;
    s->mode_pending = 0;
    s->pong_pending = 0;
    plat_event_set(s->wake);
}

/* ---- transport entry points ------------------------------------------------------------------------ */

int sbl_link_begin(sbl_sender *s, sbl_link *l)
{
    plat_mutex_lock(&s->txlk);
    plat_mutex_lock(&s->lk);
    int busy = s->owner != NULL;
    if (!busy) {
        s->owner = l;
        s->last_rx_us = plat_now_us();   /* the dead timer also covers "connected, never said HELLO" */
    }
    plat_mutex_unlock(&s->lk);
    if (busy) {
        sbl_log("%s: busy (viewer on %s), BYE(BUSY)", l->name, s->owner ? s->owner->name : "?");
        send_bye_tx(s, l, SBLK_BYE_BUSY, "another viewer is connected", 0);
    } else sbl_log("%s: connected", l->name);
    plat_mutex_unlock(&s->txlk);
    return busy ? -1 : 0;
}

void sbl_link_end(sbl_sender *s, sbl_link *l, const char *why)
{
    plat_mutex_lock(&s->txlk);
    plat_mutex_lock(&s->lk);
    if (s->owner == l) {
        end_session_locked(s, why);
        s->owner = NULL;
        sbl_log("%s: gone (%s)", l->name, why);
    }
    sblk_parser_reset(&l->parser);
    l->hunting = l->is_usb;
    plat_mutex_unlock(&s->lk);
    plat_mutex_unlock(&s->txlk);
}

/* HELLO from the viewer: (re)start the session. txlk held. Returns < 0 to close. */
static int on_hello(sbl_sender *s, sbl_link *l, const sblk_hello *h)
{
    if (h->role != SBLK_ROLE_VIEWER) {
        send_bye_tx(s, l, SBLK_BYE_PROTOCOL, "HELLO role is not viewer", 0);
        return -1;
    }
    if (h->proto_major != SBLK_PROTO_MAJOR) {
        sbl_log("%s: protocol %u.%u not supported", l->name, h->proto_major, h->proto_minor);
        send_bye_tx(s, l, SBLK_BYE_VERSION, "protocol major 1 only", 0);
        return -1;
    }
    plat_mutex_lock(&s->lk);
    if (s->owner && s->owner != l) {
        plat_mutex_unlock(&s->lk);
        sbl_log("%s: HELLO while %s has the session: BYE(BUSY)", l->name, s->owner->name);
        send_bye_tx(s, l, SBLK_BYE_BUSY, "another viewer is connected", 0);
        return 1;   /* not an error for this link (USB: keep listening) */
    }
    uint32_t w = s->disp_w;
    uint32_t minpay = SBLK_RECT_HEAD + (w ? w : 1) * 4u;
    if (h->max_payload < minpay || h->pixel_format != SBLK_PIXFMT_BGRA8888) {
        plat_mutex_unlock(&s->lk);
        sbl_log("%s: HELLO max_payload %u / pixel format %u unusable", l->name, h->max_payload, h->pixel_format);
        send_bye_tx(s, l, SBLK_BYE_PROTOCOL, "max_payload too small or pixel format not BGRA8888", 0);
        return -1;
    }
    if (s->active) end_session_locked(s, "new HELLO");
    s->owner = l;
    /* USB: whoever ended the last session may have set hunting meanwhile (the capture loop on a failed
     * send while this HELLO waited for txlk): this transfer started the new session, keep reading */
    l->hunting = 0;
    s->active = 1;
    s->gen++;
    s->peer = *h;
    s->caps = (s->our_caps & h->caps) | SBLK_CAP_ENC_RAW;
    s->tx_seq = 0;
    sbl_flow_restart(&s->flow);
    s->pong_pending = 0;
    s->need_key = 1;
    s->mode_req = 0;
    s->mode_pending = 0;
    s->last_rx_us = plat_now_us();
    s->st_frames = s->st_bytes = s->st_enc_us = s->st_rects = s->st_blocked = s->st_keys = 0;
    s->st_stalls = s->st_ack_bad = 0;
    s->st_t0 = plat_now_us();
    char name[33];
    memcpy(name, h->name, 32);
    name[32] = 0;
    sbl_log("%s: HELLO from \"%s\" v%u.%u caps 0x%x (common 0x%x) wants %ux%u max_payload %u", l->name, name,
            h->proto_major, h->proto_minor, h->caps, s->caps, h->width, h->height, h->max_payload);

    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_SOURCE;
    m.u.hello.proto_major = SBLK_PROTO_MAJOR;
    m.u.hello.proto_minor = SBLK_PROTO_MINOR;
    m.u.hello.caps = s->our_caps;
    m.u.hello.width = (uint16_t)s->disp_w;
    m.u.hello.height = (uint16_t)s->disp_h;
    s->hello_w = s->disp_w;
    s->hello_h = s->disp_h;
    m.u.hello.refresh_mhz = s->disp_refresh;
    m.u.hello.pixel_format = SBLK_PIXFMT_BGRA8888;
    m.u.hello.max_payload = SBL_RXBUF;
    snprintf(m.u.hello.name, sizeof m.u.hello.name, "%s", s->cfg.name);
    uint32_t seq = s->tx_seq++;
    plat_mutex_unlock(&s->lk);
    plat_event_set(s->wake);
    return send_msg_tx(s, l, &m, seq) < 0 ? -1 : 0;
}

/* one message from the owner (or a would-be owner) link; txlk not held. < 0 = protocol error / close */
static int on_msg(sbl_sender *s, sbl_link *l, const sblk_msg *m)
{
    sbl_ev ev[2 * 2 * SBLK_TEXT_MAX + 8];
    int n, r;

    if (m->type == SBLK_T_HELLO) {
        plat_mutex_lock(&s->txlk);
        r = on_hello(s, l, &m->u.hello);
        plat_mutex_unlock(&s->txlk);
        return r < 0 ? -2 : 0;   /* on_hello already sent its BYE */
    }

    plat_mutex_lock(&s->lk);
    int mine = s->active && s->owner == l;
    if (mine) s->last_rx_us = plat_now_us();
    plat_mutex_unlock(&s->lk);
    if (m->type == SBLK_T_BYE) {
        char t[SBLK_BYE_TEXT_MAX + 1];
        uint32_t tl = m->u.bye.text.len;
        if (tl) memcpy(t, m->u.bye.text.utf8, tl);
        t[tl] = 0;
        sbl_log("%s: BYE(%u) \"%s\"", l->name, m->u.bye.reason, t);
        return -2;
    }
    if (!mine) {
        if (l->is_usb && s->owner && s->owner != l) return 0;   /* someone else's session: ignore */
        sbl_log("%s: %s before HELLO", l->name, sblk_type_name(m->type));
        return -1;
    }

    switch (m->type) {
    case SBLK_T_ACK:
        plat_mutex_lock(&s->lk);
        if (m->u.ack.frame_id == 0) {
            s->need_key = 1;
        } else {
            r = sbl_flow_ack(&s->flow, m->u.ack.frame_id);
            /* r == 0: an old or repeated ACK (an earlier session's included: ids go on across sessions) */
            if (r < 0 && s->st_ack_bad++ < 5)
                sbl_log("%s: ACK(%u) ignored: not in flight (last sent %u, %d in flight)", l->name,
                        m->u.ack.frame_id, s->flow.sent_any ? s->flow.last_sent : 0, s->flow.n);
        }
        plat_mutex_unlock(&s->lk);
        plat_event_set(s->wake);
        return 0;

    case SBLK_T_INPUT_KEY:
        if (!(s->caps & SBLK_CAP_INPUT_KEY)) return 0;
        plat_mutex_lock(&s->lk);
        n = sbl_in_key(&s->in, &m->u.key, ev, SBL_MAX_EV_KEY);
        if (n) sbl_inject(ev, n);
        plat_mutex_unlock(&s->lk);
        if (s->cfg.verbose)
            sbl_log("key page %x usage %x %s mods %x -> %d events", m->u.key.usage_page, m->u.key.usage,
                    m->u.key.down ? "down" : "up", m->u.key.modifiers, n);
        return 0;

    case SBLK_T_INPUT_POINTER:
        if (!(s->caps & SBLK_CAP_INPUT_PTR)) return 0;
        plat_mutex_lock(&s->lk);
        sbl_inject_geom(&s->geom);
        n = sbl_in_pointer(&s->in, &m->u.pointer, &s->geom, ev, SBL_MAX_EV_PTR);
        if (n) sbl_inject(ev, n);
        plat_mutex_unlock(&s->lk);
        return 0;

    case SBLK_T_INPUT_TEXT:
        if (!(s->caps & SBLK_CAP_INPUT_TEXT)) return 0;
        n = sbl_in_text(m->u.text.utf8, m->u.text.len, ev, (int)(sizeof ev / sizeof ev[0]));
        plat_mutex_lock(&s->lk);
        if (n) sbl_inject(ev, n);
        plat_mutex_unlock(&s->lk);
        return 0;

    case SBLK_T_MODE:
        if (m->u.mode.status != SBLK_MODE_REQUEST) {
            sbl_log("%s: MODE status %u from the viewer", l->name, m->u.mode.status);
            return -1;
        }
        plat_mutex_lock(&s->lk);
        s->mode_req = 1;
        s->mode_want = m->u.mode;
        plat_mutex_unlock(&s->lk);
        plat_event_set(s->wake);
        return 0;

    case SBLK_T_PING: {
        sblk_msg p;
        memset(&p, 0, sizeof p);
        p.type = SBLK_T_PONG;
        p.u.pong.id = m->u.ping.id;
        p.u.pong.echo_us = m->u.ping.t_us;
        p.u.pong.t_us = plat_now_us();
        /* never wait for txlk here: the capture loop may be inside a long send (USB: up to 5 s when the
         * Mac stopped reading) and this thread must keep reading OUT, above all for a new HELLO */
        if (!plat_mutex_trylock(&s->txlk)) {
            plat_mutex_lock(&s->lk);
            s->pong_q = p.u.pong;
            s->pong_pending = 1;
            plat_mutex_unlock(&s->lk);
            plat_event_set(s->wake);
            return 0;
        }
        r = send_session_tx(s, &p);
        plat_mutex_unlock(&s->txlk);
        return r < 0 ? -3 : 0;
    }

    case SBLK_T_PONG:
        if (s->cfg.verbose)
            sbl_log("%s: PONG %u rtt %llu us", l->name, m->u.pong.id,
                    (unsigned long long)(plat_now_us() - m->u.pong.echo_us));
        return 0;

    case SBLK_T_FRAME_BEGIN: case SBLK_T_RECT: case SBLK_T_FRAME_END:
        sbl_log("%s: %s from the viewer", l->name, sblk_type_name(m->type));
        return -1;

    default:
        return 0;   /* unknown type without MUST_UNDERSTAND: skipped */
    }
}

static int usb_starts_hello(const uint8_t *p, size_t n)
{
    return n >= SBLK_HDR_SIZE && p[0] == 'S' && p[1] == 'B' && p[2] == 'L' && p[3] == 'K' &&
           p[4] == SBLK_VERSION && p[5] == SBLK_T_HELLO;
}

int sbl_rx(sbl_sender *s, sbl_link *l, const uint8_t *p, size_t n, int transfer_start)
{
    if (l->hunting) {
        if (!(transfer_start && usb_starts_hello(p, n))) return 0;
        l->hunting = 0;
        sblk_parser_reset(&l->parser);
    } else if (l->is_usb && transfer_start && usb_starts_hello(p, n)) {
        /* PROTOCOL.md 6.3: a HELLO at the start of an OUT transfer starts a new session even if the
         * parser holds the cut-off tail of an earlier one (Mac app killed mid-transfer); without this
         * the HELLO would be swallowed as that message's payload */
        sblk_parser_reset(&l->parser);
    }
    while (n) {
        size_t used = 0;
        sblk_msg m;
        int r = sblk_parser_feed(&l->parser, p, n, &used, &m);
        p += used;
        n -= used;
        if (r == 0) break;
        if (r < 0) {
            sbl_log("%s: protocol error: %s", l->name, sblk_strerror(r));
            plat_mutex_lock(&s->txlk);
            plat_mutex_lock(&s->lk);
            int mine = s->owner == l;
            uint32_t seq = mine && s->active ? s->tx_seq++ : 0;
            plat_mutex_unlock(&s->lk);
            if (mine || !s->owner) send_bye_tx(s, l, SBLK_BYE_PROTOCOL, sblk_strerror(r), seq);
            plat_mutex_lock(&s->lk);
            if (mine) {
                end_session_locked(s, "protocol error");
                if (l->is_usb) s->owner = NULL;
            }
            plat_mutex_unlock(&s->lk);
            plat_mutex_unlock(&s->txlk);
            sblk_parser_reset(&l->parser);
            if (l->is_usb) { l->hunting = 1; return 0; }
            return -1;
        }
        r = on_msg(s, l, &m);
        if (r < 0) {
            plat_mutex_lock(&s->txlk);
            plat_mutex_lock(&s->lk);
            int mine = s->owner == l;
            if (r == -1 && (mine || !s->owner)) {
                uint32_t seq = s->active && mine ? s->tx_seq++ : 0;
                plat_mutex_unlock(&s->lk);
                send_bye_tx(s, l, SBLK_BYE_PROTOCOL, "unexpected message", seq);
                plat_mutex_lock(&s->lk);
            }
            if (mine) {
                end_session_locked(s, r == -2 ? "BYE / HELLO refused" : r == -3 ? "send failed" : "protocol error");
                if (l->is_usb) s->owner = NULL;
            }
            plat_mutex_unlock(&s->lk);
            plat_mutex_unlock(&s->txlk);
            sblk_parser_reset(&l->parser);
            if (l->is_usb) { l->hunting = 1; return 0; }
            return -1;
        }
    }
    return 0;
}

/* BYE to the viewer and end the session (Windows shutdown, logoff, sleep, quit). */
void sbl_sender_bye(sbl_sender *s, uint32_t reason, const char *text)
{
    plat_mutex_lock(&s->txlk);
    plat_mutex_lock(&s->lk);
    sbl_link *l = s->active ? s->owner : NULL;
    uint32_t seq = s->tx_seq++;
    plat_mutex_unlock(&s->lk);
    if (l) send_bye_tx(s, l, reason, text, seq);
    plat_mutex_lock(&s->lk);
    if (l) {
        end_session_locked(s, text);
        if (l->is_usb) { s->owner = NULL; l->hunting = 1; }
    }
    plat_mutex_unlock(&s->lk);
    if (l && !l->is_usb) l->close(l);
    plat_mutex_unlock(&s->txlk);
}

/* ---- watchdog ------------------------------------------------------------------------------------- */

void sbl_watch_link(sbl_sender *s, sbl_link *l)
{
    plat_mutex_lock(&s->lk);
    l->watch_rx = 1;
    l->rx_beat_us = plat_now_us();
    if (s->nwatch < (int)(sizeof s->watch / sizeof s->watch[0])) s->watch[s->nwatch++] = l;
    plat_mutex_unlock(&s->lk);
}

void sbl_request_restart(sbl_sender *s, const char *why)
{
    plat_mutex_lock(&s->lk);
    if (!s->restart_why[0]) snprintf(s->restart_why, sizeof s->restart_why, "%s", why);
    plat_mutex_unlock(&s->lk);
}

int sbl_watchdog(sbl_sender *s, uint64_t now, char *why, size_t n)
{
    /* no locks taken: whatever is stuck may be holding them */
    if (s->restart_why[0]) { snprintf(why, n, "%s", s->restart_why); return 1; }
    uint64_t t = s->send_since_us;
    if (t && now > t && now - t > SBL_WD_SEND_US) {
        snprintf(why, n, "a send has not returned for %llu s", (unsigned long long)((now - t) / 1000000u));
        return 1;
    }
    t = s->loop_beat_us;
    if (t && now > t && now - t > SBL_WD_LOOP_US) {
        snprintf(why, n, "the capture loop has not turned for %llu s", (unsigned long long)((now - t) / 1000000u));
        return 1;
    }
    for (int i = 0; i < s->nwatch; i++) {
        sbl_link *l = s->watch[i];
        t = l->rx_beat_us;
        if (l->watch_rx && t && now > t && now - t > SBL_WD_RX_US) {
            snprintf(why, n, "%s: the receive loop has not turned for %llu s", l->name,
                     (unsigned long long)((now - t) / 1000000u));
            return 1;
        }
    }
    return 0;
}

/* ---- the capture/send loop ------------------------------------------------------------------------- */

/* a PONG the receive thread queued because txlk was busy (txlk held) */
static void flush_pong_tx(sbl_sender *s)
{
    plat_mutex_lock(&s->lk);
    int have = s->pong_pending && s->active;
    sblk_msg p;
    memset(&p, 0, sizeof p);
    p.type = SBLK_T_PONG;
    p.u.pong = s->pong_q;
    s->pong_pending = 0;
    plat_mutex_unlock(&s->lk);
    if (!have) return;
    p.u.pong.t_us = plat_now_us();
    send_session_tx(s, &p);
}

/* a send failed (txlk held): end the session; USB has no connection to close, it hunts for HELLO */
static void fail_send_tx(sbl_sender *s, sbl_link *l, uint32_t gen, const char *why)
{
    plat_mutex_lock(&s->lk);
    int still = s->active && s->gen == gen && s->owner == l;
    if (still) {
        sbl_log("%s: %s", l->name, why);
        end_session_locked(s, why);
        if (l->is_usb) { s->owner = NULL; l->hunting = 1; }
    }
    plat_mutex_unlock(&s->lk);
    if (still && !l->is_usb) l->close(l);
}

static void set_display(sbl_sender *s, const sbl_cap_info *ci)
{
    plat_mutex_lock(&s->lk);
    s->disp_w = ci->width;
    s->disp_h = ci->height;
    s->disp_refresh = ci->refresh_mhz;
    s->geom = ci->geom;
    s->our_caps = SBLK_CAP_ENC_RAW | SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID | SBLK_CAP_INPUT_KEY |
                  SBLK_CAP_INPUT_PTR | SBLK_CAP_INPUT_TEXT | (ci->can_mode ? SBLK_CAP_MODE : 0) |
                  (ci->is_starbite ? SBLK_CAP_EXTEND : 0);
    s->cap_ok = 1;
    /* the output can change mid-session (the Starbite Display arrived after the primary was opened):
     * honour MODE from then on rather than refusing it for the rest of the session */
    if (s->active) s->caps = (s->our_caps & s->peer.caps) | SBLK_CAP_ENC_RAW;
    plat_mutex_unlock(&s->lk);
}

static void send_mode_tx(sbl_sender *s, uint32_t w, uint32_t h, uint32_t refresh, uint8_t status)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_MODE;
    m.u.mode.width = (uint16_t)w;
    m.u.mode.height = (uint16_t)h;
    m.u.mode.refresh_mhz = refresh;
    m.u.mode.status = status;
    send_session_tx(s, &m);
    sbl_log("MODE %s %ux%u", status == SBLK_MODE_APPLIED ? "APPLIED" : "REJECTED", w, h);
}

/* every 10 s while frames flow: this session's averages, plus what the OS handed the capture in the last
 * 10 s (os = frames acquired, acc = frames DWM presented, move/dirty rects, full = whole-image copies) */
static void log_stats(sbl_sender *s, uint64_t now, sbl_cap *cap, sbl_cap_stats *prev)
{
    double secs = (double)(now - s->st_t0) / 1e6;
    if (secs <= 0) return;
    sbl_cap_stats cs;
    memset(&cs, 0, sizeof cs);
    if (cap) sbl_cap_get_stats(cap, &cs);
    if (cs.os_frames < prev->os_frames) memset(prev, 0, sizeof *prev);   /* the capture was re-opened */
    sbl_log("stats: %.1f fps, %.0f KiB/s, %.2f ms encode/frame, %.1f rects/frame, %llu blocked by flow control, "
            "%llu stalls; last 10 s: os %llu (acc %llu, ptr %llu), move %llu, dirty %llu, full %llu, no-meta %llu",
            (double)s->st_frames / secs, (double)s->st_bytes / 1024.0 / secs,
            s->st_frames ? (double)s->st_enc_us / 1000.0 / (double)s->st_frames : 0.0,
            s->st_frames ? (double)s->st_rects / (double)s->st_frames : 0.0, (unsigned long long)s->st_blocked,
            (unsigned long long)s->st_stalls, (unsigned long long)(cs.os_frames - prev->os_frames),
            (unsigned long long)(cs.accumulated - prev->accumulated), (unsigned long long)(cs.ptr_only - prev->ptr_only),
            (unsigned long long)(cs.move_rects - prev->move_rects), (unsigned long long)(cs.dirty_rects - prev->dirty_rects),
            (unsigned long long)(cs.full - prev->full), (unsigned long long)(cs.no_meta - prev->no_meta));
    *prev = cs;
}

void sbl_sender_run(sbl_sender *s)
{
    sbl_cap *cap = NULL;
    sbl_cap_info ci;
    sbl_enc enc;
    sbl_region dirty;
    uint64_t reopen_at = 0, next_frame = 0, cap_us = 0, last_stats = plat_now_us();
    uint64_t mode_wait_until = 0;   /* a MODE change is waiting for the capture to re-open */
    uint64_t opened_at = 0;
    uint32_t told_w = 0, told_h = 0, told_gen = 0xFFFFFFFFu, armed_gen = 0xFFFFFFFFu;
    int blocked_logged = 0, open_fail_logged = 0, reset_logged = 0;
    sbl_cap_stats cst;
    memset(&cst, 0, sizeof cst);

    memset(&ci, 0, sizeof ci);
    if (sbl_enc_init(&enc, s->cfg.lz4_accel) < 0) { sbl_log("out of memory"); return; }
    sbl_region_clear(&dirty);
    int frame_us = 1000000 / (s->cfg.fps > 0 ? s->cfg.fps : 30);

    while (!s->stop) {
        uint64_t now = plat_now_us();
        s->loop_beat_us = now;

        /* (re)open the capture; kept open without a viewer too, so HELLO can announce the size (an
         * idle duplication costs nothing: no frames are acquired without a session) */
        if (!cap && now >= reopen_at) {
            if (sbl_cap_open(&cap, &s->cfg, &ci) < 0) {
                cap = NULL;
                /* every attempt builds a DXGI factory + D3D11 device (software rendering here): with no
                 * viewer (e.g. the lock screen after the Mac left) retry only every 10 s; a HELLO
                 * re-arms it at once (below) */
                plat_mutex_lock(&s->lk);
                int watched = s->active;
                plat_mutex_unlock(&s->lk);
                reopen_at = now + (watched ? 2000000u : 10000000u);
                if (!open_fail_logged) sbl_log("capture: not available, retrying every 2 s (10 s without a viewer)");
                open_fail_logged = 1;
            } else {
                open_fail_logged = 0;
                opened_at = plat_now_us();
                sbl_log("capture: %s, \"%s\" %ux%u @ %u mHz at (%d,%d)%s", ci.method, ci.name, ci.width,
                        ci.height, ci.refresh_mhz, ci.geom.out_x, ci.geom.out_y,
                        ci.is_starbite ? " [Starbite Display]" : "");
                set_display(s, &ci);
                mode_wait_until = 0;
                sbl_enc_reset(&enc);
                sbl_region_clear(&dirty);
                plat_mutex_lock(&s->lk);
                s->need_key = 1;
                plat_mutex_unlock(&s->lk);
            }
        }

        plat_mutex_lock(&s->lk);
        int active = s->active;
        uint32_t gen = s->gen;
        int can_send = sbl_flow_can_send(&s->flow);
        int need_key = s->need_key;
        int mode_req = s->mode_req;
        sblk_mode want = s->mode_want;
        s->mode_req = 0;
        int mode_pending = s->mode_pending;
        sbl_link *owner = s->owner;
        uint64_t last_rx = s->last_rx_us, last_tx = s->last_tx_us;
        uint32_t hello_w = s->hello_w, hello_h = s->hello_h;
        int pong_pending = s->pong_pending;
        uint64_t oldest = sbl_flow_oldest_us(&s->flow);
        int inflight = s->flow.n;
        uint32_t first_id = s->flow.n ? s->flow.ids[0] : 0, last_id = s->flow.last_sent;
        plat_mutex_unlock(&s->lk);
        /* now again, AFTER reading last_rx: the receive thread may have stamped last_rx since the top of
         * the loop, and an older now made now - last_rx wrap to ~2^64 = "link dead" at once (seen on the
         * S9: "nothing received for 5000 ms: link dead" 16 ms after a HELLO, and after ACKs) */
        now = plat_now_us();

        /* keep Windows awake while a viewer is connected (this thread owns the execution state) */
        if (s->cfg.keep_awake && s->awake != active) {
            s->awake = active;
            plat_keep_awake(active);
        }

        /* dead link: nothing received for dead_ms (also a connection that never said HELLO) */
        if (owner && now > last_rx && now - last_rx > (uint64_t)s->cfg.dead_ms * 1000u) {
            plat_mutex_lock(&s->txlk);
            plat_mutex_lock(&s->lk);
            uint64_t t = plat_now_us();   /* still dead now that we hold the locks (no HELLO meanwhile)? */
            int dead = s->owner == owner && t > s->last_rx_us && t - s->last_rx_us > (uint64_t)s->cfg.dead_ms * 1000u;
            if (dead) {
                sbl_log("%s: nothing received for %d ms: link dead", owner->name, s->cfg.dead_ms);
                end_session_locked(s, "link dead");
                if (owner->is_usb) { s->owner = NULL; owner->hunting = 1; }
                else s->last_rx_us = plat_now_us();   /* until the receive thread sees the close */
            }
            plat_mutex_unlock(&s->lk);
            if (dead && s->owner == owner && !owner->is_usb) owner->close(owner);   /* under txlk: link still alive */
            plat_mutex_unlock(&s->txlk);
            continue;
        }

        if (!active) {
            told_gen = 0xFFFFFFFFu;
            int ms = 1000;
            if (!cap && reopen_at > now && (reopen_at - now) / 1000 < 1000) ms = (int)((reopen_at - now) / 1000) + 1;
            plat_event_wait(s->wake, ms);
            continue;
        }

        /* a new viewer while the capture is down (lock screen, 10 s backoff): try at once */
        if (!cap && armed_gen != gen) { armed_gen = gen; reopen_at = 0; continue; }

        if (cap) {
            /* announce a size the viewer has not been told (Display Settings, our own MODE change, a
             * re-open between our HELLO and here); told_* start from what this session's HELLO said */
            if (told_gen != gen) { told_gen = gen; told_w = hello_w; told_h = hello_h; }
            if (mode_wait_until && now > mode_wait_until) mode_wait_until = 0;   /* no re-open came: answer */
            if (ci.width != told_w || ci.height != told_h || (mode_pending && !mode_wait_until)) {
                plat_mutex_lock(&s->txlk);
                send_mode_tx(s, ci.width, ci.height, ci.refresh_mhz, SBLK_MODE_APPLIED);
                plat_mutex_unlock(&s->txlk);
                plat_mutex_lock(&s->lk);
                s->mode_pending = 0;
                s->need_key = 1;
                plat_mutex_unlock(&s->lk);
                told_w = ci.width; told_h = ci.height;
                continue;
            }
        }

        if (mode_req) {
            int r = -1;
            if (cap && (s->caps & SBLK_CAP_MODE) && ci.can_mode)
                r = sbl_cap_set_mode(cap, want.width, want.height, want.refresh_mhz);
            sbl_log("MODE request %ux%u: %s", want.width, want.height,
                    r > 0 ? "changing" : r == 0 ? "already in effect" : "refused");
            if (r > 0) {
                plat_mutex_lock(&s->lk);
                s->mode_pending = 1;
                plat_mutex_unlock(&s->lk);
                mode_wait_until = now + 3000000;
            } else {
                plat_mutex_lock(&s->txlk);
                send_mode_tx(s, ci.width, ci.height, ci.refresh_mhz, r == 0 ? SBLK_MODE_APPLIED : SBLK_MODE_REJECTED);
                plat_mutex_unlock(&s->txlk);
                if (r == 0) {   /* after APPLIED the next frame is a keyframe */
                    plat_mutex_lock(&s->lk);
                    s->need_key = 1;
                    plat_mutex_unlock(&s->lk);
                }
            }
            continue;
        }

        if (pong_pending) {
            plat_mutex_lock(&s->txlk);
            flush_pong_tx(s);
            plat_mutex_unlock(&s->txlk);
        }

        if (now > last_tx && now - last_tx >= (uint64_t)s->cfg.ping_ms * 1000u) {
            sblk_msg m;
            memset(&m, 0, sizeof m);
            m.type = SBLK_T_PING;
            m.u.ping.id = ++s->ping_id;
            m.u.ping.t_us = now;
            plat_mutex_lock(&s->txlk);
            if (send_session_tx(s, &m) < 0 && owner) fail_send_tx(s, owner, gen, "PING could not be sent: session ends");
            plat_mutex_unlock(&s->txlk);
        }

        if (now - last_stats > 10000000u) {
            if (s->st_frames) log_stats(s, now, cap, &cst);
            last_stats = now;
        }

        if (!cap) { plat_event_wait(s->wake, 250); continue; }

        if (!can_send) {
            /* 2 frames in flight: do not even ask the OS for frames; it keeps accumulating the dirty
             * area (DXGI) and the next frame covers everything that changed meanwhile */
            if (!blocked_logged) { s->st_blocked++; blocked_logged = 1; }
            /* stall: the viewer is alive (the dead timer has not fired) but has not acknowledged them for
             * stall_ms. An ACK was lost or never matched: forget them and resynchronise with a keyframe
             * rather than freeze the picture for the rest of the session */
            if (oldest && now > oldest && now - oldest > (uint64_t)s->cfg.stall_ms * 1000u) {
                plat_mutex_lock(&s->lk);
                if (s->active && s->gen == gen && sbl_flow_oldest_us(&s->flow) == oldest) {
                    s->st_stalls++;
                    sbl_log("flow: %d frames (%u..%u) unacknowledged for %llu ms: forgetting them, keyframe next "
                            "(stall %llu this session)", inflight, first_id, last_id,
                            (unsigned long long)((now - oldest) / 1000u), (unsigned long long)s->st_stalls);
                    sbl_flow_restart(&s->flow);
                    s->need_key = 1;
                }
                plat_mutex_unlock(&s->lk);
                continue;
            }
            plat_event_wait(s->wake, s->cfg.ping_ms);
            continue;
        }
        blocked_logged = 0;
        if (now < next_frame) {
            plat_event_wait(s->wake, (int)((next_frame - now) / 1000) + 1);
            continue;
        }

        int valid = sbl_cap_valid(cap);
        int r = sbl_cap_next(cap, need_key && valid ? 0 : s->cfg.ping_ms / 2, &dirty, &cap_us);
        if (r < 0) {
            uint64_t t = plat_now_us();
            /* a reset straight after a re-open (GDI on the lock screen: open works, BitBlt fails) must
             * not become a busy loop of open/fail: at most one re-open per second then */
            int churn = t - opened_at < 1000000u;
            if (!churn || !reset_logged) sbl_log("capture: %s, re-opening", r == SBL_CAP_RESET ? "reset" : "error");
            reset_logged = churn;
            sbl_cap_close(cap);
            cap = NULL;
            reopen_at = r == SBL_CAP_RESET && !churn ? 0 : t + 1000000;
            continue;
        }
        uint64_t t_cap = plat_now_us();
        if (!sbl_cap_valid(cap)) continue;
        if (!need_key && dirty.n == 0) continue;
        if (!cap_us) cap_us = t_cap;

        const uint8_t *px;
        size_t stride;
        if (sbl_cap_map(cap, &px, &stride) < 0) {
            sbl_log("capture: map failed, re-opening");
            sbl_cap_close(cap);
            cap = NULL;
            continue;
        }
        /* Encode WITHOUT txlk: a keyframe takes tens of ms on the A55s, and the receive thread needs txlk
         * for every PONG (the viewer PINGs each second), so holding it here stalled all input behind the
         * encoder. The frame is numbered from tx_seq now and renumbered at send time if a PONG/MODE went
         * out meanwhile; txlk is still held for the whole send, so a frame stays one unbroken batch. */
        plat_mutex_lock(&s->lk);
        int ok = s->active && s->gen == gen && s->owner;
        sbl_link *l = s->owner;
        uint32_t caps = s->caps, maxp = s->peer.max_payload, seq0 = s->tx_seq, seq = seq0;
        need_key = s->need_key;
        if (ok) s->need_key = 0;   /* taken now: an ACK(0) that arrives during this frame asks again */
        uint32_t id = s->flow.next_id;
        plat_mutex_unlock(&s->lk);
        if (ok) {
            uint64_t t0 = plat_now_us();
            long len = sbl_enc_frame(&enc, px, stride, ci.width, ci.height, &dirty, need_key, caps, maxp, id,
                                     cap_us, &seq);
            uint64_t t1 = plat_now_us();
            int sent = -1;
            plat_mutex_lock(&s->txlk);
            plat_mutex_lock(&s->lk);
            /* same session as when the frame was taken? (else the new one starts with a keyframe) */
            int still = s->active && s->gen == gen && s->owner == l && s->flow.next_id == id;
            uint32_t seq_now = s->tx_seq;
            if (still && len > 0) {
                if (seq_now != seq0) {
                    renumber(enc.out, (size_t)len, seq_now);
                    seq = seq_now + (seq - seq0);
                    s->st_renumbered++;
                }
                /* in flight and numbered BEFORE the send: the viewer's ACK for it can arrive on the receive
                 * thread before l->send() returns; recorded afterwards it was "not sent", ignored, and the
                 * frame stayed in flight for good (the S9's frozen picture) */
                s->tx_seq = seq;
                sbl_flow_sent(&s->flow, sbl_flow_take_id(&s->flow), plat_now_us());
            }
            plat_mutex_unlock(&s->lk);
            if (still && len > 0) sent = link_send_tx(s, l, enc.out, (size_t)len);
            if (still && len < 0) {
                sbl_log("encode failed: %s", sblk_strerror((int)len));
                plat_mutex_lock(&s->lk);
                uint32_t bseq = s->tx_seq++;
                plat_mutex_unlock(&s->lk);
                send_bye_tx(s, l, SBLK_BYE_SHUTDOWN, "frame encoding failed", bseq);
            }
            plat_mutex_lock(&s->lk);
            if (!still) {
                /* dropped: the shadow (enc.prev) already has it, but the new session's HELLO set need_key */
            } else if (len < 0 || (len > 0 && sent < 0)) {
                end_session_locked(s, len < 0 ? "encode failed" : "send failed");
                if (l->is_usb) { s->owner = NULL; l->hunting = 1; }   /* USB: no close; wait for HELLO */
            } else if (len > 0) {
                s->last_tx_us = plat_now_us();
                if (enc.keyframe) s->st_keys++;
                s->st_frames++;
                s->st_bytes += (uint64_t)len;
                s->st_enc_us += t1 - t0;
                s->st_rects += enc.nrects;
            }
            plat_mutex_unlock(&s->lk);
            if (s->cfg.verbose && still && len > 0)
                sbl_log("frame %u%s: %u rects (%u solid, %u lz4, %u raw), %llu px -> %ld bytes, %.2f ms", id,
                        enc.keyframe ? " key" : "", enc.nrects, enc.nsolid, enc.nlz4, enc.nraw,
                        (unsigned long long)enc.area, len, (double)(t1 - t0) / 1000.0);
            if (still && (len < 0 || (len > 0 && sent < 0)) && !l->is_usb) l->close(l);   /* under txlk */
            if (still && len > 0 && sent == 0) flush_pong_tx(s);   /* a PING that came in during the send */
            plat_mutex_unlock(&s->txlk);
        }
        sbl_cap_unmap(cap);
        sbl_region_clear(&dirty);
        cap_us = 0;
        next_frame = t_cap + (uint64_t)frame_us;
    }
    if (cap) sbl_cap_close(cap);
    sbl_enc_free(&enc);
    if (s->awake) { s->awake = 0; plat_keep_awake(0); }
    s->loop_beat_us = 0;
}
