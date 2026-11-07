/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link end-to-end check: the "mostly static desktop with a moving window" (scene.h).
 * Text is drawn as deterministic pseudo-glyphs with three grey levels (anti-aliased text compresses worse
 * than flat colour, so this is not an optimistic picture). The wallpaper is a 2-D gradient, not a photo:
 * a photo wallpaper costs more where the window uncovers it.
 */
#include "scene.h"

#include <stdlib.h>
#include <string.h>

#define TASKBAR_H 48

static uint32_t hash32(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16;
    return a;
}

static inline void put(uint8_t *px, size_t stride, int32_t x, int32_t y, uint32_t c)
{
    uint8_t *p = px + (size_t)y * stride + (size_t)x * 4;
    p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); p[3] = 0xFF;
}

static void fill(uint8_t *px, size_t stride, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
    for (int32_t j = y; j < y + h; j++)
        for (int32_t i = x; i < x + w; i++) put(px, stride, i, j, c);
}

/* pseudo text: glyph cells 7x12 on 20 px lines, words of 2..9 glyphs, ragged line ends */
static void text_block(uint8_t *px, size_t stride, int32_t x0, int32_t y0, int32_t w, int32_t h, uint32_t seed,
                       uint32_t bgc)
{
    static const uint32_t ink[4] = { 0x1E1E1E, 0x6A6A6A, 0xB4B4B4, 0 };
    int32_t lines = h / 20;
    for (int32_t l = 0; l < lines; l++) {
        uint32_t lh = hash32(seed * 977u + (uint32_t)l);
        int32_t len = (int32_t)(lh % (uint32_t)(w > 40 ? w - 20 : 20)) + 20;
        if (lh % 7u == 0) len = 0;                                   /* empty line */
        if (len > w) len = w;
        int32_t gx = 0;
        while (gx + 7 <= len) {
            uint32_t wh = hash32(lh + (uint32_t)gx * 31u);
            int32_t word = 2 + (int32_t)(wh % 8u);
            for (int32_t g = 0; g < word && gx + 7 <= len; g++, gx += 7) {
                uint32_t gh = hash32(wh ^ (uint32_t)g * 0x9E3779B9u);
                for (int32_t yy = 2; yy < 12; yy++)
                    for (int32_t xx = 1; xx < 6; xx++) {
                        uint32_t b = hash32(gh + (uint32_t)(yy * 8 + xx));
                        if (b % 5u < 2u) {
                            uint32_t c = ink[b % 3u];
                            put(px, stride, x0 + gx + xx, y0 + l * 20 + 4 + yy, c ? c : bgc);
                        }
                    }
            }
            gx += 7;                                                  /* space */
        }
    }
}

static void window(uint8_t *px, size_t stride, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t seed, int active)
{
    fill(px, stride, x, y, w, h, 0x8A8A8A);                             /* 1 px border */
    fill(px, stride, x + 1, y + 1, w - 2, 30, active ? 0xF3F3F3 : 0xE6E6E6);   /* title bar */
    fill(px, stride, x + 10, y + 10, 12, 12, 0x2B79D0);                 /* app icon */
    text_block(px, stride, x + 30, y + 3, w / 3, 20, seed ^ 0xABCD, active ? 0xF3F3F3 : 0xE6E6E6);
    for (int b = 0; b < 3; b++) fill(px, stride, x + w - 40 - b * 46, y + 14, 10, 2, 0x404040);   /* buttons */
    fill(px, stride, x + 1, y + 31, w - 2, 28, 0xFAFAFA);               /* toolbar */
    text_block(px, stride, x + 8, y + 33, w / 2, 20, seed ^ 0x1234, 0xFAFAFA);
    fill(px, stride, x + 1, y + 59, w - 2, h - 60, 0xFFFFFF);           /* client area */
    text_block(px, stride, x + 12, y + 64, w - 24, h - 70, seed, 0xFFFFFF);
}

/* 3x5 digit font */
static const uint16_t digits[10] = { 0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF };

static void draw_clock(scene *s, uint8_t *px)
{
    scene_rect c = s->clock;
    fill(px, s->stride, c.x, c.y, c.w, c.h, 0x202020);
    uint32_t v = s->clock_shown;
    char d[5] = { (char)('0' + (v / 600) % 10), (char)('0' + (v / 60) % 10), ':', (char)('0' + (v % 60) / 10),
                  (char)('0' + v % 10) };
    int32_t x = c.x + 4;
    for (int i = 0; i < 5; i++, x += 14) {
        if (d[i] == ':') { fill(px, s->stride, x + 4, c.y + 8, 3, 3, 0xE0E0E0); fill(px, s->stride, x + 4, c.y + 17, 3, 3, 0xE0E0E0); continue; }
        uint16_t g = digits[d[i] - '0'];
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (g & (1u << (14 - (r * 3 + k)))) fill(px, s->stride, x + k * 3, c.y + 4 + r * 4, 3, 4, 0xE0E0E0);
    }
}

static int32_t tri(uint64_t v, int32_t range)
{
    if (range <= 0) return 0;
    uint64_t p = v % (uint64_t)(2 * range);
    return (int32_t)(p < (uint64_t)range ? p : (uint64_t)(2 * range) - p);
}

