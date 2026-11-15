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

/* ================================================================================================
 * ext4 / HFS+ / APFS health, and the sniffing dispatcher
 * ============================================================================================== */

static uint8_t gbuf[4096];       /* block-sized scratch (checksums stream through it) */
static uint8_t sbk[1408];        /* a superblock / volume header / container block 0 head */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | (uint32_t)p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

/* a bounded read: never past the partition's end when its size is known */
static int rdk(const struct fw_io *io, uint64_t off, void *buf, uint32_t len)
{
    if (!io || !io->rd)
        return -1;
    if (io->size && (off > io->size || (uint64_t)len > io->size - off))
        return -1;
    return io->rd(io->u, off, buf, len);
}

static void fwb_c(struct fwb *s, char c)
{
    char t[2];
    t[0] = c;
    t[1] = 0;
    fwb_s(s, t);
}
static void fwb_2(struct fwb *s, uint64_t v)
{
    fwb_c(s, (char)('0' + v / 10U % 10U));
    fwb_c(s, (char)('0' + v % 10U));
}
/* Unix seconds as "YYYY-MM-DD hh:mm UTC" (days-from-civil inverse, integers only) */
static void fwb_time(struct fwb *s, uint64_t t)
{
    if (!t) {
        fwb_s(s, "never");
        return;
    }
    uint64_t z = t / 86400U + 719468U, rem = t % 86400U;
    uint64_t era = z / 146097U, doe = z - era * 146097U;
    uint64_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    uint64_t y = yoe + era * 400U, doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    uint64_t mp = (5U * doy + 2U) / 153U, d = doy - (153U * mp + 2U) / 5U + 1U;
    uint64_t m = mp < 10U ? mp + 3U : mp - 9U;
    if (m <= 2U)
        y++;
    fwb_u(s, y);
    fwb_c(s, '-');
    fwb_2(s, m);
    fwb_c(s, '-');
    fwb_2(s, d);
    fwb_c(s, ' ');
    fwb_2(s, rem / 3600U);
    fwb_c(s, ':');
    fwb_2(s, rem / 60U % 60U);
    fwb_s(s, " UTC");
}
/* n bytes as text, non-printable as '?', stops at NUL */
static void fwb_txt(struct fwb *s, const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n && p[i]; i++)
        fwb_c(s, p[i] >= 32 && p[i] < 127 ? (char)p[i] : '?');
}
static void fwb_hex8(struct fwb *s, uint32_t v)
{
    fwb_c(s, "0123456789abcdef"[(v >> 4) & 15U]);
    fwb_c(s, "0123456789abcdef"[v & 15U]);
}
static void fwb_uuid(struct fwb *s, const uint8_t *u)
{
    for (unsigned i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            fwb_c(s, '-');
        fwb_hex8(s, u[i]);
    }
}
static void say_ut(struct fw_out *o, const char *a, uint64_t t)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, a);
    fwb_time(&s, t);
    say(o, s.b);
}

/* ---- ext4 ---- */

/* CRC-32C (Castagnoli), bitwise, no final inversion (ext4's ext4_chksum(~0, ...) convention) */
static uint32_t crc32c_raw(uint32_t c, const uint8_t *p, uint32_t n)
{
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82F63B78U & (0U - (c & 1U)));
    }
    return c;
}

static void ext4_err(struct fw_out *o, const uint8_t *sb, const char *which, uint32_t t, uint32_t fn, uint32_t ln,
                     uint32_t ino, uint64_t blk)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "ext4: ");
    fwb_s(&s, which);
    fwb_s(&s, " error ");
    fwb_time(&s, t);
    if (sb[fn]) {
        fwb_s(&s, " in ");
        fwb_txt(&s, sb + fn, 32);
        fwb_s(&s, ":");
        fwb_u(&s, ln);
    }
    if (ino) {
        fwb_s(&s, ", inode ");
        fwb_u(&s, ino);
    }
    if (blk) {
        fwb_s(&s, ", block ");
        fwb_u(&s, blk);
    }
    say(o, s.b);
}

