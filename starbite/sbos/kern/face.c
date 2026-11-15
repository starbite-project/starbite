/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Face Unlock: the pipeline, the unlock and enrolment rules, the sealed template record (face.h).
 * Pure C, no libc. */
#include "face.h"
#include "passcode.h"

typedef float v4f __attribute__((vector_size(16)));

/* ---- small maths ------------------------------------------------------------------------------------------- */
float face_sqrtf(float x) { return x > 0 ? __builtin_sqrtf(x) : 0; }
static float fl(float x) { int i = (int)x; return (float)i > x ? (float)(i - 1) : (float)i; }
float face_expf(float x)
{
    if (x > 80) x = 80;
    if (x < -80) return 0;
    float t = x * 1.44269504f, n = fl(t), f = (t - n) * 0.69314718f;     /* e^x = 2^n e^f, 0 <= f < ln 2 */
    float p = 1 + f * (1 + f * (0.5f + f * (1.f / 6 + f * (1.f / 24 + f * (1.f / 120 + f * (1.f / 720))))));
    union { float f; uint32_t i; } u;
    u.i = (uint32_t)((int)n + 127) << 23;              /* 2^n, n in -116..116 */
    return p * u.f;
}
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static void zero(void *p, size_t n) { for (size_t i = 0; i < n; i++) ((volatile uint8_t *)p)[i] = 0; }
static void copy(void *d, const void *s, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *)d)[i] = ((const uint8_t *)s)[i]; }

float face_cos(const float *a, const float *b)
{
    float ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < FACE_DIM; i++) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return aa > 0 && bb > 0 ? ab / face_sqrtf(aa * bb) : 0;
}

/* ---- detection input / decode -------------------------------------------------------------------------------- */
void face_det_input(float *dst, unsigned dh, unsigned dw, const uint32_t *px, unsigned w, unsigned h, unsigned stride,
                    unsigned s)
{
    unsigned sh = h / s, sw = w / s;
    float inv = 1.0f / (float)(s * s);
    for (unsigned y = 0; y < dh; y++)
        for (unsigned x = 0; x < dw; x++) {
            float *d = dst + ((size_t)y * dw + x) * 4;
            if (y >= sh || x >= sw) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            uint32_t r = 0, g = 0, b = 0;
            for (unsigned yy = 0; yy < s; yy++) {
                const uint32_t *row = px + (size_t)(y * s + yy) * stride + x * s;
                for (unsigned xx = 0; xx < s; xx++) {
                    uint32_t v = row[xx];
                    r += v >> 16 & 255; g += v >> 8 & 255; b += v & 255;
                }
            }
            d[0] = (float)b * inv; d[1] = (float)g * inv; d[2] = (float)r * inv; d[3] = 0;
        }
}

static float sigm(float x) { return 1.0f / (1.0f + face_expf(-x)); }

int face_det_decode(const struct fn_ctx *yn, float thr, struct face_det *best)
{
    static const unsigned strides[3] = { 8, 16, 32 };
    int n = 0;
    float bs = -1;
    for (int i = 0; i < 3; i++) {
        unsigned rows, cols, cpc, cpo, cpb, cpk;
        const float *cls = fn_output(yn, i, &rows, &cols, &cpc), *obj = fn_output(yn, 3 + i, 0, 0, &cpo);
        const float *box = fn_output(yn, 6 + i, 0, 0, &cpb), *kps = fn_output(yn, 9 + i, 0, 0, &cpk);
        if (!cls || !obj || !box || !kps) return 0;
        float st = (float)strides[i];
        for (unsigned r = 0; r < rows; r++)
            for (unsigned c = 0; c < cols; c++) {
                size_t k = (size_t)r * cols + c;
                float sc = face_sqrtf(clampf(sigm(cls[k * cpc]), 0, 1) * clampf(sigm(obj[k * cpo]), 0, 1));
                if (sc < thr) continue;
                n++;
                if (sc <= bs) continue;
                bs = sc;
                const float *b = box + k * cpb, *p = kps + k * cpk;
                float cx = ((float)c + b[0]) * st, cy = ((float)r + b[1]) * st;
                float w = face_expf(b[2]) * st, h = face_expf(b[3]) * st;
                best->x = cx - w / 2; best->y = cy - h / 2; best->w = w; best->h = h; best->score = sc;
                for (int j = 0; j < 5; j++) {
                    best->pt[2 * j] = (p[2 * j] + (float)c) * st;
                    best->pt[2 * j + 1] = (p[2 * j + 1] + (float)r) * st;
                }
            }
    }
    return n > 0;                       /* the best face only: NMS would keep it too */
}

