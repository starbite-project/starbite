/* SPDX-License-Identifier: GPL-2.0-only
 * DTBH selection and the S-Boot merge for Android device trees: see dtmerge.h.
 * Byte arrays only (big-endian FDT, little-endian DTBH); every offset is bounds checked against the
 * blob's own totalsize, so a damaged file fails cleanly (the caller falls back to the menu).
 */
#include "dtmerge.h"

#define FDT_MAGIC 0xd00dfeedU
#define T_BEGIN 1U
#define T_END   2U
#define T_PROP  3U
#define T_NOP   4U
#define T_FEND  9U
#define FDT_MAX (32U << 20)

static uint32_t be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wbe(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint32_t a4(uint32_t v)
{
    return (v + 3U) & ~3U;
}

struct fdt {
    const uint8_t *b;
    uint32_t total, ostruct, send, ostr, ssize, rsv;
};

static int fdt_open(const uint8_t *b, uint64_t avail, struct fdt *f)
{
    if (avail < 40 || be(b) != FDT_MAGIC)
        return 0;
    f->b = b;
    f->total = be(b + 4);
    f->ostruct = be(b + 8);
    f->ostr = be(b + 12);
    f->rsv = be(b + 16);
    uint32_t ver = be(b + 20);
    f->ssize = be(b + 32);
    uint32_t sstruct = ver >= 17 ? be(b + 36) : 0;
    if (f->total > avail || f->total > FDT_MAX || ver < 16 || f->ostruct < 40 || f->ostruct >= f->total ||
        (f->ostruct & 3U) || f->ostr > f->total || f->ssize > f->total - f->ostr || f->rsv < 40 ||
        f->rsv >= f->total || (f->rsv & 7U))
        return 0;
    f->send = sstruct ? f->ostruct + sstruct : f->ostr > f->ostruct ? f->ostr : f->total;
    if (f->send > f->total || f->send < f->ostruct)
        return 0;
    return 1;
}

/* The string at nameoff, and how many bytes it may span before the strings block ends. */
static const uint8_t *fstr(const struct fdt *f, uint32_t off, uint32_t *max)
{
    if (off >= f->ssize) {
        *max = 0;
        return 0;
    }
    *max = f->ssize - off;
    return f->b + f->ostr + off;
}

static int str_eq(const uint8_t *s, uint32_t max, const char *want)
{
    uint32_t k = 0;
    if (!s)
        return 0;
    for (; k < max && want[k]; ++k)
        if (s[k] != (uint8_t)want[k])
            return 0;
    return k < max && !want[k] && !s[k];
}

/* Bytes of the token at p (4-aligned, token word included), 0 if it does not fit. */
static uint32_t tok_len(const struct fdt *f, uint32_t p)
{
    if (p + 4 > f->send)
        return 0;
    uint32_t t = be(f->b + p);
    if (t == T_BEGIN) {
        uint32_t q = p + 4;
        while (q < f->send && f->b[q])
            ++q;
        if (q >= f->send)
            return 0;
        return a4(q + 1) - p;
    }
    if (t == T_PROP) {
        if (p + 12 > f->send)
            return 0;
        uint32_t len = be(f->b + p + 4);
        if (len > f->send - p - 12)
            return 0;
        return 12 + a4(len);
    }
    if (t == T_END || t == T_NOP || t == T_FEND)
        return 4;
    return 0;
}

static int node_is(const struct fdt *f, uint32_t p, const char *want)
{
    return str_eq(f->b + p + 4, f->send - p - 4, want);
}

static int node_is_memory(const struct fdt *f, uint32_t p)
{
    const uint8_t *n = f->b + p + 4;
    static const char m[] = "memory";
    uint32_t max = f->send - p - 4;
    for (unsigned k = 0; k < 6; ++k)
        if (k >= max || n[k] != (uint8_t)m[k])
            return 0;
    return max > 6 && (n[6] == 0 || n[6] == '@');
}

static uint32_t prop_name_off(const struct fdt *f, uint32_t p)
{
    return be(f->b + p + 8);
}

/* ---- DTBH --------------------------------------------------------------------------------------- */

int dtbh_pick(const uint8_t *blob, uint64_t size, uint32_t chip, uint32_t rev, uint32_t *off,
              uint32_t *fsize, unsigned *idx, unsigned *n)
{
    struct fdt f;
    *n = 0;
    *idx = 0;
    if (size >= 40 && be(blob) == FDT_MAGIC) {
        if (!fdt_open(blob, size, &f))
            return 0;
        *off = 0;
        *fsize = f.total;
        return 1;
    }
    if (size < 12 || le32(blob) != DTBH_MAGIC)
        return 0;
    uint32_t cnt = le32(blob + 8);
    if (!cnt || cnt > 64 || 12ULL + 32ULL * cnt > size)
        return 0;
    *n = cnt;
    for (unsigned pass = 0; pass < 3; ++pass) {
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint8_t *e = blob + 12 + 32 * i;
            uint32_t c = le32(e), lo = le32(e + 12), hi = le32(e + 16), o = le32(e + 20), s = le32(e + 24);
            if (pass == 0 && !(c == chip && lo <= rev && rev <= hi))
                continue;
            if (pass == 1 && c != chip)
                continue;
            if ((uint64_t)o + s > size || !fdt_open(blob + o, s, &f))
                continue;
            *off = o;
            *fsize = f.total;
            *idx = i;
            return 1;
        }
    }
    return 0;
}

