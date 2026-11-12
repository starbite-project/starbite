/* SPDX-License-Identifier: MIT
 * sbtk: the Starbite OS UI toolkit (P1 of the UI port plan, starbite-os-ui-port-plan.md). Freestanding C: it
 * includes only this header, tk_plat.h (allocation, clock, log) and <arm_neon.h>, so the same files build into the
 * kernel (P2) and into the host harness (ui/host). The design follows the approved shell concept
 * (docs/concepts/starbite-os-shell/index.html):
 *   - every layer is drawn once into a cached premultiplied bitmap; a frame only composites the cached layers with
 *     their current translate / scale / opacity / clip (the compositor, tk_comp.c);
 *   - Liquid Glass is baked per element (neutral tone, rim specular, depth, shadow: tk_glass.c's constants) and its
 *     backdrop is the wallpaper blurred once (blur 14 pt, saturate TK_GLASS_SAT), sampled under the element where it
 *     is now, or a static backdrop filtered and bent at the rim per element (the lens);
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
float tk_sd_rrect(float px, float py, float x, float y, float w, float h, float r);   /* signed distance */
float tk_sd_rrect4(float px, float py, float x, float y, float w, float h, const tk_r4 *k);
tk_r4 tk_radii(float w, float h, float r);
/* the platform's parallel run (set it to the compositor's); tk_par_rows splits [y0, y1) into 8 bands over it */
extern void (*tk_par)(void (*fn)(void *, int), void *arg, int n);
void tk_par_rows(int y0, int y1, void (*fn)(void *, int, int), void *ctx);
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
void tk_bmp_blur_sat(tk_bmp *b, float sigma, float sat, float bright);
void tk_bmp_satbright(tk_bmp *b, float sat, float bright);
void tk_bmp_tint(tk_bmp *b, tk_col c);                 /* a constant colour over all of it */
void tk_bmp_copy(tk_bmp *dst, int dx, int dy, const tk_bmp *src, int sx, int sy, int w, int h);
void tk_bmp_down2(tk_bmp *dst, const tk_bmp *src);              /* 2x2 box (premultiplied) */
/* bilinear up: dst px (i, j) samples src at the point (x0 + i, y0 + j) of a space f times src's size */
void tk_bmp_up(tk_bmp *dst, const tk_bmp *src, int x0, int y0, float f);

/* ---- text (stb_truetype on static instances; tk_text.c) ----------------------------------------------------- */
/* the faces callers name: a weight (the names are Inter Tight's, the faces before P3p). The size picks the packed
 * face as iOS picks SF Pro: Inter's Text cut (opsz 14) below 20 pt, its Display cut (opsz 32) from 20 pt; Display
 * has 400 / 600 / 700 (500 draws as 400), and 800 draws as 700 everywhere. With ls == 0 the text gets SF's
 * size-dependent tracking (tk_text.c); a non-zero ls (px after each glyph) replaces it. */
enum { TK_IT400, TK_IT500, TK_IT600, TK_IT700, TK_IT800, TK_OF300, TK_NFONT };
/* the packed faces, in tools/sbtk-fonts.py's / sbtk-pack.py's order (a pack entry's aux) */
enum { TK_TX400, TK_TX500, TK_TX600, TK_TX700, TK_DP400, TK_DP600, TK_DP700, TK_FOF300, TK_NFACE };
#define TK_TNUM 1          /* tabular figures (font-variant-numeric: tabular-nums) */
int tk_font_load(int face, const uint8_t *ttf, const uint8_t *lay, uint32_t laylen);
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

/* ---- Liquid Glass (tk_glass.c; its header lists every constant, shared with the GPU path) ------------------- */
#define TK_GLASS_SAT 1.2f          /* the backdrop's saturate() for regular glass */
#define TK_GLASS_SAT_CLEAR 1.0f    /* clear glass */
#define TK_GLASS_BRIGHT 1.0f       /* its brightness() */
extern int tk_glass_lens;          /* 1: static backdrops are bent at the rim (tk_glass_local / _put / _from_half) */
/* the wallpaper: the concept's .wall.n (inset -30 pt, three radials over a vertical gradient) at k px per panel px */
void tk_wall_draw(tk_bmp *b, int emergency, float k);
/* the backdrop the glass shows: blur 14 pt, saturate TK_GLASS_SAT of src (both half resolution) */
void tk_glass_backdrop(tk_bmp *dst, const tk_bmp *src);
/* bake one glass element into b at (x, y, w, h) with radius r: the drop shadow (outside), and inside the neutral
 * tone (dark / light by the backdrop's luminance), the depth and the thin rim specular; no tint, gloss or outline.
 * dark = .lg.dark (a denser tone under labels). Its backdrop mask (the shape's coverage) goes into mask (same frame
 * as b) if given. */
