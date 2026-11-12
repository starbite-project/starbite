/* SPDX-License-Identifier: GPL-2.0-only
 * Settings > Startup's checks (ui/shell.h, Track J FW4): sh_health_os, sh_store_check, sh_logs_save, sh_slots_get.
 *   - an OS's health: its catalog row's root ("sd:lba A+N" card sectors of 512 bytes, "ufs:NAME" a GPT partition of
 *     UFS LU 0, anything else: nothing to check) through the loader's read-only checkers (shim/starlte/sbfwfs.c:
 *     FAT32, NTFS, ext4, HFS+, APFS, the BCD); a Windows row (Microsoft, UEFI) as the loader's windows_health:
 *     WINESP (FAT32 + the BCD and its Advanced Startup plan, lines only) and WINDOWS (NTFS);
 *   - the home store: its SBSTORE2 header, the card's head backup (p2 blocks 64..70), the settings record, the crash
 *     log, every directory entry inside the capacity (no CRC pass);
 *   - the logs: the "loader-logs" volatile entry gets the SBLOGS01 bundle (the loader's repair.c rp_logs_save
 *     format, tools/store/sbstore2.py logs reads it): Starbite OS's log ring, the pmsg ring, the loader table's
 *     lines, the crash log;
 *   - Software Update's slots (sbos-slot-a / -b headers, the settings record's slot bytes).
 * Everything but the logs is read only. The logs: the fence's one volatile grant for that entry's blocks (lent:
 * slot.c takes it back), with the store gate open for that one write (kern/sbfw_os.c sbfw_os_gate).
 * In a self-test image (sbos_selftest_enabled) nothing is read from the disks and the logs are a logged dry run.
 * The host test kern/host/sbfw-health-test.c builds this file with fakes around it.
 */
#include "sb.h"
#include "kern.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"
#include "ufs.h"
#include "sd.h"
#include "../../../shim/starlte/sbfwfs.h"
#include "../ui/shell.h"

int sbfw_os_gate(const char *what);          /* kern/sbfw_os.c: the store gate for one write (0 open) */

static int dry(void)
{
    return sbos_selftest_enabled;
}

static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | p[i - 1];
    return v;
}

static int is(const char *s, const char *pre)
{
    while (*pre)
        if (*s++ != *pre++)
            return 0;
    return 1;
}

/* ---- the result's lines: every line logged; the page keeps the problems first, then the rest, in order ---- */

#define HL_MAX 48
static struct {
    char l[HL_MAX][72];
    uint8_t prob[HL_MAX];
    unsigned n, more;
} hl;

static void hl_line(void *u, const char *s)
{
    (void)u;
    sb_logf("startup: check: ", s);
    if (hl.n >= HL_MAX) {
        ++hl.more;
        return;
    }
    sb_strcpy(hl.l[hl.n], sizeof(hl.l[0]), s);
    hl.prob[hl.n] = (uint8_t)is(s, "PROBLEM");
    ++hl.n;
}

static void hl_begin(sh_health_t *h, struct fw_out *o)
{
    uint8_t *z = (uint8_t *)h;
    for (unsigned i = 0; i < sizeof(*h); ++i)
        z[i] = 0;
    hl.n = hl.more = 0;
    o->line = hl_line;
    o->u = 0;
    o->bad = 0;
}

static void hl_end(sh_health_t *h, struct fw_out *o)
{
    h->bad = o->bad;
    unsigned cap = SH_HEALTH_LINES, k = 0, total = hl.n + hl.more;
    unsigned room = total > cap ? cap - 1 : cap;          /* the last line says how many more are in the log */
    unsigned probs = 0;
    for (unsigned i = 0; i < hl.n; ++i)
        probs += hl.prob[i];
    unsigned others = room > probs ? room - probs : 0;
    for (unsigned i = 0; i < hl.n && k < room; ++i)
        if (hl.prob[i] || others) {
            if (!hl.prob[i])
                --others;
            sb_strcpy(h->line[k++], sizeof(h->line[0]), hl.l[i]);
        }
    if (total > k && k < cap) {
        struct ub u;
        ub_init(&u);
        ub_dec(&u, total - k);
        ub_s(&u, total - k == 1 ? " more line in the log" : " more lines in the log");
        sb_strcpy(h->line[k++], sizeof(h->line[0]), u.b);
    }
}

