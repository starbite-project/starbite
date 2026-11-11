/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite firmware layer: read-only FAT32 / NTFS / BCD parsers (sbfwfs.h). No hardware, no libc;
 * static buffers (the loader's stack is small), so not reentrant: one check at a time.
 */
#include "sbfwfs.h"

/* ---- strings ---- */

void fwb_init(struct fwb *s)
{
    s->n = 0;
    s->b[0] = 0;
}
void fwb_s(struct fwb *s, const char *t)
{
    while (*t && s->n + 1 < sizeof(s->b))
        s->b[s->n++] = *t++;
    s->b[s->n] = 0;
}
void fwb_u(struct fwb *s, uint64_t v)
{
    char d[24];
    int k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < 23);
    while (k && s->n + 1 < sizeof(s->b))
        s->b[s->n++] = d[--k];
    s->b[s->n] = 0;
}
void fwb_x(struct fwb *s, uint64_t v, unsigned digits)
{
    fwb_s(s, "0x");
    for (int i = (int)digits - 1; i >= 0 && s->n + 1 < sizeof(s->b); --i)
        s->b[s->n++] = "0123456789abcdef"[(v >> (4 * i)) & 15U];
    s->b[s->n] = 0;
}
void fwb_mb(struct fwb *s, uint64_t bytes)
{
    uint64_t mb = bytes >> 20;
    if (mb >= 10240U) {
        fwb_u(s, mb >> 10);
        fwb_s(s, " GiB");
    } else {
        fwb_u(s, mb);
        fwb_s(s, " MiB");
    }
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static void say(struct fw_out *o, const char *s)
{
    if (o && o->line)
        o->line(o->u, s);
}
static void say2(struct fw_out *o, const char *a, const char *b)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, a);
    fwb_s(&s, b);
    say(o, s.b);
}
static void bad(struct fw_out *o, const char *s)
{
    if (o)
        o->bad++;
    say2(o, "PROBLEM: ", s);
}

/* ================================================================================================
 * FAT32
 * ============================================================================================== */

static uint8_t fbuf[4096];

int fat32_mount(struct fat32 *f, const struct fw_io *io)
{
    f->io = *io;
    if (io->rd(io->u, 0, fbuf, 512))
        return 0;
    if (fbuf[510] != 0x55 || fbuf[511] != 0xAA)
        return 0;
    const char *t = "FAT32   ";
    for (int i = 0; i < 8; i++)
        if (fbuf[82 + i] != (uint8_t)t[i])
            return 0;
    f->bps = le16(fbuf + 11);
    f->spc = fbuf[13];
    f->rsvd = le16(fbuf + 14);
    f->nfats = fbuf[16];
    f->fatsz = le32(fbuf + 36);
    f->root = le32(fbuf + 44);
    f->fsinfo = le16(fbuf + 48);
    uint32_t tot = le32(fbuf + 32);
    if ((f->bps != 512 && f->bps != 1024 && f->bps != 2048 && f->bps != 4096) || !f->spc || (f->spc & (f->spc - 1)) ||
        !f->nfats || f->nfats > 2 || !f->fatsz || f->root < 2)
        return 0;
    f->clus_bytes = (uint64_t)f->bps * f->spc;
    f->data_off = ((uint64_t)f->rsvd + (uint64_t)f->nfats * f->fatsz) * f->bps;
    uint64_t data_sec = tot > f->rsvd + f->nfats * f->fatsz ? tot - f->rsvd - f->nfats * f->fatsz : 0;
    f->total_clus = (uint32_t)(data_sec / f->spc);
    if (f->data_off + (uint64_t)f->total_clus * f->clus_bytes > io->size + f->clus_bytes)
        return 0;
    return 1;
}

static int fat_ent(const struct fat32 *f, uint32_t n, uint32_t *v)
{
    uint8_t b[4];
    if (n >= f->total_clus + 2)
        return 0;
    if (f->io.rd(f->io.u, (uint64_t)f->rsvd * f->bps + (uint64_t)n * 4U, b, 4))
        return 0;
    *v = le32(b) & 0x0FFFFFFFU;
    return 1;
}

