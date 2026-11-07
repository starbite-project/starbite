/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd host tests (Mac): the capture-independent parts of the Windows sender.
 *   unit: region accumulator, frame encoder (random desktops, OS dirty rects, strips, encodings,
 *         stride padding, "nothing changed = nothing sent"), flow control across the id wrap,
 *         HID -> scan code tables, absolute pointer mapping, key/button state and resync, text
 *   end to end: the real session + TCP code with the synthetic capture and a test viewer on loopback:
 *         HELLO, keyframe, pixel-exact images, 2-frames-in-flight, ACK(0), PING/PONG, input, BUSY,
 *         MODE, new HELLO, protocol error, dead link, idle cost
 * Build/run: build.sh in this directory.
 */
#include "host.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static long g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- region -------------------------------------------------------------------------------------- */

static void test_region(void)
{
    sbl_region g;
    sbl_region_clear(&g);
    sbl_region_add(&g, -10, -10, 20, 20, 100, 100);
    CHECK(g.n == 1 && g.r[0].x == 0 && g.r[0].y == 0 && g.r[0].w == 10 && g.r[0].h == 10);
    sbl_region_add(&g, 2, 2, 3, 3, 100, 100);          /* contained: dropped */
    CHECK(g.n == 1);
    sbl_region_add(&g, 90, 90, 50, 50, 100, 100);      /* clipped */
    CHECK(g.n == 2 && g.r[1].w == 10 && g.r[1].h == 10);
    sbl_region_add(&g, 0, 0, 100, 100, 100, 100);      /* contains both: replaces them */
    CHECK(g.n == 1 && g.r[0].w == 100);
    sbl_region_add(&g, 200, 200, 5, 5, 100, 100);      /* outside */
    sbl_region_add(&g, 5, 5, 0, 5, 100, 100);          /* empty */
    CHECK(g.n == 1);
    sbl_region_clear(&g);
    for (int i = 0; i < SBL_REGION_MAX; i++) sbl_region_add(&g, i * 10, i, 5, 5, 1000, 1000);
    CHECK(g.n == SBL_REGION_MAX);
    sbl_region_add(&g, 999, 999, 1, 1, 1000, 1000);    /* overflow: bounding box */
    CHECK(g.n == 1 && g.r[0].x == 0 && g.r[0].y == 0 && g.r[0].w == 1000 && g.r[0].h == 1000);
}

/* ---- encoder --------------------------------------------------------------------------------------- */

typedef struct viewer_fb { uint8_t *px; uint32_t w, h; size_t stride; } viewer_fb;

/* decodes one encoded frame and applies it; checks structure and sequence numbers */
static int apply_frame(viewer_fb *v, const uint8_t *p, size_t n, uint32_t seq0, uint32_t id, int *key)
{
    sblk_msg m;
    long k = sblk_decode(p, n, &m);
    if (k <= 0 || m.type != SBLK_T_FRAME_BEGIN || m.seq != seq0 || m.u.frame_begin.frame_id != id) return -1;
    uint32_t nr = m.u.frame_begin.nrects, w = m.u.frame_begin.width, h = m.u.frame_begin.height;
    *key = m.u.frame_begin.fflags & SBLK_FB_KEYFRAME;
    if (w != v->w || h != v->h) return -2;
    size_t off = (size_t)k;
    static uint8_t scratch[SBLK_MAX_PAYLOAD];
    for (uint32_t i = 0; i < nr; i++) {
        k = sblk_decode(p + off, n - off, &m);
        if (k <= 0 || m.type != SBLK_T_RECT || m.seq != seq0 + 1 + i) return -3;
        if (sblk_apply_rect(v->px, v->stride, v->w, v->h, &m.u.rect, scratch) < 0) return -4;
        off += (size_t)k;
    }
    k = sblk_decode(p + off, n - off, &m);
    if (k <= 0 || m.type != SBLK_T_FRAME_END || m.seq != seq0 + 1 + nr || m.u.frame_end.frame_id != id) return -5;
    off += (size_t)k;
    return off == n ? (int)nr : -6;
}

static int same_image(const viewer_fb *v, const uint8_t *cur, size_t stride)
{
    for (uint32_t y = 0; y < v->h; y++)
        if (memcmp(v->px + y * v->stride, cur + y * stride, (size_t)v->w * 4)) return 0;
    return 1;
}

static void fill(uint8_t *px, size_t stride, int32_t x, int32_t y, int32_t w, int32_t h, int kind)
{
    uint32_t c = rnd();
    for (int32_t j = y; j < y + h; j++)
        for (int32_t i = x; i < x + w; i++) {
            uint8_t *q = px + (size_t)j * stride + (size_t)i * 4;
            uint32_t v = kind == 0 ? c : kind == 1 ? rnd() : (uint32_t)((i / 3 + j) & 7) * 0x202020u + c;
            q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); q[3] = 0xFF;
        }
}

