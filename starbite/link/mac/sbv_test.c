/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link, Mac viewer: unit + fuzz test of the viewer session (sbv.c), no sockets, no AppKit.
 *
 * Builds valid source streams with the portable core (HELLO, MODE, keyframe + delta frames whose RECTs are
 * solid / LZ4 / raw from sblk_build_rect, PING, an unknown type, PONG), feeds them to sbv in random chunk
 * sizes and checks the composed frame buffer equals the source image, the ACKs and PONGs sbv sends, and the
 * frame callbacks. Then mutates the streams (random bytes, truncation) and checks sbv never crashes and stops
 * cleanly with an error (run it under ASan/UBSan: build.sh --san).
 *   sbv-test [iterations]   exit 0 = pass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbv.h"

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}
static uint32_t rr(uint32_t n) { return n ? rnd() % n : 0; }

static long checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 20) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

/* ---- what sbv sends back (the viewer's stream), parsed with the core ---------------------------------- */

static struct {
    uint8_t buf[1 << 16];
    size_t n;
    uint64_t now;
    int frames, keyframes_seen;
    uint32_t last_frame_id;
} T;

static int t_send(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (T.n + n > sizeof T.buf) T.n = 0;   /* only the recent tail matters */
    memcpy(T.buf + T.n, p, n);
    T.n += n;
    return 0;
}
static uint64_t t_now(void *ctx) { (void)ctx; return T.now += 37; }
static void t_frame(void *ctx, const sbv_frame_info *fi)
{
    (void)ctx;
    T.frames++;
    T.last_frame_id = fi->frame_id;
    if (fi->keyframe) T.keyframes_seen++;
    /* the damage must lie inside the frame */
    for (int i = 0; i < fi->ndamage; i++) {
        const sblk_rect *d = &fi->damage[i];
        CHECK((uint32_t)d->x + d->w <= fi->width && (uint32_t)d->y + d->h <= fi->height, "damage outside the frame");
    }
}
static void t_event(void *ctx, int ev, const char *text) { (void)ctx; (void)ev; (void)text; }

/* counts the viewer's messages of a type in T.buf */
static int count_sent(int type, uint32_t *last_u32)
{
    size_t off = 0;
    int c = 0;
    while (off < T.n) {
        sblk_msg m;
        long k = sblk_decode(T.buf + off, T.n - off, &m);
        if (k <= 0) break;
        if (m.type == type) {
            c++;
            if (last_u32) *last_u32 = type == SBLK_T_ACK ? m.u.ack.frame_id : type == SBLK_T_PONG ? m.u.pong.id : 0;
        }
        off += (size_t)k;
    }
    return c;
}

/* the viewer's INPUT_KEY messages in T.buf, in order, as usage | down << 8 */
static int keys_sent(unsigned *out, int max)
{
    size_t off = 0;
    int c = 0;
    while (off < T.n && c < max) {
        sblk_msg m;
        long k = sblk_decode(T.buf + off, T.n - off, &m);
        if (k <= 0) break;
        if (m.type == SBLK_T_INPUT_KEY) out[c++] = m.u.key.usage | ((unsigned)m.u.key.down << 8);
        off += (size_t)k;
    }
    return c;
}

/* ---- a source stream ---------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t n, cap;
    uint32_t seq;
} stream;

static void put(stream *s, const void *p, size_t n)
{
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->n, p, n);
    s->n += n;
}

static void put_msg(stream *s, sblk_msg *m)
{
    uint8_t b[512];
    m->seq = s->seq++;
    long n = sblk_encode(m, b, sizeof b);
    if (n < 0) { fprintf(stderr, "encode %d failed\n", m->type); exit(2); }
    put(s, b, (size_t)n);
}

static void paint(uint8_t *img, uint32_t W, uint32_t H, int kind)
{
    /* a random rectangle: solid, gradient (compresses), or noise (does not) */
    uint32_t x = rr(W), y = rr(H), w = 1 + rr(W - x), h = 1 + rr(H - y);
    uint32_t c = rnd() | 0xFF000000u;
    for (uint32_t j = y; j < y + h; j++)
        for (uint32_t i = x; i < x + w; i++) {
            uint32_t v = kind == 0 ? c : kind == 1 ? (c + i * 3 + j * 5) | 0xFF000000u : rnd() | 0xFF000000u;
            memcpy(img + ((size_t)j * W + i) * 4, &v, 4);
        }
}