void fat32_health(const struct fat32 *f, struct fw_out *o)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "FAT32: ");
    fwb_u(&s, f->bps);
    fwb_s(&s, " B sectors, ");
    fwb_u(&s, f->clus_bytes);
    fwb_s(&s, " B clusters, ");
    fwb_mb(&s, (uint64_t)f->total_clus * f->clus_bytes);
    say(o, s.b);
    uint8_t b[4];
    if (!f->io.rd(f->io.u, (uint64_t)f->rsvd * f->bps + 4, b, 4)) {
        uint32_t e1 = le32(b);
        say(o, (e1 & 0x08000000U) ? "FAT32: clean shutdown bit set (unmounted cleanly)"
                                  : "FAT32: clean shutdown bit CLEAR (not unmounted cleanly)");
        if (!(e1 & 0x04000000U))
            bad(o, "FAT32 hard-error bit set (a disk error was seen)");
        else
            say(o, "FAT32: no hard-error bit");
    }
    if (f->nfats == 2 && f->fatsz) {
        /* the first sector of both FAT copies should agree */
        static uint8_t b2[4096];
        if (f->bps <= sizeof(b2) && !f->io.rd(f->io.u, (uint64_t)f->rsvd * f->bps, fbuf, f->bps) &&
            !f->io.rd(f->io.u, ((uint64_t)f->rsvd + f->fatsz) * f->bps, b2, f->bps)) {
            int same = 1;
            for (uint32_t i = 0; i < f->bps; i++)
                same &= fbuf[i] == b2[i];
            if (same)
                say(o, "FAT32: both FAT copies agree (first sector)");
            else
                bad(o, "FAT32 copies differ in their first sector");
        }
    }
    if (f->fsinfo && !f->io.rd(f->io.u, (uint64_t)f->fsinfo * f->bps, fbuf, 512) && le32(fbuf) == 0x41615252U &&
        le32(fbuf + 484) == 0x61417272U) {
        uint32_t fr = le32(fbuf + 488);
        fwb_init(&s);
        fwb_s(&s, "FAT32: free ");
        if (fr == 0xFFFFFFFFU)
            fwb_s(&s, "unknown");
        else
            fwb_mb(&s, (uint64_t)fr * f->clus_bytes);
        say(o, s.b);
    }
}

/* walk a cluster chain: the partition offset of the chain's byte off */
int fat32_map(const struct fat32 *f, uint32_t clus, uint64_t off, uint64_t *poff)
{
    uint32_t c = clus;
    for (uint64_t skip = off / f->clus_bytes, i = 0; i < skip; i++) {
        uint32_t n;
        if (!fat_ent(f, c, &n) || n < 2 || n >= 0x0FFFFFF8U)
            return 0;
        c = n;
        if (i > f->total_clus)
            return 0;
    }
    if (c < 2 || c >= f->total_clus + 2)
        return 0;
    *poff = f->data_off + (uint64_t)(c - 2) * f->clus_bytes + off % f->clus_bytes;
    return 1;
}

/* one directory entry name compare: 8.3 (padded) or the assembled long name */
static int name_eq(const char *want, unsigned wn, const uint8_t *e, const char *lfn)
{
    if (lfn[0]) {
        unsigned i = 0;
        for (; i < wn && lfn[i]; i++)
            if (up(lfn[i]) != up(want[i]))
                break;
        if (i == wn && !lfn[i])
            return 1;
    }
    char sn[13];
    unsigned n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++)
        sn[n++] = (char)e[i];
    if (e[8] != ' ') {
        sn[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            sn[n++] = (char)e[i];
    }
    if (n != wn)
        return 0;
    for (unsigned i = 0; i < n; i++)
        if (up(sn[i]) != up(want[i]))
            return 0;
    return 1;
}

static int dir_find(const struct fat32 *f, uint32_t dclus, const char *want, unsigned wn, uint32_t *clus, uint32_t *size,
                    int *isdir)
{
    char lfn[256];
    lfn[0] = 0;
    uint32_t c = dclus;
    for (uint32_t hops = 0; c >= 2 && c < 0x0FFFFFF8U && hops <= f->total_clus; hops++) {
        uint64_t base = f->data_off + (uint64_t)(c - 2) * f->clus_bytes;
        for (uint64_t o = 0; o < f->clus_bytes; o += 32) {
            uint8_t e[32];
            if (f->io.rd(f->io.u, base + o, e, 32))
                return 0;
            if (e[0] == 0)
                return 0;
            if (e[0] == 0xE5) {
                lfn[0] = 0;
                continue;
            }
            if (e[11] == 0x0F) {             /* a long-name piece: 13 UTF-16 chars, ASCII kept */
                unsigned seq = (e[0] & 0x1FU);
                if (!seq || seq > 19)
                    continue;
                static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                if (e[0] & 0x40)
                    for (unsigned i = 0; i < 256; i++)
                        lfn[i] = 0;
                for (unsigned i = 0; i < 13; i++) {
                    uint32_t ch = le16(e + at[i]);
                    unsigned pos = (seq - 1) * 13 + i;
                    if (pos < 255)
                        lfn[pos] = ch == 0 || ch == 0xFFFF ? 0 : (ch < 128 ? (char)ch : '?');
                }
                continue;
            }
            if (e[11] & 0x08) {              /* volume label */
                lfn[0] = 0;
                continue;
            }
            int hit = name_eq(want, wn, e, lfn);
            lfn[0] = 0;
            if (hit) {
                *clus = le16(e + 20) << 16 | le16(e + 26);
                *size = le32(e + 28);
                *isdir = (e[11] & 0x10) != 0;
                return 1;
            }
        }
        uint32_t n;
        if (!fat_ent(f, c, &n))
            return 0;
        c = n;
    }
    return 0;
}

