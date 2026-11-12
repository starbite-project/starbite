/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ACL reassembly / fragmentation and L2CAP basic mode with its signalling channel, for one BR/EDR link.
 * Formats and sources: bt_l2cap.h. Pure C (no hardware, no libc), single-threaded; every malformed input is dropped
 * and counted. Seen on a phone: not yet (host-tested in kern/host/bthid-test.c). */
#include "bt_l2cap.h"

#define OUT_MAX 128U                        /* the longest frame payload we send (signalling, HID output reports) */
#define ACL_START 0x2000U                   /* Packet_Boundary 0b10: the first fragment (automatically flushable) */
#define ACL_CONT 0x1000U                    /* 0b01: a continuation */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void zero(void *d, unsigned n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = 0;
}

void l2_init(struct l2cap *l, const struct l2ops *ops, void *ctx)
{
    zero(l, sizeof(*l));
    l->ops = ops;
    l->ctx = ctx;
    l->next_ident = 1;
    l->next_cid = 0x0040;
}

void l2_link_up(struct l2cap *l, uint16_t handle, uint16_t acl_mtu)
{
    l->up = 1;
    l->handle = handle & 0x0fff;
    l->acl_mtu = acl_mtu >= 8 ? acl_mtu : 8;
    l->rx_have = l->rx_need = l->rx_dropping = 0;
}

static void ch_close(struct l2cap *l, struct l2ch *c, uint16_t why)
{
    if (c->state == L2C_FREE)
        return;
    struct l2ch was = *c;                   /* the slot is free before the callback (it may open a channel) */
    zero(c, sizeof(*c));
    was.why = why;
    was.state = L2C_FREE;
    if (l->ops->closed)
        l->ops->closed(l->ctx, &was);
}

void l2_link_down(struct l2cap *l)
{
    for (unsigned i = 0; i < L2_NCH; i++)
        ch_close(l, &l->ch[i], 0xFFFD);
    l->up = 0;
    l->rx_have = l->rx_need = l->rx_dropping = 0;
}

/* ---- out ---- */

void l2_frame_out(struct l2cap *l, uint16_t cid, const uint8_t *p, unsigned n)
{
    uint8_t f[4 + OUT_MAX], pkt[4 + 4 + OUT_MAX];
    if (!l->up || n > OUT_MAX)
        return;
    put16(f, (uint16_t)n);
    put16(f + 2, cid);
    for (unsigned i = 0; i < n; i++)
        f[4 + i] = p[i];
    unsigned total = 4 + n, at = 0;
    while (at < total) {
        unsigned k = total - at;
        if (k > l->acl_mtu)
            k = l->acl_mtu;
        put16(pkt, (uint16_t)(l->handle | (at ? ACL_CONT : ACL_START)));
        put16(pkt + 2, (uint16_t)k);
        for (unsigned i = 0; i < k; i++)
            pkt[4 + i] = f[at + i];
        l->ops->send_acl(l->ctx, pkt, 4 + k);
        l->acl_out++;
        at += k;
    }
    l->frames_out++;
}

static uint8_t nid(struct l2cap *l)
{
    uint8_t i = l->next_ident++;
    if (!l->next_ident)
        l->next_ident = 1;
    return i;
}

static void sig_out(struct l2cap *l, uint8_t code, uint8_t id, const uint8_t *d, unsigned n)
{
    uint8_t c[4 + 64];
    if (n > 64)
        n = 64;
    c[0] = code;
    c[1] = id;
    put16(c + 2, (uint16_t)n);
    for (unsigned i = 0; i < n; i++)
        c[4 + i] = d[i];
    l2_frame_out(l, L2_SIG_CID, c, 4 + n);
}

static void reject(struct l2cap *l, uint8_t id, uint16_t reason, uint16_t a, uint16_t b)
{
    uint8_t d[6];
    put16(d, reason);
    put16(d + 2, a);
    put16(d + 4, b);
    sig_out(l, L2_CMD_REJECT, id, d, reason == 0x0002 ? 6U : 2U);
    l->rejects++;
}

