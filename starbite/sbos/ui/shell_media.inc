/* SPDX-License-Identifier: MIT
 * Photos, Files and Music for Starbite OS Next: the microSD card's exFAT partition, read only (included by
 * shell_lock.c after shell_cell.inc; it uses the shell's drawing helpers and app state). The platform gives four
 * calls (shell.h: sh_card_status / _mount / _list / _read); nothing here ever writes, and the OS store's partition
 * on the same card is never looked at (the kernel's reader only opens the exFAT partition).
 *
 * The card is slow, so nothing is read in a touch handler: every read, listing and decode is a step of med_step(),
 * run from the idle refresh (sh_refresh, ~10 ms a turn, then a frame) and, in small steps only, between the frames
 * of a gesture (sh_tick, 2.5 ms). A step is one page of a folder, 32 KiB of a file, one MCU row of a JPEG or ~32 KiB
 * of a PNG's pixels.
 *
 * The page is not the usual four strips (17 MB): a header layer, and the content as tiles that scroll under it and
 * are redrawn as they come into view (Files / Music: a row a tile at full resolution; Photos: one row of three
 * thumbnails a tile at half resolution, scaled 2x). The heap (an own page's strips: 17 MB + 4.3 of half copies):
 *   Files / Music   12 tiles 1440 x 226 = 15.6 MB + their half copies 3.9 + the header's words 2.4; Now Playing
 *                   (9 MB with its half copies) and a picture opened from Files free the tiles while they cover them
 *   Photos          7 tiles 720 x 241 = 4.9 MB + halves 1.2 + the header's words 2.4; the viewer's picture at most
 *                   720 x 1480 = 4.3 MB (+ 1.1); a decode: 192 KiB window (JPEG) or the file (PNG, at most 6 MiB)
 *   kept after the first open: the library's index (photos 120 KB, songs 68 KB at most). */

#define MD_HDR (CONTENT_TOP + 8)             /* the header's height, pt (P3r: iOS's bands: nav row, large title, a caption) */
#define MD_NT 12                           /* tiles at most: Files / Music 12 (a row each), Photos 7 */
#define MD_RP 226                           /* a Files / Music row, px (61.2 pt) */
#define MD_CELL 238                         /* a thumbnail, half px; + 3 the gap */
#define MD_PNG_MAX (6u << 20)
enum { MD_NONE, MD_FILES, MD_PHOTOS, MD_MUSIC };
enum { MK_OTHER, MK_PHOTO, MK_AUDIO, MK_IMAGE, MK_VIDEO, MK_DOC, MK_ZIP };

static tk_layer medh, medht, medmsg, medt[MD_NT], medvbg, medvimg, medvui, medvbar, medverr, medpbg, medpart, medpnav, medpinfo;
static tk_layer medtg, medseg, medvbot, medvth;      /* B5: Photos' tiles' clipped group, its Years / Months / All
                                                      * capsule, the viewer's bottom circles, the opening thumbnail */

/* ---- the library: the card's pictures and songs, found a folder page at a time ------------------------------- */
#define ML_DIRS 48
#define ML_PHOTOS 1500
#define ML_TRACKS 400
typedef struct { uint32_t date, size; uint8_t dir, meta; char name[70]; } ml_item;
typedef struct { ml_item it; char title[48], artist[40]; } ml_track;
static struct {
    char dirs[ML_DIRS][200];
    uint8_t depth[ML_DIRS];
    int ndirs, cur, first, state;           /* state: 0 not started, 1 looking, 2 done */
    ml_item *ph;
    ml_track *tr;
    int nph, ntr;
} ML;

static struct {
    int app, pend, card;
    char why[64];
    int hdr_px, tp, ntile, half, rows, nt;
    int tidx[MD_NT];
    uint8_t tcell[MD_NT][3];
    float scroll, v, base;
    int momentum;
    tk_spring sp;
    /* the finger */
    int down, moved, axis, zone;
    float x0, y0, bx, by;
    tk_tracker trx, try_;
    /* Files */
    char dir[256];
    sh_dirent_t *ent;
    int nent, listing, lerr, more, lfirst;
    /* the viewer */
    int view, vsingle, vanim, vdir, verr;
    char vpath[400];
    float vx, vy, va, vat;
    /* Now Playing */
    int play, pstage, redo;
    float pa;
    /* B5: Photos as iOS 26: Years / Months / All (0 all, 1 months, 2 years), opened at the newest; the viewer's zoom
     * (vz now, vzt the target; px, py the pan, pt), its chrome (vui 1 shown), a tap waiting for a second one, the
     * full-resolution decode once zoomed (vfull: 0 half, 1 decoding, 2 full); the picture's own size */
    int vmode, to_end, ngrp, vui, vfull, tap_wait;
    float vz, vzt, px, py, pxt, pyt, tap_t, tap_x, tap_y;
    int vsw, vsh;
    float tha, thx, thy, thw;                 /* the opening thumbnail's flight (tha 0..1) */
} MD = { .view = -1, .play = -1, .vz = 1, .vzt = 1, .vui = 1 };
/* Months / Years: the groups of the library (oldest first), a card each */
#define MD_GRP 160
static struct { int first, n, key; } MDG[MD_GRP];
/* the thumbnails decoded so far (All): RGB565 at the cell's size, the least recently used given back first, so
 * scrolling back never decodes again (96 x 113 KB at most, freed when Photos closes) */
#define TC_N 96
static struct { int photo; uint32_t use; uint16_t *px; } TC[TC_N];
static uint32_t tc_clock;

static int med_is(const char *k) { return !tk_strcmp(k, "photos") || !tk_strcmp(k, "files") || !tk_strcmp(k, "music"); }

