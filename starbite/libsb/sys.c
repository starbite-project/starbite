/* SPDX-License-Identifier: GPL-2.0-only
 * libsb system services: start-up, timer, keys, reset, power off, boot-next, PSCI, probes, the
 * arena. The key, reset and power-off paths are shim/starlte/menu.c's (hw_keys, reboot,
 * hw_power_off), the boot-next mailbox is menu.c's boot_next() read side.
 */
#include "sb.h"

struct sb_boot sb_boot;

/* ---- time ---------------------------------------------------------------------------------- */

uint64_t sb_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t sb_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 26000000U;           /* S-Boot leaves CNTFRQ 0; the timer is 26 MHz */
}

uint64_t sb_us(void)
{
    return sb_ticks() / (sb_freq() / 1000000U);
}

uint64_t sb_ms(void)
{
    return sb_ticks() / (sb_freq() / 1000U);
}

void sb_udelay(uint64_t us)
{
    uint64_t t0 = sb_ticks(), n = sb_freq() / 1000000U * us;
    while (sb_ticks() - t0 < n)
        ;
}

void sb_mdelay(unsigned ms)
{
    sb_udelay((uint64_t)ms * 1000U);
}

/* ---- keys (starlte DTB gpio_keys, active low) ------------------------------------------------ */

#define ALIVE_GPA0_DAT 0x14050024UL     /* bit 3 Volume Up, 4 Volume Down, 6 Bixby */
#define ALIVE_GPA2_DAT 0x14050064UL     /* bit 4 Power */

unsigned sb_keys(void)
{
    uint32_t a = *(volatile uint32_t *)ALIVE_GPA0_DAT;
    uint32_t b = *(volatile uint32_t *)ALIVE_GPA2_DAT;
    return (!((a >> 3) & 1U) ? SB_K_UP : 0) | (!((a >> 4) & 1U) ? SB_K_DOWN : 0) |
           (!((a >> 6) & 1U) ? SB_K_BIXBY : 0) | (!((b >> 4) & 1U) ? SB_K_POWER : 0);
}

void sb_keys_release_wait(void)
{
    while (sb_keys())
        sb_mdelay(20);
}

/* ---- reset, power off, boot-next --------------------------------------------------------------- */

void sb_reboot(uint32_t inform3)
{
    volatile uint32_t *pmu = (volatile uint32_t *)SB_PMU;
    sb_log("reboot");
    sb_touch_clear();
    pmu[0x808 / 4] = 0x12345678U;       /* INFORM2 */
    pmu[0x80c / 4] = inform3;           /* INFORM3: normal / download / recovery */
    __asm__ volatile("dsb sy" : : : "memory");
    pmu[0x400 / 4] = 1;                 /* SWRESET */
    __asm__ volatile("dsb sy" : : : "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* PS_HOLD low (exynos-reboot.c exynos_power_off: PMU +0x330C bit 8). */
void sb_power_off(void)
{
    volatile uint32_t *ps = (volatile uint32_t *)(SB_PMU + 0x330c);
    sb_log("power off (PS_HOLD low)");
    sb_touch_clear();
    sb_keys_release_wait();
    sb_mdelay(300);
    __asm__ volatile("dmb oshst" : : : "memory");
    *ps = *ps & ~0x100U;
    sb_mdelay(3000);
}

#define NEXT2_MAGIC 0x21325458454e4253ULL   /* "SBNEXT2!" */

int sb_boot_next_peek(char key[24])
{
    volatile uint64_t *mb = (volatile uint64_t *)SB_NEXT_MBOX;
    key[0] = 0;
    if (mb[0] != NEXT2_MAGIC)
        return 0;
    for (unsigned k = 0; k < 23; ++k)
        key[k] = *(volatile char *)(SB_NEXT_MBOX + 8 + k);
    key[23] = 0;
    return key[0] != 0;
}

/* The mailbox is RAM above the pmsg ring (0xFED17FE0, non-cacheable here); the loader reads and
 * clears it on the next boot (shim/starlte/menu.c boot_next) and starts the entry once. */
void sb_boot_next(const char *key)
{
    volatile uint64_t *mb = (volatile uint64_t *)SB_NEXT_MBOX;
    volatile char *k = (volatile char *)(SB_NEXT_MBOX + 8);
    unsigned i = 0;
    for (; i < 23 && key[i]; ++i)
        k[i] = key[i];
    for (; i < 24; ++i)
        k[i] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    mb[0] = NEXT2_MAGIC;
    __asm__ volatile("dsb sy" : : : "memory");
    sb_logf("boot-next ", key);
    sb_reboot(SB_INFORM_NORMAL);
}

/* ---- PSCI ---------------------------------------------------------------------------------- */

int64_t sb_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "memory");
    return (int64_t)x0;
}

/* ---- probes -------------------------------------------------------------------------------- */

int sb_probe32(uint64_t addr, uint32_t *v)
{
    uint64_t c = sb_fault.count;
    uint32_t x;
    if (!sb_map_kind(addr)) {           /* unmapped: a translation fault, counted like one */
        *v = 0;
        return 0;
    }
    sb_probe_depth++;
    __asm__ volatile("mov %w0, wzr\n\tldr %w0, [%1]\n\tdsb sy" : "=&r"(x) : "r"(addr) : "memory");
    sb_probe_depth--;
    *v = x;
    return sb_fault.count == c;
}

/* ---- arena --------------------------------------------------------------------------------- */

extern char _image_end[];
static uintptr_t arena_next, arena_start;

void *sb_alloc(uint64_t size)
{
    if (!arena_next)
        arena_start = arena_next = ((uintptr_t)_image_end + 0xfffffU) & ~(uintptr_t)0xfffffU;
    uintptr_t p = (arena_next + 63U) & ~(uintptr_t)63U;
    if (p + size > SB_ARENA_END || p + size < p)
        return 0;
    arena_next = p + size;
    return (void *)p;
}

uint64_t sb_arena_used(void)
{
    return arena_next - arena_start;
}

uint64_t sb_arena_free(void)
{
    if (!arena_next)
        (void)sb_alloc(0);
    return SB_ARENA_END - arena_next;
}

/* ---- start --------------------------------------------------------------------------------- */

#define DECON_TRIG 0x16030070UL         /* DECON_F HW_SW_TRIG_CONTROL (shim/starlte/fbtext.c) */

void sb_start(void)
{
    volatile uint64_t *ll = (volatile uint64_t *)SB_LOADER_LOG;
    sb_boot.loader_tag = ll[0];
    sb_boot.loader_entry = ll[15];
    /* The loader leaves the trigger free-running for bare images; set it anyway so CPU writes
     * reach the panel whoever started us. */
    *(volatile uint32_t *)DECON_TRIG = 0x1281U;
    __asm__ volatile("dsb sy" : : : "memory");
    sb_log("libsb up");
    sb_logx("base ", sb_boot.base);
    sb_logx("fdt ", sb_boot.fdt);
    sb_logx("relocs ", (uint64_t)sb_boot.relocs);
    if (sb_boot.relocs < 0)
        sb_crash_show("RELOCATION FAILED (BAD ENTRY)", 0);
    if (sb_boot.base < SB_LOAD_MIN || sb_boot.base >= SB_ARENA_END)
        sb_log("warning: image outside the expected window");
    sb_main();
    sb_crash_show("SB_MAIN RETURNED", 0);
}