static void conn_rsp(struct l2cap *l, uint8_t id, uint16_t dcid, uint16_t scid, uint16_t result, uint16_t status)
{
    uint8_t d[8];
    put16(d, dcid);
    put16(d + 2, scid);
    put16(d + 4, result);
    put16(d + 6, status);
    sig_out(l, L2_CONN_RSP, id, d, 8);
}

static void conf_req(struct l2cap *l, struct l2ch *c, int with_mtu)
{
    uint8_t d[8];
    put16(d, c->rcid);
    put16(d + 2, 0);
    d[4] = 0x01;                            /* MTU */
    d[5] = 2;
    put16(d + 6, (uint16_t)L2_MTU);
    c->ident = nid(l);
    c->conf_out = 1;
    c->dead = l->now + L2_RTX_MS;
    sig_out(l, L2_CONF_REQ, c->ident, d, with_mtu ? 8U : 4U);
}

static void maybe_open(struct l2cap *l, struct l2ch *c)
{
    if (c->state == L2C_CONFIG && c->conf_out == 2 && c->conf_in) {
        c->state = L2C_OPEN;
        c->dead = 0;
        if (l->ops->opened)
            l->ops->opened(l->ctx, c);
    }
}

static struct l2ch *ch_new(struct l2cap *l)
{
    for (unsigned i = 0; i < L2_NCH; i++)
        if (l->ch[i].state == L2C_FREE) {
            struct l2ch *c = &l->ch[i];
            zero(c, sizeof(*c));
            c->lcid = l->next_cid++;
            if (l->next_cid < 0x0040)
                l->next_cid = 0x0040;
            c->rmtu = L2_MTU;
            return c;
        }
    return 0;
}

static struct l2ch *by_lcid(struct l2cap *l, uint16_t lcid)
{
    for (unsigned i = 0; i < L2_NCH; i++)
        if (l->ch[i].state != L2C_FREE && l->ch[i].lcid == lcid)
            return &l->ch[i];
    return 0;
}

struct l2ch *l2_find_psm(struct l2cap *l, uint16_t psm)
{
    for (unsigned i = 0; i < L2_NCH; i++)
        if (l->ch[i].state != L2C_FREE && l->ch[i].psm == psm)
            return &l->ch[i];
    return 0;
}

struct l2ch *l2_connect(struct l2cap *l, uint16_t psm)
{
    if (!l->up)
        return 0;
    struct l2ch *c = ch_new(l);
    if (!c)
        return 0;
    uint8_t d[4];
    c->psm = psm;
    c->state = L2C_WAIT_CONN;
    c->ident = nid(l);
    c->dead = l->now + L2_RTX_MS;
    put16(d, psm);
    put16(d + 2, c->lcid);
    sig_out(l, L2_CONN_REQ, c->ident, d, 4);
    return c;
}

void l2_secure(struct l2cap *l, int ok)
{
    for (unsigned i = 0; i < L2_NCH; i++) {
        struct l2ch *c = &l->ch[i];
        if (c->state != L2C_WAIT_SEC)
            continue;
        if (ok) {
            conn_rsp(l, c->req_ident, c->lcid, c->rcid, L2_CR_SUCCESS, 0);
            c->state = L2C_CONFIG;
            conf_req(l, c, 1);
        } else {
            conn_rsp(l, c->req_ident, 0, c->rcid, L2_CR_SEC_BLOCK, 0);
            ch_close(l, c, L2_CR_SEC_BLOCK);
        }
    }
}

int l2_send(struct l2cap *l, struct l2ch *c, const uint8_t *p, unsigned n)
{
    if (!l->up || !c || c->state != L2C_OPEN || n > OUT_MAX || n > c->rmtu)
        return 0;
    l2_frame_out(l, c->rcid, p, n);
    return 1;
}