int fdt_board_id(const uint8_t *sb, uint32_t *chip, uint32_t *rev)
{
    struct fdt f;
    int have = 0, in_chosen = 0;
    uint32_t dt_rev = 0, ch_rev = 0, have_ch = 0;
    *chip = 0;
    *rev = 0;
    if (!fdt_open(sb, FDT_MAX, &f))
        return 0;
    unsigned depth = 0;
    for (uint32_t p = f.ostruct, l; p < f.send; p += l) {
        l = tok_len(&f, p);
        if (!l)
            return 0;
        uint32_t t = be(sb + p);
        if (t == T_BEGIN) {
            ++depth;
            in_chosen = depth == 2 && node_is(&f, p, "chosen");
        } else if (t == T_END) {
            if (!depth--)
                return 0;
            in_chosen = 0;
            if (!depth)
                break;
        } else if (t == T_PROP && be(sb + p + 4) == 4) {
            uint32_t max;
            const uint8_t *nm = fstr(&f, prop_name_off(&f, p), &max);
            uint32_t v = be(sb + p + 12);
            if (depth == 1 && str_eq(nm, max, "model_info-chip")) {
                *chip = v;
                have = 1;
            } else if (depth == 1 && str_eq(nm, max, "model_info-hw_rev")) {
                dt_rev = v;
            } else if (in_chosen && depth == 2 && str_eq(nm, max, "revision")) {
                ch_rev = v;
                have_ch = 1;
            }
        } else if (t == T_FEND) {
            break;
        }
    }
    *rev = have_ch ? ch_rev : dt_rev;
    return have;
}

/* ---- the merge ---------------------------------------------------------------------------------- */

#define MAX_ROOT 64U
#define MAX_MEM  16U
#define EXTRA_STR 1024U

struct mg {
    uint8_t *o;
    uint32_t w, cap;
    int bad;
    const struct fdt *base, *sb;
    char xs[EXTRA_STR];          /* strings the base lacks, appended after its block */
    uint32_t xn;
    const char *extra;
};

static void emit(struct mg *m, const uint8_t *src, uint32_t n)
{
    if (m->bad || n > m->cap - m->w) {
        m->bad = 1;
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
        m->o[m->w + i] = src[i];
    m->w += n;
}

static void emit_u32(struct mg *m, uint32_t v)
{
    uint8_t b[4];
    wbe(b, v);
    emit(m, b, 4);
}

static void emit_pad(struct mg *m)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    if (m->w & 3U)
        emit(m, zero, 4U - (m->w & 3U));
}

