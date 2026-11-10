/* SPDX-License-Identifier: MIT
 * sbtk: the Starbite OS UI toolkit (P1 of the UI port plan, starbite-os-ui-port-plan.md). Freestanding C: it
 * includes only this header, tk_plat.h (allocation, clock, log) and <arm_neon.h>, so the same files build into the
 * kernel (P2) and into the host harness (ui/host). The design follows the approved shell concept
 * (docs/concepts/starbite-os-shell/index.html):
 *   - every layer is drawn once into a cached premultiplied bitmap; a frame only composites the cached layers with
 *     their current translate / scale / opacity / clip (the compositor, tk_comp.c);
 *   - Liquid Glass is baked per element (tint, rims, specular, border, shadow) and its backdrop is the wallpaper
 *     blurred once (blur 14 pt, saturate 1.9, brightness 1.06), sampled under the element where it is now;
 *   - motion frames composite at half resolution and are scaled 2x to the panel; the first frame at rest is full;
 *   - the springs and the gestures are the concept's own (SPR, the 100 ms velocity window, the projections).
 * Units: panel px (1440 x 2960) unless a name ends in _pt (the concept's 390-pt-wide logical unit, x 1440/390). */
#ifndef SBTK_H
#define SBTK_H
#include <stdint.h>
#include <stddef.h>

#define TK_W 1440
#define TK_H 2960
#define TK_PT (1440.0f / 390.0f)          /* px per concept pt */
#define TK_HPT 802.0f                      /* the concept's phone height in pt (bottom-anchored things use it) */
#define PT(v) ((float)(v) * TK_PT)

/* ---- colours and bitmaps ------------------------------------------------------------------------------------ */
typedef struct { float r, g, b, a; } tk_col;          /* straight alpha, r g b in 0..255, a in 0..1 (CSS rgba) */
#define TK_RGBA(r, g, b, a) ((tk_col){ (r), (g), (b), (a) })
tk_col tk_hex(uint32_t rgb, float a);                  /* 0xRRGGBB */

typedef struct { int w, h, stride; uint32_t *px; } tk_bmp;    /* premultiplied 0xAARRGGBB, stride in pixels */
typedef struct { int w, h; float *v; } tk_fm;                  /* a float coverage / alpha plane, 0..1 */
typedef struct { int w, h; uint8_t *v; } tk_a8;                /* an 8-bit mask */
typedef struct { float x, y, w, h; } tk_rect;
typedef struct { int x0, y0, x1, y1; } tk_irect;               /* half-open */

int tk_bmp_new(tk_bmp *b, int w, int h);                       /* zeroed (transparent); 0 = no memory */
void tk_bmp_free(tk_bmp *b);
int tk_fm_new(tk_fm *m, int w, int h);
void tk_fm_free(tk_fm *m);
int tk_a8_new(tk_a8 *m, int w, int h);
void tk_a8_free(tk_a8 *m);
uint32_t tk_premul(tk_col c);

/* ---- paints: what colour a pixel gets (bake time, float) ---------------------------------------------------- */
#define TK_MAXSTOP 6
typedef struct { float pos; tk_col c; } tk_stop;
enum { TK_P_SOLID, TK_P_LINEAR, TK_P_RADIAL };
typedef struct {
    int kind, n;
    tk_col solid;
    tk_stop s[TK_MAXSTOP];
    float bx, by, bw, bh;      /* the gradient's box (CSS background box) in the target's px */
    float angle;               /* linear: CSS degrees (180 = top to bottom) */
    float rx, ry, cx, cy;      /* radial: radii and centre as fractions of the box (CSS "rx% ry% at cx% cy%") */
} tk_paint;
tk_paint tk_solid(tk_col c);
tk_paint tk_linear(float angle, float bx, float by, float bw, float bh, int n, const tk_stop *s);
tk_paint tk_radial(float rx, float ry, float cx, float cy, float bx, float by, float bw, float bh, int n, const tk_stop *s);
tk_col tk_paint_at(const tk_paint *p, float x, float y);       /* premultiplied interpolation, as CSS */

/* ---- coverage planes and drawing (bake time) ---------------------------------------------------------------- */
typedef struct { float tl, tr, br, bl; } tk_r4;
extern tk_r4 tk_corner;            /* per-corner radii for every rounded box while tk_use_r4 (else the r given) */
extern int tk_use_r4;
void tk_fm_clear(tk_fm *m, float v);
void tk_fm_rrect(tk_fm *m, float ox, float oy, float x, float y, float w, float h, float r);   /* max-combine AA */
void tk_fm_circle(tk_fm *m, float ox, float oy, float cx, float cy, float r);
/* a stroked arc (a ring's progress): centre, radius, stroke width, from angle a0 (radians, 0 = 12 o'clock,
 * clockwise) over sweep; round caps if caps */