void l2_disconnect(struct l2cap *l, struct l2ch *c)
{
    if (!c || c->state == L2C_FREE)
        return;
    if (c->state == L2C_WAIT_SEC) {
        conn_rsp(l, c->req_ident, 0, c->rcid, L2_CR_SEC_BLOCK, 0);
        ch_close(l, c, 0);
        return;
    }
    if (c->state == L2C_WAIT_CONN || c->state == L2C_WAIT_DISC || !l->up) {
        ch_close(l, c, 0);
        return;
    }
    uint8_t d[4];
    put16(d, c->rcid);
    put16(d + 2, c->lcid);
    c->state = L2C_WAIT_DISC;
    c->ident = nid(l);
    c->dead = l->now + L2_RTX_MS;
    sig_out(l, L2_DISC_REQ, c->ident, d, 4);
}

void l2_tick(struct l2cap *l, uint64_t now_ms)
{
    l->now = now_ms;
    for (unsigned i = 0; i < L2_NCH; i++) {
        struct l2ch *c = &l->ch[i];
        if (c->state != L2C_FREE && c->state != L2C_OPEN && c->state != L2C_WAIT_SEC && c->dead && now_ms >= c->dead)
            ch_close(l, c, 0xFFFF);
    }
}

/* ---- in: the signalling channel ---- */

static int known_opt(uint8_t t)
{
    return t >= 1 && t <= 7;
}

static void on_conf_req(struct l2cap *l, uint8_t id, const uint8_t *d, unsigned n)
{
    struct l2ch *c = by_lcid(l, le16(d));
    uint16_t flags = le16(d + 2);
    if (!c || (c->state != L2C_CONFIG && c->state != L2C_OPEN)) {
        reject(l, id, 0x0002, le16(d), 0);
        return;
    }
    uint8_t rsp[6 + 32];
    unsigned rn = 6, bad = 0;
    uint16_t result = 0, mtu = c->rmtu;
    for (unsigned at = 4; at < n;) {
        if (at + 2 > n || at + 2 + d[at + 1] > n) {
            result = 0x0002;                /* rejected: the options do not parse */
            l->malformed++;
            break;
        }
        uint8_t t = d[at], ol = d[at + 1];
        const uint8_t *v = d + at + 2;
        if ((t & 0x7f) == 0x01 && ol == 2) {
            mtu = le16(v);
        } else if ((t & 0x7f) == 0x04 && ol >= 1 && v[0] != 0) {
            /* a retransmission / streaming mode: only basic mode here */
            if (!bad && rn + 11 <= sizeof(rsp)) {
                rsp[rn++] = 0x04;
                rsp[rn++] = 9;
                for (unsigned k = 0; k < 9; k++)
                    rsp[rn++] = 0;
                result = 0x0001;
            }
        } else if (!(t & 0x80) && !known_opt(t)) {
            if (result != 0x0003)
                rn = 6;                     /* only the unknown ones go back */
            result = 0x0003;
            for (unsigned k = 0; k < 2U + ol && rn < sizeof(rsp); k++)
                rsp[rn++] = d[at + k];
            bad = 1;
        }
        at += 2U + ol;
    }
    if (result == 0 && mtu < 48)            /* the minimum MTU (vol 3 part A 5.1) */
        mtu = 48;
    put16(rsp, c->rcid);
    put16(rsp + 2, (uint16_t)(flags & 1U));
    put16(rsp + 4, result);
    if (result == 0)
        rn = 6;
    sig_out(l, L2_CONF_RSP, id, rsp, rn);
    if (result == 0) {
        c->rmtu = mtu;
        if (!(flags & 1U)) {
            c->conf_in = 1;
            if (c->conf_out == 2)
                maybe_open(l, c);
            else if (!c->dead)
                c->dead = l->now + L2_RTX_MS;
        }
    }
}

