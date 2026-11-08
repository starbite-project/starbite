/* SPDX-License-Identifier: GPL-2.0-only
 * sbm on libsb (Starbite OS and test images): the struct sbm_plat for a bare-metal image.
 *  - probe32: libsb's sb_probe32 (survives a synchronous abort), plus a check for an asynchronous one: libsb
 *    runs with PSTATE.A masked, so an SError a refused read raises stays pending in ISR_EL1.A. It is then taken
 *    at once into libsb's vector, which only counts it (crt0.S vec_serror), and the read counts as faulted. An
 *    SError already pending before the read is taken first and reported separately (sbm_sb_stale_serrors).
 *    If EL3 routes SErrors to itself (SCR_EL3.EA), none of this sees them: EL3 decides. docs/MODEM.md 5.
 *  - log: the caller's line sink (screen + pmsg).
 *  - barrier: dmb ish.
 *  - smc: smc #0 (sb_psci's call). sbm only makes EL3's CP_CTRL read call with it (sbm_cp_ctrl_read).
 *  - radio_read: the RADIO partition (GPT "RADIO" on UFS LU0), read only, through a 4 KiB block buffer.
 *  - poke32: one store + clean/invalidate to DRAM, for sbm_probe_write_test's two fixed words only: any other
 *    address is refused here as well (a second lock).
 *  - stage 3 (sbm_sb_plat_stage3), each with its own exact allow-list, a second lock behind the core's:
 *    smc: CP_CTRL read; CP_CTRL write only when it changes nothing but bits 1, 2, 3, 17 of the value EL3 last
 *         read back (PMUCAL's CP_PWRON, CP_RESET_SET, CP_START, dump_pc_no_pg); the SSS clock on/off; the
 *         security call mode 2 (2, 0, 0) and mode 0 (0xF0000000, BOOT size, MAIN size within the CP memory).
 *    mmio_wr32: PMU 0x1320-0x1338 and 0x280 (cal_cp_reset_assert), mailbox ISSR0-63, INTGR1 = 2 (the wakeup
 *         interrupt), AP2CP_CFG = 1; stage 4 also INTGR1 = 1 (the message interrupt) and INTCR0 = 1 << 16
 *         (acknowledging the CP's message). Nothing else.
 *    ipc (stage 4): the IPC region, modem_if + the map's ipc_off, for the legacy queue words and the SBD layout.
 *    carve_write / carve_read: modem_if + [0, 0x8000000) only (the CP memory and VSS; never the IPC region,
 *         never cp_ram_logging), a store fault or SError makes it fail.
 *  - Partitions: RADIO and EFS are read through sb_blk_read only; this file has no block-write call (host/run.sh
 *    greps for one), and the images also hold libsb's storage lock (sb_storage_hold) for the whole run.
 */
#include "sb.h"
#include "ufs.h"
#include "../sbm.h"
#include "sbm_sb.h"
#include "ext4.h"
#include "jbd2.h"

static unsigned stale_serrors;

static int serror_pending(void)
{
    uint64_t isr;
    __asm__ volatile("isb\n\tmrs %0, isr_el1" : "=r"(isr) : : "memory");
    return (isr >> 8) & 1;
}

static void serror_take(void)
{
    /* unmask A for a moment: the pending SError enters vec_serror (count, eret); then mask it again */
    __asm__ volatile("msr daifclr, #4\n\tisb\n\tnop\n\tmsr daifset, #4\n\tisb" : : : "memory");
}

static int probe(void *ctx, uint64_t pa, uint32_t *v)
{
    (void)ctx;
    if (serror_pending()) {
        stale_serrors++;
        serror_take();
    }
    uint64_t se = sb_fault.serror_count;
    int ok = sb_probe32(pa, v);
    __asm__ volatile("dsb sy" : : : "memory");
    if (serror_pending()) {
        serror_take();
        ok = 0;
    }
    if (sb_fault.serror_count != se)
        ok = 0;
    if (!ok)
        *v = 0;
    return ok;
}

static uint64_t smc(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)ctx;
    if (id != SBM_SMC_CP || a1 != SBM_SMC_READ_CTRL || a2 > 1 || a3 > 1)   /* the read call only */
        return 0xFFFF;
    return (uint64_t)sb_psci(id, a1, a2, a3);
}

