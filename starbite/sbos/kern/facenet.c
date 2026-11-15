/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Face Unlock: the CNN runtime (facenet.h). Pure C, no libc. */
#include "facenet.h"
#include "passcode.h"                   /* pass_sha256: the pack's digest */

typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
/* lane-wise select / max without the ternary on vectors (clang's C has none) */
static inline v4f vsel(v4i m, v4f a, v4f b) { return (v4f)(((v4i)a & m) | ((v4i)b & ~m)); }
static inline v4f vmax(v4f a, v4f b) { return vsel(a > b, a, b); }

struct fn_hdr {
    char magic[8];
    uint32_t version, nnet, len;
    uint32_t net_off[3];
    uint8_t digest[32];
};
_Static_assert(sizeof(struct fn_op) == 36, "fn_op is 36 bytes");
_Static_assert(sizeof(struct fn_net) == 96, "fn_net is 96 bytes");
_Static_assert(sizeof(struct fn_hdr) == 64, "the pack's header is 64 bytes");

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int fn_pack_check(const uint8_t *pak, size_t len, int check_digest)
{
    static const char m[8] = FN_MAGIC;
    if (!pak || len < 64 || ((uintptr_t)pak & 15)) return -1;
    for (int i = 0; i < 8; i++)
        if (pak[i] != (uint8_t)m[i]) return -1;
    if (rd32(pak + 8) != 1 || rd32(pak + 12) != 2 || rd32(pak + 16) != len) return -2;
    for (int i = 0; i < 2; i++) {
        uint32_t o = rd32(pak + 20 + 4 * i);
        if (o & 15 || o + sizeof(struct fn_net) > len) return -2;
        const struct fn_net *n = (const struct fn_net *)(pak + o);
        if (!n->nops || n->nops > FN_MAX_OPS || !n->nslots || n->nslots > FN_MAX_SLOTS || n->nout > 16 ||
            o + sizeof(struct fn_net) + n->nops * sizeof(struct fn_op) > len)
            return -2;
    }
    if (check_digest) {
        uint8_t d[32];
        pass_sha256(pak + 64, (uint32_t)(len - 64), d);
        int bad = 0;
        for (int i = 0; i < 32; i++) bad |= d[i] ^ pak[32 + i];
        if (bad) return -3;
    }
    return 0;
}

/* ---- sizes ------------------------------------------------------------------------------------------------- */
static unsigned out_h(const struct fn_op *o, unsigned h)
{
    if (o->type == FN_OP_POOL) return h / 2;
    if ((o->type == FN_OP_CONV3 || o->type == FN_OP_DW) && o->stride == 2) return (h + 1) / 2;
    if (o->type == FN_OP_FC) return 1;
    return h;
}
static size_t rows8(size_t p) { return (p + 7) & ~(size_t)7; }       /* PW's tile: slots hold whole 8-row groups */

