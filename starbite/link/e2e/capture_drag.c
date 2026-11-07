/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link end-to-end check: a capture backend for the REAL sender (win/sender: session, encoder,
 * flow control, TCP) that shows scene.h, a mostly static desktop with one window dragged around, with
 * compositor-style dirty rects. Drop-in for win/sender/capture_synth.c (same sbl_cap_* and the synth_*
 * calls main.c makes), so `sblinkd-drag` is sblinkd-host with a realistic desktop.
 *
 * Environment (main.c's options are not changed):
 *   SBLINK_SCENE_SPEED  pixels per 60 Hz tick the window moves (default 4 = 240 px/s; 0 = static desktop)
 *   SBLINK_SCENE_STOP   after this many seconds the whole desktop stops changing, clock included (default:
 *                       never): checks that a static desktop costs nothing and gives a stable final image
 *   SBLINK_SCENE_HASH   file: once stopped, "<W>x<H> <hash>" of the desktop (FNV-1a 64 as the viewer's)
 * --synth-auto FPS sets the compositor tick rate (60 = a 60 Hz desktop); 0 = static.
 */
#include "../win/sender/host.h"
#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t dg_w = 1280, dg_h = 720;
static volatile uint32_t dg_want_w, dg_want_h;
static int dg_hz;
static uint64_t dg_caps;

void synth_set_auto(int fps) { dg_hz = fps; }
void synth_set_size(uint32_t w, uint32_t h) { dg_w = w; dg_h = h; }
void synth_get_size(uint32_t *w, uint32_t *h) { *w = dg_w; *h = dg_h; }
uint64_t synth_captures(void) { return dg_caps; }

struct sbl_cap {
    scene sc;
    uint64_t t0;
    uint64_t stop_tick;           /* 0 = never */
    int stopped, hashed;
};

static long envnum(const char *name, long def)
{
    const char *v = getenv(name);
    return v && *v ? strtol(v, NULL, 10) : def;
}

int sbl_cap_open(sbl_cap **cp, const sbl_cfg *cfg, sbl_cap_info *info)
{
    (void)cfg;
    if (dg_want_w) { dg_w = dg_want_w; dg_h = dg_want_h; dg_want_w = dg_want_h = 0; }
    sbl_cap *c = calloc(1, sizeof *c);
    if (!c) return -1;
    if (scene_init(&c->sc, dg_w, dg_h, (int32_t)envnum("SBLINK_SCENE_SPEED", 4)) < 0) { free(c); return -1; }
    long stop = envnum("SBLINK_SCENE_STOP", 0);
    c->stop_tick = stop > 0 ? (uint64_t)stop * SCENE_TICK_HZ : 0;
    c->t0 = plat_now_us();
    memset(info, 0, sizeof *info);
    info->width = dg_w; info->height = dg_h; info->refresh_mhz = 60000;
    info->is_starbite = 1;
    info->can_mode = 1;
    info->geom.out_w = (int32_t)dg_w; info->geom.out_h = (int32_t)dg_h;
    info->geom.disp_w = dg_w; info->geom.disp_h = dg_h;
    snprintf(info->name, sizeof info->name, "Starbite Display (drag scene)");
    info->method = "drag-scene";
    *cp = c;
    return 0;
}

void sbl_cap_close(sbl_cap *c)
{
    if (!c) return;
    scene_free(&c->sc);
    free(c);
}

static void write_hash(sbl_cap *c)
{
    const char *path = getenv("SBLINK_SCENE_HASH");
    if (!path || !*path || c->hashed) return;
    c->hashed = 1;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ux%u %016llx\n", c->sc.w, c->sc.h, (unsigned long long)scene_hash(&c->sc));
    fclose(f);
    /* the image itself next to it (<path>.bgra, tight rows), to locate a mismatch */
    char raw[1024];
    snprintf(raw, sizeof raw, "%s.bgra", path);
    FILE *r = fopen(raw, "wb");
    if (r) {
        for (uint32_t y = 0; y < c->sc.h; y++) fwrite(c->sc.px + (size_t)y * c->sc.stride, 4, c->sc.w, r);
        fclose(r);
    }
    rename(tmp, path);
}

int sbl_cap_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us)
{
    if (dg_want_w) return SBL_CAP_RESET;
    if (dg_hz <= 0 || c->stopped) { if (timeout_ms) plat_sleep_ms(timeout_ms); return 0; }   /* nothing moves */
    uint64_t now = plat_now_us();
    uint64_t due = (now - c->t0) * (uint64_t)dg_hz / 1000000u;
    if (due <= c->sc.tick) {
        /* wait for the next compositor tick, if it comes within the timeout */
        uint64_t next_us = c->t0 + (c->sc.tick + 1) * 1000000u / (uint64_t)dg_hz;
        uint64_t wait_ms = (next_us - now + 999) / 1000;
        if ((int)wait_ms > timeout_ms) { if (timeout_ms) plat_sleep_ms(timeout_ms); return 0; }
        plat_sleep_ms((int)wait_ms);
        if (dg_want_w) return SBL_CAP_RESET;
        due = c->sc.tick + 1;
    }
    /* the scene's own clock is 60 Hz; dg_hz only sets how often the "compositor" draws */
    uint64_t t = due * SCENE_TICK_HZ / (uint64_t)dg_hz;
    if (c->stop_tick && t >= c->stop_tick && !c->stopped) {
        /* last step to exactly the stop position, then the window stays */
        scene_rect r[SCENE_MAX_DIRTY];
        int n = scene_step(&c->sc, c->stop_tick, r);
        c->sc.moving = 0;
        c->stopped = 1;
        for (int i = 0; i < n; i++) sbl_region_add(dirty, r[i].x, r[i].y, r[i].w, r[i].h, c->sc.w, c->sc.h);
        write_hash(c);
        *capture_us = plat_now_us();
        dg_caps++;
        return n > 0;
    }
    scene_rect r[SCENE_MAX_DIRTY];
    int n = scene_step(&c->sc, t > c->sc.tick ? t : c->sc.tick + 1, r);
    for (int i = 0; i < n; i++) sbl_region_add(dirty, r[i].x, r[i].y, r[i].w, r[i].h, c->sc.w, c->sc.h);
    if (n == 0) return 0;
    dg_caps++;
    *capture_us = plat_now_us();
    return 1;
}

int sbl_cap_valid(sbl_cap *c) { return c != NULL; }

int sbl_cap_map(sbl_cap *c, const uint8_t **px, size_t *stride)
{
    *px = c->sc.px;
    *stride = c->sc.stride;
    return 0;
}
void sbl_cap_unmap(sbl_cap *c) { (void)c; }

int sbl_cap_set_mode(sbl_cap *c, uint32_t w, uint32_t h, uint32_t refresh_mhz)
{
    (void)refresh_mhz;
    if (w < 640 || h < 480 || w > 4096 || h > 2304) return -1;
    w &= ~1u; h &= ~1u;
    if (w == c->sc.w && h == c->sc.h) return 0;
    dg_want_h = h;
    dg_want_w = w;
    return 1;
}