void tk_glass_bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow);
void tk_glass_bake_ref(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow);   /* the same, exact math, one core */
/* the glass's backdrop filter applied in place to what b already holds under the shape (glass inside a backdrop
 * root, as the lock list's cards: Chrome filters only the list's own content, not the wallpaper); bent at the rim */
void tk_glass_local(tk_bmp *dst, const tk_bmp *src, float x, float y, float w, float h, float r);
void tk_glass_local_put(tk_bmp *b, const tk_bmp *t, int X, int Y, float x, float y, float w, float h, float r);
/* tk_glass_local over a backdrop held at half resolution (b's origin at the panel point (box, boy)) */
void tk_glass_from_half(tk_bmp *b, int box, int boy, const tk_bmp *half, float x, float y, float w, float h, float r);
/* P4 (the GPU row): glass with a mask (d_glass, which records its shape) is the compositor's lens (../kern/lens.h:
 * comp2 on the GPU over a live backdrop, draw_lens on the CPU over the wallpaper's): its bake keeps only the shadow */
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
    uint16_t *hspan;           /* per row of half: the first and one past the last column with anything (0 0: none) */
    tk_a8 glass, ghalf;        /* backdrop mask: the wallpaper glass shows through it (same frame as bmp) */
    int glass_opaque_only;     /* the backdrop only while the composed opacity is 1 (Chrome's backdrop root) */
    float x, y;                /* bitmap origin in the parent's space, px */
    float bx, by;              /* the owner's rest position (the shell moves layers relative to it) */
    float sx, sy, ox, oy;      /* scale about (ox, oy) in the parent's space; 1 = none */
    float alpha;
    int hidden;
    int opaque;                /* every px of bmp (or solid) is opaque: what is under it need not be drawn */
    int external;              /* 1.0: bmp is memory the layer doesn't own (tk_layer_external: the Linux guest's window
                                * surface, uncached): the GPU samples it in place; no half copy, no block map, never freed */
    int clip;                  /* clip children and self to clip_r (parent space) */
    tk_rect clip_r;
    float fade_top, fade_bot;  /* mask-image fades inside clip_r (px, 0 = none) */
    float clip_radius;         /* rounded clip (px; 0 = square) */
    int slice;                 /* 3-slice stretch, 1 vertical / 2 horizontal: drawn slice_h long, the first s0 and last s1 px kept */
    float slice_h, slice_s0, slice_s1;
    uint32_t solid;            /* a solid-colour layer (premultiplied; no bitmap) of solid_w x solid_h px at (x, y) */
    float solid_w, solid_h;
    tk_layer *child, *next;
    uint32_t gen;              /* bumped when bmp changes (redraw its area) */
    /* compositor state */
    float gsh[24][5];          /* v8: the glass shapes in its mask (x, y, w, h, r in bitmap px): the lens's geometry */
    int ngsh;
    uint32_t gtint[24];        /* V2 (TK_GLASS_SHAPES_V2): each shape's tint, ARGB, 0 none */
    uint8_t gvar[24], gpress[24];   /* V2: 0 regular / 1 clear (iOS 26); the press flex 0..255 (0 none) */
    uint32_t gpu_gen;          /* P4: gen + 1 when the GPU last had this layer's bitmap and mask flushed to it */
    uint8_t *gpu_occ;          /* P4 v4: per 32 px block of bmp 0 empty / 1 partial / 2 opaque (the GPU's bins skip and cull) */
    uint32_t gpu_occ_w, gpu_occ_h, gpu_occ_cap, gpu_occ_frame;
    int gpu_occ_ok, gpu_occ_seen;
    tk_irect last;             /* screen rect drawn last frame */
    uint32_t last_gen;
    float last_key[8];
};
void tk_layer_init(tk_layer *l, const char *name);
void tk_layer_add(tk_layer *parent, tk_layer *child);     /* appended on top */
void tk_layer_dirty(tk_layer *l);                         /* its bitmap changed */
void tk_layer_prepare(tk_layer *l);                       /* make its half copy now (idle time), not on first motion */
/* 1.0 (Track E's zero-copy Linux window): the layer shows w x h px of memory it doesn't own (opaque ARGB, stride px),
 * sampled by the GPU in place (the platform maps that range: kernel gpucomp_add_ext). Call it each frame the pointer
 * or the pixels change (it bumps gen); px 0 detaches. Detach before freeing the layer (relayer would free bmp). */
