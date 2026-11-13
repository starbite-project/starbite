/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's state slot on the phone (docs/STARBITE-OS.md 6.5): the store entry "sbos-state" (1 MiB, the
 * volatile flag: made on the Mac with `sbstore2.py build --volatile sbos-state=1048576`). Its first 128 blocks
 * are half 0, the next 128 half 1 (state.c's format), on the home store (UFS, or the microSD card's store once
 * it is home: docs/STORE-ON-CARD.md). libsb's store_volatile_grant checks the entry and lets the fence write
 * exactly its blocks (sb_write_volatile: UFS forced unit access then a cache flush, the card programmed then
 * read back); every write is read back here too. Without the entry nothing is written anywhere and the settings last until a restart.
 * UFS is S-Boot's running host (no link start): brought up at the first read, after the first frame. */
#include "sb.h"
#include "store.h"
#include "kern.h"

#define SLOT_NAME "sbos-state"
#define HALF_BLOCKS (STATE_HALF / 4096U)

static int st;                              /* 0 not tried, 1 granted, -1 none */
static struct store_ent ent;
static unsigned med;                        /* SB_MED_UFS or SB_MED_SD: the medium the grant is on */
static uint8_t *back;                       /* read-back buffer */
static int lent;                            /* Software Update (update.c) took the fence's one grant: take it back first */

/* update.c: the fence's grant is another entry's now (a window of an update slot) */
void hw_slot_lend(void)
{
    lent = 1;
}

static int find(void)
{
    if (st)
        return st > 0;
    st = -1;
    if (!store_volatile_grant(SLOT_NAME, 2U * STATE_HALF, &ent)) {
        sb_log("slot: no \"sbos-state\" store entry (volatile, 1 MiB): settings are not saved");
        return 0;
    }
    back = sb_alloc(STATE_HALF);
    if (!back)
        return 0;
    st = 1;
    med = ent.medium == STORE_SD ? SB_MED_SD : SB_MED_UFS;
    struct ub u;
    ub_init(&u);
    ub_s(&u, med == SB_MED_SD ? "slot: \"sbos-state\" at sd:" : "slot: \"sbos-state\" at ufs:");
    ub_dec(&u, ent.lba);
    ub_s(&u, ", ");
    ub_dec(&u, ent.size / 4096U);
    ub_s(&u, " blocks");
    sb_log(u.b);
    return 1;
}

/* the slot's medium (SB_MED_SD / SB_MED_UFS), -1 no slot (the drivers' sd probe writes its inactive half on the card) */
int hw_slot_medium(void)
{
    return find() ? (int)med : -1;
}

int hw_slot_read(uint8_t *h0, uint8_t *h1)
{
    if (!find())
        return 0;
    if (sb_blk_read(med, 0, ent.lba, HALF_BLOCKS, h0) ||
        sb_blk_read(med, 0, ent.lba + HALF_BLOCKS, HALF_BLOCKS, h1)) {
        sb_log("slot: the read failed: settings are not restored");
        return 0;
    }
    return 1;
}

/* 1 written and read back, 0 failed, -1 a dry run (nothing written) */
int hw_slot_write(int half, const uint8_t *src, uint32_t n)
{
    if (st <= 0 || half < 0 || half > 1 || !n || n > STATE_HALF)
        return 0;
    uint64_t blocks = (n + 4095U) / 4096U, lba = ent.lba + (uint64_t)half * HALF_BLOCKS;
    if (lent) {
        struct store_ent e;
        if (!store_volatile_grant(SLOT_NAME, 2U * STATE_HALF, &e) || e.lba != ent.lba) {
            sb_log("slot: the state entry's grant did not come back after the update's writes");
            return 0;
        }
        lent = 0;
    }
    int r = sb_write_volatile(med, lba, blocks, src);
    if (r == SB_W_DRYRUN)
        return -1;                          /* logged by the fence; nothing written */
    if (r)
        return 0;
    if (sb_blk_read(med, 0, lba, blocks, back)) {
        sb_log("slot: the read-back failed");
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (back[i] != src[i]) {
            sb_log("slot: the read-back differs");
            return 0;
        }
    return 1;
}