void ext4_health(const struct fw_io *io, struct fw_out *o)
{
    uint8_t *sb = sbk;
    struct fwb s;
    if (rdk(io, 1024, sb, 1024)) {
        bad(o, "ext4: the superblock could not be read");
        return;
    }
    if (le16(sb + 0x38) != 0xEF53U) {
        bad(o, "ext4: no superblock magic (0xEF53 at byte 1080)");
        return;
    }
    uint32_t lg = le32(sb + 0x18), comp = le32(sb + 0x5C), inc = le32(sb + 0x60), ro = le32(sb + 0x64);
    if (lg > 6) {
        bad(o, "ext4: block size out of range");
        return;
    }
    uint64_t bs = 1024ULL << lg, blocks = le32(sb + 4), freeb = le32(sb + 0x0C);
    if (inc & 0x80U) {
        blocks |= (uint64_t)le32(sb + 0x150) << 32;
        freeb |= (uint64_t)le32(sb + 0x158) << 32;
    }
    fwb_init(&s);
    fwb_s(&s, "ext4: ");
    fwb_u(&s, bs);
    fwb_s(&s, " B blocks, ");
    fwb_u(&s, blocks);
    fwb_s(&s, " blocks (");
    fwb_mb(&s, blocks < (1ULL << 47) ? blocks * bs : ~0ULL);
    fwb_s(&s, "), free ");
    fwb_u(&s, freeb);
    fwb_s(&s, " (");
    fwb_mb(&s, freeb < (1ULL << 47) ? freeb * bs : ~0ULL);
    fwb_s(&s, ")");
    say(o, s.b);
    fwb_init(&s);
    fwb_s(&s, "ext4: inodes ");
    fwb_u(&s, le32(sb));
    fwb_s(&s, ", free ");
    fwb_u(&s, le32(sb + 0x10));
    fwb_s(&s, ", revision ");
    fwb_u(&s, le32(sb + 0x4C));
    say(o, s.b);
    if (freeb > blocks || le32(sb + 0x10) > le32(sb))
        bad(o, "ext4: more free blocks or inodes than exist");
    if (io->size && blocks > io->size / bs)
        bad(o, "ext4: the volume is larger than its partition");
    fwb_init(&s);
    fwb_s(&s, "ext4: label '");
    fwb_txt(&s, sb + 0x78, 16);
    fwb_s(&s, "', UUID ");
    fwb_uuid(&s, sb + 0x68);
    say(o, s.b);

    uint32_t st = le16(sb + 0x3A), eb = le16(sb + 0x3C);
    if (st & 2U)
        bad(o, "ext4: state ERRORS (the kernel saw file-system errors; fsck needed)");
    else if (st & 1U)
        say(o, "ext4: state clean");
    else
        say(o, "ext4: state not clean (mounted now, or not unmounted cleanly)");
    if (st & 4U)
        say(o, "ext4: orphan inodes are being recovered");
    say(o, eb == 1 ? "ext4: on errors: continue"
           : eb == 2 ? "ext4: on errors: remount read-only"
           : eb == 3 ? "ext4: on errors: panic"
                     : "ext4: on errors: (unknown setting)");
    uint32_t ec = le32(sb + 0x194);
    if (ec) {
        fwb_init(&s);
        fwb_s(&s, "ext4: ");
        fwb_u(&s, ec);
        fwb_s(&s, " error(s) recorded since the last fsck");
        bad(o, s.b);
    }
    if (le32(sb + 0x198))
        ext4_err(o, sb, "first", le32(sb + 0x198), 0x1A8, le32(sb + 0x1C8), le32(sb + 0x19C), le64(sb + 0x1A0));
    if (le32(sb + 0x1CC))
        ext4_err(o, sb, "last", le32(sb + 0x1CC), 0x1E0, le32(sb + 0x1D4), le32(sb + 0x1D0), le64(sb + 0x1D8));

    say_ut(o, "ext4: last mounted ", le32(sb + 0x2C));
    say_ut(o, "ext4: last written ", le32(sb + 0x30));
    say_ut(o, "ext4: last checked (fsck) ", le32(sb + 0x40));
    uint32_t mc = le16(sb + 0x34), mx = le16(sb + 0x36);
    fwb_init(&s);
    fwb_s(&s, "ext4: mounted ");
    fwb_u(&s, mc);
    fwb_s(&s, " times since fsck, max ");
    if (mx == 0 || mx >= 0x8000U)
        fwb_s(&s, "none");
    else
        fwb_u(&s, mx);
    if (mx && mx < 0x8000U && mc >= mx)
        fwb_s(&s, " (fsck due)");
    say(o, s.b);

    fwb_init(&s);
    fwb_s(&s, "ext4: features:");
    fwb_s(&s, (comp & 4U) ? (inc & 8U) ? " journal(external)" : " journal" : " no-journal");
    if (inc & 0x40U)
        fwb_s(&s, " extents");
    if (inc & 0x80U)
        fwb_s(&s, " 64bit");
    if (ro & 0x400U)
        fwb_s(&s, " metadata_csum");
    if (inc & 4U)
        fwb_s(&s, " needs_recovery");
    say(o, s.b);
    if (inc & 4U)
        bad(o, "ext4: needs_recovery: the journal must be replayed (not unmounted cleanly)");
    if (ro & 0x400U) {
        uint32_t want = le32(sb + 0x3FC), got = crc32c_raw(~0U, sb, 1020);
        if (sb[0x175] != 1)
            bad(o, "ext4: metadata_csum with an unknown checksum type");
        else if (got == want)
            say(o, "ext4: superblock checksum OK (crc32c)");
        else {
            fwb_init(&s);
            fwb_s(&s, "ext4: superblock checksum WRONG: stored ");
            fwb_x(&s, want, 8);
            fwb_s(&s, ", computed ");
            fwb_x(&s, got, 8);
            bad(o, s.b);
        }
    }
}