static void test_encoder(void)
{
    const uint32_t W = 1000, H = 600;
    const size_t stride = W * 4 + 96;
    uint8_t *cur = malloc(stride * H);
    viewer_fb v = { malloc(W * 4 * H), W, H, W * 4 };
    memset(cur, 0, stride * H);
    synth_render(0, W, H, cur, stride);
    sbl_enc e;
    CHECK(sbl_enc_init(&e, 1) == 0);
    uint32_t seq = 100, id = 1;
    int key;

    /* first frame: forced keyframe, full-width strips under max_payload */
    long n = sbl_enc_frame(&e, cur, stride, W, H, NULL, 0, SBLK_CAP_V1_ALL, 256 * 1024, id, 77, &seq);
    CHECK(n > 0 && e.keyframe);
    int nr = apply_frame(&v, e.out, (size_t)n, 100, id, &key);
    CHECKF(nr > 0, "nr %d", nr);
    CHECK(key && same_image(&v, cur, stride));
    CHECK(seq == 100u + 2 + (uint32_t)nr);
    printf("  keyframe %ux%u (synthetic desktop): %d strips, %u solid / %u lz4 / %u raw, %ld bytes (%.1f%% of raw)\n",
           W, H, nr, e.nsolid, e.nlz4, e.nraw, n, 100.0 * (double)n / (W * H * 4.0));

    /* nothing changed: nothing sent, sequence untouched */
    sbl_region g;
    sbl_region_full(&g, W, H);
    uint32_t s0 = seq;
    CHECK(sbl_enc_frame(&e, cur, stride, W, H, &g, 0, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, ++id, 0, &seq) == 0);
    CHECK(seq == s0);
    id--;

    /* one pixel: one 64x64 tile at most, and only if the OS reports it */
    cur[(size_t)300 * stride + 500 * 4] ^= 0x55;
    sbl_region_clear(&g);
    sbl_region_add(&g, 0, 0, 100, 100, W, H);   /* the OS reports something else: nothing found */
    CHECK(sbl_enc_frame(&e, cur, stride, W, H, &g, 0, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, id + 1, 0, &seq) == 0);
    sbl_region_add(&g, 490, 290, 20, 20, W, H);
    n = sbl_enc_frame(&e, cur, stride, W, H, &g, 0, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, ++id, 0, &seq);
    CHECK(n > 0 && !e.keyframe && e.nrects == 1 && e.area <= 20 * 20);
    nr = apply_frame(&v, e.out, (size_t)n, seq - 3, id, &key);
    CHECK(nr == 1 && !key && same_image(&v, cur, stride));
    printf("  one changed pixel: %ld bytes on the wire\n", n);

    /* random desktops: changes, OS rects that cover them (plus noise rects), caps and payload limits */
    uint32_t caps_set[3] = { SBLK_CAP_ENC_RAW, SBLK_CAP_ENC_RAW | SBLK_CAP_ENC_LZ4, SBLK_CAP_V1_ALL };
    int bad = 0;
    for (int it = 0; it < 400; it++) {
        sbl_region_clear(&g);
        int nch = 1 + (int)(rnd() % 6);
        for (int c = 0; c < nch; c++) {
            int32_t w = 1 + (int32_t)(rnd() % (it % 10 == 0 ? W : 200)), h = 1 + (int32_t)(rnd() % (it % 10 == 0 ? H : 150));
            int32_t x = (int32_t)(rnd() % (W - (uint32_t)w + 1)), y = (int32_t)(rnd() % (H - (uint32_t)h + 1));
            fill(cur, stride, x, y, w, h, (int)(rnd() % 3));
            int32_t gx = (int32_t)(rnd() % 40), gy = (int32_t)(rnd() % 40);
            sbl_region_add(&g, x - gx, y - gy, w + 2 * gx, h + 2 * gy, W, H);
        }
        if (rnd() % 3 == 0) sbl_region_add(&g, (int32_t)(rnd() % W), (int32_t)(rnd() % H), 50, 50, W, H);
        int force = rnd() % 25 == 0;
        uint32_t caps = caps_set[rnd() % 3];
        uint32_t maxp = rnd() % 2 ? SBLK_MAX_PAYLOAD : SBLK_RECT_HEAD + W * 4 * (1 + rnd() % 20);
        uint32_t sq = seq;
        n = sbl_enc_frame(&e, cur, stride, W, H, &g, force, caps, maxp, id + 1, 0, &seq);
        if (n == 0) continue;   /* a fill may repeat the old pixels */
        id++;
        if (n < 0) { bad++; continue; }
        nr = apply_frame(&v, e.out, (size_t)n, sq, id, &key);
        if (nr < 0 || !same_image(&v, cur, stride) || (force && !key)) bad++;
        /* every RECT respects the payload limit and the encodings allowed */
        size_t off = 0;
        sblk_msg m;
        while (off < (size_t)n) {
            long k = sblk_decode(e.out + off, (size_t)n - off, &m);
            if (k <= 0) { bad++; break; }
            if (m.type == SBLK_T_RECT) {
                if ((size_t)k - SBLK_HDR_SIZE > maxp) bad++;
                if (m.u.rect.encoding == SBLK_ENC_LZ4 && !(caps & SBLK_CAP_ENC_LZ4)) bad++;
                if (m.u.rect.encoding == SBLK_ENC_SOLID && !(caps & SBLK_CAP_ENC_SOLID)) bad++;
            }
            off += (size_t)k;
        }
    }
    CHECKF(bad == 0, "%d bad frames", bad);

    /* a size or stride change forces a keyframe */
    size_t stride2 = W * 4;
    uint8_t *cur2 = malloc(stride2 * H);
    for (uint32_t y = 0; y < H; y++) memcpy(cur2 + y * stride2, cur + y * stride, W * 4);
    n = sbl_enc_frame(&e, cur2, stride2, W, H, NULL, 0, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, ++id, 0, &seq);
    CHECK(n > 0 && e.keyframe);
    /* max_payload too small for one row */
    CHECK(sbl_enc_frame(&e, cur2, stride2, W, H, NULL, 1, SBLK_CAP_V1_ALL, W * 4, id, 0, &seq) == SBLK_E_ARG);

    /* timing: a typical update (a few windows' worth) and a 1920x1080 keyframe */
    {
        const uint32_t FW = 1920, FH = 1080;
        size_t fs = FW * 4;
        uint8_t *f = malloc(fs * FH);
        synth_render(3, FW, FH, f, fs);
        sbl_enc e2;
        sbl_enc_init(&e2, 1);
        uint32_t sq = 0;
        uint64_t t0 = plat_now_us();
        n = sbl_enc_frame(&e2, f, fs, FW, FH, NULL, 1, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, 1, 0, &sq);
        uint64_t t1 = plat_now_us();
        synth_render(4, FW, FH, f, fs);
        sbl_region_clear(&g);
        sbl_region_add(&g, 0, 0, 400, 300, FW, FH);
        sbl_region_add(&g, 1700, 1040, 220, 40, FW, FH);
        uint64_t t2 = plat_now_us();
        long n2 = sbl_enc_frame(&e2, f, fs, FW, FH, &g, 0, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD, 2, 0, &sq);
        uint64_t t3 = plat_now_us();
        CHECK(n > 0 && n2 > 0);
        printf("  1920x1080 keyframe: %ld bytes (%.1f%%) in %.2f ms; box+counter update: %u rects, %ld bytes in %.3f ms (Mac)\n",
               n, 100.0 * (double)n / (FW * FH * 4.0), (double)(t1 - t0) / 1000.0, e2.nrects, n2,
               (double)(t3 - t2) / 1000.0);
        sbl_enc_free(&e2);
        free(f);
    }
    sbl_enc_free(&e);
    free(cur); free(cur2); free(v.px);
}

/* ---- flow control -------------------------------------------------------------------------------- */

