/* SPDX-License-Identifier: GPL-2.0-only
 * sbui page: a retained, scrolling list of grouped cards (sbui_w.h).
 */
#include "sbui_w.h"

#define CARD_X 40
#define CARD_W (UI_W - 2 * CARD_X)
#define PADX 48
#define RADIUS 36
#define TOP_PAD 8
#define BOTTOM_PAD 80

/* ---- pool ------------------------------------------------------------------------------------ */

#define POOL 8
static struct pitem *pool_a[POOL], *pool_b[POOL];
static int pool_used[POOL];

void page_init(struct page *p, int vy0, int vy1)
{
    int s = 0;
    while (s < POOL && pool_used[s])
        ++s;
    if (s == POOL)
        s = POOL - 1;                       /* nesting deeper than the pool: share the last slot */
    if (!pool_a[s]) {
        pool_a[s] = sb_alloc(sizeof(struct pitem) * PAGE_MAX);
        pool_b[s] = sb_alloc(sizeof(struct pitem) * PAGE_MAX);
    }
    pool_used[s] = 1;
    p->slot = s;
    p->build = pool_a[s];
    p->shown = pool_b[s];
    p->n = p->nshown = p->nrows = 0;
    p->total = p->scroll = 0;
    p->focus = -1;
    p->vy0 = vy0;
    p->vy1 = vy1;
    p->drawn = 0;
    p->pressed = -1;
    p->fling_v = 0;
}

void page_free(struct page *p)
{
    if (p->slot >= 0 && p->slot < POOL)
        pool_used[p->slot] = 0;
    p->slot = -1;
}

/* ---- building ------------------------------------------------------------------------------- */