/* ---- small things -------------------------------------------------------------------------------------------- */
static float md_rubber(float over, float dim) { return (1 - 1 / (over * .55f / dim + 1)) * dim; }
static int md_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int md_cmp(const char *a, const char *b)
{
    while (*a && md_lc((unsigned char)*a) == md_lc((unsigned char)*b)) { a++; b++; }
    return md_lc((unsigned char)*a) - md_lc((unsigned char)*b);
}
static int md_cat(char *o, int n, int cap, const char *s)
{
    while (*s && n + 1 < cap) o[n++] = *s++;
    o[n] = 0;
    return n;
}
static int md_path(char *o, int cap, const char *dir, const char *name)
{
    int n = md_cat(o, 0, cap, dir[0] == '/' && !dir[1] ? "" : dir);
    n = md_cat(o, n, cap, "/");
    n = md_cat(o, n, cap, name);
    return (size_t)n == tk_strlen(dir[0] == '/' && !dir[1] ? "" : dir) + 1 + tk_strlen(name);     /* 0: it did not fit */
}
static const char *md_ext(const char *name)
{
    const char *e = 0;
    for (const char *c = name; *c; c++) if (*c == '.') e = c + 1;
    return e ? e : "";
}
static int md_kind(const char *name)
{
    static const struct { const char *e; int k; } X[] = {
        { "jpg", MK_PHOTO }, { "jpeg", MK_PHOTO }, { "png", MK_PHOTO }, { "mp3", MK_AUDIO }, { "aac", MK_AUDIO },
        { "m4a", MK_AUDIO }, { "flac", MK_AUDIO }, { "wav", MK_AUDIO }, { "heic", MK_IMAGE }, { "gif", MK_IMAGE },
        { "webp", MK_IMAGE }, { "bmp", MK_IMAGE }, { "mp4", MK_VIDEO }, { "mov", MK_VIDEO }, { "mkv", MK_VIDEO },
        { "pdf", MK_DOC }, { "txt", MK_DOC }, { "doc", MK_DOC }, { "docx", MK_DOC }, { "zip", MK_ZIP }, { "gz", MK_ZIP },
        { "7z", MK_ZIP }, { "img", MK_ZIP } };
    const char *e = md_ext(name);
    for (unsigned i = 0; i < sizeof X / sizeof X[0]; i++) if (!md_cmp(e, X[i].e)) return X[i].k;
    return MK_OTHER;
}
/* "3 Nov 2026" from exFAT's stamp ("" if none) */
static void md_date(char *b, uint32_t st)
{
    int y = (int)(st >> 25) + 1980, m = (int)(st >> 21 & 15), d = (int)(st >> 16 & 31);
    b[0] = 0;
    if (!st || m < 1 || m > 12 || !d) return;
    int n = put_dec(b, d);
    b[n++] = ' ';
    for (int i = 0; i < 3; i++) b[n++] = MO[m - 1][i];
    b[n++] = ' ';
    n += put_dec(b + n, y);
    b[n] = 0;
}
static void md_size(char *b, uint64_t s)
{
    static const char *const U[4] = { " bytes", " KB", " MB", " GB" };
    int u = 0;
    uint64_t v = s * 10;
    while (v >= 10240 && u < 3) { v /= 1024; u++; }
    int n = put_dec(b, (int)(v / 10));
    if (u >= 2 && v < 1000) { b[n++] = '.'; b[n++] = (char)('0' + v % 10); }
    md_cat(b, n, 24, U[u]);
}
static int md_count(char *b, int n, const char *one, const char *many)
{
    int k = put_dec(b, n);
    b[k++] = ' ';
    return md_cat(b, k, 64, n == 1 ? one : many);
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */
/* a filled triangle (the transport's glyphs): coverage from four sub-rows a pixel */
static void d_tri(dc *d, float ax, float ay, float bx, float by, float cx, float cy, tk_col col)
{
    float X[3] = { PT(ax) - d->ox, PT(bx) - d->ox, PT(cx) - d->ox }, Y[3] = { PT(ay) - d->oy, PT(by) - d->oy, PT(cy) - d->oy };
    int x0 = (int)tk_floorf(tk_minf(X[0], tk_minf(X[1], X[2]))), x1 = (int)tk_floorf(tk_maxf(X[0], tk_maxf(X[1], X[2]))) + 1;
    int y0 = (int)tk_floorf(tk_minf(Y[0], tk_minf(Y[1], Y[2]))), y1 = (int)tk_floorf(tk_maxf(Y[0], tk_maxf(Y[1], Y[2]))) + 1;
    tk_fm m;
    if (x1 <= x0 || y1 <= y0 || !tk_fm_new(&m, x1 - x0, y1 - y0)) return;
    for (int j = 0; j < m.h; j++)
        for (int s = 0; s < 4; s++) {
            float y = (float)(y0 + j) + (s + .5f) / 4, xl = 1e9f, xr = -1e9f;
            for (int e = 0; e < 3; e++) {
                float ya = Y[e], yb = Y[(e + 1) % 3], xa = X[e], xb = X[(e + 1) % 3];
                if ((ya <= y && yb > y) || (yb <= y && ya > y)) {
                    float x = xa + (xb - xa) * (y - ya) / (yb - ya);
                    xl = tk_minf(xl, x); xr = tk_maxf(xr, x);
                }
            }
            for (int i = 0; i < m.w && xr > xl; i++) {
                float c = tk_minf(xr, (float)(x0 + i + 1)) - tk_maxf(xl, (float)(x0 + i));
                if (c > 0) m.v[j * m.w + i] += tk_minf(c, 1) / 4;
            }
        }
    tk_paint p = tk_solid(col);
    tk_draw_fm(d->b, &m, x0, y0, &p, 1);
    tk_fm_free(&m);
}
/* a pair of beamed notes in an s pt box */
static void d_note(dc *d, float x, float y, float s, tk_col c)
{
    d_rrect(d, x + .10f * s, y + .64f * s, .32f * s, .25f * s, .12f * s, c);
    d_rrect(d, x + .56f * s, y + .54f * s, .32f * s, .25f * s, .12f * s, c);
    d_rrect(d, x + .35f * s, y + .12f * s, .07f * s, .65f * s, .02f * s, c);
    d_rrect(d, x + .81f * s, y + .12f * s, .07f * s, .55f * s, .02f * s, c);
    d_rrect(d, x + .35f * s, y + .08f * s, .53f * s, .15f * s, .03f * s, c);
}
/* Files' 40 pt icons: a folder, or a page with its kind's colour band and extension */
static void d_fileicon(dc *d, float x, float y, const sh_dirent_t *e)
{
    if (e->dir) {
        d_rrect(d, x + 2, y + 5, 17, 10, 3, tk_hex(0x3a8fe8, 1));
        d_rrect(d, x + 2, y + 9, 36, 25, 5, tk_hex(0x57a9ff, 1));
        return;
    }
    static const uint32_t KC[7] = { 0x8e8e93, 0x34c759, 0xff375f, 0x34c759, 0xaf52de, 0x2f7bf6, 0x8e8e93 };
    int k = md_kind(e->name);
    d_rrect(d, x + 6, y + 2, 28, 36, 5, tk_hex(0xe9edf3, 1));
    d_rrect(d, x + 6, y + 21, 28, 12, 0, tk_hex(KC[k], 1));
    char up[5];
    const char *x4 = md_ext(e->name);
    int n = 0;
    for (; x4[n] && n < 4; n++) up[n] = x4[n] >= 'a' && x4[n] <= 'z' ? (char)(x4[n] - 32) : x4[n];
    up[n] = 0;
    if (x4[n]) up[0] = 0;                       /* a longer extension: the band alone */
    float w = tw(TK_IT700, 8, up, 0, 0);
    d_text(d, TK_IT700, 8, x + 20 - w / 2, y + 30, up, 0, 0, W(1), 0);
}

/* ---- the platform's card ------------------------------------------------------------------------------------- */
static void med_card(void)
{
    const char *why = "";
    MD.card = sh_card_status(&why);
    md_cat(MD.why, 0, sizeof MD.why, why ? why : "");
}

/* ---- the decode job: one picture at a time, into a thumbnail's cell or the viewer --------------------------- */
static struct {
    int kind;                   /* 0 none, 1 a thumbnail, 2 the viewer's picture */
    int photo, tile, slot, cell, stage;
    char path[400];
    uint64_t size;
    uint8_t *file;              /* a PNG's whole file, or a JPEG's EXIF thumbnail */
    uint32_t got;
    mj_t j;
    mp_t p;
    mr_t r;
    tk_bmp out;
    int bw, bh;                 /* the box the picture must fill (thumbnail) or fit (viewer), before its turn */
    const char *err;
} DJ;
enum { DS_SNIFF, DS_PNG_READ, DS_JPEG, DS_PNG };

static int dj_read(void *ctx, uint64_t off, uint8_t *dst, uint32_t len) { (void)ctx; return sh_card_read(DJ.path, off, dst, len); }
static void dj_row(void *ctx, int y, const uint8_t *rgb) { (void)ctx; mr_row(&DJ.r, y, rgb); }
static void dj_free(void)
{
    mj_free(&DJ.j);
    mp_free(&DJ.p);
    mr_free(&DJ.r);
    tk_free(DJ.file);
    DJ.file = 0;
    tk_bmp_free(&DJ.out);
    DJ.kind = 0;
}
static float dj_progress(void)
{
    if (DJ.stage == DS_JPEG && DJ.j.mcuy) return (float)DJ.j.my / (float)DJ.j.mcuy;
    if (DJ.stage == DS_PNG_READ && DJ.size) return .3f * (float)DJ.got / (float)DJ.size;
    if (DJ.stage == DS_PNG && DJ.p.h) return .3f + .7f * (float)DJ.p.y / (float)DJ.p.h;
    return 0;
}
/* the rows of a w x h picture (as decoded) into DJ.out: a centred square for a thumbnail, the whole for the viewer */
static int dj_target(int w, int h, int orient)
{
    mr_t *r = &DJ.r;
    tk_memset(r, 0, sizeof *r);
    r->sw = w; r->sh = h; r->orient = orient;
    if (DJ.kind == 1) {
        /* B5: the box filled (a square cell, or a card): the centre cut to its shape */
        int turn = orient == 6 || orient == 8, bw = turn ? DJ.bh : DJ.bw, bh = turn ? DJ.bw : DJ.bh;
        if ((int64_t)w * bh > (int64_t)h * bw) { r->ch = h; r->cw = tk_maxi(1, (int)((int64_t)h * bw / bh)); }
        else { r->cw = w; r->ch = tk_maxi(1, (int)((int64_t)w * bh / bw)); }
        r->cx = (w - r->cw) / 2; r->cy = (h - r->ch) / 2;
        r->tw = tk_mini(r->cw, bw); r->th = tk_mini(r->ch, bh);
    } else {
        int turn = orient == 6 || orient == 8, ow = DJ.stage == DS_SNIFF ? DJ.j.w : w, oh = DJ.stage == DS_SNIFF ? DJ.j.h_ : h;
        MD.vsw = turn ? oh : ow; MD.vsh = turn ? ow : oh;   /* the picture's own size (info) */
        int bw = turn ? DJ.bh : DJ.bw, bh = turn ? DJ.bw : DJ.bh;
        float f = tk_minf(1, tk_minf((float)bw / (float)w, (float)bh / (float)h));
        r->cw = w; r->ch = h;
        r->tw = tk_maxi(1, (int)((float)w * f)); r->th = tk_maxi(1, (int)((float)h * f));
    }
    int W_, H_;
    mr_size(r, &W_, &H_);
    r->dst = &DJ.out;
    if (!tk_bmp_new(&DJ.out, W_, H_) || !mr_init(r)) { DJ.err = "Not enough memory for this picture."; return 0; }
    return 1;
}
static void dj_jpeg_file(void)
{
    tk_memset(&DJ.j, 0, sizeof DJ.j);
    DJ.j.buf = tk_alloc(MJ_WIN);
    DJ.j.own_buf = 1;
    DJ.j.cap = DJ.j.buf ? MJ_WIN : 0;
    DJ.j.fsize = DJ.size;
    DJ.j.rd = dj_read;
    DJ.j.row = dj_row;
}
/* the size the decoder must give for the target (as decoded, before the turn) */
static void dj_want(int w, int h, int orient, int *ww, int *wh)
{
    if (DJ.kind == 1) {
        int turn = orient == 6 || orient == 8, bw = turn ? DJ.bh : DJ.bw, bh = turn ? DJ.bw : DJ.bh;
        float sc = tk_maxf((float)bw / (float)w, (float)bh / (float)h);
        *ww = (int)((float)w * sc + .999f); *wh = (int)((float)h * sc + .999f);
        return;
    }
    int turn = orient == 6 || orient == 8, bw = turn ? DJ.bh : DJ.bw, bh = turn ? DJ.bw : DJ.bh;
    float f = tk_minf(1, tk_minf((float)bw / (float)w, (float)bh / (float)h));
    *ww = (int)((float)w * f);
    *wh = (int)((float)h * f);
}
/* 0 more to do, 1 done (DJ.out is the picture), -1 failed (DJ.err) */
static int dj_step(void)
{
    if (DJ.stage == DS_SNIFF) {
        uint8_t sig[8];
        int n = sh_card_read(DJ.path, 0, sig, 8);
        if (n < 8) { DJ.err = n < 0 ? "The card could not be read." : "The picture's file is empty."; return -1; }
        if (sig[0] == 0x89 && sig[1] == 'P') {
            if (DJ.size > MD_PNG_MAX) { DJ.err = "PNG pictures over 6 MB can't be shown yet."; return -1; }
            DJ.file = tk_alloc((size_t)DJ.size + 512);
            if (!DJ.file) { DJ.err = "Not enough memory for this picture."; return -1; }
            DJ.got = 0;
            DJ.stage = DS_PNG_READ;
            return 0;
        }
        if (sig[0] != 0xff || sig[1] != 0xd8) { DJ.err = "This kind of picture can't be shown yet."; return -1; }
        dj_jpeg_file();
        if (!DJ.j.buf) { DJ.err = "Not enough memory for this picture."; return -1; }
        if (!mj_head(&DJ.j)) { DJ.err = DJ.j.err; return -1; }
        int orient = DJ.j.orient, ww, wh;
        /* a thumbnail from the camera's own small copy in the EXIF block, when it has one of a useful size */
        if (DJ.kind == 1 && DJ.j.th_len >= 1024 && DJ.j.th_len <= 65536 && (DJ.file = tk_alloc(DJ.j.th_len + 1024)) != 0) {
            /* (read as whole sectors from a sector's start: the card's fast path) */
            uint32_t tl = DJ.j.th_len, lead = (uint32_t)(DJ.j.th_off & 511), span = (lead + tl + 511) & ~511u;
            int got = sh_card_read(DJ.path, DJ.j.th_off - lead, DJ.file, span);
            mj_free(&DJ.j);
            tk_memset(&DJ.j, 0, sizeof DJ.j);
            DJ.j.buf = DJ.file + lead; DJ.j.cap = DJ.j.len = tl; DJ.j.fsize = tl; DJ.j.eof = 1; DJ.j.row = dj_row;
            if (got >= (int)(lead + tl) && mj_head(&DJ.j) && tk_mini(DJ.j.w, DJ.j.h_) >= 120) {
                DJ.j.orient = orient;
            } else {
                /* no use: the picture itself */
                mj_free(&DJ.j);
                tk_free(DJ.file);
                DJ.file = 0;
                dj_jpeg_file();
                if (!DJ.j.buf || !mj_head(&DJ.j)) { DJ.err = DJ.j.err ? DJ.j.err : "Not enough memory for this picture."; return -1; }
            }
        }
        dj_want(DJ.j.w, DJ.j.h_, orient, &ww, &wh);
        if (!mj_start(&DJ.j, ww, wh)) { DJ.err = DJ.j.err; return -1; }
        if (!dj_target(DJ.j.ow, DJ.j.oh, orient)) return -1;
        DJ.stage = DS_JPEG;
        return 0;
    }
    if (DJ.stage == DS_PNG_READ) {
        uint32_t want = (uint32_t)DJ.size - DJ.got;
        int n = sh_card_read(DJ.path, DJ.got, DJ.file + DJ.got, want > (32u << 10) ? (32u << 10) : want);
        if (n <= 0) { DJ.err = "The card could not be read."; return -1; }
        DJ.got += (uint32_t)n;
        if (DJ.got < DJ.size) return 0;
        tk_memset(&DJ.p, 0, sizeof DJ.p);
        DJ.p.in = DJ.file; DJ.p.n = DJ.got; DJ.p.row = dj_row;
        if (!mp_head(&DJ.p)) { DJ.err = DJ.p.err; return -1; }
        if (!dj_target(DJ.p.w, DJ.p.h, 1)) return -1;
        DJ.stage = DS_PNG;
        return 0;
    }
    int rc = DJ.stage == DS_JPEG ? mj_step(&DJ.j) : mp_step(&DJ.p);
    if (rc < 0) { DJ.err = DJ.stage == DS_JPEG ? DJ.j.err : DJ.p.err; return -1; }
    if (rc) mr_flush(&DJ.r);
    return rc;
}
static void dj_begin(int kind, const char *path, uint64_t size, int bw, int bh)
{
    if (DJ.kind) dj_free();
    DJ.kind = kind;
    DJ.stage = DS_SNIFF;
    DJ.err = 0;
    DJ.size = size;
    DJ.bw = bw; DJ.bh = bh;
    md_cat(DJ.path, 0, sizeof DJ.path, path);
}

/* ---- the tiles ------------------------------------------------------------------------------------------------ */
/* B5: Photos has no large title: the grid starts under a 22 pt "Library" row and scrolls under the status band, and
 * ends over the Years / Months / All capsule */
#define MD_PTOP 98.0f                        /* Photos: the grid's top at scroll 0 */
#define MD_PBOT 96.0f                        /* room at its end (the capsule and the home band) */
#define MD_CARD_H 236.0f                     /* a Months / Years card, pt (with its 14 gap) */
static float md_top(void) { return MD.app == MD_PHOTOS ? MD_PTOP : MD_HDR; }
static float med_max(void)
{
    float all = (float)(MD.ntile * MD.tp) / TK_PT + (MD.app == MD_PHOTOS ? MD_PBOT : 40), vp = H_PT - md_top();
    return all > vp ? all - vp : 0;
}
static uint16_t *tc_get(int photo)
{
    for (int i = 0; i < TC_N; i++) if (TC[i].px && TC[i].photo == photo) { TC[i].use = ++tc_clock; return TC[i].px; }
    return 0;
}
static void tc_put(int photo, const tk_bmp *b, int x0)
{
    int k = -1;
    for (int i = 0; i < TC_N && k < 0; i++) if (!TC[i].px) k = i;
    if (k < 0) { k = 0; for (int i = 1; i < TC_N; i++) if (TC[i].use < TC[k].use) k = i; }
    if (!TC[k].px && !(TC[k].px = tk_alloc((size_t)MD_CELL * MD_CELL * 2))) return;
    TC[k].photo = photo; TC[k].use = ++tc_clock;
    for (int y = 0; y < MD_CELL; y++)
        for (int x = 0; x < MD_CELL; x++) {
            uint32_t c = b->px[(size_t)y * b->stride + x0 + x];
            TC[k].px[y * MD_CELL + x] = (uint16_t)((c >> 8 & 0xf800) | (c >> 5 & 0x7e0) | (c >> 3 & 0x1f));
        }
}
static void tc_blit(tk_bmp *b, int x0, const uint16_t *t)
{
    for (int y = 0; y < MD_CELL; y++)
        for (int x = 0; x < MD_CELL; x++) {
            uint32_t v = t[y * MD_CELL + x], r = v >> 11 & 31, g = v >> 5 & 63, bl = v & 31;
            b->px[(size_t)y * b->stride + x0 + x] = 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2);
        }
}
static void tc_free(void)
{
    for (int i = 0; i < TC_N; i++) { tk_free(TC[i].px); TC[i].px = 0; }
}
static void med_row_files(dc *d, int r)
{
    float cy = (float)(r * MD_RP) / TK_PT;
    char b[160], dt[24], sz[24];
    if (r == MD.nent) {
        /* the folder's foot, as iOS Files: how many, and that nothing here can be changed */
        int n = md_count(b, MD.nent, "item", "items");
        n = md_cat(b, n, sizeof b, MD.more ? " shown (the folder has more)" : "");
        md_cat(b, n, sizeof b, " \xc2\xb7 Read only");
        float w = tw(TK_IT400, 13, b, 0, TK_TNUM);
        d_text(d, TK_IT400, 13, (390 - w) / 2, bl(TK_IT400, 13, cy + 16), b, 0, TK_TNUM, C_LABEL2, 0);
        return;
    }
    if (r > MD.nent) return;
    const sh_dirent_t *e = &MD.ent[r];
    d_fileicon(d, 20, cy + 10, e);
    ellipsize(b, sizeof b, TK_IT400, 17, e->name, e->dir ? 262 : 286);
    d_text(d, TK_IT400, 17, 72, bl(TK_IT400, 17, cy + 10), b, 0, 0, INK, 0);
    md_date(dt, e->date);
    int n = 0;
    if (e->dir) n = md_cat(b, 0, sizeof b, "Folder");
    else { md_size(sz, e->size); n = md_cat(b, 0, sizeof b, sz); }
    if (dt[0]) { n = md_cat(b, n, sizeof b, " \xc2\xb7 "); md_cat(b, n, sizeof b, dt); }
    d_text(d, TK_IT400, 13, 72, bl(TK_IT400, 13, cy + 34), b, 0, TK_TNUM, C_LABEL2, 0);
    if (e->dir) d_icon(d, "chev", 356, cy + 23.5f, .45f);
    d_rrect(d, 72, cy + 60.6f, 318, .5f, 0, C_SEP);
}
static void med_row_music(dc *d, int r)
{
    float cy = (float)(r * MD_RP) / TK_PT;
    char b[160], sz[24];
    if (r == ML.ntr) {
        int n = md_count(b, ML.ntr, "song", "songs");
        md_cat(b, n, sizeof b, " \xc2\xb7 Playback isn't available yet");
        float w = tw(TK_IT400, 13, b, 0, TK_TNUM);
        d_text(d, TK_IT400, 13, (390 - w) / 2, bl(TK_IT400, 13, cy + 16), b, 0, TK_TNUM, C_LABEL2, 0);
        return;
    }
    if (r > ML.ntr) return;
    const ml_track *t = &ML.tr[r];
    d_rrect(d, 20, cy + 8, 45, 45, 7, sbc(SBC_GRAY5));
    d_note(d, 31, cy + 18, 23, tk_hex(0xff375f, 1));
    ellipsize(b, sizeof b, TK_IT400, 17, t->title[0] ? t->title : t->it.name, 286);
    d_text(d, TK_IT400, 17, 78, bl(TK_IT400, 17, cy + 10), b, 0, 0, INK, 0);
    int n = 0;
    if (t->artist[0]) n = md_cat(b, 0, sizeof b, t->artist);
    else {
        const char *x = md_ext(t->it.name);
        for (; x[n] && n < 6; n++) b[n] = x[n] >= 'a' && x[n] <= 'z' ? (char)(x[n] - 32) : x[n];
        b[n] = 0;
    }
    md_size(sz, t->it.size);
    n = md_cat(b, n, sizeof b, " \xc2\xb7 ");
    md_cat(b, n, sizeof b, sz);
    char e[160];
    ellipsize(e, sizeof e, TK_IT400, 14, b, 286);
    d_text(d, TK_IT400, 14, 78, bl(TK_IT400, 14, cy + 34), e, 0, TK_TNUM, C_LABEL2, 0);
    d_rrect(d, 78, cy + 60.6f, 312, .5f, 0, C_SEP);
}
static void med_tile_draw(int s, int i)
{
    tk_layer *l = &medt[s];
    int w = MD.half ? TK_W / 2 : TK_W, h = MD.half ? MD.tp / 2 : MD.tp;
    /* (never tk_layer_init here: the layer is linked, and that would cut the page's layers after it loose) */
    tk_layer_dirty(l);
    MD.tidx[s] = i;                             /* (also when there is no memory: no new try every frame) */
    if (l->bmp.w != w || l->bmp.h != h) {
        tk_bmp_free(&l->bmp);
        if (!tk_bmp_new(&l->bmp, w, h)) return;
    }
    tk_fill_rect_raw(&l->bmp, 0, 0, w, h, sbc_argb(SBC_GROUND));
    l->opaque = 1;
    if (MD.app == MD_PHOTOS && MD.vmode) {
        /* a card: its key picture comes from the decode (thumb_next), its words over it then */
        MD.tcell[s][0] = 0;
        tk_fill_rect_raw(&l->bmp, 0, 0, w, h, 0xff0a0f1fu);
        if (i < MD.ngrp) {
            tk_paint pp = tk_solid(sbc(SBC_GRAY6));
            tk_fill_rrect(&l->bmp, 16 * TK_PT / 2, 0, (390 - 32) * TK_PT / 2, (MD_CARD_H - 14) * TK_PT / 2, 22 * TK_PT / 2, &pp, 1);
        }
        MD.pend = 1;
        return;
    }
    if (MD.app == MD_PHOTOS) {
        for (int c = 0; c < 3; c++) {
            MD.tcell[s][c] = 0;
            if (i * 3 + c >= ML.nph) continue;
            const uint16_t *t = tc_get(i * 3 + c);
            if (t) { tc_blit(&l->bmp, c * (MD_CELL + 3), t); MD.tcell[s][c] = 1; }
            else tk_fill_rect_raw(&l->bmp, c * (MD_CELL + 3), 0, MD_CELL, MD_CELL, sbc_argb(SBC_GRAY6));
        }
        MD.pend = 1;
        return;
    }
    dc d = { &l->bmp, 0, 0, (float)(i * MD.tp) };
    if (MD.app == MD_FILES) med_row_files(&d, i); else med_row_music(&d, i);       /* a row a tile */
    if (MD.app == MD_MUSIC) MD.pend = 1;        /* its rows' titles may still have to be read */
}
/* the tiles in view: drawn if they are not, and placed */
static void med_tiles(void)
{
    int sp = (int)tk_floorf(PT(MD.scroll) + .5f), top = (int)(PT(md_top()) + .5f), vp = TK_H - top;
    /* Photos: the rows also go under the header's fade (up to the panel's top) */
    int above = MD.app == MD_PHOTOS ? top : 0;
    int i0 = sp - above < 0 ? 0 : (sp - above) / MD.tp, i1 = sp + vp - 1 < 0 ? -1 : (sp + vp - 1) / MD.tp;
    if (i1 >= MD.ntile) i1 = MD.ntile - 1;
    for (int i = i0; i <= i1; i++) if (MD.tidx[i % MD.nt] != i) med_tile_draw(i % MD.nt, i);
    for (int s = 0; s < MD_NT; s++) {
        tk_layer *l = &medt[s];
        int i = MD.tidx[s];
        if (i < i0 || i > i1 || !l->bmp.px) { l->hidden = 1; continue; }
        float y = (float)(top + i * MD.tp - sp);
        l->hidden = 0;
        l->sx = l->sy = MD.half ? 2 : 1;
        l->ox = l->oy = 0;
        l->x = 0;
        l->y = MD.half ? y / 2 : y;
        l->gen++;
    }
    /* the group: clipped to the panel (Photos: fading under the status band and the capsule) or under the header */
    medtg.clip = 1;
    medtg.clip_r = (tk_rect){ 0, MD.app == MD_PHOTOS ? 0 : (float)MD.hdr_px, TK_W, MD.app == MD_PHOTOS ? TK_H : TK_H - (float)MD.hdr_px };
    medtg.fade_top = 0;
    medtg.fade_bot = 0;
    medtg.gen++;
}
static void med_tiles_reset(void)
{
    for (int s = 0; s < MD_NT; s++) MD.tidx[s] = -1;
}
/* Files' and Music's tiles (23 MB with their half copies) are not kept under a page that covers them (a picture
 * from Files, Now Playing): freed here, drawn again by med_tick (MD.redo) before the page goes */