/* The output's offset of string s (max bytes may be read): the base's block, else the extras. */
static uint32_t str_off(struct mg *m, const uint8_t *s, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && s[n])
        ++n;
    if (n >= max) {
        m->bad = 1;
        return 0;
    }
    const uint8_t *bs = m->base->b + m->base->ostr;
    for (uint32_t i = 0; i + n < m->base->ssize; ++i) {
        uint32_t k = 0;
        while (k < n && bs[i + k] == s[k])
            ++k;
        if (k == n && !bs[i + n])
            return i;
    }
    for (uint32_t i = 0; i + n < m->xn; ++i) {
        uint32_t k = 0;
        while (k < n && (uint8_t)m->xs[i + k] == s[k])
            ++k;
        if (k == n && !m->xs[i + n])
            return m->base->ssize + i;
    }
    if (m->xn + n + 1 > EXTRA_STR) {
        m->bad = 1;
        return 0;
    }
    uint32_t at = m->xn;
    for (uint32_t k = 0; k < n; ++k)
        m->xs[m->xn++] = (char)s[k];
    m->xs[m->xn++] = 0;
    return m->base->ssize + at;
}

/* An S-Boot property token at p, its name moved to the output's strings; bootargs extended when
 * extend is set. */
static void emit_sb_prop(struct mg *m, uint32_t p, int extend)
{
    const struct fdt *s = m->sb;
    uint32_t len = be(s->b + p + 4), max;
    const uint8_t *nm = fstr(s, prop_name_off(s, p), &max);
    if (!nm) {
        m->bad = 1;
        return;
    }
    uint32_t off = str_off(m, nm, max);
    const uint8_t *val = s->b + p + 12;
    uint32_t elen = 0;
    while (m->extra && m->extra[elen])
        ++elen;
    if (extend && elen && len && str_eq(nm, max, "bootargs")) {
        uint32_t old = val[len - 1] ? len : len - 1;   /* without its NUL */
        emit_u32(m, T_PROP);
        emit_u32(m, old + 1 + elen + 1);
        emit_u32(m, off);
        emit(m, val, old);
        static const uint8_t sp = ' ', nul = 0;
        emit(m, &sp, 1);
        emit(m, (const uint8_t *)m->extra, elen);
        emit(m, &nul, 1);
        emit_pad(m);
        return;
    }
    emit_u32(m, T_PROP);
    emit_u32(m, len);
    emit_u32(m, off);
    emit(m, val, len);
    emit_pad(m);
}

/* What S-Boot's FDT holds that the merge needs. */
struct sbinfo {
    uint32_t root[MAX_ROOT], nroot;
    uint32_t chosen0, chosen1;     /* its /chosen property tokens [chosen0, chosen1); 0 = none */
    int has_chosen;
    uint32_t mem0[MAX_MEM], mem1[MAX_MEM], nmem;   /* whole memory nodes [mem0, mem1) */
};

static int sb_scan(const struct fdt *f, struct sbinfo *si)
{
    unsigned depth = 0;
    int in_chosen = 0;
    uint32_t mem_at = 0;
    int in_mem = 0;
    si->nroot = si->nmem = 0;
    si->has_chosen = 0;
    si->chosen0 = si->chosen1 = 0;
    for (uint32_t p = f->ostruct, l; p < f->send; p += l) {
        l = tok_len(f, p);
        if (!l)
            return 0;
        uint32_t t = be(f->b + p);
        if (t == T_BEGIN) {
            ++depth;
            if (in_chosen && depth == 3 && !si->chosen1)
                si->chosen1 = p;
            if (depth == 2 && node_is(f, p, "chosen") && !si->has_chosen) {
                in_chosen = 1;
                si->has_chosen = 1;
                si->chosen0 = p + l;
            } else if (depth == 2 && node_is_memory(f, p)) {
                in_mem = 1;
                mem_at = p;
            }
        } else if (t == T_END) {
            if (!depth)
                return 0;
            if (depth == 2 && in_chosen) {
                if (!si->chosen1)
                    si->chosen1 = p;
                in_chosen = 0;
            }
            if (depth == 2 && in_mem) {
                in_mem = 0;
                if (si->nmem < MAX_MEM) {
                    si->mem0[si->nmem] = mem_at;
                    si->mem1[si->nmem++] = p + 4;
                }
            }
            if (--depth == 0)
                return 1;
        } else if (t == T_PROP) {
            if (depth == 1 && si->nroot < MAX_ROOT)
                si->root[si->nroot++] = p;
        } else if (t == T_FEND) {
            return 0;
        }
    }
    return 0;
}

