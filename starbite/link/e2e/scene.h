/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link end-to-end check: a "mostly static desktop with a moving window" for measuring the real
 * sender pipeline (win/sender) on the Mac. Wallpaper, taskbar with a clock, two static windows with text,
 * and one text window that is dragged around continuously. The scene is drawn incrementally, as a
 * compositor would: each step restores the old window area, draws the window at its new place, and
 * reports exactly those areas as dirty (what DXGI desktop duplication reports for a drag).
 */
#ifndef SB_SCENE_H
#define SB_SCENE_H

#include <stddef.h>
#include <stdint.h>

#define SCENE_TICK_HZ 60u        /* the "compositor" refresh: one step per tick */
#define SCENE_MAX_DIRTY 4

typedef struct scene_rect { int32_t x, y, w, h; } scene_rect;

typedef struct scene {
    uint32_t w, h;
    size_t   stride;             /* bytes per row of px/bg (padded, like a mapped texture) */
    uint8_t *px;                 /* the current desktop image, BGRA */
    uint8_t *bg;                 /* the desktop without the moving window */
    uint8_t *win;                /* the moving window's own image (ww x wh, tight) */
    int32_t  ww, wh;             /* moving window size */
    uint64_t tick;               /* scene time in ticks */
    int      moving;             /* 0: the window stays put (only the clock changes) */
    int32_t  speed;              /* pixels per tick along the path */
    scene_rect cur;              /* where the moving window is now */
    uint32_t clock_shown;        /* clock value drawn (seconds) */
    scene_rect clock;
} scene;

int  scene_init(scene *s, uint32_t w, uint32_t h, int32_t speed);
void scene_free(scene *s);
/* Advances the scene to tick t (>= s->tick; any jump is one redraw, as a compositor that skipped frames).
 * Writes the dirty areas (0..SCENE_MAX_DIRTY) to out and returns how many. */
int  scene_step(scene *s, uint64_t t, scene_rect *out);
/* FNV-1a 64 over the visible bytes, rows top to bottom (same as the Mac viewer's sbv_fb_hash). */
uint64_t scene_hash(const scene *s);

#endif
