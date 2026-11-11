/* SPDX-License-Identifier: MIT
 * Pictures for Starbite OS Next's Photos (included by shell_media.inc; freestanding, the toolkit's own allocator).
 * Both decoders are our own, written for a small heap and a slow card:
 *   - mj: baseline JPEG (Huffman, 8 bit, grey or YCbCr, any sampling up to 4, restart intervals), decoded one MCU
 *     row per step from a sliding window over the file, at 1/1, 1/2, 1/4 or 1/8 size (1/8: the DC terms alone), so
 *     a 12 MP photo never exists in RAM at full size. Progressive and arithmetic JPEGs are refused (said so).
 *   - mp: PNG (8 / 16 bit and the packed depths, grey / RGB / palette, alpha over black; not interlaced), from the
 *     whole file in RAM: an inflate that stops between symbols, so a step is a few rows.
 *   - mr: the rows of either go through a box filter straight into the target bitmap (a crop, a size, the EXIF
 *     orientation): no full-size picture in between.
 * The IDCT is the Loeffler-Ligtenberg-Moschytz one in 12-bit fixed point (as the IJG's jidctint). */

/* ---- the rows' way into a bitmap ----------------------------------------------------------------------------- */
typedef struct {
    tk_bmp *dst;
    int sw, sh;                 /* the rows coming in */
    int cx, cy, cw, ch;         /* the part of them wanted */
    int tw, th;                 /* its size in the target (<= cw, ch), before the turn */
    int orient;                 /* EXIF: 1 as is, 3 upside down, 6 a quarter turn clockwise, 8 anticlockwise */
    int ty, rows;               /* the target row being summed */
    uint32_t *acc;              /* tw x 3 sums */
    uint16_t *xmap, *xcnt;      /* per source column of the crop: its target column; per target column: how many */
} mr_t;

static void mr_free(mr_t *r)
{
    tk_free(r->acc); tk_free(r->xmap); tk_free(r->xcnt);
    r->acc = 0; r->xmap = 0; r->xcnt = 0;
}
/* the target's size after the turn */
static void mr_size(const mr_t *r, int *w, int *h)
{
    int turn = r->orient == 6 || r->orient == 8;
    *w = turn ? r->th : r->tw;
    *h = turn ? r->tw : r->th;
}
static int mr_init(mr_t *r)
{
    r->acc = tk_alloc((size_t)r->tw * 3 * sizeof(uint32_t));
    r->xmap = tk_alloc((size_t)r->cw * sizeof(uint16_t));
    r->xcnt = tk_alloc((size_t)r->tw * sizeof(uint16_t));
    if (!r->acc || !r->xmap || !r->xcnt) { mr_free(r); return 0; }
    for (int x = 0; x < r->cw; x++) {
        int t = (int)((int64_t)x * r->tw / r->cw);
        r->xmap[x] = (uint16_t)t;
        r->xcnt[t]++;
    }
    r->ty = 0;
    r->rows = 0;
    return 1;
}
static void mr_flush(mr_t *r)
{
    if (!r->rows) return;
    tk_bmp *b = r->dst;
    for (int x = 0; x < r->tw; x++) {
        uint32_t n = (uint32_t)r->xcnt[x] * (uint32_t)r->rows, h = n / 2;
        uint32_t R = (r->acc[3 * x] + h) / n, G = (r->acc[3 * x + 1] + h) / n, B = (r->acc[3 * x + 2] + h) / n;
        int dx = x, dy = r->ty;
        if (r->orient == 3) { dx = r->tw - 1 - x; dy = r->th - 1 - r->ty; }
        else if (r->orient == 6) { dx = r->th - 1 - r->ty; dy = x; }
        else if (r->orient == 8) { dx = r->ty; dy = r->tw - 1 - x; }
        if (dx >= 0 && dy >= 0 && dx < b->w && dy < b->h) b->px[dy * b->stride + dx] = 0xff000000u | R << 16 | G << 8 | B;
        r->acc[3 * x] = r->acc[3 * x + 1] = r->acc[3 * x + 2] = 0;
    }
    r->rows = 0;
}
static void mr_row(mr_t *r, int y, const uint8_t *rgb)
{
    if (y < r->cy || y >= r->cy + r->ch) return;
    int ty = (int)((int64_t)(y - r->cy) * r->th / r->ch);
    if (ty != r->ty) { mr_flush(r); r->ty = ty; }
    const uint8_t *p = rgb + 3 * r->cx;
    for (int x = 0; x < r->cw; x++, p += 3) {
        uint32_t *a = r->acc + 3 * r->xmap[x];
        a[0] += p[0]; a[1] += p[1]; a[2] += p[2];
    }
    r->rows++;
}