int fat32_find(const struct fat32 *f, const char *path, uint32_t *clus, uint32_t *size)
{
    uint32_t c = f->root, sz = 0;
    int isdir = 1;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *q = p;
        while (*q && *q != '/' && *q != '\\')
            q++;
        if (!isdir || !dir_find(f, c, p, (unsigned)(q - p), &c, &sz, &isdir))
            return 0;
        p = q;
    }
    if (isdir)
        return 0;
    *clus = c;
    *size = sz;
    return 1;
}

int64_t fat32_read(const struct fat32 *f, uint32_t clus, uint32_t size, uint8_t *buf, uint32_t cap)
{
    if (size > cap)
        return -1;
    uint32_t c = clus, done = 0;
    for (uint32_t hops = 0; done < size; hops++) {
        if (c < 2 || c >= f->total_clus + 2 || hops > f->total_clus)
            return -1;
        uint64_t base = f->data_off + (uint64_t)(c - 2) * f->clus_bytes;
        uint32_t n = size - done < f->clus_bytes ? size - done : (uint32_t)f->clus_bytes;
        if (f->io.rd(f->io.u, base, buf + done, n))
            return -1;
        done += n;
        if (done < size) {
            uint32_t nx;
            if (!fat_ent(f, c, &nx))
                return -1;
            c = nx;
        }
    }
    return done;
}

/* ================================================================================================
 * NTFS
 * ============================================================================================== */

static uint8_t mrec[4096], mrec2[4096];

/* the update sequence array put back: 1 when every sector's tail matched */
static int fixup(uint8_t *r, uint32_t rs, uint32_t bps)
{
    uint32_t uo = le16(r + 4), un = le16(r + 6);
    if (!un || uo + 2U * un > rs || (un - 1) * bps != rs)
        return 0;
    for (uint32_t i = 1; i < un; i++) {
        uint8_t *t = r + i * bps - 2;
        if (t[0] != r[uo] || t[1] != r[uo + 1])
            return 0;
        t[0] = r[uo + 2 * i];
        t[1] = r[uo + 2 * i + 1];
    }
    return 1;
}