static scene_rect place(const scene *s, uint64_t t)
{
    int32_t m = 8;
    int32_t rx = (int32_t)s->w - s->ww - 2 * m, ry = (int32_t)s->h - TASKBAR_H - s->wh - 2 * m;
    uint64_t d = t * (uint64_t)s->speed;
    scene_rect r = { m + tri(d, rx), m + tri(d * 2 / 3, ry), s->ww, s->wh };
    return r;
}

int scene_init(scene *s, uint32_t w, uint32_t h, int32_t speed)
{
    memset(s, 0, sizeof *s);
    if (w < 640 || h < 480 || w > 8192 || h > 8192) return -1;
    s->w = w; s->h = h;
    s->stride = (size_t)w * 4 + 64;
    s->px = malloc(s->stride * h);
    s->bg = malloc(s->stride * h);
    s->ww = (int32_t)(w * 46 / 100); s->wh = (int32_t)(h * 55 / 100);
    s->win = malloc((size_t)s->ww * (size_t)s->wh * 4);
    if (!s->px || !s->bg || !s->win) { scene_free(s); return -1; }
    s->speed = speed;
    s->moving = speed != 0;

    /* wallpaper: a 2-D gradient */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = 10 + x * 50 / w, g = 70 + y * 90 / h, b = 150 + (x + y) * 70 / (w + h);
            put(s->bg, s->stride, (int32_t)x, (int32_t)y, (r << 16) | (g << 8) | b);
        }
    /* desktop icons */
    for (int i = 0; i < 5 && 24 + i * 90 + 70 < (int)h - TASKBAR_H; i++) {
        fill(s->bg, s->stride, 24, 24 + i * 90, 44, 40, 0xF0C040 - (uint32_t)i * 0x101000);
        text_block(s->bg, s->stride, 14, 68 + i * 90, 64, 20, 900u + (uint32_t)i, 0x0050A0);
    }
    /* two static windows (an explorer and a settings page) */
    window(s->bg, s->stride, (int32_t)(w * 12 / 100), (int32_t)(h * 8 / 100), (int32_t)(w * 50 / 100),
           (int32_t)(h * 60 / 100), 11, 0);
    window(s->bg, s->stride, (int32_t)(w * 52 / 100), (int32_t)(h * 25 / 100), (int32_t)(w * 44 / 100),
           (int32_t)(h * 58 / 100), 22, 0);
    /* taskbar */
    fill(s->bg, s->stride, 0, (int32_t)h - TASKBAR_H, (int32_t)w, TASKBAR_H, 0x202020);
    for (int i = 0; i < 8; i++)
        fill(s->bg, s->stride, (int32_t)w / 2 - 200 + i * 50, (int32_t)h - 38, 28, 28, 0x3060A0 + (uint32_t)i * 0x0A0A00);
    s->clock = (scene_rect){ (int32_t)w - 90, (int32_t)h - 40, 80, 32 };
    s->clock_shown = 0;
    draw_clock(s, s->bg);

    /* the moving window's own image */
    size_t wst = (size_t)s->ww * 4;
    window(s->win, wst, 0, 0, s->ww, s->wh, 33, 1);

    memcpy(s->px, s->bg, s->stride * h);
    s->cur = place(s, 0);
    for (int32_t y = 0; y < s->wh; y++)
        memcpy(s->px + (size_t)(s->cur.y + y) * s->stride + (size_t)s->cur.x * 4, s->win + (size_t)y * wst, wst);
    return 0;
}

void scene_free(scene *s)
{
    free(s->px); free(s->bg); free(s->win);
    s->px = s->bg = s->win = NULL;
}

int scene_step(scene *s, uint64_t t, scene_rect *out)
{
    int n = 0;
    if (t <= s->tick) return 0;
    s->tick = t;
    uint32_t secs = (uint32_t)(t / SCENE_TICK_HZ);
    if (secs != s->clock_shown) {
        /* the clock lives in bg (the window never covers the taskbar) */
        s->clock_shown = secs;
        draw_clock(s, s->bg);
        for (int32_t y = 0; y < s->clock.h; y++)
            memcpy(s->px + (size_t)(s->clock.y + y) * s->stride + (size_t)s->clock.x * 4,
                   s->bg + (size_t)(s->clock.y + y) * s->stride + (size_t)s->clock.x * 4, (size_t)s->clock.w * 4);
        out[n++] = s->clock;
    }
    if (s->moving) {
        scene_rect o = s->cur, r = place(s, t);
        if (r.x != o.x || r.y != o.y) {
            for (int32_t y = 0; y < o.h; y++)
                memcpy(s->px + (size_t)(o.y + y) * s->stride + (size_t)o.x * 4,
                       s->bg + (size_t)(o.y + y) * s->stride + (size_t)o.x * 4, (size_t)o.w * 4);
            size_t wst = (size_t)s->ww * 4;
            for (int32_t y = 0; y < r.h; y++)
                memcpy(s->px + (size_t)(r.y + y) * s->stride + (size_t)r.x * 4, s->win + (size_t)y * wst, wst);
            s->cur = r;
            out[n++] = o;
            out[n++] = r;
        }
    }
    return n;
}

uint64_t scene_hash(const scene *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < s->h; y++) {
        const uint8_t *p = s->px + (size_t)y * s->stride;
        for (size_t i = 0; i < (size_t)s->w * 4; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    }
    return h;
}