/* ---- JPEG ---------------------------------------------------------------------------------------------------- */
typedef int (*mj_read)(void *ctx, uint64_t off, uint8_t *dst, uint32_t len);   /* how many bytes, < 0 an error */
typedef struct { uint16_t look[512]; int32_t maxcode[18]; int32_t valoff[17]; uint8_t vals[256]; } mj_huf;
typedef struct {
    /* the window over the file: buf[pos..len) is the file from foff + pos */
    uint8_t *buf;
    uint32_t cap, pos, len;
    uint64_t foff, fsize;
    int eof, own_buf;
    mj_read rd;
    void *rctx;
    uint32_t bitbuf;
    int bitcnt, marker, starved;
    uint16_t q[4][64];
    mj_huf h[2][4];
    int w, h_, nc, hmax, vmax, mcux, mcuy, my, restart, rst_left;
    struct { int id, hs, vs, tq, td, ta, pred, pw; uint8_t *plane; } c[3];
    int scale, bs, ow, oh;
    uint8_t *rgb;
    int orient;                 /* EXIF orientation (1 if none) */
    uint64_t th_off;            /* the EXIF thumbnail (a JPEG) in the file, 0 if none */
    uint32_t th_len;
    const char *err;
    void (*row)(void *ctx, int y, const uint8_t *rgb);
    void *row_ctx;
} mj_t;

#define MJ_WIN (192u << 10)
#define MJ_LOW (96u << 10)
#define MJ_CHUNK (32u << 10)