static void say(struct fw_out *o, const char *a, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    if (b)
        ub_s(&u, b);
    o->line(o->u, u.b);
}

static void bad(struct fw_out *o, const char *a, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "PROBLEM: ");
    ub_s(&u, a);
    if (b)
        ub_s(&u, b);
    ++o->bad;
    o->line(o->u, u.b);
}

/* ---- reads: a UFS partition or a card range through one 4 KiB block cache ---- */

static uint8_t blk[4096];
static uint64_t blk_key = ~0ULL;

struct pio {
    uint64_t first, last;                    /* UFS LU 0 4 KiB blocks */
};
static int prd(void *u, uint64_t off, void *buf, uint32_t len)
{
    const struct pio *p = (const struct pio *)u;
    uint8_t *d = (uint8_t *)buf;
    while (len) {
        uint64_t lba = p->first + off / 4096U;
        uint32_t in = (uint32_t)(off % 4096U), n = 4096U - in;
        if (n > len)
            n = len;
        if (lba > p->last)
            return -1;
        if (lba != blk_key) {
            blk_key = ~0ULL;
            if (sb_blk_read(SB_MED_UFS, 0, lba, 1, blk))
                return -1;
            blk_key = lba;
        }
        for (uint32_t i = 0; i < n; ++i)
            d[i] = blk[in + i];
        d += n;
        off += n;
        len -= n;
    }
    return 0;
}

static int part_io(const char *name, struct pio *p, struct fw_io *io)
{
    if (gpt_load() <= 0 || !gpt_find(name, &p->first, &p->last) || p->last < p->first)
        return 0;
    io->rd = prd;
    io->u = p;
    io->size = (p->last - p->first + 1) * 4096U;
    blk_key = ~0ULL;
    return 1;
}

struct cio {
    uint64_t first, count;                   /* 512-byte card sectors */
};
static int crd(void *u, uint64_t off, void *buf, uint32_t len)
{
    const struct cio *c = (const struct cio *)u;
    uint8_t *d = (uint8_t *)buf;
    while (len) {
        uint64_t sec = (off / 4096U) * 8U;
        uint32_t in = (uint32_t)(off % 4096U), n = 4096U - in;
        if (n > len)
            n = len;
        if (sec + 8 > c->count)
            return -1;
        uint64_t s = c->first + sec, key = (1ULL << 63) | s;
        if (key != blk_key) {
            blk_key = ~0ULL;
            /* a 4 KiB-aligned range: the block layer; else eight sectors from the card driver */
            if ((s & 7U) ? sd_read(s, 8, (uintptr_t)blk) : sb_blk_read(SB_MED_SD, 0, s / 8U, 1, blk))
                return -1;
            blk_key = key;
        }
        for (uint32_t i = 0; i < n; ++i)
            d[i] = blk[in + i];
        d += n;
        off += n;
        len -= n;
    }
    return 0;
}

static uint64_t parse_u(const char **p)
{
    uint64_t v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10U + (uint64_t)(*(*p)++ - '0');
    return v;
}

static void fs_set(sh_health_t *h, int kind)
{
    const char *n = kind == FS_FAT32 ? "FAT32" : kind == FS_NTFS ? "NTFS" : kind == FS_EXT4 ? "ext4" :
                    kind == FS_HFSPLUS ? "HFS+" : kind == FS_APFS ? "APFS" : kind == FS_HFS ? "HFS" :
                    kind == FS_EXFAT ? "exFAT" : "-";
    sb_strcpy(h->fs, sizeof(h->fs), n);
}

