/* SPDX-License-Identifier: GPL-2.0-only
 * Self-relocation (docs/STARBITE-MASTER-PLAN.md section 4.2: "The OS images fix their own
 * addresses at entry"). The image is linked at address 0 as a static PIE (engineer.ld,
 * ld -pie --no-dynamic-linker -z text), so the only dynamic relocations are R_AARCH64_RELATIVE
 * in .rela.dyn: pointers in .data / .rodata / .got whose link-time value is the addend. At run
 * time each one becomes base + addend. build.sh refuses an image with any other relocation type.
 *
 * Called by crt0.S before anything reads a pointer from memory. It uses only its arguments and
 * locals (no globals, no calls), so it is correct before it has run. Also compiled into the host
 * test (starbite/tools/reloc-test.c), which relocates the real image at an arbitrary address and
 * compares it with the same objects linked there.
 */
#include <stdint.h>

#define R_AARCH64_RELATIVE 1027U

struct sb_rela {
    uint64_t off, info;
    int64_t addend;
};

/* Number of entries applied, or -(index + 1) of the first entry that is not RELATIVE or points
 * outside [0, limit) (limit 0: no check). */
int64_t sb_reloc_check(uint64_t base, const struct sb_rela *r, const struct sb_rela *end, uint64_t limit)
{
    int64_t n = 0;
    for (; r < end; ++r, ++n) {
        if ((uint32_t)r->info != R_AARCH64_RELATIVE || (r->info >> 32) != 0)
            return -(n + 1);
        if (limit && (r->off >= limit || (r->off & 7U)))
            return -(n + 1);
        *(volatile uint64_t *)(base + r->off) = base + (uint64_t)r->addend;
    }
    return n;
}

int64_t sb_reloc(uint64_t base, const struct sb_rela *r, const struct sb_rela *end)
{
    return sb_reloc_check(base, r, end, 0);
}