static const uint8_t MJ_ZZ[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
                                   13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59,
                                   52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static void mj_free(mj_t *j)
{
    if (j->own_buf) tk_free(j->buf);
    j->buf = 0;
    for (int i = 0; i < 3; i++) { tk_free(j->c[i].plane); j->c[i].plane = 0; }
    tk_free(j->rgb);
    j->rgb = 0;
}
/* more of the file into the window (at most n bytes): 1 if anything came */
static int mj_more(mj_t *j, uint32_t n)
{
    if (j->eof || !j->rd) { j->eof = 1; return 0; }
    uint32_t drop = j->pos & ~511u;             /* whole sectors go: what stays, and the next read, stay sector aligned */
    if (drop) {
        uint32_t keep = j->len - drop;
        for (uint32_t i = 0; i < keep; i++) j->buf[i] = j->buf[drop + i];
        j->foff += drop;
        j->pos -= drop;
        j->len = keep;
    }
    if (n > j->cap - j->len) n = j->cap - j->len;
    if (!n) return 0;
    int got = j->rd(j->rctx, j->foff + j->len, j->buf + j->len, n);
    if (got <= 0) { j->eof = 1; if (got < 0) j->err = "The card could not be read."; return 0; }
    j->len += (uint32_t)got;
    if (j->foff + j->len >= j->fsize) j->eof = 1;
    return 1;
}
/* n bytes at pos (the header's small reads). Every read of the file starts on a 512-byte boundary and is whole
 * sectors long (but the file's last), into the 64-byte aligned window: the card's fast path */
static int mj_need(mj_t *j, uint32_t n)
{
    while (j->len - j->pos < n) {
        uint32_t miss = n - (j->len - j->pos);
        if (!mj_more(j, miss < 4096 ? 4096 : (miss + 511) & ~511u)) return 0;
    }
    return 1;
}
/* skip n bytes, without reading what is not in the window */
static void mj_skip(mj_t *j, uint64_t n)
{
    if (n <= j->len - j->pos) { j->pos += (uint32_t)n; return; }
    uint64_t to = j->foff + j->pos + n;
    j->foff = to & ~(uint64_t)511;
    j->pos = j->len = 0;
    if (!j->rd || to >= j->fsize) { j->eof = 1; return; }
    mj_more(j, 4096);
    j->pos = (uint32_t)(to & 511) < j->len ? (uint32_t)(to & 511) : j->len;
}
static void mj_exif(mj_t *j, uint32_t n)
{
    /* "Exif\0\0", a TIFF header, IFD0 (0x0112 the orientation), then IFD1 (0x0201 / 0x0202: the thumbnail) */
    const uint8_t *p = j->buf + j->pos, *t = p + 6;
    if (n < 14 || p[0] != 'E' || p[1] != 'x' || p[2] != 'i' || p[3] != 'f') return;
    uint32_t tn = n - 6;
    int le = t[0] == 'I';
#define E16(o) ((o) + 2 <= tn ? (le ? (uint32_t)(t[o] | t[(o) + 1] << 8) : (uint32_t)(t[o] << 8 | t[(o) + 1])) : 0u)
#define E32(o) ((o) + 4 <= tn ? (le ? E16(o) | E16((o) + 2) << 16 : E16(o) << 16 | E16((o) + 2)) : 0u)
    uint32_t ifd = E32(4);
    for (int k = 0; k < 2 && ifd && ifd + 2 <= tn; k++) {
        uint32_t cnt = E16(ifd), toff = 0, tlen = 0;
        for (uint32_t i = 0; i < cnt && ifd + 2 + 12 * i + 12 <= tn; i++) {
            uint32_t e = ifd + 2 + 12 * i, tag = E16(e);
            if (!k && tag == 0x0112) { uint32_t o = E16(e + 8); if (o >= 1 && o <= 8) j->orient = (int)o; }
            if (k && tag == 0x0201) toff = E32(e + 8);
            if (k && tag == 0x0202) tlen = E32(e + 8);
        }
        if (k && toff && tlen && toff + tlen <= tn) { j->th_off = j->foff + j->pos + 6 + toff; j->th_len = tlen; }
        ifd = E32(ifd + 2 + 12 * cnt);
    }
#undef E16
#undef E32
    /* mirrored pictures (2, 4, 5, 7) are shown by their turn alone */
    if (j->orient == 2) j->orient = 1; else if (j->orient == 4) j->orient = 3;
    else if (j->orient == 5) j->orient = 6; else if (j->orient == 7) j->orient = 8;
}
static int mj_dht(mj_t *j, const uint8_t *p, uint32_t n)
{
    while (n >= 17) {
        int tc = p[0] >> 4, th = p[0] & 15, total = 0;
        if (tc > 1 || th > 3) return 0;
        mj_huf *h = &j->h[tc][th];
        for (int i = 1; i <= 16; i++) total += p[i];
        if (total > 256 || (uint32_t)total + 17 > n) return 0;
        tk_memcpy(h->vals, p + 17, (size_t)total);
        tk_memset(h->look, 0, sizeof h->look);
        int code = 0, k = 0;
        for (int len = 1; len <= 16; len++) {
            h->valoff[len] = k - code;
            for (int i = 0; i < p[len]; i++, k++, code++)
                if (len <= 9) {
                    int first = code << (9 - len);
                    for (int f = 0; f < 1 << (9 - len); f++) h->look[first + f] = (uint16_t)(len << 8 | h->vals[k]);
                }
            h->maxcode[len] = p[len] ? code - 1 : -1;
            code <<= 1;
        }
        h->maxcode[17] = 0x7fffffff;
        p += 17 + total;
        n -= 17 + (uint32_t)total;
    }
    return 1;
}
/* the header up to the scan: 1 its size, orientation and tables are known, 0 refused (err says why) */
static int mj_head(mj_t *j)
{
    j->orient = 1;
    if (!mj_need(j, 2) || j->buf[j->pos] != 0xff || j->buf[j->pos + 1] != 0xd8) { j->err = "Not a JPEG picture."; return 0; }
    j->pos += 2;
    for (;;) {
        if (!mj_need(j, 4)) { j->err = "The picture's file is cut short."; return 0; }
        const uint8_t *p = j->buf + j->pos;
        if (p[0] != 0xff) { j->err = "The picture's file is damaged."; return 0; }
        if (p[1] == 0xff) { j->pos++; continue; }
        int m = p[1];
        uint32_t n = (uint32_t)(p[2] << 8 | p[3]);
        if (n < 2) { j->err = "The picture's file is damaged."; return 0; }
        n -= 2;
        j->pos += 4;
        if (m == 0xe1 && n <= 65533 && mj_need(j, n)) { if (!j->th_off && j->orient == 1) mj_exif(j, n); j->pos += n; continue; }
        if ((m >= 0xe0 && m <= 0xef) || m == 0xfe) { mj_skip(j, n); continue; }
        if (!mj_need(j, n)) { j->err = "The picture's file is cut short."; return 0; }
        p = j->buf + j->pos;
        if (m == 0xdb) {
            uint32_t o = 0;
            while (o + 65 <= n) {
                int pq = p[o] >> 4, tq = p[o] & 15;
                if (tq > 3 || (pq && o + 129 > n)) break;
                for (int i = 0; i < 64; i++) j->q[tq][MJ_ZZ[i]] = pq ? (uint16_t)(p[o + 1 + 2 * i] << 8 | p[o + 2 + 2 * i]) : p[o + 1 + i];
                o += pq ? 129 : 65;
            }
        } else if (m == 0xc4) {
            if (!mj_dht(j, p, n)) { j->err = "The picture's file is damaged."; return 0; }
        } else if (m == 0xdd && n >= 2) {
            j->restart = p[0] << 8 | p[1];
        } else if (m == 0xc0 || m == 0xc1) {
            if (n < 6 || p[0] != 8) { j->err = "This kind of JPEG (12 bit) can't be shown yet."; return 0; }
            j->h_ = p[1] << 8 | p[2];
            j->w = p[3] << 8 | p[4];
            j->nc = p[5];
            if ((j->nc != 1 && j->nc != 3) || n < 6 + 3u * (uint32_t)j->nc || !j->w || !j->h_) { j->err = "This kind of JPEG (CMYK) can't be shown yet."; return 0; }
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = p[6 + 3 * i];
                j->c[i].hs = p[7 + 3 * i] >> 4;
                j->c[i].vs = p[7 + 3 * i] & 15;
                j->c[i].tq = p[8 + 3 * i] & 3;
                if (!j->c[i].hs || !j->c[i].vs || j->c[i].hs > 4 || j->c[i].vs > 4) { j->err = "The picture's file is damaged."; return 0; }
                if (j->c[i].hs > j->hmax) j->hmax = j->c[i].hs;
                if (j->c[i].vs > j->vmax) j->vmax = j->c[i].vs;
            }
            if (j->nc == 1) { j->c[0].hs = j->c[0].vs = j->hmax = j->vmax = 1; }
        } else if (m == 0xc2) {
            j->err = "Progressive JPEGs can't be shown yet.";
            return 0;
        } else if (m >= 0xc3 && m <= 0xcf && m != 0xc8) {
            j->err = "This kind of JPEG can't be shown yet.";
            return 0;
        } else if (m == 0xda) {
            if (!j->w || n < 1 || p[0] != j->nc || n < 1 + 2u * (uint32_t)j->nc) { j->err = "The picture's file is damaged."; return 0; }
            for (int i = 0; i < j->nc; i++) {
                int ok = 0;
                for (int k = 0; k < j->nc; k++)
                    if (j->c[k].id == p[1 + 2 * i]) { j->c[k].td = p[2 + 2 * i] >> 4 & 3; j->c[k].ta = p[2 + 2 * i] & 3; ok = 1; }
                if (!ok) { j->err = "The picture's file is damaged."; return 0; }
            }
            j->pos += n;
            break;
        } else if (m == 0xd9) {
            j->err = "The picture's file is cut short.";
            return 0;
        }
        j->pos += n;
    }
    return 1;
}
/* after mj_head: the buffers for the largest reduction that still gives at least want_w x want_h */
static int mj_start(mj_t *j, int want_w, int want_h)
{
    int s = 8;
    while (s > 1 && ((j->w + s - 1) / s < want_w || (j->h_ + s - 1) / s < want_h)) s >>= 1;
    j->scale = s;
    j->bs = 8 / s;
    j->ow = (j->w + s - 1) / s;
    j->oh = (j->h_ + s - 1) / s;
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h_ + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->nc; i++) {
        j->c[i].pw = j->mcux * j->c[i].hs * j->bs;
        j->c[i].plane = tk_alloc((size_t)j->c[i].pw * (size_t)(j->c[i].vs * j->bs));
        if (!j->c[i].plane) { j->err = "Not enough memory for this picture."; return 0; }
    }
    j->rgb = tk_alloc((size_t)j->mcux * (size_t)(j->hmax * j->bs) * 3);
    if (!j->rgb) { j->err = "Not enough memory for this picture."; return 0; }
    j->rst_left = j->restart;
    j->bitbuf = 0; j->bitcnt = 0; j->marker = 0; j->my = 0;
    return 1;
}