void tk_fm_arc(tk_fm *m, float ox, float oy, float cx, float cy, float r, float sw, float a0, float sweep, int caps);
void tk_fm_blur(tk_fm *m, float sigma);                        /* a gaussian (three box passes) */
void tk_fm_mul(tk_fm *m, const tk_fm *k, int invert);          /* m *= k (or 1 - k) */
void tk_fm_shift(tk_fm *dst, const tk_fm *src, float dx, float dy);   /* bilinear shift */
/* draw paint through coverage: target pixel (ox + i, oy + j) gets paint(ox + i, oy + j) x m(i, j) x alpha over */
void tk_draw_fm(tk_bmp *b, const tk_fm *m, int ox, int oy, const tk_paint *p, float alpha);
void tk_fill_rrect(tk_bmp *b, float x, float y, float w, float h, float r, const tk_paint *p, float alpha);
void tk_fill_rect_raw(tk_bmp *b, int x, int y, int w, int h, uint32_t pm);   /* copy, no blend */
/* CSS box-shadow, outer (clipped to outside the box, as Chrome) or inset; offsets / blur / spread in px */
void tk_box_shadow(tk_bmp *b, float x, float y, float w, float h, float r, float dx, float dy, float blur,
                   float spread, tk_col c, int inset);
/* a premultiplied image over the target at an integer position, times alpha */
void tk_blit(tk_bmp *dst, const tk_bmp *src, int x, int y, float alpha);
void tk_blit_tinted(tk_bmp *dst, const tk_bmp *src, int x, int y, tk_col tint);   /* src alpha x tint colour */
/* CSS filters on a premultiplied region: blur (gaussian sigma px, edges mirrored), saturate, brightness */
void tk_bmp_blur(tk_bmp *b, float sigma);
void tk_bmp_satbright(tk_bmp *b, float sat, float bright);
void tk_bmp_copy(tk_bmp *dst, int dx, int dy, const tk_bmp *src, int sx, int sy, int w, int h);
void tk_bmp_down2(tk_bmp *dst, const tk_bmp *src);              /* 2x2 box (premultiplied) */

/* ---- text (stb_truetype on static instances; tk_text.c) ----------------------------------------------------- */
enum { TK_IT400, TK_IT500, TK_IT600, TK_IT700, TK_IT800, TK_OF300, TK_NFONT };
#define TK_TNUM 1          /* tabular figures (font-variant-numeric: tabular-nums) */
int tk_font_load(int id, const uint8_t *ttf, const uint8_t *lay, uint32_t laylen);
float tk_ascent(int font, float size);                 /* hhea ascent / descent (> 0) in px for a size in px */
float tk_descent(int font, float size);
float tk_text_w(int font, float size, const char *s, float ls, int flags);   /* advance incl. ls after each */
/* coverage of a UTF-8 string into m, pen at (x, baseline) relative to m's origin */
void tk_text_fm(tk_fm *m, int font, float size, float x, float baseline, const char *s, float ls, int flags);
/* convenience: text with an optional CSS text-shadow (sh.a > 0) drawn over b */
typedef struct { float dx, dy, blur; tk_col c; } tk_shadow;
void tk_text(tk_bmp *b, int font, float size, float x, float baseline, const char *s, float ls, int flags,
             const tk_paint *p, const tk_shadow *sh);
/* CSS line box: the baseline of a line of line-height lh px whose box starts at top (half-leading model) */
float tk_baseline(int font, float size, float top, float lh);
float tk_normal_lh(int font, float size);

/* ---- the asset pack (icons, fonts) --------------------------------------------------------------------------- */
int tk_pack_open(const uint8_t *pak, uint32_t len);    /* loads the fonts; icons stay in place */
const tk_bmp *tk_icon(const char *name);               /* NULL if missing */
float tk_icon_pad(const char *name);                   /* the margin baked around it (glows), px */

/* ---- Liquid Glass (tk_glass.c) ----------------------------------------------------------------------------- */
/* the wallpaper: the concept's .wall.n (inset -30 pt, three radials over a vertical gradient) and the star */
void tk_wall_draw(tk_bmp *b, int emergency);
/* the backdrop the glass shows: blur 14 pt, saturate 1.9, brightness 1.06 of src */
void tk_glass_backdrop(tk_bmp *dst, const tk_bmp *src);
/* bake one .lg element into b at (x, y, w, h) with radius r: the drop shadow (outside), the tint gradient, the inset
 * rims and glow, the specular radials and the 1 pt gradient border; dark = .lg.dark. Its backdrop mask (the shape's
 * coverage) goes into mask (same frame as b) if given. */
void tk_glass_bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow);
/* the glass's backdrop filter applied in place to what b already holds under the shape (glass inside a backdrop
 * root, as the lock list's cards: Chrome filters only the list's own content, not the wallpaper) */
void tk_glass_local(tk_bmp *dst, const tk_bmp *src, float x, float y, float w, float h, float r);
/* rotate a small premultiplied bitmap about its centre (bilinear) into dst of the same size: a chevron's turn */
void tk_bmp_rotate(tk_bmp *dst, const tk_bmp *src, float deg);
/* CSS cubic-bezier(x1, y1, x2, y2) at t */
float tk_bezier(float x1, float y1, float x2, float y2, float t);