void ntfs_health(const struct fw_io *io, struct fw_out *o)
{
    uint8_t b[512];
    struct fwb s;
    if (io->rd(io->u, 0, b, 512)) {
        bad(o, "NTFS: the boot sector could not be read");
        return;
    }
    const char *t = "NTFS    ";
    for (int i = 0; i < 8; i++)
        if (b[3 + i] != (uint8_t)t[i]) {
            bad(o, "NTFS: no NTFS boot sector (OEM id)");
            return;
        }
    uint32_t bps = le16(b + 11), spc = b[13];
    if (spc > 0x80)
        spc = 1U << (256U - spc);
    int8_t cpr = (int8_t)b[64];
    uint64_t cl = (uint64_t)bps * spc, mft = le64(b + 48), mirr = le64(b + 56), sect = le64(b + 40);
    uint32_t rs = cpr < 0 ? 1U << (uint32_t)(-cpr) : (uint32_t)cpr * (uint32_t)cl;
    if (b[510] != 0x55 || b[511] != 0xAA || !bps || bps > 4096 || !cl || rs < 512 || rs > sizeof(mrec) || rs % bps) {
        bad(o, "NTFS: boot sector values out of range");
        return;
    }
    fwb_init(&s);
    fwb_s(&s, "NTFS: ");
    fwb_mb(&s, sect * bps);
    fwb_s(&s, ", ");
    fwb_u(&s, cl);
    fwb_s(&s, " B clusters, ");
    fwb_u(&s, rs);
    fwb_s(&s, " B MFT records");
    say(o, s.b);
    if (sect * bps > io->size + bps)
        bad(o, "NTFS: the volume is larger than its partition");
    /* $MFT records 0..3 against $MFTMirr (raw: both are written together) */
    int mirror_ok = 1;
    for (uint32_t n = 0; n < 4; n++) {
        if (io->rd(io->u, mft * cl + n * rs, mrec, rs) || io->rd(io->u, mirr * cl + n * rs, mrec2, rs)) {
            bad(o, "NTFS: $MFT / $MFTMirr unreadable");
            return;
        }
        if (mrec[0] != 'F' || mrec[1] != 'I' || mrec[2] != 'L' || mrec[3] != 'E') {
            bad(o, "NTFS: an $MFT system record has no FILE signature");
            mirror_ok = 0;
        }
        for (uint32_t i = 0; i < rs; i++)
            if (mrec[i] != mrec2[i]) {
                mirror_ok = 0;
                break;
            }
    }
    say(o, mirror_ok ? "NTFS: $MFT records 0-3 match $MFTMirr" : "NTFS: $MFT and $MFTMirr differ (chkdsk would compare them)");
    /* $Volume (record 3): version, flags, label */
    if (io->rd(io->u, mft * cl + 3 * rs, mrec, rs) || !fixup(mrec, rs, bps)) {
        bad(o, "NTFS: $Volume record unreadable or torn (update sequence)");
        return;
    }
    uint32_t a = le16(mrec + 20);
    int got = 0;
    char label[48];
    label[0] = 0;
    for (int guard = 0; a + 16 <= rs && guard < 64; guard++) {
        uint32_t ty = le32(mrec + a), al = le32(mrec + a + 4);
        if (ty == 0xFFFFFFFFU || al < 16 || a + al > rs)
            break;
        if (!mrec[a + 8]) {                    /* resident */
            uint32_t vl = le32(mrec + a + 16), vo = le16(mrec + a + 20);
            const uint8_t *v = mrec + a + vo;
            if (a + vo + vl <= rs) {
                if (ty == 0x70 && vl >= 12) {
                    got = 1;
                    uint32_t fl = le16(v + 10);
                    fwb_init(&s);
                    fwb_s(&s, "NTFS: version ");
                    fwb_u(&s, v[8]);
                    fwb_s(&s, ".");
                    fwb_u(&s, v[9]);
                    fwb_s(&s, ", flags ");
                    fwb_x(&s, fl, 4);
                    say(o, s.b);
                    if (fl & 0x0001U)
                        bad(o, "NTFS volume is DIRTY (chkdsk will run; or Windows did not shut down cleanly)");
                    else
                        say(o, "NTFS: clean (the dirty flag is clear)");
                    if (fl & 0x0004U)
                        say(o, "NTFS: upgrade on mount requested");
                    if (fl & 0x8000U)
                        say(o, "NTFS: chkdsk underway / modified by chkdsk");
                } else if (ty == 0x60) {
                    unsigned n = 0;
                    for (uint32_t i = 0; i + 1 < vl && n + 1 < sizeof(label); i += 2) {
                        uint32_t ch = le16(v + i);
                        label[n++] = ch < 128 ? (char)ch : '?';
                    }
                    label[n] = 0;
                }
            }
        }
        a += al;
    }
    if (label[0])
        say2(o, "NTFS: label ", label);
    if (!got)
        bad(o, "NTFS: $Volume has no volume information");
}

/* ================================================================================================
 * BCD (registry hive)
 * ============================================================================================== */

static const uint8_t *H;
static uint32_t HL;

/* a cell's body (after the size), bounds-checked: 0 if not inside the hive */
static const uint8_t *cellp(uint32_t off, uint32_t need)
{
    uint64_t o = 4096ULL + off + 4ULL;
    if (off == 0xFFFFFFFFU || o + need > HL)
        return 0;
    return H + o;
}

static int nk_name_is(const uint8_t *nk, const char *want)
{
    uint32_t fl = le16(nk + 2), nl = le16(nk + 0x48);
    const uint8_t *nm = nk + 0x4c;
    unsigned wn = 0;
    while (want[wn])
        wn++;
    if (fl & 0x20) {                 /* ASCII */
        if (nl != wn)
            return 0;
        for (unsigned i = 0; i < wn; i++)
            if (up((char)nm[i]) != up(want[i]))
                return 0;
        return 1;
    }
    if (nl != 2 * wn)
        return 0;
    for (unsigned i = 0; i < wn; i++)
        if (nm[2 * i + 1] || up((char)nm[2 * i]) != up(want[i]))
            return 0;
    return 1;
}

