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
 *   "SBOVRRLE": the same overlay run-length encoded (tools/brand/rle.py):
 *     u32 n, n x { u32 x, y, w, h, runs; runs x { u32 length, 0xAARRGGBB } };
 *   "SBSPLASH" u32 w, h + RGB565: a full-screen picture (splash.mjs).
 * The boot menu and the loader's status lines then go in the gap under
 * Samsung's logo.
 */
static uint64_t splash_start, splash_end;

static uint32_t
blend(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24, o = 0xff000000U;
    if (a == 255)
        d = 0;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        uint32_t c = (((s >> sh) & 255U) * a + ((d >> sh) & 255U) * (255U - a)) / 255U;
        o |= c << sh;
    }
    return o;
}

static int
star_splash_draw(void)
{
    uint64_t start = splash_start, end = splash_end;
    if (end <= start + 16)
        return 0;
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)start;
    volatile uint32_t *fb = (volatile uint32_t *)STAR_FB_BASE;
    if (h[0] == 0x564f4253U && h[1] == 0x454c5252U) {           /* "SBOVRRLE" */
        uint32_t n = h[2];
        const volatile uint32_t *q = h + 3;
        for (uint32_t r = 0; r < n && r < 16; r++) {
            if ((uintptr_t)(q + 5) > end)
                return 0;
            uint32_t x = q[0], y = q[1], w = q[2], ht = q[3], runs = q[4];
            q += 5;
            if ((uintptr_t)(q + 2 * (uint64_t)runs) > end || x + w > STAR_FB_WIDTH ||
                y + ht > STAR_FB_HEIGHT)
                return 0;
            uint64_t i = 0, total = (uint64_t)w * ht;
            for (uint32_t k = 0; k < runs; k++) {
                uint32_t length = q[0], s = q[1];
                q += 2;
                if (length > total - i)
                    return 0;
                if (s >> 24 == 0) {
                    i += length;
                    continue;
                }
                for (; length; --length, ++i) {
                    volatile uint32_t *p = fb + (uint64_t)(y + i / w) * STAR_FB_WIDTH + x + i % w;
                    *p = blend(s, *p);
                }
            }
        }
        star_fb_row_base(23);
    } else if (h[0] == 0x564f4253U && h[1] == 0x59414c52U) {    /* "SBOVRLAY" */
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
                    uint32_t s = *q++;
                    if (s >> 24)
                        row[xx] = blend(s, row[xx]);
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
 *
 * Bundles (tools/linux-bundle.py) follow each other after the payload: a
 * 4 KiB page with u64 magic + u64 size, the data, padding to 4 KiB.
 */
#define LINUX_BASE RAM_BASE
#define BUNDLE_LINUX 0x2158554e494c4253ULL   /* "SBLINUX!" */
#define BUNDLE_UEFI  0x2121494645554253ULL   /* "SBUEFI!!" */
static uintptr_t
find_bundle(const struct payload *p, uintptr_t rd_start, uintptr_t rd_end, uint64_t magic,
    uint64_t *size)
{
    uint64_t end = __builtin_offsetof(struct payload, segs) + p->nsegs * sizeof(struct payload_seg);
    if (p->adt_off + p->adt_size > end)
        end = p->adt_off + p->adt_size;
    for (uint32_t i = 0; i < p->nsegs; ++i)
        if (p->segs[i].file_off + p->segs[i].file_size > end)
            end = p->segs[i].file_off + p->segs[i].file_size;
    uintptr_t b = align_page(rd_start + end);
    while (b + PAGE_SIZE < rd_end) {
        uint64_t m = *(volatile uint64_t *)b, s = *(volatile uint64_t *)(b + 8);
        if ((m != BUNDLE_LINUX && m != BUNDLE_UEFI) || s > rd_end - b - PAGE_SIZE)
            return 0;
        if (m == magic) {
            *size = s;
            return b + PAGE_SIZE;
        }
        b = align_page(b + PAGE_SIZE + s);
    }
    return 0;
}

static uintptr_t
star_linux(volatile uint64_t *log, uintptr_t fdt, const struct payload *p, uint64_t length,
    uintptr_t rd_start, uintptr_t rd_end)
{
    uint64_t size;
    uintptr_t img = find_bundle(p, rd_start, rd_end, BUNDLE_LINUX, &size);
    if (!img)
        return fail(log, -20, "NO LINUX KIOSK KERNEL");
    if (size < 64 || *(volatile uint32_t *)(img + 56) != 0x644d5241U)
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

/*
 * Windows 11: the exynos9810-woa UEFI (BOOT kernel of its release image:
 * edk2-msm style bootshim + EDK2 FD) in an "SBUEFI!!" bundle. The bootshim
 * is position independent and copies the FD to 0x90000000 itself, so it is
 * entered where it lies, as S-Boot would enter it (x0 = FDT, MMU off).
 *
 * The woa BDS keeps a boot-attempt counter ("BAT1" magic + count at
 * 0xFED17F10, in the exynos_ss log page, survives warm resets) and sends the
 * third unacknowledged Windows boot straight to TWRP; the same page above the
 * pmsg ring (0xFED17F0C..0xFED18000) holds its other cross-reset latches (UFS
 * crash, APM probe, RAM status, KiSystemStartup). Every boot through this
 * menu is a deliberate one, so the page is cleared like a cold boot.
 */
#define UEFI_LATCH_PAGE 0xFED17F0CUL   /* ring data ends at 0xFED1400C + 0x3F00 */
#define UEFI_LATCH_END  0xFED18000UL

/*
 * Append to the ramoops pmsg ring (exynos_ss log_pstore 0xFED10000: dmesg
 * 16 KiB, then pmsg 16 KiB at 0xFED14000; header u32 "DBGC", start, size),
 * the ring the woa UEFI also writes, so TWRP shows both in
 * /sys/fs/pstore/pmsg-ramoops-0 on the next boot.
 */
#define PMSG_BASE 0xFED14000UL
#define PMSG_DATA 0x3F00U   /* the woa UEFI's flood cap: keeps clear of its latches */
static void
pmsg_put(const char *s)
{
    volatile uint32_t *h = (volatile uint32_t *)PMSG_BASE;
    volatile uint8_t *d = (volatile uint8_t *)(PMSG_BASE + 12);
    if (h[0] != 0x43474244U || h[1] >= PMSG_DATA || h[2] > PMSG_DATA) {
        h[0] = 0x43474244U;                                     /* "DBGC" */
        h[1] = 0;
        h[2] = 0;
    }
    for (; *s; ++s) {
        uint32_t start = h[1];
        d[start] = (uint8_t)*s;
        h[1] = (start + 1) % PMSG_DATA;
        if (h[2] < PMSG_DATA)
            h[2] = h[2] + 1;
    }
    clean_bytes(PMSG_BASE, PMSG_BASE + 16384);
}

static void
pmsg_hex(uint64_t v)
{
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; ++i)
        b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    pmsg_put(b);
}

static uintptr_t
star_uefi(volatile uint64_t *log, uintptr_t fdt, const struct payload *p,
    uintptr_t rd_start, uintptr_t rd_end)
{
    uint64_t size;
    uintptr_t img = find_bundle(p, rd_start, rd_end, BUNDLE_UEFI, &size);
    if (!img)
        return fail(log, -30, "NO WINDOWS UEFI");
    if (size < 64 || *(volatile uint32_t *)(img + 56) != 0x644d5241U)
        return fail(log, -31, "BAD WINDOWS UEFI");
    /* The FD lands at 0x90000000..0x90700000: the bundle must not be there. */
    if (img + size > 0x90000000UL)
        return fail(log, -32, "UEFI BUNDLE TOO HIGH");
    star_fb_hex(1, "UEFI AT", img);
    /* Empty ring: everything after the marker is this UEFI run's trace. */
    ((volatile uint32_t *)PMSG_BASE)[0] = 0;
    pmsg_put("STARBITE: entering UEFI at ");
    pmsg_hex(img);
    pmsg_put(" size ");
    pmsg_hex(size);
    pmsg_put(" fdt ");
    pmsg_hex(fdt);
    pmsg_put("\n");
    zero_bytes(UEFI_LATCH_PAGE, UEFI_LATCH_END - UEFI_LATCH_PAGE);
    clean_bytes(UEFI_LATCH_PAGE, UEFI_LATCH_END);
    star_fb_restore((uint32_t)log[77]);
    log[10] = 0x200;
    log[15] = img;
    log[17] = fdt;
    clean_bytes(LOG_BASE, LOG_BASE + PAGE_SIZE);
    __asm__ volatile("ic iallu; dsb sy; isb" : : : "memory");
    return img;
}

/* Returns entry PA or zero. Status stays in the log page at LOG_BASE. */
/*
 * Cross-reset words a Windows run leaves in the latch page, logged before
 * star_uefi() clears it: boot-attempt counter (7F10/14), UFS driver crash
 * latch + 'WATL' guard (7F40/44), RAM_STATUS (7F60), KiSystemStartup
 * progress (7F80) and the Windows handoff marker (7FB0).
 */
static void
star_dump_latches(void)
{
    static const uint32_t off[] = { 0x10, 0x14, 0x40, 0x44, 0x60, 0x80, 0xB0 };
    pmsg_put("STARBITE: latches");
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) {
        pmsg_put(" 7f");
        pmsg_hex(*(volatile uint32_t *)(0xFED17F00UL + off[i]));
    }
    pmsg_put("\n");
}

uintptr_t star_prepare_xnu(uintptr_t fdt, volatile uint64_t *log, uint64_t el, uint64_t sctlr)
{
    uint64_t rd_start, rd_end;
    static char cmdline[BOOT_LINE_LENGTH];
    star_dump_latches();
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
        if (pick == 3)
            return star_uefi(log, fdt, p, rd_start, rd_end);
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
