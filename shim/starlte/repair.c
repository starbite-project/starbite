/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Recovery's repair and reset tools: see repair.h, docs/STARBITE-RECOVERY.md section 8.
 *
 * Writes, all after a plan the user confirmed: the card store's head from its backup (sd.c's fence
 * slot 2 opened for p2 blocks 0..6 only during the write), the BOOT partition from a CRC-checked
 * store entry (only BOOT's LBA range, each block read back), and volatile store entries
 * (store_volatile_write). Everything is staged in RP_BUF, the OS staging area, unused in recovery.
 * No libc, no pointer tables (no absolute relocations in the loader Image).
 */
#include <stdint.h>
#include "repair.h"
#include "store.h"
#include "sd.h"
#include "ufs.h"
#include "sbfmt.h"
#include "post.h"
#include "hw.h"

void star_pmsg(const char *s);

#define RP_BUF     0x8c000000UL         /* xnu_boot.c STAGE .. UEFI_FD: 64 MiB */
#define RP_BUF_END 0x90000000UL
#define RP_HEAD    0x8bf00000UL         /* 2 x 28 KiB heads (write-back RAM below STAGE) */
#define PMSG_ZONE  0xFED14000UL
#define DMSG_ZONE  0xFED10000UL
#define LOG_PAGE   0x91FFF000UL

static void line(struct fw_out *o, const char *a, const char *b)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, a);
    if (b)
        fwb_s(&s, b);
    o->line(o->u, s.b);
    struct fwb p;
    fwb_init(&p);
    fwb_s(&p, "STARBITE repair: ");
    fwb_s(&p, s.b);
    fwb_s(&p, "\n");
    star_pmsg(p.b);
}

static void line_u(struct fw_out *o, const char *a, uint64_t v, const char *b)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, a);
    fwb_u(&s, v);
    if (b)
        fwb_s(&s, b);
    line(o, s.b, 0);
}

static void bad(struct fw_out *o, const char *a, const char *b)
{
    ++o->bad;
    line(o, a, b);
}

static void zero(uintptr_t p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i += 8)
        *(volatile uint64_t *)(p + i) = 0;
}

/* ---- the store check ------------------------------------------------------------------------ */

int rp_store_check(struct fw_out *o, int full)
{
    o->bad = 0;
    int h = store_home();
    if (h < 0) {
        const char *w = store_home_why();
        bad(o, w ? w : "NO OS STORE", 0);
        return o->bad;
    }
    line(o, h == (int)STORE_SD ? "HOME STORE: MICROSD CARD" : "HOME STORE: UFS (USERDATA +16 GIB)", 0);
    int n = store_open((unsigned)h);
    line_u(o, "ENTRIES ", (uint64_t)n, 0);
    line_u(o, "USED MIB ", store_used((unsigned)h) * 4096U >> 20, 0);
    struct star_settings s;
    int ok = settings_load(&s);
    int st = settings_status();
    if (ok)
        line(o, "SETTINGS RECORD OK", 0);
    else
        bad(o, st == 0 ? "NO SETTINGS RECORD (DEFAULTS)" : "SETTINGS RECORD BAD (CRC)", 0);
    struct store_ent e;
    if (store_crashlog(&e))
        line(o, "CRASH LOG ENTRY OK", 0);
    else
        bad(o, "NO CRASH LOG ENTRY", 0);
    if (h == (int)STORE_SD) {
        uint64_t first = 0, blocks = 0;
        struct sbhead_info hi;
        if (sd_store_area(&first, &blocks) && !sd_read4k(first + RP_HEAD_BAK, 7, RP_HEAD) &&
            sbstore_head_check((const uint8_t *)RP_HEAD, blocks, &hi))
            line_u(o, "HEAD BACKUP OK, ENTRIES ", hi.count, 0);
        else
            bad(o, "NO HEAD BACKUP (CARD-STORE.SH WRITES IT)", 0);
    }
    if (!full)
        return o->bad;
    unsigned crc_bad = 0, checked = 0;
    for (unsigned i = 0; store_get((unsigned)h, i, &e); ++i) {
        if (e.flags & SF_VOLATILE)
            continue;
        if (e.size > RP_BUF_END - RP_BUF) {
            bad(o, "TOO BIG TO CHECK HERE: ", e.name);
            continue;
        }
        ++checked;
        int r = store_load(&e, RP_BUF, RP_BUF_END - RP_BUF);
        if (r) {
            ++crc_bad;
            bad(o, r == -2 ? "CRC BAD: " : "READ FAILED: ", e.name);
        }
    }
    struct fwb t;
    fwb_init(&t);
    fwb_u(&t, checked);
    fwb_s(&t, " ENTRIES READ, ");
    fwb_u(&t, crc_bad);
    fwb_s(&t, crc_bad ? " BAD: REFLASH THEM FROM THE MAC" : " BAD");
    line(o, t.b, 0);
    return o->bad;
}