static void med_tiles_free(void)
{
    for (int s = 0; s < MD_NT; s++) { relayer(&medt[s]); medt[s].hidden = 1; MD.tidx[s] = -1; }
}

/* ---- the header and the states in words ---------------------------------------------------------------------- */
static const char *md_base(const char *path)
{
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '/' && c[1]) b = c + 1;
    return b;
}
static int md_root(void) { return !MD.dir[0] || (MD.dir[0] == '/' && !MD.dir[1]); }
/* B5: Photos' header: the panel's top darkened into the grid (iOS's scroll edge), "Library" 22 / 700 with the
 * library's dates or count, the Select capsule; the bottom's capsule (Years / Months / All) over its own fade */
static const char *const MD_SEG[3] = { "Years", "Months", "All" };
#define SEG_W 244.0f
#define SEG_Y (H_PT - 30 - 8 - 40)
static void med_seg(void)
{
    relayer(&medseg);
    medseg.hidden = 1;
    if (MD.app != MD_PHOTOS) return;
    tk_layer *nx = medseg.next;
    dc d = mk(&medseg, "med-seg", 0, SEG_Y - 52, 390, H_PT - SEG_Y + 52, 0, 0);
    medseg.next = nx;
    if (!d.b->px) return;
    for (int j = 0; j < d.b->h; j++) {                 /* the fade: the ground from nothing to .9 */
        float a = tk_clampf((float)j / (float)PT(70), 0, 1) * .9f;
        tk_fill_rect_raw(d.b, 0, j, d.b->w, 1, (uint32_t)(a * 255) << 24 | (uint32_t)(a * 0x0a) << 16 | (uint32_t)(a * 0x0f) << 8 | (uint32_t)(a * 0x1f));
    }
    float x0 = 195 - SEG_W / 2, sw = SEG_W / 3;
    d_rrect(&d, x0, SEG_Y, SEG_W, 40, 20, W(.2f));
    d_rrect(&d, x0 + .5f, SEG_Y + .5f, SEG_W - 1, 39, 19.5f, tk_hex(0x1e2333, 1));
    int sel = MD.vmode == 0 ? 2 : MD.vmode == 1 ? 1 : 0;
    d_rrect(&d, x0 + 3 + sw * sel, SEG_Y + 3, sw - 6, 34, 17, tk_hex(0x636366, 1));     /* the selection (§12.2) */
    for (int k = 0; k < 3; k++) d_text_box(&d, TK_IT600, 14, x0 + sw * k + sw / 2, bl(TK_IT600, 14, SEG_Y + 11), sw - 8, 1, MD_SEG[k], 0, 0, W(k == sel ? 1 : .8f), 0);
    medseg.hidden = 0;
}
static void med_pheader(void)
{
    char b[96], d1[24], d2[24];
    dc d = { medht.bmp.px ? &medht.bmp : 0, 0, 0, 0 };
    relayer(&medht);
    tk_layer *nx = medht.next;
    d = mk(&medht, "med-head-text", 0, 0, 390, MD_PTOP + 16, 0, 0);
    medht.next = nx;
    if (!d.b->px) return;
    for (int j = 0; j < d.b->h; j++) {                 /* the scroll edge: .92 under the status band, then nothing */
        float u = (float)j / (float)d.b->h, a = u < .62f ? .94f : .94f * (1 - (u - .62f) / .38f);
        tk_fill_rect_raw(d.b, 0, j, d.b->w, 1, (uint32_t)(a * 255) << 24 | (uint32_t)(a * 0x0a) << 16 | (uint32_t)(a * 0x0f) << 8 | (uint32_t)(a * 0x1f));
    }
    const char *t = MD.vmode == 1 ? "Months" : MD.vmode == 2 ? "Years" : "Library";
    d_text(&d, TK_IT700, 22, MARGIN, bl(TK_IT700, 22, SAFE_TOP + 6), t, 0, 0, INK, 0);
    const char *line = "";
    b[0] = 0;
    if (ML.state == 2 && ML.nph) {
        md_date(d1, ML.ph[0].date);
        md_date(d2, ML.ph[ML.nph - 1].date);
        int n = 0;
        if (d1[0] && d2[0] && tk_strcmp(d1, d2)) { n = md_cat(b, 0, sizeof b, d1); n = md_cat(b, n, sizeof b, " \xe2\x80\x93 "); md_cat(b, n, sizeof b, d2); }
        else md_count(b, ML.nph, "photo", "photos");
        line = b;
    } else if (ML.state == 2) line = "";
    else if (MD.card == SH_CARD_READY) { int n = md_cat(b, 0, sizeof b, "Looking for photos\xe2\x80\xa6 "); n += put_dec(b + n, ML.nph); md_cat(b, n, sizeof b, " found"); line = b; }
    d_text(&d, TK_IT400, 13, MARGIN, bl(TK_IT400, 13, SAFE_TOP + 33), line, 0, TK_TNUM, C_LABEL2, 0);
    /* Select: a glass capsule (the card is read only: selecting does nothing yet) */
    float sw = tw(TK_IT500, 15, "Select", 0, 0) + 28;
    d_rrect(&d, CONTENT_R - sw, SAFE_TOP + 4, sw, 36, 18, W(.16f));
    d_text(&d, TK_IT500, 15, CONTENT_R - sw + 14, bl(TK_IT500, 15, SAFE_TOP + 13), "Select", 0, 0, INK, 0);
    med_seg();
}
static void med_header(void)
{
    /* the header's words (its ground, over the tiles that scroll under it, is medh: a solid layer, no bitmap) */
    relayer(&medht);
    dc d = mk(&medht, "med-head-text", 0, 44, 390, MD_HDR - 44, 0, 0);
    if (!d.b->px) return;
    char b[96];
    const char *line = "";
    b[0] = 0;
    if (MD.app == MD_FILES && !md_root()) {
        /* a folder: back to its parent, its name */
        char par[200], nm[200];
        int n = md_cat(par, 0, sizeof par, MD.dir);
        while (n > 0 && par[n - 1] != '/') n--;
        par[n > 1 ? n - 1 : 0] = 0;
        d_icon(&d, "back", MARGIN, NAV_MID - 10, 1);
        ellipsize(nm, sizeof nm, TK_IT400, 17, par[0] ? md_base(par) : "microSD Card", 300);
        d_text(&d, TK_IT400, 17, MARGIN + 14, NAV_BASE, nm, 0, 0, ACCENT(), 0);
        d_h1(&d, md_base(MD.dir));
        return;
    }
    if (MD.app == MD_FILES) {
        d_h1(&d, "microSD Card");
        line = MD.card == SH_CARD_READY ? "Read only \xc2\xb7 exFAT" : "Read only";
    } else if (MD.app == MD_PHOTOS) {
        med_pheader();
        return;
    } else {
        d_h1(&d, "Library");
        line = "Playback isn't available yet";
    }
    d_text(&d, TK_IT400, 13, MARGIN, LARGE_BASE + 20, line, 0, TK_TNUM, C_LABEL2, 0);
}
/* what to say instead of a list (0: the list is shown) */
static int med_words(const char **title, const char **line)
{
    *line = "";
    if (MD.card == SH_CARD_MOUNTING) { *title = "Reading the Card\xe2\x80\xa6"; return 1; }
    if (MD.card == SH_CARD_NONE) { *title = "No Card"; *line = "Put a microSD card in the phone to see what is on it."; return 1; }
    if (MD.card == SH_CARD_ERROR) { *title = "Can't Read the Card"; *line = MD.why[0] ? MD.why : "The card did not answer."; return 1; }
    if (MD.app == MD_FILES) {
        if (MD.lerr) { *title = "Can't Read This Folder"; *line = "The card did not answer."; return 1; }
        if (MD.listing) { *title = "Reading\xe2\x80\xa6"; return 1; }
        if (!MD.nent) { *title = "Folder is Empty"; return 1; }
        return 0;
    }
    if (ML.state != 2) { *title = MD.app == MD_PHOTOS ? "Looking for Photos\xe2\x80\xa6" : "Looking for Music\xe2\x80\xa6"; return 1; }
    if (MD.app == MD_PHOTOS && !ML.nph) { *title = "No Photos"; *line = "There are no JPEG or PNG pictures on the card."; return 1; }
    if (MD.app == MD_MUSIC && !ML.ntr) { *title = "No Music"; *line = "There are no MP3, AAC, M4A, FLAC or WAV files on the card."; return 1; }
    return 0;
}
/* the content again after a change of state: the tiles' count, the words, the header */
static void med_layout(void)
{
    const char *title, *line;
    int words = med_words(&title, &line);
    MD.half = MD.app == MD_PHOTOS;
    if (MD.app == MD_PHOTOS && MD.vmode && !words) {
        /* Months / Years: the library's groups (each a card with its newest picture) */
        MD.ngrp = 0;
        for (int i = 0; i < ML.nph; i++) {
            uint32_t st = ML.ph[i].date;
            int key = MD.vmode == 1 ? (int)(st >> 25) * 12 + (int)(st >> 21 & 15) : (int)(st >> 25);
            if (!MD.ngrp || MDG[MD.ngrp - 1].key != key) {
                if (MD.ngrp == MD_GRP) break;
                MDG[MD.ngrp].first = i; MDG[MD.ngrp].n = 0; MDG[MD.ngrp].key = key; MD.ngrp++;
            }
            MDG[MD.ngrp - 1].n++;
        }
    }
    MD.tp = MD.half ? (MD.vmode ? (int)(PT(MD_CARD_H) / 2 + .5f) * 2 : 2 * (MD_CELL + 3)) : MD_RP;
    MD.nt = MD.half ? (MD.vmode ? 5 : 9) : MD_NT;
    MD.rows = words ? 0 : MD.app == MD_FILES ? MD.nent + 1 : MD.app == MD_MUSIC ? ML.ntr + 1 : MD.vmode ? MD.ngrp : (ML.nph + 2) / 3;
    MD.ntile = MD.rows;
    if (MD.app == MD_PHOTOS && MD.to_end && MD.rows) { MD.scroll = med_max(); MD.to_end = 0; }   /* iOS: at the newest */
    MD.scroll = tk_clampf(MD.scroll, 0, med_max());
    MD.momentum = 0; MD.sp.live = 0;
    med_header();
    relayer(&medmsg);
    medmsg.hidden = 1;
    if (words) {
        tk_layer *nx = medmsg.next;
        dc d = mk(&medmsg, "med-msg", 0, 322, 390, 112, 0, 0);
        medmsg.next = nx;
        d_empty(&d, 150, 780, title, line);
    }
    medh.hidden = MD.app == MD_PHOTOS;                 /* Photos: the header is the fade (medht), no band */
    med_tiles_reset();
    med_tiles();
    relink();
    MD.pend = 1;
}