static inline void mj_fillbits(mj_t *j)
{
    while (j->bitcnt <= 24) {
        uint32_t b = 0;
        if (!j->marker) {
            if (j->pos < j->len) {
                b = j->buf[j->pos++];
                if (b == 0xff) {
                    if (j->pos < j->len) {
                        uint8_t n = j->buf[j->pos++];
                        if (n) { j->marker = n; b = 0; }
                    } else { j->marker = 0xd9; b = 0; j->starved |= !j->eof; }
                }
            } else j->starved |= !j->eof;
        }
        j->bitbuf |= b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}
static inline int mj_sym(mj_t *j, const mj_huf *h)
{
    if (j->bitcnt < 16) mj_fillbits(j);
    unsigned e = h->look[j->bitbuf >> 23];
    if (e) { int n = (int)(e >> 8); j->bitbuf <<= n; j->bitcnt -= n; return (int)(e & 255); }
    int32_t code = (int32_t)(j->bitbuf >> 16);
    for (int n = 10; n <= 16; n++) {
        int32_t c = code >> (16 - n);
        if (c <= h->maxcode[n]) { j->bitbuf <<= n; j->bitcnt -= n; return h->vals[(h->valoff[n] + c) & 255]; }
    }
    j->bitbuf <<= 16; j->bitcnt -= 16;
    return 0;
}
static inline int mj_recv(mj_t *j, int n)
{
    if (!n) return 0;
    if (j->bitcnt < n) mj_fillbits(j);
    int v = (int)(j->bitbuf >> (32 - n));
    j->bitbuf <<= n;
    j->bitcnt -= n;
    return v < 1 << (n - 1) ? v - (1 << n) + 1 : v;
}

#define MJ_F(x) ((int)((x) * 4096 + .5f))
#define MJ_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; p3 = s6; p1 = (p2 + p3) * MJ_F(.5411961f); t2 = p1 + p3 * -MJ_F(1.847759065f); t3 = p1 + p2 * MJ_F(.765366865f); \
    p2 = s0; p3 = s4; t0 = (p2 + p3) * 4096; t1 = (p2 - p3) * 4096; \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * MJ_F(1.175875602f); t0 = t0 * MJ_F(.298631336f); t1 = t1 * MJ_F(2.053119869f); \
    t2 = t2 * MJ_F(3.072711026f); t3 = t3 * MJ_F(1.501321110f); p1 = p5 + p1 * -MJ_F(.899976223f); \
    p2 = p5 + p2 * -MJ_F(2.562915447f); p3 = p3 * -MJ_F(1.961570560f); p4 = p4 * -MJ_F(.390180644f); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;
static inline uint8_t mj_clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void mj_idct(const int16_t *d, uint8_t *out)
{
    int v[64];
    for (int i = 0; i < 8; i++) {
        const int16_t *s = d + i;
        int *o = v + i;
        if (!(s[8] | s[16] | s[24] | s[32] | s[40] | s[48] | s[56])) {
            int dc = s[0] * 4;
            o[0] = o[8] = o[16] = o[24] = o[32] = o[40] = o[48] = o[56] = dc;
            continue;
        }
        MJ_1D(s[0], s[8], s[16], s[24], s[32], s[40], s[48], s[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        o[0] = (x0 + t3) >> 10; o[56] = (x0 - t3) >> 10; o[8] = (x1 + t2) >> 10; o[48] = (x1 - t2) >> 10;
        o[16] = (x2 + t1) >> 10; o[40] = (x2 - t1) >> 10; o[24] = (x3 + t0) >> 10; o[32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; i++) {
        const int *s = v + 8 * i;
        uint8_t *o = out + 8 * i;
        MJ_1D(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = mj_clamp((x0 + t3) >> 17); o[7] = mj_clamp((x0 - t3) >> 17); o[1] = mj_clamp((x1 + t2) >> 17);
        o[6] = mj_clamp((x1 - t2) >> 17); o[2] = mj_clamp((x2 + t1) >> 17); o[5] = mj_clamp((x2 - t1) >> 17);
        o[3] = mj_clamp((x3 + t0) >> 17); o[4] = mj_clamp((x3 - t0) >> 17);
    }
}
/* one block of component c into its plane at block (bx, by) of the MCU row */
static void mj_block(mj_t *j, int ci, int bx, int by)
{
    int16_t d[64];
    const uint16_t *q = j->q[j->c[ci].tq];
    const mj_huf *hd = &j->h[0][j->c[ci].td], *ha = &j->h[1][j->c[ci].ta];
    int full = j->scale != 8;
    if (full) tk_memset(d, 0, sizeof d);
    int t = mj_sym(j, hd);
    j->c[ci].pred += mj_recv(j, t & 15);
    int dc = j->c[ci].pred * q[0];
    d[0] = (int16_t)dc;
    for (int k = 1; k < 64;) {
        int rs = mj_sym(j, ha), r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        int v = mj_recv(j, s);
        if (k < 64 && full) d[MJ_ZZ[k]] = (int16_t)(v * q[MJ_ZZ[k]]);
        k++;
    }
    int bs = j->bs, pw = j->c[ci].pw;
    uint8_t *o = j->c[ci].plane + (size_t)(by * bs) * (size_t)pw + (size_t)(bx * bs);
    if (!full) { o[0] = mj_clamp(((dc + 4) >> 3) + 128); return; }
    uint8_t px[64];
    mj_idct(d, px);
    if (bs == 8) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) o[y * pw + x] = px[8 * y + x];
    } else {
        int s = j->scale, sh = s == 2 ? 2 : 4;
        for (int y = 0; y < bs; y++)
            for (int x = 0; x < bs; x++) {
                int a = 0;
                for (int yy = 0; yy < s; yy++) for (int xx = 0; xx < s; xx++) a += px[8 * (y * s + yy) + x * s + xx];
                o[y * pw + x] = (uint8_t)((a + (1 << (sh - 1))) >> sh);
            }
    }
}
static void mj_restart(mj_t *j)
{
    /* the bits left belong to nothing; the RSTn marker is either already seen or next in the bytes */
    if (!j->marker)
        while (j->pos + 1 < j->len) {
            if (j->buf[j->pos] == 0xff && j->buf[j->pos + 1] >= 0xd0 && j->buf[j->pos + 1] <= 0xd7) { j->pos += 2; break; }
            if (j->buf[j->pos] == 0xff && j->buf[j->pos + 1] && j->buf[j->pos + 1] != 0xff) break;
            j->pos++;
        }
    if (j->marker >= 0xd0 && j->marker <= 0xd7) j->marker = 0;
    j->bitbuf = 0;
    j->bitcnt = 0;
    for (int i = 0; i < 3; i++) j->c[i].pred = 0;
    j->rst_left = j->restart;
}
/* one step: some more of the file, or one MCU row. 0 more to do, 1 the picture is whole, -1 failed (err) */
static int mj_step(mj_t *j)
{
    if (j->my >= j->mcuy) return 1;
    if (!j->eof && j->len - j->pos < MJ_LOW) { mj_more(j, MJ_CHUNK); if (j->err) return -1; return 0; }
    for (int mx = 0; mx < j->mcux; mx++) {
        if (j->restart && !j->rst_left) mj_restart(j);
        for (int i = 0; i < j->nc; i++)
            for (int by = 0; by < j->c[i].vs; by++)
                for (int bx = 0; bx < j->c[i].hs; bx++) mj_block(j, i, mx * j->c[i].hs + bx, by);
        if (j->restart) j->rst_left--;
    }
    if (j->starved) { j->err = "This JPEG's rows are too large to show yet."; return -1; }
    /* the MCU row's colours, a row at a time */
    int rows = j->vmax * j->bs, y0 = j->my * rows;
    for (int y = 0; y < rows && y0 + y < j->oh; y++) {
        uint8_t *o = j->rgb;
        const uint8_t *Y = j->c[0].plane + (size_t)(y * j->c[0].vs / j->vmax) * (size_t)j->c[0].pw;
        if (j->nc == 1) {
            for (int x = 0; x < j->ow; x++, o += 3) o[0] = o[1] = o[2] = Y[x];
        } else {
            const uint8_t *Cb = j->c[1].plane + (size_t)(y * j->c[1].vs / j->vmax) * (size_t)j->c[1].pw;
            const uint8_t *Cr = j->c[2].plane + (size_t)(y * j->c[2].vs / j->vmax) * (size_t)j->c[2].pw;
            int h0 = j->c[0].hs, h1 = j->c[1].hs, h2 = j->c[2].hs, hm = j->hmax;
            for (int x = 0; x < j->ow; x++, o += 3) {
                int yy = Y[h0 == hm ? x : x * h0 / hm], cb = Cb[h1 * 2 == hm ? x >> 1 : x * h1 / hm] - 128,
                    cr = Cr[h2 * 2 == hm ? x >> 1 : x * h2 / hm] - 128;
                o[0] = mj_clamp(yy + ((91881 * cr + 32768) >> 16));
                o[1] = mj_clamp(yy - ((22554 * cb + 46802 * cr + 32768) >> 16));
                o[2] = mj_clamp(yy + ((116130 * cb + 32768) >> 16));
            }
        }
        j->row(j->row_ctx, y0 + y, j->rgb);
    }
    j->my++;
    return j->my >= j->mcuy;
}

/* ---- PNG ----------------------------------------------------------------------------------------------------- */
typedef struct { uint16_t fast[1024]; uint16_t count[16], sym[288]; } mp_huf;
typedef struct {
    const uint8_t *in;
    uint32_t n, pos, chunk_left;
    int in_end;
    uint32_t bb;
    int bc;
    uint8_t *win;               /* 32 KiB */
    uint32_t wp;
    int final, btype, in_block, started;
    uint32_t stored_left;
    mp_huf *hl, *hd;
    int w, h, depth, ctype, bpp, chans;      /* bpp: bytes per pixel for the filters (>= 1) */
    uint32_t linelen, lp;
    uint8_t *line, *prev, *rgb;
    uint8_t pal[256][3];
    int y;
    const char *err;
    void (*row)(void *ctx, int y, const uint8_t *rgb);
    void *row_ctx;
} mp_t;

static void mp_free(mp_t *p)
{
    tk_free(p->win); tk_free(p->hl); tk_free(p->hd); tk_free(p->line); tk_free(p->prev); tk_free(p->rgb);
    p->win = 0; p->hl = p->hd = 0; p->line = p->prev = p->rgb = 0;
}
static uint32_t mp_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* the next byte of the IDAT chunks' data */
static inline uint32_t mp_byte(mp_t *p)
{
    while (!p->chunk_left) {
        if (p->in_end || p->pos + 12 > p->n) { p->in_end = 1; return 0; }
        p->pos += 4;                                        /* the last chunk's CRC */
        if (p->pos + 8 > p->n) { p->in_end = 1; return 0; }
        uint32_t len = mp_be32(p->in + p->pos);
        const uint8_t *t = p->in + p->pos + 4;
        if (t[0] != 'I' || t[1] != 'D' || t[2] != 'A' || t[3] != 'T' || len > p->n - p->pos - 8) { p->in_end = 1; return 0; }
        p->pos += 8;
        p->chunk_left = len;
    }
    p->chunk_left--;
    return p->in[p->pos++];
}
static inline uint32_t mp_bits(mp_t *p, int n)
{
    while (p->bc < n) { p->bb |= mp_byte(p) << p->bc; p->bc += 8; }
    uint32_t v = p->bb & ((1u << n) - 1);
    p->bb >>= n;
    p->bc -= n;
    return v;
}
static void mp_build(mp_huf *h, const uint8_t *len, int n)
{
    uint16_t offs[16], next[16];
    tk_memset(h->count, 0, sizeof h->count);
    tk_memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    int code = 0;
    next[0] = 0;
    for (int l = 1; l < 16; l++) {
        if (l > 1) offs[l] = (uint16_t)(offs[l - 1] + h->count[l - 1]);
        code = (code + (l > 1 ? h->count[l - 1] : 0)) << 1;
        next[l] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        int l = len[i];
        if (!l) continue;
        h->sym[offs[l]++] = (uint16_t)i;
        int c = next[l]++;
        if (l <= 10) {
            int rev = 0;
            for (int b = 0; b < l; b++) rev |= (c >> b & 1) << (l - 1 - b);
            for (int f = rev; f < 1024; f += 1 << l) h->fast[f] = (uint16_t)(l << 12 | i);
        }
    }
}
static inline int mp_sym(mp_t *p, const mp_huf *h)
{
    while (p->bc < 15) { p->bb |= mp_byte(p) << p->bc; p->bc += 8; }
    unsigned e = h->fast[p->bb & 1023];
    if (e) { int l = (int)(e >> 12); p->bb >>= l; p->bc -= l; return (int)(e & 4095); }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)(p->bb & 1);
        p->bb >>= 1;
        p->bc--;
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    p->err = "The picture's file is damaged.";
    return 256;
}
static int mp_paeth(int a, int b, int c)
{
    int pa = b - c, pb = a - c, pc = pa + pb;
    pa = pa < 0 ? -pa : pa; pb = pb < 0 ? -pb : pb; pc = pc < 0 ? -pc : pc;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
/* a whole scanline (its filter byte first) has arrived */
static void mp_line(mp_t *p)
{
    uint8_t *c = p->line + 1, *u = p->prev + 1;
    int n = (int)p->linelen - 1, bpp = p->bpp, f = p->line[0];
    if (f == 1) { for (int i = bpp; i < n; i++) c[i] = (uint8_t)(c[i] + c[i - bpp]); }
    else if (f == 2) { for (int i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + u[i]); }
    else if (f == 3) { for (int i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + (((i >= bpp ? c[i - bpp] : 0) + u[i]) >> 1)); }
    else if (f == 4) { for (int i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + mp_paeth(i >= bpp ? c[i - bpp] : 0, u[i], i >= bpp ? u[i - bpp] : 0)); }
    uint8_t *o = p->rgb;
    int st = p->depth == 16 ? 2 : 1;
    for (int x = 0; x < p->w; x++, o += 3) {
        int r, g, b, a = 255;
        if (p->depth < 8) {
            int bit = x * p->depth, v = c[bit >> 3] >> (8 - p->depth - (bit & 7)) & ((1 << p->depth) - 1);
            if (p->ctype == 3) { r = p->pal[v][0]; g = p->pal[v][1]; b = p->pal[v][2]; }
            else r = g = b = v * 255 / ((1 << p->depth) - 1);
        } else {
            const uint8_t *s = c + x * p->chans * st;
            if (p->ctype == 3) { r = p->pal[s[0]][0]; g = p->pal[s[0]][1]; b = p->pal[s[0]][2]; }
            else if (p->ctype == 0 || p->ctype == 4) { r = g = b = s[0]; if (p->ctype == 4) a = s[st]; }
            else { r = s[0]; g = s[st]; b = s[2 * st]; if (p->ctype == 6) a = s[3 * st]; }
        }
        if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }      /* over black */
        o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
    }
    p->row(p->row_ctx, p->y, p->rgb);
    p->y++;
    uint8_t *t = p->prev; p->prev = p->line; p->line = t;
    p->lp = 0;
}
static inline void mp_out(mp_t *p, uint8_t b)
{
    p->win[p->wp++ & 32767] = b;
    if (p->y >= p->h) return;
    p->line[p->lp++] = b;
    if (p->lp == p->linelen) mp_line(p);
}
/* the header (the whole file is at in[0..n)): 1 ready, 0 refused (err) */
static int mp_head(mp_t *p)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (p->n < 33) { p->err = "Not a PNG picture."; return 0; }
    for (int i = 0; i < 8; i++) if (p->in[i] != SIG[i]) { p->err = "Not a PNG picture."; return 0; }
    const uint8_t *h = p->in + 16;
    p->w = (int)mp_be32(h);
    p->h = (int)mp_be32(h + 4);
    p->depth = h[8];
    p->ctype = h[9];
    if (p->w <= 0 || p->h <= 0 || p->w > 16384 || p->h > 16384) { p->err = "The picture is too large to show."; return 0; }
    if (h[12]) { p->err = "Interlaced PNGs can't be shown yet."; return 0; }
    p->chans = p->ctype == 2 ? 3 : p->ctype == 4 ? 2 : p->ctype == 6 ? 4 : 1;
    int ok = (p->ctype == 0 && (p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8 || p->depth == 16)) ||
             (p->ctype == 3 && (p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8)) ||
             ((p->ctype == 2 || p->ctype == 4 || p->ctype == 6) && (p->depth == 8 || p->depth == 16));
    if (!ok) { p->err = "The picture's file is damaged."; return 0; }
    int bits = p->chans * p->depth;
    p->bpp = bits >= 8 ? bits / 8 : 1;
    p->linelen = 1 + ((uint32_t)p->w * (uint32_t)bits + 7) / 8;
    /* the chunks before the first IDAT (the palette) */
    uint32_t pos = 8;
    for (;;) {
        if (pos + 12 > p->n) { p->err = "The picture's file is cut short."; return 0; }
        uint32_t len = mp_be32(p->in + pos);
        const uint8_t *t = p->in + pos + 4;
        if (len > p->n - pos - 12) { p->err = "The picture's file is cut short."; return 0; }
        if (t[0] == 'P' && t[1] == 'L' && t[2] == 'T' && t[3] == 'E')
            for (uint32_t i = 0; i < len / 3 && i < 256; i++) { p->pal[i][0] = t[4 + 3 * i]; p->pal[i][1] = t[5 + 3 * i]; p->pal[i][2] = t[6 + 3 * i]; }
        if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') { p->pos = pos + 8; p->chunk_left = len; break; }
        if (t[0] == 'I' && t[1] == 'E') { p->err = "The picture's file is damaged."; return 0; }
        pos += 12 + len;
    }
    p->win = tk_alloc(32768);
    p->hl = tk_alloc(sizeof(mp_huf));
    p->hd = tk_alloc(sizeof(mp_huf));
    p->line = tk_alloc(p->linelen + 8);
    p->prev = tk_alloc(p->linelen + 8);
    p->rgb = tk_alloc((size_t)p->w * 3);
    if (!p->win || !p->hl || !p->hd || !p->line || !p->prev || !p->rgb) { p->err = "Not enough memory for this picture."; return 0; }
    return 1;
}
/* one step: about 32 KiB of the picture's bytes (a few rows). 0 more, 1 whole, -1 failed */
static int mp_step(mp_t *p)
{
    static const uint16_t LB[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t LX[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t DB[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t DX[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    static const uint8_t ORD[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    if (!p->started) { mp_bits(p, 16); p->started = 1; }           /* zlib's two header bytes */
    uint32_t budget = p->wp + (32u << 10);
    while (p->y < p->h && (int32_t)(budget - p->wp) > 0) {
        if (p->err) return -1;
        if (p->in_end && !p->bc) { p->err = "The picture's file is cut short."; return -1; }
        if (!p->in_block) {
            if (p->final) break;
            p->final = (int)mp_bits(p, 1);
            p->btype = (int)mp_bits(p, 2);
            if (p->btype == 0) {
                int drop = p->bc & 7;                        /* to the byte boundary; the bytes read ahead stay */
                p->bb >>= drop; p->bc -= drop;
                p->stored_left = mp_bits(p, 16);
                mp_bits(p, 16);
            } else if (p->btype == 1) {
                uint8_t l[288];
                for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                mp_build(p->hl, l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                mp_build(p->hd, l, 30);
            } else if (p->btype == 2) {
                uint8_t l[320], cl[19];
                int nl = (int)mp_bits(p, 5) + 257, nd = (int)mp_bits(p, 5) + 1, nc = (int)mp_bits(p, 4) + 4, i = 0;
                tk_memset(cl, 0, sizeof cl);
                for (int k = 0; k < nc; k++) cl[ORD[k]] = (uint8_t)mp_bits(p, 3);
                mp_build(p->hl, cl, 19);
                while (i < nl + nd) {
                    int s = mp_sym(p, p->hl);
                    if (s < 16) l[i++] = (uint8_t)s;
                    else {
                        int rep, v = 0;
                        if (s == 16) { if (!i) { p->err = "The picture's file is damaged."; return -1; } v = l[i - 1]; rep = 3 + (int)mp_bits(p, 2); }
                        else if (s == 17) rep = 3 + (int)mp_bits(p, 3);
                        else if (s == 18) rep = 11 + (int)mp_bits(p, 7);
                        else { p->err = "The picture's file is damaged."; return -1; }
                        if (i + rep > nl + nd) { p->err = "The picture's file is damaged."; return -1; }
                        while (rep--) l[i++] = (uint8_t)v;
                    }
                }
                mp_build(p->hl, l, nl);
                mp_build(p->hd, l + nl, nd);
            } else { p->err = "The picture's file is damaged."; return -1; }
            p->in_block = 1;
        }
        if (p->btype == 0) {
            while (p->stored_left && (int32_t)(budget - p->wp) > 0 && !p->in_end) { mp_out(p, (uint8_t)mp_bits(p, 8)); p->stored_left--; }
            if (!p->stored_left) p->in_block = 0;
            continue;
        }
        int s = mp_sym(p, p->hl);
        if (s < 256) { mp_out(p, (uint8_t)s); continue; }
        if (s == 256) { p->in_block = 0; continue; }
        s -= 257;
        if (s >= 29) { p->err = "The picture's file is damaged."; return -1; }
        int len = LB[s] + (int)mp_bits(p, LX[s]);
        int ds = mp_sym(p, p->hd);
        if (ds >= 30) { p->err = "The picture's file is damaged."; return -1; }
        uint32_t dist = DB[ds] + mp_bits(p, DX[ds]);
        if (dist > p->wp) { p->err = "The picture's file is damaged."; return -1; }
        while (len--) mp_out(p, p->win[(p->wp - dist) & 32767]);
    }
    if (p->err) return -1;
    if (p->y >= p->h) return 1;
    if (p->final && !p->in_block) { p->err = "The picture's file is cut short."; return -1; }
    return 0;
}