/* stage 3's EL3 calls */
static uint32_t ctrl_half[2][2];
static unsigned ctrl_known[2];          /* bit h = half h was read since the last write */
static uint64_t smc3(void *ctx, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)ctx;
    if (id == SBM_SMC_CP && a1 == SBM_SMC_READ_CTRL && a2 <= 1 && a3 <= 1) {
        uint64_t r = (uint64_t)sb_psci(id, a1, a2, a3);
        if (!(r & 0xFFFF)) {
            ctrl_half[a3][a2] = (uint32_t)r >> 16;
            ctrl_known[a3] |= 1u << a2;
        }
        return r;
    }
    if (id == SBM_SMC_CP && a1 == SBM_SMC_WRITE_CTRL && a3 <= 1 && a2 <= 0xFFFFFFFFULL) {
        const uint32_t bits = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 17);
        uint32_t last = ctrl_half[a3][1] << 16 | ctrl_half[a3][0];
        if (ctrl_known[a3] != 3 || (((uint32_t)a2 ^ last) & ~bits))
            return 0xFFFF;
        ctrl_known[a3] = 0;             /* read it again before the next write */
        return (uint64_t)sb_psci(id, a1, a2, a3);
    }
    if (id == SBM_SMC_SSS_CLK && a1 <= 1 && !a2 && !a3)
        return (uint64_t)sb_psci(id, a1, a2, a3);
    if (id == SBM_SMC_CP && a1 == SBM_SEC_REINIT && !a2 && !a3)
        return (uint64_t)sb_psci(id, a1, a2, a3);
    if (id == SBM_SMC_CP && a1 == (SBM_SEC_NORMAL | SBM_MODEM_IF_PA) && a2 && a2 <= 0x10000 && a3 &&
        a3 <= SBM_DT_CP_SIZE - 0x10000)
        return (uint64_t)sb_psci(id, a1, a2, a3);
    return 0xFFFF;
}

static int link_on;                     /* stage 4 only: the message interrupt and its acknowledge */
static int mmio_wr(void *ctx, uint64_t pa, uint32_t v)
{
    (void)ctx;
    int ok = (pa >= SBM_PMU_PA + 0x1320 && pa <= SBM_PMU_PA + 0x1338 && !(pa & 3)) ||
             pa == SBM_PMU_PA + SBM_PMU_CENTRAL_SEQ_CP_CFG ||
             (pa >= SBM_MBOX_PA + SBM_MBOX_ISSR(0) && pa <= SBM_MBOX_PA + SBM_MBOX_ISSR(63) && !(pa & 3)) ||
             (pa == SBM_MBOX_PA + SBM_MBOX_INTGR1 && (v == 2 || (v == 1 && link_on))) ||
             (pa == SBM_MBOX_PA + SBM_MBOX_INTCR0 && v == (1u << 16) && link_on) ||
             (pa == SBM_AP2CP_CFG_PA && v == 1);
    if (!ok)
        return 0;
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" : : : "memory");
    return 1;
}

#define CARVE_END 0x08000000U           /* the CP memory + VSS: below the IPC region */
static int carve_copy(uint32_t off, void *dst, const void *src, uint32_t len)
{
    if (off >= CARVE_END || len > CARVE_END - off)
        return 0;
    if (serror_pending()) {
        stale_serrors++;
        serror_take();
    }
    uint64_t se = sb_fault.serror_count, c = sb_fault.count;
    sb_probe_depth++;
    memcpy(dst, src, len);
    __asm__ volatile("dsb sy" : : : "memory");
    sb_probe_depth--;
    int ok = sb_fault.count == c;
    if (serror_pending()) {
        serror_take();
        ok = 0;
    }
    return ok && sb_fault.serror_count == se;
}
static int carve_write(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    (void)ctx;
    void *d = (void *)(uintptr_t)(SBM_MODEM_IF_PA + off);
    int ok = carve_copy(off, d, src, len);
    sb_dcache_clean_inval(d, len);      /* a no-op on the non-cacheable map; kept if the map ever changes */
    return ok;
}
static int carve_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    (void)ctx;
    const void *s = (const void *)(uintptr_t)(SBM_MODEM_IF_PA + off);
    if (off < CARVE_END && len <= CARVE_END - off)
        sb_dcache_clean_inval(s, len);
    return carve_copy(off, dst, s, len);
}
static void udelay(void *ctx, uint32_t us) { (void)ctx; sb_udelay(us); }
static uint64_t now_us(void *ctx) { (void)ctx; return sb_us(); }
static void feed(void *ctx) { (void)ctx; sb_wdt_feed(0); }

static uint64_t radio_first, radio_last;
static int radio_state;                 /* 0 not tried, 1 found, -1 not found */
static uint8_t radio_blk[4096] __attribute__((aligned(64)));
static uint64_t radio_blk_lba = ~0ULL;

