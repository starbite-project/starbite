/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw on libsb (Starbite OS and test images): the struct bcmw_os for a bare-metal image.
 *  - registers: the SoC is identity-mapped Device memory (mmu.c); the first read of a block whose
 *    clock is not known to be on goes through sb_probe32, which survives a bus fault;
 *  - delays and the clock: sb_udelay, sb_us;
 *  - DMA memory: 0xa6000000..0xa6800000, inside libsb's non-cacheable window (mmu.c: 0xa0000000..
 *    0xa7ffffff, the RAM test window; a Wi-Fi image does not run the RAM test). Uncached, so no
 *    cache maintenance. Below 4 GiB; the chip is given the physical address (the PCIe SysMMU in
 *    bypass). Starbite OS proper should give the radio its own carve-out: see docs/WIFI-BCM4361.md;
 *  - files: read-only from the phone's own VENDOR partition (GPT name "VENDOR"), ext4, /firmware/
 *    (LineageOS) or /etc/wifi/ (Samsung's Android), into the arena. Never shipped in an image.
 */
#include "sb.h"
#include "ufs.h"
#include "../bcmw.h"
#include "../ext4.h"
#include "bcmw_sb.h"

#define DMA_BASE 0xa6000000UL
#define DMA_END  0xa6800000UL
_Static_assert(DMA_BASE >= SB_TEST_BASE && DMA_END <= SB_TEST_BASE + SB_TEST_SIZE, "the radio's DMA leaves the non-cacheable window");
_Static_assert(DMA_END <= 0xa7000000UL, "the radio's DMA meets the Utility OS's USB window at 0xa7000000");

static uintptr_t dma_next = DMA_BASE;
static uint64_t vendor_first, vendor_last;
static struct ext4 fs;
static int fs_state;                /* 0 not tried, 1 open, -1 failed */
static uint8_t *blk;
static uint64_t blk_lba = ~0ULL;
static struct { char name[40]; const uint8_t *data; uint32_t len; } files[8];
static unsigned nfiles;

static uint32_t sb_rd32(void *ctx, uint64_t pa) { (void)ctx; return *(volatile uint32_t *)(uintptr_t)pa; }
static void sb_wr32(void *ctx, uint64_t pa, uint32_t v)
{
    (void)ctx;
    if (bcmw_write_allowed(pa))     /* the core checked; this is the second lock */
        *(volatile uint32_t *)(uintptr_t)pa = v;
}
static uint16_t sb_rd16(void *ctx, uint64_t pa) { (void)ctx; return *(volatile uint16_t *)(uintptr_t)pa; }
static void sb_wr16(void *ctx, uint64_t pa, uint16_t v)
{
    (void)ctx;
    if (bcmw_write_allowed(pa & ~3ULL))
        *(volatile uint16_t *)(uintptr_t)pa = v;
}
static int sb_probe(void *ctx, uint64_t pa, uint32_t *v) { (void)ctx; return sb_probe32(pa, v); }
static void sb_wmb(void *ctx) { (void)ctx; __asm__ volatile("dsb st" : : : "memory"); }
static void sb_delay(void *ctx, uint32_t us) { (void)ctx; sb_udelay(us); }
static uint64_t sb_now(void *ctx) { (void)ctx; return sb_us(); }

static void *sb_dma(void *ctx, uint32_t size, uint32_t align, uint64_t *bus)
{
    uintptr_t p;
    (void)ctx;
    if (align < 64)
        align = 64;
    p = (dma_next + align - 1) & ~(uintptr_t)(align - 1);
    if (p + size > DMA_END)
        return 0;
    dma_next = p + size;
    *bus = p;
    return (void *)p;
}

/* ---- VENDOR ---------------------------------------------------------------------------------- */

static int vendor_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *d = dst;
    (void)ctx;
    while (len) {
        uint64_t lba = off / 4096;
        uint32_t in = (uint32_t)(off % 4096), n = 4096 - in < len ? 4096 - in : len;
        if (vendor_first + lba > vendor_last)
            return -1;
        if (lba != blk_lba) {
            if (sb_blk_read(SB_MED_UFS, 0, vendor_first + lba, 1, blk))
                return -1;
            blk_lba = lba;
        }
        memcpy(d, blk + in, n);
        d += n, off += n, len -= n;
    }
    return 0;
}

int bcmw_sb_vendor(void (*say)(const char *))
{
    int e;
    if (fs_state)
        return fs_state > 0;
    fs_state = -1;
    if (!ufs_up() || gpt_load() <= 0 || !gpt_find("VENDOR", &vendor_first, &vendor_last)) {
        say("wifi: no VENDOR partition (UFS or the GPT did not answer)");
        return 0;
    }
    blk = sb_alloc(4096);
    fs.read = vendor_read;
    e = ext4_open(&fs);
    if (e) {
        say(e == EXT4_E_NOTEXT4 ? "wifi: VENDOR is not ext4 (EROFS?): its files cannot be read here"
                                : "wifi: VENDOR's ext4 does not open");
        return 0;
    }
    say("wifi: VENDOR is ext4: the radio's files are read from it (read only)");
    fs_state = 1;
    return 1;
}

static int sb_file(void *ctx, const char *name, const uint8_t **data, uint32_t *len)
{
    static const char *const dirs[] = { "firmware/", "etc/wifi/" };
    (void)ctx;
    for (unsigned i = 0; i < nfiles; i++)
        if (sb_streq(files[i].name, name)) {
            *data = files[i].data, *len = files[i].len;
            return 0;
        }
    if (fs_state <= 0 || nfiles == sizeof(files) / sizeof(files[0]))
        return -1;
    for (unsigned d = 0; d < 2; d++) {
        char path[64];
        uint32_t ino;
        uint8_t inode[EXT4_INODE_MAX];
        uint64_t size;
        uint8_t *buf;
        sb_strcpy(path, sizeof(path), dirs[d]);
        sb_strcpy(path + sb_strlen(path), sizeof(path) - sb_strlen(path), name);
        if (ext4_lookup(&fs, path, &ino) || ext4_inode(&fs, ino, inode))
            continue;
        size = ext4_size(inode);
        if (size == 0 || size > (4U << 20) || !(buf = sb_alloc(size)))
            return -1;
        if (ext4_read(&fs, inode, 0, buf, (uint32_t)size))
            return -1;
        sb_strcpy(files[nfiles].name, sizeof(files[nfiles].name), name);
        files[nfiles].data = buf;
        files[nfiles].len = (uint32_t)size;
        nfiles++;
        *data = buf, *len = (uint32_t)size;
        sb_logf("wifi: read from VENDOR: /", path);
        return 0;
    }
    return -1;
}

void bcmw_sb_os(struct bcmw_os *os, void (*log)(void *, const char *), void (*event)(void *, const struct bcmw_event *))
{
    memset(os, 0, sizeof(*os));
    os->rd32 = sb_rd32, os->wr32 = sb_wr32, os->rd16 = sb_rd16, os->wr16 = sb_wr16;
    os->probe32 = sb_probe, os->wmb = sb_wmb, os->udelay = sb_delay, os->now_us = sb_now;
    os->dma_alloc = sb_dma, os->file = sb_file, os->log = log, os->event = event;
}