static uint32_t fnv(uint32_t h, const void *d, unsigned n)
{
    const uint8_t *b = d;
    for (unsigned i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619U;
    return h;
}

static struct pitem *add(struct page *p, int kind, const char *k, const char *v, uint32_t c, uint32_t flags)
{
    if (!p->build || p->n >= PAGE_MAX)
        return 0;
    struct pitem *it = &p->build[p->n++];
    it->kind = (uint8_t)kind;
    it->flags = (uint16_t)flags;
    it->color = c;
    it->row = -1;
    it->draw = 0;
    it->arg = 0;
    it->lines = 1;
    sb_strcpy(it->k, sizeof(it->k), k ? k : "");
    sb_strcpy(it->v, sizeof(it->v), v ? v : "");
    it->hash = 0;
    return it;
}

void page_begin(struct page *p)
{
    p->n = 0;
    p->nrows = 0;
}

void page_hdr(struct page *p, const char *t)
{
    (void)add(p, PI_HDR, t, 0, C_L2, 0);
}

void page_kv(struct page *p, const char *k, const char *v, uint32_t c)
{
    (void)add(p, PI_KV, k, v, c ? c : C_L1, 0);
}

void page_mono(struct page *p, const char *t, uint32_t c)
{
    (void)add(p, PI_MONO, 0, t, c ? c : C_L1, 0);
}

void page_big(struct page *p, const char *caption, const char *v, uint32_t c)
{
    (void)add(p, PI_BIG, caption, v, c ? c : C_L1, 0);
}

int page_row(struct page *p, const char *t, const char *s, uint32_t flags)
{
    struct pitem *it = add(p, PI_ROW, t, s, C_L1, flags);
    if (!it)
        return -1;
    it->row = (int16_t)p->nrows++;
    return it->row;
}

int page_button(struct page *p, const char *t, uint32_t c)
{
    struct pitem *it = add(p, PI_BUTTON, t, 0, c ? c : C_STAR, 0);
    if (!it)
        return -1;
    it->row = (int16_t)p->nrows++;
    return it->row;
}

void page_note(struct page *p, const char *t, uint32_t c)
{
    (void)add(p, PI_NOTE, 0, t, c ? c : C_L3, 0);
}

void page_gap(struct page *p, int h)
{
    struct pitem *it = add(p, PI_GAP, 0, 0, 0, 0);
    if (it)
        it->lines = (uint8_t)(h > 255 ? 255 : h < 0 ? 0 : h);
}

void page_custom(struct page *p, int h, pi_draw_fn draw, uintptr_t arg, uint32_t hash)
{
    struct pitem *it = add(p, PI_CUSTOM, 0, 0, 0, 0);
    if (!it)
        return;
    it->draw = draw;
    it->arg = arg;
    it->hash = hash;
    it->y = h;                      /* the height, until layout */
}

/* ---- layout ------------------------------------------------------------------------------------ */

/* Bytes of s (from its start) that fit in w px, broken after a space when possible. */
static unsigned wrap_len(int font, const char *s, int w)
{
    unsigned fit = ui_text_fit(font, s, w);
    if (!s[fit])
        return fit;
    unsigned br = fit;
    while (br > 0 && s[br] != ' ')
        --br;
    return br > fit / 3 ? br + 1 : (fit ? fit : 1);
}

static unsigned count_lines(int font, const char *s, int w)
{
    unsigned n = 0;
    while (*s) {
        s += wrap_len(font, s, w);
        ++n;
        if (n >= 12)
            break;
    }
    return n ? n : 1;
}

static int in_card(int kind)
{
    return kind == PI_KV || kind == PI_MONO || kind == PI_ROW || kind == PI_CUSTOM;
}

static int kv_single(const struct pitem *it)
{
    return ui_text_w(F_ROW, it->k) + ui_text_w(F_ROW, it->v) + 2 * PADX + 48 <= CARD_W;
}

static void layout(struct page *p)
{
    int y = TOP_PAD;
    for (int i = 0; i < p->n; ++i) {
        struct pitem *it = &p->build[i];
        int prev = i ? p->build[i - 1].kind : -1, next = i + 1 < p->n ? p->build[i + 1].kind : -1;
        it->first = !in_card(it->kind) || !in_card(prev);
        it->last = !in_card(it->kind) || !in_card(next);
        if (it->kind == PI_MONO) {
            it->first = prev != PI_MONO;
            it->last = next != PI_MONO;
        } else if (in_card(it->kind)) {
            if (prev == PI_MONO)
                it->first = 1;
            if (next == PI_MONO)
                it->last = 1;
        }
        int h;
        switch (it->kind) {
        case PI_HDR:
            h = i ? 128 : 96;
            break;
        case PI_KV:
            if (!it->v[0] || kv_single(it)) {
                h = 124;
                it->lines = 1;
            } else {
                it->lines = (uint8_t)count_lines(F_ROW, it->v, CARD_W - 2 * PADX);
                h = 76 + 64 * it->lines + 34;
            }
            break;
        case PI_MONO:
            h = 50 + (it->first ? 18 : 0) + (it->last ? 18 : 0);
            break;
        case PI_BIG:
            h = 330;
            break;
        case PI_ROW:
            h = it->v[0] && !(it->flags & R_VALUE) ? 176 : 136;
            break;
        case PI_BUTTON:
            h = 150;
            break;
        case PI_NOTE:
            it->lines = (uint8_t)count_lines(F_DETAIL, it->v, CARD_W - 2 * PADX);
            h = 24 + 56 * it->lines + 12;
            break;
        case PI_GAP:
            h = it->lines;
            break;
        default:
            h = it->y;
            break;
        }
        if (it->kind == PI_BIG || it->kind == PI_BUTTON) {
            it->first = it->last = 1;
            if (i && p->build[i - 1].kind != PI_HDR && p->build[i - 1].kind != PI_GAP)
                y += 24;
        }
        it->y = y;
        it->h = h;
        if (it->kind != PI_CUSTOM)
            it->hash = 0;
        uint32_t hs = it->kind == PI_CUSTOM ? it->hash : 2166136261U;
        hs = fnv(hs, &it->kind, 1);
        hs = fnv(hs, &it->flags, 2);
        hs = fnv(hs, &it->color, 4);
        hs = fnv(hs, &it->first, 2);
        hs = fnv(hs, it->k, sb_strlen(it->k));
        hs = fnv(hs, "\1", 1);
        hs = fnv(hs, it->v, sb_strlen(it->v));
        it->hash = hs;
        y += h;
        if (it->kind == PI_BIG || it->kind == PI_BUTTON)
            y += 24;
    }
    p->total = y + BOTTOM_PAD;
}

/* ---- drawing ------------------------------------------------------------------------------------ */

static void chevron(int x, int cy, uint32_t c)
{
    ui_line(x, cy - 20, x + 18, cy, 5, c);
    ui_line(x + 18, cy, x, cy + 20, 5, c);
}

static void toggle(int x, int cy, int on)
{
    ui_round(x, cy - 38, 128, 76, 38, on ? C_GREEN : C_CARD3);
    ui_disc(on ? x + 128 - 38 : x + 38, cy, 32, C_L1);
}

static void draw_item(const struct page *p, const struct pitem *it, int py)
{
    int x = CARD_X, w = CARD_W;
    int focus = it->row >= 0 && (it->row == p->focus || it->row == p->pressed);
    if (in_card(it->kind) || it->kind == PI_BIG || it->kind == PI_BUTTON) {
        unsigned corners = (it->first ? 3U : 0U) | (it->last ? 12U : 0U);
        uint32_t bg = focus ? C_CARD2 : C_CARD;
        if (it->kind == PI_BUTTON)
            bg = focus ? ui_mix(it->color, C_CARD, 90) : ui_mix(it->color, C_CARD, 46);
        ui_round_c(x, py, w, it->h, RADIUS, corners, bg);
        if (!it->last && it->kind != PI_MONO)
            ui_fill(x + PADX, py + it->h - 2, w - PADX, 2, C_SEP);
    }
    switch (it->kind) {
    case PI_HDR:
        ui_text(F_HEAD, x + PADX, py + it->h - 30, it->k, it->color);
        break;
    case PI_KV:
        if (it->lines == 1 && (!it->v[0] || kv_single(it))) {
            ui_text_cut(F_ROW, x + PADX, py + 80, it->k, w - 2 * PADX, C_L2);
            ui_text_r(F_ROW, x + w - PADX, py + 80, it->v, it->color);
        } else {
            ui_text_cut(F_ROW, x + PADX, py + 72, it->k, w - 2 * PADX, C_L2);
            const char *s = it->v;
            for (int l = 0; *s && l < it->lines; ++l) {
                unsigned n = wrap_len(F_ROW, s, w - 2 * PADX);
                char line[176];
                unsigned k = 0;
                for (; k < n && k + 1 < sizeof(line); ++k)
                    line[k] = s[k];
                line[k] = 0;
                ui_text(F_ROW, x + PADX, py + 72 + 64 * (l + 1), line, it->color);
                s += n;
            }
        }
        break;
    case PI_MONO:
        ui_text_cut(F_MONO, x + 28, py + (it->first ? 18 : 0) + 37, it->v, w - 40, it->color);
        break;
    case PI_BIG:
        ui_text_c(F_BIG, x + w / 2, py + 196, it->v, it->color);
        ui_text_c(F_DETAIL, x + w / 2, py + 274, it->k, C_L2);
        break;
    case PI_ROW: {
        uint32_t tc = (it->flags & R_DIS) ? C_L3 : (it->flags & R_RED) ? C_RED : (it->flags & R_WARN) ? C_ORANGE : C_L1;
        int right = x + w - PADX;
        if (it->flags & R_CHECK) {
            toggle(right - 128, py + it->h / 2, (it->flags & R_ON) != 0);
            right -= 128 + 32;
        } else if (!(it->flags & R_NOCHEV) && !(it->flags & R_DIS)) {
            chevron(right - 18, py + it->h / 2, C_L3);
            right -= 18 + 32;
        }
        if (it->v[0] && !(it->flags & R_VALUE)) {
            ui_text_cut(F_ROW, x + PADX, py + 78, it->k, right - x - PADX, tc);
            ui_text_cut(F_DETAIL, x + PADX, py + 138, it->v, right - x - PADX, (it->flags & R_DIS) ? C_L3 : C_L2);
        } else {
            int vw = it->v[0] ? ui_text_w(F_ROW, it->v) + 24 : 0;
            ui_text_cut(F_ROW, x + PADX, py + 86, it->k, right - x - PADX - vw, tc);
            if (it->v[0])
                ui_text_r(F_ROW, right, py + 86, it->v, C_L2);
        }
        break;
    }
    case PI_BUTTON:
        ui_text_c(F_BTN, x + w / 2, py + 95, it->k, it->color);
        break;
    case PI_NOTE: {
        const char *s = it->v;
        for (int l = 0; *s && l < it->lines; ++l) {
            unsigned n = wrap_len(F_DETAIL, s, w - 2 * PADX);
            char line[176];
            unsigned k = 0;
            for (; k < n && k + 1 < sizeof(line); ++k)
                line[k] = s[k];
            line[k] = 0;
            ui_text(F_DETAIL, x + PADX, py + 24 + 44 + 56 * l, line, it->color);
            s += n;
        }
        break;
    }
    case PI_CUSTOM:
        if (it->draw)
            it->draw(it, x, py, w, focus);
        break;
    default:
        break;
    }
}

/* Panel rows [y0, y1) of the viewport. */
static void draw_band(const struct page *p, int y0, int y1)
{
    if (y0 < p->vy0)
        y0 = p->vy0;
    if (y1 > p->vy1)
        y1 = p->vy1;
    if (y1 <= y0)
        return;
    struct ui_rect save = ui_get_clip();
    ui_clip(0, y0, UI_W, y1 - y0);
    ui_fill(0, y0, UI_W, y1 - y0, C_BG);
    int top = p->vy0 - p->scroll;
    for (int i = 0; i < p->nshown; ++i) {
        const struct pitem *it = &p->shown[i];
        int py = top + it->y;
        if (py >= y1)
            break;
        if (py + it->h > y0)
            draw_item(p, it, py);
    }
    ui_set_clip(save);
}

void page_redraw(struct page *p)
{
    draw_band(p, p->vy0, p->vy1);
    p->drawn = 1;
}

int page_max_scroll(const struct page *p)
{
    int m = p->total - (p->vy1 - p->vy0);
    return m > 0 ? m : 0;
}

void page_end(struct page *p)
{
    layout(p);
    struct pitem *old = p->shown;
    int nold = p->nshown;
    p->shown = p->build;
    p->nshown = p->n;
    p->build = old;
    int ms = page_max_scroll(p);
    if (!p->drawn || p->scroll > ms) {
        if (p->scroll > ms)
            p->scroll = ms;
        page_redraw(p);
        return;
    }
    int top = p->vy0 - p->scroll;
    int n = p->nshown < nold ? p->nshown : nold, i = 0;
    for (; i < n; ++i) {
        const struct pitem *a = &p->shown[i], *b = &old[i];
        if (a->y != b->y || a->h != b->h || a->kind != b->kind)
            break;
        if (a->hash != b->hash)
            draw_band(p, top + a->y, top + a->y + a->h);
    }
    if (i < p->nshown || i < nold) {
        int from = i < p->nshown ? p->shown[i].y : p->total;
        if (i < nold && old[i].y < from)
            from = old[i].y;
        draw_band(p, top + from, p->vy1);
    }
}

#ifdef SB_HOST
struct page *host_last_page;    /* the preview's scroll check: redraw it whole and compare */
#endif

void page_set_scroll(struct page *p, int s)
{
#ifdef SB_HOST
    host_last_page = p;
#endif
    int ms = page_max_scroll(p);
    if (s > ms)
        s = ms;
    if (s < 0)
        s = 0;
    int d = s - p->scroll;
    if (!d)
        return;
    p->scroll = s;
    int vh = p->vy1 - p->vy0;
    if (!p->drawn || (d > 0 ? d : -d) >= vh * 3 / 4) {
        page_redraw(p);
        return;
    }
    ui_scroll_rows(p->vy0, p->vy1, -d);
    if (d > 0)
        draw_band(p, p->vy1 - d, p->vy1);
    else
        draw_band(p, p->vy0, p->vy0 - d);
}

const struct pitem *page_row_item(const struct page *p, int row)
{
    for (int i = 0; i < p->nshown; ++i)
        if (p->shown[i].row == row)
            return &p->shown[i];
    return 0;
}

static void draw_row(struct page *p, int row)
{
    const struct pitem *it = page_row_item(p, row);
    if (!it)
        return;
    int top = p->vy0 - p->scroll;
    draw_band(p, top + it->y, top + it->y + it->h);
}

void page_set_focus(struct page *p, int row)
{
    int old = p->focus;
    p->focus = row;
    if (old != row && p->drawn) {
        if (old >= 0)
            draw_row(p, old);
        if (row >= 0)
            draw_row(p, row);
    }
}

void page_press(struct page *p, int row, int on)
{
    int old = p->pressed;
    p->pressed = on ? row : -1;
    if (old >= 0 && old != p->pressed)
        draw_row(p, old);
    if (p->pressed >= 0 && p->pressed != old)
        draw_row(p, p->pressed);
}

int page_hit(const struct page *p, int x, int y, int *item)
{
    (void)x;
    if (y < p->vy0 || y >= p->vy1)
        return -1;
    int cy = y - p->vy0 + p->scroll;
    for (int i = 0; i < p->nshown; ++i) {
        const struct pitem *it = &p->shown[i];
        if (cy >= it->y && cy < it->y + it->h) {
            if (item)
                *item = i;
            return it->row >= 0 && !(it->flags & R_DIS) ? it->row : -1;
        }
    }
    return -1;
}

int page_next_row(const struct page *p, int from, int dir)
{
    for (int r = from + dir; r >= 0 && r < p->nrows; r += dir) {
        const struct pitem *it = page_row_item(p, r);
        if (it && !(it->flags & R_DIS))
            return r;
    }
    return from;
}

void page_keep_visible(struct page *p, int row)
{
    const struct pitem *it = page_row_item(p, row);
    if (!it)
        return;
    int vh = p->vy1 - p->vy0;
    if (it->y - 40 < p->scroll)
        page_set_scroll(p, it->y - 40);
    else if (it->y + it->h + 40 > p->scroll + vh)
        page_set_scroll(p, it->y + it->h + 40 - vh);
}

int page_scroll_event(struct page *p, const struct ui_ev *e)
{
    switch (e->type) {
    case EV_PRESS:
        p->fling_v = 0;
        return 0;
    case EV_DRAG:
        p->fling_v = 0;
        page_press(p, -1, 0);
        page_set_scroll(p, p->scroll - e->d);
        return 1;
    case EV_RELEASE:
        p->fling_v = -e->d;
        if (p->fling_v > 12000)
            p->fling_v = 12000;
        if (p->fling_v < -12000)
            p->fling_v = -12000;
        p->fling_t = sb_us();
        return 1;
    default:
        return 0;
    }
}

int page_fling_step(struct page *p)
{
    if (!p->fling_v)
        return 0;
    uint64_t now = sb_us();
    int64_t dt = (int64_t)(now - p->fling_t);
    if (dt < 1000)
        return 1;
    if (dt > 50000)
        dt = 50000;
    p->fling_t = now;
    int before = p->scroll;
    page_set_scroll(p, p->scroll + (int)((int64_t)p->fling_v * dt / 1000000));
    /* deceleration: about iOS's 0.998 per ms */
    int64_t v = p->fling_v;
    for (int64_t t = 0; t < dt; t += 4000)
        v = v * 992 / 1000;
    p->fling_v = (int)v;
    if (p->scroll == before || (p->fling_v < 60 && p->fling_v > -60))
        p->fling_v = 0;
    return p->fling_v != 0;
}