static void test_flow(void)
{
    sbl_flow f;
    sbl_flow_reset(&f);
    CHECK(sbl_flow_can_send(&f) && f.n == 0);
    CHECK(sbl_flow_ack(&f, 1) == -1);          /* nothing sent yet */
    uint32_t a = sbl_flow_take_id(&f); sbl_flow_sent(&f, a);
    uint32_t b = sbl_flow_take_id(&f); sbl_flow_sent(&f, b);
    CHECK(a == 1 && b == 2 && !sbl_flow_can_send(&f));
    CHECK(sbl_flow_ack(&f, 3) == -1 && f.n == 2);   /* never sent */
    CHECK(sbl_flow_ack(&f, 1) == 1 && f.n == 1 && sbl_flow_can_send(&f));
    CHECK(sbl_flow_ack(&f, 1) == 0 && f.n == 1);    /* duplicate */
    CHECK(sbl_flow_ack(&f, 2) == 1 && f.n == 0);

    /* across the wrap: ..., 0xFFFFFFFE, 0xFFFFFFFF, 1, 2 (0 is never used) */
    sbl_flow_reset(&f);
    f.next_id = 0xFFFFFFFEu;
    uint32_t ids[4];
    for (int i = 0; i < 4; i++) {
        ids[i] = sbl_flow_take_id(&f);
        sbl_flow_sent(&f, ids[i]);
        if (f.n == 2) CHECK(sbl_flow_ack(&f, ids[i - 1]) == 1);
    }
    CHECK(ids[0] == 0xFFFFFFFEu && ids[1] == 0xFFFFFFFFu && ids[2] == 1 && ids[3] == 2);
    CHECK(f.n == 1 && f.ids[0] == 2);
    /* two in flight across the wrap: counted as 2 (last_sent - last_acked would say 3) */
    sbl_flow_reset(&f);
    f.next_id = 0xFFFFFFFFu;
    a = sbl_flow_take_id(&f); sbl_flow_sent(&f, a);
    b = sbl_flow_take_id(&f); sbl_flow_sent(&f, b);
    CHECK(a == 0xFFFFFFFFu && b == 1 && f.n == 2 && !sbl_flow_can_send(&f));
    CHECK(sbl_flow_ack(&f, 1) == 1 && f.n == 0);   /* cumulative across the wrap */
}

/* ---- input ---------------------------------------------------------------------------------------- */

static int ev_is(const sbl_ev *e, int kind, int code, int flags)
{
    return e->kind == kind && e->code == code && e->flags == flags;
}