static int radio_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (!radio_state)
        radio_state = ufs_up() && gpt_load() > 0 && gpt_find("RADIO", &radio_first, &radio_last) ? 1 : -1;
    if (radio_state < 0)
        return -1;
    uint8_t *d = buf;
    while (len) {
        uint64_t lba = radio_first + off / 4096;
        uint32_t o = (uint32_t)(off % 4096), k = 4096 - o < len ? 4096 - o : len;
        if (lba > radio_last)
            return -1;
        if (lba != radio_blk_lba) {
            if (sb_blk_read(SB_MED_UFS, 0, lba, 1, radio_blk) != 0) {
                radio_blk_lba = ~0ULL;
                return -1;
            }
            radio_blk_lba = lba;
        }
        memcpy(d, radio_blk + o, k);
        d += k;
        off += k;
        len -= k;
    }
    return 0;
}

static int poke(void *ctx, uint64_t pa, uint32_t v)
{
    (void)ctx;
    if (pa != SBM_MODEM_IF_PA + SBM_DT_IPC_OFF + SBM_SBD_CMD_OFF && pa != SBM_MODEM_IF_PA + SBM_DT_CP_SIZE - 4)
        return 0;
    if (serror_pending()) {
        stale_serrors++;
        serror_take();
    }
    uint64_t se = sb_fault.serror_count, c = sb_fault.count;
    sb_probe_depth++;
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" : : : "memory");
    sb_dcache_clean_inval((const volatile void *)(uintptr_t)pa, 4);   /* to DRAM; the next read comes from it */
    __asm__ volatile("isb" : : : "memory");
    sb_probe_depth--;
    int ok = sb_fault.count == c;
    if (serror_pending()) {
        serror_take();
        ok = 0;
    }
    if (sb_fault.serror_count != se)
        ok = 0;
    return ok;
}

static void barrier(void *ctx)
{
    (void)ctx;
    __asm__ volatile("dmb ish" : : : "memory");
}

static void (*sink)(const char *);
static void log_line(void *ctx, const char *s)
{
    (void)ctx;
    if (sink)
        sink(s);
}

void sbm_sb_plat(struct sbm_plat *p, void (*line)(const char *))
{
    sink = line;
    p->ctx = 0;
    p->probe32 = probe;
    p->log = log_line;
    p->barrier = barrier;
    p->smc = 0;                         /* step 4 only: sbm_sb_plat_step4 */
    p->radio_read = radio_read;
    p->poke32 = 0;
}

void sbm_sb_plat_step4(struct sbm_plat *p)
{
    p->smc = smc;
    p->poke32 = poke;
}

void sbm_sb_plat_stage4(struct sbm_plat *p, uint32_t ipc_off)
{
    sbm_sb_plat_stage3(p);
    link_on = 1;
    p->ipc = (uint8_t *)(uintptr_t)(SBM_MODEM_IF_PA + ipc_off);   /* non-cacheable in libsb's map */
}

void sbm_sb_plat_stage3(struct sbm_plat *p)
{
    p->smc = smc3;
    p->poke32 = 0;
    p->mmio_wr32 = mmio_wr;
    p->carve_write = carve_write;
    p->carve_read = carve_read;
    p->udelay = udelay;
    p->now_us = now_us;
    p->feed = feed;
}

/* ---- the image: RADIO (TOC, map, BOOT/MAIN/VSS) and EFS's nv_data.bin, read only, into RAM ------- */

struct part { uint64_t first, last, blk_lba; uint8_t *blk; };
static int part_read(struct part *pt, uint64_t off, void *buf, uint32_t len)
{
    uint8_t *d = buf;
    while (len) {
        uint64_t lba = pt->first + off / 4096;
        uint32_t o = (uint32_t)(off % 4096), k = 4096 - o < len ? 4096 - o : len;
        if (lba > pt->last)
            return -1;
        if (o == 0 && k == 4096 && len >= 4096) {          /* whole blocks straight into the buffer */
            uint64_t n = len / 4096, room = pt->last - lba + 1;
            if (n > room)
                n = room;
            if (n > 256)
                n = 256;
            if (sb_blk_read(SB_MED_UFS, 0, lba, n, d) != 0)
                return -1;
            d += n * 4096;
            off += n * 4096;
            len -= (uint32_t)(n * 4096);
            sb_wdt_feed(0);
            continue;
        }
        if (lba != pt->blk_lba) {
            if (sb_blk_read(SB_MED_UFS, 0, lba, 1, pt->blk) != 0) {
                pt->blk_lba = ~0ULL;
                return -1;
            }
            pt->blk_lba = lba;
        }
        memcpy(d, pt->blk + o, k);
        d += k;
        off += k;
        len -= k;
    }
    return 0;
}
/* EFS reads go through the journal overlay once jbd2_scan has built it (never written back anywhere) */
static const struct jbd2 *efs_journal;
static uint32_t efs_bs = 4096;
static uint8_t efs_jblk[4096] __attribute__((aligned(64)));
static int efs_raw(void *ctx, uint64_t off, void *dst, uint32_t len) { return part_read(ctx, off, dst, len); }
static int efs_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    return jbd2_read(efs_journal, efs_bs, efs_raw, ctx, off, dst, len, efs_jblk, sizeof(efs_jblk));
}