/* ---- Files: a folder's entries ------------------------------------------------------------------------------- */
#define MD_ENT 600
static void files_open(const char *dir)
{
    md_cat(MD.dir, 0, sizeof MD.dir, dir);
    MD.nent = 0; MD.listing = 1; MD.lerr = 0; MD.more = 0; MD.lfirst = 0;
    MD.scroll = 0;
    if (!MD.ent) MD.ent = tk_alloc(sizeof(sh_dirent_t) * MD_ENT);
    if (!MD.ent) { MD.listing = 0; MD.lerr = 1; }
}
static void files_sort(void)
{
    /* by name, as iOS Files (a shell sort: no recursion, no second copy) */
    for (int gap = MD.nent / 2; gap > 0; gap /= 2)
        for (int i = gap; i < MD.nent; i++) {
            sh_dirent_t t = MD.ent[i];
            int k = i;
            for (; k >= gap && md_cmp(MD.ent[k - gap].name, t.name) > 0; k -= gap) MD.ent[k] = MD.ent[k - gap];
            MD.ent[k] = t;
        }
}
/* one page of the folder */
static void files_step(void)
{
    static sh_dirent_t pg[32];
    int n = sh_card_list(md_root() ? "/" : MD.dir, pg, 32, MD.lfirst);
    if (n < 0) { MD.listing = 0; MD.lerr = 1; med_layout(); return; }
    MD.lfirst += n;
    for (int i = 0; i < n; i++) {
        if (pg[i].name[0] == '.' || !md_cmp(pg[i].name, "System Volume Information")) continue;
        if (MD.nent >= MD_ENT) { MD.more = 1; n = 0; break; }
        MD.ent[MD.nent++] = pg[i];
    }
    if (n < 32) {
        MD.listing = 0;
        files_sort();
        med_layout();
    }
}

/* ---- the library's search ------------------------------------------------------------------------------------ */
static void ml_sort(void)
{
    /* B5: the oldest first, as iOS's grid (the newest at the bottom); the songs by name */
    for (int gap = ML.nph / 2; gap > 0; gap /= 2)
        for (int i = gap; i < ML.nph; i++) {
            ml_item t = ML.ph[i];
            int k = i;
            for (; k >= gap && (ML.ph[k - gap].date > t.date || (ML.ph[k - gap].date == t.date && md_cmp(ML.ph[k - gap].name, t.name) > 0)); k -= gap)
                ML.ph[k] = ML.ph[k - gap];
            ML.ph[k] = t;
        }
    for (int gap = ML.ntr / 2; gap > 0; gap /= 2)
        for (int i = gap; i < ML.ntr; i++) {
            ml_track t = ML.tr[i];
            int k = i;
            for (; k >= gap && md_cmp(ML.tr[k - gap].it.name, t.it.name) > 0; k -= gap) ML.tr[k] = ML.tr[k - gap];
            ML.tr[k] = t;
        }
}
static void ml_step(void)
{
    static sh_dirent_t pg[32];
    if (!ML.state) {
        if (!ML.ph) ML.ph = tk_alloc(sizeof(ml_item) * ML_PHOTOS);
        if (!ML.tr) ML.tr = tk_alloc(sizeof(ml_track) * ML_TRACKS);
        ML.ndirs = 1; ML.cur = 0; ML.first = 0; ML.nph = ML.ntr = 0;
        ML.dirs[0][0] = 0; ML.depth[0] = 0;
        ML.state = ML.ph && ML.tr ? 1 : 2;
        return;
    }
    if (ML.cur >= ML.ndirs) { ML.state = 2; ml_sort(); med_layout(); return; }
    const char *dir = ML.dirs[ML.cur];
    int n = sh_card_list(dir[0] ? dir : "/", pg, 32, ML.first), found = ML.nph;
    ML.first += n > 0 ? n : 0;
    for (int i = 0; i < n; i++) {
        const sh_dirent_t *e = &pg[i];
        if (e->name[0] == '.') continue;
        if (e->dir) {
            if (!md_cmp(e->name, "System Volume Information") || !md_cmp(e->name, "LOST.DIR") || !md_cmp(e->name, "$RECYCLE.BIN") ||
                !md_cmp(e->name, "Android") || ML.depth[ML.cur] >= 5 || ML.ndirs >= ML_DIRS) continue;
            if (!md_path(ML.dirs[ML.ndirs], sizeof ML.dirs[0], dir, e->name)) continue;
            ML.depth[ML.ndirs] = (uint8_t)(ML.depth[ML.cur] + 1);
            /* the camera's and the pictures' folders first */
            int at = ML.ndirs++;
            if (!ML.cur && (!md_cmp(e->name, "DCIM") || !md_cmp(e->name, "Pictures"))) {
                char t[200];
                int first = ML.cur + 1;
                tk_memcpy(t, ML.dirs[at], 200);
                for (int k = at; k > first; k--) { tk_memcpy(ML.dirs[k], ML.dirs[k - 1], 200); ML.depth[k] = ML.depth[k - 1]; }
                tk_memcpy(ML.dirs[first], t, 200);
                ML.depth[first] = 1;
            }
            continue;
        }
        int k = md_kind(e->name);
        if ((k != MK_PHOTO && k != MK_AUDIO) || tk_strlen(e->name) >= sizeof ML.ph[0].name) continue;
        ml_item *it = k == MK_PHOTO ? (ML.nph < ML_PHOTOS ? &ML.ph[ML.nph++] : 0) : (ML.ntr < ML_TRACKS ? &ML.tr[ML.ntr++].it : 0);
        if (!it) continue;
        if (k == MK_AUDIO) { tk_memset(&ML.tr[ML.ntr - 1], 0, sizeof(ml_track)); }
        it->date = e->date;
        it->size = e->size > 0xffffffffu ? 0xffffffffu : (uint32_t)e->size;
        it->dir = (uint8_t)ML.cur;
        it->meta = 0;
        md_cat(it->name, 0, sizeof it->name, e->name);
    }
    if (n < 32) { ML.cur++; ML.first = 0; }
    if (MD.app == MD_PHOTOS && found != ML.nph) { med_header(); relink(); }
}
static void ml_path(char *o, int cap, const ml_item *it) { md_path(o, cap, ML.dirs[it->dir], it->name); }