static void test_input(void)
{
    /* tables */
    CHECK(sbl_hid_scan(0x04) == 0x1E && sbl_hid_scan(0x1D) == 0x2C && sbl_hid_scan(0x27) == 0x0B);
    CHECK(sbl_hid_scan(0x28) == 0x1C && sbl_hid_scan(0x58) == 0x11C);           /* Enter, keypad Enter E0 1C */
    CHECK(sbl_hid_scan(0x4F) == 0x14D && sbl_hid_scan(0x52) == 0x148);          /* arrows E0 */
    CHECK(sbl_hid_scan(0xE3) == 0x15B && sbl_hid_scan(0xE7) == 0x15C);          /* GUI = Windows keys */
    CHECK(sbl_hid_scan(0xE6) == 0x138 && sbl_hid_scan(0xE2) == 0x38);           /* AltGr, Alt */
    CHECK(sbl_hid_scan(0x44) == 0x57 && sbl_hid_scan(0x45) == 0x58);            /* F11 F12 */
    CHECK(sbl_hid_scan(0x66) == 0 && sbl_hid_scan(0x48) == 0 && sbl_hid_scan(0x300) == 0);
    CHECK(sbl_hid_vk(7, 0x48) == 0x13 && sbl_hid_vk(0x0C, 0xE9) == 0xAF && sbl_hid_vk(0x0C, 0xCD) == 0xB3);
    /* every letter/digit/F-key is mapped and no two keyboard usages share a scan code (except the
     * two that are the same key on a real keyboard: \ and Non-US #) */
    int dup = 0, mapped = 0;
    for (uint16_t u = 0; u < 0xE8; u++) {
        uint16_t sc = sbl_hid_scan(u);
        if (!sc) continue;
        mapped++;
        for (uint16_t v = (uint16_t)(u + 1); v < 0xE8; v++)
            if (sbl_hid_scan(v) == sc && !(u == 0x31 && v == 0x32)) dup++;
    }
    CHECK(dup == 0);
    CHECKF(mapped >= 125, "%d mapped", mapped);

    /* absolute mapping: every pixel lands on itself, for truncation and for rounding, on several
     * desktop layouts (negative virtual origin, the output not at the origin) */
    sbl_geom gs[3] = {
        { 0, 0, 1280, 720, 0, 0, 1280, 720, 1280, 720 },
        { 1440, 0, 1920, 1080, 0, 0, 3360, 2960, 1920, 1080 },         /* phone panel 1440x2960 + display */
        { -1920, -200, 1920, 1080, -1920, -200, 3360, 2960, 1920, 1080 },
    };
    int miss = 0;
    for (int k = 0; k < 3; k++) {
        const sbl_geom *g = &gs[k];
        for (int32_t x = -5; x < (int32_t)g->disp_w + 5; x++) {
            int32_t nx, ny;
            sbl_map_abs(g, x, x % (int32_t)g->disp_h, &nx, &ny);
            int32_t want = g->out_x + (x < 0 ? 0 : x >= (int32_t)g->disp_w ? (int32_t)g->disp_w - 1 : x);
            int32_t tr = g->virt_x + (int32_t)(((int64_t)nx * g->virt_w) >> 16);
            int32_t rd = g->virt_x + (int32_t)(((int64_t)nx * g->virt_w + 32768) >> 16);
            if (tr != want || rd != want || nx < 0 || nx > 65535) miss++;
        }
        for (int32_t y = 0; y < (int32_t)g->disp_h; y++) {
            int32_t nx, ny;
            sbl_map_abs(g, 0, y, &nx, &ny);
            int32_t want = g->out_y + y;
            int32_t tr = g->virt_y + (int32_t)(((int64_t)ny * g->virt_h) >> 16);
            int32_t rd = g->virt_y + (int32_t)(((int64_t)ny * g->virt_h + 32768) >> 16);
            if (tr != want || rd != want) miss++;
        }
    }
    CHECKF(miss == 0, "%d pixels missed", miss);

    /* pointer: move, buttons, wheel */
    sbl_input st;
    memset(&st, 0, sizeof st);
    sbl_ev ev[64];
    sblk_input_pointer p = { 10, 20, SBLK_BTN_LEFT, 0, 0 };
    int n = sbl_in_pointer(&st, &p, &gs[0], ev, 64);
    CHECK(n == 2 && ev[0].kind == SBL_EV_MOVE_ABS && ev_is(&ev[1], SBL_EV_BUTTON, SBL_BTN_L, 0));
    p.buttons = SBLK_BTN_RIGHT | SBLK_BTN_FORWARD; p.wheel_dy = -120; p.wheel_dx = 12;
    n = sbl_in_pointer(&st, &p, &gs[0], ev, 64);
    CHECK(n == 6 && ev_is(&ev[1], SBL_EV_BUTTON, SBL_BTN_L, SBL_EVF_UP) && ev_is(&ev[2], SBL_EV_BUTTON, SBL_BTN_R, 0) &&
          ev_is(&ev[3], SBL_EV_BUTTON, SBL_BTN_X2, 0) && ev[4].kind == SBL_EV_WHEEL && ev[4].data == -120 &&
          ev[5].kind == SBL_EV_HWHEEL && ev[5].data == 12);

    /* keys: down/up, modifier resync, consumer keys, release all */
    sblk_input_key k = { 7, 0xE1, 1, SBLK_MOD_LSHIFT };      /* LShift down */
    n = sbl_in_key(&st, &k, ev, 64);
    CHECK(n == 1 && ev_is(&ev[0], SBL_EV_KEY_SCAN, 0x2A, 0));
    k.usage = 0x04; k.modifiers = SBLK_MOD_LSHIFT;           /* A with shift */
    n = sbl_in_key(&st, &k, ev, 64);
    CHECK(n == 1 && ev_is(&ev[0], SBL_EV_KEY_SCAN, 0x1E, 0));
    k.usage = 0x4F; k.down = 1; k.modifiers = 0;              /* Right arrow; shift was released elsewhere */
    n = sbl_in_key(&st, &k, ev, 64);
    CHECK(n == 2 && ev_is(&ev[0], SBL_EV_KEY_SCAN, 0x2A, SBL_EVF_UP) && ev_is(&ev[1], SBL_EV_KEY_SCAN, 0x4D, SBL_EVF_EXT));
    sblk_input_key c = { 0x0C, 0xE9, 1, 0 };                  /* volume up */
    n = sbl_in_key(&st, &c, ev, 64);
    CHECK(n == 1 && ev_is(&ev[0], SBL_EV_KEY_VK, 0xAF, SBL_EVF_EXT));
    sblk_input_key pw = { 7, 0x66, 1, 0 };                    /* Power: never */
    CHECK(sbl_in_key(&st, &pw, ev, 64) == 0);
    n = sbl_in_release_all(&st, ev, SBL_MAX_EV_RELEASE);      /* A, Right, VolUp, R button, X2 button */
    CHECK(n == 5);
    CHECK(ev_is(&ev[0], SBL_EV_KEY_SCAN, 0x1E, SBL_EVF_UP) && ev_is(&ev[1], SBL_EV_KEY_SCAN, 0x4D, SBL_EVF_UP | SBL_EVF_EXT));
    CHECK(ev_is(&ev[2], SBL_EV_KEY_VK, 0xAF, SBL_EVF_UP | SBL_EVF_EXT) && ev_is(&ev[3], SBL_EV_BUTTON, SBL_BTN_R, SBL_EVF_UP));
    CHECK(sbl_in_release_all(&st, ev, SBL_MAX_EV_RELEASE) == 0);

    /* text: ASCII, 2-byte, 4-byte (surrogates), CRLF/LF = Enter, tab, invalid bytes */
    const uint8_t t[] = "h\xc3\xa9\xf0\x9f\x98\x80\r\n\tx\xff";
    n = sbl_in_text(t, (uint32_t)sizeof t - 1, ev, 64);
    CHECK(n == 2 * 8);
    CHECK(ev_is(&ev[0], SBL_EV_UNICODE, 'h', 0) && ev_is(&ev[1], SBL_EV_UNICODE, 'h', SBL_EVF_UP));
    CHECK(ev_is(&ev[2], SBL_EV_UNICODE, 0xE9, 0));
    CHECK(ev_is(&ev[4], SBL_EV_UNICODE, 0xD83D, 0) && ev_is(&ev[6], SBL_EV_UNICODE, 0xDE00, 0));
    CHECK(ev_is(&ev[8], SBL_EV_KEY_VK, 0x0D, 0) && ev_is(&ev[10], SBL_EV_KEY_VK, 0x09, 0));
    CHECK(ev_is(&ev[12], SBL_EV_UNICODE, 'x', 0) && ev_is(&ev[14], SBL_EV_UNICODE, 0xFFFD, 0));
    /* fuzz: never more than 4 events per byte, never out of bounds */
    sbl_ev big[4 * 64];
    int over = 0;
    for (int it = 0; it < 20000; it++) {
        uint8_t b[64];
        uint32_t len = 1 + rnd() % 64;
        for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)rnd();
        n = sbl_in_text(b, len, big, 4 * 64);
        if (n < 0 || n > 4 * (int)len) over++;
        for (int i = 0; i < n; i++) if (big[i].kind == SBL_EV_UNICODE && big[i].code < 0x20) over++;
    }
    CHECK(over == 0);
}

/* ---- end to end over TCP ------------------------------------------------------------------------- */

typedef struct tview {
    int fd;
    sblk_parser p;
    uint8_t *pbuf;
    uint8_t *fb;
    uint32_t w, h;
    int auto_ack, answer_ping;
    uint32_t rx_seq;            /* next expected sequence */
    int seq_bad;
    int got_hello;
    sblk_hello hello;
    int got_mode;
    sblk_mode mode;
    uint32_t frames, keyframes, last_id, last_key_id, in_frame, rects_left;
    uint64_t bytes;
    int got_pong;
    sblk_pong pong;
    int bye, bye_reason, closed;
    uint32_t tx_seq;
    int bad;
} tview;

static uint8_t g_scratch[SBLK_MAX_PAYLOAD];

static int tv_connect(tview *v, int port)
{
    memset(v, 0, sizeof *v);
    v->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(v->fd, (struct sockaddr *)&a, sizeof a)) return -1;
    int one = 1;
    setsockopt(v->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    v->pbuf = malloc(SBLK_MAX_PAYLOAD);
    sblk_parser_init(&v->p, v->pbuf, SBLK_MAX_PAYLOAD);
    v->auto_ack = 1;
    v->answer_ping = 1;
    return 0;
}

