/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd: dirty-region accumulator, frame encoder, flow control (portable; host-tested by test_host.c).
 *
 * CPU rules (PROTOCOL.md section 8): only the OS's dirty rects are compared (64x64 tiles, against the
 * shadow of what the viewer has), only tiles that really changed are encoded, solid -> LZ4 -> raw.
 * A frame with no real change produces nothing at all.
 */
#include "sender.h"

#include <stdlib.h>
#include <string.h>

/* ---- region -------------------------------------------------------------------------------------- */

void sbl_region_clear(sbl_region *g) { g->n = 0; }

void sbl_region_full(sbl_region *g, uint32_t fw, uint32_t fh)
{
    g->n = 0;
    sbl_region_add(g, 0, 0, (int32_t)fw, (int32_t)fh, fw, fh);
}

static int contains(const sblk_rect *a, const sblk_rect *b)   /* a contains b */
{
    return b->x >= a->x && b->y >= a->y && (uint32_t)b->x + b->w <= (uint32_t)a->x + a->w &&
           (uint32_t)b->y + b->h <= (uint32_t)a->y + a->h;
}

void sbl_region_add(sbl_region *g, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fw, uint32_t fh)
{
    if (w <= 0 || h <= 0 || fw == 0 || fh == 0) return;
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)fw) x1 = fw;
    if (y1 > (int64_t)fh) y1 = fh;
    if (x0 >= x1 || y0 >= y1) return;
    sblk_rect r = { (uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0) };

    int k = 0;
    for (int i = 0; i < g->n; i++) {
        if (contains(&g->r[i], &r)) return;
        if (!contains(&r, &g->r[i])) g->r[k++] = g->r[i];
    }
    g->n = k;
    if (g->n < SBL_REGION_MAX) { g->r[g->n++] = r; return; }

    uint32_t bx0 = r.x, by0 = r.y, bx1 = (uint32_t)r.x + r.w, by1 = (uint32_t)r.y + r.h;
    for (int i = 0; i < g->n; i++) {
        const sblk_rect *o = &g->r[i];
        if (o->x < bx0) bx0 = o->x;
        if (o->y < by0) by0 = o->y;
        if ((uint32_t)o->x + o->w > bx1) bx1 = (uint32_t)o->x + o->w;
        if ((uint32_t)o->y + o->h > by1) by1 = (uint32_t)o->y + o->h;
    }
    g->n = 1;
    g->r[0].x = (uint16_t)bx0; g->r[0].y = (uint16_t)by0;
    g->r[0].w = (uint16_t)(bx1 - bx0); g->r[0].h = (uint16_t)(by1 - by0);
}

/* ---- encoder ------------------------------------------------------------------------------------- */

int sbl_enc_init(sbl_enc *e, int lz4_accel)
{
    memset(e, 0, sizeof *e);
    e->lz4_accel = lz4_accel > 0 ? lz4_accel : 1;
    e->scratch = malloc(SBLK_MAX_PAYLOAD);
    e->lz4state = malloc(SBLK_LZ4_STATE_SIZE);
    e->dirty = malloc(SBL_MAX_RECTS * sizeof(sblk_rect));
    e->strips = malloc(SBL_MAX_STRIPS * sizeof(sblk_rect));
    if (!e->scratch || !e->lz4state || !e->dirty || !e->strips) { sbl_enc_free(e); return SBLK_E_SPACE; }
    return SBLK_OK;
}

void sbl_enc_free(sbl_enc *e)
{
    free(e->prev); free(e->scratch); free(e->lz4state); free(e->dirty); free(e->strips); free(e->out);
    memset(e, 0, sizeof *e);
}

void sbl_enc_reset(sbl_enc *e)
{
    free(e->prev);
    e->prev = NULL;
}

static void bbox_collapse(sblk_rect *r, int *n)
{
    uint32_t bx0 = 0xFFFFFFFFu, by0 = 0xFFFFFFFFu, bx1 = 0, by1 = 0;
    for (int i = 0; i < *n; i++) {
        if (r[i].x < bx0) bx0 = r[i].x;
        if (r[i].y < by0) by0 = r[i].y;
        if ((uint32_t)r[i].x + r[i].w > bx1) bx1 = (uint32_t)r[i].x + r[i].w;
        if ((uint32_t)r[i].y + r[i].h > by1) by1 = (uint32_t)r[i].y + r[i].h;
    }
    if (*n == 0) return;
    r[0].x = (uint16_t)bx0; r[0].y = (uint16_t)by0;
    r[0].w = (uint16_t)(bx1 - bx0); r[0].h = (uint16_t)(by1 - by0);
    *n = 1;
}