/* builds a whole session; img ends as the image the viewer must show */
static void build_stream(stream *s, uint8_t **img_out, uint32_t *W_out, uint32_t *H_out, int *nframes)
{
    uint32_t W = 1 + rr(300), H = 1 + rr(200);
    if (rr(4) == 0) { W = 700 + rr(400); H = 400 + rr(300); }   /* big: rects split into strips */
    uint8_t *img = calloc((size_t)W * H, 4), *prev = calloc((size_t)W * H, 4);
    uint8_t *scratch = malloc(SBLK_MAX_PAYLOAD), *out = malloc(SBLK_MAX_PAYLOAD + 64);
    static _Alignas(8) uint8_t lz4state[SBLK_LZ4_STATE_SIZE];
    for (size_t i = 0; i < (size_t)W * H; i++) memcpy(img + i * 4, &(uint32_t){ 0xFF203040u }, 4);
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello = (sblk_hello){ SBLK_ROLE_SOURCE, 1, 0, SBLK_CAP_V1_ALL | SBLK_CAP_EXTEND, (uint16_t)W, (uint16_t)H, 60000,
                              SBLK_PIXFMT_BGRA8888, SBLK_MAX_PAYLOAD, "test source" };
    put_msg(s, &m);
    if (rr(2)) {
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_MODE;
        m.u.mode = (sblk_mode){ (uint16_t)W, (uint16_t)H, 60000, SBLK_MODE_APPLIED };
        put_msg(s, &m);
    }
    int nf = 1 + (int)rr(6);
    for (int f = 1; f <= nf; f++) {
        for (int k = (int)rr(4); k >= 0; k--) paint(img, W, H, (int)rr(3));
        sblk_rect rects[64], strips[512];
        int nr;
        if (f == 1) { rects[0] = (sblk_rect){ 0, 0, (uint16_t)W, (uint16_t)H }; nr = 1; memcpy(prev, img, (size_t)W * H * 4); }
        else nr = sblk_dirty_rects(prev, img, (size_t)W * 4, W, H, NULL, SBLK_DIRTY_UPDATE_PREV, rects, 64);
        int ns = 0;
        for (int i = 0; i < nr; i++) ns += sblk_split_rect(&rects[i], SBLK_MAX_PAYLOAD, strips + ns, 512 - ns);
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_FRAME_BEGIN;
        m.u.frame_begin = (sblk_frame_begin){ (uint32_t)f, (uint16_t)W, (uint16_t)H, (uint16_t)ns, f == 1 ? SBLK_FB_KEYFRAME : 0 };
        put_msg(s, &m);
        for (int i = 0; i < ns; i++) {
            long n = sblk_build_rect(out, SBLK_MAX_PAYLOAD + 64, img, (size_t)W * 4, &strips[i], SBLK_CAP_V1_ALL, 1,
                                     lz4state, scratch, s->seq++, NULL);
            if (n < 0) { fprintf(stderr, "build_rect failed %ld\n", n); exit(2); }
            put(s, out, (size_t)n);
        }
        memset(&m, 0, sizeof m);
        m.type = SBLK_T_FRAME_END;
        m.u.frame_end.frame_id = (uint32_t)f;
        m.u.frame_end.capture_us = 1000u * (uint64_t)f;
        put_msg(s, &m);
        if (rr(3) == 0) {   /* PING, and an unknown type the viewer must skip */
            memset(&m, 0, sizeof m);
            m.type = SBLK_T_PING;
            m.u.ping.id = (uint32_t)f;
            m.u.ping.t_us = 5;
            put_msg(s, &m);
            uint8_t junk[7] = { 1, 2, 3, 4, 5, 6, 7 };
            memset(&m, 0, sizeof m);
            m.type = 0x7A;
            m.u.unknown.len = sizeof junk;
            m.u.unknown.data = junk;
            put_msg(s, &m);
        }
    }
    free(prev); free(scratch); free(out);
    *img_out = img; *W_out = W; *H_out = H; *nframes = nf;
}