static uint32_t subkey(uint32_t nkoff, const char *want, int depth);
static uint32_t list_find(uint32_t lo, const char *want, int depth)
{
    const uint8_t *l = cellp(lo, 4);
    if (!l || depth > 4)
        return 0xFFFFFFFFU;
    uint32_t n = le16(l + 2);
    int ri = l[0] == 'r' && l[1] == 'i', li = l[0] == 'l' && l[1] == 'i';
    int lf = (l[0] == 'l' && (l[1] == 'f' || l[1] == 'h'));
    if (!ri && !li && !lf)
        return 0xFFFFFFFFU;
    uint32_t step = lf ? 8 : 4;
    if (!cellp(lo, 4 + n * step))
        return 0xFFFFFFFFU;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = le32(l + 4 + i * step);
        if (ri) {
            uint32_t r = list_find(k, want, depth + 1);
            if (r != 0xFFFFFFFFU)
                return r;
            continue;
        }
        const uint8_t *nk = cellp(k, 0x4c);
        if (nk && nk[0] == 'n' && nk[1] == 'k' && cellp(k, 0x4c + le16(nk + 0x48)) && nk_name_is(nk, want))
            return k;
    }
    return 0xFFFFFFFFU;
}
static uint32_t subkey(uint32_t nkoff, const char *want, int depth)
{
    const uint8_t *nk = cellp(nkoff, 0x4c);
    if (!nk || nk[0] != 'n' || nk[1] != 'k' || !le32(nk + 0x14))
        return 0xFFFFFFFFU;
    return list_find(le32(nk + 0x1c), want, depth);
}
static uint32_t path(uint32_t root, const char *const *parts, unsigned n)
{
    uint32_t k = root;
    for (unsigned i = 0; i < n && k != 0xFFFFFFFFU; i++)
        k = subkey(k, parts[i], 0);
    return k;
}
/* the value "Element" of key k: type, data pointer, length, and the data's hive file offset */
static int element(uint32_t k, uint32_t *type, const uint8_t **data, uint32_t *len, uint32_t *foff)
{
    const uint8_t *nk = cellp(k, 0x4c);
    if (!nk)
        return 0;
    uint32_t nv = le32(nk + 0x24), vl = le32(nk + 0x28);
    const uint8_t *lst = nv ? cellp(vl, 4 * nv) : 0;
    for (uint32_t i = 0; lst && i < nv && i < 64; i++) {
        uint32_t vo = le32(lst + 4 * i);
        const uint8_t *vk = cellp(vo, 0x14);
        if (!vk || vk[0] != 'v' || vk[1] != 'k')
            continue;
        uint32_t nl = le16(vk + 2), ds = le32(vk + 4), dof = le32(vk + 8);
        if (!cellp(vo, 0x14 + nl))
            continue;
        const uint8_t *nm = vk + 0x14;
        const char *w = "Element";
        int eq = nl == 7;
        for (unsigned j = 0; eq && j < 7; j++)
            eq = up((char)nm[j]) == up(w[j]);
        if (!eq)
            continue;
        *type = le32(vk + 0x0c);
        if (ds & 0x80000000U) {      /* inline in the offset field */
            *len = ds & 0x7fffffffU;
            *data = vk + 8;
            *foff = (uint32_t)(vk + 8 - H);
        } else {
            *len = ds;
            *data = cellp(dof, ds);
            if (!*data || ds > 16384)
                return 0;
            *foff = 4096U + dof + 4U;
        }
        return 1;
    }
    return 0;
}
/* a GUID string from UTF-16 data ("{...}", 38 chars): 1 ok */
static int guid_of(const uint8_t *d, uint32_t len, char out[40])
{
    if (len < 76)
        return 0;
    for (unsigned i = 0; i < 38; i++) {
        uint32_t ch = le16(d + 2 * i);
        if (ch >= 128)
            return 0;
        out[i] = (char)ch;
    }
    out[38] = 0;
    return out[0] == '{' && out[37] == '}' && out[9] == '-' && out[14] == '-' && out[19] == '-' && out[24] == '-';
}
static void describe(uint32_t objs, const char *guid, char out[40])
{
    const char *el[2] = { "Elements", "12000004" };
    uint32_t k = subkey(objs, guid, 0), ty = 0, len = 0, fo = 0;
    const uint8_t *d = 0;
    out[0] = 0;
    if (k == 0xFFFFFFFFU || (k = path(k, el, 2)) == 0xFFFFFFFFU || !element(k, &ty, &d, &len, &fo) || ty != 1)
        return;
    unsigned n = 0;
    for (uint32_t i = 0; i + 1 < len && n + 1 < 40; i += 2) {
        uint32_t ch = le16(d + i);
        if (!ch)
            break;
        out[n++] = ch < 128 ? (char)ch : '?';
    }
    out[n] = 0;
}