static void root_check(sh_health_t *h, struct fw_out *o, const char *root)
{
    struct fw_io io;
    struct ub u;
    int kind;
    if (is(root, "sd:lba")) {
        const char *p = root + 6;
        while (*p == ' ')
            ++p;
        static struct cio c;
        c.first = parse_u(&p);
        c.count = *p == '+' ? (++p, parse_u(&p)) : 0;
        ub_init(&u);
        ub_s(&u, "card sectors ");
        ub_dec(&u, c.first);
        ub_c(&u, '+');
        ub_dec(&u, c.count);
        sb_strcpy(h->where, sizeof(h->where), u.b);
        h->checked = 1;
        sd_allow();
        if (c.count < 8 || !sd_up()) {
            bad(o, c.count < 8 ? "the root's card range is empty" : "the microSD card does not answer", 0);
            return;
        }
        io.rd = crd;
        io.u = &c;
        io.size = c.count * 512U;
        blk_key = ~0ULL;
        say(o, "Root: microSD card, ", u.b);
    } else if (is(root, "ufs:")) {
        char name[24];
        unsigned k = 0;
        for (const char *p = root + 4; k + 1 < sizeof(name) &&
             ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'); ++p)
            name[k++] = *p;
        name[k] = 0;
        ub_init(&u);
        ub_s(&u, "UFS ");
        ub_s(&u, name);
        sb_strcpy(h->where, sizeof(h->where), u.b);
        h->checked = 1;
        static struct pio pp;
        if (!k || !part_io(name, &pp, &io)) {
            bad(o, "the root partition is not in the UFS partition table: ", name);
            return;
        }
        say(o, "Root: UFS partition ", name);
    } else {
        sb_strcpy(h->where, sizeof(h->where), root[0] ? root : "none");
        sb_strcpy(h->fs, sizeof(h->fs), "-");
        say(o, "Nothing to check: the root is ", root[0] ? root : "not set (RAM only)");
        return;
    }
    kind = fs_health(&io, o);
    fs_set(h, kind);
    if (kind == FS_UNKNOWN)
        bad(o, "no known file system at the root", 0);
}

/* Windows (the loader's windows_health): WINESP FAT32 + the BCD (its plan as lines), WINDOWS NTFS */
static uint8_t *bcd_mem;
#define BCD_CAP (1U << 20)

static void windows_check(sh_health_t *h, struct fw_out *o)
{
    struct pio p;
    struct fw_io io;
    static struct fat32 f;
    struct bcd_plan plan;
    sb_strcpy(h->where, sizeof(h->where), "UFS WINESP + WINDOWS");
    sb_strcpy(h->fs, sizeof(h->fs), "NTFS");
    h->checked = 1;
    if (gpt_load() <= 0) {
        bad(o, "the UFS partition table could not be read", 0);
        return;
    }
    if (!part_io("WINESP", &p, &io) || !fat32_mount(&f, &io)) {
        bad(o, "WINESP is not a readable FAT32 volume", 0);
    } else {
        fat32_health(&f, o);
        uint32_t c, sz;
        if (!bcd_mem)
            bcd_mem = sb_alloc(BCD_CAP);
        if (!fat32_find(&f, "/EFI/Microsoft/Boot/BCD", &c, &sz)) {
            bad(o, "\\EFI\\Microsoft\\Boot\\BCD not found on WINESP", 0);
        } else if (!bcd_mem) {
            say(o, "The BCD: no memory to read it here", 0);
        } else {
            int64_t n = fat32_read(&f, c, sz, bcd_mem, BCD_CAP);
            if (n < 8192) {
                bad(o, "the BCD could not be read", 0);
            } else {
                bcd_check(bcd_mem, (uint32_t)n, &plan, o);
                uint64_t po;
                if (plan.ok && fat32_map(&f, c, plan.off, &po)) {
                    struct ub u;
                    ub_init(&u);
                    ub_s(&u, "Plan: WINESP block ");
                    ub_dec(&u, po / 4096U);
                    ub_s(&u, " (UFS LBA ");
                    ub_dec(&u, p.first + po / 4096U);
                    ub_s(&u, ") byte ");
                    ub_dec(&u, po % 4096U);
                    ub_s(&u, ", 76 bytes (not applied here)");
                    o->line(o->u, u.b);
                }
            }
        }
    }
    if (!part_io("WINDOWS", &p, &io))
        bad(o, "no WINDOWS partition", 0);
    else
        ntfs_health(&io, o);
}

int sh_health_os(const char *key, sh_health_t *h)
{
    struct fw_out o;
    hl_begin(h, &o);
    int k = key && key[0] ? cat_find(key) : -1;
    const struct cat_ent *c = k >= 0 ? cat_get((unsigned)k) : 0;
    if (!c) {
        sb_strcpy(h->line[0], sizeof(h->line[0]), "No such operating system in the catalog");
        return -1;
    }
    if (dry()) {
        sb_strcpy(h->line[0], sizeof(h->line[0]), "A dry run: the self test reads no disks");
        sb_logf("startup: would check the health of ", key);
        return -2;
    }
    sb_logf("startup: health check of ", key);
    if (c->method == M_UEFI && (c->category == CAT_MICROSOFT || is(c->key, "win")))
        windows_check(h, &o);
    else
        root_check(h, &o, c->root);
    say(&o, o.bad ? "Problems found: see above" : (h->checked ? "No problems found" : "Nothing was checked"), 0);
    hl_end(h, &o);
    return 0;
}

/* ---- the home store ---- */

#define HEAD_BAK 64U                             /* the card store head's backup: p2 blocks 64..70 */
static uint8_t head[7 * 4096], hbak[7 * 4096];

/* the loader's sbstore_head_check: header CRC, layout, the directory's CRC (cap 0: any capacity) */
static int head_ok(const uint8_t *hd, uint64_t cap, uint32_t *count)
{
    const uint8_t *b = hd + 4096;
    if (!is((const char *)b, "SBSTORE2") || get(b + 8, 4) != 2 || sb_crc32(b, 48) != (uint32_t)get(b + 48, 4))
        return 0;
    uint32_t n = (uint32_t)get(b + 12, 4), dir = (uint32_t)get(b + 32, 4), dirn = (uint32_t)get(b + 36, 4);
    uint64_t c = get(b + 16, 8), used = get(b + 24, 8);
    if (n > 256 || dir != 3 || dirn != 4 || get(b + 40, 4) != 2 || used > c || (cap && c != cap))
        return 0;
    if (sb_crc32(hd + 3 * 4096, (uint64_t)n * 64U) != (uint32_t)get(b + 44, 4))
        return 0;
    *count = n;
    return 1;
}

static int same_blocks(const uint8_t *a, const uint8_t *b, unsigned from, unsigned to)
{
    for (unsigned i = from * 4096U; i < to * 4096U; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int sh_store_check(sh_health_t *h)
{
    struct fw_out o;
    struct ub u;
    hl_begin(h, &o);
    sb_strcpy(h->fs, sizeof(h->fs), "-");
    if (dry()) {
        sb_strcpy(h->line[0], sizeof(h->line[0]), "A dry run: the self test reads no disks");
        sb_log("startup: would check the OS store");
        return -2;
    }
    int hm = store_home();
    if (hm < 0) {
        sb_strcpy(h->line[0], sizeof(h->line[0]), "No OS store on this phone");
        return -1;
    }
    unsigned med = hm == (int)STORE_SD ? SB_MED_SD : SB_MED_UFS;
    uint64_t base = store_base((unsigned)hm);
    sb_strcpy(h->where, sizeof(h->where), hm == (int)STORE_SD ? "the microSD card" : "UFS (USERDATA +16 GiB)");
    h->checked = 1;
    say(&o, "Home store: ", h->where);
    /* the header and the directory, as on the medium (not libsb's cached view) */
    if (sb_blk_read(med, 0, base, 7, head)) {
        bad(&o, "the store's first blocks do not read", 0);
        goto out;
    }
    const uint8_t *b = head + 4096;
    uint32_t n = 0;
    if (!is((const char *)b, "SBSTORE2") || get(b + 8, 4) != 2 || sb_crc32(b, 48) != (uint32_t)get(b + 48, 4)) {
        bad(&o, "the SBSTORE2 header is damaged (magic, version or CRC)", 0);
        goto out;
    }
    n = (uint32_t)get(b + 12, 4);
    uint64_t cap = get(b + 16, 8), used = get(b + 24, 8);
    uint32_t dir = (uint32_t)get(b + 32, 4), dirn = (uint32_t)get(b + 36, 4), setb = (uint32_t)get(b + 40, 4);
    ub_init(&u);
    ub_s(&u, "Header: OK, ");
    ub_dec(&u, n);
    ub_s(&u, n == 1 ? " entry, " : " entries, ");
    ub_size(&u, used * 4096U);
    ub_s(&u, " of ");
    ub_size(&u, cap * 4096U);
    ub_s(&u, " used");
    o.line(o.u, u.b);
    if (used > cap || n > STORE_MAX || !dirn || dirn > STORE_MAX * 64U / 4096U || n * 64U > dirn * 4096U ||
        (uint64_t)dir + dirn > cap) {
        bad(&o, "the header's layout is out of range", 0);
        goto out;
    }
    /* the directory: its CRC, every entry inside the capacity */
    static uint8_t dbuf[STORE_MAX * 64U];
    if (sb_blk_read(med, 0, base + dir, dirn, dbuf)) {
        bad(&o, "the directory does not read", 0);
    } else if (sb_crc32(dbuf, (uint64_t)n * 64U) != (uint32_t)get(b + 44, 4)) {
        bad(&o, "the directory's CRC does not match", 0);
    } else {
        unsigned outside = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t *e = dbuf + 64U * i;
            uint64_t eb = get(e + 32, 8), sz = get(e + 40, 8);
            if (eb > cap || (sz + 4095U) / 4096U > cap - eb || (sz && eb < (uint64_t)dir + dirn)) {
                char nm[33];
                unsigned k = 0;
                for (; k < 32 && e[k]; ++k)
                    nm[k] = (char)e[k];
                nm[k] = 0;
                bad(&o, "entry outside the store: ", nm);
                ++outside;
            }
        }
        if (!outside)
            say(&o, "Entries: all inside the store", 0);
    }
    /* the settings record */
    {
        struct star_settings st;
        static uint8_t sb[4096];
        int ok = settings_load(&st);
        int magic = setb && setb < cap && !sb_blk_read(med, 0, base + setb, 1, sb) && is((const char *)sb, "SBSETTNG");
        if (ok)
            say(&o, "Settings record: OK", 0);
        else if (magic)
            bad(&o, "the settings record is damaged (CRC)", 0);
        else
            say(&o, "Settings record: none yet (the loader's defaults)", 0);
    }
    /* the crash log */
    {
        struct store_ent e;
        if (!store_find("home:crashlog", &e)) {
            bad(&o, "no crash log entry (\"crashlog\")", 0);
        } else {
            int c = sbfw_crash_count();
            if (c < 0) {
                bad(&o, "the crash log's head is not valid", 0);
            } else {
                ub_init(&u);
                ub_s(&u, "Crash log: OK, ");
                ub_dec(&u, (unsigned)c);
                ub_s(&u, c == 1 ? " report" : " reports");
                o.line(o.u, u.b);
            }
        }
    }
    /* the card's head backup (Starbite Recovery restores the head from it) */
    if (hm == (int)STORE_SD) {
        uint64_t first = 0, blocks = 0;
        uint32_t nb = 0, nc = 0;
        if (!sd_store_area(&first, &blocks) || sb_blk_read(SB_MED_SD, 0, first + HEAD_BAK, 7, hbak) ||
            !head_ok(hbak, blocks, &nb)) {
            bad(&o, "no valid head backup at card p2 blocks 64..70", 0);
        } else if (head_ok(head, blocks, &nc) && same_blocks(head, hbak, 0, 2) && same_blocks(head, hbak, 3, 7)) {
            say(&o, "Head backup: OK, the same as the store's head", 0);
        } else {
            ub_init(&u);
            ub_s(&u, "Head backup: valid but older (");
            ub_dec(&u, nb);
            ub_s(&u, " entries; the store has ");
            ub_dec(&u, n);
            ub_s(&u, ")");
            o.line(o.u, u.b);
        }
    }
out:
    say(&o, o.bad ? "Problems found: see above" : "No problems found", 0);
    hl_end(h, &o);
    return 0;
}

/* ---- the logs ---- */

#define LG_CAP (256U << 10)                       /* the bundle's size at most (the loader's entry: 256 KiB) */
#define LG_PMSG 0x4000U
static uint8_t *lg;

#ifndef SB_HOST
static uint32_t lg_add(uint32_t *off, unsigned *count, uint32_t cap, const char *name, const uint8_t *src, uint32_t size)
{
    if (*count >= 160)
        return 0;
    if (*off + size > cap)
        size = *off < cap ? cap - *off : 0;
    uint8_t *d = lg + 16 + 24U * *count;
    for (unsigned i = 0; i < 16; ++i)
        d[i] = 0;
    for (unsigned i = 0; i < 15 && name[i]; ++i)
        d[i] = (uint8_t)name[i];
    for (unsigned i = 0; i < 4; ++i) {
        d[16 + i] = (uint8_t)(*off >> (8 * i));
        d[20 + i] = (uint8_t)(size >> (8 * i));
    }
    for (uint32_t i = 0; i < size; ++i)
        lg[*off + i] = src[i];
    *off += (size + 4095U) & ~4095U;
    ++*count;
    return size;
}
#endif

/* text lines into the bundle at off, at most cap - off bytes: the bytes written */
static uint32_t lg_text(uint32_t off, uint32_t cap, unsigned n, const char *(*line)(unsigned))
{
    uint32_t at = off;
    for (unsigned i = 0; i < n; ++i) {
        const char *s = line(i);
        if (!s[0])
            continue;
        for (; *s && at + 1 < cap; ++s)
            lg[at++] = (uint8_t)*s;
        if (at + 1 >= cap)
            break;
        lg[at++] = '\n';
    }
    return at - off;
}

static const char *fw_line(unsigned i)
{
    return sb_fw.lines[i];
}

static void lg_section(uint32_t *off, unsigned *count, uint32_t cap, const char *name, uint32_t size)
{
    /* the bytes are already at *off: only the directory slot and the alignment */
    if (*count >= 160)
        return;
    uint8_t *d = lg + 16 + 24U * *count;
    for (unsigned i = 0; i < 16; ++i)
        d[i] = 0;
    for (unsigned i = 0; i < 15 && name[i]; ++i)
        d[i] = (uint8_t)name[i];
    for (unsigned i = 0; i < 4; ++i) {
        d[16 + i] = (uint8_t)(*off >> (8 * i));
        d[20 + i] = (uint8_t)(size >> (8 * i));
    }
    *off += (size + 4095U) & ~4095U;
    if (*off > cap)
        *off = cap;
    ++*count;
}

int sh_logs_save(void)
{
    struct store_ent e;
    if (store_home() < 0 || !store_find("home:loader-logs", &e) || !(e.flags & 1U) || e.size < 8192U) {
        sb_log("startup: no \"loader-logs\" volatile entry on the home store (sbstore2.py --volatile "
               "loader-logs=262144): logs not saved");
        return -2;
    }
    if (dry()) {
        sb_log("startup: would save the logs to \"loader-logs\" (a dry run: the self test)");
        return 0;
    }
    uint32_t cap = (uint32_t)((e.size > LG_CAP ? LG_CAP : e.size) & ~4095ULL);
    if (!lg)
        lg = sb_alloc(LG_CAP);
    if (!lg)
        return -1;
    for (uint32_t i = 0; i < cap; ++i)
        lg[i] = 0;
    uint32_t off = 4096;
    unsigned count = 0;
    /* Starbite OS's own log ring, as text */
    if (off < cap)
        lg_section(&off, &count, cap, "sbos-log", lg_text(off, cap, sb_log_count(), sb_log_line));
#ifndef SB_HOST
    /* the pmsg ring (every OS's lines since the last cold start; sbstore2.py decodes the "DBGC" zone) */
    if (off < cap)
        lg_add(&off, &count, cap, "pmsg", (const uint8_t *)SB_PMSG, LG_PMSG);
#endif
    /* the loader table's last console lines */
    if (sb_fw.valid && off < cap)
        lg_section(&off, &count, cap, "loader-lines", lg_text(off, cap, SBFW_LINES, fw_line));
    /* the crash log, raw (sbstore2.py crashlog decodes it) */
    struct store_ent cl;
    if (off < cap && store_find("home:crashlog", &cl)) {
        uint64_t nb = cl.size / 4096U;
        if (nb > (cap - off) / 4096U)
            nb = (cap - off) / 4096U;
        uint64_t k = 0;
        for (; k < nb; ++k)
            if (store_read_block(&cl, k, lg + off + k * 4096U))
                break;
        if (k)
            lg_section(&off, &count, cap, "crashlog", (uint32_t)(k * 4096U));
    }
    for (unsigned i = 0; i < 8; ++i)
        lg[i] = (uint8_t)"SBLOGS01"[i];
    lg[8] = 1;
    lg[12] = (uint8_t)count;
    uint64_t blocks = (off < cap ? off : cap) / 4096U;
    unsigned med = e.medium == STORE_SD ? SB_MED_SD : SB_MED_UFS;
    int r = sbfw_os_gate("startup: save the logs");
    if (r) {
        sb_log("startup: the store gate did not open: logs not saved");
        return -1;
    }
    struct store_ent g;
    if (!store_volatile_grant_part("loader-logs", 8192U, 0, blocks, &g) || g.lba != e.lba) {
        sb_gate_close();
        sb_log("startup: \"loader-logs\" was not granted: logs not saved");
        return -1;
    }
    hw_slot_lend();                              /* the fence's one grant is this entry's now: slot.c takes it back */
    r = sb_write_volatile(med, e.lba, blocks, lg);
    sb_gate_close();
    if (r == SB_W_DRYRUN)
        return 0;
    if (r) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "startup: the logs were not written: ");
        ub_sdec(&u, r);
        sb_log(u.b);
        return -1;
    }
    /* read back */
    static uint8_t rb[4096];
    for (uint64_t k = 0; k < blocks; ++k) {
        if (sb_blk_read(med, 0, e.lba + k, 1, rb))
            return -1;
        for (unsigned i = 0; i < 4096; ++i)
            if (rb[i] != lg[k * 4096U + i]) {
                sb_log("startup: the logs' read-back differs");
                return -1;
            }
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "startup: logs saved to \"loader-logs\": ");
    ub_dec(&u, count);
    ub_s(&u, " sections, ");
    ub_dec(&u, blocks);
    ub_s(&u, " blocks (Mac: sbstore2.py logs)");
    sb_log(u.b);
    return 0;
}

