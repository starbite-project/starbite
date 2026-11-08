/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's normal-mode look (docs/STARBITE-OS-PLAN-0.6-0.9.md 0.6): a wallpaper made at start, a blurred copy
 * of it made once, and "glass": the blurred wallpaper under a card, tinted, with a light top edge. No live blur and
 * no shaders: two buffers of the panel's size and a per-pixel blend (a full screen of glass is about 4 Mpx).
 *
 * Wallpapers (generated, Starbite's own; no image files): 0 Aurora (deep blue to violet, two soft light bands and
 * stars), 1 Dusk (warm), 2 Ocean (teal), 3 Graphite (neutral). Emergency Mode draws on black as before. */
#include "sb.h"
#include "sbui.h"
#include "kern.h"

static uint32_t *wp, *wpb;                  /* the wallpaper and its blurred copy (UI_W x UI_H, ARGB) */
static int ready, style = -1;
int look_emergency;                         /* Emergency Mode: black, the big tiles (the settings record keeps it) */
int look_saved_style;                       /* the settings record's wallpaper, made once at start */

int look_glass_on(void)
{
    return ready && !look_emergency;
}

int look_style(void)
{
    return style < 0 ? 0 : style;
}

int look_is_ready(void)
{
    return ready;
}

static uint32_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xff000000U | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* a soft band: brightness 0..256 at distance d (in px) from its centre line, width w */
static int band(int d, int w)
{
    if (d < 0)
        d = -d;
    if (d >= w)
        return 0;
    int t = 256 - d * 256 / w;
    return t * t / 256;
}

struct pal { int top[3], bot[3], a[3], b[3]; };
static const struct pal pals[4] = {
    { { 8, 10, 38 }, { 40, 12, 64 }, { 40, 170, 200 }, { 150, 70, 220 } },     /* Aurora */
    { { 30, 10, 30 }, { 90, 30, 20 }, { 240, 120, 60 }, { 200, 60, 120 } },    /* Dusk */
    { { 4, 20, 34 }, { 6, 50, 60 }, { 30, 190, 170 }, { 40, 110, 210 } },      /* Ocean */
    { { 14, 14, 18 }, { 34, 34, 40 }, { 120, 120, 140 }, { 80, 80, 100 } },    /* Graphite */
};

static void make(int s)
{
    const struct pal *p = &pals[s & 3];
    for (int y = 0; y < UI_H; ++y) {
        int t = y * 256 / UI_H;
        int br = p->top[0] + (p->bot[0] - p->top[0]) * t / 256;
        int bg = p->top[1] + (p->bot[1] - p->top[1]) * t / 256;
        int bb_ = p->top[2] + (p->bot[2] - p->top[2]) * t / 256;
        uint32_t *row = wp + (uint64_t)y * UI_W;
        for (int x = 0; x < UI_W; ++x) {
            /* two light bands crossing diagonally, gently waving (integer only) */
            int w1 = (x * 3) / 5 + ((x * x) >> 12) % 180;
            int w2 = UI_W - x / 2 + ((x * 7) >> 4) % 140;
            int a = band(y - 900 - w1 / 2, 520) * 150 / 256;
            int b = band(y - 1900 + w2 / 3, 640) * 120 / 256;
            int r = br + (p->a[0] * a + p->b[0] * b) / 256;
            int g = bg + (p->a[1] * a + p->b[1] * b) / 256;
            int c = bb_ + (p->a[2] * a + p->b[2] * b) / 256;
            uint32_t h = hash32((uint32_t)(y * UI_W + x) ^ 0x5b5b0000U);
            if ((h & 0x3fffU) < 3 && s != 3) {          /* stars, a few per thousand pixels */
                int v = 120 + (int)((h >> 16) & 127);
                r += v, g += v, c += v;
            }
            row[x] = rgb(r, g, c);
        }
    }
    /* the blurred copy: averaged in 16 x 16 cells, then those cells smoothed into each other with a 5 x 5 box (twice),
     * scaled back up with bilinear blending, and darkened a little so white text on glass stays readable */
    enum { C = 16 };
    static uint32_t cell[(UI_H / C + 2) * (UI_W / C + 2)], cell2[(UI_H / C + 2) * (UI_W / C + 2)];
    int cw = UI_W / C, ch = UI_H / C;
    for (int cy = 0; cy < ch; ++cy)
        for (int cx = 0; cx < cw; ++cx) {
            unsigned r = 0, g = 0, b = 0;
            for (int y = 0; y < C; y += 4)
                for (int x = 0; x < C; x += 4) {
                    uint32_t v = wp[(uint64_t)(cy * C + y) * UI_W + cx * C + x];
                    r += (v >> 16) & 255, g += (v >> 8) & 255, b += v & 255;
                }
            cell[cy * cw + cx] = rgb((int)r / 16, (int)g / 16, (int)b / 16);
        }
    for (int pass = 0; pass < 2; ++pass)
        for (int cy = 0; cy < ch; ++cy)
            for (int cx = 0; cx < cw; ++cx) {
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        int yy = cy + dy, xx = cx + dx;
                        if (yy < 0 || yy >= ch || xx < 0 || xx >= cw)
                            continue;
                        uint32_t v = (pass ? cell2 : cell)[yy * cw + xx];
                        r += (v >> 16) & 255, g += (v >> 8) & 255, b += v & 255, ++n;
                    }
                (pass ? cell : cell2)[cy * cw + cx] = rgb((int)(r / n), (int)(g / n), (int)(b / n));
            }
    for (int y = 0; y < UI_H; ++y) {
        int fy = y * 256 / C - 128, cy0 = fy >> 8, ty = fy & 255;
        if (cy0 < 0)
            cy0 = 0, ty = 0;
        int cy1 = cy0 + 1 < ch ? cy0 + 1 : cy0;
        uint32_t *row = wpb + (uint64_t)y * UI_W;
        for (int x = 0; x < UI_W; ++x) {
            int fx = x * 256 / C - 128, cx0 = fx >> 8, tx = fx & 255;
            if (cx0 < 0)
                cx0 = 0, tx = 0;
            int cx1 = cx0 + 1 < cw ? cx0 + 1 : cx0;
            uint32_t a = cell[cy0 * cw + cx0], b = cell[cy0 * cw + cx1], c = cell[cy1 * cw + cx0], d = cell[cy1 * cw + cx1];
            int ch_[3];
            for (int k = 0; k < 3; ++k) {
                int sh = 16 - 8 * k;
                int top = (int)((a >> sh) & 255) * (256 - tx) + (int)((b >> sh) & 255) * tx;
                int bot = (int)((c >> sh) & 255) * (256 - tx) + (int)((d >> sh) & 255) * tx;
                ch_[k] = ((top >> 8) * (256 - ty) + (bot >> 8) * ty) >> 8;
                ch_[k] = ch_[k] * 200 / 256;
            }
            row[x] = rgb(ch_[0], ch_[1], ch_[2]);
        }
    }
}