static void signalling(struct l2cap *l, const uint8_t *f, unsigned n)
{
    for (unsigned at = 0; at < n;) {
        if (at + 4 > n) {
            l->malformed++;
            return;
        }
        uint8_t code = f[at], id = f[at + 1];
        unsigned cl = le16(f + at + 2);
        const uint8_t *d = f + at + 4;
        if (at + 4 + cl > n) {
            l->malformed++;
            return;
        }
        at += 4 + cl;
        if (!id) {                          /* identifier 0 is never valid */
            l->malformed++;
            continue;
        }
        switch (code) {
        case L2_CONN_REQ: {
            if (cl < 4) {
                l->malformed++;
                break;
            }
            uint16_t psm = le16(d), scid = le16(d + 2);
            if (scid < 0x0040) {
                conn_rsp(l, id, 0, scid, 0x0006, 0);    /* invalid source CID */
                break;
            }
            int dup = 0;
            for (unsigned i = 0; i < L2_NCH; i++)
                if (l->ch[i].state != L2C_FREE && l->ch[i].incoming && l->ch[i].rcid == scid)
                    dup = 1;
            if (dup) {
                l->dropped++;
                break;
            }
            struct l2ch *c = ch_new(l);
            if (!c) {
                conn_rsp(l, id, 0, scid, L2_CR_NO_MEM, 0);
                break;
            }
            uint16_t r = l->ops->accept ? l->ops->accept(l->ctx, psm) : L2_CR_BAD_PSM;
            if (r == L2_CR_SUCCESS || r == L2_CR_PEND) {
                c->psm = psm;
                c->rcid = scid;
                c->incoming = 1;
                c->req_ident = id;
                if (r == L2_CR_PEND) {
                    c->state = L2C_WAIT_SEC;
                    conn_rsp(l, id, c->lcid, scid, L2_CR_PEND, 0x0001);   /* authentication pending */
                } else {
                    c->state = L2C_CONFIG;
                    conn_rsp(l, id, c->lcid, scid, L2_CR_SUCCESS, 0);
                    conf_req(l, c, 1);
                }
            } else {
                conn_rsp(l, id, 0, scid, r, 0);
                zero(c, sizeof(*c));
            }
            break;
        }
        case L2_CONN_RSP: {
            if (cl < 8) {
                l->malformed++;
                break;
            }
            struct l2ch *c = by_lcid(l, le16(d + 2));
            uint16_t res = le16(d + 4);
            if (!c || c->state != L2C_WAIT_CONN || c->ident != id) {
                l->dropped++;
                break;
            }
            if (res == L2_CR_SUCCESS) {
                c->rcid = le16(d);
                c->state = L2C_CONFIG;
                conf_req(l, c, 1);
            } else if (res == L2_CR_PEND) {
                c->dead = l->now + 6U * L2_RTX_MS;      /* ERTX */
            } else {
                ch_close(l, c, res);
            }
            break;
        }
        case L2_CONF_REQ:
            if (cl < 4) {
                l->malformed++;
                break;
            }
            on_conf_req(l, id, d, cl);
            break;
        case L2_CONF_RSP: {
            if (cl < 6) {
                l->malformed++;
                break;
            }
            struct l2ch *c = by_lcid(l, le16(d));
            uint16_t res = le16(d + 4);
            if (!c || c->state != L2C_CONFIG || c->conf_out != 1 || c->ident != id) {
                l->dropped++;
                break;
            }
            if (res == 0) {
                c->conf_out = 2;
                c->dead = c->conf_in ? 0 : l->now + L2_RTX_MS;
                maybe_open(l, c);
            } else if (res == 0x0004) {
                c->dead = l->now + 6U * L2_RTX_MS;      /* pending */
            } else if (!c->why) {
                c->why = 1;                 /* one more try without options (the default MTU is ours too) */
                conf_req(l, c, 0);
            } else {
                ch_close(l, c, 0xFFFE);
            }
            break;
        }
        case L2_DISC_REQ: {
            if (cl < 4) {
                l->malformed++;
                break;
            }
            struct l2ch *c = by_lcid(l, le16(d));
            if (!c || c->rcid != le16(d + 2)) {
                reject(l, id, 0x0002, le16(d), le16(d + 2));
                break;
            }
            sig_out(l, L2_DISC_RSP, id, d, 4);
            ch_close(l, c, 0);
            break;
        }
        case L2_DISC_RSP: {
            if (cl < 4) {
                l->malformed++;
                break;
            }
            struct l2ch *c = by_lcid(l, le16(d + 2));
            if (c && c->state == L2C_WAIT_DISC && c->ident == id)
                ch_close(l, c, 0);
            else
                l->dropped++;
            break;
        }
        case L2_ECHO_REQ:
            sig_out(l, L2_ECHO_RSP, id, d, cl);
            break;
        case L2_INFO_REQ: {
            if (cl < 2) {
                l->malformed++;
                break;
            }
            uint8_t r[12] = { d[0], d[1], 0, 0 };
            unsigned rn = 4;
            uint16_t type = le16(d);
            if (type == 0x0001) {           /* connectionless MTU */
                put16(r + 4, (uint16_t)L2_MTU);
                rn = 6;
            } else if (type == 0x0002) {    /* extended features: none (basic mode only) */
                rn = 8;
            } else if (type == 0x0003) {    /* fixed channels: the signalling channel only */
                r[4] = 0x02;
                rn = 12;
            } else {
                put16(r + 2, 0x0001);       /* not supported */
            }
            sig_out(l, L2_INFO_RSP, id, r, rn);
            break;
        }
        case L2_CMD_REJECT:
            for (unsigned i = 0; i < L2_NCH; i++)
                if (l->ch[i].state != L2C_FREE && l->ch[i].state != L2C_OPEN && l->ch[i].ident == id)
                    ch_close(l, &l->ch[i], 0xFFFE);
            break;
        case L2_ECHO_RSP:
        case L2_INFO_RSP:
            break;
        default:
            reject(l, id, 0x0000, 0, 0);    /* command not understood */
            break;
        }
    }
}