static void tv_close(tview *v)
{
    if (v->fd >= 0) close(v->fd);
    v->fd = -1;
    free(v->pbuf); free(v->fb);
    v->pbuf = NULL; v->fb = NULL;
}

static int tv_send(tview *v, sblk_msg *m)
{
    uint8_t b[2048];
    m->seq = v->tx_seq++;
    long n = sblk_encode(m, b, sizeof b);
    if (n <= 0) return -1;
    return send(v->fd, b, (size_t)n, 0) == n ? 0 : -1;
}

static int tv_hello(tview *v, uint32_t caps, uint32_t maxp)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_VIEWER;
    m.u.hello.proto_major = 1;
    m.u.hello.caps = caps;
    m.u.hello.pixel_format = SBLK_PIXFMT_BGRA8888;
    m.u.hello.max_payload = maxp;
    snprintf(m.u.hello.name, sizeof m.u.hello.name, "test viewer");
    v->tx_seq = 0;
    v->rx_seq = 0;
    v->got_hello = 0;
    return tv_send(v, &m);
}

static void tv_ack(tview *v, uint32_t id)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_ACK;
    m.u.ack.frame_id = id;
    tv_send(v, &m);
}

static void tv_on_msg(tview *v, const sblk_msg *m)
{
    if (m->type == SBLK_T_HELLO) { v->rx_seq = 0; v->got_hello = 1; v->hello = m->u.hello; }
    if (m->seq != v->rx_seq) v->seq_bad++;
    v->rx_seq = m->seq + 1;
    switch (m->type) {
    case SBLK_T_MODE: v->got_mode++; v->mode = m->u.mode; break;
    case SBLK_T_FRAME_BEGIN:
        if (v->in_frame) v->bad++;
        if (m->u.frame_begin.width != v->w || m->u.frame_begin.height != v->h) {
            free(v->fb);
            v->w = m->u.frame_begin.width; v->h = m->u.frame_begin.height;
            v->fb = calloc((size_t)v->w * v->h, 4);
            if (!(m->u.frame_begin.fflags & SBLK_FB_KEYFRAME)) v->bad++;   /* size change => keyframe */
        }
        v->in_frame = m->u.frame_begin.frame_id;
        v->rects_left = m->u.frame_begin.nrects;
        if (m->u.frame_begin.fflags & SBLK_FB_KEYFRAME) { v->keyframes++; v->last_key_id = v->in_frame; }
        break;
    case SBLK_T_RECT:
        if (!v->in_frame || !v->rects_left) v->bad++;
        v->rects_left--;
        if (sblk_apply_rect(v->fb, (size_t)v->w * 4, v->w, v->h, &m->u.rect, g_scratch) < 0) v->bad++;
        break;
    case SBLK_T_FRAME_END:
        if (m->u.frame_end.frame_id != v->in_frame || v->rects_left) v->bad++;
        v->in_frame = 0;
        v->frames++;
        v->last_id = m->u.frame_end.frame_id;
        if (v->auto_ack) tv_ack(v, v->last_id);
        break;
    case SBLK_T_PING:
        if (v->answer_ping) {
            sblk_msg r;
            memset(&r, 0, sizeof r);
            r.type = SBLK_T_PONG;
            r.u.pong.id = m->u.ping.id;
            r.u.pong.echo_us = m->u.ping.t_us;
            r.u.pong.t_us = plat_now_us();
            tv_send(v, &r);
        }
        break;
    case SBLK_T_PONG: v->got_pong++; v->pong = m->u.pong; break;
    case SBLK_T_BYE: v->bye++; v->bye_reason = (int)m->u.bye.reason; break;
    }
}

/* reads for up to ms (returns early once until() is true) */
static void tv_pump(tview *v, int ms, int (*until)(tview *))
{
    static uint8_t buf[256 * 1024];
    uint64_t end = plat_now_us() + (uint64_t)ms * 1000u;
    while (!v->closed) {
        if (until && until(v)) return;
        uint64_t now = plat_now_us();
        if (now >= end) return;
        struct pollfd pf = { v->fd, POLLIN, 0 };
        int r = poll(&pf, 1, (int)((end - now) / 1000) + 1);
        if (r <= 0) continue;
        ssize_t k = recv(v->fd, buf, sizeof buf, 0);
        if (k <= 0) { v->closed = 1; return; }
        v->bytes += (uint64_t)k;
        size_t off = 0;
        while (off < (size_t)k) {
            size_t used;
            sblk_msg m;
            int e = sblk_parser_feed(&v->p, buf + off, (size_t)k - off, &used, &m);
            off += used;
            if (e < 0) { v->bad++; v->closed = 1; return; }
            if (e == 0) break;
            tv_on_msg(v, &m);
        }
    }
}

static int u_hello(tview *v) { return v->got_hello; }
static int u_key(tview *v) { return v->keyframes > 0 && !v->in_frame; }
static int u_bye(tview *v) { return v->bye > 0; }
static int u_pong(tview *v) { return v->got_pong > 0; }
static int u_mode(tview *v) { return v->got_mode > 0; }

/* lets frames flow until nothing new arrives for quiet_ms */
static void tv_settle(tview *v, int quiet_ms, int max_ms)
{
    uint64_t end = plat_now_us() + (uint64_t)max_ms * 1000u;
    while (plat_now_us() < end && !v->closed) {
        uint32_t f = v->frames;
        tv_pump(v, quiet_ms, NULL);
        if (v->frames == f && !v->in_frame) return;
    }
}

static int tv_matches(tview *v, uint32_t scene)
{
    if (!v->fb) return 0;
    uint8_t *ref = malloc((size_t)v->w * v->h * 4);
    synth_render(scene, v->w, v->h, ref, (size_t)v->w * 4);
    int ok = memcmp(ref, v->fb, (size_t)v->w * v->h * 4) == 0;
    free(ref);
    return ok;
}

static int wait_events(int n, int ms)
{
    sbl_ev ev[64];
    for (int t = 0; t < ms; t += 5) {
        if (rec_get(ev, 64) >= n) return 1;
        plat_sleep_ms(5);
    }
    return 0;
}

static sbl_sender g_s;
static void run_thread(void *a) { sbl_sender_run((sbl_sender *)a); }