/* ---- HFS+ / HFSX ---- */

#define HFS_EPOCH_DELTA 2082844800U      /* 1904-01-01 -> 1970-01-01 */

static void say_hfst(struct fw_out *o, const char *a, uint32_t t, const char *zone)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, a);
    if (!t)
        fwb_s(&s, "never");
    else if (t < HFS_EPOCH_DELTA)
        fwb_s(&s, "before 1970");
    else {
        fwb_time(&s, t - HFS_EPOCH_DELTA);
        if (zone) {
            s.n = s.n >= 4 ? s.n - 4 : s.n;  /* drop " UTC" */
            s.b[s.n] = 0;
            fwb_s(&s, zone);
        }
    }
    say(o, s.b);
}

void hfs_health(const struct fw_io *io, struct fw_out *o)
{
    uint8_t *h = sbk;
    struct fwb s;
    if (rdk(io, 1024, h, 512)) {
        bad(o, "HFS+: the volume header could not be read");
        return;
    }
    uint32_t sig = be16(h);
    if (sig == 0x4244U) {                  /* "BD": classic HFS */
        say(o, be16(h + 0x7C) == 0x482BU ? "HFS: classic HFS wrapper around an HFS+ volume (not followed)"
                                          : "HFS: classic HFS (not checked)");
        return;
    }
    if (sig != 0x482BU && sig != 0x4858U) {
        bad(o, "HFS+: no volume header signature (H+ / HX at byte 1024)");
        return;
    }
    uint32_t ver = be16(h + 2), attr = be32(h + 4), lmv = be32(h + 8), jib = be32(h + 12);
    uint32_t bs = be32(h + 40), total = be32(h + 44), fr = be32(h + 48);
    if (bs < 512 || bs > (1U << 20) || (bs & (bs - 1U))) {
        bad(o, "HFS+: block size out of range");
        return;
    }
    fwb_init(&s);
    fwb_s(&s, sig == 0x4858U ? "HFSX (case-sensitive HFS+): version " : "HFS+: version ");
    fwb_u(&s, ver);
    fwb_s(&s, ", ");
    fwb_u(&s, bs);
    fwb_s(&s, " B blocks, ");
    fwb_u(&s, total);
    fwb_s(&s, " blocks (");
    fwb_mb(&s, (uint64_t)total * bs);
    fwb_s(&s, "), free ");
    fwb_mb(&s, (uint64_t)fr * bs);
    say(o, s.b);
    if ((sig == 0x482BU && ver != 4) || (sig == 0x4858U && ver != 5))
        say(o, "HFS+: unexpected version for this signature");
    if (fr > total)
        bad(o, "HFS+: more free blocks than blocks");
    uint64_t vol = (uint64_t)total * bs;
    if (io->size && vol > io->size)
        bad(o, "HFS+: the volume is larger than its partition");
    fwb_init(&s);
    fwb_s(&s, "HFS+: ");
    fwb_u(&s, be32(h + 32));
    fwb_s(&s, " files, ");
    fwb_u(&s, be32(h + 36));
    fwb_s(&s, " folders");
    say(o, s.b);
    fwb_init(&s);
    fwb_s(&s, "HFS+: last mounted by '");
    for (int i = 24; i >= 0; i -= 8) {
        uint32_t c = (lmv >> i) & 0xFFU;
        fwb_c(&s, c >= 32 && c < 127 ? (char)c : '?');
    }
    fwb_s(&s, lmv == 0x31302E30U ? "' (macOS, not journaled)"
              : lmv == 0x4846534AU ? "' (macOS, journaled)"
              : lmv == 0x6673636BU ? "' (fsck_hfs)"
              : lmv == 0x382E3130U ? "' (Mac OS 8.1-9)"
                                   : "' (other)");
    say(o, s.b);
    fwb_init(&s);
    fwb_s(&s, "HFS+: attributes ");
    fwb_x(&s, attr, 8);
    if (attr & (1U << 13)) {
        fwb_s(&s, ", journaled (journal info block ");
        fwb_u(&s, jib);
        fwb_s(&s, ")");
    } else
        fwb_s(&s, ", not journaled");
    if (attr & (1U << 15))
        fwb_s(&s, ", software-locked");
    say(o, s.b);
    if (attr & (1U << 8))
        say(o, "HFS+: unmounted cleanly");
    else
        bad(o, "HFS+: NOT unmounted cleanly (mounted now, or the last mount did not end cleanly)");
    if (attr & (1U << 11))
        bad(o, "HFS+: the inconsistent bit is set (fsck_hfs needed)");
    say_hfst(o, "HFS+: created ", be32(h + 16), " local");
    say_hfst(o, "HFS+: modified ", be32(h + 20), 0);
    say_hfst(o, "HFS+: last checked (fsck) ", be32(h + 28), 0);
    say_hfst(o, "HFS+: backed up ", be32(h + 24), 0);
    /* the alternate volume header: 1024 bytes before the volume's end */
    uint8_t a[2];
    if (vol >= 4096 && !rdk(io, vol - 1024, a, 2))
        say(o, be16(a) == sig ? "HFS+: alternate volume header present"
                              : "HFS+: no alternate volume header at the volume's end");
}

