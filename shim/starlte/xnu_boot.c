/* SPDX-License-Identifier: GPL-2.0-only
 * starlte S-Boot loader for mkpayload.py version 1. Based on ios-a5xelte
 * shim/herolte/xnu_boot.c (wardevsa, herolte branch). Single CPU, 4 KiB XNU,
 * isolated 472 MiB RAM window 0x92000000..0xaf800000: free in the starlte
 * DTB and cmdline (vendor logs 0x91202000..0x9179c000, RKP hypervisor region
 * from 0xaf800000, TIMA 0xb8000000, seclog 0xc0000000, framebuffer
 * 0xcc000000).
 *
 * The probe showed S-Boot placing the initrd at 0x89000000 and the FDT at
 * 0x8a000000, as on herolte: the packer appends the payload to this Image
 * (kernel slot); the initrd is only a fallback.
 */
#include <stdint.h>
#include "../fdt.h"
#include "fbtext.h"
#include "menu.h"

#define LOG_BASE 0x91fff000UL
#define RAM_BASE 0x92000000UL
#define RAM_END  0xaf800000UL
/* The second DRAM bank is not described until its layout is measured. */
#define BANK2_BASE RAM_END
#define BANK2_END  RAM_END
#define DRAM_LOW 0x80000000UL
#define PAGE_SIZE 4096UL
#define BOOT_LINE_LENGTH 608
#define FAIL_DELAY_S 30U

struct payload_seg {
    uint64_t vmaddr, vmsize, file_off, file_size;
};
struct payload {
    char magic[8];
    uint32_t version, nsegs;
    uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
    uint64_t phys_base, mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    struct payload_seg segs[];
};
struct boot_video {
    uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

static void zero_bytes(uintptr_t dst, uint64_t size)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    for (uint64_t i = 0; i < size; ++i)
        d[i] = 0;
}

static void copy_bytes(uintptr_t dst, uintptr_t src, uint64_t size)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    volatile const uint8_t *s = (volatile const uint8_t *)src;
    for (uint64_t i = 0; i < size; ++i)
        d[i] = s[i];
}

static void clean_bytes(uintptr_t start, uintptr_t end)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, CTR_EL0" : "=r" (ctr));
    uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
    for (uintptr_t p = start & ~(line - 1); p < end; p += line)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

static int magic_ok(const char *value)
{
    const char *want = "SWEETXNU";
    for (unsigned i = 0; i < 8; ++i)
        if (value[i] != want[i])
            return 0;
    return 1;
}

static uintptr_t align_page(uintptr_t value)
{
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (value));
    return value;
}

/* Show why the loader stopped, give time for a photo, then request Download mode. */
static uintptr_t fail(volatile uint64_t *log, int64_t step, const char *why)
{
    uint64_t freq;
    log[10] = (uint64_t)step;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    star_fb_text(3, "LOADER FAIL:");
    star_fb_text(4, why);
    star_fb_text(5, "DOWNLOAD MODE IN 30 S");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));
    if (!freq)
        freq = 26000000U;
    uint64_t t0 = counter();
    while ((counter() - t0) / freq < FAIL_DELAY_S)
        ;
    return 0;
}

/*
 * Project Starbite boot branding (developed by the Starbite project), from
 * the BOOT ramdisk (tools/brand, tools/make-xnu-boot.sh SPLASH=):
 *   "SBOVRLAY" u32 n, n x { u32 x, y, w, h; w*h u32 0xAARRGGBB }: blended over
 *     S-Boot's own boot logo, which stays on screen (overlay.mjs);
 *   "SBSPLASH" u32 w, h + RGB565: a full-screen picture (splash.mjs).
 * The boot menu and the loader's status lines then go in the gap under
 * Samsung's logo.
 */
static uint64_t splash_start, splash_end;