/* ---- USB-style receive (sbl_rx with transfer boundaries, no capture loop) -------------------------- */

static uint8_t g_usb_out[64 * 1024];
static size_t g_usb_n;
static int usb_rec_send(sbl_link *l, const uint8_t *p, size_t n)
{
    (void)l;
    if (g_usb_n + n <= sizeof g_usb_out) { memcpy(g_usb_out + g_usb_n, p, n); g_usb_n += n; }
    return 0;
}
static void usb_rec_close(sbl_link *l) { (void)l; }

/* counts the messages of type t the "phone" sent since the last reset */
static int usb_count(int t)
{
    int c = 0;
    size_t off = 0;
    while (off < g_usb_n) {
        sblk_msg m;
        long k = sblk_decode(g_usb_out + off, g_usb_n - off, &m);
        if (k <= 0) return -1;
        if (m.type == t) c++;
        off += (size_t)k;
    }
    return c;
}

static size_t enc_hello(uint8_t *b, size_t cap)
{
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_VIEWER;
    m.u.hello.proto_major = 1;
    m.u.hello.caps = SBLK_CAP_V1_ALL;
    m.u.hello.pixel_format = SBLK_PIXFMT_BGRA8888;
    m.u.hello.max_payload = SBLK_MAX_PAYLOAD;
    return (size_t)sblk_encode(&m, b, cap);
}

static void test_usb_rx(void)
{
    static sbl_sender us;
    static sbl_link ul;
    sbl_cfg cfg;
    sbl_cfg_default(&cfg);
    CHECK(sbl_sender_init(&us, &cfg) == 0);
    sbl_link_init(&ul, "usb test", 1);
    ul.send = usb_rec_send;
    ul.close = usb_rec_close;

    uint8_t hello[128], text[128];
    size_t hn = enc_hello(hello, sizeof hello);
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_TEXT;
    m.u.text.utf8 = (const uint8_t *)"0123456789012345678901234567890123456789012345678901234567890123456789"
                                     "012345678901234567890123456789";
    m.u.text.len = 100;
    size_t tn = (size_t)sblk_encode(&m, text, sizeof text);
    CHECK(hn == 72 && tn == 116);

    /* hunting: data that does not start a transfer with HELLO is ignored */
    g_usb_n = 0;
    CHECK(sbl_rx(&us, &ul, text, tn, 1) == 0 && g_usb_n == 0 && !us.active);
    /* HELLO as its own transfer: session, HELLO answered */
    CHECK(sbl_rx(&us, &ul, hello, hn, 1) == 0 && us.active && us.owner == &ul && usb_count(SBLK_T_HELLO) == 1);
    /* the Mac app dies after the first 20 bytes of an INPUT_TEXT; the new app sends HELLO as a transfer of
     * its own (PROTOCOL.md 6.3): new session at once, not swallowed as the old message's payload */
    g_usb_n = 0;
    CHECK(sbl_rx(&us, &ul, text, 20, 1) == 0);
    CHECK(sbl_rx(&us, &ul, hello, hn, 1) == 0);
    CHECKF(usb_count(SBLK_T_HELLO) == 1 && usb_count(SBLK_T_BYE) == 0 && us.active,
           "after a cut-off message, HELLO answered %d times, BYE %d, active %d", usb_count(SBLK_T_HELLO),
           usb_count(SBLK_T_BYE), us.active);
    /* the same bytes in the MIDDLE of a transfer are payload, not a new session: a continuation that
     * happens to look like HELLO must not reset the parser */
    g_usb_n = 0;
    uint8_t cont[256];
    memcpy(cont, text, 20);
    memcpy(cont + 20, text + 20, tn - 20);
    CHECK(sbl_rx(&us, &ul, cont, 20, 1) == 0 && sbl_rx(&us, &ul, cont + 20, tn - 20, 0) == 0);
    CHECK(g_usb_n == 0 && us.active);
    /* garbage: BYE(PROTOCOL), then hunting until a transfer starts with HELLO */
    g_usb_n = 0;
    CHECK(sbl_rx(&us, &ul, (const uint8_t *)"GARBAGE GARBAGE!", 16, 1) == 0);
    CHECK(usb_count(SBLK_T_BYE) == 1 && !us.active && us.owner == NULL && ul.hunting);
    g_usb_n = 0;
    CHECK(sbl_rx(&us, &ul, hello, hn, 0) == 0 && g_usb_n == 0);   /* not at a transfer start: ignored */
    CHECK(sbl_rx(&us, &ul, hello, hn, 1) == 0 && usb_count(SBLK_T_HELLO) == 1 && us.active);
    sbl_link_end(&us, &ul, "test done");
    CHECK(!us.active && us.owner == NULL);
}