/* ---- layers and the compositor (tk_comp.c) ----------------------------------------------------------------- */
typedef struct tk_layer tk_layer;
struct tk_layer {
    const char *name;
    tk_bmp bmp;                /* cached content, premultiplied; may be empty (a group) */
    tk_bmp half;               /* its 2x2-downscaled copy for motion frames (made on first use) */
    tk_a8 glass, ghalf;        /* backdrop mask: the wallpaper glass shows through it (same frame as bmp) */
    int glass_opaque_only;     /* the backdrop only while the composed opacity is 1 (Chrome's backdrop root) */
    float x, y;                /* bitmap origin in the parent's space, px */
    float bx, by;              /* the owner's rest position (the shell moves layers relative to it) */
    float sx, sy, ox, oy;      /* scale about (ox, oy) in the parent's space; 1 = none */
    float alpha;
    int hidden;
    int clip;                  /* clip children and self to clip_r (parent space) */
    tk_rect clip_r;
    float fade_top, fade_bot;  /* mask-image fades inside clip_r (px, 0 = none) */
    float clip_radius;         /* rounded clip (px; 0 = square) */
    int slice;                 /* 3-slice stretch, 1 vertical / 2 horizontal: drawn slice_h long, the first s0 and last s1 px kept */
    float slice_h, slice_s0, slice_s1;
    tk_layer *child, *next;
    uint32_t gen;              /* bumped when bmp changes (redraw its area) */
    /* compositor state */
    tk_irect last;             /* screen rect drawn last frame */
    uint32_t last_gen;
    float last_key[8];
};
void tk_layer_init(tk_layer *l, const char *name);
void tk_layer_add(tk_layer *parent, tk_layer *child);     /* appended on top */
void tk_layer_dirty(tk_layer *l);                         /* its bitmap changed */
void tk_layer_prepare(tk_layer *l);                       /* make its half copy now (idle time), not on first motion */

typedef struct {
    tk_bmp fb;                 /* the panel (host: RAM; device: the framebuffer) */
    tk_bmp base, base_half;    /* wallpaper + star, full and half */
    tk_bmp bw, bw_half;        /* the glass backdrop of base */
    tk_bmp half;               /* the half-resolution motion canvas */
    tk_layer *root;
    int motion;                /* the next frame is a motion frame (half resolution) */
    tk_irect moved;            /* what motion frames covered since the last full frame */
    int have_moved;
    /* stats of the last frame */
    int last_half, last_px;
    uint64_t last_us, us_comp, us_present;
} tk_comp;
int tk_comp_init(tk_comp *c);
/* render a group's children (as if the group were at identity) into a screen-sized transparent bitmap and their
 * glass masks into mask: one cached layer for a group that moves or fades as one (CALayer's shouldRasterize) */
void tk_flatten(tk_layer *group, tk_bmp *out, tk_a8 *mask);
/* draw a frame: motion = half resolution and 2x present; full = redraw everything; returns the drawn px */
int tk_comp_frame(tk_comp *c, int motion, int full);

/* ---- motion: springs, the velocity tracker, gestures (tk_motion.c) ------------------------------------------ */
enum { TK_SPR_UNLOCK, TK_SPR_OPEN, TK_SPR_CLOSE, TK_SPR_SHEET, TK_SPR_ROWS, TK_SPR_CC, TK_NSPR };
typedef struct {
    float x, v, to, eps, k, c;
    int live;
} tk_spring;
/* the concept's spring(): mass 1, eps = max(1e-3, |to - from| x 0.002), velocity in units per s */
void tk_spring_start(tk_spring *s, float from, float to, float v0, int kind);
/* one rAF tick: dt in s (capped at 0.05), 4 semi-implicit Euler substeps; returns 1 while live, 0 when it rested
 * this tick (x is then exactly to) */
int tk_spring_step(tk_spring *s, float dt);

typedef struct { float y[64]; float t[64]; int n; } tk_tracker;      /* the last 100 ms of one coordinate */
void tk_tracker_reset(tk_tracker *t);
void tk_tracker_add(tk_tracker *t, float y, float t_ms);
float tk_tracker_v(const tk_tracker *t);                             /* units per ms, as the concept's v() */

/* ---- platform ---------------------------------------------------------------------------------------------- */
void *tk_alloc(size_t n);          /* zeroed, 64-byte aligned */
void tk_free(void *p);
uint64_t tk_us(void);
void tk_log(const char *s);

/* ---- math (tk_math.c; no libm) ------------------------------------------------------------------------------ */
float tk_sqrtf(float x);
float tk_floorf(float x);
float tk_expf(float x);
float tk_powf(float x, float y);
float tk_sinf(float x);
float tk_cosf(float x);
float tk_atan2f(float y, float x);
float tk_acosf(float x);
float tk_fabsf(float x);
float tk_fmodf(float x, float y);
static inline float tk_clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float tk_minf(float a, float b) { return a < b ? a : b; }
static inline float tk_maxf(float a, float b) { return a > b ? a : b; }
static inline int tk_mini(int a, int b) { return a < b ? a : b; }
static inline int tk_maxi(int a, int b) { return a > b ? a : b; }
size_t tk_strlen(const char *s);
int tk_strcmp(const char *a, const char *b);
void tk_memcpy(void *d, const void *s, size_t n);
void tk_memset(void *d, int v, size_t n);

#endif
