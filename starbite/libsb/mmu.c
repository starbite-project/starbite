/* SPDX-License-Identifier: GPL-2.0-only
 * libsb's identity map and caches (after shim/starlte/mmu.c). The loader enters every OS with the
 * MMU and caches off and the data cache cleaned. Map (36-bit, 4 KiB granule, level 1 + level 2):
 *
 *   0x000000000..0x07fffffff  Device-nGnRnE, XN            SoC registers (and APM SRAM 0x02039000)
 *   0x080000000..0x09fffffff  Normal write-back            S-Boot's FDT 0x8a000000, the BOOT ramdisk
 *                                                          0x89000000, the loader's log page, this
 *                                                          image (0x92080000), its arena
 *   0x0a0000000..0x0a7ffffff  Normal non-cacheable, XN     RAM test window (tests DRAM, not caches)
 *   0x0a8000000..0x0aeffffff  Normal write-back            free
 *   0x0af000000..0x0af7fffff  Normal non-cacheable, XN     UFS DMA (ufs.c) and read scratch
 *   0x0af800000..0x0bfffffff  unmapped                     RKP, TIMA, secure world, seclog start
 *   0x0c0000000..0x0ffffffff  Normal non-cacheable, XN     framebuffer 0xcc000000 (write-combined),
 *                                                          pstore / pmsg / boot-next 0xFED10000
 *   0x880000000..0x8ffffffff  Normal non-cacheable, XN     second DRAM bank (peek only)
 *   everything else           unmapped (a fault: probes skip it, anything else is a crash)
 *
 * Only memory the CPU alone writes is cacheable. DMA (UFS) lands in the non-cacheable window;
 * the second core's mailbox (sb_core_box) is invalidated by hand. Written in plain C with no
 * pointer stored in data: crt0.S calls it before the image is relocated.
 */
#include "sb.h"

#define BLOCK      0x1ULL
#define TABLE      0x3ULL
#define ATTR(i)    ((uint64_t)(i) << 2)
#define SH_INNER   (3ULL << 8)
#define AF         (1ULL << 10)
#define XN         ((1ULL << 53) | (1ULL << 54))
#define DEV        (BLOCK | ATTR(0) | AF | XN)
#define WB         (BLOCK | ATTR(1) | SH_INNER | AF)
#define NC         (BLOCK | ATTR(2) | SH_INNER | AF | XN)
#define MAIR       ((0x44ULL << 16) | (0xffULL << 8) | 0x00ULL)
#define TCR        (28ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (1ULL << 32))

static uint64_t l1[64] __attribute__((aligned(4096)));
static uint64_t l2_low[512] __attribute__((aligned(4096)));   /* 0x80000000..0xbfffffff */
static int mmu_on;

/* hidden: called before relocation, so it must be a direct `bl`, not a call through the
 * GOT (an unrelocated GOT entry jumped to garbage: the first phone boot reset silently). */
extern void sb_dcache_all(unsigned op) __attribute__((visibility("hidden")));

static uint64_t l2_desc(uint64_t pa)
{
    if (pa >= 0xa0000000ULL && pa < 0xa8000000ULL)
        return NC | pa;
    if (pa >= 0xaf000000ULL && pa < 0xaf800000ULL)
        return NC | pa;
    if (pa >= 0xaf800000ULL)
        return 0;
    return WB | pa;
}

int sb_mmu_on(void)
{
    return mmu_on;
}

void sb_mmu_enable(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (sctlr & 1)
        return;
    for (unsigned i = 0; i < 64; ++i)
        l1[i] = 0;
    l1[0] = DEV | 0x00000000ULL;
    l1[1] = DEV | 0x40000000ULL;
    for (unsigned i = 0; i < 512; ++i)
        l2_low[i] = l2_desc(0x80000000ULL + ((uint64_t)i << 21));
    l1[2] = TABLE | (uint64_t)(uintptr_t)l2_low;
    l1[3] = NC | 0xc0000000ULL;
    l1[34] = NC | 0x880000000ULL;
    l1[35] = NC | 0x8c0000000ULL;
    sb_dcache_all(0);
    __asm__ volatile(
        "dsb sy\n"
        "ic iallu\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :: "r"(MAIR), "r"(TCR), "r"((uint64_t)(uintptr_t)l1) : "memory");
    sctlr |= 1ULL | (1ULL << 2) | (1ULL << 12);   /* M, C, I */
    sctlr &= ~(1ULL << 1);                          /* A: no alignment checks on normal memory */
    __asm__ volatile("msr sctlr_el1, %0\nisb" :: "r"(sctlr) : "memory");
    mmu_on = 1;
}

int sb_map_kind(uint64_t pa)
{
    if (pa < 0x80000000ULL)
        return 1;
    if (pa < 0xc0000000ULL) {
        uint64_t d = l2_desc(pa & ~0x1fffffULL);
        return !d ? 0 : (d & ATTR(7)) == ATTR(1) ? 2 : 3;
    }
    if (pa < 0x100000000ULL || (pa >= 0x880000000ULL && pa < 0x900000000ULL))
        return 3;
    return 0;
}

/* Tier 4 attempts (engineer/armed.c): one block mapped Device-nGnRnE for one access, then the map
 * as it was. The other cores are idle then (no test runs while a utility writes); the TLB is
 * invalidated inner-shareable anyway. */
static uint64_t *temp_slot, temp_old;

int sb_mmu_map_temp(uint64_t pa)
{
    if (!mmu_on || temp_slot || pa >= (64ULL << 30))
        return -1;
    if (sb_map_kind(pa))
        return 0;
    uint64_t *slot, d;
    if (pa >= 0x80000000ULL && pa < 0xc0000000ULL) {
        slot = &l2_low[(pa >> 21) & 0x1ffU];
        d = DEV | (pa & ~0x1fffffULL);
    } else {
        slot = &l1[pa >> 30];
        d = DEV | (pa & ~0x3fffffffULL);
    }
    temp_old = *slot;
    temp_slot = slot;
    *slot = d;
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
    return 1;
}

void sb_mmu_unmap_temp(void)
{
    if (!temp_slot)
        return;
    *temp_slot = temp_old;
    temp_slot = 0;
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
}

void sb_dcache_clean_inval(const volatile void *addr, uint64_t len)
{
    if (!mmu_on)
        return;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)63, end = (uintptr_t)addr + len;
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Lines another observer wrote (the second core's box): clean + invalidate is safe here because
 * the CPU never dirties them while the other side writes. */
void sb_dcache_inval(const volatile void *addr, uint64_t len)
{
    sb_dcache_clean_inval(addr, len);
}