static void test_e2e(void)
{
    sbl_cfg cfg;
    sbl_cfg_default(&cfg);
    cfg.dead_ms = 1500;
    cfg.ping_ms = 300;
    cfg.fps = 60;
    synth_set_size(800, 600);
    synth_set_scene(1);
    CHECK(sbl_sender_init(&g_s, &cfg) == 0);
    int port = 0;
    CHECK(sbl_tcp_start(&g_s, "127.0.0.1", 0, &port) == 0 && port > 0);
    plat_thread(run_thread, &g_s);
    plat_sleep_ms(100);

    tview v;
    CHECK(tv_connect(&v, port) == 0);
    CHECK(tv_hello(&v, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD) == 0);
    tv_pump(&v, 2000, u_key);
    CHECK(v.got_hello && v.hello.role == SBLK_ROLE_SOURCE && v.hello.proto_major == 1);
    CHECK(v.hello.width == 800 && v.hello.height == 600 && v.hello.pixel_format == SBLK_PIXFMT_BGRA8888);
    CHECK((v.hello.caps & (SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID | SBLK_CAP_MODE | SBLK_CAP_EXTEND)) ==
          (SBLK_CAP_ENC_LZ4 | SBLK_CAP_ENC_SOLID | SBLK_CAP_MODE | SBLK_CAP_EXTEND));
    CHECK(v.keyframes == 1 && v.w == 800 && v.h == 600);
    CHECK(tv_matches(&v, 1));
    printf("  e2e: HELLO + keyframe 800x600: %llu bytes received\n", (unsigned long long)v.bytes);

    /* scenes: pixel-exact after every change; only changed areas travel */
    int mism = 0;
    for (int i = 0; i < 25; i++) {
        uint32_t sc = i < 15 ? (uint32_t)(2 + i) : (rnd() % 100000);
        uint64_t b0 = v.bytes;
        synth_set_scene(sc);
        tv_settle(&v, 120, 3000);
        if (!tv_matches(&v, sc)) mism++;
        if (i == 3) printf("  e2e: one scene step = %llu bytes\n", (unsigned long long)(v.bytes - b0));
    }
    CHECKF(mism == 0, "%d scenes differ", mism);

    /* idle: no frames when nothing changes (only PINGs) */
    uint32_t f0 = v.frames;
    uint64_t c0 = synth_captures();
    tv_pump(&v, 1000, NULL);
    CHECK(v.frames == f0 && synth_captures() == c0);

    /* flow control: stop ACKing while the desktop animates: exactly 2 frames in flight */
    v.auto_ack = 0;
    f0 = v.frames;
    synth_set_auto(60);
    tv_pump(&v, 1000, NULL);
    CHECKF(v.frames - f0 == 2, "%u frames without ACK", v.frames - f0);
    uint64_t caps_blocked = synth_captures();
    tv_pump(&v, 300, NULL);
    CHECKF(synth_captures() == caps_blocked, "captured %llu frames while blocked",
           (unsigned long long)(synth_captures() - caps_blocked));
    tv_ack(&v, v.last_id);
    v.auto_ack = 1;
    tv_pump(&v, 500, NULL);
    CHECK(v.frames - f0 > 6);
    printf("  e2e: animated at 60 Hz with ACKs: %u frames in 0.5 s (cap 60 fps)\n", v.frames - f0 - 2);
    synth_set_auto(0);
    tv_settle(&v, 150, 3000);
    synth_set_scene(424242);
    tv_settle(&v, 150, 3000);
    CHECK(tv_matches(&v, 424242));

    /* ACK(0): keyframe */
    uint32_t k0 = v.keyframes;
    tv_ack(&v, 0);
    synth_set_scene(424243);
    tv_settle(&v, 150, 3000);
    CHECK(v.keyframes == k0 + 1 && tv_matches(&v, 424243));

    /* PING -> PONG */
    sblk_msg m;
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_PING;
    m.u.ping.id = 77;
    m.u.ping.t_us = 0x1122334455667788ull;
    tv_send(&v, &m);
    tv_pump(&v, 1000, u_pong);
    CHECK(v.got_pong && v.pong.id == 77 && v.pong.echo_us == 0x1122334455667788ull);

    /* PINGs while frames (keyframes: long encodes) flow: every PING answered, sequence numbers stay
     * consecutive although PONGs are sent while frames are being encoded (renumbered at send) */
    {
        int pings = 0, pongs0 = v.got_pong, seqbad0 = v.seq_bad, bad0 = v.bad;
        uint64_t ren0 = g_s.st_renumbered, end = plat_now_us() + 3000000u;
        synth_set_auto(60);
        while (plat_now_us() < end && (pings < 200 || g_s.st_renumbered == ren0)) {
            memset(&m, 0, sizeof m);
            m.type = SBLK_T_PING;
            m.u.ping.id = 1000u + (uint32_t)pings++;
            tv_send(&v, &m);
            if (pings % 4 == 0) tv_ack(&v, 0);   /* keyframes: the slowest encodes */
            tv_pump(&v, 2, NULL);
        }
        synth_set_auto(0);
        tv_settle(&v, 150, 3000);
        tv_pump(&v, 300, NULL);
        CHECKF(v.got_pong - pongs0 == pings, "%d PINGs, %d PONGs", pings, v.got_pong - pongs0);
        CHECKF(v.seq_bad == seqbad0 && v.bad == bad0, "seq_bad %d bad %d", v.seq_bad - seqbad0, v.bad - bad0);
        printf("  e2e: %d PINGs during frames: all answered, %llu frames renumbered, sequence intact\n", pings,
               (unsigned long long)(g_s.st_renumbered - ren0));
        CHECK(g_s.st_renumbered > ren0);
        synth_set_scene(424244);
        tv_settle(&v, 150, 3000);
        CHECK(tv_matches(&v, 424244));
    }

    /* input */
    rec_clear();
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_POINTER;
    m.u.pointer.x = 100; m.u.pointer.y = 50; m.u.pointer.buttons = SBLK_BTN_LEFT;
    tv_send(&v, &m);
    m.u.pointer.buttons = 0; m.u.pointer.wheel_dy = 120;
    tv_send(&v, &m);
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_KEY;
    m.u.key.usage_page = 7; m.u.key.usage = 0xE1; m.u.key.down = 1; m.u.key.modifiers = SBLK_MOD_LSHIFT;
    tv_send(&v, &m);
    m.u.key.usage = 0x05; m.u.key.modifiers = 0;       /* B, shift released while unfocused */
    tv_send(&v, &m);
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_TEXT;
    m.u.text.utf8 = (const uint8_t *)"ok\n";
    m.u.text.len = 3;
    tv_send(&v, &m);
    CHECK(wait_events(14, 2000));
    sbl_ev ev[64];
    int n = rec_get(ev, 64);
    CHECKF(n == 14, "%d events", n);
    if (n >= 14) {
        int32_t nx, ny;
        sbl_geom g = { 0, 0, 800, 600, 0, 0, 800, 600, 800, 600 };
        sbl_map_abs(&g, 100, 50, &nx, &ny);
        CHECK(ev[0].kind == SBL_EV_MOVE_ABS && ev[0].x == nx && ev[0].y == ny);
        CHECK(ev_is(&ev[1], SBL_EV_BUTTON, SBL_BTN_L, 0) && ev[2].kind == SBL_EV_MOVE_ABS);
        CHECK(ev_is(&ev[3], SBL_EV_BUTTON, SBL_BTN_L, SBL_EVF_UP) && ev[4].kind == SBL_EV_WHEEL && ev[4].data == 120);
        CHECK(ev_is(&ev[5], SBL_EV_KEY_SCAN, 0x2A, 0) && ev_is(&ev[6], SBL_EV_KEY_SCAN, 0x2A, SBL_EVF_UP) &&
              ev_is(&ev[7], SBL_EV_KEY_SCAN, 0x30, 0));
        CHECK(ev_is(&ev[8], SBL_EV_UNICODE, 'o', 0) && ev_is(&ev[10], SBL_EV_UNICODE, 'k', 0) &&
              ev_is(&ev[12], SBL_EV_KEY_VK, 0x0D, 0) && ev_is(&ev[13], SBL_EV_KEY_VK, 0x0D, SBL_EVF_UP));
    }
    /* (B is still held: the session restart below must release it) */

    /* a second viewer is refused */
    tview v2;
    CHECK(tv_connect(&v2, port) == 0);
    tv_pump(&v2, 2000, u_bye);
    CHECK(v2.bye && v2.bye_reason == SBLK_BYE_BUSY);
    tv_pump(&v2, 1000, NULL);
    CHECK(v2.closed);
    tv_close(&v2);

    /* MODE: 640x480 -> APPLIED, then a keyframe of the new size */
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_MODE;
    m.u.mode.width = 640; m.u.mode.height = 480; m.u.mode.status = SBLK_MODE_REQUEST;
    k0 = v.keyframes;
    tv_send(&v, &m);
    tv_pump(&v, 3000, u_mode);
    CHECKF(v.got_mode && v.mode.status == SBLK_MODE_APPLIED && v.mode.width == 640 && v.mode.height == 480, "mode %d st %u %ux%u", v.got_mode, v.mode.status, v.mode.width, v.mode.height);
    tv_settle(&v, 200, 3000);
    CHECK(v.w == 640 && v.h == 480 && v.keyframes > k0 && tv_matches(&v, synth_scene()));
    v.got_mode = 0;
    m.u.mode.width = 50; m.u.mode.height = 50;    /* unsupported: REJECTED with the size kept */
    tv_send(&v, &m);
    tv_pump(&v, 3000, u_mode);
    CHECK(v.got_mode && v.mode.status == SBLK_MODE_REJECTED && v.mode.width == 640);

    /* a new HELLO on the same connection: new session, keyframe, held keys released */
    rec_clear();
    k0 = v.keyframes;
    tv_hello(&v, SBLK_CAP_ENC_RAW | SBLK_CAP_INPUT_PTR, SBLK_MAX_PAYLOAD);   /* RAW only this time */
    tv_pump(&v, 2000, u_hello);
    CHECK(v.got_hello && v.hello.width == 640);
    tv_settle(&v, 200, 3000);
    CHECK(v.keyframes == k0 + 1 && tv_matches(&v, synth_scene()));
    CHECK(wait_events(1, 1000));
    n = rec_get(ev, 64);
    CHECK(n == 1 && ev_is(&ev[0], SBL_EV_KEY_SCAN, 0x30, SBL_EVF_UP));
    /* without INPUT_KEY in the viewer's caps, keys are ignored */
    rec_clear();
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_INPUT_KEY;
    m.u.key.usage_page = 7; m.u.key.usage = 0x04; m.u.key.down = 1;
    tv_send(&v, &m);
    plat_sleep_ms(200);
    CHECK(rec_get(ev, 64) == 0);
    CHECK(v.seq_bad == 0 && v.bad == 0);

    /* garbage: BYE(PROTOCOL) and close; then a new viewer gets in */
    send(v.fd, "GARBAGE GARBAGE!", 16, 0);
    tv_pump(&v, 2000, NULL);
    CHECK(v.bye && v.bye_reason == SBLK_BYE_PROTOCOL && v.closed);
    tv_close(&v);
    plat_sleep_ms(100);

    CHECK(tv_connect(&v, port) == 0);
    tv_hello(&v, SBLK_CAP_V1_ALL, SBLK_MAX_PAYLOAD);
    tv_pump(&v, 2000, u_key);
    CHECK(v.got_hello && v.keyframes == 1 && tv_matches(&v, synth_scene()));
    /* small max_payload: still pixel exact (many strips) */
    tv_close(&v);
    plat_sleep_ms(100);
    CHECK(tv_connect(&v, port) == 0);
    tv_hello(&v, SBLK_CAP_V1_ALL, 64 * 1024);
    tv_pump(&v, 2000, u_key);
    synth_set_scene(99);
    tv_settle(&v, 150, 3000);
    CHECK(v.got_hello && tv_matches(&v, 99) && v.bad == 0 && v.seq_bad == 0);

    /* dead link: a viewer that goes silent is closed after dead_ms (1.5 s here) */
    v.answer_ping = 0;
    v.auto_ack = 0;
    uint64_t t0 = plat_now_us();
    tv_pump(&v, 4000, NULL);
    double dt = (double)(plat_now_us() - t0) / 1e6;
    CHECKF(v.closed && dt > 1.2 && dt < 3.0, "closed %d after %.2f s", v.closed, dt);
    tv_close(&v);
    plat_sleep_ms(100);

    /* a connection that never says HELLO is closed too, and HELLO with a bad role is refused */
    CHECK(tv_connect(&v, port) == 0);
    tv_pump(&v, 4000, NULL);
    CHECK(v.closed && !v.got_hello);
    tv_close(&v);
    plat_sleep_ms(100);
    CHECK(tv_connect(&v, port) == 0);
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_SOURCE;
    m.u.hello.proto_major = 1;
    m.u.hello.max_payload = SBLK_MAX_PAYLOAD;
    m.u.hello.pixel_format = 1;
    tv_send(&v, &m);
    tv_pump(&v, 2000, NULL);
    CHECK(v.bye && v.bye_reason == SBLK_BYE_PROTOCOL && v.closed);
    tv_close(&v);
    plat_sleep_ms(100);
    CHECK(tv_connect(&v, port) == 0);
    memset(&m, 0, sizeof m);
    m.type = SBLK_T_HELLO;
    m.u.hello.role = SBLK_ROLE_VIEWER;
    m.u.hello.proto_major = 2;
    m.u.hello.max_payload = SBLK_MAX_PAYLOAD;
    m.u.hello.pixel_format = 1;
    tv_send(&v, &m);
    tv_pump(&v, 2000, NULL);
    CHECK(v.bye && v.bye_reason == SBLK_BYE_VERSION && v.closed);
    tv_close(&v);
}

int main(void)
{
    plat_log_open(NULL, getenv("SBL_TEST_LOG") != NULL);
    printf("region\n");   test_region();
    printf("encoder\n");  test_encoder();
    printf("flow\n");     test_flow();
    printf("input\n");    test_input();
    printf("usb receive (transfer boundaries)\n"); test_usb_rx();
    printf("end to end (TCP loopback, synthetic desktop)\n"); test_e2e();
    sbl_sender_stop(&g_s);
    printf("%ld checks, %ld failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
