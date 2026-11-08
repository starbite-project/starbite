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
 *         interrupt), AP2CP_CFG = 1. Nothing else.
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

static int mmio_wr(void *ctx, uint64_t pa, uint32_t v)
{
    (void)ctx;
    int ok = (pa >= SBM_PMU_PA + 0x1320 && pa <= SBM_PMU_PA + 0x1338 && !(pa & 3)) ||
             pa == SBM_PMU_PA + SBM_PMU_CENTRAL_SEQ_CP_CFG ||
             (pa >= SBM_MBOX_PA + SBM_MBOX_ISSR(0) && pa <= SBM_MBOX_PA + SBM_MBOX_ISSR(63) && !(pa & 3)) ||
             (pa == SBM_MBOX_PA + SBM_MBOX_INTGR1 && v == 2) || (pa == SBM_AP2CP_CFG_PA && v == 1);
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
static int efs_read(void *ctx, uint64_t off, void *dst, uint32_t len) { return part_read(ctx, off, dst, len); }

int sbm_sb_load_image(struct sbm_image *im, void (*line)(const char *))
{
    static uint8_t blk[4096] __attribute__((aligned(64))), efs_blk[4096] __attribute__((aligned(64)));
    static struct ext4 fs;
    struct part radio = { 0, 0, ~0ULL, blk }, efs = { 0, 0, ~0ULL, efs_blk };
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
            uint32_t ino;
            uint8_t inode[EXT4_INODE_MAX];
            line("load: EFS /nv_data.bin (read only)");
            if (!gpt_find("EFS", &efs.first, &efs.last))
                return SBM_ENOENT;
            fs.read = efs_read;
            fs.ctx = &efs;
            if (ext4_open(&fs) != 0 || ext4_lookup(&fs, "/nv_data.bin", &ino) != 0 || ext4_inode(&fs, ino, inode) != 0) {
                line("load: EFS: no ext4 /nv_data.bin");
                return SBM_ENOENT;
            }
            if (ext4_size(inode) != l->size) {
                sbm_fmt(s, sizeof(s), "load: nv_data.bin is %u bytes, the TOC says %u", (unsigned)ext4_size(inode), l->size);
                line(s);
                return SBM_EINVAL;
            }
            for (uint32_t o = 0; o < l->size; o += 4096)
                if (ext4_read(&fs, inode, o, d + o, l->size - o < 4096 ? l->size - o : 4096) != 0)
                    return SBM_EIO;
        }
        im->data[k] = d;
    }
    return 0;
}

unsigned sbm_sb_stale_serrors(void) { return stale_serrors; }