/* ---- alignment ----------------------------------------------------------------------------------------------- */
static const float TPL[10] = { 38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f };

/* the least-squares similarity dst = a * src + t (points as complex numbers; as Umeyama's without a reflection) */
static void sim_fit(const float *src, const float *dst, float m[6])
{
    float sx = 0, sy = 0, dx = 0, dy = 0;
    for (int i = 0; i < 5; i++) { sx += src[2 * i]; sy += src[2 * i + 1]; dx += dst[2 * i]; dy += dst[2 * i + 1]; }
    sx /= 5; sy /= 5; dx /= 5; dy /= 5;
    float num_r = 0, num_i = 0, den = 0;
    for (int i = 0; i < 5; i++) {
        float ax = src[2 * i] - sx, ay = src[2 * i + 1] - sy, bx = dst[2 * i] - dx, by = dst[2 * i + 1] - dy;
        num_r += ax * bx + ay * by;           /* conj(a) * b */
        num_i += ax * by - ay * bx;
        den += ax * ax + ay * ay;
    }
    float ar = den > 0 ? num_r / den : 1, ai = den > 0 ? num_i / den : 0;
    m[0] = ar; m[1] = -ai; m[2] = dx - (ar * sx - ai * sy);
    m[3] = ai; m[4] = ar;  m[5] = dy - (ai * sx + ar * sy);
}

void face_align(float *dst, const uint32_t *px, unsigned w, unsigned h, unsigned stride, const struct face_det *d,
                float mean, float scale)
{
    float m[6];
    sim_fit(d->pt, TPL, m);
    /* the inverse: src = A^-1 (dst - t) */
    float det = m[0] * m[4] - m[1] * m[3];
    if (det == 0) det = 1;
    float i0 = m[4] / det, i1 = -m[1] / det, i3 = -m[3] / det, i4 = m[0] / det;
    for (int y = 0; y < 112; y++)
        for (int x = 0; x < 112; x++) {
            float ux = (float)x - m[2], uy = (float)y - m[5];
            float sx = i0 * ux + i1 * uy, sy = i3 * ux + i4 * uy;
            float *o = dst + ((size_t)y * 112 + x) * 4;
            float acc[3] = { 0, 0, 0 };
            float fx = fl(sx), fy = fl(sy);
            int x0 = (int)fx, y0 = (int)fy;
            float ax = sx - fx, ay = sy - fy;
            for (int k = 0; k < 4; k++) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                float wk = (k & 1 ? ax : 1 - ax) * (k >> 1 ? ay : 1 - ay);
                if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h) continue;   /* outside: black, as OpenCV */
                uint32_t v = px[(size_t)yy * stride + (unsigned)xx];
                acc[0] += wk * (float)(v >> 16 & 255);
                acc[1] += wk * (float)(v >> 8 & 255);
                acc[2] += wk * (float)(v & 255);
            }
            o[0] = (acc[0] - mean) * scale;      /* R G B: SFace's order */
            o[1] = (acc[1] - mean) * scale;
            o[2] = (acc[2] - mean) * scale;
            o[3] = 0;
        }
}