static int
star_splash_draw(void)
{
    uint64_t start = splash_start, end = splash_end;
    if (end <= start + 16)
        return 0;
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)start;
    volatile uint32_t *fb = (volatile uint32_t *)STAR_FB_BASE;
    if (h[0] == 0x564f4253U && h[1] == 0x59414c52U) {           /* "SBOVRLAY" */
        uint32_t n = h[2];
        const volatile uint32_t *q = h + 3;
        for (uint32_t r = 0; r < n && r < 16; r++) {
            uint32_t x = q[0], y = q[1], w = q[2], ht = q[3];
            q += 4;
            if ((uintptr_t)(q + (uint64_t)w * ht) > end || x + w > STAR_FB_WIDTH ||
                y + ht > STAR_FB_HEIGHT)
                return 0;
            for (uint32_t yy = 0; yy < ht; yy++) {
                volatile uint32_t *row = fb + (uint64_t)(y + yy) * STAR_FB_WIDTH + x;
                for (uint32_t xx = 0; xx < w; xx++) {
                    uint32_t s = *q++, a = s >> 24;
                    if (a == 0)
                        continue;
                    uint32_t d = a == 255 ? 0 : row[xx], o = 0xff000000U;
                    for (unsigned sh = 0; sh < 24; sh += 8) {
                        uint32_t c = (((s >> sh) & 255U) * a + ((d >> sh) & 255U) * (255U - a)) / 255U;
                        o |= c << sh;
                    }
                    row[xx] = o;
                }
            }
        }
        star_fb_row_base(23);
    } else if (h[0] == 0x50534253U && h[1] == 0x4853414cU) {   /* "SBSPLASH" */
        uint32_t w = h[2], ht = h[3];
        if (w != STAR_FB_WIDTH || ht != STAR_FB_HEIGHT || end - start < 16 + (uint64_t)w * ht * 2)
            return 0;
        const volatile uint32_t *px = (const volatile uint32_t *)(uintptr_t)(start + 16);
        for (uint32_t i = 0; i < w * ht / 2; i++) {
            uint32_t two = px[i];
            for (unsigned k = 0; k < 2; k++) {
                uint32_t p = (two >> (16 * k)) & 0xffffU;
                uint32_t r = (p >> 11) & 31U, g = (p >> 5) & 63U, b = p & 31U;
                fb[2 * i + k] = 0xff000000U | (r << 19 | (r >> 2) << 16) |
                    (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
        star_fb_row_base(38);
    } else {
        return 0;
    }
    return 1;
}

static int
star_splash(uintptr_t fdt)
{
    if (!fdt || (fdt & 7) || !fdt_chosen_u64(fdt, "linux,initrd-start", &splash_start) ||
        !fdt_chosen_u64(fdt, "linux,initrd-end", &splash_end))
        return 0;
    return star_splash_draw();
}

void
star_overlay_redraw(void)
{
    (void)star_splash_draw();
}

/*
 * Linux kiosk: a Linux arm64 Image (tools/kiosk, Exynos9810-LTS 4.9
 * kernel with its initramfs built in) appended after the SWEETXNU payload as
 * a 4 KiB-aligned "SBLINUX!" bundle: u64 magic, u64 Image size, Image at
 * +4096. It is copied to the XNU window's 2 MiB-aligned base + text_offset
 * (Documentation/arm64/booting.txt) and entered with x0 = S-Boot's FDT.
 * S-Boot's initrd is our overlay, so /chosen's initrd is emptied first.
 */
#define LINUX_BASE RAM_BASE
static uintptr_t
star_linux(volatile uint64_t *log, uintptr_t fdt, const struct payload *p, uint64_t length,
    uintptr_t rd_start, uintptr_t rd_end)
{
    uint64_t end = __builtin_offsetof(struct payload, segs) + p->nsegs * sizeof(struct payload_seg);
    if (p->adt_off + p->adt_size > end)
        end = p->adt_off + p->adt_size;
    for (uint32_t i = 0; i < p->nsegs; ++i)
        if (p->segs[i].file_off + p->segs[i].file_size > end)
            end = p->segs[i].file_off + p->segs[i].file_size;
    uintptr_t b = align_page(rd_start + end);
    while (b + PAGE_SIZE < rd_end && *(volatile uint64_t *)b != 0x2158554e494c4253ULL)  /* "SBLINUX!" */
        b += PAGE_SIZE;
    if (b + PAGE_SIZE >= rd_end)
        return fail(log, -20, "NO LINUX KIOSK KERNEL");
    uint64_t size = *(volatile uint64_t *)(b + 8);
    uintptr_t img = b + PAGE_SIZE;
    if (size < 64 || size > rd_end - img || *(volatile uint32_t *)(img + 56) != 0x644d5241U)
        return fail(log, -21, "BAD KIOSK KERNEL");
    uint64_t text_offset = *(volatile uint64_t *)(img + 8);
    uint64_t image_size = *(volatile uint64_t *)(img + 16);
    if (!image_size)
        image_size = size;
    uintptr_t dst = LINUX_BASE + text_offset;
    if (text_offset >= (2UL << 20) || dst + image_size > RAM_END)
        return fail(log, -22, "KIOSK KERNEL TOO LARGE");
    star_fb_hex(1, "LINUX AT", dst);
    copy_bytes(dst, img, size);
    zero_bytes(dst + size, image_size > size ? image_size - size : 0);
    clean_bytes(dst, dst + image_size);
    uint64_t start;
    if (fdt_chosen_u64(fdt, "linux,initrd-start", &start))
        (void)fdt_chosen_set_u64(fdt, "linux,initrd-end", start);
    clean_bytes(fdt, fdt + (2UL << 20));
    /* Last screen write: hand the display back as S-Boot left it. */
    star_fb_restore((uint32_t)log[77]);
    log[10] = 0x100;
    log[15] = dst;
    log[17] = fdt;              /* xnu_boot.S passes log[17] in x0 */
    (void)length;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return dst;
}

/* Returns entry PA or zero. Status stays in the log page at LOG_BASE. */
uintptr_t star_prepare_xnu(uintptr_t fdt, volatile uint64_t *log, uint64_t el, uint64_t sctlr)
{
    uint64_t rd_start, rd_end;
    static char cmdline[BOOT_LINE_LENGTH];
    log[77] = star_fb_enable();
    int branded = star_splash(fdt);
    if (el != 4)
        return fail(log, -10, "NOT EL1");
    if (sctlr & 1)
        return fail(log, -11, "MMU ALREADY ON");
    /* Appended payload first: its length is bounded by the Image header. */
    extern char _start[], _image_end[];
    uint64_t image_size = *(volatile uint64_t *)(uintptr_t)(_start + 16);
    rd_start = (uintptr_t)_image_end;
    rd_end = (uintptr_t)_start + image_size;
    if (rd_end <= rd_start || !magic_ok((const char *)rd_start)) {
        if (!fdt || (fdt & 7) ||
            !fdt_chosen_u64(fdt, "linux,initrd-start", &rd_start) ||
            !fdt_chosen_u64(fdt, "linux,initrd-end", &rd_end) ||
            rd_end <= rd_start)
            return fail(log, -1, "NO PAYLOAD");
    }
    if (rd_start < DRAM_LOW || rd_end > RAM_BASE)
        return fail(log, -1, "PAYLOAD OVERLAPS XNU RAM");
    log[11] = rd_start;
    log[12] = rd_end;

    const struct payload *p = (const struct payload *)(uintptr_t)rd_start;
    uint64_t length = rd_end - rd_start;
    if (length < sizeof(*p) || !magic_ok(p->magic) || p->version != 1 ||
        !p->nsegs || p->nsegs > 32 ||
        __builtin_offsetof(struct payload, segs) + p->nsegs * sizeof(struct payload_seg) > length ||
        p->adt_off > length || p->adt_size > length - p->adt_off)
        return fail(log, -2, "BAD SWEETXNU PAYLOAD");
    /* The payload itself sits below RAM_BASE, outside both windows. */
    uint64_t window_end = p->phys_base >= BANK2_BASE ? BANK2_END : RAM_END;
    int in_low = p->phys_base >= RAM_BASE && p->phys_base < RAM_END;
    int in_bank2 = p->phys_base >= BANK2_BASE && p->phys_base < BANK2_END;
    if (!(in_low || in_bank2) ||
        !p->mem_size || p->mem_size > window_end - p->phys_base ||
        p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
        return fail(log, -3, "PAYLOAD RAM OUTSIDE WINDOW");
    for (unsigned i = 0; i < BOOT_LINE_LENGTH; ++i)
        cmdline[i] = p->cmdline[i];
    cmdline[BOOT_LINE_LENGTH - 1] = 0;
    /* The boot menu (and Starbite Engineer) on the branded screen; XNU RAM
     * is still free here, for Engineer's RAM test. */
    if (branded) {
        struct star_info in;
        in.el = el;
        in.sctlr = sctlr;
        in.fdt = fdt;
        in.rd_start = rd_start;
        in.rd_end = rd_end;
        in.phys_base = p->phys_base;
        in.mem_size = p->mem_size;
        in.entry_va = p->entry_va;
        in.nsegs = p->nsegs;
        int pick = star_menu(&in, cmdline, BOOT_LINE_LENGTH);
        if (!pick)
            return 0;
        if (pick == 2)
            return star_linux(log, fdt, p, length, rd_start, rd_end);
    }
    star_fb_text(0, "PROJECT STARBITE");
    star_fb_text(1, "");
    star_fb_hex(2, "RAM BASE", p->phys_base);
    log[13] = p->phys_base;
    log[14] = p->mem_size;
    uintptr_t memory_end = p->phys_base + p->mem_size;
    uintptr_t kernel_end = p->phys_base + (p->kernel_end - p->virt_base);
    if (kernel_end >= memory_end)
        return fail(log, -3, "KERNEL TOO LARGE");

    star_fb_text(1, "COPYING KERNEL");
    for (uint32_t i = 0; i < p->nsegs; ++i) {
        const struct payload_seg *s = &p->segs[i];
        if (s->vmaddr < p->virt_base || s->file_size > s->vmsize ||
            s->file_off > length || s->file_size > length - s->file_off ||
            s->vmsize > p->mem_size || s->vmaddr - p->virt_base > p->mem_size - s->vmsize)
            return fail(log, -4, "BAD SEGMENT");
        uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
        copy_bytes(dst, rd_start + s->file_off, s->file_size);
        zero_bytes(dst + s->file_size, s->vmsize - s->file_size);
        clean_bytes(dst, dst + s->vmsize);
    }
    log[10] = 4;
    uintptr_t adt_pa = kernel_end;
    uintptr_t ba_pa = align_page(adt_pa + p->adt_size);
    if (ba_pa + PAGE_SIZE > memory_end || ba_pa < adt_pa)
        return fail(log, -5, "NO ROOM FOR BOOT ARGS");
    copy_bytes(adt_pa, rd_start + p->adt_off, p->adt_size);
    zero_bytes(ba_pa, PAGE_SIZE);
    volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = ba_pa + PAGE_SIZE;
    ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    for (unsigned i = 0; i < BOOT_LINE_LENGTH; ++i)
        ba->CommandLine[i] = cmdline[i];
    ba->memSizeActual = p->mem_size;
    clean_bytes(adt_pa, ba_pa + PAGE_SIZE);

    uintptr_t entry_pa = p->phys_base + (p->entry_va - p->virt_base);
    if (entry_pa >= kernel_end)
        return fail(log, -6, "ENTRY OUTSIDE KERNEL");
    log[15] = entry_pa;
    log[16] = p->nsegs;
    log[17] = ba_pa;
    log[10] = 5;
    star_fb_hex(1, "ENTRY", entry_pa);
    star_fb_hex(2, "BOOTARGS", ba_pa);
    star_fb_text(3, "JUMP TO XNU");
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return entry_pa;
}