/* ---- a song's title and artist from its ID3v2 tag (the first 4 KiB) ------------------------------------------ */
static void id3_text(char *o, int cap, const uint8_t *p, uint32_t n)
{
    int k = 0, enc = n ? p[0] : 0;
    if (!n) return;
    p++; n--;
    if (enc == 1 || enc == 2) {
        /* UTF-16 (1: with a byte order mark; 2: big endian) to UTF-8, the BMP only */
        int be = enc == 2;
        if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) { be = 1; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) { be = 0; p += 2; n -= 2; }
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            unsigned c = be ? (unsigned)(p[i] << 8 | p[i + 1]) : (unsigned)(p[i] | p[i + 1] << 8);
            if (!c) break;
            if (c < 0x80) { if (k + 1 < cap) o[k++] = (char)c; }
            else if (c < 0x800) { if (k + 2 < cap) { o[k++] = (char)(0xc0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); } }
            else if (c < 0xd800 || c >= 0xe000) { if (k + 3 < cap) { o[k++] = (char)(0xe0 | c >> 12); o[k++] = (char)(0x80 | (c >> 6 & 63)); o[k++] = (char)(0x80 | (c & 63)); } }
        }
    } else {
        uint32_t i = 0;
        for (; i < n && p[i]; i++) {
            unsigned c = p[i];
            if (enc == 3 || c < 0x80) { if (k + 1 >= cap) break; o[k++] = (char)c; }      /* UTF-8 as it is */
            else { if (k + 2 >= cap) break; o[k++] = (char)(0xc0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); }   /* Latin-1 */
        }
        if (enc == 3 && i < n && (p[i] & 0xc0) == 0x80) {       /* cut inside a UTF-8 sequence: without its start */
            while (k > 0 && ((unsigned char)o[k - 1] & 0xc0) == 0x80) k--;
            if (k > 0) k--;
        }
    }
    o[k] = 0;
}
static void id3_read(ml_track *t)
{
    static _Alignas(64) uint8_t b[4096];
    char path[400];
    t->it.meta = 1;
    ml_path(path, sizeof path, &t->it);
    int n = sh_card_read(path, 0, b, sizeof b);
    if (n < 20 || b[0] != 'I' || b[1] != 'D' || b[2] != '3' || b[3] < 3 || b[3] > 4) return;
    uint32_t end = 10 + ((uint32_t)(b[6] & 127) << 21 | (uint32_t)(b[7] & 127) << 14 | (uint32_t)(b[8] & 127) << 7 | (uint32_t)(b[9] & 127));
    if (end > (uint32_t)n) end = (uint32_t)n;
    for (uint32_t o = 10; o + 10 <= end;) {
        const uint8_t *f = b + o;
        if (!f[0]) break;
        uint32_t len = b[3] == 4 ? (uint32_t)(f[4] & 127) << 21 | (uint32_t)(f[5] & 127) << 14 | (uint32_t)(f[6] & 127) << 7 | (uint32_t)(f[7] & 127)
                                 : (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
        if (len > end - o - 10) break;
        if (f[0] == 'T' && f[1] == 'I' && f[2] == 'T' && f[3] == '2') id3_text(t->title, sizeof t->title, f + 10, len);
        if (f[0] == 'T' && f[1] == 'P' && f[2] == 'E' && f[3] == '1') id3_text(t->artist, sizeof t->artist, f + 10, len);
        o += 10 + len;
    }
}

/* ---- the viewer ----------------------------------------------------------------------------------------------- */
static float md_ease(float t) { t = tk_clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static void vw_place(void)
{
    int on = MD.view >= 0 || MD.va > 0;
    float a = MD.va * (1 - tk_clampf(MD.vy / 420, 0, .7f)), ui = a * (MD.vui ? 1 : 0);
    medvbg.hidden = !on;
    medvbg.alpha = a;
    medvui.hidden = !on || !medvui.bmp.px || ui <= 0;
    medvui.alpha = ui;
    medvbot.hidden = !on || !medvbot.bmp.px || ui <= 0;
    medvbot.alpha = ui;
    medverr.hidden = !on || !MD.verr || !medverr.bmp.px;
    medverr.alpha = a;
    medvimg.hidden = !on || !medvimg.bmp.px;
    if (medvimg.bmp.px) {
        float bw = (float)medvimg.bmp.w, bh = (float)medvimg.bmp.h;
        float f = tk_minf((float)TK_W / bw, (float)TK_H / bh) * (1 - tk_clampf(MD.vy / 1000, 0, .35f)) * MD.vz;
        float X = ((float)TK_W - bw * f) / 2 + PT(MD.vx + MD.px), Y = ((float)TK_H - bh * f) / 2 + PT(MD.vy + MD.py);
        medvimg.sx = medvimg.sy = f;
        medvimg.ox = medvimg.oy = 0;
        medvimg.x = X / f;
        medvimg.y = Y / f;
        medvimg.alpha = MD.va;
        medvimg.gen++;
    }
    /* B5: the tile's thumbnail flying into place while the picture decodes (iOS: the tile zooms into the viewer) */
    medvth.hidden = !on || !medvth.bmp.px || (medvimg.bmp.px && MD.tha >= 1);
    if (!medvth.hidden) {
        float e = md_ease(MD.tha), w = MD.thw + (390 - MD.thw) * e, x = MD.thx * (1 - e) + MD.vx, y = MD.thy + ((H_PT - 390) / 2 - MD.thy) * e + MD.vy;
        float sc = PT(w) / (float)medvth.bmp.w;
        medvth.sx = medvth.sy = sc;
        medvth.ox = medvth.oy = 0;
        medvth.x = PT(x) / sc;
        medvth.y = PT(y) / sc;
        medvth.gen++;
    }
    float p = DJ.kind == 2 && MD.vfull != 1 ? dj_progress() : 0;
    medvbar.hidden = !on || DJ.kind != 2 || MD.vfull == 1;
    medvbar.solid = 0xffffffffu;
    medvbar.solid_w = tk_maxf(8, (float)TK_W * p);
    medvbar.solid_h = 6;
    medvbar.x = 0;
    medvbar.y = PT(98);
    medvbar.alpha = .8f * a;
    medvbar.gen++;
}
/* the pan's room at this zoom (pt each way) */
static void vw_room(float z, float *rx, float *ry)
{
    *rx = *ry = 0;
    if (!medvimg.bmp.px) return;
    float bw = (float)medvimg.bmp.w, bh = (float)medvimg.bmp.h, f = tk_minf((float)TK_W / bw, (float)TK_H / bh) * z;
    *rx = tk_maxf(0, (bw * f / TK_PT - 390) / 2);
    *ry = tk_maxf(0, (bh * f / TK_PT - H_PT) / 2);
}
/* a picture after (dir 1) or before (-1) this one (a file opened from Files stands alone) */
/* the photo entry of Files' folder after (dir 1) or before (-1) entry i, or -1 */
static int vw_files_next(int i, int dir)
{
    for (int k = i + dir; MD.ent && k >= 0 && k < MD.nent; k += dir) if (!MD.ent[k].dir && md_kind(MD.ent[k].name) == MK_PHOTO) return k;
    return -1;
}
static int vw_can(int dir)
{
    if (MD.vzt > 1.01f) return 0;                     /* zoomed in: a drag pans */
    if (MD.vsingle) return vw_files_next(MD.view, dir) >= 0;   /* B5: a picture from Files: its folder's others */
    return dir > 0 ? MD.view + 1 < ML.nph : MD.view > 0;
}
static void vw_bottom(void);
static void vw_show(int idx)
{
    char path[400], b[96], dt[24];
    const char *name;
    uint32_t date;
    uint64_t size;
    MD.view = idx;
    MD.vx = MD.vy = 0;
    MD.vanim = 0;
    MD.verr = 0;
    if (MD.vsingle) {
        if (!md_path(path, sizeof path, MD.dir, MD.ent[idx].name)) md_cat(path, 0, sizeof path, MD.vpath);
        const sh_dirent_t *e = &MD.ent[idx];
        name = e->name; date = e->date; size = e->size;
    } else {
        ml_path(path, sizeof path, &ML.ph[idx]);
        name = ML.ph[idx].name; date = ML.ph[idx].date; size = ML.ph[idx].size;
    }
    relayer(&medvimg);
    medvimg.hidden = 1;
    relayer(&medverr);
    relayer(&medvui);
    MD.vz = MD.vzt = 1; MD.px = MD.py = MD.pxt = MD.pyt = 0; MD.vfull = 0; MD.tap_wait = 0; MD.vsw = MD.vsh = 0;
    md_cat(MD.vpath, 0, sizeof MD.vpath, path);
    /* B5 (docs/design §8): a soft veil under the status band, the 44 glass back circle, the date 15 / 600 and the time
     * and place in the set 13 / 400, centred */
    dc d = mk(&medvui, "med-vui", 0, 0, 390, 104, 0, 0);
    for (int j = 0; j < d.b->h; j++) {
        float u = (float)j / (float)d.b->h, al = .55f * (1 - u * u);
        tk_fill_rect_raw(d.b, 0, j, d.b->w, 1, (uint32_t)(al * 255) << 24);
    }
    d_back(&d, W(1));
    md_date(dt, date);
    ellipsize(b, sizeof b, TK_IT600, 15, dt[0] ? dt : name, 230);
    d_text_box(&d, TK_IT600, 15, 195, bl(TK_IT600, 15, SAFE_TOP + 4), 230, 1, b, 0, 0, INK, 0);
    int n = 0;
    if (date) {
        int hh = (int)(date >> 11 & 31), mm = (int)(date >> 5 & 63);
        b[n++] = (char)('0' + hh / 10); b[n++] = (char)('0' + hh % 10); b[n++] = ':'; b[n++] = (char)('0' + mm / 10); b[n++] = (char)('0' + mm % 10);
        b[n] = 0;
    }
    if (!MD.vsingle) { if (n) n = md_cat(b, n, sizeof b, " \xc2\xb7 "); n += put_dec(b + n, idx + 1); n = md_cat(b, n, sizeof b, " of "); n += put_dec(b + n, ML.nph); }
    else if (!n) md_cat(b, 0, sizeof b, name);
    d_text_box(&d, TK_IT400, 13, 195, bl(TK_IT400, 13, SAFE_TOP + 24), 230, 1, b, 0, TK_TNUM, W(.75f), 0);
    vw_bottom();
    dj_begin(2, path, size, TK_W / 2, TK_H / 2);
    relink();
    vw_place();
    MD.pend = 1;
}
/* the bottom row (§8): share, heart, info and delete as 44 glass circles (the card is read only: info works) */
#define VB_Y (H_PT - 30 - 8 - 44)
static const float VBX[4] = { MARGIN, 128, 218, 390 - MARGIN - 44 };
static void vw_bottom(void)
{
    relayer(&medvbot);
    tk_layer *nx = medvbot.next;
    dc d = mk(&medvbot, "med-vbot", 0, VB_Y - 30, 390, H_PT - VB_Y + 30, 0, 0);
    medvbot.next = nx;
    if (!d.b->px) return;
    for (int j = 0; j < d.b->h; j++) {
        float u = (float)j / (float)d.b->h, al = .5f * u;
        tk_fill_rect_raw(d.b, 0, j, d.b->w, 1, (uint32_t)(al * 255) << 24);
    }
    for (int k = 0; k < 4; k++) {
        float cx = VBX[k] + 22, cy = VB_Y + 22;
        d_rrect(&d, VBX[k], VB_Y, 44, 44, 22, W(.2f));
        d_rrect(&d, VBX[k] + .5f, VB_Y + .5f, 43, 43, 21.5f, tk_hex(0x1c1f2a, .92f));
        tk_col c = W(k == 2 ? 1 : .45f);
        if (k == 0) {                                   /* share: a box, an arrow up out of it */
            d_seg(&d, cx - 7, cy - 2, cx - 7, cy + 8, 1.8f, c); d_seg(&d, cx + 7, cy - 2, cx + 7, cy + 8, 1.8f, c);
            d_seg(&d, cx - 7, cy + 8, cx + 7, cy + 8, 1.8f, c);
            d_seg(&d, cx, cy - 10, cx, cy + 3, 1.8f, c); d_seg(&d, cx - 4, cy - 6, cx, cy - 10, 1.8f, c); d_seg(&d, cx + 4, cy - 6, cx, cy - 10, 1.8f, c);
        } else if (k == 1) {                            /* heart */
            float lx = 0, ly = 0;                       /* the heart curve: x = 16 sin^3 t, y = 13 cos t - 5 cos 2t - ... */
            for (int q = 0; q <= 32; q++) {
                float a = 6.2831853f * (float)q / 32, sn = tk_sinf(a), x = 16 * sn * sn * sn;
                float y = 13 * tk_cosf(a) - 5 * tk_cosf(2 * a) - 2 * tk_cosf(3 * a) - tk_cosf(4 * a);
                float X = cx + x * .55f, Y = cy - 1 - y * .55f;
                if (q) d_seg(&d, lx, ly, X, Y, 1.8f, c);
                lx = X; ly = Y;
            }
        } else if (k == 2) {                            /* info */
            d_ring(&d, cx, cy, 9.5f, 1.8f, 1, c, 0);
            d_rrect(&d, cx - 1.1f, cy - 2, 2.2f, 7.5f, 1.1f, c); d_rrect(&d, cx - 1.3f, cy - 6.4f, 2.6f, 2.6f, 1.3f, c);
        } else {                                        /* delete: a bin */
            d_seg(&d, cx - 8, cy - 6, cx + 8, cy - 6, 1.8f, c); d_seg(&d, cx - 3, cy - 9, cx + 3, cy - 9, 1.8f, c);
            d_seg(&d, cx - 6, cy - 4, cx - 5, cy + 9, 1.8f, c); d_seg(&d, cx + 6, cy - 4, cx + 5, cy + 9, 1.8f, c);
            d_seg(&d, cx - 5, cy + 9, cx + 5, cy + 9, 1.8f, c);
        }
    }
    trim(&medvbot);
}
static void vw_open(int idx, int single)
{
    MD.vsingle = single;
    MD.va = single ? 1 : 0;                 /* from Files: black at once, and the list's tiles go (the heap) */
    MD.vat = 1;
    if (single) med_tiles_free();
    tk_layer_init(&medvbg, "med-vbg");
    medvbg.solid = 0xff000000u;
    medvbg.solid_w = TK_W;
    medvbg.solid_h = TK_H;
    medvbg.opaque = 1;
    tk_layer_init(&medvbar, "med-vbar");
    MD.vui = 1;
    relayer(&medvth); medvth.hidden = 1; MD.tha = 1;
    vw_show(idx);
    /* B5: from the grid: its thumbnail (if decoded) grows from the tile while the picture decodes */
    const uint16_t *t = single ? 0 : tc_get(idx);
    if (t && !MD.vmode) {
        tk_layer *nx = medvth.next;
        tk_layer_init(&medvth, "med-vth");
        medvth.next = nx;
        if (tk_bmp_new(&medvth.bmp, MD_CELL, MD_CELL)) {
            tc_blit(&medvth.bmp, 0, t);
            float cell = (float)(MD_CELL + 3) * 2 / TK_PT;
            MD.thw = (float)MD_CELL * 2 / TK_PT;
            MD.thx = (float)(idx % 3) * cell;
            MD.thy = md_top() + (float)(idx / 3) * cell - MD.scroll;
            MD.tha = 0;
        }
    }
    vw_place();
}
static void vw_free(void)
{
    if (DJ.kind == 2) dj_free();
    MD.pend = 1;                             /* the grid's thumbnails again */
    MD.view = -1;
    MD.va = MD.vat = 0;
    MD.vanim = 0;
    relayer(&medvimg); relayer(&medvui); relayer(&medverr); relayer(&medvbot); relayer(&medvth);
    medvimg.hidden = medvui.hidden = medverr.hidden = medvbg.hidden = medvbar.hidden = medvbot.hidden = medvth.hidden = 1;
    MD.vz = MD.vzt = 1; MD.px = MD.py = MD.pxt = MD.pyt = 0; MD.vfull = 0; MD.tap_wait = 0; MD.tha = 1;
}
static void vw_failed(const char *why)
{
    MD.verr = 1;
    relayer(&medverr);
    dc d = mk(&medverr, "med-verr", 0, 322, 390, 112, 0, 0);
    d_empty(&d, 0, H_PT, "Can't Show This Picture", why);
    relink();
    vw_place();
}

/* ---- Now Playing (no sound yet: the card with its controls dimmed) ------------------------------------------- */
static void np_place(void)
{
    int on = MD.play >= 0;
    medpbg.hidden = !on;
    medpbg.alpha = 1;                        /* the ground at once (the list's tiles are freed under it) */
    medpart.hidden = !on || !medpart.bmp.px;
    medpnav.hidden = !on || !medpnav.bmp.px;
    medpinfo.hidden = !on || !medpinfo.bmp.px;
    medpart.alpha = medpnav.alpha = medpinfo.alpha = MD.pa;
}
static void np_free(void)
{
    MD.play = -1;
    MD.pstage = 0;
    MD.pa = 0;
    relayer(&medpart); relayer(&medpnav); relayer(&medpinfo);
    np_place();
}
/* drawn over two frames after the tap, not in it */
static void np_build(void)
{
    const ml_track *t = &ML.tr[MD.play];
    char b[160];
    if (MD.pstage == 1) {
        tk_layer_init(&medpbg, "med-pbg");
        medpbg.solid = sbc_argb(SBC_GROUND);
        medpbg.solid_w = TK_W;
        medpbg.solid_h = TK_H;
        medpbg.opaque = 1;
        med_tiles_free();
        relayer(&medpnav);
        dc d = mk(&medpnav, "med-pnav", 0, 44, 390, 50, 0, 0);
        d_icon(&d, "back", 18, 53, 1);
        d_text(&d, TK_IT400, 17, 32, bl(TK_IT400, 17, 53), "Library", 0, 0, ACCENT(), 0);
        /* the artwork's place at half resolution (a flat square and the notes: nothing sharp to lose) */
        relayer(&medpart);
        tk_layer_init(&medpart, "med-part");
        int s = (int)PT(135);
        if (tk_bmp_new(&medpart.bmp, s, s)) {
            dc a = { &medpart.bmp, 0, 0, 0 };
            d_rrect(&a, 0, 0, 135, 135, 9, sbc(SBC_GRAY5));
            d_note(&a, 36, 34, 62, tk_hex(0xff375f, 1));
            medpart.sx = medpart.sy = 2;
            medpart.x = PT(60) / 2;
            medpart.y = PT(108) / 2;
        }
        MD.pstage = 2;
    } else {
        relayer(&medpinfo);
        dc d = mk(&medpinfo, "med-pinfo", 0, 396, 390, 300, 0, 0);
        ellipsize(b, sizeof b, TK_IT700, 22, t->title[0] ? t->title : t->it.name, 326);
        d_text(&d, TK_IT700, 22, 32, bl(TK_IT700, 22, 404), b, 0, 0, INK, 0);
        ellipsize(b, sizeof b, TK_IT400, 17, t->artist[0] ? t->artist : "Unknown artist", 326);
        d_text(&d, TK_IT400, 17, 32, bl(TK_IT400, 17, 434), b, 0, 0, C_LABEL2, 0);
        /* the scrubber at its start, the times unknown; the transport, dimmed: nothing here can be pressed */
        d_rrect(&d, 32, 482, 326, 6, 3, W(.14f));
        d_text(&d, TK_IT400, 12, 32, bl(TK_IT400, 12, 496), "0:00", 0, TK_TNUM, C_LABEL3, 0);
        float w = tw(TK_IT400, 12, "--:--", 0, TK_TNUM);
        d_text(&d, TK_IT400, 12, 358 - w, bl(TK_IT400, 12, 496), "--:--", 0, TK_TNUM, C_LABEL3, 0);
        tk_col dim = W(.22f);
        d_tri(&d, 178, 548, 178, 592, 216, 570, dim);                      /* play */
        d_tri(&d, 112, 570, 134, 556, 134, 584, dim);                      /* previous */
        d_tri(&d, 90, 570, 112, 556, 112, 584, dim);
        d_tri(&d, 278, 570, 256, 556, 256, 584, dim);                      /* next */
        d_tri(&d, 300, 570, 278, 556, 278, 584, dim);
        w = tw(TK_IT600, 15, "Playback isn't available yet", 0, 0);
        d_text(&d, TK_IT600, 15, 195 - w / 2, bl(TK_IT600, 15, 632), "Playback isn't available yet", 0, 0, W(.7f), 0);
        const char *l2 = "Starbite OS has no sound output so far.";
        w = tw(TK_IT400, 13, l2, 0, 0);
        d_text(&d, TK_IT400, 13, 195 - w / 2, bl(TK_IT400, 13, 656), l2, 0, 0, C_LABEL2, 0);
        MD.pstage = 3;
    }
    relink();
    np_place();
}

/* ---- the work, a step at a time ------------------------------------------------------------------------------- */
/* the next thumbnail a tile in view still needs: 1 if a job was started */
static int thumb_next(void)
{
    if (MD.app != MD_PHOTOS || ML.state != 2 || MD.view >= 0) return 0;
    int best = -1, bs = 0, bc = 0;
    for (int s = 0; s < MD_NT; s++) {
        if (MD.tidx[s] < 0 || medt[s].hidden) continue;
        if (MD.vmode) {                                 /* a card: its group's newest picture */
            int g = MD.tidx[s];
            if (g < MD.ngrp && !MD.tcell[s][0] && (best < 0 || g < best)) { best = g; bs = s; bc = 0; }
            continue;
        }
        for (int c = 0; c < 3; c++) {
            int ph = MD.tidx[s] * 3 + c;
            if (ph < ML.nph && !MD.tcell[s][c] && (best < 0 || ph < best)) { best = ph; bs = s; bc = c; }
        }
    }
    if (best < 0) return 0;
    char path[400];
    int ph = MD.vmode ? MDG[best].first + MDG[best].n - 1 : best;
    ml_path(path, sizeof path, &ML.ph[ph]);
    if (MD.vmode) dj_begin(1, path, ML.ph[ph].size, (int)(PT(390 - 32) / 2), (int)(PT(MD_CARD_H - 14) / 2));
    else dj_begin(1, path, ML.ph[ph].size, MD_CELL, MD_CELL);
    DJ.photo = ph; DJ.slot = bs; DJ.cell = bc; DJ.tile = MD.tidx[bs];
    return 1;
}
/* a card's picture into its tile, its corners rounded into the ground, the words over a shade */
static void card_done(int ok)
{
    int s = DJ.slot;
    tk_bmp *b = &medt[s].bmp;
    int x0 = (int)(PT(16) / 2), cw = (int)(PT(390 - 32) / 2), ch = (int)(PT(MD_CARD_H - 14) / 2);
    if (ok) {
        tk_bmp t = DJ.out;
        int ox = (t.w < cw) ? (cw - t.w) / 2 : 0, oy = (t.h < ch) ? (ch - t.h) / 2 : 0;
        tk_bmp_copy(b, x0 + ox, oy, &t, 0, 0, tk_mini(t.w, cw), tk_mini(t.h, ch));
    }
    /* the shade under the words: the bottom third darkened */
    for (int y = ch * 2 / 3; y < ch; y++) {
        float a = (float)(y - ch * 2 / 3) / (float)(ch / 3) * .55f;
        for (int x = x0; x < x0 + cw; x++) {
            uint32_t c = b->px[(size_t)y * b->stride + x];
            uint32_t r = (uint32_t)((float)(c >> 16 & 255) * (1 - a)), g = (uint32_t)((float)(c >> 8 & 255) * (1 - a)), bb = (uint32_t)((float)(c & 255) * (1 - a));
            b->px[(size_t)y * b->stride + x] = 0xff000000u | r << 16 | g << 8 | bb;
        }
    }
    /* the corners (r 22) back into the ground */
    tk_r4 k = { PT(22) / 2, PT(22) / 2, PT(22) / 2, PT(22) / 2 };
    int rr = (int)(PT(22) / 2) + 2;
    for (int y = 0; y < ch; y++) {
        if (y >= rr && y < ch - rr) continue;
        for (int x = x0; x < x0 + cw; x++) {
            if (x >= x0 + rr && x < x0 + cw - rr) continue;
            float cov = tk_clampf(.5f - tk_sd_rrect4((float)x + .5f, (float)y + .5f, (float)x0, 0, (float)cw, (float)ch, &k), 0, 1);
            uint32_t c = b->px[(size_t)y * b->stride + x], G = 0xff0a0f1fu;
            uint32_t r = (uint32_t)((float)(c >> 16 & 255) * cov + (float)(G >> 16 & 255) * (1 - cov));
            uint32_t g = (uint32_t)((float)(c >> 8 & 255) * cov + (float)(G >> 8 & 255) * (1 - cov));
            uint32_t bb = (uint32_t)((float)(c & 255) * cov + (float)(G & 255) * (1 - cov));
            b->px[(size_t)y * b->stride + x] = 0xff000000u | r << 16 | g << 8 | bb;
        }
    }
    /* "November 2026" (or "2026") 22 / 700 and the count, in white (at the tile's half resolution) */
    const struct { int first, n, key; } *gr = (const void *)&MDG[DJ.tile];
    char t1[32], t2[32];
    int n = 0;
    if (MD.vmode == 1) { const char *m = MO[(gr->key % 12 + 11) % 12]; n = md_cat(t1, 0, sizeof t1, m); t1[n++] = ' '; n += put_dec(t1 + n, gr->key / 12 + 1980); t1[n] = 0; }
    else put_dec(t1, gr->key + 1980);
    md_count(t2, gr->n, "photo", "photos");
    tk_paint wp = tk_solid(W(1)), gp = tk_solid(W(.8f));
    tk_text(b, TK_IT700, PT(22) / 2, (float)x0 + PT(16) / 2, (float)ch - PT(34) / 2, t1, 0, 0, &wp, 0);
    tk_text(b, TK_IT400, PT(15) / 2, (float)x0 + PT(16) / 2, (float)ch - PT(13) / 2, t2, 0, TK_TNUM, &gp, 0);
    MD.tcell[s][0] = ok ? 1 : 2;
    tk_layer_dirty(&medt[s]);
}
static void thumb_done(int ok)
{
    int s = DJ.slot, c = DJ.cell;
    if (MD.app == MD_PHOTOS && MD.vmode && MD.tidx[s] == DJ.tile && medt[s].bmp.px) { card_done(ok); dj_free(); return; }
    if (MD.app == MD_PHOTOS && MD.tidx[s] == DJ.tile && medt[s].bmp.px) {
        tk_bmp *b = &medt[s].bmp;
        int x = c * (MD_CELL + 3);
        if (ok && DJ.out.w == MD_CELL) tk_bmp_copy(b, x, 0, &DJ.out, 0, 0, MD_CELL, MD_CELL);
        else if (ok) {
            /* a small picture: up to the cell */
            tk_bmp t;
            if (tk_bmp_new(&t, MD_CELL, MD_CELL)) {
                tk_bmp_up(&t, &DJ.out, 0, 0, (float)MD_CELL / (float)DJ.out.w);
                tk_bmp_copy(b, x, 0, &t, 0, 0, MD_CELL, MD_CELL);
                tk_bmp_free(&t);
            }
        } else {
            /* a picture that cannot be shown: a plain frame (the viewer says why) */
            tk_fill_rect_raw(b, x, 0, MD_CELL, MD_CELL, sbc_argb(SBC_GRAY6));
            tk_fill_rect_raw(b, x + 89, 94, 60, 50, sbc_argb(SBC_GRAY4));
            tk_fill_rect_raw(b, x + 92, 97, 54, 44, sbc_argb(SBC_GRAY6));
        }
        MD.tcell[s][c] = ok ? 1 : 2;
        if (ok && !MD.vmode) tc_put(DJ.photo, b, x);   /* B5: kept: scrolling back never decodes it again */
        tk_layer_dirty(&medt[s]);
    }
    dj_free();
}
/* one step. 0: nothing left; 1: did something; 2: the next step is a slow one and slow is 0 */
static int med_step(int slow)
{
    if (!MD.app || !AP.on || AP.closing) return 0;
    if (MD.card == SH_CARD_MOUNTING) {
        if (!slow) return 2;
        sh_card_mount();
        med_card();
        if (MD.card == SH_CARD_MOUNTING) { MD.card = SH_CARD_ERROR; md_cat(MD.why, 0, sizeof MD.why, "The card did not answer."); }
        med_layout();
        return 1;
    }
    if (MD.card != SH_CARD_READY) return 0;
    if (MD.app == MD_FILES && MD.listing) { if (!slow) return 2; files_step(); return 1; }
    if (MD.app != MD_FILES && ML.state != 2) { if (!slow) return 2; ml_step(); return 1; }
    if (DJ.kind || thumb_next()) {
        int rc = dj_step();
        if (DJ.kind == 2) {
            if (rc > 0) {
                relayer(&medvimg);
                tk_layer *nx = medvimg.next;
                tk_layer_init(&medvimg, "med-vimg");
                medvimg.next = nx;
                if (MD.vfull == 1) MD.vfull = 2;       /* B5: the zoomed picture at the panel's resolution now */
                medvimg.bmp = DJ.out;
                tk_memset(&DJ.out, 0, sizeof DJ.out);
                dj_free();
                relink();
            } else if (rc < 0 && MD.vfull == 1) {
                dj_free();                              /* no full resolution (the heap): the half one stays */
                MD.vfull = 2;
            } else if (rc < 0) {
                const char *why = DJ.err;
                dj_free();
                vw_failed(why);
            }
            vw_place();
        } else if (rc) thumb_done(rc > 0);
        return 1;
    }
    if (MD.app == MD_MUSIC && ML.state == 2 && MD.play < 0)
        for (int s = 0; s < MD_NT; s++) {
            if (MD.tidx[s] < 0 || medt[s].hidden) continue;
            int r = MD.tidx[s];
            if (r < ML.ntr && !ML.tr[r].it.meta) {
                id3_read(&ML.tr[r]);
                if (ML.tr[r].title[0] || ML.tr[r].artist[0]) { MD.tidx[s] = -1; med_tiles(); }
                return 1;
            }
        }
    return 0;
}
/* steps for about us microseconds; 1 if anything was done (a frame shows it) */
static int med_work(uint32_t us, int slow)
{
    if (!MD.pend) return 0;
    uint64_t t0 = tk_us();
    int did = 0;
    for (;;) {
        int r = med_step(slow);
        if (!r) { MD.pend = 0; break; }
        if (r == 2) break;
        did = 1;
        if (tk_us() - t0 >= us) break;
    }
    if (did && !sh_moving()) for (int s = 0; s < MD_NT; s++) if (!medt[s].hidden) tk_layer_prepare(&medt[s]);
    return did;
}
static int med_pending(void) { return MD.app && AP.on && !AP.closing && MD.pend; }

/* ---- open, close, the page's layers --------------------------------------------------------------------------- */
static void med_build(void)
{
    const char *k = APPS[AP.a].k;
    int app = !tk_strcmp(k, "files") ? MD_FILES : !tk_strcmp(k, "photos") ? MD_PHOTOS : MD_MUSIC;
    if (MD.app != app) {
        MD.app = app;
        MD.vmode = 0; MD.to_end = app == MD_PHOTOS;      /* B5: Photos opens at the newest */
        tk_layer_init(&medtg, "med-tiles");
        tk_layer_init(&medseg, "med-seg"); medseg.hidden = 1;
        tk_layer_init(&medvbot, "med-vbot"); medvbot.hidden = 1;
        tk_layer_init(&medvth, "med-vth"); medvth.hidden = 1;
        MD.hdr_px = (int)(PT(MD_HDR) + .5f);
        MD.scroll = 0;
        MD.view = -1; MD.play = -1; MD.pstage = 0; MD.va = MD.vat = 0; MD.pa = 0;
        MD.nt = MD_NT;
        for (int s = 0; s < MD_NT; s++) { tk_layer_init(&medt[s], "med-tile"); medt[s].hidden = 1; }
        tk_layer_init(&medh, "med-head");
        medh.solid = sbc_argb(SBC_GROUND);
        medh.solid_w = TK_W;
        medh.solid_h = (float)MD.hdr_px;
        medh.opaque = 1;
        tk_layer_init(&medvbg, "med-vbg"); tk_layer_init(&medvbar, "med-vbar"); tk_layer_init(&medpbg, "med-pbg");
        medvbg.hidden = medvbar.hidden = medpbg.hidden = 1;
        if (app == MD_FILES) files_open("/");
    }
    med_card();
    med_layout();
}
static void med_close(void)
{
    if (!MD.app) return;
    if (DJ.kind) dj_free();
    vw_free();
    np_free();
    for (int s = 0; s < MD_NT; s++) { relayer(&medt[s]); medt[s].hidden = 1; }
    relayer(&medht); relayer(&medmsg); relayer(&medseg);
    medh.hidden = medht.hidden = medmsg.hidden = medseg.hidden = 1;
    tc_free();
    tk_free(MD.ent);
    MD.ent = 0;
    MD.nent = 0;
    MD.app = 0;
    MD.pend = 0;
    MD.down = MD.momentum = 0;
    MD.sp.live = 0;
}
/* the layers under the app's content group, bottom to top (relink) */
static int med_layers(tk_layer **o)
{
    int n = 0;
    tk_layer *t[MD_NT];
    for (int s = 0; s < MD_NT; s++) t[s] = &medt[s];
    link(&medtg, t, MD_NT);                          /* B5: the tiles in a clipped group */
    o[n++] = &medtg;
    o[n++] = &medh; o[n++] = &medht; o[n++] = &medmsg; o[n++] = &medseg;
    o[n++] = &medpbg; o[n++] = &medpart; o[n++] = &medpnav; o[n++] = &medpinfo;
    o[n++] = &medvbg; o[n++] = &medvth; o[n++] = &medvimg; o[n++] = &medverr; o[n++] = &medvui; o[n++] = &medvbot; o[n++] = &medvbar;
    return n;
}

/* ---- the finger ----------------------------------------------------------------------------------------------- */
/* B5: the viewer's tap, once it is known not to be the first of a double tap: the bar's buttons, else the chrome
 * shown or hidden (iOS) */
static void vw_tap1(float x, float y)
{
    if (MD.vui && x < MARGIN + 54 && y >= SAFE_TOP - 4 && y < SAFE_TOP + 48) { MD.vat = 0; MD.vanim = 3; MD.redo = MD.vsingle; return; }
    if (MD.vui && y >= VB_Y - 6 && y < VB_Y + 50) {
        for (int k = 0; k < 4; k++)
            if (x >= VBX[k] - 6 && x < VBX[k] + 50) {
                if (k == 2) {
                    char b[96], sz[24];
                    int n = 0;
                    if (MD.vsw) { n = put_dec(b, MD.vsw); n = md_cat(b, n, sizeof b, " \xc3\x97 "); n += put_dec(b + n, MD.vsh); n = md_cat(b, n, sizeof b, " \xc2\xb7 "); }
                    uint64_t size = MD.vsingle ? MD.ent[MD.view].size : ML.ph[MD.view].size;
                    md_size(sz, size);
                    md_cat(b, n, sizeof b, sz);
                    toast(b);
                } else toast(k == 0 ? "Sharing isn't available yet" : k == 1 ? "The card is read only: no Favourites yet" : "The card is read only: nothing is deleted");
                return;
            }
        return;
    }
    MD.vui = !MD.vui;
    vw_place();
}
/* a double tap: in to 2.5x round the finger (the picture again at the panel's resolution), or back out */
static void vw_tap2(float x, float y)
{
    if (MD.vzt > 1.01f) { MD.vzt = 1; MD.pxt = MD.pyt = 0; return; }
    float z = 2.5f, rx, ry;
    MD.vzt = z;
    vw_room(z, &rx, &ry);
    MD.pxt = tk_clampf((195 - x) * (z - 1), -rx, rx);
    MD.pyt = tk_clampf((H_PT / 2 - y) * (z - 1), -ry, ry);
    MD.vui = 0;
}
static void med_ptap(float x, float y)
{
    /* Photos' header and capsule */
    if (y < SAFE_TOP + 48 && x >= CONTENT_R - 90) { toast("Selecting photos isn't available on this read-only card yet"); return; }
    if (y >= SEG_Y - 6 && y < SEG_Y + 46 && tk_fabsf(x - 195) < SEG_W / 2) {
        int k = tk_mini(2, tk_maxi(0, (int)((x - (195 - SEG_W / 2)) / (SEG_W / 3)))), mode = k == 2 ? 0 : k == 1 ? 1 : 2;
        if (mode != MD.vmode) { sh_haptic(); MD.vmode = mode; MD.to_end = 1; if (DJ.kind == 1) dj_free(); med_tiles_reset(); med_layout(); }
        return;
    }
}
static void med_tap(float x, float y)
{
    int back = x < 150 && y >= 40 && y < 92;
    if (MD.view >= 0) {
        /* a tap waits 280 ms for a second one (a double tap zooms) */
        if (MD.tap_wait && now_ms - MD.tap_t < 280 && tk_fabsf(x - MD.tap_x) < 40 && tk_fabsf(y - MD.tap_y) < 40) { MD.tap_wait = 0; vw_tap2(x, y); return; }
        if (MD.vui && (y < SAFE_TOP + 48 || y >= VB_Y - 6)) { MD.tap_wait = 0; vw_tap1(x, y); return; }   /* buttons at once */
        MD.tap_wait = 1; MD.tap_t = now_ms; MD.tap_x = x; MD.tap_y = y;
        return;
    }
    if (MD.play >= 0) { if (back && MD.pstage == 3) MD.pstage = 4; return; }   /* the list again in the next frame (med_tick) */
    if (MD.app == MD_PHOTOS && (y < SAFE_TOP + 48 || y >= SEG_Y - 6)) { med_ptap(x, y); return; }
    if (y < md_top() && MD.app != MD_PHOTOS) {
        if (back && MD.app == MD_FILES && !md_root()) {
            char par[256];
            int n = md_cat(par, 0, sizeof par, MD.dir);
            while (n > 0 && par[n - 1] != '/') n--;
            par[n > 1 ? n - 1 : 0] = 0;
            files_open(par[0] ? par : "/");
            med_layout();
        }
        return;
    }
    if (!MD.rows) return;
    int cp = (int)(PT(y - md_top() + MD.scroll));
    if (cp < 0) return;
    if (MD.app == MD_PHOTOS && MD.vmode) {
        /* a card: Months goes to All at its month, Years to Months at its year (iOS) */
        int g = cp / MD.tp;
        if (g >= MD.ngrp) return;
        int first = MDG[g].first, mode = MD.vmode - 1;
        MD.vmode = mode;
        med_tiles_reset();
        med_layout();
        if (!mode) MD.scroll = tk_clampf((float)(first / 3) * (float)MD.tp / TK_PT - 8, 0, med_max());
        else for (int k = 0; k < MD.ngrp; k++) if (MDG[k].first >= first) { MD.scroll = tk_clampf((float)k * (float)MD.tp / TK_PT, 0, med_max()); break; }
        med_tiles();
        relink();
        return;
    }
    if (MD.app == MD_PHOTOS) {
        int idx = cp / MD.tp * 3 + tk_mini(2, (int)(x / 130));
        if (idx < ML.nph) vw_open(idx, 0);
        return;
    }
    int r = cp / MD_RP;
    if (MD.app == MD_MUSIC) {
        if (r < ML.ntr) { MD.play = r; MD.pstage = 1; MD.pa = 0; }
        return;
    }
    if (r >= MD.nent) return;
    const sh_dirent_t *e = &MD.ent[r];
    char path[400];
    if (!md_path(path, e->dir ? (int)sizeof MD.dir : (int)sizeof path, MD.dir, e->name)) { toast("This name is too long to open"); return; }
    if (e->dir) { files_open(path); med_layout(); }
    else if (md_kind(e->name) == MK_PHOTO) { md_cat(MD.vpath, 0, sizeof MD.vpath, path); vw_open(r, 1); }
    else toast("This file can't be opened yet");
}
static void med_touch(int ph, float x, float y, float t)
{
    if (ph == 0) {
        MD.x0 = x; MD.y0 = y;
        MD.moved = 0; MD.axis = 0; MD.down = 1;
        MD.zone = MD.view >= 0 ? 2 : MD.play >= 0 ? 3 : MD.app == MD_PHOTOS ? ((y >= SEG_Y - 6 && tk_fabsf(x - 195) < SEG_W / 2) || (y < SAFE_TOP + 48 && x >= CONTENT_R - 90) ? 0 : 1) :
                  y < MD_HDR ? 0 : 1;
        MD.momentum = 0; MD.sp.live = 0;
        MD.base = MD.scroll;
        if (MD.zone == 2 && MD.vanim != 3) MD.vanim = 0;
        MD.bx = MD.vzt > 1.01f ? MD.px : MD.vx; MD.by = MD.vzt > 1.01f ? MD.py : MD.vy;
        tk_tracker_reset(&MD.trx); tk_tracker_reset(&MD.try_);
        tk_tracker_add(&MD.trx, x, t); tk_tracker_add(&MD.try_, y, t);
        return;
    }
    float dx = x - MD.x0, dy = y - MD.y0;
    if (ph == 1) {
        tk_tracker_add(&MD.trx, x, t); tk_tracker_add(&MD.try_, y, t);
        if (!MD.moved && (tk_fabsf(dx) > 6 || tk_fabsf(dy) > 6)) {
            MD.moved = 1;
            MD.axis = tk_fabsf(dx) > tk_fabsf(dy) ? 1 : 2;
            if (MD.zone == 2 && MD.vsingle && MD.axis == 2) MD.redo = 1;      /* the list shows under a pull down */
        }
        if (!MD.moved) return;
        if (MD.zone == 1 && MD.rows) {
            float s = MD.base - dy, m = med_max();
            if (s < 0) s = -md_rubber(-s, H_PT - md_top());
            else if (s > m) s = m + md_rubber(s - m, H_PT - md_top());
            MD.scroll = s;
            med_tiles();
        } else if (MD.zone == 2 && MD.vanim != 3 && MD.vzt > 1.01f) {
            float rx, ry, nx = MD.bx + dx, ny = MD.by + dy;
            vw_room(MD.vz, &rx, &ry);
            MD.px = MD.pxt = nx > rx ? rx + md_rubber(nx - rx, 390) : nx < -rx ? -rx - md_rubber(-rx - nx, 390) : nx;
            MD.py = MD.pyt = ny > ry ? ry + md_rubber(ny - ry, H_PT) : ny < -ry ? -ry - md_rubber(-ry - ny, H_PT) : ny;
            vw_place();
        } else if (MD.zone == 2 && MD.vanim != 3) {
            if (MD.axis == 1) {
                int can = vw_can(dx < 0 ? 1 : -1);
                MD.vx = MD.bx + (can ? dx : dx * .3f);
            } else
                MD.vy = MD.by + (dy > 0 ? dy : dy * .2f);
            vw_place();
        }
        return;
    }
    MD.down = 0;
    if (!MD.moved) { med_tap(x, y); return; }
    if (MD.zone == 1 && MD.rows) {
        float v = -tk_tracker_v(&MD.try_), m = med_max();
        if (MD.scroll < 0 || MD.scroll > m) tk_spring_start(&MD.sp, MD.scroll, MD.scroll < 0 ? 0 : m, v * 1000, TK_SPR_ROWS);
        else if (tk_fabsf(v) > .05f) { MD.v = v; MD.momentum = 1; }
    } else if (MD.zone == 2 && MD.vanim != 3 && MD.vzt > 1.01f) {
        float rx, ry;                                    /* the pan back inside the picture */
        vw_room(MD.vz, &rx, &ry);
        MD.pxt = tk_clampf(MD.px, -rx, rx);
        MD.pyt = tk_clampf(MD.py, -ry, ry);
    } else if (MD.zone == 2 && MD.vanim != 3) {
        float vx = tk_tracker_v(&MD.trx), vy = tk_tracker_v(&MD.try_);
        MD.vanim = 2;
        if (MD.axis == 1) {
            if ((MD.vx < -80 || vx < -.4f) && vw_can(1)) { MD.vanim = 1; MD.vdir = 1; }
            else if ((MD.vx > 80 || vx > .4f) && vw_can(-1)) { MD.vanim = 1; MD.vdir = -1; }
        } else if (MD.vy > 100 || vy > .5f) { MD.vanim = 3; MD.vat = 0; }
    }
}
static int med_moving(void)
{
    return MD.app && ((MD.down && MD.moved) || MD.redo || MD.momentum || MD.sp.live || MD.vanim || MD.va != MD.vat ||
                      (MD.play >= 0 && (MD.pstage != 3 || MD.pa < 1)) ||
                      (MD.view >= 0 && (MD.vz != MD.vzt || MD.px != MD.pxt || MD.py != MD.pyt || MD.tap_wait || MD.tha < 1)));
}
/* a frame's worth of its motion, and a little of the work between a gesture's frames */
static int med_tick(float dt)
{
    if (!MD.app) return 0;
    int mv = 0;
    if (MD.redo) { MD.redo = 0; med_tiles(); mv = 1; }
    if (MD.sp.live) {
        tk_spring_step(&MD.sp, dt);
        MD.scroll = MD.sp.x;
        med_tiles();
        mv = 1;
    } else if (MD.momentum) {
        float ms = dt * 1000, m = med_max(), decay = tk_powf(.998f, ms);
        MD.scroll += MD.v * (1 - decay) / (1 - .998f);
        MD.v *= decay;
        if (MD.scroll < 0 || MD.scroll > m) {
            MD.momentum = 0;
            tk_spring_start(&MD.sp, MD.scroll, MD.scroll < 0 ? 0 : m, MD.v * 1000, TK_SPR_ROWS);
        } else if (tk_fabsf(MD.v) < .01f) MD.momentum = 0;
        med_tiles();
        mv = 1;
    }
    if (MD.va != MD.vat) {
        MD.va = MD.vat > MD.va ? tk_minf(MD.vat, MD.va + dt / .2f) : tk_maxf(MD.vat, MD.va - dt / .2f);
        mv = 1;
        if (MD.va <= 0 && MD.vat <= 0) vw_free(); else vw_place();
    } else if (MD.vanim == 3) vw_free();
    if (MD.view >= 0) {
        /* B5: the zoom and the pan ease to their targets; a lone tap after its 280 ms; the thumbnail's flight */
        if (MD.vz != MD.vzt || MD.px != MD.pxt || MD.py != MD.pyt) {
            float k = tk_minf(1, dt * 13);
            MD.vz += (MD.vzt - MD.vz) * k; MD.px += (MD.pxt - MD.px) * k; MD.py += (MD.pyt - MD.py) * k;
            if (tk_fabsf(MD.vz - MD.vzt) < .002f && tk_fabsf(MD.px - MD.pxt) < .2f && tk_fabsf(MD.py - MD.pyt) < .2f) { MD.vz = MD.vzt; MD.px = MD.pxt; MD.py = MD.pyt; }
            vw_place();
            mv = 1;
        }
        if (MD.tap_wait && now_ms - MD.tap_t >= 280) { MD.tap_wait = 0; vw_tap1(MD.tap_x, MD.tap_y); mv = 1; }
        if (MD.tha < 1) { MD.tha = tk_minf(1, MD.tha + dt / .32f); vw_place(); mv = 1; }
        else if (!medvth.hidden && medvimg.bmp.px) vw_place();
        /* zoomed in at rest: the picture again at the panel's resolution (one decode, idle steps) */
        if (MD.vzt > 1.01f && MD.vz == MD.vzt && !MD.vfull && !DJ.kind && medvimg.bmp.px && !MD.verr) {
            uint64_t size = MD.vsingle ? MD.ent[MD.view].size : ML.ph[MD.view].size;
            dj_begin(2, MD.vpath, size, TK_W, TK_H);
            MD.vfull = 1;
            MD.pend = 1;
        }
    }
    if (MD.vanim == 1) {
        MD.vx -= (float)MD.vdir * 390 * dt / .16f;
        mv = 1;
        if (tk_fabsf(MD.vx) >= 400) vw_show(MD.vsingle ? vw_files_next(MD.view, MD.vdir) : MD.view + MD.vdir); else vw_place();
    } else if (MD.vanim == 2) {
        float k = tk_minf(1, dt * 16);
        MD.vx -= MD.vx * k;
        MD.vy -= MD.vy * k;
        if (tk_fabsf(MD.vx) < .3f && tk_fabsf(MD.vy) < .3f) { MD.vx = MD.vy = 0; MD.vanim = 0; }
        vw_place();
        mv = 1;
    }
    if (MD.play >= 0) {
        if (MD.pstage && MD.pstage < 3) { np_build(); mv = 1; }
        else if (MD.pstage == 4) { med_tiles(); np_free(); mv = 1; }
        else if (MD.pa < 1) { MD.pa = tk_minf(1, MD.pa + dt / .2f); np_place(); mv = 1; }
    }
    med_work(2500, 0);
    return mv;
}
/* the host's stills: "mwait" all the work now; "mdir" a folder; "mview" / "mplay" an item; "mscroll" pt */
static int med_set(const char *what, const char *key, float v)
{
    if (!MD.app) return 0;
    if (!tk_strcmp(what, "mwait")) {
        for (int i = 0; i < 200000 && MD.pend; i++) med_work(1000000, 1);
    } else if (!tk_strcmp(what, "mdir") && MD.app == MD_FILES) {
        files_open(key);
        med_layout();
    } else if (!tk_strcmp(what, "mview") && MD.app == MD_PHOTOS && (int)v < ML.nph) {
        vw_open((int)v, 0);
        MD.va = 1;
        vw_place();
    } else if (!tk_strcmp(what, "mplay") && MD.app == MD_MUSIC && (int)v < ML.ntr) {
        MD.play = (int)v; MD.pstage = 1;
        np_build(); np_build();
        MD.pa = 1;
        np_place();
    } else if (!tk_strcmp(what, "mmode") && MD.app == MD_PHOTOS) {     /* B5: 0 All, 1 Months, 2 Years */
        MD.vmode = (int)v; MD.to_end = 1;
        med_tiles_reset();
        med_layout();
        MD.pend = 1;
    } else if (!tk_strcmp(what, "mzoom") && MD.view >= 0) {           /* B5: a double tap at (195, v) */
        vw_tap2(195, v);
        MD.vz = MD.vzt; MD.px = MD.pxt; MD.py = MD.pyt;
        vw_place();
    } else if (!tk_strcmp(what, "mscroll")) {
        MD.scroll = tk_clampf(v, 0, med_max());
        med_tiles();
        MD.pend = 1;
    } else
        return 0;
    return 1;
}
