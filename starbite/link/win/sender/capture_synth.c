/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd host build: a synthetic "desktop" behind the capture interface of sender.h, with OS-style
 * dirty rects (exact areas, and every 7th frame the whole screen, as a real OS sometimes does).
 * Lets the Mac viewer app and test_host.c talk to the real sender code without the phone.
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

static plat_mutex sy_lk;
static int sy_init;
static plat_event sy_ev;
static uint32_t sy_scene, sy_w = 1280, sy_h = 720, sy_want_w, sy_want_h;
static int sy_auto;
static uint64_t sy_caps;

static void sy_once(void)
{
    if (sy_init) return;
    sy_init = 1;
    plat_mutex_init(&sy_lk);
    sy_ev = plat_event_new();
}

typedef struct box { int32_t x, y, w, h; } box;

static box box_of(uint32_t k, uint32_t w, uint32_t h)
{
    box b;
    uint32_t sx = w > 200 ? w - 200 : 1, sy = h > 200 ? h - 200 : 1;
    b.x = (int32_t)(20 + (k * 24u) % sx);
    b.y = (int32_t)(40 + (k * 16u) % sy);
    b.w = 160; b.h = 120;
    return b;
}
static box counter_of(uint32_t w, uint32_t h)
{
    box b = { (int32_t)w - 220, (int32_t)h - 40, 200, 32 };
    if (b.x < 0) b.x = 0;
    if (b.y < 0) b.y = 0;
    return b;
}

static void put(uint8_t *px, size_t stride, uint32_t x, uint32_t y, uint32_t c)
{
    uint8_t *p = px + (size_t)y * stride + (size_t)x * 4;
    p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); p[3] = 0xFF;
}

void synth_render(uint32_t k, uint32_t w, uint32_t h, uint8_t *px, size_t stride)
{
    uint32_t wx0 = 100, wy0 = 80, wx1 = w > 200 ? (w - 100 < 900 ? w - 100 : 900) : w, wy1 = h > 180 ? (h - 100 < 600 ? h - 100 : 600) : h;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c;
            if (y + 48 >= h) c = 0x202020;                                  /* taskbar */
            else if (x >= wx0 && x < wx1 && y >= wy0 && y < wy1) {           /* a window with text lines */
                uint32_t line = (y - wy0) / 20, in = (y - wy0) % 20;
                c = (in >= 6 && in < 14 && x > wx0 + 10 && x < wx1 - 10 && ((x / 7 + line) % 3)) ? 0x303030 : 0xFFFFFF;
                if (y < wy0 + 28) c = 0xE0E0E0;                              /* title bar */
            } else c = 0x0050A0u + ((y / 8) % 32) * 0x020100u;                /* wallpaper bands */
            put(px, stride, x, y, c);
        }
    box b = box_of(k, w, h);
    for (int32_t y = b.y; y < b.y + b.h && y < (int32_t)h; y++)
        for (int32_t x = b.x; x < b.x + b.w && x < (int32_t)w; x++)
            put(px, stride, (uint32_t)x, (uint32_t)y, (((uint32_t)(x + y) + k) & 15u) * 0x101008u + 0x400000u);
    box c = counter_of(w, h);
    for (int32_t y = c.y; y < c.y + c.h && y < (int32_t)h; y++)
        for (int32_t x = c.x; x < c.x + c.w && x < (int32_t)w; x++) {
            uint32_t bit = (uint32_t)(x - c.x) / 8u;
            put(px, stride, (uint32_t)x, (uint32_t)y, ((k >> (bit % 24u)) & 1u) ? 0x00FF00 : 0x101010);
        }
}

void synth_set_scene(uint32_t k)
{
    sy_once();
    plat_mutex_lock(&sy_lk);
    sy_scene = k;
    plat_mutex_unlock(&sy_lk);
    plat_event_set(sy_ev);
}
uint32_t synth_scene(void)
{
    sy_once();
    plat_mutex_lock(&sy_lk);
    uint32_t k = sy_scene;
    plat_mutex_unlock(&sy_lk);
    return k;
}
void synth_set_auto(int fps) { sy_once(); sy_auto = fps; plat_event_set(sy_ev); }
void synth_set_size(uint32_t w, uint32_t h) { sy_once(); sy_w = w; sy_h = h; }
void synth_get_size(uint32_t *w, uint32_t *h) { *w = sy_w; *h = sy_h; }
uint64_t synth_captures(void) { return sy_caps; }