/* 1 if base's root has a property named like S-Boot's property token at sp. */
static int base_root_has(const struct fdt *b, const struct fdt *s, uint32_t sp)
{
    uint32_t smax;
    const uint8_t *sn = fstr(s, prop_name_off(s, sp), &smax);
    unsigned depth = 0;
    for (uint32_t p = b->ostruct, l; p < b->send; p += l) {
        l = tok_len(b, p);
        if (!l)
            return 0;
        uint32_t t = be(b->b + p);
        if (t == T_BEGIN) {
            if (++depth == 2)
                return 0;              /* root properties come before its subnodes */
        } else if (t == T_END) {
            return 0;
        } else if (t == T_PROP && depth == 1) {
            uint32_t bmax;
            const uint8_t *bn = fstr(b, prop_name_off(b, p), &bmax);
            uint32_t k = 0;
            if (!bn || !sn)
                continue;
            while (k < bmax && k < smax && bn[k] == sn[k] && bn[k])
                ++k;
            if (k < bmax && k < smax && !bn[k] && !sn[k])
                return 1;
        }
    }
    return 0;
}

static void emit_root_extras(struct mg *m, const struct sbinfo *si)
{
    for (uint32_t i = 0; i < si->nroot; ++i)
        if (!base_root_has(m->base, m->sb, si->root[i]))
            emit_sb_prop(m, si->root[i], 0);
}

static void emit_sb_chosen_props(struct mg *m, const struct sbinfo *si)
{
    const struct fdt *s = m->sb;
    for (uint32_t p = si->chosen0, l; p < si->chosen1; p += l) {
        l = tok_len(s, p);
        if (!l) {
            m->bad = 1;
            return;
        }
        if (be(s->b + p) == T_PROP)
            emit_sb_prop(m, p, 1);
    }
}

static void emit_sb_node(struct mg *m, uint32_t from, uint32_t to)
{
    const struct fdt *s = m->sb;
    for (uint32_t p = from, l; p < to && !m->bad; p += l) {
        l = tok_len(s, p);
        if (!l) {
            m->bad = 1;
            return;
        }
        uint32_t t = be(s->b + p);
        if (t == T_PROP)
            emit_sb_prop(m, p, 0);
        else if (t != T_NOP)
            emit(m, s->b + p, l);
    }
}

static struct mg mg;   /* 1 KiB of strings: static, the loader's stack is small */

