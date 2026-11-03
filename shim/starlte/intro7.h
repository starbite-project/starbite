/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7's intro (intro7.c): S-Boot's splash -> Liquid Glass -> the menu's first frame, the
 * concept's introAt(t) / finishIntro() (docs/concepts/ios27-boot.html), 2350 ms. Section 4.3 of
 * docs/MENU7-PORT.md.
 *
 * S-Boot has shown logo.jpg (Samsung's own splash, from UP_PARAM) since power on; the loader
 * leaves it on the panel while it bakes the wallpaper. intro7_capture then copies what is on the
 * panel (its non-black row bands, I7_CAPT) and the intro starts from exactly that picture: the
 * captured bands are over-layer images (SB7_S_CAPT0.., sb7_put) that fade out (the logo block
 * rising, as the concept's "Galaxy S9") while the concept's SAMSUNG glass wordmark fades in and
 * the wallpaper's star appears; from there it is the concept's intro. Only when nothing can be
 * captured (a black panel, too much picture, the area not free) does the intro draw the concept's
 * own splash (SAMSUNG, "Galaxy S9", "Powered by starbite") as before. Every frame is a pure
 * function of t and the captured picture: intro7_frame(t) builds the menu (m7_build with the
 * intro's struct m7_xf), the wallpaper's uniforms (struct g7_wall), the glass wordmark (struct
 * g7_word) and the splash items, and glass draws it. Any key or tap skips to the end; Bixby held
 * (700 ms) skips and opens Lab.
 *
 * Compiled with FP/SIMD. Frozen interface: a change goes through the integrator.
 */
#pragma once
#include <stdint.h>
#include "glass.h"
#include "menu7.h"

/* intro7_run's results */
#define I7_DONE 0                   /* ran to the end: the menu's first frame is on the panel */
#define I7_SKIPPED 1                /* a key or a tap ended it (finishIntro(true)) */
#define I7_LAB 2                    /* Bixby held: the caller opens Lab after finishIntro(true) */

/* Run the intro at M7_FPS from t = 0 (real time: frames are dropped, never slowed down). Calls
 * m7_prepare_boot first and m7_finish_intro at the end. */
int intro7_run(void);
/* The intro's frame at t ms into f (no drawing; the preview harness renders state=intro&t=). */
void intro7_frame(struct g7_frame *f, float t);
/* Copy S-Boot's splash off the panel (G7_FB) into I7_CAPT, before anything is drawn on it.
 * intro7_run calls it first; the preview harness calls it after putting a splash on its fake
 * panel. Returns the bands captured (the native splash path), or 0: the concept's splash. */
int intro7_capture(void);

/* ---- the captured splash (docs/MENU7-PORT.md section 3) ---- */

/* 0x83800000 .. 0x84000000, write-back, below G7_CANVAS: free while the menu runs once the BOOT
 * Image (with its appended payload) and the ramdisk are checked to lie outside it (run time).
 * Only the intro reads it: glass decodes each band into G7_CACHE at its first frame. */
#define I7_CAPT       (0x83800000UL + G7_HOSTOFF)
#define I7_CAPT_SIZE  0x00800000UL
#define I7_CAPT_BANDS 8             /* row bands (images SB7_S_CAPT0 ..); closer ones merge */
#define I7_CAPT_GAP   8             /* black rows that still join two bands */
#define I7_CAPT_MAX_PX (384U * 1024U)   /* 1.5 MiB of G7_CACHE (3 MiB) at most */
/* the timeline from the captured picture: bands above I7_NAT_SPLIT (the logo block) fade over
 * I7_GS9_FADE and rise -36 eo(t / I7_GS9_RISE), as #gs9; the ones below ("Powered by ...") fade
 * over I7_POW_FADE; the glass wordmark's alpha is c01((t - I7_NAT_WORD0) / I7_NAT_WORD_MS)
 * (white, fully in when the splash is gone; it drains from I7_DRAIN0 as in the concept). */
#define I7_NAT_SPLIT  1973          /* 2 / 3 of the panel */
#define I7_NAT_WORD0  100
#define I7_NAT_WORD_MS 160

/* ---- the timeline (ms), the concept's introAt; e = m7_ease, eo = m7_eout, c01 = clamp to 0..1 ---- */

#define I7_MS 2350                  /* INTRO_MS */
/* splash, over layer (above the menu): */
#define I7_SAM_FADE 220             /* SAMSUNG (white) opacity 1 - t / 220 */
#define I7_GS9_FADE 260             /* "Galaxy S9" opacity 1 - t / 260, rises -36 * eo(t / 420) px */
#define I7_GS9_RISE 420
#define I7_POW_FADE 200             /* "Powered by", "starbite" 1 - t / 200 */
#define I7_STAR_FADE 120            /* the wordmark's star 1 - t / 120 (it becomes the wallpaper's star) */
/* the wallpaper's star flies a quadratic path star0 -> C -> STAR: */
#define I7_FLY0 120                 /* e = ease((t - 120) / 950) */
#define I7_FLY_MS 950
#define I7_FLY_CX 1390.0f           /* control point */
#define I7_FLY_CY 1520.0f
#define I7_BLOOM0 950               /* bloom = sin(pi * c01((t - 950) / 600)) */
#define I7_BLOOM_MS 600
/* wall: si = c01(t / 120) * (1 + .45 bloom); sr = lerp(star0 r, 54, e((t - 180) / 900));
 *       sw = .72 * e((t - 250) / 700); rr = t < 600 ? 0 : 3900 * eo((t - 600) / 1000); rf 800; amb 1 */
#define I7_SI_RAMP 120
#define I7_SR0 180
#define I7_SR_MS 900
#define I7_SW0 250
#define I7_SW_MS 700
#define I7_REV0 600
#define I7_REV_MS 1000
#define I7_REV_R 3900.0f
#define I7_REV_FEATHER 800.0f
/* dim = lerp(1, .8, e((t - 1300) / 700)) */
#define I7_DIM0 1300
#define I7_DIM_MS 700
/* glass wordmark: tf = 1 - e((t - 220) / 520); tm = e((t - 700) / 350); ta = t < 1050;
 * cap = (720, 1380, lerp(1180, 940, tm), lerp(170, 136, tm)) */
#define I7_DRAIN0 220
#define I7_DRAIN_MS 520
#define I7_MELT0 700
#define I7_MELT_MS 350
#define I7_CAP_X 720.0f
#define I7_CAP_Y 1380.0f
#define I7_CAP_W0 1180.0f
#define I7_CAP_W1 940.0f
#define I7_CAP_H0 170.0f
#define I7_CAP_H1 136.0f
/* the capsule becomes the island at 1050: pill glass alpha 1, scale 1,
 * pill_dy = 1136 * (1 - spA((t - 1050) / 1000 s, .45, .80)); pillIn opacity c01((t - 1250) / 300) */
#define I7_ISLAND0 1050
#define I7_ISLAND_DY 1136.0f
#define I7_ISLAND_IN0 1250
#define I7_ISLAND_IN_MS 300
/* menu: ta = spA((t - 1330) / 1000, .60, .86): brand, title dy 90 (1 - ta), opacity c01(1.3 ta);
 * subtitle dy 110 (1 - spA((t - 1380) / 1000, .6, .86)), opacity c01((t - 1380) / 400);
 * foot c01((t - 1700) / 400); status c01((t - 1350) / 400);
 * section i: s = spA((t - 1400 - 55 i) / 1000, .55, .80): dy 320 (1 - s), scale .96 + .04 s,
 *            alpha c01(1.6 s); dock: s = spA((t - 1550) / 1000, .55, .80): dy 340 (1 - s),
 *            glass alpha c01(1.6 s); lens alpha c01((t - 1850) / 300) */
#define I7_TITLE0 1330
#define I7_TITLE_DY 90.0f
#define I7_SUB0 1380
#define I7_SUB_DY 110.0f
#define I7_FOOT0 1700
#define I7_STATUS0 1350
#define I7_CARDS0 1400
#define I7_CARD_STAGGER 55
#define I7_CARD_DY 320.0f
#define I7_DOCK0 1550
#define I7_DOCK_DY 340.0f
#define I7_LENS0 1850
#define I7_FADE_MS 400