void tk_layer_external(tk_layer *l, const uint32_t *px, int w, int h, int stride);
void tk_layer_glass_shape(tk_layer *l, float x, float y, float w, float h, float r);
/* V2 (Track A's request): each recorded shape carries a variant, a tint and a press; tk_layer_glass_shape takes them
 * from tk_gstyle (set it around a d_glass call; {0, 0, 0} = regular, untinted, at rest) */
#define TK_GLASS_SHAPES_V2 1
typedef struct { int variant; uint32_t tint; float press; } tk_gstyle_t;
extern tk_gstyle_t tk_gstyle;
void tk_layer_glass_shape_v2(tk_layer *l, float x, float y, float w, float h, float r, int variant, uint32_t tint, float press);   /* v8: a rounded box of its mask */

/* P4, the GPU compositor (kernel gpucomp.c): one draw item as the GPU takes it, the layout of the kernel's struct
 * gc_quad (a bitmap or a solid of w x h, isx / isy source px per screen px from (dx0, dy0), alpha, the clip rect with
 * a rounded edge, fades by screen y, a 3-slice, glass: the backdrop bd through the 8-bit mask first) */
enum { TK_GPU_SOLID = 1, TK_GPU_BILINEAR = 2, TK_GPU_GLASS = 4, TK_GPU_FADE = 8, TK_GPU_SLICE_V = 16, TK_GPU_SLICE_H = 32,
       TK_GPU_OPAQUE = 64, TK_GPU_LENS = 256 };   /* LENS (v9): a transparent solid over a glass shape: Liquid Glass */
typedef struct {
    const void *src;
    uint32_t stride, w, h, flags, solid;
    float alpha, dx0, dy0, isx, isy, cx0, cy0, cx1, cy1, radius, ft0, ft1, fb0, fb1, len, s0, s1;
    const void *mask;
    uint32_t mstride;
    const void *bd;
    uint32_t bstride;
    float bk;
    uint32_t bw, bh;
    float lx, ly, lw, lh;      /* v9 (LENS): the glass shape (screen px); the quad reaches LENS_APRON px past it */
    float lsat;                /* v10 (LENS): the backdrop's saturation (0 = 1) */
    uint32_t live;             /* v10 (LENS): the backdrop is the frame's first `live` items, blurred (the platform's
                                * gpu makes it; 0, or a platform without it: bd, the blurred wallpaper) */
    const uint8_t *occ;        /* the layer's block map (gpu_occ), or 0 */
    uint32_t occ_w, occ_h;
    uint32_t gen;              /* the content's stamp (the layer's gen, the ground's): what a cache of the frame's
                                * unchanged bottom items compares */
    float lq[16];              /* 1.0 (LENS, glass V3): the lens's record (kern/lens.h lens_make: the shape's size,
                                * variant, tint, press and the backdrop's luminance under it; LQ_N floats) */
} tk_gpu_item;
#define TK_GPU_MAX 200     /* = GPUCOMP_MAX_QUADS (1.0: was 127, a busier frame went to the CPU) */