/* ---- the card store's head from its backup -------------------------------------------------- */

static int head_state(struct fw_out *o, uint64_t *first, struct sbhead_info *cur, struct sbhead_info *bak, int *cur_ok)
{
    uint64_t blocks = 0;
    sd_allow();
    if (!sd_store_area(first, &blocks)) {
        bad(o, "NO STORE PARTITION ON THE CARD (MBR TYPE 0xDA)", 0);
        return 0;
    }
    if (sd_read4k(*first, 7, RP_HEAD) || sd_read4k(*first + RP_HEAD_BAK, 7, RP_HEAD + 7 * 4096U)) {
        bad(o, "THE CARD DOES NOT READ", 0);
        return 0;
    }
    *cur_ok = sbstore_head_check((const uint8_t *)RP_HEAD, blocks, cur);
    int bak_ok = sbstore_head_check((const uint8_t *)(RP_HEAD + 7 * 4096U), blocks, bak);
    line(o, *cur_ok ? "HEAD: VALID" : "HEAD: DAMAGED (HEADER OR DIRECTORY)", 0);
    if (bak_ok)
        line_u(o, "BACKUP AT P2 BLOCK 64: VALID, ENTRIES ", bak->count, 0);
    else
        line(o, "BACKUP AT P2 BLOCK 64: NONE OR DAMAGED", 0);
    return bak_ok;
}

int rp_head_plan(struct fw_out *o)
{
    o->bad = 0;
    uint64_t first;
    struct sbhead_info cur, bak;
    int cur_ok = 0;
    int bak_ok = head_state(o, &first, &cur, &bak, &cur_ok);
    if (cur_ok) {
        line(o, "NOTHING TO REPAIR", 0);
        return 0;
    }
    if (!bak_ok) {
        bad(o, "NO GOOD BACKUP: REWRITE THE STORE FROM THE MAC", 0);
        return 0;
    }
    if (!bak.home) {
        bad(o, "THE BACKUP IS NOT A HOME STORE: NOT USED", 0);
        return 0;
    }
    line(o, "WOULD WRITE P2 BLOCKS 0, 1, 3-6 FROM THE BACKUP", 0);
    line(o, "BLOCK 2 (SETTINGS) STAYS AS IT IS", 0);
    return 1;
}

int rp_head_apply(struct fw_out *o)
{
    if (!rp_head_plan(o))
        return -1;
    uint64_t first = 0, blocks = 0;
    if (!sd_store_area(&first, &blocks))
        return -1;
    sd_fence(2, first, 7);
    int r = 0;
    for (unsigned b = 0; b < 7 && !r; ++b) {
        if (b == 2)
            continue;
        uintptr_t src = RP_HEAD + 7 * 4096U + b * 4096U;
        if (sd_write4k(first + b, 1, src) || sd_read4k(first + b, 1, RP_BUF)) {
            r = -2;
            break;
        }
        for (unsigned j = 0; j < 4096U; j += 8)
            if (*(volatile uint64_t *)(RP_BUF + j) != *(volatile uint64_t *)(src + j))
                r = -3;
    }
    sd_fence(2, 0, 0);
    if (r) {
        bad(o, r == -2 ? "WRITE FAILED" : "READ-BACK MISMATCH", 0);
        return r;
    }
    store_rehome();
    line(o, store_home() == (int)STORE_SD ? "REPAIRED: THE CARD STORE OPENS AGAIN" : "WRITTEN, BUT THE STORE STILL DOES NOT OPEN", 0);
    return store_home() == (int)STORE_SD ? 0 : -4;
}

/* ---- BOOT from the store's boot-good.img ------------------------------------------------------ */

static uint64_t boot_first, boot_blocks, img_blocks;
static int img_ok;
static uint32_t img_crc;