/* the wallpaper for style s (made once per style; about 0.2 s on an A55, after the first frame) */
void look_init(int s)
{
    if (s < 0 || s > 3)
        s = 0;
    if (ready && s == style)
        return;
    if (!wp)
        wp = sb_alloc((uint64_t)UI_W * UI_H * 4U);
    if (!wpb)
        wpb = sb_alloc((uint64_t)UI_W * UI_H * 4U);
    if (!wp || !wpb)
        return;                             /* no memory: black as in Emergency Mode */
    uint64_t t0 = sb_ms();
    make(s);
    style = s;
    ready = 1;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "look: the wallpaper made in ");
    ub_dec(&u, sb_ms() - t0);
    ub_s(&u, " ms");
    sb_log(u.b);
}

static struct ui_rect clipped(int x, int y, int w, int h)
{
    struct ui_rect c = ui_get_clip(), r = { x, y, w, h };
    int x0 = r.x > c.x ? r.x : c.x, y0 = r.y > c.y ? r.y : c.y;
    int x1 = r.x + r.w < c.x + c.w ? r.x + r.w : c.x + c.w, y1 = r.y + r.h < c.y + c.h ? r.y + r.h : c.y + c.h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > UI_W)
        x1 = UI_W;
    if (y1 > UI_H)
        y1 = UI_H;
    struct ui_rect o = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return o;
}

/* the background: the wallpaper, or black */
void look_bg(int x, int y, int w, int h)
{
    if (!look_glass_on()) {
        ui_fill(x, y, w, h, 0xff000000U);
        return;
    }
    struct ui_rect r = clipped(x, y, w, h);
    uint32_t *bb = ui_back();
    for (int yy = r.y; yy < r.y + r.h; ++yy)
        memcpy(bb + (uint64_t)yy * UI_W + r.x, wp + (uint64_t)yy * UI_W + r.x, (size_t)r.w * 4U);
    ui_dirty(r.x, r.y, r.w, r.h);           /* a black background never needed presenting; a wallpaper does */
}

static inline uint32_t blend(uint32_t under, uint32_t over, unsigned a)
{
    unsigned na = 256 - a;
    unsigned r = (((under >> 16) & 255) * na + ((over >> 16) & 255) * a) >> 8;
    unsigned g = (((under >> 8) & 255) * na + ((over >> 8) & 255) * a) >> 8;
    unsigned b = ((under & 255) * na + (over & 255) * a) >> 8;
    return 0xff000000U | r << 16 | g << 8 | b;
}

/* the horizontal inset of a rounded corner at row dy (0 = the top edge) for radius r */
static int inset(int dy, int r)
{
    if (dy >= r)
        return 0;
    int d = r - dy;                         /* r - sqrt(r^2 - d^2), by search (r <= 120) */
    int x = 0;
    while (x < r && (r - x) * (r - x) + d * d > r * r)
        ++x;
    return x;
}

/* glass: the blurred wallpaper in the rounded rect, tinted (a / 256), a lighter top edge */
void look_glass(int x, int y, int w, int h, int r, uint32_t tint, unsigned a)
{
    if (!look_glass_on()) {
        ui_round(x, y, w, h, r, blend(0xff000000U, tint, a > 40 ? a : 40));
        return;
    }
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    if (a > 256)
        a = 256;
    struct ui_rect c = clipped(x, y, w, h);
    uint32_t *bb = ui_back();
    for (int yy = c.y; yy < c.y + c.h; ++yy) {
        int dy = yy - y, in = dy < r ? inset(dy, r) : dy >= h - r ? inset(h - 1 - dy, r) : 0;
        int x0 = x + in > c.x ? x + in : c.x, x1 = x + w - in < c.x + c.w ? x + w - in : c.x + c.w;
        uint32_t *row = bb + (uint64_t)yy * UI_W, *src = wpb + (uint64_t)yy * UI_W;
        unsigned aa = dy < 3 ? a + 60 : a;              /* the light catching the top edge */
        if (aa > 256)
            aa = 256;
        for (int xx = x0; xx < x1; ++xx)
            row[xx] = blend(src[xx], tint, aa);
    }
    ui_dirty(c.x, c.y, c.w, c.h);
}

void look_glass_disc(int cx, int cy, int r, uint32_t tint, unsigned a)
{
    look_glass(cx - r, cy - r, 2 * r, 2 * r, r, tint, a);
}