/* ---- liveness / pose signals --------------------------------------------------------------------------------- */
static unsigned patch(const uint8_t *g, unsigned w, unsigned h, float cx, float cy, float ax, float ay,
                      uint32_t hist[16], uint32_t *sum)
{
    unsigned n = 0;
    int x0 = (int)(cx - ax), x1 = (int)(cx + ax), y0 = (int)(cy - ay), y1 = (int)(cy + ay);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int)w) x1 = (int)w - 1;
    if (y1 >= (int)h) y1 = (int)h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t v = g[(size_t)y * w + (unsigned)x];
            if (hist) hist[v >> 4]++;
            *sum += v;
            n++;
        }
    return n;
}
/* the mean of the darkest quarter, from a 16-bin histogram (bins taken whole, the last one in part) */
static float dark_quarter(const uint32_t hist[16], unsigned n)
{
    unsigned want = n / 4, got = 0;
    float s = 0;
    for (int b = 0; b < 16 && got < want; b++) {
        unsigned t = hist[b] < want - got ? hist[b] : want - got;
        s += (float)t * ((float)b * 16 + 8);
        got += t;
    }
    return got ? s / (float)got : 0;
}

void face_signals(const struct face_det *d, const uint8_t *grey, unsigned w, unsigned h, struct face_sig *s)
{
    float rx = d->pt[0], ry = d->pt[1], lx = d->pt[2], ly = d->pt[3], nx = d->pt[4], ny = d->pt[5];
    float ux = lx - rx, uy = ly - ry, dd = face_sqrtf(ux * ux + uy * uy);
    zero(s, sizeof *s);
    if (dd < 4) return;
    ux /= dd; uy /= dd;
    float vx = nx - (rx + lx) / 2, vy = ny - (ry + ly) / 2;
    s->yaw = (vx * ux + vy * uy) / dd;
    s->pitch = (vx * -uy + vy * ux) / dd;     /* the eye line turned a quarter towards the chin */
    s->size = dd / (float)w;
    float eye = 0;
    for (int e = 0; e < 2; e++) {
        float cx = d->pt[2 * e], cy = d->pt[2 * e + 1];
        uint32_t hist[16] = { 0 }, sum = 0, skin = 0;
        unsigned n = patch(grey, w, h, cx, cy, 0.18f * dd, 0.10f * dd, hist, &sum);
        unsigned m = patch(grey, w, h, cx - uy * 0.32f * dd, cy + ux * 0.32f * dd, 0.14f * dd, 0.06f * dd, 0, &skin);
        if (!n || !m) continue;
        float dk = dark_quarter(hist, n), sk = (float)skin / (float)m;
        eye += clampf((sk - dk) / (sk + 1), 0, 1);
    }
    s->eye = eye / 2;
    uint32_t sum = 0;
    unsigned n = 0;
    for (int y = (int)d->y; y < (int)(d->y + d->h); y += 4)
        for (int x = (int)d->x; x < (int)(d->x + d->w); x += 4)
            if (x >= 0 && y >= 0 && x < (int)w && y < (int)h) { sum += grey[(size_t)y * w + (unsigned)x]; n++; }
    s->bright = n ? (float)sum / (float)n : 0;
}

/* ---- the engine ---------------------------------------------------------------------------------------------- */
#define DET_THR 0.6f

int face_eng_open(struct face_eng *e, const uint8_t *pak, size_t pak_len, const struct fn_plat *plat)
{
    zero(e, sizeof *e);
    if (fn_pack_check(pak, pak_len, 0) < 0) return -1;
    e->pak = pak;
    e->plat = plat;
    e->fw = FACE_FRAME_W;
    e->fh = FACE_FRAME_H;
    unsigned dh = (e->fh / FACE_DET_SCALE + 31) / 32 * 32, dw = (e->fw / FACE_DET_SCALE + 31) / 32 * 32;
    if (fn_open(&e->yn, pak, FN_NET_YUNET, dh, dw, plat) < 0) return -2;
    if (fn_open(&e->sf, pak, FN_NET_SFACE, 112, 112, plat) < 0) { fn_close(&e->yn); return -2; }
    e->frame = plat->alloc((size_t)e->fw * e->fh * 4);
    e->grey = plat->alloc((size_t)e->fw * e->fh);
    if (!e->frame || !e->grey) { face_eng_close(e); return -3; }
    return 0;
}
void face_eng_close(struct face_eng *e)
{
    fn_close(&e->yn);
    fn_close(&e->sf);
    if (e->plat && e->plat->free) {
        if (e->frame) e->plat->free(e->frame);
        if (e->grey) e->plat->free(e->grey);
    }
    e->frame = 0;
    e->grey = 0;
    zero(e->emb, sizeof e->emb);
}