struct names { char line[160]; unsigned n, len; int seen; void (*out)(const char *); };
static void name_cb(void *ctx, const char *name, unsigned len, uint32_t ino)
{
    struct names *nm = ctx;
    (void)ino;
    nm->n++;
    if (len == 11 && !memcmp(name, "nv_data.bin", 11))
        nm->seen = 1;
    if (nm->len + len + 2 > 70) {
        sbm_redact_text(nm->line, sizeof(nm->line));
        nm->out(nm->line);
        sb_strcpy(nm->line, sizeof(nm->line), "EFS: / ");
        nm->len = 7;
    }
    for (unsigned i = 0; i < len && nm->len + 2 < sizeof(nm->line); ++i)
        nm->line[nm->len++] = (name[i] >= 0x20 && name[i] < 0x7F) ? name[i] : '?';
    nm->line[nm->len++] = ' ';
    nm->line[nm->len] = 0;
}

/* EFS /nv_data.bin, read only: the superblock facts, the journal (replayed in memory if the file system needs
 * recovery), the root's names, the file. Every step says how it went. */
static struct jbd2_map jmap[8192];
static struct jbd2 jrn;
static int efs_nv(uint8_t *d, uint32_t size, void (*line)(const char *))
{
    static uint8_t blk[4096] __attribute__((aligned(64)));
    static struct ext4 fs;
    static struct part efs;
    uint8_t sb[256];
    char s[160];
    uint32_t ino;
    uint8_t inode[EXT4_INODE_MAX];
    efs.blk = blk;
    efs.blk_lba = ~0ULL;
    efs_journal = 0;
    if (!gpt_find("EFS", &efs.first, &efs.last)) {
        line("EFS: no GPT partition named EFS");
        return SBM_ENOENT;
    }
    sbm_fmt(s, sizeof(s), "EFS: partition %u KiB", (unsigned)((efs.last - efs.first + 1) * 4));
    line(s);
    if (part_read(&efs, 1024, sb, sizeof(sb)) != 0) {
        line("EFS: superblock read failed");
        return SBM_EIO;
    }
    uint32_t magic = sb[0x38] | sb[0x39] << 8, logbs = sbm_le32(sb + 0x18), compat = sbm_le32(sb + 0x5C),
             incompat = sbm_le32(sb + 0x60), ro = sbm_le32(sb + 0x64), jinum = sbm_le32(sb + 0xE0),
             state = sb[0x3A] | sb[0x3B] << 8;
    sbm_fmt(s, sizeof(s), "EFS: magic 0x%04x bs %u compat 0x%x incompat 0x%x ro 0x%x", magic, 1024u << (logbs & 7),
            compat, incompat, ro);
    line(s);
    sbm_fmt(s, sizeof(s), "EFS: state 0x%x journal inode %u needs_recovery %s", state, jinum,
            (incompat & 0x4) ? "YES" : "no");
    line(s);
    if (magic != 0xEF53) {
        line("EFS: not ext4 (f2fs? another layout): stop");
        return SBM_EINVAL;
    }
    fs.read = efs_read;
    fs.ctx = &efs;
    int e = ext4_open(&fs);
    if (e) {
        sbm_fmt(s, sizeof(s), "EFS: ext4_open refused: %d", e);
        line(s);
        return SBM_EINVAL;
    }
    efs_bs = fs.bs;
    if ((incompat & 0x4) && (compat & 0x4) && jinum) {
        e = jbd2_scan(&fs, jinum, &jrn, jmap, sizeof(jmap) / sizeof(jmap[0]));
        sbm_fmt(s, sizeof(s), "EFS journal: %d %s start %u seq %u tx %u blocks %u revoked %u%s", e,
                jrn.why ? jrn.why : "ok", jrn.start, jrn.seq0, jrn.transactions, jrn.n, jrn.skipped_revoked,
                jrn.dropped_incomplete ? " (+1 incomplete, ignored)" : "");
        line(s);
        if (e)
            return SBM_EINVAL;
        efs_journal = &jrn;
        efs.blk_lba = ~0ULL;
        if ((e = ext4_open(&fs)) != 0) {    /* again, through the replayed blocks */
            sbm_fmt(s, sizeof(s), "EFS: ext4_open after the replay refused: %d", e);
            line(s);
            return SBM_EINVAL;
        }
    } else if (incompat & 0x4) {
        line("EFS: needs recovery but has no journal inode: stop");
        return SBM_EINVAL;
    }
    struct names nm;
    memset(&nm, 0, sizeof(nm));
    nm.out = line;
    sb_strcpy(nm.line, sizeof(nm.line), "EFS: / ");
    nm.len = 7;
    e = ext4_list(&fs, 2, name_cb, &nm);
    if (nm.len > 7) {
        sbm_redact_text(nm.line, sizeof(nm.line));
        line(nm.line);
    }
    sbm_fmt(s, sizeof(s), "EFS: root: %u names (list %d), nv_data.bin %s", nm.n, e, nm.seen ? "present" : "ABSENT");
    line(s);
    if ((e = ext4_lookup(&fs, "/nv_data.bin", &ino)) != 0 || (e = ext4_inode(&fs, ino, inode)) != 0) {
        sbm_fmt(s, sizeof(s), "EFS: /nv_data.bin lookup failed: %d", e);
        line(s);
        return SBM_ENOENT;
    }
    uint32_t mode = inode[0] | inode[1] << 8, flags = sbm_le32(inode + 0x20);
    sbm_fmt(s, sizeof(s), "EFS: nv_data.bin inode %u mode 0x%x flags 0x%x size %u", ino, mode, flags,
            (unsigned)ext4_size(inode));
    line(s);
    if ((mode & 0xF000) != 0x8000 || (flags & 0x10000000)) {   /* a regular file, not inline data */
        line("EFS: nv_data.bin is not a plain regular file: stop");
        return SBM_EINVAL;
    }
    if (ext4_size(inode) != size) {
        sbm_fmt(s, sizeof(s), "EFS: nv_data.bin is %u bytes, the TOC says %u", (unsigned)ext4_size(inode), size);
        line(s);
        return SBM_EINVAL;
    }
    for (uint32_t o = 0; o < size; o += 4096)
        if ((e = ext4_read(&fs, inode, o, d + o, size - o < 4096 ? size - o : 4096)) != 0) {
            sbm_fmt(s, sizeof(s), "EFS: nv_data.bin read failed at 0x%x: %d", o, e);
            line(s);
            return SBM_EIO;
        }
    return 0;
}