void bcd_check(const uint8_t *h, uint32_t len, struct bcd_plan *plan, struct fw_out *o)
{
    struct fwb s;
    plan->ok = 0;
    plan->cur[0] = plan->next[0] = plan->cur_name[0] = plan->next_name[0] = 0;
    H = h;
    HL = len;
    if (len < 8192 || h[0] != 'r' || h[1] != 'e' || h[2] != 'g' || h[3] != 'f') {
        bad(o, "BCD: not a registry hive");
        return;
    }
    uint32_t x = 0;
    for (unsigned i = 0; i < 127; i++)
        x ^= le32(h + 4 * i);
    if (x != le32(h + 508))
        bad(o, "BCD: header checksum wrong");
    uint32_t s1 = le32(h + 4), s2 = le32(h + 8);
    fwb_init(&s);
    fwb_s(&s, "BCD: hive sequence ");
    fwb_u(&s, s1);
    fwb_s(&s, " / ");
    fwb_u(&s, s2);
    fwb_s(&s, s1 == s2 ? " (clean)" : " (DIRTY: Windows has a log to replay)");
    say(o, s.b);
    int clean = s1 == s2;
    uint32_t root = le32(h + 0x24);
    const char *objp[1] = { "Objects" };
    uint32_t objs = path(root, objp, 1);
    if (objs == 0xFFFFFFFFU) {
        bad(o, "BCD: no Objects key");
        return;
    }
    const char *dp[3] = { "{9dea862c-5cdd-4e70-acc1-f32b344d4795}", "Elements", "23000003" };
    uint32_t dk = path(objs, dp, 3), ty = 0, dl = 0, doff = 0;
    const uint8_t *dd = 0;
    if (dk == 0xFFFFFFFFU || !element(dk, &ty, &dd, &dl, &doff) || ty != 1 || !guid_of(dd, dl, plan->cur)) {
        bad(o, "BCD: the boot manager has no default entry");
        return;
    }
    describe(objs, plan->cur, plan->cur_name);
    say2(o, "BCD: default ", plan->cur);
    if (plan->cur_name[0])
        say2(o, "BCD:   = ", plan->cur_name);
    const char *rp[2] = { "Elements", "14000008" };
    uint32_t wk = subkey(objs, plan->cur, 0), rk = wk == 0xFFFFFFFFU ? wk : path(wk, rp, 2);
    const uint8_t *rd = 0;
    uint32_t rl = 0, rty = 0, roff = 0;
    if (rk == 0xFFFFFFFFU || !element(rk, &rty, &rd, &rl, &roff) || rty != 7 || !guid_of(rd, rl, plan->next)) {
        say(o, "BCD: the default entry has no recovery sequence (no WinRE linked)");
        return;
    }
    describe(objs, plan->next, plan->next_name);
    say2(o, "BCD: recovery sequence ", plan->next);
    if (plan->next_name[0])
        say2(o, "BCD:   = ", plan->next_name);
    const char *wp[2] = { "Elements", "26000022" };
    uint32_t nk2 = subkey(objs, plan->next, 0);
    if (nk2 == 0xFFFFFFFFU) {
        bad(o, "BCD: the recovery entry is missing");
        return;
    }
    say(o, path(nk2, wp, 2) != 0xFFFFFFFFU ? "BCD: the recovery entry is a WinPE boot (WinRE)"
                                           : "BCD: the recovery entry is not marked WinPE");
    plan->off = doff;
    plan->len = 76;
    if (!clean) {
        bad(o, "BCD: dirty hive: no in-place change until Windows has written it back");
        return;
    }
    plan->ok = 1;
    fwb_init(&s);
    fwb_s(&s, "BCD: Advanced Startup plan: hive bytes ");
    fwb_x(&s, doff, 5);
    fwb_s(&s, "..+76, default ");
    fwb_s(&s, plan->cur);
    say(o, s.b);
    say2(o, "BCD:   -> ", plan->next);
}