long sbl_enc_frame(sbl_enc *e, const uint8_t *cur, size_t stride, uint32_t w, uint32_t h,
                   const sbl_region *dirty, int keyframe, uint32_t caps, uint32_t max_payload,
                   uint32_t frame_id, uint64_t capture_us, uint32_t *seq)
{
    if (!e || !e->scratch || !cur || !seq || w == 0 || h == 0 || w > 0xFFFFu || h > 0xFFFFu ||
        stride < (size_t)w * 4)
        return SBLK_E_ARG;
    if (max_payload > SBLK_MAX_PAYLOAD) max_payload = SBLK_MAX_PAYLOAD;
    if (max_payload < SBLK_RECT_HEAD + (size_t)w * 4) return SBLK_E_ARG;   /* cannot send a full row */
    caps |= SBLK_CAP_ENC_RAW;

    if (!e->prev || e->stride != stride || e->w != w || e->h != h) {
        free(e->prev);
        e->prev = malloc(stride * h);
        if (!e->prev) return SBLK_E_SPACE;
        e->stride = stride; e->w = w; e->h = h;
        keyframe = 1;
    }

    int n = 0;
    if (!keyframe) {
        for (int i = 0; dirty && i < dirty->n; i++) {
            if (n > SBL_MAX_RECTS - 8) bbox_collapse(e->dirty, &n);
            int k = sblk_dirty_rects(e->prev, cur, stride, w, h, &dirty->r[i], SBLK_DIRTY_UPDATE_PREV,
                                     e->dirty + n, SBL_MAX_RECTS - n);
            if (k < 0) return k;
            n += k;
        }
        if (n == 0) return 0;
    }

    int ns = 0;
    if (!keyframe) {
        for (int i = 0; i < n; i++) {
            int k = sblk_split_rect(&e->dirty[i], max_payload, e->strips + ns, SBL_MAX_STRIPS - ns);
            if (k == SBLK_E_SPACE) { keyframe = 1; break; }   /* absurdly many strips: send it all */
            if (k < 0) return k;
            ns += k;
        }
    }
    if (keyframe) {
        for (uint32_t y = 0; y < h; y++) memcpy(e->prev + y * stride, cur + y * stride, (size_t)w * 4);
        sblk_rect all = { 0, 0, (uint16_t)w, (uint16_t)h };
        ns = sblk_split_rect(&all, max_payload, e->strips, SBL_MAX_STRIPS);
        if (ns < 0) return ns;
    }

    size_t need = SBLK_HDR_SIZE + SBLK_FRAME_BEGIN_SIZE + SBLK_HDR_SIZE + SBLK_FRAME_END_SIZE;
    for (int i = 0; i < ns; i++) need += SBLK_RECT_MSG_HEAD + (size_t)e->strips[i].w * e->strips[i].h * 4;
    if (need > e->outcap) {
        free(e->out);
        e->outcap = need + need / 8;
        e->out = malloc(e->outcap);
        if (!e->out) { e->outcap = 0; return SBLK_E_SPACE; }
    }

    uint32_t s = *seq;
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_FRAME_BEGIN;
    m.seq = s++;
    m.u.frame_begin.frame_id = frame_id;
    m.u.frame_begin.width = (uint16_t)w;
    m.u.frame_begin.height = (uint16_t)h;
    m.u.frame_begin.nrects = (uint16_t)ns;
    m.u.frame_begin.fflags = keyframe ? SBLK_FB_KEYFRAME : 0;
    long r = sblk_encode(&m, e->out, e->outcap);
    if (r < 0) return r;
    size_t off = (size_t)r;

    e->nrects = (uint32_t)ns; e->nsolid = e->nlz4 = e->nraw = 0; e->area = 0; e->keyframe = keyframe;
    for (int i = 0; i < ns; i++) {
        uint8_t enc = 0;
        r = sblk_build_rect(e->out + off, e->outcap - off, cur, stride, &e->strips[i], caps, e->lz4_accel,
                            e->lz4state, e->scratch, s++, &enc);
        if (r < 0) return r;
        off += (size_t)r;
        if (enc == SBLK_ENC_SOLID) e->nsolid++;
        else if (enc == SBLK_ENC_LZ4) e->nlz4++;
        else e->nraw++;
        e->area += (uint64_t)e->strips[i].w * e->strips[i].h;
    }

    memset(&m, 0, sizeof m);
    m.type = SBLK_T_FRAME_END;
    m.seq = s++;
    m.u.frame_end.frame_id = frame_id;
    m.u.frame_end.capture_us = capture_us;
    r = sblk_encode(&m, e->out + off, e->outcap - off);
    if (r < 0) return r;
    off += (size_t)r;

    *seq = s;
    e->outlen = off;
    return (long)off;
}

/* ---- flow control -------------------------------------------------------------------------------- */

void sbl_flow_reset(sbl_flow *f)
{
    memset(f, 0, sizeof *f);
    f->next_id = 1;
}

uint32_t sbl_flow_take_id(sbl_flow *f)
{
    uint32_t id = f->next_id;
    f->next_id = id + 1u ? id + 1u : 1u;   /* never 0 */
    return id;
}

void sbl_flow_restart(sbl_flow *f)
{
    uint32_t next = f->next_id ? f->next_id : 1u;
    memset(f, 0, sizeof *f);
    f->next_id = next;
}

void sbl_flow_sent(sbl_flow *f, uint32_t id, uint64_t now_us)
{
    if (f->n < (int)SBLK_MAX_IN_FLIGHT) { f->ids[f->n] = id; f->ts[f->n] = now_us; f->n++; }
    f->last_sent = id;
    f->sent_any = 1;
}

/* serial-number distance from a to b; the skipped id 0 makes it one too big across the wrap, which
 * only matters for "how far", never for "before/after" */
static int32_t serial_diff(uint32_t b, uint32_t a) { return (int32_t)(b - a); }

int sbl_flow_ack(sbl_flow *f, uint32_t id)
{
    if (!f->sent_any || serial_diff(id, f->last_sent) > 0) return -1;
    int k = -1;
    for (int i = 0; i < f->n; i++)
        if (serial_diff(id, f->ids[i]) >= 0) k = i;
    if (k < 0) return 0;
    memmove(f->ids, f->ids + k + 1, (size_t)(f->n - k - 1) * sizeof f->ids[0]);
    memmove(f->ts, f->ts + k + 1, (size_t)(f->n - k - 1) * sizeof f->ts[0]);
    f->n -= k + 1;
    return 1;
}