/* ---- APFS ---- */

/* Fletcher-64 over 32-bit words, mod 2^32-1 */
static void fl_add(uint64_t *s1, uint64_t *s2, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        *s1 = (*s1 + le32(p + i)) % 0xFFFFFFFFULL;
        *s2 = (*s2 + *s1) % 0xFFFFFFFFULL;
    }
}
/* an APFS object's checksum check, block streamed through gbuf: 1 matches, 0 wrong, -1 unreadable */
static int apfs_ck(const struct fw_io *io, uint64_t off, uint32_t bs, uint64_t *stored, uint64_t *calc)
{
    uint64_t s1 = 0, s2 = 0;
    for (uint32_t done = 0; done < bs; done += sizeof(gbuf)) {
        if (rdk(io, off + done, gbuf, sizeof(gbuf)))
            return -1;
        if (!done) {
            *stored = le64(gbuf);
            fl_add(&s1, &s2, gbuf + 8, sizeof(gbuf) - 8);
        } else
            fl_add(&s1, &s2, gbuf, sizeof(gbuf));
    }
    uint64_t c1 = 0xFFFFFFFFULL - ((s1 + s2) % 0xFFFFFFFFULL);
    uint64_t c2 = 0xFFFFFFFFULL - ((s1 + c1) % 0xFFFFFFFFULL);
    *calc = c2 << 32 | c1;
    return *calc == *stored;
}

/* one checkpoint area (descriptor or data): 1 consistent */
static int apfs_area(struct fw_out *o, const char *what, uint32_t raw, uint64_t base, uint32_t next, uint32_t idx,
                     uint32_t len, uint64_t bc)
{
    struct fwb s;
    uint32_t n = raw & 0x7FFFFFFFU;
    fwb_init(&s);
    fwb_s(&s, "APFS: checkpoint ");
    fwb_s(&s, what);
    if (raw & 0x80000000U) {
        fwb_s(&s, " area non-contiguous (tree oid ");
        fwb_u(&s, base);
        fwb_s(&s, ", not followed)");
        say(o, s.b);
        return 1;
    }
    fwb_s(&s, " area blocks ");
    fwb_u(&s, base);
    fwb_s(&s, "+");
    fwb_u(&s, n);
    fwb_s(&s, ", current index ");
    fwb_u(&s, idx);
    fwb_s(&s, " len ");
    fwb_u(&s, len);
    fwb_s(&s, ", next ");
    fwb_u(&s, next);
    say(o, s.b);
    if (!n || !base || base > bc || n > bc - base) {
        fwb_init(&s);
        fwb_s(&s, "APFS: the checkpoint ");
        fwb_s(&s, what);
        fwb_s(&s, " area lies outside the container");
        bad(o, s.b);
        return 0;
    }
    if (idx >= n || len > n || next >= n) {
        fwb_init(&s);
        fwb_s(&s, "APFS: the checkpoint ");
        fwb_s(&s, what);
        fwb_s(&s, " index / length / next out of range");
        bad(o, s.b);
        return 0;
    }
    return 1;
}