int face_eng_feed(struct face_eng *e, const uint32_t *px, unsigned w, unsigned h, unsigned stride)
{
    if (e->det_busy || !e->frame || w != e->fw || h != e->fh) return -1;
    for (unsigned y = 0; y < h; y++) {
        const uint32_t *s = px + (size_t)y * stride;
        uint32_t *d = e->frame + (size_t)y * w;
        uint8_t *g = e->grey + (size_t)y * w;
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = s[x];
            d[x] = v;
            g[x] = (uint8_t)(((v >> 16 & 255) * 77 + (v >> 8 & 255) * 150 + (v & 255) * 29) >> 8);
        }
    }
    face_det_input(fn_input(&e->yn), e->yn.h, e->yn.w, e->frame, w, h, w, FACE_DET_SCALE);
    fn_restart(&e->yn);
    e->det_busy = 1;
    e->det_ready = 0;
    e->det_us = 0;
    return 0;
}

int face_eng_embed(struct face_eng *e)
{
    if (e->emb_busy || !e->det_ready || !e->nfaces) return -1;
    face_align(fn_input(&e->sf), e->frame, e->fw, e->fh, e->fw, &e->det, e->sf.net->mean, e->sf.net->scale);
    fn_restart(&e->sf);
    e->emb_busy = 1;
    e->emb_ready = 0;
    e->emb_us = 0;
    return 0;
}

static uint64_t now(const struct face_eng *e) { return e->plat->now_us ? e->plat->now_us() : 0; }

int face_eng_step(struct face_eng *e, uint32_t budget_us)
{
    uint64_t t0 = now(e);
    if (e->det_busy) {
        int done = fn_run(&e->yn, budget_us);
        uint32_t dt = (uint32_t)(now(e) - t0);
        e->det_us += dt;
        if (dt > e->step_us_max) e->step_us_max = dt;
        if (!done) return FE_BUSY;
        e->det_busy = 0;
        struct face_det d;
        zero(&d, sizeof d);
        e->nfaces = face_det_decode(&e->yn, DET_THR, &d);
        if (e->nfaces) {
            float s = (float)FACE_DET_SCALE;
            d.x *= s; d.y *= s; d.w *= s; d.h *= s;
            for (int i = 0; i < 10; i++) d.pt[i] *= s;
            e->det = d;
            face_signals(&e->det, e->grey, e->fw, e->fh, &e->sig);
        }
        e->det_ready = 1;
        return FE_DET;
    }
    if (e->emb_busy) {
        int done = fn_run(&e->sf, budget_us);
        uint32_t dt = (uint32_t)(now(e) - t0);
        e->emb_us += dt;
        if (dt > e->step_us_max) e->step_us_max = dt;
        if (!done) return FE_BUSY;
        e->emb_busy = 0;
        const float *o = fn_output(&e->sf, 0, 0, 0, 0);
        float n = 0;
        for (int i = 0; i < FACE_DIM; i++) n += o[i] * o[i];
        n = n > 0 ? 1.0f / face_sqrtf(n) : 0;
        for (int i = 0; i < FACE_DIM; i++) e->emb[i] = o[i] * n;
        e->emb_ready = 1;
        return FE_EMB;
    }
    return FE_IDLE;
}