/* ---- Software Update's slots ---- */

int sh_slots_get(sh_slots_t *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); ++i)
        z[i] = 0;
    if (store_home() < 0)
        return -1;
    s->state[0] = 1;                             /* the normal image: the row's own file */
    unsigned active = 0, trying = 0, tries = 0;
    if (!dry())
        sbfw_slot_get(&active, &trying, &tries);
    s->active = (int)active;
    s->trying = (int)trying;
    static uint8_t hb[4096];
    for (unsigned k = 1; k <= 2; ++k) {
        struct store_ent e;
        if (!store_find(k == 1 ? "home:sbos-slot-a" : "home:sbos-slot-b", &e)) {
            s->state[k] = -1;
            continue;
        }
        s->state[k] = 0;
        if (dry() || store_read_block(&e, 0, hb))
            continue;
        /* a valid image for whichever row its header names */
        char key[24];
        unsigned i = 0;
        for (; i < 23 && hb[64 + i]; ++i)
            key[i] = (char)hb[64 + i];
        key[i] = 0;
        struct sbslot_info si;
        if (key[0] && sbslot_check(hb, e.size, key, &si)) {
            s->state[k] = 1;
            sb_strcpy(s->version[k], sizeof(s->version[k]), si.vstr[0] ? si.vstr : "?");
        }
    }
    return 0;
}