/* ---- in: ACL packets into frames ---- */

static void frame(struct l2cap *l, const uint8_t *f, unsigned n)
{
    uint16_t len = le16(f), cid = le16(f + 2);
    if (len + 4U != n) {
        l->malformed++;
        return;
    }
    l->frames_in++;
    if (cid == L2_SIG_CID) {
        signalling(l, f + 4, len);
        return;
    }
    struct l2ch *c = by_lcid(l, cid);
    if (c && c->state == L2C_OPEN && l->ops->data) {
        l->ops->data(l->ctx, c, f + 4, len);
        return;
    }
    l->dropped++;
}

void l2_acl_in(struct l2cap *l, const uint8_t *pkt, unsigned n)
{
    if (!l->up || n < 4) {
        l->dropped++;
        return;
    }
    uint16_t hf = le16(pkt), dl = le16(pkt + 2);
    unsigned pb = (hf >> 12) & 3U;
    if ((hf & 0x0fff) != l->handle || dl + 4U != n || pb == 3) {
        l->malformed++;
        return;
    }
    const uint8_t *d = pkt + 4;
    if (pb != 1) {                          /* a start (0b10; 0b00 taken as one too) */
        if (l->rx_have)
            l->dropped++;                   /* the frame before it never finished */
        l->rx_have = l->rx_need = l->rx_dropping = 0;
    } else if (!l->rx_have) {
        l->dropped++;                       /* a continuation with nothing started */
        return;
    }
    for (unsigned i = 0; i < dl; i++) {
        if (l->rx_have < sizeof(l->rx))
            l->rx[l->rx_have] = d[i];
        l->rx_have++;
        if (l->rx_have == 4) {
            l->rx_need = 4U + le16(l->rx);
            l->rx_dropping = l->rx_need > sizeof(l->rx);
        }
        if (l->rx_have >= 4 && l->rx_have == l->rx_need) {
            if (l->rx_dropping)
                l->dropped++;
            else
                frame(l, l->rx, l->rx_have);
            l->rx_have = l->rx_need = l->rx_dropping = 0;
            if (i + 1 < dl) {               /* bytes past the frame's own length */
                l->malformed++;
                return;
            }
        }
    }
}
