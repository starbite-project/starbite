/* SPDX-License-Identifier: GPL-2.0-only
 * menuchain (chain.S): copy the loader to its S-Boot address and, if packed, the ramdisk to where
 * S-Boot put BOOT's (the FDT's linux,initrd-start), whose end it updates. MMU off: every access is
 * Device memory, so all copies are 8-byte aligned (pack.py pads to 4 KiB).
 */
#include <stdint.h>
#include "../../../shim/fdt.h"

struct chain_hdr { uint64_t magic, loader_off, loader_len, rd_off, rd_len, entry, wdt_count; };
extern struct chain_hdr chain_hdr;
uintptr_t chain_main(uintptr_t fdt, uintptr_t base);

static void copy8(uintptr_t dst, uintptr_t src, uint64_t n)
{
    volatile uint64_t *d = (volatile uint64_t *)dst;
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    for (uint64_t i = 0; i < n / 8; ++i)
        d[i] = s[i];
}

/* The dead-man (pack.py --wdt S): cluster 1's watchdog armed with reset, as the loader's wdt_arm
 * (wdt.c) does, before anything else. The loader's own launch path arms it only for entries with
 * a launch record, and a test loader never feeds it: if the test loader hangs anywhere, the SoC
 * resets back to the BOOT loader after S seconds. A test loader that finishes reboots before. */
static void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static void deadman(uint32_t count)
{
    wr32(0x10060000UL, 0);                       /* WTCON: stopped */
    wr32(0x10060004UL, count);                   /* WTDAT */
    wr32(0x10060008UL, count);                   /* WTCNT */
    wr32(0x1006000CUL, 0);                       /* WTCLRINT */
    wr32(0x10060000UL, 0xFF39U);                 /* prescaler 255, DIV128, ENABLE, RSTEN */
    wr32(0x14060408UL, rd32(0x14060408UL) & ~(1U << 23));   /* PMU: unblock cluster 1's reset */
    wr32(0x1406040CUL, rd32(0x1406040CUL) & ~(1U << 23));
}

uintptr_t chain_main(uintptr_t fdt, uintptr_t base)
{
    const struct chain_hdr *h = &chain_hdr;
    if (h->magic == 0x314e49414843374dULL && h->wdt_count && h->wdt_count <= 0xffffU)
        deadman((uint32_t)h->wdt_count);
    if (h->magic != 0x314e49414843374dULL || !h->loader_len || (h->loader_len & 7) || h->entry != 0x80080000UL ||
        h->loader_len > 0x83800000UL - 0x80080000UL)
        return 0;
    /* the loader Image header: "ARMd" at +56 */
    if (*(volatile uint32_t *)(base + h->loader_off + 56) != 0x644d5241U)
        return 0;
    if (h->rd_len) {
        uint64_t s = 0, e = 0;
        if (!fdt_chosen_u64(fdt, "linux,initrd-start", &s) || !fdt_chosen_u64(fdt, "linux,initrd-end", &e))
            return 0;
        /* the new ramdisk must fit below S-Boot's FDT, above the loader */
        if ((s & 7) || s < 0x83800000UL || (fdt > s && s + h->rd_len > fdt) || s + h->rd_len > 0x8c000000UL)
            return 0;
        copy8(s, base + h->rd_off, (h->rd_len + 7) & ~7ULL);
        if (!fdt_chosen_set_u64(fdt, "linux,initrd-end", s + h->rd_len))
            return 0;
    }
    copy8(h->entry, base + h->loader_off, h->loader_len);
    return h->entry;
}
