/* SPDX-License-Identifier: GPL-2.0-only
 * The write fence (docs/STARBITE-MASTER-PLAN.md section 4.2, "The write fence"), dry-run in v1.
 *
 * Every block write libsb could ever make goes through sb_write(medium, lba, n). It classifies the
 * target against the live GPT by name and range:
 *   never   EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT
 *           head (blocks before the first partition) and tail (after the last), anything that
 *           crosses a partition boundary or is in no partition, any partition not in the lists
 *           below. The boot LUNs are not addressable at all: libsb only knows LU0 and the card.
 *   tier 2  BOOT, RECOVERY, UP_PARAM
 *   tier 1  SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA (the Apple slot and the
 *           Starbite store inside it), the microSD card
 * In v1 nothing is written whatever the tier: sb_write logs the decision ("would write ...") in
 * the Starbite log and returns SB_W_DRYRUN (or SB_W_NEVER / SB_W_TIER / SB_W_RANGE). There is no
 * UFS or SD write command in libsb to call (ufs.c accepts read opcodes only; sd.c never writes):
 * EN4 adds them behind this function.
 * Not done yet: the cross-check against the stock PIT (device/pit.txt) the plan asks for.
 */
#include "sb.h"
#include "ufs.h"

static int eq(const char *a, const char *b)
{
    return sb_streq(a, b);
}

int sb_never_part(const char *g)
{
    return eq(g, "EFS") || eq(g, "CPEFS") || eq(g, "PARAM") || eq(g, "KEYSTORAGE") || eq(g, "PERSISTENT") ||
           eq(g, "STEADY") || eq(g, "BOTA0") || eq(g, "BOTA1") || eq(g, "BOTA2") || eq(g, "DTB") ||
           eq(g, "DTBO");
}

static int tier2_part(const char *g)
{
    return eq(g, "BOOT") || eq(g, "RECOVERY") || eq(g, "UP_PARAM");
}

static int tier1_part(const char *g)
{
    return eq(g, "SYSTEM") || eq(g, "VENDOR") || eq(g, "ODM") || eq(g, "A16DATA") || eq(g, "WINESP") ||
           eq(g, "WINDOWS") || eq(g, "USERDATA");
}

int sb_write_decide(unsigned medium, uint64_t lba, uint64_t n, struct sb_wdecide *d)
{
    d->tier = 3;
    d->part[0] = 0;
    d->first = d->last = 0;
    d->why = "unknown target";
    if (!n) {
        d->why = "empty write";
        return SB_W_RANGE;
    }
    if (medium == SB_MED_SD) {
        sb_strcpy(d->part, sizeof(d->part), "card");
        d->tier = 1;
        d->why = "the microSD card";
        return 0;
    }
    if (medium != SB_MED_UFS) {
        d->why = "boot LUNs and other media are never writable";
        return SB_W_NEVER;
    }
    if (gpt_load() <= 0) {
        d->why = "no readable GPT: refused";
        return SB_W_RANGE;
    }
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < gpt_n; ++i) {
        if (gpt_first[i] < lo)
            lo = gpt_first[i];
        if (gpt_last[i] > hi)
            hi = gpt_last[i];
    }
    if (lba < lo || lba + n - 1 > hi) {
        sb_strcpy(d->part, sizeof(d->part), lba < lo ? "GPT/PIT head" : "GPT tail");
        d->why = "the partition table region: never";
        return SB_W_NEVER;
    }
    for (int i = 0; i < gpt_n; ++i) {
        if (lba < gpt_first[i] || lba > gpt_last[i])
            continue;
        sb_strcpy(d->part, sizeof(d->part), gpt_name[i]);
        d->first = gpt_first[i];
        d->last = gpt_last[i];
        if (lba + n - 1 > gpt_last[i]) {
            d->why = "crosses the end of the partition: refused";
            return SB_W_RANGE;
        }
        if (sb_never_part(gpt_name[i])) {
            d->why = "never-touch partition";
            return SB_W_NEVER;
        }
        if (tier2_part(gpt_name[i])) {
            d->tier = 2;
            d->why = "boot chain: tier 2 (type the partition name)";
            return 0;
        }
        if (tier1_part(gpt_name[i])) {
            d->tier = 1;
            d->why = "OS partition: tier 1";
            return 0;
        }
        d->why = "not on the writable list: never";
        return SB_W_NEVER;
    }
    d->why = "between partitions: refused";
    return SB_W_RANGE;
}

int sb_write(unsigned medium, uint64_t lba, uint64_t n, const void *src, int armed_tier)
{
    struct sb_wdecide d;
    struct ub u;
    (void)src;
    int r = sb_write_decide(medium, lba, n, &d);
    ub_init(&u);
    ub_s(&u, "fence: would write ");
    ub_dec(&u, n);
    ub_s(&u, " blk at ");
    ub_s(&u, medium == SB_MED_SD ? "sd:" : "ufs:");
    ub_dec(&u, lba);
    ub_s(&u, " (");
    ub_s(&u, d.part[0] ? d.part : "?");
    ub_s(&u, ", tier ");
    ub_dec(&u, (uint64_t)d.tier);
    ub_s(&u, "): ");
    if (r) {
        ub_s(&u, d.why);
    } else if (armed_tier < d.tier) {
        ub_s(&u, "needs tier ");
        ub_dec(&u, (uint64_t)d.tier);
        r = SB_W_TIER;
    } else {
        ub_s(&u, "DRY RUN, nothing written");
        r = SB_W_DRYRUN;
    }
    sb_log(u.b);
    return r;
}