struct sbl_cap {
    uint32_t w, h, shown;      /* shown = the scene in px */
    uint8_t *px;
    size_t stride;
    uint64_t last_auto;
    uint32_t nframes;
};

int sbl_cap_open(sbl_cap **cp, const sbl_cfg *cfg, sbl_cap_info *info)
{
    (void)cfg;
    sy_once();
    if (sy_want_w) { sy_w = sy_want_w; sy_h = sy_want_h; sy_want_w = sy_want_h = 0; }
    sbl_cap *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->w = sy_w; c->h = sy_h;
    c->stride = (size_t)c->w * 4 + 64;   /* padded rows, like a mapped texture */
    c->px = malloc(c->stride * c->h);
    if (!c->px) { free(c); return -1; }
    c->shown = synth_scene();
    synth_render(c->shown, c->w, c->h, c->px, c->stride);
    memset(info, 0, sizeof *info);
    info->width = c->w; info->height = c->h; info->refresh_mhz = 60000;
    info->is_starbite = 1;
    info->can_mode = 1;
    info->geom.out_x = 0; info->geom.out_y = 0;
    info->geom.out_w = (int32_t)c->w; info->geom.out_h = (int32_t)c->h;
    info->geom.disp_w = c->w; info->geom.disp_h = c->h;
    memcpy(info->name, "Starbite Display (synthetic)", 29);
    info->method = "synth";
    *cp = c;
    return 0;
}

void sbl_cap_close(sbl_cap *c)
{
    if (!c) return;
    free(c->px);
    free(c);
}

int sbl_cap_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us)
{
    if (sy_want_w) return SBL_CAP_RESET;
    uint64_t now = plat_now_us();
    if (sy_auto > 0) {
        uint64_t period = 1000000u / (uint64_t)sy_auto;
        if (now - c->last_auto < period) {
            uint64_t left = (period - (now - c->last_auto)) / 1000u;
            if ((int)left > timeout_ms) { if (timeout_ms) plat_event_wait(sy_ev, timeout_ms); return 0; }
            plat_sleep_ms((int)left);
        }
        c->last_auto = plat_now_us();
        plat_mutex_lock(&sy_lk);
        sy_scene++;
        plat_mutex_unlock(&sy_lk);
    }
    uint32_t k = synth_scene();
    if (k == c->shown) {
        if (timeout_ms) plat_event_wait(sy_ev, timeout_ms);
        k = synth_scene();
        if (sy_want_w) return SBL_CAP_RESET;
        if (k == c->shown) return 0;
    }
    synth_render(k, c->w, c->h, c->px, c->stride);
    if (++c->nframes % 7 == 0) {
        sbl_region_add(dirty, 0, 0, (int32_t)c->w, (int32_t)c->h, c->w, c->h);
    } else {
        box a = box_of(c->shown, c->w, c->h), b = box_of(k, c->w, c->h), n = counter_of(c->w, c->h);
        sbl_region_add(dirty, a.x, a.y, a.w, a.h, c->w, c->h);
        sbl_region_add(dirty, b.x, b.y, b.w, b.h, c->w, c->h);
        sbl_region_add(dirty, n.x, n.y, n.w, n.h, c->w, c->h);
    }
    c->shown = k;
    sy_caps++;
    *capture_us = plat_now_us();
    return 1;
}

int sbl_cap_valid(sbl_cap *c) { return c != NULL; }

int sbl_cap_map(sbl_cap *c, const uint8_t **px, size_t *stride)
{
    *px = c->px;
    *stride = c->stride;
    return 0;
}
void sbl_cap_unmap(sbl_cap *c) { (void)c; }

int sbl_cap_set_mode(sbl_cap *c, uint32_t w, uint32_t h, uint32_t refresh_mhz)
{
    (void)refresh_mhz;
    if (w < 64 || h < 64 || w > 4096 || h > 2304) return -1;
    w &= ~1u; h &= ~1u;   /* "nearest supported mode": even sizes */
    if (w == c->w && h == c->h) return 0;
    sy_want_w = w; sy_want_h = h;
    plat_event_set(sy_ev);
    return 1;
}