void apfs_health(const struct fw_io *io, struct fw_out *o)
{
    uint8_t *nx = sbk;
    struct fwb s;
    if (rdk(io, 0, nx, sizeof(sbk))) {
        bad(o, "APFS: block 0 could not be read");
        return;
    }
    if (nx[32] != 'N' || nx[33] != 'X' || nx[34] != 'S' || nx[35] != 'B') {
        bad(o, "APFS: no container superblock magic (NXSB at byte 32)");
        return;
    }
    uint32_t bs = le32(nx + 36);
    uint64_t bc = le64(nx + 40), oid = le64(nx + 8), xid = le64(nx + 16);
    uint32_t ty = le32(nx + 24);
    if (bs < 4096 || bs > 65536 || (bs & (bs - 1U))) {
        bad(o, "APFS: block size out of range");
        return;
    }
    fwb_init(&s);
    fwb_s(&s, "APFS container: ");
    fwb_u(&s, bs);
    fwb_s(&s, " B blocks, ");
    fwb_u(&s, bc);
    fwb_s(&s, " blocks (");
    fwb_mb(&s, bc < (1ULL << 47) ? bc * bs : ~0ULL);
    fwb_s(&s, "), xid ");
    fwb_u(&s, xid);
    say(o, s.b);
    fwb_init(&s);
    fwb_s(&s, "APFS: UUID ");
    fwb_uuid(&s, nx + 72);
    say(o, s.b);
    if (bc < 2 || bc > (1ULL << 47)) {
        bad(o, "APFS: block count out of range");
        return;
    }
    if (io->size && bc > io->size / bs)
        bad(o, "APFS: the container is larger than its partition");
    uint64_t st = 0, ca = 0;
    int ck = apfs_ck(io, 0, bs, &st, &ca);
    if (ck > 0)
        say(o, "APFS: block 0 checksum OK (Fletcher-64)");
    else if (ck < 0)
        bad(o, "APFS: block 0 could not be read whole");
    else {
        fwb_init(&s);
        fwb_s(&s, "APFS: block 0 checksum WRONG: stored ");
        fwb_x(&s, st, 16);
        fwb_s(&s, ", computed ");
        fwb_x(&s, ca, 16);
        bad(o, s.b);
    }
    /* the checkpoint as far as block 0 shows */
    int ok = 1;
    if (oid != 1 || (ty & 0xFFFFU) != 1) {
        bad(o, "APFS: block 0 is not the container superblock object (oid 1, type NX_SUPERBLOCK)");
        ok = 0;
    }
    uint32_t dr = le32(nx + 104), dtr = le32(nx + 108);
    uint64_t db = le64(nx + 112), dtb = le64(nx + 120);
    ok &= apfs_area(o, "descriptor", dr, db, le32(nx + 128), le32(nx + 136), le32(nx + 140), bc);
    ok &= apfs_area(o, "data", dtr, dtb, le32(nx + 132), le32(nx + 144), le32(nx + 148), bc);
    if (le64(nx + 96) <= xid) {
        bad(o, "APFS: next xid is not after block 0's xid");
        ok = 0;
    }
    if (!le64(nx + 152) || !le64(nx + 160)) {
        bad(o, "APFS: no space manager or object map");
        ok = 0;
    }
    uint32_t maxfs = le32(nx + 180), nv = 0;
    for (unsigned i = 0; i < 100; i++)
        nv += le64(nx + 184 + 8 * i) != 0;
    fwb_init(&s);
    fwb_s(&s, "APFS: ");
    fwb_u(&s, nv);
    fwb_s(&s, " volume(s), at most ");
    fwb_u(&s, maxfs);
    say(o, s.b);
    if (!maxfs || maxfs > 100 || nv > maxfs) {
        bad(o, "APFS: volume count out of range");
        ok = 0;
    }
    say(o, ok ? "APFS: checkpoint layout consistent (as far as block 0 shows)"
              : "APFS: checkpoint layout INCONSISTENT (see above)");
    /* the newest container superblock in a contiguous descriptor area (block 0 is only a copy) */
    uint32_t n = dr & 0x7FFFFFFFU;
    if (!ok || (dr & 0x80000000U))
        return;
    uint64_t best = 0, bi = 0;
    uint32_t scan = n < 1024U ? n : 1024U, got = 0;
    for (uint32_t i = 0; i < scan; i++, got++) {
        uint8_t hd[40];
        if (rdk(io, (db + i) * bs, hd, sizeof(hd)))
            break;
        if (hd[32] == 'N' && hd[33] == 'X' && hd[34] == 'S' && hd[35] == 'B' && (le32(hd + 24) & 0xFFFFU) == 1 &&
            le64(hd + 16) >= best) {
            best = le64(hd + 16);
            bi = i;
        }
    }
    if (!best) {
        say(o, got ? "APFS: no container superblock found in the descriptor area"
                   : "APFS: the descriptor area could not be read");
        return;
    }
    ck = apfs_ck(io, (db + bi) * bs, bs, &st, &ca);
    fwb_init(&s);
    fwb_s(&s, "APFS: newest checkpoint superblock xid ");
    fwb_u(&s, best);
    fwb_s(&s, " at descriptor block ");
    fwb_u(&s, bi);
    fwb_s(&s, ck > 0 ? ", checksum OK" : ck < 0 ? ", unreadable" : ", checksum WRONG");
    fwb_s(&s, best == xid ? " (= block 0)" : best > xid ? " (newer than block 0)" : " (older than block 0)");
    if (ck > 0)
        say(o, s.b);
    else
        bad(o, s.b);
}