uint32_t fdt_merge_sboot(const uint8_t *base, const uint8_t *sboot, uint8_t *out, uint32_t cap,
                         const char *extra)
{
    struct fdt B, S;
    struct sbinfo si;
    if (!fdt_open(base, FDT_MAX, &B) || !fdt_open(sboot, FDT_MAX, &S) || !sb_scan(&S, &si))
        return 0;
    struct mg *m = &mg;
    m->o = out;
    m->w = 0;
    m->cap = cap;
    m->bad = 0;
    m->base = &B;
    m->sb = &S;
    m->xn = 0;
    m->extra = extra;
    static const uint8_t hdr0[40] = { 0 };
    emit(m, hdr0, 40);
    /* memory reservation map: base's, through its terminating 0/0 pair */
    for (uint32_t p = B.rsv;; p += 16) {
        if (p + 16 > B.total) {
            m->bad = 1;
            break;
        }
        emit(m, base + p, 16);
        if (!be(base + p) && !be(base + p + 4) && !be(base + p + 8) && !be(base + p + 12))
            break;
    }
    uint32_t ostruct = m->w;
    unsigned depth = 0;
    int extras_done = 0, chosen_done = 0, in_chosen = 0, ended = 0;
    unsigned skip = 0;             /* depth of a memory node being dropped, 0 = none */
    for (uint32_t p = B.ostruct, l; p < B.send && !m->bad && !ended; p += l) {
        l = tok_len(&B, p);
        if (!l) {
            m->bad = 1;
            break;
        }
        uint32_t t = be(base + p);
        if (skip) {
            if (t == T_BEGIN)
                ++depth;
            else if (t == T_END && depth-- == skip)
                skip = 0;
            continue;
        }
        switch (t) {
        case T_BEGIN:
            ++depth;
            if (depth == 2 && !extras_done) {
                emit_root_extras(m, &si);
                extras_done = 1;
            }
            if (depth == 2 && node_is_memory(&B, p)) {
                skip = depth;
                break;
            }
            emit(m, base + p, l);
            if (depth == 2 && node_is(&B, p, "chosen") && si.has_chosen && !chosen_done) {
                emit_sb_chosen_props(m, &si);
                chosen_done = 1;
                in_chosen = 1;
            }
            break;
        case T_PROP:
            if (!(in_chosen && depth == 2))
                emit(m, base + p, l);
            break;
        case T_END:
            if (!depth) {
                m->bad = 1;
                break;
            }
            if (depth == 2)
                in_chosen = 0;
            if (depth == 1) {
                if (!extras_done) {
                    emit_root_extras(m, &si);
                    extras_done = 1;
                }
                if (!chosen_done && si.has_chosen) {
                    static const uint8_t chosen[12] = { 0, 0, 0, 1, 'c', 'h', 'o', 's', 'e', 'n', 0, 0 };
                    emit(m, chosen, 12);
                    emit_sb_chosen_props(m, &si);
                    emit_u32(m, T_END);
                    chosen_done = 1;
                }
                for (uint32_t i = 0; i < si.nmem; ++i)
                    emit_sb_node(m, si.mem0[i], si.mem1[i]);
            }
            emit(m, base + p, l);
            --depth;
            break;
        case T_NOP:
            break;
        default:   /* T_FEND */
            emit(m, base + p, l);
            ended = 1;
            break;
        }
    }
    if (!ended || depth)
        m->bad = 1;
    uint32_t sstruct = m->w - ostruct, ostr = m->w;
    emit(m, base + B.ostr, B.ssize);
    emit(m, (const uint8_t *)m->xs, m->xn);
    if (m->bad)
        return 0;
    uint32_t total = m->w;
    wbe(out + 0, FDT_MAGIC);
    wbe(out + 4, total);
    wbe(out + 8, ostruct);
    wbe(out + 12, ostr);
    wbe(out + 16, 40);
    wbe(out + 20, 17);
    wbe(out + 24, 16);
    wbe(out + 28, be(base + 28));
    wbe(out + 32, B.ssize + m->xn);
    wbe(out + 36, sstruct);
    return total;
}

int fdt_patch_pairs(uint8_t *fdtb, const char *prop, uint32_t key, uint32_t from, uint32_t to)
{
    struct fdt f;
    int n = 0;
    if (!fdt_open(fdtb, FDT_MAX, &f))
        return 0;
    for (uint32_t p = f.ostruct, l; p < f.send; p += l) {
        l = tok_len(&f, p);
        if (!l)
            break;
        uint32_t t = be(fdtb + p);
        if (t == T_FEND)
            break;
        if (t != T_PROP)
            continue;
        uint32_t len = be(fdtb + p + 4), max;
        const uint8_t *nm = fstr(&f, prop_name_off(&f, p), &max);
        if (!str_eq(nm, max, prop))
            continue;
        uint8_t *v = fdtb + p + 12;
        for (uint32_t i = 0; i + 8 <= len; i += 8)
            if (be(v + i) == key && be(v + i + 4) == from) {
                wbe(v + i + 4, to);
                ++n;
            }
    }
    return n;
}