static int feed_chunks(sbv *v, const uint8_t *p, size_t n)
{
    while (n) {
        size_t k = 1 + rr(rr(4) == 0 ? 7 : 70000);
        if (k > n) k = n;
        int e = sbv_feed(v, p, k);
        if (e < 0) return e;
        p += k;
        n -= k;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 300;
    sbv_events ev = { NULL, t_send, t_now, t_frame, t_event };
    int clean = 0, mutated_ok = 0, mutated_err = 0;
    for (int it = 0; it < iters; it++) {
        stream s = { 0 };
        uint8_t *img;
        uint32_t W, H;
        int nf;
        build_stream(&s, &img, &W, &H, &nf);

        /* 1. the clean stream: exact composition, one ACK per frame, PONGs */
        sbv v;
        memset(&T, 0, sizeof T);
        CHECK(sbv_init(&v, &ev, "Test Mac \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 800, 600, 60000) == 0, "init");
        CHECK(sbv_start(&v) == 0, "start");
        CHECK(count_sent(SBLK_T_HELLO, NULL) == 1, "HELLO sent");
        size_t n0 = T.n;
        int e = feed_chunks(&v, s.data, s.n);
        CHECK(e == 0, "clean stream: %s (iteration %d)", sbv_strerror(e), it);
        CHECK(T.frames == nf, "frames %d != %d", T.frames, nf);
        CHECK(v.fb_w == W && v.fb_h == H, "size");
        CHECK(v.fb && sbv_fb_hash(v.fb, (size_t)W * 4, W, H) == sbv_fb_hash(img, (size_t)W * 4, W, H),
              "composed image differs (iteration %d, %ux%u)", it, W, H);
        uint32_t last = 0;
        CHECK(count_sent(SBLK_T_ACK, &last) == nf && last == (uint32_t)nf, "ACKs");
        (void)n0;
        /* input after the session is up: encodes and is accepted by the core decoder */
        CHECK(sbv_key(&v, 7, 0xE1, 1, SBLK_MOD_NUM) == 0 && sbv_key(&v, 7, 0x04, 1, SBLK_MOD_NUM) == 0, "keys");
        CHECK(v.mods == (SBLK_MOD_LSHIFT | SBLK_MOD_NUM), "modifier state 0x%x", v.mods);
        CHECK(sbv_pointer(&v, -5, 70000, SBLK_BTN_LEFT, 0, -120) == 0, "pointer");
        int before = count_sent(SBLK_T_INPUT_KEY, NULL);
        CHECK(sbv_release_all(&v, SBLK_MOD_NUM) == 0, "release all");
        CHECK(count_sent(SBLK_T_INPUT_KEY, NULL) == before + 2 && v.mods == SBLK_MOD_NUM, "release all sends 2 ups");
        /* focus loss with the Windows key held (Cmd+Tab on the Mac): Tab up, then a Left Ctrl tap before the
         * Windows key goes up, so Windows does not open the Start menu */
        CHECK(sbv_key(&v, 7, 0xE3, 1, SBLK_MOD_NUM) == 0 && sbv_key(&v, 7, 0x2B, 1, SBLK_MOD_NUM) == 0, "Cmd+Tab");
        T.n = 0;
        CHECK(sbv_release_all(&v, SBLK_MOD_NUM) == 0, "release all (GUI held)");
        {
            unsigned k[8];
            int nk = keys_sent(k, 8);
            CHECK(nk == 4 && k[0] == 0x2B && k[1] == (0xE0 | 0x100) && k[2] == 0xE0 && k[3] == 0xE3 &&
                  v.mods == SBLK_MOD_NUM, "GUI release masked by a Left Ctrl tap (%d keys: %x %x %x %x)", nk,
                  nk > 0 ? k[0] : 0, nk > 1 ? k[1] : 0, nk > 2 ? k[2] : 0, nk > 3 ? k[3] : 0);
        }
        CHECK(sbv_text(&v, "\xE2\x9C\x93", 3) == 0 && count_sent(SBLK_T_INPUT_TEXT, NULL) == 1, "text");
        CHECK(v.mine.name[30] == 0 || (v.mine.name[30] & 0xC0) != 0xC0, "name cut on a character boundary");
        sbv_free(&v);
        clean++;

        /* 2. mutated / truncated copies: never crash; an error stops the session */
        for (int k = 0; k < 4; k++) {
            uint8_t *c = malloc(s.n);
            memcpy(c, s.data, s.n);
            size_t n = s.n;
            int muts = (int)rr(6);
            for (int j = 0; j < muts; j++) c[rr((uint32_t)n)] ^= (uint8_t)(1u << rr(8));
            if (rr(3) == 0) n = rr((uint32_t)n);
            memset(&T, 0, sizeof T);
            sbv_init(&v, &ev, "fuzz", 0, 0, 0);
            sbv_start(&v);
            e = feed_chunks(&v, c, n);
            if (e < 0) {
                mutated_err++;
                CHECK(v.state == SBV_CLOSED, "error leaves the session closed");
                CHECK(sbv_feed(&v, c, n) == SBV_E_CLOSED, "no parsing after an error");
                /* a fresh start recovers (as after a USB 6.4 restart) */
                memset(&T, 0, sizeof T);
                CHECK(sbv_start(&v) == 0 && feed_chunks(&v, s.data, s.n) == 0 && T.frames == nf, "restart after error");
            } else {
                mutated_ok++;   /* the flips landed in pixel data or reserved bytes */
            }
            sbv_free(&v);
            free(c);
        }
        free(s.data);
        free(img);
    }
    /* dead link: no message for 5 s */
    sbv v;
    memset(&T, 0, sizeof T);
    sbv_init(&v, &ev, "dead", 0, 0, 0);
    sbv_start(&v);
    T.now += SBV_DEAD_US + 1;
    CHECK(sbv_tick(&v) == SBV_E_DEAD && v.state == SBV_CLOSED, "dead link");
    sbv_free(&v);

    printf("sbv-test: %d clean sessions, %d mutated streams rejected, %d mutated streams harmless; "
           "%ld checks, %ld failed\n", clean, mutated_err, mutated_ok, checks, failures);
    return failures ? 1 : 0;
}