/* ---- the dispatcher ---- */

const char *fs_name(int kind)
{
    if (kind == FS_FAT32)
        return "FAT32";
    if (kind == FS_NTFS)
        return "NTFS";
    if (kind == FS_EXT4)
        return "ext2/3/4";
    if (kind == FS_HFSPLUS)
        return "HFS+ / HFSX";
    if (kind == FS_APFS)
        return "APFS";
    if (kind == FS_HFS)
        return "classic HFS";
    if (kind == FS_EXFAT)
        return "exFAT";
    return "unknown";
}

static int bytes_are(const uint8_t *p, const char *t)
{
    for (; *t; p++, t++)
        if (*p != (uint8_t)*t)
            return 0;
    return 1;
}

int fs_sniff(const struct fw_io *io)
{
    uint8_t *b = gbuf;
    if (rdk(io, 0, b, 512))
        return FS_UNKNOWN;
    if (bytes_are(b + 32, "NXSB"))
        return FS_APFS;
    if (bytes_are(b + 3, "NTFS    "))
        return FS_NTFS;
    if (bytes_are(b + 3, "EXFAT   "))
        return FS_EXFAT;
    if (bytes_are(b + 82, "FAT32   ") && b[510] == 0x55 && b[511] == 0xAA)
        return FS_FAT32;
    if (rdk(io, 1024, b, 512))
        return FS_UNKNOWN;
    if (le16(b + 0x38) == 0xEF53U)
        return FS_EXT4;
    if (b[0] == 'H' && (b[1] == '+' || b[1] == 'X'))
        return FS_HFSPLUS;
    if (b[0] == 'B' && b[1] == 'D')
        return FS_HFS;
    return FS_UNKNOWN;
}

int fs_health(const struct fw_io *io, struct fw_out *o)
{
    int k = fs_sniff(io);
    say2(o, "file system: ", fs_name(k));
    if (k == FS_FAT32) {
        struct fat32 f;
        if (fat32_mount(&f, io))
            fat32_health(&f, o);
        else
            bad(o, "FAT32: boot sector values out of range");
    } else if (k == FS_NTFS)
        ntfs_health(io, o);
    else if (k == FS_EXT4)
        ext4_health(io, o);
    else if (k == FS_HFSPLUS || k == FS_HFS)
        hfs_health(io, o);
    else if (k == FS_APFS)
        apfs_health(io, o);
    else if (k == FS_EXFAT)
        say(o, "exFAT: no health check yet");
    else
        say(o, "no known file system (FAT32, NTFS, ext4, HFS+, APFS) at the start of this range");
    return k;
}
