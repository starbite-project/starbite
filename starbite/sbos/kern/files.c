/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS files (docs/STARBITE-OS.md 5: the files group), v0.2: read-only files on the microSD card's exFAT
 * partition, through libsb's card driver and the Utility OS's exFAT reader (starbite/utility/exfat.c). The card
 * is brought up on the first read, not at boot (it costs time and power). Nothing is ever written. */
#include "sb.h"
#include "sd.h"
#include "kern.h"
#include "exfat.h"

static struct exfat fs;
static uint8_t *cbuf;
static int st;                              /* 0 not tried, 1 mounted, -1 failed (not retried for 30 s) */
static uint64_t st_t;
static struct exfat_ent scratch[64];

static int card_rd(uint64_t sector, uint32_t count, void *dst)
{
    return sd_read(sector, count, (uintptr_t)dst);
}

static int mount(void)
{
    if (st > 0)
        return 1;
    if (st < 0 && sb_ms() - st_t < 30000U)
        return 0;
    st_t = sb_ms();
    if (!cbuf)
        cbuf = sb_alloc(1U << 17);
    uint64_t start;
    sd_allow();
    if (!cbuf || !sd_up()) {
        st = -1;
        sb_log("files: no card");
        return 0;
    }
    int r = exfat_find_volume(card_rd, cbuf, &start);
    if (!r)
        r = exfat_mount(&fs, card_rd, start, cbuf, 1U << 17);
    st = r ? -1 : 1;
    sb_log(st > 0 ? "files: card mounted (exFAT, read only)" : "files: the card has no exFAT volume");
    return st > 0;
}

int kfile_read(const char *path, void *dst, uint32_t cap)
{
    struct exfat_ent e;
    if (!mount())
        return SBOS_EAGAIN;
    if (exfat_walk(&fs, path, &e, scratch, (int)(sizeof(scratch) / sizeof(scratch[0]))) || e.dir)
        return SBOS_EINVAL;
    uint64_t n = e.size < cap ? e.size : cap;
    if (n && exfat_read_chain(&fs, e.first, e.nofat, 0, dst, n))
        return SBOS_EAGAIN;
    return (int)n;
}