int fn_open(struct fn_ctx *c, const uint8_t *pak, int net, unsigned h, unsigned w, const struct fn_plat *plat)
{
    for (size_t i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
    if (net < 0 || net > 1 || !plat || !plat->alloc || !h || !w) return -1;
    c->pak = pak;
    c->net = (const struct fn_net *)(pak + rd32(pak + 20 + 4 * net));
    c->ops = (const struct fn_op *)(c->net + 1);
    c->plat = plat;
    c->h = h;
    c->w = w;
    /* each slot's largest tensor, the widest 1 x 1 layer's input (the pool's tiles) */
    size_t need[FN_MAX_SLOTS] = { 0 }, kmax = 4;
    unsigned sh[FN_MAX_SLOTS] = { 0 }, sw[FN_MAX_SLOTS] = { 0 }, scp[FN_MAX_SLOTS] = { 0 };
    sh[0] = h;
    sw[0] = w;
    scp[0] = 4;
    need[0] = rows8((size_t)h * w) * 4;
    for (unsigned i = 0; i < c->net->nops; i++) {
        const struct fn_op *o = &c->ops[i];
        if (o->in >= c->net->nslots || o->out >= c->net->nslots || o->out == 0 ||
            (o->type == FN_OP_UPADD && o->in2 >= c->net->nslots))
            return -2;
        unsigned ih = sh[o->in], iw = sw[o->in];
        if (!ih || !iw || scp[o->in] != o->cinp) return -2;
        unsigned oh = out_h(o, ih), ow = out_h(o, iw);
        if (o->type == FN_OP_UPADD && (sh[o->in2] * 2 != ih || sw[o->in2] * 2 != iw || scp[o->in2] != o->cinp))
            return -2;
        if (o->type == FN_OP_FC && (size_t)ih * iw != o->hw) return -2;
        size_t n = rows8((size_t)oh * ow) * o->coutp;
        if (n > need[o->out]) need[o->out] = n;
        if (o->type == FN_OP_PW && o->cinp > kmax) kmax = o->cinp;
        if (o->type == FN_OP_FC && (o->wbits == 4 ? ((size_t)o->hw * o->cinp) % FN_FC_GROUP : 0)) return -2;
        sh[o->out] = oh;
        sw[o->out] = ow;
        scp[o->out] = o->coutp;
        if (o->type == FN_OP_PW || o->type == FN_OP_FC) c->macs += (uint64_t)oh * ow * o->cinp * o->coutp * (o->type == FN_OP_FC ? o->hw : 1);
        else if (o->type == FN_OP_DW) c->macs += (uint64_t)oh * ow * o->coutp * 9;
        else if (o->type == FN_OP_CONV3) c->macs += (uint64_t)oh * ow * o->coutp * 9 * o->cin;
    }
    size_t tile = (kmax * 8 * 4 + 63) & ~(size_t)63, total = tile * FN_POOL + 64;
    for (unsigned s = 0; s < c->net->nslots; s++) total += (need[s] * 4 + 63) & ~(size_t)63;
    uint8_t *a = plat->alloc(total + 64);
    if (!a) return -3;
    c->arena = a;
    c->arena_bytes = total + 64;
    uint8_t *p = (uint8_t *)(((uintptr_t)a + 63) & ~(uintptr_t)63);
    for (unsigned s = 0; s < c->net->nslots; s++) {
        c->slot[s] = (float *)p;
        p += (need[s] * 4 + 63) & ~(size_t)63;
    }
    for (int i = 0; i < FN_POOL; i++) c->pool[i] = (float *)(p + (size_t)i * tile);
    c->sh[0] = h;
    c->sw[0] = w;
    return 0;
}
float *fn_input(struct fn_ctx *c) { return c->slot[0]; }
void fn_restart(struct fn_ctx *c) { c->next = 0; }
void fn_close(struct fn_ctx *c)
{
    if (c->arena && c->plat && c->plat->free) c->plat->free(c->arena);
    c->arena = 0;
}
const float *fn_output(const struct fn_ctx *c, int i, unsigned *h, unsigned *w, unsigned *cp)
{
    if (i < 0 || (unsigned)i >= c->net->nout) return 0;
    unsigned s = c->net->outs[i];
    /* the op that last wrote this slot gives the channels */
    unsigned coutp = 4;
    for (unsigned k = 0; k < c->net->nops; k++)
        if (c->ops[k].out == s) coutp = c->ops[k].coutp;
    if (h) *h = c->sh[s];
    if (w) *w = c->sw[s];
    if (cp) *cp = coutp;
    return c->slot[s];
}

/* ---- kernels ----------------------------------------------------------------------------------------------- */
struct job {
    const struct fn_ctx *c;
    const struct fn_op *o;
    const float *in, *in2;
    float *out;
    unsigned ih, iw, oh, ow;
    unsigned chunk;                /* rows (or pixel groups) a job */
};

static inline v4f act4(v4f x, int act, const float *slope)
{
    if (act == FN_ACT_RELU) {
        v4f z = { 0, 0, 0, 0 };
        return vmax(x, z);
    }
    if (act == FN_ACT_PRELU) {
        v4f s = *(const v4f *)slope, z = { 0, 0, 0, 0 };
        return vsel(x > z, x, x * s);
    }
    return x;
}

/* full 3 x 3 conv, pad 1: input cinp channels (cin real), weights [3][3][cin][coutp]; one output row a job */
static void k_conv3(void *arg, int y)
{
    const struct job *j = arg;
    const struct fn_op *o = j->o;
    const float *wt = (const float *)(j->c->pak + o->w), *bias = (const float *)(j->c->pak + o->b);
    const float *slope = o->a ? (const float *)(j->c->pak + o->a) : 0;
    unsigned cop = o->coutp, cip = o->cinp, s = o->stride;
    for (unsigned x = 0; x < j->ow; x++) {
        float *dst = j->out + ((size_t)y * j->ow + x) * cop;
        for (unsigned co = 0; co < cop; co += 4) {
            v4f acc = *(const v4f *)(bias + co);
            for (int ky = 0; ky < 3; ky++) {
                int iy = (int)(y * s) + ky - 1;
                if (iy < 0 || iy >= (int)j->ih) continue;
                for (int kx = 0; kx < 3; kx++) {
                    int ix = (int)(x * s) + kx - 1;
                    if (ix < 0 || ix >= (int)j->iw) continue;
                    const float *px = j->in + ((size_t)iy * j->iw + ix) * cip;
                    const float *wk = wt + (size_t)((ky * 3 + kx) * o->cin) * cop + co;
                    for (unsigned ci = 0; ci < o->cin; ci++) acc += px[ci] * *(const v4f *)(wk + (size_t)ci * cop);
                }
            }
            *(v4f *)(dst + co) = act4(acc, o->act, slope ? slope + co : 0);
        }
    }
}

/* depthwise 3 x 3, pad 1; one output row a job */
static void k_dw(void *arg, int y)
{
    const struct job *j = arg;
    const struct fn_op *o = j->o;
    const float *wt = (const float *)(j->c->pak + o->w), *bias = (const float *)(j->c->pak + o->b);
    const float *slope = o->a ? (const float *)(j->c->pak + o->a) : 0;
    unsigned cp = o->coutp, s = o->stride;
    for (unsigned x = 0; x < j->ow; x++) {
        float *dst = j->out + ((size_t)y * j->ow + x) * cp;
        int y0 = (int)(y * s) - 1, x0 = (int)(x * s) - 1;
        int inside = y0 >= 0 && y0 + 2 < (int)j->ih && x0 >= 0 && x0 + 2 < (int)j->iw;
        for (unsigned c = 0; c < cp; c += 4) {
            v4f acc = *(const v4f *)(bias + c);
            if (inside) {
                const float *p0 = j->in + ((size_t)y0 * j->iw + x0) * cp + c;
                size_t rs = (size_t)j->iw * cp;
                for (int ky = 0; ky < 3; ky++)
                    for (int kx = 0; kx < 3; kx++)
                        acc += *(const v4f *)(p0 + ky * rs + (size_t)kx * cp) * *(const v4f *)(wt + (size_t)(ky * 3 + kx) * cp + c);
            } else {
                for (int ky = 0; ky < 3; ky++) {
                    int iy = y0 + ky;
                    if (iy < 0 || iy >= (int)j->ih) continue;
                    for (int kx = 0; kx < 3; kx++) {
                        int ix = x0 + kx;
                        if (ix < 0 || ix >= (int)j->iw) continue;
                        acc += *(const v4f *)(j->in + ((size_t)iy * j->iw + ix) * cp + c) *
                               *(const v4f *)(wt + (size_t)(ky * 3 + kx) * cp + c);
                    }
                }
            }
            *(v4f *)(dst + c) = act4(acc, o->act, slope ? slope + c : 0);
        }
    }
}

/* 1 x 1 conv: one tile of tw output channels of the layer's weights [coutp][cinp] (int8 with a scale per channel, or
 * float when ws is 0) as [cinp][tw] floats */
static void dequant_tile(const struct job *j, unsigned n0, unsigned tw, float *dst)
{
    const struct fn_op *o = j->o;
    unsigned K = o->cinp;
    if (!o->ws) {                       /* float weights (YuNet's small layers) */
        const float *fw = (const float *)(j->c->pak + o->w);
        for (unsigned t = 0; t < tw; t++) {
            const float *row = fw + (size_t)(n0 + t) * K;
            for (unsigned k = 0; k < K; k++) dst[(size_t)k * tw + t] = row[k];
        }
        return;
    }
    const int8_t *q = (const int8_t *)(j->c->pak + o->w);
    const float *sc = (const float *)(j->c->pak + o->ws);
    for (unsigned t = 0; t < tw; t++) {
        const int8_t *row = q + (size_t)(n0 + t) * K;
        float s = sc[n0 + t];
        for (unsigned k = 0; k < K; k++) dst[(size_t)k * tw + t] = (float)row[k] * s;
    }
}
static float *pool_get(struct fn_ctx *c, int *slot)
{
    for (;;)
        for (int i = 0; i < FN_POOL; i++)
            if (!__atomic_exchange_n(&c->pool_busy[i], 1, __ATOMIC_ACQUIRE)) {
                *slot = i;
                return c->pool[i];
            }
}
static void pool_put(struct fn_ctx *c, int slot) { __atomic_store_n(&c->pool_busy[slot], 0, __ATOMIC_RELEASE); }

/* the GEMM: a job is one tile of 8 (or 4) output channels x one chunk of pixels (rows of the NHWC input), in
 * register tiles of 8 pixels x 8 channels */
static void k_pw(void *arg, int ji)
{
    const struct job *j = arg;
    const struct fn_op *o = j->o;
    struct fn_ctx *c = (struct fn_ctx *)j->c;
    const float *bias = (const float *)(c->pak + o->b);
    const float *slope = o->a ? (const float *)(c->pak + o->a) : 0;
    unsigned K = o->cinp, N = o->coutp, tw = N % 8 ? 4 : 8, ntiles = N / tw;
    unsigned n0 = ((unsigned)ji % ntiles) * tw;
    size_t P = rows8((size_t)j->oh * j->ow), p0 = (size_t)((unsigned)ji / ntiles) * j->chunk, p1 = p0 + j->chunk;
    if (p1 > P) p1 = P;
    int ps;
    float *b = pool_get(c, &ps);
    dequant_tile(j, n0, tw, b);
    for (size_t p = p0; p < p1; p += 8) {
        const float *a = j->in + p * K;
        float *d = j->out + p * N;
        if (tw == 8) {
            v4f b0i = *(const v4f *)(bias + n0), b1i = *(const v4f *)(bias + n0 + 4);
            v4f c00 = b0i, c01 = b1i, c10 = b0i, c11 = b1i, c20 = b0i, c21 = b1i, c30 = b0i, c31 = b1i;
            v4f c40 = b0i, c41 = b1i, c50 = b0i, c51 = b1i, c60 = b0i, c61 = b1i, c70 = b0i, c71 = b1i;
            for (unsigned k = 0; k < K; k += 4) {
                v4f a0 = *(const v4f *)(a + k), a1 = *(const v4f *)(a + K + k), a2 = *(const v4f *)(a + 2 * K + k),
                    a3 = *(const v4f *)(a + 3 * K + k), a4 = *(const v4f *)(a + 4 * K + k),
                    a5 = *(const v4f *)(a + 5 * K + k), a6 = *(const v4f *)(a + 6 * K + k),
                    a7 = *(const v4f *)(a + 7 * K + k);
#define FN_STEP(L)                                                                                       \
    do {                                                                                                 \
        v4f w0 = *(const v4f *)(b + (size_t)(k + L) * 8), w1 = *(const v4f *)(b + (size_t)(k + L) * 8 + 4); \
        c00 += w0 * a0[L]; c01 += w1 * a0[L]; c10 += w0 * a1[L]; c11 += w1 * a1[L];                   \
        c20 += w0 * a2[L]; c21 += w1 * a2[L]; c30 += w0 * a3[L]; c31 += w1 * a3[L];                   \
        c40 += w0 * a4[L]; c41 += w1 * a4[L]; c50 += w0 * a5[L]; c51 += w1 * a5[L];                   \
        c60 += w0 * a6[L]; c61 += w1 * a6[L]; c70 += w0 * a7[L]; c71 += w1 * a7[L];                   \
    } while (0)
                FN_STEP(0); FN_STEP(1); FN_STEP(2); FN_STEP(3);
#undef FN_STEP
            }
            const float *s0 = slope ? slope + n0 : 0, *s1 = slope ? slope + n0 + 4 : 0;
            *(v4f *)(d + n0) = act4(c00, o->act, s0); *(v4f *)(d + n0 + 4) = act4(c01, o->act, s1);
            *(v4f *)(d + N + n0) = act4(c10, o->act, s0); *(v4f *)(d + N + n0 + 4) = act4(c11, o->act, s1);
            *(v4f *)(d + 2 * N + n0) = act4(c20, o->act, s0); *(v4f *)(d + 2 * N + n0 + 4) = act4(c21, o->act, s1);
            *(v4f *)(d + 3 * N + n0) = act4(c30, o->act, s0); *(v4f *)(d + 3 * N + n0 + 4) = act4(c31, o->act, s1);
            *(v4f *)(d + 4 * N + n0) = act4(c40, o->act, s0); *(v4f *)(d + 4 * N + n0 + 4) = act4(c41, o->act, s1);
            *(v4f *)(d + 5 * N + n0) = act4(c50, o->act, s0); *(v4f *)(d + 5 * N + n0 + 4) = act4(c51, o->act, s1);
            *(v4f *)(d + 6 * N + n0) = act4(c60, o->act, s0); *(v4f *)(d + 6 * N + n0 + 4) = act4(c61, o->act, s1);
            *(v4f *)(d + 7 * N + n0) = act4(c70, o->act, s0); *(v4f *)(d + 7 * N + n0 + 4) = act4(c71, o->act, s1);
        } else {
            for (unsigned r = 0; r < 8; r++) {
                v4f acc = *(const v4f *)(bias + n0);
                const float *ar = a + (size_t)r * K;
                for (unsigned k = 0; k < K; k++) acc += *(const v4f *)(b + (size_t)k * 4) * ar[k];
                *(v4f *)(d + (size_t)r * N + n0) = act4(acc, o->act, slope ? slope + n0 : 0);
            }
        }
    }
    pool_put(c, ps);
}

static void k_pool(void *arg, int y)
{
    const struct job *j = arg;
    unsigned cp = j->o->coutp;
    for (unsigned x = 0; x < j->ow; x++) {
        const float *p = j->in + ((size_t)(2 * y) * j->iw + 2 * x) * cp;
        size_t rs = (size_t)j->iw * cp;
        float *d = j->out + ((size_t)y * j->ow + x) * cp;
        for (unsigned c = 0; c < cp; c += 4) {
            v4f m = *(const v4f *)(p + c), t = *(const v4f *)(p + cp + c);
            m = vmax(m, t);
            t = *(const v4f *)(p + rs + c);
            m = vmax(m, t);
            t = *(const v4f *)(p + rs + cp + c);
            m = vmax(m, t);
            *(v4f *)(d + c) = m;
        }
    }
}

static void k_upadd(void *arg, int y)
{
    const struct job *j = arg;
    unsigned cp = j->o->coutp, w2 = j->iw / 2;
    for (unsigned x = 0; x < j->ow; x++) {
        const float *a = j->in + ((size_t)y * j->iw + x) * cp, *b = j->in2 + ((size_t)(y / 2) * w2 + x / 2) * cp;
        float *d = j->out + ((size_t)y * j->ow + x) * cp;
        for (unsigned c = 0; c < cp; c += 4) *(v4f *)(d + c) = *(const v4f *)(a + c) + *(const v4f *)(b + c);
    }
}

/* fully connected on 4-bit weights [coutp][hw * cinp / 2] (NHWC order, a scale per FN_FC_GROUP inputs) or int8 with
 * a scale per row; one output a job */
static void k_fc(void *arg, int n)
{
    const struct job *j = arg;
    const struct fn_op *o = j->o;
    size_t K = (size_t)o->hw * o->cinp;
    const float *sc = (const float *)(j->c->pak + o->ws), *bias = (const float *)(j->c->pak + o->b);
    float sum = 0;
    if (o->wbits == 4) {
        const uint8_t *q = j->c->pak + o->w + (size_t)n * (K / 2);
        const float *gs = sc + (size_t)n * (K / FN_FC_GROUP);
        for (size_t g = 0; g < K / FN_FC_GROUP; g++) {
            v4f acc = { 0, 0, 0, 0 };
            const uint8_t *qg = q + g * (FN_FC_GROUP / 2);
            const float *x = j->in + g * FN_FC_GROUP;
            for (unsigned k = 0; k < FN_FC_GROUP; k += 4) {
                uint8_t b0 = qg[k / 2], b1 = qg[k / 2 + 1];
                /* signed nibbles: shift up to the top of a byte, then arithmetically down */
                v4f wv = { (float)((int8_t)(b0 << 4) >> 4), (float)((int8_t)b0 >> 4),
                           (float)((int8_t)(b1 << 4) >> 4), (float)((int8_t)b1 >> 4) };
                acc += wv * *(const v4f *)(x + k);
            }
            sum += (acc[0] + acc[1] + acc[2] + acc[3]) * gs[g];
        }
    } else {
        const int8_t *q = (const int8_t *)(j->c->pak + o->w) + (size_t)n * K;
        v4f acc = { 0, 0, 0, 0 };
        for (size_t k = 0; k < K; k += 4) {
            v4f wv = { (float)q[k], (float)q[k + 1], (float)q[k + 2], (float)q[k + 3] };
            acc += wv * *(const v4f *)(j->in + k);
        }
        sum = (acc[0] + acc[1] + acc[2] + acc[3]) * sc[n];
    }
    j->out[n] = sum + bias[n];
}

static void par(const struct fn_ctx *c, void (*fn)(void *, int), void *arg, int n)
{
    if (c->plat->par && n > 1) c->plat->par(fn, arg, n);
    else
        for (int i = 0; i < n; i++) fn(arg, i);
}

static void run_op(struct fn_ctx *c, const struct fn_op *o)
{
    struct job j = { c, o, c->slot[o->in], o->type == FN_OP_UPADD ? c->slot[o->in2] : 0, c->slot[o->out],
                     c->sh[o->in], c->sw[o->in], 0, 0, 0 };
    j.oh = out_h(o, j.ih);
    j.ow = out_h(o, j.iw);
    switch (o->type) {
    case FN_OP_CONV3: par(c, k_conv3, &j, (int)j.oh); break;
    case FN_OP_DW: par(c, k_dw, &j, (int)j.oh); break;
    case FN_OP_POOL: par(c, k_pool, &j, (int)j.oh); break;
    case FN_OP_UPADD: par(c, k_upadd, &j, (int)j.oh); break;
    case FN_OP_FC: {
        float *d = j.out;
        for (unsigned i = o->cout; i < o->coutp; i++) d[i] = 0;
        par(c, k_fc, &j, (int)o->cout);
        break;
    }
    case FN_OP_PW: {
        unsigned tw = o->coutp % 8 ? 4 : 8, ntiles = o->coutp / tw;
        size_t P = rows8((size_t)j.oh * j.ow), groups = P / 8;
        /* >= ~16 jobs a layer (four cores, uneven finish), >= 4 groups of 8 pixels a job (the tile's dequantising
         * is then <= 1/32 of the job's work) */
        size_t chunks = (16 + ntiles - 1) / ntiles;
        if (chunks > groups / 4) chunks = groups / 4;
        if (!chunks) chunks = 1;
        size_t per = (groups + chunks - 1) / chunks;
        j.chunk = (unsigned)(per * 8);
        chunks = (P + j.chunk - 1) / j.chunk;
        par(c, k_pw, &j, (int)(ntiles * chunks));
        break;
    }
    }
    c->sh[o->out] = j.oh;
    c->sw[o->out] = j.ow;
}

int fn_run(struct fn_ctx *c, uint32_t budget_us)
{
    uint64_t t0 = c->plat->now_us && budget_us ? c->plat->now_us() : 0;
    while (c->next < c->net->nops) {
        uint64_t a = c->plat->now_us ? c->plat->now_us() : 0;
        run_op(c, &c->ops[c->next]);
        c->next++;
        if (c->plat->now_us) {
            uint64_t b = c->plat->now_us();
            if (b - a > c->op_us_max) c->op_us_max = (uint32_t)(b - a);
            /* stop when the next op of the same size would overrun */
            if (budget_us && b - t0 + c->op_us_max > budget_us) break;
        }
    }
    return c->next >= c->net->nops;
}