int sbm_sb_load_image(struct sbm_image *im, void (*line)(const char *))
{
    static uint8_t blk[4096] __attribute__((aligned(64)));
    struct part radio = { 0, 0, ~0ULL, blk };
    uint8_t head[0x400];
    char s[160];
    memset(im, 0, sizeof(*im));
    if (!ufs_up() || gpt_load() <= 0 || !gpt_find("RADIO", &radio.first, &radio.last)) {
        line("load: no RADIO partition");
        return SBM_ENOENT;
    }
    if (part_read(&radio, 0, head, sizeof(head)) != 0)
        return SBM_EIO;
    uint64_t size = (radio.last - radio.first + 1) * 4096;
    int r = sbm_toc_parse(head, size, &im->toc);
    sbm_memmap_parse(head + SBM_MEMMAP_OFF, &im->mm);
    if (r < 0 || !im->mm.valid || (r = sbm_plan_build(&im->toc, &im->mm, &im->plan)) < 0) {
        sbm_fmt(s, sizeof(s), "load: RADIO's TOC / map / plan refused (%s, map %s)", sbm_strerror(r),
                im->mm.valid ? "valid" : "invalid");
        line(s);
        return r < 0 ? r : SBM_EINVAL;
    }
    for (unsigned k = 0; k < im->plan.n; ++k) {
        const struct sbm_load *l = &im->plan.l[k];
        uint8_t *d = sb_alloc(l->size);
        if (!d)
            return SBM_ENOSPC;
        if (l->src == SBM_SRC_RADIO) {
            sbm_fmt(s, sizeof(s), "load: RADIO %s 0x%x bytes at 0x%x", l->name, l->size, l->src_off);
            line(s);
            if (part_read(&radio, l->src_off, d, l->size) != 0)
                return SBM_EIO;
        } else {
            line("load: EFS /nv_data.bin (read only)");
            int e = efs_nv(d, l->size, line);
            if (e < 0)
                return e;
        }
        im->data[k] = d;
    }
    return 0;
}

unsigned sbm_sb_stale_serrors(void) { return stale_serrors; }