static int boot_load(struct fw_out *o)
{
    img_ok = 0;
    uint64_t last = 0;
    if (!ufs_up() || !gpt_find("BOOT", &boot_first, &last)) {
        bad(o, "NO BOOT PARTITION (UFS)", 0);
        return 0;
    }
    boot_blocks = last - boot_first + 1;
    struct store_ent e;
    if (!store_find("boot-good.img", &e)) {
        bad(o, "NO BOOT-GOOD.IMG IN THE STORE", 0);
        line(o, "ADD IT FROM THE MAC: STORE-UPDATE.SH", 0);
        return 0;
    }
    img_blocks = (e.size + 4095U) / 4096U;
    if (img_blocks > boot_blocks || e.size > RP_BUF_END - RP_BUF) {
        bad(o, "BOOT-GOOD.IMG IS BIGGER THAN BOOT", 0);
        return 0;
    }
    line_u(o, "BOOT-GOOD.IMG: KIB ", e.size >> 10, 0);
    int r = store_load(&e, RP_BUF, RP_BUF_END - RP_BUF);
    if (r) {
        bad(o, r == -2 ? "BOOT-GOOD.IMG CRC BAD" : "BOOT-GOOD.IMG READ FAILED", 0);
        return 0;
    }
    zero(RP_BUF + e.size, img_blocks * 4096U - e.size);
    static const char m[8] = { 'A', 'N', 'D', 'R', 'O', 'I', 'D', '!' };
    for (unsigned i = 0; i < 8; ++i)
        if (*(volatile uint8_t *)(RP_BUF + i) != (uint8_t)m[i]) {
            bad(o, "BOOT-GOOD.IMG IS NOT A BOOT IMAGE", 0);
            return 0;
        }
    img_crc = e.crc;
    img_ok = 1;
    return 1;
}

/* blocks of BOOT that differ from the image (UFS reads only) */
static uint64_t boot_diff(void)
{
    uint64_t d = 0;
    for (uint64_t b = 0; b < img_blocks; ++b) {
        if (ufs_read(boot_first + b, 1))
            return ~0ULL;
        for (unsigned j = 0; j < 4096U; j += 8)
            if (*(volatile uint64_t *)(UFS_BUF + j) != *(volatile uint64_t *)(RP_BUF + b * 4096U + j)) {
                ++d;
                break;
            }
    }
    return d;
}

int rp_boot_plan(struct fw_out *o)
{
    o->bad = 0;
    if (!boot_load(o))
        return 0;
    uint64_t d = boot_diff();
    if (d == ~0ULL) {
        bad(o, "BOOT DOES NOT READ", 0);
        return 0;
    }
    if (!d) {
        line(o, "BOOT ALREADY HOLDS THIS IMAGE: NOTHING TO DO", 0);
        return 0;
    }
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "WOULD WRITE ");
    fwb_u(&s, d);
    fwb_s(&s, " OF ");
    fwb_u(&s, img_blocks);
    fwb_s(&s, " BLOCKS OF BOOT");
    line(o, s.b, 0);
    line(o, "IF IT STOPS HALF WAY: DOWNLOAD MODE (VOL DOWN+BIXBY)", 0);
    return 1;
}

int rp_boot_apply(struct fw_out *o)
{
    if (!img_ok && !boot_load(o))
        return -1;
    uint64_t wrote = 0;
    for (uint64_t b = 0; b < img_blocks; ++b) {
        uint64_t lba = boot_first + b;
        if (lba < boot_first || lba >= boot_first + boot_blocks)
            return -1;                         /* never outside BOOT */
        uintptr_t src = RP_BUF + b * 4096U;
        if (ufs_read(lba, 1))
            return bad(o, "BOOT READ FAILED", 0), -2;
        int same = 1;
        for (unsigned j = 0; j < 4096U && same; j += 8)
            same = *(volatile uint64_t *)(UFS_BUF + j) == *(volatile uint64_t *)(src + j);
        if (same)
            continue;
        if (ufs_write_block(lba, src) || ufs_read(lba, 1))
            return bad(o, "BOOT WRITE FAILED", 0), -2;
        for (unsigned j = 0; j < 4096U; j += 8)
            if (*(volatile uint64_t *)(UFS_BUF + j) != *(volatile uint64_t *)(src + j))
                return bad(o, "BOOT READ-BACK MISMATCH", 0), -3;
        ++wrote;
    }
    line_u(o, "BOOT WRITTEN AND READ BACK, BLOCKS ", wrote, 0);
    img_ok = 0;
    return boot_diff() == 0 ? 0 : -4;
}

/* ---- a volatile entry reset ------------------------------------------------------------------- */

static int vol_find(struct fw_out *o, const char *name, struct store_ent *e)
{
    int h = store_home();
    if (h < 0 || !store_find(name, e) || !(e->flags & SF_VOLATILE) || e->medium != (unsigned)h) {
        bad(o, "NO SUCH DATA ENTRY: ", name);
        return 0;
    }
    return 1;
}

int rp_reset_plan(struct fw_out *o, const char *name)
{
    o->bad = 0;
    struct store_ent e;
    if (!vol_find(o, name, &e))
        return 0;
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "WOULD ERASE ");
    fwb_s(&s, name);
    fwb_s(&s, " (");
    fwb_u(&s, e.size >> 10);
    fwb_s(&s, " KIB)");
    line(o, s.b, 0);
    line(o, "ITS SETTINGS AND SAVED DATA START OVER", 0);
    return 1;
}