/* ---- templates ------------------------------------------------------------------------------------------------ */
void face_tpl_put(struct face_tpl *t, int i, const float *emb)
{
    float n = 0;
    for (int k = 0; k < FACE_DIM; k++) n += emb[k] * emb[k];
    n = n > 0 ? 127.0f / face_sqrtf(n) : 0;
    for (int k = 0; k < FACE_DIM; k++) {
        float v = emb[k] * n;
        t->v[i][k] = (int8_t)(v >= 0 ? (int)(v + 0.5f) : -(int)(-v + 0.5f));
    }
}
float face_tpl_score(const struct face_tpl *t, const float *emb)
{
    float best = -1;
    for (int i = 0; i < t->n && i < FACE_NTPL; i++) {
        float tv[FACE_DIM];
        for (int k = 0; k < FACE_DIM; k++) tv[k] = (float)t->v[i][k];
        float c = face_cos(tv, emb);
        if (c > best) best = c;
    }
    return best;
}

/* ---- the record's crypto: HMAC-SHA256, a SHA-256 counter stream ----------------------------------------------- */
void face_hmac(const uint8_t *key, unsigned klen, const uint8_t *m, unsigned n, uint8_t out[32])
{
    uint8_t k[64], pad[64], ih[32];
    zero(k, sizeof k);
    if (klen > 64) pass_sha256(key, klen, k);
    else copy(k, key, klen);
    struct pass_sha c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    pass_sha_init(&c);
    pass_sha_update(&c, pad, 64);
    pass_sha_update(&c, m, n);
    pass_sha_final(&c, ih);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    pass_sha_init(&c);
    pass_sha_update(&c, pad, 64);
    pass_sha_update(&c, ih, 32);
    pass_sha_final(&c, out);
    zero(k, sizeof k);
    zero(pad, sizeof pad);
    zero(ih, sizeof ih);
    zero(&c, sizeof c);
}
static void subkey(const uint8_t *key, const char *label, uint8_t out[32])
{
    unsigned n = 0;
    while (label[n]) n++;
    face_hmac(key, 32, (const uint8_t *)label, n, out);
}
static void stream_xor(const uint8_t kenc[32], const uint8_t nonce[16], uint8_t *buf, unsigned n)
{
    uint8_t blk[52], ks[32];
    copy(blk, kenc, 32);
    copy(blk + 32, nonce, 16);
    for (uint32_t i = 0; i * 32 < n; i++) {
        blk[48] = (uint8_t)i; blk[49] = (uint8_t)(i >> 8); blk[50] = (uint8_t)(i >> 16); blk[51] = (uint8_t)(i >> 24);
        pass_sha256(blk, sizeof blk, ks);
        for (unsigned k = 0; k < 32 && i * 32 + k < n; k++) buf[i * 32 + k] ^= ks[k];
    }
    zero(blk, sizeof blk);
    zero(ks, sizeof ks);
}
static int ct_eq(const uint8_t *a, const uint8_t *b, unsigned n)
{
    uint8_t d = 0;
    for (unsigned i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

int face_tpl_seal(const struct face_tpl *t, const uint8_t key[FACE_KEY], const uint8_t nonce[16], uint8_t out[FACE_SEAL_LEN])
{
    uint8_t ke[32], km[32];
    subkey(key, "sbos.face.enc", ke);
    subkey(key, "sbos.face.mac", km);
    copy(out, nonce, 16);
    copy(out + 16, t, sizeof *t);
    stream_xor(ke, nonce, out + 16, sizeof *t);
    face_hmac(km, 32, out, 16 + sizeof *t, out + 16 + sizeof *t);
    zero(ke, 32);
    zero(km, 32);
    return 0;
}
int face_tpl_open(struct face_tpl *t, const uint8_t key[FACE_KEY], const uint8_t *in, unsigned n)
{
    if (n != FACE_SEAL_LEN) return -1;
    uint8_t ke[32], km[32], mac[32];
    subkey(key, "sbos.face.mac", km);
    face_hmac(km, 32, in, 16 + sizeof *t, mac);
    int ok = ct_eq(mac, in + 16 + sizeof *t, 32);
    zero(km, 32);
    if (!ok) return -1;
    subkey(key, "sbos.face.enc", ke);
    copy(t, in + 16, sizeof *t);
    stream_xor(ke, in, (uint8_t *)t, sizeof *t);
    zero(ke, 32);
    if (t->n > FACE_NTPL || t->mode > FACE_SECURITY) { zero(t, sizeof *t); return -1; }
    return 0;
}

/* the key from the digits: SHA-256 over salt | digits | label, then rounds of SHA-256(h | salt) */
static void kek(const char *digits, const uint8_t salt[16], uint32_t rounds, uint8_t out[32])
{
    uint8_t b[48 + 16];
    unsigned n = 0;
    copy(b, salt, 16);
    while (digits[n] && n < 16) { b[16 + n] = (uint8_t)digits[n]; n++; }
    copy(b + 16 + n, "sbos.face.kek", 13);
    pass_sha256(b, 16 + n + 13, out);
    for (uint32_t r = 0; r < rounds; r++) {
        copy(b, out, 32);
        copy(b + 32, salt, 16);
        pass_sha256(b, 48, out);
    }
    zero(b, sizeof b);
}
int face_key_wrap(const uint8_t key[FACE_KEY], const char *digits, const uint8_t salt[16], uint8_t out[FACE_WRAP_LEN])
{
    uint8_t k[32], ke[32], km[32], pad[32];
    kek(digits, salt, FACE_WRAP_ROUNDS, k);
    subkey(k, "sbos.face.wrap", ke);
    subkey(k, "sbos.face.wmac", km);
    face_hmac(ke, 32, salt, 16, pad);
    copy(out, salt, 16);
    out[16] = (uint8_t)FACE_WRAP_ROUNDS; out[17] = (uint8_t)(FACE_WRAP_ROUNDS >> 8);
    out[18] = (uint8_t)(FACE_WRAP_ROUNDS >> 16); out[19] = (uint8_t)(FACE_WRAP_ROUNDS >> 24);
    for (int i = 0; i < 32; i++) out[20 + i] = key[i] ^ pad[i];
    face_hmac(km, 32, out, 52, out + 52);
    zero(k, 32); zero(ke, 32); zero(km, 32); zero(pad, 32);
    return 0;
}
int face_key_unwrap(uint8_t key[FACE_KEY], const char *digits, const uint8_t *in, unsigned n)
{
    if (n != FACE_WRAP_LEN) return -1;
    uint32_t rounds = (uint32_t)in[16] | (uint32_t)in[17] << 8 | (uint32_t)in[18] << 16 | (uint32_t)in[19] << 24;
    if (rounds < 1000 || rounds > 1000000) return -1;
    uint8_t k[32], ke[32], km[32], pad[32], mac[32];
    kek(digits, in, rounds, k);
    subkey(k, "sbos.face.wmac", km);
    face_hmac(km, 32, in, 52, mac);
    int ok = ct_eq(mac, in + 52, 32);
    if (ok) {
        subkey(k, "sbos.face.wrap", ke);
        face_hmac(ke, 32, in, 16, pad);
        for (int i = 0; i < 32; i++) key[i] = in[20 + i] ^ pad[i];
    }
    zero(k, 32); zero(ke, 32); zero(km, 32); zero(pad, 32); zero(mac, 32);
    return ok ? 0 : -1;
}

/* ---- one unlock attempt ---------------------------------------------------------------------------------------- */
void face_scan_begin(struct face_scan *s, int mode, uint64_t now_ms)
{
    zero(s, sizeof *s);
    s->mode = mode;
    s->t0 = now_ms;
    s->best = -1;
    s->yaw_min = 9;
    s->yaw_max = -9;
}
/* the homography mapping 4 points a to b (8 unknowns, h33 = 1; Gaussian elimination with pivoting): 0, -1 degenerate */
static int homog4(const float *a, const float *b, float h[9])
{
    float m[8][9];
    for (int i = 0; i < 4; i++) {
        float x = a[2 * i], y = a[2 * i + 1], u = b[2 * i], v = b[2 * i + 1];
        float r0[9] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u }, r1[9] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
        for (int k = 0; k < 9; k++) { m[2 * i][k] = r0[k]; m[2 * i + 1][k] = r1[k]; }
    }
    for (int c = 0; c < 8; c++) {
        int p = c;
        for (int r = c + 1; r < 8; r++)
            if ((m[r][c] < 0 ? -m[r][c] : m[r][c]) > (m[p][c] < 0 ? -m[p][c] : m[p][c])) p = r;
        if (m[p][c] > -1e-9f && m[p][c] < 1e-9f) return -1;
        if (p != c)
            for (int k = 0; k < 9; k++) { float t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
        for (int r = 0; r < 8; r++) {
            if (r == c) continue;
            float f = m[r][c] / m[c][c];
            for (int k = c; k < 9; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int i = 0; i < 8; i++) h[i] = m[i][8] / m[i][i];
    h[8] = 1;
    return 0;
}
float face_parallax(const float ref[10], const float cur[10])
{
    /* the plane's points: both eyes, both mouth corners (0, 1, 3, 4); the nose (2) is the one off it */
    float a[8] = { ref[0], ref[1], ref[2], ref[3], ref[6], ref[7], ref[8], ref[9] };
    float b[8] = { cur[0], cur[1], cur[2], cur[3], cur[6], cur[7], cur[8], cur[9] };
    float h[9];
    if (homog4(a, b, h) < 0) return 0;
    float x = ref[4], y = ref[5], w = h[6] * x + h[7] * y + h[8];
    if (w > -1e-6f && w < 1e-6f) return 0;
    float px = (h[0] * x + h[1] * y + h[2]) / w, py = (h[3] * x + h[4] * y + h[5]) / w;
    float dx = cur[4] - px, dy = cur[5] - py, ex = cur[2] - cur[0], ey = cur[3] - cur[1];
    float e = face_sqrtf(ex * ex + ey * ey);
    return e > 1 ? face_sqrtf(dx * dx + dy * dy) / e : 0;
}
int face_scan_live(const struct face_scan *s)
{
    return (s->faces >= 2 && s->yaw_max - s->yaw_min >= FACE_YAW_TURN && s->par_max >= FACE_PARALLAX) || s->blink;
}
void face_scan_det(struct face_scan *s, int found, const struct face_det *d, const struct face_sig *g, uint64_t now_ms)
{
    if (s->result != FS_RUN) return;
    if (!found) return;
    if (!s->faces) {
        s->t_face = now_ms;
        copy(s->ref, d->pt, sizeof s->ref);
    } else {
        float p = face_parallax(s->ref, d->pt);
        if (p > s->par_max) s->par_max = p;
    }
    s->faces++;
    if (g->yaw < s->yaw_min) s->yaw_min = g->yaw;
    if (g->yaw > s->yaw_max) s->yaw_max = g->yaw;
    /* a blink: the eyes' openness falls below 55 % of the open baseline, then comes back above 80 % */
    float e = g->eye;
    if (s->faces <= 3 || e > s->eye_base) {
        if (e > s->eye_base) s->eye_base = e;
    }
    if (s->faces > 3 && s->eye_base > 0.08f) {
        if (e < 0.55f * s->eye_base) s->eye_min_after = e > 0 ? e : 0.0001f;
        else if (s->eye_min_after > 0 && e > 0.8f * s->eye_base) s->blink = 1;
    }
    s->hint = g->size < FACE_CLOSER_SIZE ? FS_HINT_CLOSER : g->bright < 35 ? FS_HINT_LIGHT : FS_HINT_NONE;
}
void face_scan_emb(struct face_scan *s, float score, uint64_t now_ms)
{
    (void)now_ms;
    if (s->result != FS_RUN) return;
    s->embeds++;
    if (score > s->best) s->best = score;
    if (s->mode == FACE_SPEED) {
        if (score >= FACE_THR_SPEED) s->result = FS_MATCH;
        return;
    }
    if (score >= FACE_THR_SECURITY) s->matches++;
}
int face_scan_tick(struct face_scan *s, uint64_t now_ms)
{
    if (s->result != FS_RUN) return s->result;
    if (s->mode == FACE_SECURITY) {
        if (s->matches >= 2 && face_scan_live(s)) return s->result = FS_MATCH;
        if (s->matches >= 1 && !face_scan_live(s) && now_ms - s->t_face > 800) s->hint = FS_HINT_TURN;
    }
    uint64_t lim = s->mode == FACE_SPEED ? FACE_SPEED_MS : FACE_SECURITY_MS;
    if (s->faces && s->embeds && now_ms - s->t_face >= lim) return s->result = FS_NOMATCH;
    if (now_ms - s->t0 >= lim + 1000) return s->result = s->faces ? FS_NOMATCH : FS_NOFACE;
    return FS_RUN;
}

/* ---- enrolment ------------------------------------------------------------------------------------------------- */
void face_enrol_begin(struct face_enrol *en)
{
    zero(en, sizeof *en);
    en->want = -1;
}
/* the pose of a detection: the centre first (roughly straight on: |yaw| < 0.10); the others relative to the centre's
 * own yaw and pitch (landmarks have a small bias per face, and "straight" is the person's own) */
static int pose_of(const struct face_enrol *en, const struct face_sig *g)
{
    if (!(en->have & 1)) return g->yaw > -0.10f && g->yaw < 0.10f ? FB_CENTRE : -1;
    float dy = g->yaw - en->yaw0, dp = g->pitch - en->pitch0;
    if (dy < -0.12f) return FB_LEFT;
    if (dy > 0.12f) return FB_RIGHT;
    if (dy > -0.08f && dy < 0.08f) {
        if (dp < -0.07f) return FB_UP;
        if (dp > 0.07f) return FB_DOWN;
        if (dy > -0.04f && dy < 0.04f && dp > -0.03f && dp < 0.03f) return FB_CENTRE;
    }
    return -1;
}
int face_enrol_det(struct face_enrol *en, const struct face_sig *g, float score)
{
    if (en->want >= 0) return -1;              /* one embedding at a time */
    if (g->size < FACE_CLOSER_SIZE) return -1; /* too far for a good template */
    int p = pose_of(en, g);
    if (p < 0) return -1;
    float q = score * (g->size < 0.15f ? g->size / 0.15f : 1);
    if ((en->have >> p & 1) && q <= en->q[p] + 0.02f) return -1;
    en->want = p;
    en->pend_pitch = g->pitch;
    en->pend_yaw = g->yaw;
    en->pend_q = q;
    en->pend_eye = g->eye;
    return p;
}
int face_enrol_emb(struct face_enrol *en, const float *emb)
{
    int p = en->want;
    en->want = -1;
    if (p < 0) return 0;
    if (p != FB_CENTRE || (en->have & 1)) {
        float tv[FACE_DIM];
        for (int k = 0; k < FACE_DIM; k++) tv[k] = (float)en->t.v[FB_CENTRE][k];
        if (face_cos(tv, emb) < 0.5f) return 0;   /* someone else, or a bad frame */
    }
    face_tpl_put(&en->t, p, emb);
    en->q[p] = en->pend_q;
    en->have |= 1 << p;
    if (p == FB_CENTRE) {
        if (!en->centre_n) { en->pitch0 = en->pend_pitch; en->yaw0 = en->pend_yaw; }   /* the first centre sets "straight" */
        en->centre_n++;
        en->eye_sum += en->pend_eye;
        en->eye_n++;
    }
    en->t.n = FACE_NTPL;                        /* the record always has five rows; missing ones copy the centre */
    for (int i = 0; i < FACE_NTPL; i++)
        if (!(en->have >> i & 1)) copy(en->t.v[i], en->t.v[FB_CENTRE], FACE_DIM);
    en->t.eye_ref = en->eye_n ? en->eye_sum / (float)en->eye_n : 0;
    return 1;
}
int face_enrol_done(const struct face_enrol *en) { return en->have == (1 << FACE_NTPL) - 1; }
int face_enrol_count(const struct face_enrol *en)
{
    int n = 0;
    for (int i = 0; i < FACE_NTPL; i++) n += en->have >> i & 1;
    return n;
}