typedef struct {
    tk_bmp fb;                 /* the full-resolution frame in RAM (blends read it) */
    tk_bmp panel;              /* where frames go (the framebuffer: written only) */
    tk_bmp base_half;          /* the wallpaper at half resolution (the star is a layer) */
    tk_bmp bw_half;            /* the glass backdrop: the wallpaper blurred, half resolution */
    tk_bmp half;               /* the half-resolution motion canvas */
    tk_irect dmg[16];          /* the last frame's panel rects (for a partial panel update) */
    int nd;
    tk_layer *root;
    /* run fn(arg, 0 .. n - 1) on as many cores as the platform has, returning when all are done (NULL: one core) */
    void (*par)(void (*fn)(void *, int), void *arg, int n);
    int motion;                /* the next frame is a motion frame (half resolution) */
    tk_irect moved;            /* what motion frames covered since the last full frame */
    int have_moved;
    /* stats of the last frame */
    int last_half, last_px;
    uint64_t last_us, us_comp, us_present;
    uint32_t ground_gen;       /* bumped when base_half / bw_half change: the next frame redraws everything */
    /* P2r, two panel buffers (tear-free flips): acquire (NULL: one buffer) returns the buffer the display does not
     * read, after the last flip latched; that buffer holds the frame before the last, so the present also brings
     * over what the last presented frame changed (pd, its panel rects; pd_half: drawn from the half canvas) */
    /* P2t: three buffers. acquire also says how many presented frames the buffer is behind (*age: 1 = it holds
     * the last frame but one; <= 0 or > TK_HIST: unknown, everything comes over); the compositor keeps the
     * presented frames' rects (hd) for that */
    uint32_t *(*acquire)(void *ctx, int *age);
    void *acq_ctx;
#define TK_HIST 8
    tk_irect hd[TK_HIST][16];
    int hn[TK_HIST], hh[TK_HIST];
    uint32_t fno;              /* presented frames so far (with acquire) */
    uint64_t us_wait, us_catch;
    uint64_t us_idle, us_halfs;
    uint32_t sharp_motion;     /* motion frames drawn at full resolution (small: P2s) */
    /* P4: the GPU compositor (NULL: the CPU draws). gpu composes the whole frame into dst (a scanout buffer from
     * acquire): 1 done, 0 not (the CPU draws this frame); gpu_clean flushes what the CPU wrote before the GPU reads */
    int (*gpu)(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us);
    void (*gpu_clean)(void *ctx, const void *p, uint64_t n);
    void *gpu_ctx;
    uint32_t gpu_frames, gpu_fallbacks, gpu_ground;
    int gpu_half;              /* v7: the frame gpu is asked for is a big move's: half resolution, 2x, all of it */
    uint32_t gpu_halves;       /* such frames so far */
    uint32_t gpu_idle_flushed; /* v10: layers made ready for the GPU in idle time (not in a moving frame) */
    int gpu_stale;
    uint32_t gpu_items;        /* the last GPU frame's quads */             /* P4: GPU frames since the CPU last drew everything (its canvases are behind) */
    uint64_t us_gpu;  /* the last frame's half-canvas catch-up (tk_comp_idle) and half copies made in it */  /* the last frame's wait for the free buffer and its catch-up pass */
} tk_comp;
int tk_comp_init(tk_comp *c, uint32_t *fb, uint32_t *panel);   /* NULL: allocated */
/* render a group's children (as if the group were at identity) into a screen-sized transparent bitmap and their
 * glass masks into mask: one cached layer for a group that moves or fades as one (CALayer's shouldRasterize) */
void tk_flatten(tk_layer *group, tk_bmp *out, tk_a8 *mask, float scale);   /* scale .5: a half-resolution cache */
void tk_flatten_shapes(tk_layer *dst);
/* P4: the lens's glass at px (x, y) of the last tk_flatten's frame (its glass shapes, the wallpaper's backdrop
 * c->bw_half bent, toned and lit as the compositor's lens): opaque; fb where no shape is */
uint32_t tk_flat_lens(const tk_comp *c, int x, int y, uint32_t fb);    /* v8: the last tk_flatten's glass shapes (its mask's frame) into dst */
/* draw a frame: motion = half resolution and 2x present; full = redraw everything; returns the drawn px */
int tk_comp_frame(tk_comp *c, int motion, int full);
void tk_comp_prepare(tk_comp *c);
int tk_comp_idle_pending(tk_comp *c);
/* the frame on the panel now, full resolution, in RAM (P4: after GPU frames the CPU draws it there once, without
 * presenting: the scanout buffers are uncached for the CPU); for a snapshot of the screen right after tk_comp_frame */
const tk_bmp *tk_comp_shown(tk_comp *c);
#define TK_COMP_SHOWN 1                       /* the shell's shown_frame() uses it */   /* v10: idle work left (the GPU's layer flushes, a turn at a time) */
void tk_comp_idle(tk_comp *c);                            /* idle time: the half canvas follows the rest frames */
uint64_t tk_comp_bench(tk_comp *c, int kind);      /* the present's kernels vs uibench's loops, us */                         /* idle: make the visible layers' half copies */

/* ---- motion: springs, the velocity tracker, gestures (tk_motion.c) ------------------------------------------ */
enum { TK_SPR_UNLOCK, TK_SPR_OPEN, TK_SPR_CLOSE, TK_SPR_SHEET, TK_SPR_ROWS, TK_SPR_CC, TK_SPR_PRESS, TK_SPR_POP, TK_SPR_MORPH, TK_SPR_FAST,
       TK_SPR_BANNER, TK_SPR_FAN, TK_SPR_CARD, TK_SPR_GROW, TK_NSPR };
typedef struct {
    float x, v, to, eps, k, c;
    int live;
} tk_spring;
/* the concept's spring(): mass 1, eps = max(1e-4, |to - from| x 0.0005), velocity in units per s (B4: iOS's constants) */
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
void *tk_shrink(void *p, size_t n);  /* keep the first n bytes, give the rest back; the block may move */
size_t tk_heap_used(void);
size_t tk_heap_peak(void);
void tk_heap_peak_reset(void);      /* the peak from now (the host self test: per film) */
uint64_t tk_heap_largest(void);    /* the largest free block (the kernel's heap; the host's with SBTK_HEAP_MB) */
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