int rp_reset_apply(struct fw_out *o, const char *name)
{
    struct store_ent e;
    if (!vol_find(o, name, &e))
        return -1;
    uint64_t blocks = (e.size + 4095U) / 4096U;
    zero(RP_BUF, 64U * 4096U);
    for (uint64_t b = 0; b < blocks; b += 64) {
        uint64_t n = blocks - b < 64 ? blocks - b : 64;
        int r = store_volatile_write(name, b, n, RP_BUF);
        if (r)
            return bad(o, r == -3 ? "READ-BACK MISMATCH" : "WRITE FAILED", 0), r;
    }
    line(o, "ERASED AND READ BACK", 0);
    return 0;
}

/* ---- logs to the store ------------------------------------------------------------------------ */

/* The bundle (tools/store/sbstore2.py logs reads it): block 0 = "SBLOGS01", u32 version 1, u32 count,
 * then count x { char name[16], u32 offset, u32 size } at 16; the sections follow, 4 KiB aligned. */
static uint32_t lg_add(uintptr_t base, uint32_t *off, unsigned *count, const char *name, uintptr_t src, uint32_t size,
                       uint32_t cap)
{
    if (*off + size > cap)
        size = *off < cap ? cap - *off : 0;
    uintptr_t d = base + 16 + 24U * *count;
    for (unsigned i = 0; i < 16; ++i)
        *(volatile uint8_t *)(d + i) = 0;
    for (unsigned i = 0; i < 15 && name[i]; ++i)
        *(volatile uint8_t *)(d + i) = (uint8_t)name[i];
    *(volatile uint32_t *)(d + 16) = *off;
    *(volatile uint32_t *)(d + 20) = size;
    for (uint32_t i = 0; i < size; ++i)
        *(volatile uint8_t *)(base + *off + i) = *(volatile const uint8_t *)(src + i);
    *off += (size + 4095U) & ~4095U;
    ++*count;
    return size;
}

int rp_logs_save(struct fw_out *o)
{
    o->bad = 0;
    struct store_ent e;
    if (!vol_find(o, "loader-logs", &e)) {
        line(o, "ADD IT: STORE-UPDATE.SH --VOLATILE LOADER-LOGS=262144", 0);
        return -1;
    }
    uint32_t cap = (uint32_t)(e.size & ~4095ULL);
    zero(RP_BUF, cap);
    uint32_t off = 4096;
    unsigned count = 0;
    lg_add(RP_BUF, &off, &count, "pmsg", PMSG_ZONE, 0x4000, cap);
    lg_add(RP_BUF, &off, &count, "dmesg", DMSG_ZONE, 0x4000, cap);
    lg_add(RP_BUF, &off, &count, "loader-log-page", LOG_PAGE, 4096, cap);
    struct store_ent cl;
    if (store_crashlog(&cl) && !store_read(&cl, 0, CL_BYTES / CL_BLOCK, RP_BUF + 0x01000000UL))
        lg_add(RP_BUF, &off, &count, "crashlog", RP_BUF + 0x01000000UL, CL_BYTES, cap);
    const struct post_report *pr = post_last();
    if (pr && pr->magic == POST_MAGIC)
        lg_add(RP_BUF, &off, &count, "post", (uintptr_t)pr, (uint32_t)sizeof(*pr), cap);
    static const char m[8] = { 'S', 'B', 'L', 'O', 'G', 'S', '0', '1' };
    for (unsigned i = 0; i < 8; ++i)
        *(volatile uint8_t *)(RP_BUF + i) = (uint8_t)m[i];
    *(volatile uint32_t *)(RP_BUF + 8) = 1;
    *(volatile uint32_t *)(RP_BUF + 12) = count;
    uint64_t blocks = (off < cap ? off : cap) / 4096U;
    for (uint64_t b = 0; b < blocks; b += 64) {
        uint64_t n = blocks - b < 64 ? blocks - b : 64;
        int r = store_volatile_write("loader-logs", b, n, RP_BUF + b * 4096U);
        if (r)
            return bad(o, r == -3 ? "LOGS: READ-BACK MISMATCH" : "LOGS: WRITE FAILED", 0), r;
    }
    line_u(o, "LOGS SAVED TO LOADER-LOGS, SECTIONS ", count, 0);
    line(o, "MAC: SBSTORE2.PY LOGS (AFTER STORE-PULL.SH SD)", 0);
    return 0;
}
